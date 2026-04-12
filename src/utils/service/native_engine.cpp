/**
 * @file native_engine.cpp
 * @brief NativeEngine — ScriptEngine implementation for native C/C++ shared libraries.
 *
 * Loads shared libraries via dlopen (POSIX) or LoadLibrary (Windows).
 * Resolves callback symbols at load time. Dispatches invoke calls as
 * direct function pointer calls with zero marshaling.
 *
 * See native_engine_api.h for the native engine-side API.
 */
#include "utils/native_engine.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include "utils/crypto_utils.hpp"
#include "utils/format_tools.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"
#include "utils/native_engine_api.h"
#include "utils/schema_field_layout.hpp"
#include "utils/role_host_core.hpp"

#include <fstream>
#include <sstream>

// ── Platform dynamic loading ────────────────────────────────────────────
// Supports: Linux, macOS, FreeBSD (POSIX dlopen) and Windows (LoadLibrary).
// RTLD_NOW: resolve all symbols at load time (fail-fast on missing symbols).
// RTLD_LOCAL: don't pollute global symbol table (native engine is self-contained).
#if defined(_WIN32) || defined(_WIN64)
#   include <windows.h>
#   define DL_OPEN(path)      LoadLibraryA(path)
#   define DL_SYM(handle, s)  reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), s))
#   define DL_CLOSE(handle)   FreeLibrary(static_cast<HMODULE>(handle))
    static std::string dl_error_win32()
    {
        DWORD err = GetLastError();
        if (err == 0) return "unknown error";
        char *buf = nullptr;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                       nullptr, err, 0, reinterpret_cast<char *>(&buf), 0, nullptr);
        std::string msg = buf ? buf : "unknown error";
        LocalFree(buf);
        return msg;
    }
#   define DL_ERROR()         dl_error_win32().c_str()
#else
#   include <dlfcn.h>
#   define DL_OPEN(path)      dlopen(path, RTLD_NOW | RTLD_LOCAL)
#   define DL_SYM(handle, s)  dlsym(handle, s)
#   define DL_CLOSE(handle)   dlclose(handle)
#   define DL_ERROR()         dlerror()
#endif

