/**
 * @file native_engine.cpp
 * @brief NativeEngine — ScriptEngine implementation for native C/C++ plugins.
 *
 * Loads shared libraries via dlopen (POSIX) or LoadLibrary (Windows).
 * Resolves callback symbols at load time. Dispatches invoke calls as
 * direct function pointer calls with zero marshaling.
 *
 * See plugin_api.h for the plugin-side API.
 */
#include "native_engine.hpp"

#include "utils/crypto_utils.hpp"
#include "utils/format_tools.hpp"
#include "utils/logger.hpp"
#include "utils/plugin_api.h"
#include "utils/role_host_core.hpp"

#include <fstream>
#include <sstream>

// ── Platform dynamic loading ────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#   include <windows.h>
#   define DL_OPEN(path)      LoadLibraryA(path)
#   define DL_SYM(handle, s)  GetProcAddress(static_cast<HMODULE>(handle), s)
#   define DL_CLOSE(handle)   FreeLibrary(static_cast<HMODULE>(handle))
#   define DL_ERROR()         "LoadLibrary failed"
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

void ctx_log(const PlhPluginContext *ctx, int level, const char *msg)
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

void ctx_report_metric(const PlhPluginContext *ctx, const char *key, double value)
{
    if (!ctx || !ctx->_core || !key) return;
    static_cast<RoleHostCore *>(ctx->_core)->report_metric(key, value);
}

void ctx_clear_custom_metrics(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->clear_custom_metrics();
}

void ctx_request_stop(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->request_stop();
}

void ctx_set_critical_error(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return;
    static_cast<RoleHostCore *>(ctx->_core)->set_critical_error();
}

int ctx_is_critical_error(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->is_critical_error() ? 1 : 0;
}

const char *ctx_stop_reason(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return "normal";
    static thread_local std::string buf;
    buf = static_cast<RoleHostCore *>(ctx->_core)->stop_reason_string();
    return buf.c_str();
}

uint64_t ctx_out_written(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->out_written();
}

uint64_t ctx_in_received(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->in_received();
}

uint64_t ctx_drops(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->drops();
}

uint64_t ctx_script_errors(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->script_errors();
}

uint64_t ctx_loop_overrun_count(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->loop_overrun_count();
}

uint64_t ctx_last_cycle_work_us(const PlhPluginContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->last_cycle_work_us();
}

} // anonymous namespace

// ============================================================================
// PluginContextStorage — owns the C-linkage context strings
// ============================================================================

struct NativeEngine::PluginContextStorage
{
    PlhPluginContext ctx{};

    // Storage for strings (PlhPluginContext holds pointers into these).
    std::string role_tag;
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string role_dir;
    std::string log_label;  ///< e.g. "[native libfoo.so]"