namespace pylabhub::scripting
{

// ============================================================================
// Context function pointer implementations (static, anonymous namespace)
// ============================================================================

namespace
{

void ctx_log(const PlhNativeContext *ctx, int level, const char *msg)
{
    if (!msg) return;
    const char *label = (ctx && ctx->_log_label) ? ctx->_log_label : "[native]";
    switch (level)
    {
        case PLH_LOG_DEBUG: LOGGER_DEBUG("{} {}", label, msg); break;
        case PLH_LOG_WARN:  LOGGER_WARN("{} {}", label, msg); break;
        case PLH_LOG_ERROR: LOGGER_ERROR("{} {}", label, msg); break;
        case PLH_LOG_INFO:  // fallthrough
        default:            LOGGER_INFO("{} {}", label, msg); break;
    }
}

void ctx_report_metric(const PlhNativeContext *ctx, const char *key, double value)
{
    if (!ctx || !ctx->_core || !key) return;
    static_cast<RoleHostCore *>(ctx->_core)->report_metric(key, value);
}

void ctx_clear_custom_metrics(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->clear_custom_metrics();
}

void ctx_request_stop(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->request_stop();
}

void ctx_set_critical_error(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->set_critical_error();
}

int ctx_is_critical_error(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->is_critical_error() ? 1 : 0;
}

const char *ctx_stop_reason(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return "normal";
    static thread_local std::string buf;
    buf = static_cast<RoleHostCore *>(ctx->_core)->stop_reason_string();
    return buf.c_str();
}

uint64_t ctx_out_slots_written(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->out_slots_written();
}

uint64_t ctx_in_slots_received(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->in_slots_received();
}

uint64_t ctx_out_drop_count(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->out_drop_count();
}

uint64_t ctx_script_error_count(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->script_error_count();
}

uint64_t ctx_loop_overrun_count(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->loop_overrun_count();
}

uint64_t ctx_last_cycle_work_us(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->last_cycle_work_us();
}

// ── Spinlock functions ──────────────────────────────────────────────────────

static std::optional<ChannelSide> side_from_int(int side)
{
    if (side == PLH_SIDE_AUTO) return std::nullopt;
    return static_cast<ChannelSide>(side);
}

int ctx_spinlock_lock(const PlhNativeContext *ctx, int index, int side, int timeout_ms)
{
    if (!ctx || !ctx->_api) return 0;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try
    {
        auto lock = api->get_spinlock(static_cast<size_t>(index), side_from_int(side));
        return lock.try_lock_for(timeout_ms) ? 1 : 0;
    }
    catch (...) { return 0; }
}

void ctx_spinlock_unlock(const PlhNativeContext *ctx, int index, int side)
{
    if (!ctx || !ctx->_api) return;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try
    {
        auto lock = api->get_spinlock(static_cast<size_t>(index), side_from_int(side));
        lock.unlock();
    }
    catch (...) {}
}

uint32_t ctx_spinlock_count(const PlhNativeContext *ctx, int side)
{
    if (!ctx || !ctx->_api) return 0;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try { return api->spinlock_count(side_from_int(side)); }
    catch (...) { return 0; }
}

int ctx_spinlock_is_locked(const PlhNativeContext *ctx, int index, int side)
{
    if (!ctx || !ctx->_api) return 0;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try
    {
        auto lock = api->get_spinlock(static_cast<size_t>(index), side_from_int(side));
        return lock.is_locked_by_current_process() ? 1 : 0;
    }
    catch (...) { return 0; }
}

// ── Schema size functions ───────────────────────────────────────────────────

size_t ctx_slot_logical_size(const PlhNativeContext *ctx, int side)
{
    if (!ctx || !ctx->_api) return 0;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try { return api->slot_logical_size(side_from_int(side)); }
    catch (...) { return 0; }
}

size_t ctx_flexzone_logical_size(const PlhNativeContext *ctx, int side)
{
    if (!ctx || !ctx->_api) return 0;
    auto *api = static_cast<RoleAPIBase *>(ctx->_api);
    try { return api->flexzone_logical_size(side_from_int(side)); }
    catch (...) { return 0; }
}

// ── Channel messaging functions ─────────────────────────────────────────────

int ctx_wait_for_role(const PlhNativeContext *ctx, const char *uid, int timeout_ms)
{
    if (!ctx || !ctx->_api || !uid) return 0;
    return static_cast<RoleAPIBase *>(ctx->_api)->wait_for_role(uid, timeout_ms) ? 1 : 0;
}

// ── Channel pub/sub (HEP-CORE-0030) ──────────────────────────────────────────

char *ctx_join_channel(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return nullptr;
    auto result = static_cast<RoleAPIBase *>(ctx->_api)->join_channel(channel);
    if (!result.has_value()) return nullptr;
    auto s = result->dump();
    char *out = static_cast<char *>(malloc(s.size() + 1));
    if (out) { std::memcpy(out, s.c_str(), s.size() + 1); }
    return out;
}

int ctx_leave_channel(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return 0;
    return static_cast<RoleAPIBase *>(ctx->_api)->leave_channel(channel) ? 1 : 0;
}

void ctx_send_channel_msg(const PlhNativeContext *ctx, const char *channel, const char *body_json)
{
    if (!ctx || !ctx->_api || !channel || !body_json) return;
    try
    {
        auto body = nlohmann::json::parse(body_json);
        static_cast<RoleAPIBase *>(ctx->_api)->send_channel_msg(channel, body);
    }
    catch (const nlohmann::json::exception &) {}
}

char *ctx_channel_members(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return nullptr;
    auto result = static_cast<RoleAPIBase *>(ctx->_api)->channel_members(channel);
    if (!result.has_value()) return nullptr;
    auto s = result->dump();
    char *out = static_cast<char *>(malloc(s.size() + 1));
    if (out) { std::memcpy(out, s.c_str(), s.size() + 1); }
    return out;
}

} // anonymous namespace

// ============================================================================
// NativeContextStorage — owns the C-linkage context strings
// ============================================================================

struct NativeEngine::NativeContextStorage
{
    PlhNativeContext ctx{};

    // Storage for strings (PlhNativeContext holds pointers into these).
    std::string role_tag;
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string role_dir;
    std::string log_label;  ///< e.g. "[native libfoo.so]"

    void wire(RoleHostCore *core, RoleAPIBase *api)
    {
        assert(core != nullptr && "RoleHostCore must not be null");
        assert(api != nullptr && "RoleAPIBase must not be null");

        // Identity strings.
        ctx.role_tag    = role_tag.c_str();
        ctx.uid         = uid.c_str();
        ctx.name        = name.c_str();
        ctx.channel     = channel.c_str();
        ctx.out_channel = out_channel.empty() ? nullptr : out_channel.c_str();
        ctx.log_level   = log_level.c_str();
        ctx.role_dir    = role_dir.c_str();

        // Magic sentinels + opaque host data.
        ctx._magic     = PLH_CONTEXT_MAGIC;
        ctx._magic_end = PLH_CONTEXT_MAGIC;
        ctx._core = core;
        ctx._api  = api;
        ctx._log_label = log_label.c_str();

        // Framework API function pointers.
        ctx.log                = ctx_log;
        ctx.report_metric      = ctx_report_metric;
        ctx.clear_custom_metrics = ctx_clear_custom_metrics;
        ctx.request_stop       = ctx_request_stop;
        ctx.set_critical_error = ctx_set_critical_error;
        ctx.is_critical_error  = ctx_is_critical_error;
        ctx.stop_reason        = ctx_stop_reason;
        ctx.out_slots_written  = ctx_out_slots_written;
        ctx.in_slots_received  = ctx_in_slots_received;
        ctx.out_drop_count     = ctx_out_drop_count;
        ctx.script_error_count = ctx_script_error_count;
        ctx.loop_overrun_count = ctx_loop_overrun_count;
        ctx.last_cycle_work_us = ctx_last_cycle_work_us;

        // Spinlock
        ctx.spinlock_lock      = ctx_spinlock_lock;
        ctx.spinlock_unlock    = ctx_spinlock_unlock;
        ctx.spinlock_count     = ctx_spinlock_count;
        ctx.spinlock_is_locked = ctx_spinlock_is_locked;

        // Schema sizes
        ctx.slot_logical_size    = ctx_slot_logical_size;
        ctx.flexzone_logical_size = ctx_flexzone_logical_size;

        ctx.wait_for_role  = ctx_wait_for_role;

        // Channel pub/sub (HEP-CORE-0030)
        ctx.join_channel      = ctx_join_channel;
        ctx.leave_channel     = ctx_leave_channel;
        ctx.send_channel_msg  = ctx_send_channel_msg;
        ctx.channel_members   = ctx_channel_members;
    }
};

// ============================================================================
// Constructor / Destructor (in .cpp for Pimpl completeness)
// ============================================================================

NativeEngine::NativeEngine() = default;

NativeEngine::~NativeEngine()
{
    if (dl_handle_)
    {
        if (fn_finalize_)
            fn_finalize_();
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
    }
}

// ============================================================================
// init_engine_
// ============================================================================

bool NativeEngine::init_engine_(const std::string &log_tag, RoleHostCore * /*core*/)
{
    log_tag_ = log_tag;
    return true;
}

// ============================================================================
// load_script — dlopen the .so, resolve symbols, verify integrity
// ============================================================================

bool NativeEngine::load_script(const std::filesystem::path &script_dir,
                                const std::string &entry_point,
                                const std::string &required_callback)
{
    // For native engines: try script_dir/entry_point first, then script_dir alone.
    lib_path_ = script_dir / entry_point;
    LOGGER_DEBUG("[{}] native: trying path: {}", log_tag_, lib_path_.string());
    if (!std::filesystem::exists(lib_path_))
    {
        lib_path_ = script_dir;
        LOGGER_DEBUG("[{}] native: fallback path: {}", log_tag_, lib_path_.string());
        if (!std::filesystem::exists(lib_path_))
        {
            LOGGER_ERROR("[{}] native engine not found: {} (also tried: {})",
                         log_tag_, lib_path_.string(), (script_dir / entry_point).string());
            return false;
        }
    }

    // ── File integrity check ────────────────────────────────────────
    if (!verify_file_checksum_())
        return false;

    // ── dlopen ──────────────────────────────────────────────────────
    dl_handle_ = DL_OPEN(lib_path_.string().c_str());
    if (!dl_handle_)
    {
        LOGGER_ERROR("[{}] dlopen failed: {} — {}", log_tag_, lib_path_.string(), DL_ERROR());
        return false;
    }

    // ── ABI check ──────────────────────────────────────────────────
    if (!verify_abi_())
    {
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    // ── Resolve required symbols ───────────────────────────────────
    fn_init_     = reinterpret_cast<FnNativeInit>(resolve_sym_("native_init"));
    fn_finalize_ = reinterpret_cast<FnNativeFinalize>(resolve_sym_("native_finalize"));

    if (!fn_init_ || !fn_finalize_)
    {
        LOGGER_ERROR("[{}] native engine missing required symbols: native_init and/or native_finalize",
                     log_tag_);
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    // ── Resolve optional callback symbols ──────────────────────────
    fn_on_init_       = reinterpret_cast<FnVoid>(resolve_sym_("on_init"));
    fn_on_stop_       = reinterpret_cast<FnVoid>(resolve_sym_("on_stop"));
    fn_on_produce_    = reinterpret_cast<FnOnProduce>(resolve_sym_("on_produce"));
    fn_on_consume_    = reinterpret_cast<FnOnConsume>(resolve_sym_("on_consume"));
    fn_on_process_    = reinterpret_cast<FnOnProcess>(resolve_sym_("on_process"));
    fn_on_inbox_      = reinterpret_cast<FnOnInbox>(resolve_sym_("on_inbox"));
    fn_on_heartbeat_  = reinterpret_cast<FnVoid>(resolve_sym_("on_heartbeat"));
    fn_is_thread_safe_ = reinterpret_cast<FnBool>(resolve_sym_("native_is_thread_safe"));

    // ── Check required callback ────────────────────────────────────
    if (!required_callback.empty() && !has_callback(required_callback))
    {
        LOGGER_ERROR("[{}] native engine missing required callback: {}", log_tag_, required_callback);
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    // Log native engine info.
    auto fn_name = reinterpret_cast<FnCstr>(resolve_sym_("native_name"));
    auto fn_ver  = reinterpret_cast<FnCstr>(resolve_sym_("native_version"));
    LOGGER_INFO("[{}] native engine loaded: {} v{} from {}",
                log_tag_,
                fn_name ? fn_name() : "(unnamed)",
                fn_ver  ? fn_ver()  : "?",
                lib_path_.string());

    return true;
}

// ============================================================================
// build_api_ — populate native engine context and call native_init
// ============================================================================

bool NativeEngine::build_api_(RoleAPIBase &api)
{
    native_ctx_ = std::make_unique<NativeContextStorage>();
    native_ctx_->role_tag    = api.role_tag();
    native_ctx_->uid         = api.uid();
    native_ctx_->name        = api.name();
    native_ctx_->channel     = api.channel();
    native_ctx_->out_channel = api.out_channel();
    native_ctx_->log_level   = api.log_level();
    native_ctx_->log_label   = "[native " + lib_path_.filename().string() + "]";
    native_ctx_->role_dir    = api.role_dir();
    native_ctx_->wire(api.core(), &api);

    if (!fn_init_(&native_ctx_->ctx))
    {
        LOGGER_ERROR("[{}] native_init returned false", log_tag_);
        return false;
    }

    // Register as a dynamic lifecycle module for timeout-guarded finalization.
    // The NativeEngine + external lib are one atomic unit:
    //   startup  = already done (dlopen + native_init above)
    //   shutdown = native_finalize + dlclose (timeout-guarded by lifecycle)
    lifecycle_module_name_ = "NativeEngine:" + lib_path_.filename().string();
    {
        pylabhub::utils::ModuleDef mod(lifecycle_module_name_.c_str());
        mod.add_dependency("pylabhub::utils::Logger");
        mod.set_startup([](const char *, void *) {}, ""); // no-op
        mod.set_shutdown(
            [](const char *, void *) {},
            std::chrono::milliseconds{5000});
        if (pylabhub::utils::RegisterDynamicModule(std::move(mod)))
        {
            pylabhub::utils::LoadModule(lifecycle_module_name_.c_str());
            lifecycle_registered_ = true;
            LOGGER_DEBUG("[{}] lifecycle module registered: {}", log_tag_, lifecycle_module_name_);
        }
    }

    return true;
}

// ============================================================================
// finalize_engine_
// ============================================================================

void NativeEngine::finalize_engine_()
{
    // Call native engine's finalize before unloading.
    if (fn_finalize_)
        fn_finalize_();
    fn_finalize_ = nullptr;

    native_ctx_.reset();

    if (dl_handle_)
    {
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
    }

    // Clear all function pointers (all invalid after dlclose).
    fn_init_ = nullptr;
    fn_on_init_ = nullptr;
    fn_on_stop_ = nullptr;
    fn_on_produce_ = nullptr;
    fn_on_consume_ = nullptr;
    fn_on_process_ = nullptr;
    fn_on_inbox_ = nullptr;
    fn_on_heartbeat_ = nullptr;
    fn_is_thread_safe_ = nullptr;

    // Unregister from lifecycle (if registered).
    if (lifecycle_registered_)
    {
        pylabhub::utils::UnloadModule(lifecycle_module_name_.c_str());
        lifecycle_registered_ = false;
    }
}

// ============================================================================
// has_callback
// ============================================================================

bool NativeEngine::has_callback(const std::string &name) const
{
    if (name == "on_init")       return fn_on_init_ != nullptr;
    if (name == "on_stop")       return fn_on_stop_ != nullptr;
    if (name == "on_produce")    return fn_on_produce_ != nullptr;
    if (name == "on_consume")    return fn_on_consume_ != nullptr;
    if (name == "on_process")    return fn_on_process_ != nullptr;
    if (name == "on_inbox")      return fn_on_inbox_ != nullptr;
    if (name == "on_heartbeat")  return fn_on_heartbeat_ != nullptr;
    // Only known callback names are recognized. Generic invoke() does
    // its own dlsym at call time — has_callback is for the defined
    // callback vocabulary, not arbitrary exported symbols.
    return false;
}

// ============================================================================
// register_slot_type — validate schema against native engine's compiled struct
// ============================================================================

bool NativeEngine::register_slot_type(const hub::SchemaSpec &spec,
                                       const std::string &type_name,
                                       const std::string &packing)
{
    if (!spec.has_schema)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') called with has_schema=false",
                     log_tag_, type_name);
        return false;
    }

    // Compute expected size from schema (infrastructure-authoritative).
    auto [layout, expected_size] = hub::compute_field_layout(to_field_descs(spec.fields), packing);

    // Compute expected canonical schema from config.
    std::string expected_schema = compute_canonical_schema_(spec);

    // Check native engine's schema descriptor (if exported).
    std::string sym_schema = "native_schema_" + type_name;
    auto fn_schema = reinterpret_cast<FnSchemaDesc>(resolve_sym_(sym_schema.c_str()));

    if (fn_schema)
    {
        const char *native_schema = fn_schema();
        if (native_schema && expected_schema != native_schema)
        {
            LOGGER_ERROR("[{}] schema mismatch for {}: config='{}', native engine='{}'",
                         log_tag_, type_name, expected_schema, native_schema);
            return false;
        }
    }
    else
    {
        LOGGER_WARN("[{}] native engine does not export {} — skipping schema validation for {}",
                    log_tag_, sym_schema, type_name);
    }

    // Check native engine's sizeof (if exported).
    std::string sym_sizeof = "native_sizeof_" + type_name;
    auto fn_sz = reinterpret_cast<FnSizeof>(resolve_sym_(sym_sizeof.c_str()));

    if (!fn_sz)
    {
        LOGGER_ERROR("[{}] native engine must export '{}' for type '{}'",
                     log_tag_, sym_sizeof, type_name);
        return false;
    }

    size_t native_sz = fn_sz();
    if (native_sz != expected_size)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') size mismatch: "
                     "native={}, schema={}",
                     log_tag_, type_name, native_sz, expected_size);
        return false;
    }
    type_sizes_[type_name] = native_sz;