    void wire(RoleHostCore *core)
    {
        // Identity strings.
        ctx.role_tag    = role_tag.c_str();
        ctx.uid         = uid.c_str();
        ctx.name        = name.c_str();
        ctx.channel     = channel.c_str();
        ctx.out_channel = out_channel.empty() ? nullptr : out_channel.c_str();
        ctx.log_level   = log_level.c_str();
        ctx.role_dir    = role_dir.c_str();

        // Opaque host data (used by function pointer implementations).
        ctx._core = core;
        ctx._log_label = log_label.c_str();

        // Framework API function pointers.
        ctx.log                = ctx_log;
        ctx.report_metric      = ctx_report_metric;
        ctx.clear_custom_metrics = ctx_clear_custom_metrics;
        ctx.request_stop       = ctx_request_stop;
        ctx.set_critical_error = ctx_set_critical_error;
        ctx.is_critical_error  = ctx_is_critical_error;
        ctx.stop_reason        = ctx_stop_reason;
        ctx.out_written        = ctx_out_written;
        ctx.in_received        = ctx_in_received;
        ctx.drops              = ctx_drops;
        ctx.script_errors      = ctx_script_errors;
        ctx.loop_overrun_count = ctx_loop_overrun_count;
        ctx.last_cycle_work_us = ctx_last_cycle_work_us;
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
            LOGGER_ERROR("[{}] native plugin not found: {} (also tried: {})",
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
    fn_init_     = reinterpret_cast<FnPluginInit>(resolve_sym_("plugin_init"));
    fn_finalize_ = reinterpret_cast<FnPluginFinalize>(resolve_sym_("plugin_finalize"));

    if (!fn_init_ || !fn_finalize_)
    {
        LOGGER_ERROR("[{}] plugin missing required symbols: plugin_init and/or plugin_finalize",
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
    fn_is_thread_safe_ = reinterpret_cast<FnBool>(resolve_sym_("plugin_is_thread_safe"));

    // ── Check required callback ────────────────────────────────────
    if (!required_callback.empty() && !has_callback(required_callback))
    {
        LOGGER_ERROR("[{}] plugin missing required callback: {}", log_tag_, required_callback);
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    // Log plugin info.
    auto fn_name = reinterpret_cast<FnCstr>(resolve_sym_("plugin_name"));
    auto fn_ver  = reinterpret_cast<FnCstr>(resolve_sym_("plugin_version"));
    LOGGER_INFO("[{}] native plugin loaded: {} v{} from {}",
                log_tag_,
                fn_name ? fn_name() : "(unnamed)",
                fn_ver  ? fn_ver()  : "?",
                lib_path_.string());

    return true;
}

// ============================================================================
// build_api_ — populate plugin context and call plugin_init
// ============================================================================

bool NativeEngine::build_api_(const RoleContext &ctx)
{
    plugin_ctx_ = std::make_unique<PluginContextStorage>();
    plugin_ctx_->role_tag    = ctx.role_tag;
    plugin_ctx_->uid         = ctx.uid;
    plugin_ctx_->name        = ctx.name;
    plugin_ctx_->channel     = ctx.channel;
    plugin_ctx_->out_channel = ctx.out_channel;
    plugin_ctx_->log_level   = ctx.log_level;
    plugin_ctx_->log_label   = "[native " + lib_path_.filename().string() + "]";
    plugin_ctx_->role_dir    = ctx.role_dir;
    plugin_ctx_->wire(ctx_.core);

    if (!fn_init_(&plugin_ctx_->ctx))
    {
        LOGGER_ERROR("[{}] plugin_init returned false", log_tag_);
        return false;
    }

    return true;
}

// ============================================================================
// finalize_engine_
// ============================================================================

void NativeEngine::finalize_engine_()
{
    if (fn_finalize_)
        fn_finalize_();
    fn_finalize_ = nullptr;

    plugin_ctx_.reset();

    if (dl_handle_)
    {
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
    }

    // Clear all function pointers.
    fn_init_ = nullptr;
    fn_on_init_ = nullptr;
    fn_on_stop_ = nullptr;
    fn_on_produce_ = nullptr;
    fn_on_consume_ = nullptr;
    fn_on_process_ = nullptr;
    fn_on_inbox_ = nullptr;
    fn_on_heartbeat_ = nullptr;
    fn_is_thread_safe_ = nullptr;
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
    // For generic invoke, check via dlsym.
    return dl_handle_ && resolve_sym_(name.c_str()) != nullptr;
}

// ============================================================================
// register_slot_type — validate schema against plugin's compiled struct
// ============================================================================

bool NativeEngine::register_slot_type(const SchemaSpec &spec,
                                       const std::string &type_name,
                                       const std::string & /*packing*/)
{
    if (!spec.has_schema)
        return true; // no schema to validate

    // Compute expected canonical schema from config.
    std::string expected_schema = compute_canonical_schema_(spec);

    // Check plugin's schema descriptor (if exported).
    std::string sym_schema = "plugin_schema_" + type_name;
    auto fn_schema = reinterpret_cast<FnSchemaDesc>(resolve_sym_(sym_schema.c_str()));

    if (fn_schema)
    {
        const char *plugin_schema = fn_schema();
        if (plugin_schema && expected_schema != plugin_schema)
        {
            LOGGER_ERROR("[{}] schema mismatch for {}: config='{}', plugin='{}'",
                         log_tag_, type_name, expected_schema, plugin_schema);
            return false;
        }
    }
    else
    {
        LOGGER_WARN("[{}] plugin does not export {} — skipping schema validation for {}",
                    log_tag_, sym_schema, type_name);
    }

    // Check plugin's sizeof (if exported).
    std::string sym_sizeof = "plugin_sizeof_" + type_name;
    auto fn_sz = reinterpret_cast<FnSizeof>(resolve_sym_(sym_sizeof.c_str()));

    if (fn_sz)
    {
        size_t plugin_sz = fn_sz();
        // Store for type_sizeof() queries.
        type_sizes_[type_name] = plugin_sz;
    }
    else
    {
        // No sizeof exported — can't validate size. Store 0.
        type_sizes_[type_name] = 0;
    }

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
        fn_on_init_();
}

void NativeEngine::invoke_on_stop()
{
    if (fn_on_stop_)
        fn_on_stop_();
}

InvokeResult NativeEngine::invoke_produce(
    void *out_slot, size_t out_sz,
    void *flexzone, size_t fz_sz, const char * /*fz_type*/,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_produce_)
        return InvokeResult::Discard;
    bool commit = fn_on_produce_(out_slot, out_sz, flexzone, fz_sz);
    return commit ? InvokeResult::Commit : InvokeResult::Discard;
}

void NativeEngine::invoke_consume(
    const void *in_slot, size_t in_sz,
    const void *flexzone, size_t fz_sz, const char * /*fz_type*/,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (fn_on_consume_)
        fn_on_consume_(in_slot, in_sz, flexzone, fz_sz);
}

InvokeResult NativeEngine::invoke_process(
    const void *in_slot, size_t in_sz,
    void *out_slot, size_t out_sz,
    void *flexzone, size_t fz_sz, const char * /*fz_type*/,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_process_)
        return InvokeResult::Discard;
    bool commit = fn_on_process_(in_slot, in_sz, out_slot, out_sz, flexzone, fz_sz);
    return commit ? InvokeResult::Commit : InvokeResult::Discard;
}

void NativeEngine::invoke_on_inbox(
    const void *data, size_t sz,
    const std::string &sender)
{
    if (fn_on_inbox_)
        fn_on_inbox_(data, sz, sender.c_str());
}

// ============================================================================
// Generic invoke / eval
// ============================================================================

bool NativeEngine::invoke(const std::string &name)
{
    if (name == "on_heartbeat" && fn_on_heartbeat_)
    {
        fn_on_heartbeat_();
        return true;
    }
    // Try dlsym for arbitrary symbol.
    auto fn = reinterpret_cast<FnVoid>(resolve_sym_(name.c_str()));
    if (fn)
    {
        fn();
        return true;
    }
    return false;
}

bool NativeEngine::invoke(const std::string &name, const nlohmann::json & /*args*/)
{
    // Native plugins don't support args for generic invoke (use callbacks instead).
    return invoke(name);
}

InvokeResponse NativeEngine::eval(const std::string & /*code*/)
{
    // eval() is not applicable to compiled plugins.
    return {InvokeStatus::NotFound, {}};
}

// ============================================================================
// Error state / threading
// ============================================================================

uint64_t NativeEngine::script_error_count() const noexcept
{
    return ctx_.core ? ctx_.core->script_errors() : 0;
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
        resolve_sym_("plugin_abi_info"));

    if (!fn)
    {
        LOGGER_WARN("[{}] plugin does not export plugin_abi_info — skipping ABI check", log_tag_);
        return true; // permissive: allow if not exported
    }

    const PlhAbiInfo *info = fn();
    if (!info)
    {
        LOGGER_ERROR("[{}] plugin_abi_info returned null", log_tag_);
        return false;
    }

    // Version guard: if plugin was compiled with a newer struct, it may have
    // fields we don't know about — still safe if the fields we check match.
    if (info->struct_size < sizeof(PlhAbiInfo))
    {
        LOGGER_ERROR("[{}] plugin ABI struct too small ({} < {})",
                     log_tag_, info->struct_size, sizeof(PlhAbiInfo));
        return false;
    }

    // Pointer size mismatch = ABI incompatible (32/64-bit).
    if (info->sizeof_void_ptr != sizeof(void *))
    {
        LOGGER_ERROR("[{}] pointer size mismatch: plugin={}, host={}",
                     log_tag_, info->sizeof_void_ptr,
                     static_cast<uint32_t>(sizeof(void *)));
        return false;
    }

    if (info->sizeof_size_t != sizeof(size_t))
    {
        LOGGER_ERROR("[{}] size_t mismatch: plugin={}, host={}",
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
        LOGGER_ERROR("[{}] byte order mismatch: plugin={}, host={}",
                     log_tag_, info->byte_order, host_byte_order);
        return false;
    }

    // API version check.
    if (info->api_version != PLH_PLUGIN_API_VERSION)
    {
        LOGGER_ERROR("[{}] API version mismatch: plugin={}, host={}",
                     log_tag_, info->api_version, PLH_PLUGIN_API_VERSION);
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
        LOGGER_ERROR("[{}] cannot open plugin file for checksum: {}",
                     log_tag_, lib_path_.string());
        return false;
    }

    const auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buf(static_cast<size_t>(file_size));
    if (!file.read(buf.data(), file_size))
    {
        LOGGER_ERROR("[{}] failed to read plugin file: {}", log_tag_, lib_path_.string());
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
        LOGGER_ERROR("[{}] plugin checksum mismatch: expected={}, actual={}",
                     log_tag_, expected_checksum_, hex);
        return false;
    }

    LOGGER_INFO("[{}] plugin checksum verified: {}", log_tag_, hex);
    return true;
}

std::string NativeEngine::compute_canonical_schema_(const SchemaSpec &spec) const
{
    // Build canonical schema string: "name:type:count:length|name:type:count:length|..."
    // Same format as compute_schema_hash() in script_host_helpers.hpp.
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