    return true;
}

size_t NativeEngine::type_sizeof(const std::string &type_name) const
{
    auto it = type_sizes_.find(type_name);
    return it != type_sizes_.end() ? it->second : 0;
}

// ============================================================================
// Callback invocations — zero-marshaling direct function pointer calls
// ============================================================================

void NativeEngine::invoke_on_init()
{
    if (fn_on_init_)
        fn_on_init_(nullptr);
}

void NativeEngine::invoke_on_stop()
{
    if (fn_on_stop_)
        fn_on_stop_(nullptr);
}

InvokeResult NativeEngine::invoke_produce(
    InvokeTx tx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_produce_)
        return InvokeResult::Discard;
    plh_tx_t c_tx{tx.slot, tx.slot_size, tx.fz, tx.fz_size};
    return fn_on_produce_(&c_tx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_consume(
    InvokeRx rx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_consume_)
        return InvokeResult::Discard;
    plh_rx_t c_rx{rx.slot, rx.slot_size, rx.fz, rx.fz_size};
    return fn_on_consume_(&c_rx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_process(
    InvokeRx rx, InvokeTx tx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_process_)
        return InvokeResult::Discard;
    plh_rx_t c_rx{rx.slot, rx.slot_size, rx.fz, rx.fz_size};
    plh_tx_t c_tx{tx.slot, tx.slot_size, tx.fz, tx.fz_size};
    return fn_on_process_(&c_rx, &c_tx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_on_inbox(
    InvokeInbox msg)
{
    if (!fn_on_inbox_)
        return InvokeResult::Discard;
    plh_inbox_msg_t c_msg{msg.data, msg.data_size, msg.sender_uid.c_str(), msg.seq};
    return fn_on_inbox_(&c_msg) ? InvokeResult::Commit : InvokeResult::Discard;
}

// ============================================================================
// Generic invoke / eval
// ============================================================================

bool NativeEngine::invoke(const std::string &name)
{
    if (name == "on_heartbeat" && fn_on_heartbeat_)
    {
        fn_on_heartbeat_(nullptr);
        return true;
    }
    auto fn = reinterpret_cast<FnVoid>(resolve_sym_(name.c_str()));
    if (fn)
    {
        fn(nullptr);
        return true;
    }
    return false;
}

bool NativeEngine::invoke(const std::string &name, const nlohmann::json &args)
{
    if (name == "on_heartbeat" && fn_on_heartbeat_)
    {
        if (args.empty())
        {
            fn_on_heartbeat_(nullptr);
        }
        else
        {
            std::string json_str = args.dump();
            fn_on_heartbeat_(json_str.c_str());
        }
        return true;
    }
    auto fn = reinterpret_cast<FnVoid>(resolve_sym_(name.c_str()));
    if (fn)
    {
        if (args.empty())
        {
            fn(nullptr);
        }
        else
        {
            std::string json_str = args.dump();
            fn(json_str.c_str());
        }
        return true;
    }
    return false;
}

InvokeResponse NativeEngine::eval(const std::string & /*code*/)
{
    // eval() is not applicable to compiled native engines.
    return {InvokeStatus::NotFound, {}};
}

// ============================================================================
// Error state / threading
// ============================================================================

uint64_t NativeEngine::script_error_count() const noexcept
{
    return api_ ? api_->core()->script_error_count() : 0;
}

bool NativeEngine::supports_multi_state() const noexcept
{
    return fn_is_thread_safe_ && fn_is_thread_safe_();
}

void NativeEngine::set_expected_checksum(std::string hex_checksum)
{
    expected_checksum_ = std::move(hex_checksum);
}

// ============================================================================
// Internal helpers
// ============================================================================

void *NativeEngine::resolve_sym_(const char *name) const
{
    if (!dl_handle_ || !name)
        return nullptr;
    return DL_SYM(dl_handle_, name);
}

bool NativeEngine::verify_abi_() const
{
    auto fn = reinterpret_cast<const PlhAbiInfo *(*)()>(
        resolve_sym_("native_abi_info"));

    if (!fn)
    {
        LOGGER_WARN("[{}] native engine does not export native engine_abi_info — skipping ABI check", log_tag_);
        return true; // permissive: allow if not exported
    }

    const PlhAbiInfo *info = fn();
    if (!info)
    {
        LOGGER_ERROR("[{}] native engine_abi_info returned null", log_tag_);
        return false;
    }

    // Version guard: if native engine was compiled with a newer struct, it may have
    // fields we don't know about — still safe if the fields we check match.
    if (info->struct_size < sizeof(PlhAbiInfo))
    {
        LOGGER_ERROR("[{}] native engine ABI struct too small ({} < {})",
                     log_tag_, info->struct_size, sizeof(PlhAbiInfo));
        return false;
    }

    // Pointer size mismatch = ABI incompatible (32/64-bit).
    if (info->sizeof_void_ptr != sizeof(void *))
    {
        LOGGER_ERROR("[{}] pointer size mismatch: native engine={}, host={}",
                     log_tag_, info->sizeof_void_ptr,
                     static_cast<uint32_t>(sizeof(void *)));
        return false;
    }

    if (info->sizeof_size_t != sizeof(size_t))
    {
        LOGGER_ERROR("[{}] size_t mismatch: native engine={}, host={}",
                     log_tag_, info->sizeof_size_t,
                     static_cast<uint32_t>(sizeof(size_t)));
        return false;
    }

    // Byte order check.
    constexpr uint32_t host_byte_order =
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        2;
#else
        1;
#endif
    if (info->byte_order != 0 && info->byte_order != host_byte_order)
    {
        LOGGER_ERROR("[{}] byte order mismatch: native engine={}, host={}",
                     log_tag_, info->byte_order, host_byte_order);
        return false;
    }

    // API version check.
    if (info->api_version != PLH_NATIVE_API_VERSION)
    {
        LOGGER_ERROR("[{}] API version mismatch: native engine={}, host={}",
                     log_tag_, info->api_version, PLH_NATIVE_API_VERSION);
        return false;
    }

    return true;
}

bool NativeEngine::verify_file_checksum_() const
{
    if (expected_checksum_.empty())
        return true; // no checksum configured — skip

    // Read file into memory and compute BLAKE2b-256.
    std::ifstream file(lib_path_, std::ios::binary | std::ios::ate);
    if (!file)
    {
        LOGGER_ERROR("[{}] cannot open native engine file for checksum: {}",
                     log_tag_, lib_path_.string());
        return false;
    }

    const auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buf(static_cast<size_t>(file_size));
    if (!file.read(buf.data(), file_size))
    {
        LOGGER_ERROR("[{}] failed to read native engine file: {}", log_tag_, lib_path_.string());
        return false;
    }

    auto hash = pylabhub::crypto::compute_blake2b_array(buf.data(), buf.size());

    // Convert to hex string for comparison.
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : hash)
    {
        static const char digits[] = "0123456789abcdef";
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 0x0f]);
    }

    if (hex != expected_checksum_)
    {
        LOGGER_ERROR("[{}] native engine checksum mismatch: expected={}, actual={}",
                     log_tag_, expected_checksum_, hex);
        return false;
    }

    LOGGER_INFO("[{}] native engine checksum verified: {}", log_tag_, hex);
    return true;
}

std::string NativeEngine::compute_canonical_schema_(const hub::SchemaSpec &spec) const
{
    // Build canonical schema string: "name:type:count:length|name:type:count:length|..."
    // Format: "name:type:count:length|name:type:count:length|..."
    // Must match the string passed to PLH_DECLARE_SCHEMA in the native engine.
    std::ostringstream ss;
    for (size_t i = 0; i < spec.fields.size(); ++i)
    {
        if (i > 0) ss << '|';
        const auto &f = spec.fields[i];
        ss << f.name << ':' << f.type_str << ':' << f.count << ':' << f.length;
    }
    return ss.str();
}

} // namespace pylabhub::scripting
