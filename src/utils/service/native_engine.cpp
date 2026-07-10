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
#include "utils/format_tools.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"
#include "utils/native_engine_api.h"
#include "plh_version_registry.hpp"   // ComponentVersions + check_abi (HEP-0032)
#include "utils/schema_field_layout.hpp"
#include "utils/schema_utils.hpp"        // compute_schema_size
#include "utils/role_host_core.hpp"
#include "utils/hub_api.hpp"          // build_api_(HubAPI&) — B13 fix 2026-05-21

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

// Hub-side variant (audit B13, 2026-05-21).  _core is null for hub
// scripts; _api holds a HubAPI* and dispatches via the host's
// shutdown surface.  ctx_request_stop() above can't be reused
// because it casts _core which is null in hub builds.
void ctx_request_stop_hub(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return;
    static_cast<hub_host::HubAPI *>(ctx->_api)->request_shutdown();
}

// ── Hub-side API surface (API v7 #84, 2026-06-11) ──────────────────────
//
// Implementations of the ctx->hub_* fn ptrs.  Each one:
//   1. Validates ctx + _api are non-null (defensive — the framework
//      always supplies them, but the C ABI is robust to plugin errors).
//   2. Calls the corresponding HubAPI method inside a try/catch.
//   3. For JSON-returning fns: writes the result into a thread-local
//      std::string and returns its c_str().  Lifetime contract:
//      pointer valid until the NEXT ctx_hub_*_json call on the SAME
//      thread (any subsequent call overwrites the shared buffer).
//
// The shared buffer is intentional: 13 separate thread_local strings
// would cost more memory and complicate the lifetime story.  One
// buffer + one documented "valid until next call" rule is simpler.
//
// Symmetric to ctx_metrics_snapshot, plugin authors must drop the
// pointer in on_stop() to avoid post-finalize aliasing.
namespace
{
// Shared thread-local JSON-string scratch.  See lifetime contract above.
std::string &hub_json_scratch() noexcept
{
    static thread_local std::string buf;
    return buf;
}

// Always returns a valid C string into the thread-local scratch.
// On error (null ctx/api, HubAPI threw): buf cleared to "" and the
// returned pointer is still valid until the next hub_*_json call on
// the same thread.  Plugin authors can rely on a non-null return —
// they distinguish "no data" from "error" by string emptiness +
// out-of-band signal (logs), or by parsing the returned JSON.
template <typename Fn>
const char *hub_json_invoke(const PlhNativeContext *ctx, Fn &&fn) noexcept
{
    auto &buf = hub_json_scratch();
    if (!ctx || !ctx->_api) { buf.clear(); return buf.c_str(); }
    try
    {
        buf = fn(*static_cast<hub_host::HubAPI *>(ctx->_api)).dump();
    }
    catch (...) { buf.clear(); }
    return buf.c_str();
}
}  // namespace

const char *ctx_hub_metrics_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.metrics(); });
}

const char *ctx_hub_config_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.config(); });
}

const char *ctx_hub_query_metrics_json(const PlhNativeContext *ctx,
                                       const char *categories_json)
{
    return hub_json_invoke(ctx,
        [categories_json](hub_host::HubAPI &api) {
            std::vector<std::string> cats;
            if (categories_json && *categories_json)
            {
                const auto j = nlohmann::json::parse(categories_json);
                if (j.is_array())
                    for (const auto &e : j)
                        if (e.is_string()) cats.push_back(e.get<std::string>());
            }
            return api.query_metrics(cats);
        });
}

const char *ctx_hub_list_channels_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.list_channels(); });
}

const char *ctx_hub_get_channel_json(const PlhNativeContext *ctx,
                                     const char *name)
{
    if (!name) return nullptr;
    return hub_json_invoke(ctx,
        [name](hub_host::HubAPI &api) { return api.get_channel(name); });
}

const char *ctx_hub_list_roles_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.list_roles(); });
}

const char *ctx_hub_get_role_json(const PlhNativeContext *ctx,
                                  const char *role_uid)
{
    if (!role_uid) return nullptr;
    return hub_json_invoke(ctx,
        [role_uid](hub_host::HubAPI &api) { return api.get_role(role_uid); });
}

const char *ctx_hub_list_bands_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.list_bands(); });
}

const char *ctx_hub_get_band_json(const PlhNativeContext *ctx,
                                  const char *name)
{
    if (!name) return nullptr;
    return hub_json_invoke(ctx,
        [name](hub_host::HubAPI &api) { return api.get_band(name); });
}

const char *ctx_hub_list_peers_json(const PlhNativeContext *ctx)
{
    return hub_json_invoke(ctx,
        [](hub_host::HubAPI &api) { return api.list_peers(); });
}

const char *ctx_hub_get_peer_json(const PlhNativeContext *ctx,
                                  const char *hub_uid)
{
    if (!hub_uid) return nullptr;
    return hub_json_invoke(ctx,
        [hub_uid](hub_host::HubAPI &api) { return api.get_peer(hub_uid); });
}

int ctx_hub_close_channel(const PlhNativeContext *ctx, const char *name)
{
    if (!ctx || !ctx->_api || !name) return -1;
    try
    {
        static_cast<hub_host::HubAPI *>(ctx->_api)->close_channel(name);
        return 1;
    }
    catch (...) { return -1; }
}

int ctx_hub_broadcast_channel(const PlhNativeContext *ctx,
                              const char *channel,
                              const char *message,
                              const char *data_json)
{
    if (!ctx || !ctx->_api || !channel || !message) return -1;
    try
    {
        static_cast<hub_host::HubAPI *>(ctx->_api)->broadcast_channel(
            channel, message, data_json ? data_json : "");
        return 1;
    }
    catch (...) { return -1; }
}

int ctx_hub_post_event(const PlhNativeContext *ctx,
                       const char *name,
                       const char *data_json)
{
    if (!ctx || !ctx->_api || !name) return -1;
    try
    {
        nlohmann::json body = nlohmann::json::object();
        if (data_json && *data_json)
            body = nlohmann::json::parse(data_json);
        static_cast<hub_host::HubAPI *>(ctx->_api)->post_event(name, body);
        return 1;
    }
    catch (const std::invalid_argument &)
    {
        // Bad identifier per HEP-CORE-0033 G2.2.0b grammar — distinct
        // from generic errors so plugin can decide whether to retry.
        return 0;
    }
    catch (...) { return -1; }
}

int64_t ctx_hub_augment_timeout_ms(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return -1;
    try
    {
        return static_cast<hub_host::HubAPI *>(ctx->_api)->augment_timeout_ms();
    }
    catch (...) { return -1; }
}

void ctx_hub_set_augment_timeout(const PlhNativeContext *ctx, int64_t ms)
{
    if (!ctx || !ctx->_api) return;
    try
    {
        static_cast<hub_host::HubAPI *>(ctx->_api)->set_augment_timeout(ms);
    }
    catch (...) { /* defensive: nothing observable to do */ }
}

// ── Hub-side stubs for role-only fn ptrs (API v7 #84, Part A) ──────────
//
// Each stub returns the documented sentinel for its return type.
// Wired into wire_hub() so pure-C hub plugins can call ANY ctx fn
// without crashing — same shape as a wired-but-unready surface.
// See HEP-CORE-0028 §4.9 "Hub-side API surface" matrix.

void     hub_stub_void_role_state(const PlhNativeContext *) noexcept {}
void     hub_stub_report_metric(const PlhNativeContext *, const char *, double) noexcept {}
void     hub_stub_set_critical_error(const PlhNativeContext *, const char *) noexcept {}
int      hub_stub_is_critical_error(const PlhNativeContext *) noexcept { return 0; }
int      hub_stub_stop_reason(const PlhNativeContext *) noexcept { return PLH_STOP_REASON_NORMAL; }
uint64_t hub_stub_counter_zero(const PlhNativeContext *) noexcept { return 0; }
int      hub_stub_spinlock_lock(const PlhNativeContext *, int, int, int) noexcept { return 0; }
void     hub_stub_spinlock_unlock(const PlhNativeContext *, int, int) noexcept {}
uint32_t hub_stub_spinlock_count(const PlhNativeContext *, int) noexcept { return 0; }
int      hub_stub_spinlock_is_locked(const PlhNativeContext *, int, int) noexcept { return 0; }
size_t   hub_stub_size_arg2_zero(const PlhNativeContext *, int) noexcept { return 0; }
int      hub_stub_wait_for_role(const PlhNativeContext *, const char *, int) noexcept { return 0; }
int      hub_stub_band_int_arg2(const PlhNativeContext *, const char *) noexcept { return -1; }
void     hub_stub_band_broadcast(const PlhNativeContext *, const char *, const char *) noexcept {}
int      hub_stub_band_members(const PlhNativeContext *, const char *, plh_band_member_visitor, void *) noexcept { return -1; }
int      hub_stub_band_member_contains(const PlhNativeContext *, const char *, const char *) noexcept { return -1; }
int      hub_stub_allowed_peers(const PlhNativeContext *, const char *, plh_allowed_peer_visitor, void *) noexcept { return -1; }
int      hub_stub_role_uid_visit(const PlhNativeContext *, const char *, plh_role_uid_visitor, void *) noexcept { return -1; }
int      hub_stub_allowed_peer_contains(const PlhNativeContext *, const char *, const char *) noexcept { return -1; }
int      hub_stub_is_channel_ready(const PlhNativeContext *, const char *) noexcept { return -1; }
int      hub_stub_queue_mechanism(const PlhNativeContext *, int) noexcept { return PLH_MECHANISM_UNINITIALIZED; }
int      hub_stub_queue_policy(const PlhNativeContext *) noexcept { return PLH_QUEUE_POLICY_UNKNOWN; }
const void *hub_stub_metrics_snapshot(const PlhNativeContext *) noexcept { return nullptr; }
int      hub_stub_metrics_get(const void *, const char *, double *) noexcept { return -1; }
int      hub_stub_is_in_band(const PlhNativeContext *, const char *) noexcept { return -1; }
int      hub_stub_update_flexzone_checksum(const PlhNativeContext *) noexcept { return 0; }
void     hub_stub_set_verify_checksum(const PlhNativeContext *, int) noexcept {}

// ── Role-side stubs for hub-only fn ptrs (API v7, #84, 2026-06-11) ────
// When a plugin runs on a ROLE-side ctx, the hub_* methods are
// meaningless (the role has no HubAPI* reference).  We wire them to
// noop stubs returning "" (empty JSON / sentinel) so plugins can call
// any ctx fn without crashing.  See HEP-CORE-0028 §4.9.
extern "C"
{
const char *role_stub_hub_json_no_arg(const PlhNativeContext *) noexcept { return ""; }
const char *role_stub_hub_json_query(const PlhNativeContext *, const char *) noexcept { return ""; }
const char *role_stub_hub_json_query_arg(const PlhNativeContext *, const char *) noexcept { return ""; }
int     role_stub_hub_close_channel(const PlhNativeContext *, const char *) noexcept { return -1; }
int     role_stub_hub_broadcast_channel(const PlhNativeContext *, const char *, const char *, const char *) noexcept { return -1; }
int     role_stub_hub_post_event(const PlhNativeContext *, const char *, const char *) noexcept { return -1; }
int64_t role_stub_hub_augment_timeout_ms(const PlhNativeContext *) noexcept { return 0; }
void    role_stub_hub_set_augment_timeout(const PlhNativeContext *, int64_t) noexcept {}
}

void ctx_set_critical_error(const PlhNativeContext *ctx, const char *msg)
{
    // Audit S2 (2026-05-18) — `msg` is REQUIRED per the C ABI v3
    // contract.  A NULL msg from a plugin is a plugin bug; we log a
    // placeholder so the role still gets a breadcrumb in the log
    // and still flag critical (graceful degradation rather than
    // crashing in LOGGER_ERROR on NULL deref).  Plugin authors:
    // always pass a non-NULL string describing the unrecoverable
    // condition.  Routed through RoleAPIBase so the log format
    // matches Python and Lua.
    if (!ctx) return;
    const std::string_view sv = msg
        ? std::string_view{msg}
        : std::string_view{"(no message — C plugin passed NULL; bug)"};
    if (ctx->_api)
    {
        auto *api = static_cast<RoleAPIBase *>(ctx->_api);
        api->set_critical_error(sv);
        return;
    }
    if (ctx->_core)
        static_cast<RoleHostCore *>(ctx->_core)->set_critical_error();
}

int ctx_is_critical_error(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_core) return 0;
    return static_cast<RoleHostCore *>(ctx->_core)->is_critical_error() ? 1 : 0;
}

int ctx_stop_reason(const PlhNativeContext *ctx)
{
    // API v6 (#194): returns a PLH_STOP_REASON_* macro value.
    // Cast from the typed enum on RoleHostCore — values match the
    // PLH_STOP_REASON_* macros at the top of native_engine_api.h, so
    // the cast is identity-preserving.
    if (!ctx || !ctx->_core) return PLH_STOP_REASON_NORMAL;
    return static_cast<int>(
        static_cast<RoleHostCore *>(ctx->_core)->stop_reason());
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
    catch (const std::exception &e)
    {
        // Swallow — C ABI boundary into native script.  Unlock failure
        // is rare (typically PID mismatch on a stale lock).  Log at
        // WARN so regressions don't disappear silently.
        LOGGER_WARN("native_engine: ctx_spinlock_unlock(idx={}) threw: {}",
                    index, e.what());
    }
    catch (...)
    {
        LOGGER_WARN("native_engine: ctx_spinlock_unlock(idx={}) "
                    "threw (non-std exception)", index);
    }
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

// ── Channel/role messaging functions ────────────────────────────────────────

int ctx_wait_for_role(const PlhNativeContext *ctx, const char *uid, int timeout_ms)
{
    if (!ctx || !ctx->_api || !uid) return 0;
    return static_cast<RoleAPIBase *>(ctx->_api)->wait_for_role(uid, timeout_ms) ? 1 : 0;
}

// ── Helper: map RoleAPIBase policy_info() string → PLH_QUEUE_POLICY_* macro ─

int policy_string_to_macro(const std::string &s) noexcept
{
    // Maps the strings reported by ShmQueue::policy_info() and
    // ZmqQueue::policy_info() onto the v6 macro codomain.  See
    // hub_shm_queue.cpp + hub_zmq_queue.cpp for the originating strings.
    // The "shm_unconnected" / "zmq_unconnected" placeholders (returned
    // when the queue impl pImpl is null) fall through to the final
    // PLH_QUEUE_POLICY_UNKNOWN return — same as the empty string and
    // any unrecognized identifier.
    if (s.empty())                              return PLH_QUEUE_POLICY_UNKNOWN;
    if (s == "shm_read" || s == "shm_write")    return PLH_QUEUE_POLICY_SHM;
    if (s == "zmq_push_drop")                   return PLH_QUEUE_POLICY_ZMQ_DROP;
    if (s == "zmq_push_block")                  return PLH_QUEUE_POLICY_ZMQ_BLOCK;
    // "zmq_pull_ring_N" — the depth tail is variable per queue.
    if (s.rfind("zmq_pull_ring", 0) == 0)       return PLH_QUEUE_POLICY_ZMQ_RING;
    if (s == "shm_unconnected" || s == "zmq_unconnected")
        return PLH_QUEUE_POLICY_UNKNOWN;        // known placeholder; no warn.
    // Drift catch — fresh-eye audit B1, 2026-06-11.  A new policy
    // string added to hub_shm_queue.cpp / hub_zmq_queue.cpp without
    // updating this mapper silently degrades plugins to UNKNOWN.
    // Warn once per unique string so the gap surfaces in CI logs
    // without spamming the data path.
    static thread_local std::unordered_set<std::string> warned_unknowns;
    if (warned_unknowns.insert(s).second)
        LOGGER_WARN("native_engine: policy_string_to_macro encountered "
                    "unrecognized policy '{}'; mapping to "
                    "PLH_QUEUE_POLICY_UNKNOWN.  Update the mapper to "
                    "track the new policy.", s);
    return PLH_QUEUE_POLICY_UNKNOWN;
}

// ── Helper: snapshot the broker's band-member list for a channel ─────────────
//
// Factored out of ctx_band_members / _contains / _count so the three
// sister functions share one extraction path.  Returns the broker
// reply's `members[]` array on success, std::nullopt on transport
// error / not-a-member / unknown channel.
std::optional<nlohmann::json>
fetch_band_members(const PlhNativeContext *ctx, const char *channel) noexcept
{
    try
    {
        auto reply = static_cast<RoleAPIBase *>(ctx->_api)->band_members(channel);
        if (!reply.has_value()) return std::nullopt;
        // Broker reply shape: { "members": [ {role_uid, role_name}, ... ] }
        return reply->value("members", nlohmann::json::array());
    }
    catch (...) { return std::nullopt; }
}

// ── Band pub/sub (HEP-CORE-0030) — API v6 visitor + inquiry ─────────────────

int ctx_band_join(const PlhNativeContext *ctx, const char *channel)
{
    // API v6 (#194): plain int return; drops the v5 list-from-join
    // behaviour (which was a malloc'd JSON of the broker reply body).
    // Plugin calls ctx->band_members(...) separately to enumerate after
    // a successful join.  Status semantics match band_leave: 1 = success,
    // 0 = broker rejected (e.g. invalid band name), -1 = arg / transport
    // error.
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        auto result = static_cast<RoleAPIBase *>(ctx->_api)->band_join(channel);
        if (!result.has_value()) return -1;
        return (result->value("status", std::string{}) == "success") ? 1 : 0;
    }
    catch (...) { return -1; }
}

int ctx_band_leave(const PlhNativeContext *ctx, const char *channel)
{
    // API v6 — same shape as band_join.  See audit A1 (2026-05-20) note
    // in the v5 history for why we gate on status == "success" rather
    // than has_value(): broker may reply with a typed
    // `{status:error, NOT_A_MEMBER}` body which counts as a failed
    // leave, not a successful one.
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        auto result = static_cast<RoleAPIBase *>(ctx->_api)->band_leave(channel);
        if (!result.has_value()) return -1;
        return (result->value("status", std::string{}) == "success") ? 1 : 0;
    }
    catch (...) { return -1; }
}

void ctx_band_broadcast(const PlhNativeContext *ctx, const char *channel, const char *body_json)
{
    if (!ctx || !ctx->_api || !channel || !body_json) return;
    try
    {
        auto body = nlohmann::json::parse(body_json);
        static_cast<RoleAPIBase *>(ctx->_api)->band_broadcast(channel, body);
    }
    catch (const nlohmann::json::exception &e)
    {
        // Native script passed malformed JSON.  Silent drop hides a
        // real bug (script's body_json string is invalid); log at WARN
        // so the script developer sees the cause in the role log.
        LOGGER_WARN("native_engine: ctx_band_broadcast('{}') JSON parse "
                    "error: {}", channel, e.what());
    }
}

int ctx_band_members(const PlhNativeContext *ctx,
                     const char *channel,
                     plh_band_member_visitor visitor,
                     void *userdata)
{
    // API v6 visitor pattern.  Host snapshots band membership via a
    // broker REQ, then visits each member through the plugin's
    // callback.  Per HEP-CORE-0028 §4.8 the visitor MUST be noexcept;
    // we wrap the loop in a try/catch as defense-in-depth.
    if (!ctx || !ctx->_api || !channel || !visitor) return -1;
    auto arr_opt = fetch_band_members(ctx, channel);
    if (!arr_opt.has_value()) return -1;
    try
    {
        int count = 0;
        for (const auto &m : *arr_opt)
        {
            const std::string uid  = m.value("role_uid",  std::string{});
            const std::string name = m.value("role_name", std::string{});
            // Skip malformed entries — broker shouldn't send members
            // without a role_uid, but defending here keeps plugin
            // visitors from receiving "" as a real UID (fresh-eye
            // audit A1, 2026-06-11).
            if (uid.empty()) continue;
            const plh_band_member_t entry{
                uid.c_str(),
                name.empty() ? nullptr : name.c_str(),
            };
            visitor(&entry, userdata);
            ++count;
        }
        return count;
    }
    catch (...)
    {
        LOGGER_ERROR("native_engine: ctx_band_members('{}') visitor "
                     "threw across C ABI; aborting iteration", channel);
        return -1;
    }
}

int ctx_band_member_contains(const PlhNativeContext *ctx,
                             const char *channel,
                             const char *role_uid)
{
    // NOTE on cost: this fires a broker REQ per call.  Header doc
    // calls out the asymmetry vs the allowed_peers cluster (which
    // serves from a local push-cache).  For repeated queries in a hot
    // loop, plugin should snapshot once via ctx->band_members(...) and
    // query a local set.
    if (!ctx || !ctx->_api || !channel || !role_uid) return -1;
    auto arr_opt = fetch_band_members(ctx, channel);
    if (!arr_opt.has_value()) return -1;
    for (const auto &m : *arr_opt)
        if (m.value("role_uid", std::string{}) == role_uid) return 1;
    return 0;
}

int ctx_band_member_count(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    auto arr_opt = fetch_band_members(ctx, channel);
    if (!arr_opt.has_value()) return -1;
    // Match ctx_band_members semantics — skip malformed entries so
    // count agrees with what the visitor sees (audit A1).
    int count = 0;
    for (const auto &m : *arr_opt)
        if (!m.value("role_uid", std::string{}).empty()) ++count;
    return count;
}

// ── Channel-auth observability (HEP-CORE-0036 §I11 + §6.7) — API v6 ─────────

int ctx_allowed_peers(const PlhNativeContext *ctx,
                      const char *channel,
                      plh_allowed_peer_visitor visitor,
                      void *userdata)
{
    if (!ctx || !ctx->_api || !channel || !visitor) return -1;
    try
    {
        const auto peers =
            static_cast<RoleAPIBase *>(ctx->_api)->allowed_peers(channel);
        for (const auto &p : peers)
        {
            const plh_allowed_peer_t entry{
                p.role_uid.c_str(),
                p.pubkey.c_str(),
            };
            visitor(&entry, userdata);
        }
        return static_cast<int>(peers.size());
    }
    catch (...)
    {
        LOGGER_ERROR("native_engine: ctx_allowed_peers('{}') visitor "
                     "threw across C ABI; aborting iteration", channel);
        return -1;
    }
}

int ctx_allowed_peer_contains(const PlhNativeContext *ctx,
                              const char *channel,
                              const char *role_uid)
{
    if (!ctx || !ctx->_api || !channel || !role_uid) return -1;
    try
    {
        const auto peers =
            static_cast<RoleAPIBase *>(ctx->_api)->allowed_peers(channel);
        for (const auto &p : peers)
            if (p.role_uid == role_uid) return 1;
        return 0;
    }
    catch (...) { return -1; }
}

int ctx_allowed_peer_count(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        return static_cast<int>(
            static_cast<RoleAPIBase *>(ctx->_api)->allowed_peers(channel).size());
    }
    catch (...) { return -1; }
}

int ctx_is_channel_ready(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        return static_cast<RoleAPIBase *>(ctx->_api)->is_channel_ready(channel)
            ? 1 : 0;
    }
    catch (...) { return -1; }
}

int ctx_consumer_count(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        return static_cast<int>(
            static_cast<RoleAPIBase *>(ctx->_api)->consumer_count(channel));
    }
    catch (...) { return -1; }
}

int ctx_producer_count(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    try
    {
        return static_cast<int>(
            static_cast<RoleAPIBase *>(ctx->_api)->producer_count(channel));
    }
    catch (...) { return -1; }
}

/// HEP-CORE-0028 §6a.5 — visit every LIVE producer role_uid on
/// `channel`.  Symmetric with ctx_consumers below; both snapshot the
/// binding-side live_peers map under RoleAPIBase's internal lock.
int ctx_producers(const PlhNativeContext *ctx,
                  const char *channel,
                  plh_role_uid_visitor visitor,
                  void *userdata)
{
    if (!ctx || !ctx->_api || !channel || !visitor) return -1;
    try
    {
        const auto uids =
            static_cast<RoleAPIBase *>(ctx->_api)->producers(channel);
        for (const auto &u : uids)
            visitor(u.c_str(), userdata);
        return static_cast<int>(uids.size());
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("native_engine: ctx_producers('{}') threw: {}",
                     channel, e.what());
        return -1;
    }
    catch (...) { return -1; }
}

int ctx_consumers(const PlhNativeContext *ctx,
                  const char *channel,
                  plh_role_uid_visitor visitor,
                  void *userdata)
{
    if (!ctx || !ctx->_api || !channel || !visitor) return -1;
    try
    {
        const auto uids =
            static_cast<RoleAPIBase *>(ctx->_api)->consumers(channel);
        for (const auto &u : uids)
            visitor(u.c_str(), userdata);
        return static_cast<int>(uids.size());
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("native_engine: ctx_consumers('{}') threw: {}",
                     channel, e.what());
        return -1;
    }
    catch (...) { return -1; }
}

int ctx_queue_mechanism(const PlhNativeContext *ctx, int side)
{
    // API v6: returns one of PLH_MECHANISM_* macros (UNINITIALIZED=0,
    // CURVE=1).  hub::Mechanism enum values are co-numbered with the
    // macros (HEP-CORE-0035 §2), so the int cast is identity.
    if (!ctx || !ctx->_api) return PLH_MECHANISM_UNINITIALIZED;
    const ChannelSide cs =
        (side == PLH_SIDE_TX) ? ChannelSide::Tx : ChannelSide::Rx;
    try
    {
        return static_cast<int>(
            static_cast<RoleAPIBase *>(ctx->_api)->queue_mechanism(cs));
    }
    catch (...) { return PLH_MECHANISM_UNINITIALIZED; }
}

// ── Queue diagnostic surface — capacity + policy + last_seq ─────────────────

uint64_t ctx_out_capacity(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return 0;
    try { return static_cast<uint64_t>(static_cast<RoleAPIBase *>(ctx->_api)->out_capacity()); }
    catch (...) { return 0; }
}

uint64_t ctx_in_capacity(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return 0;
    try { return static_cast<uint64_t>(static_cast<RoleAPIBase *>(ctx->_api)->in_capacity()); }
    catch (...) { return 0; }
}

int ctx_out_policy(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return PLH_QUEUE_POLICY_UNKNOWN;
    try { return policy_string_to_macro(
            static_cast<RoleAPIBase *>(ctx->_api)->out_policy()); }
    catch (...) { return PLH_QUEUE_POLICY_UNKNOWN; }
}

int ctx_in_policy(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return PLH_QUEUE_POLICY_UNKNOWN;
    try { return policy_string_to_macro(
            static_cast<RoleAPIBase *>(ctx->_api)->in_policy()); }
    catch (...) { return PLH_QUEUE_POLICY_UNKNOWN; }
}

uint64_t ctx_last_seq(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return 0;
    try { return static_cast<RoleAPIBase *>(ctx->_api)->last_seq(); }
    catch (...) { return 0; }
}

// ── Metrics snapshot (API v6 #194) ──────────────────────────────────────────
//
// Two-call surface: metrics_snapshot() builds a thread-local
// dotted-key→double map from RoleAPIBase::snapshot_metrics_json() and
// returns the map pointer as an opaque view; metrics_get() looks up
// keys against it.  The cache is rebuilt on every snapshot() call —
// callers that need many lookups should snapshot once, then query
// repeatedly against the same opaque pointer.

void flatten_json_into(const nlohmann::json &node,
                       const std::string &prefix,
                       std::unordered_map<std::string, double> &out)
{
    if (node.is_object())
    {
        for (const auto &[k, v] : node.items())
        {
            const std::string dotted = prefix.empty() ? k : prefix + "." + k;
            flatten_json_into(v, dotted, out);
        }
    }
    else if (node.is_number())
    {
        out.emplace(prefix, node.get<double>());
    }
    else if (node.is_boolean())
    {
        out.emplace(prefix, node.get<bool>() ? 1.0 : 0.0);
    }
    // Strings / arrays / null are not numeric metrics; skip silently.
}

// Thread-local metrics snapshot cache (#194).
//
// LIFETIME CONTRACT (HEP-CORE-0028 §5.4 + audit A3):
//   - The opaque pointer returned by metrics_snapshot() is the address
//     of a thread-local static.
//   - "Valid until the next snapshot on the same thread."
//   - A plugin that stashes the pointer across finalize_engine_() and
//     a subsequent reload on the same thread will alias the new
//     plugin's cache — but only the metrics tree for the SAME thread
//     and only via the shared `&cache` address.  We do not (cannot,
//     cheaply) prevent this; plugins MUST drop snapshot pointers in
//     their on_stop().
//   - As a guard-rail: the cache is cleared at the start of every
//     snapshot call, so stale pointers retained briefly between calls
//     return 0 for every key (not stale data from a prior tree).
const void *ctx_metrics_snapshot(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return nullptr;
    static thread_local std::unordered_map<std::string, double> cache;
    try
    {
        cache.clear();
        const auto j =
            static_cast<RoleAPIBase *>(ctx->_api)->snapshot_metrics_json();
        flatten_json_into(j, std::string{}, cache);
        return &cache;
    }
    catch (...) { return nullptr; }
}

int ctx_metrics_get(const void *snapshot, const char *key, double *out)
{
    if (!snapshot || !key || !out) return -1;
    try
    {
        const auto *cache =
            static_cast<const std::unordered_map<std::string, double> *>(snapshot);
        auto it = cache->find(key);
        if (it == cache->end()) return 0;
        *out = it->second;
        return 1;
    }
    catch (...) { return -1; }
}

// ── Flexzone control + band-membership query ────────────────────────────────

int ctx_is_in_band(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return -1;
    try { return static_cast<RoleAPIBase *>(ctx->_api)->is_in_band(channel) ? 1 : 0; }
    catch (...) { return -1; }
}

int ctx_update_flexzone_checksum(const PlhNativeContext *ctx)
{
    if (!ctx || !ctx->_api) return 0;
    try { return static_cast<RoleAPIBase *>(ctx->_api)->update_flexzone_checksum() ? 1 : 0; }
    catch (...) { return 0; }
}

void ctx_set_verify_checksum(const PlhNativeContext *ctx, int enable)
{
    if (!ctx || !ctx->_api) return;
    try { static_cast<RoleAPIBase *>(ctx->_api)->set_verify_checksum(enable != 0); }
    catch (...) { /* defensive: nothing observable to do */ }
}

} // anonymous namespace

// ============================================================================
// NativeContextStorage — owns the C-linkage context strings
// ============================================================================

struct NativeEngine::NativeContextStorage
{
    PlhNativeContext ctx{};

    // Storage for strings (PlhNativeContext holds pointers into these).
    std::string short_tag;
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string role_dir;
    std::string log_label;  ///< e.g. "[native libfoo.so]"

    // Flexzone — cached once at wire(), stable for SHM lifetime. C/C++
    // plugins access fz via plh_tx_t.fz / plh_rx_t.fz (populated from
    // here by the bridge per invoke — zero-cost stack copy).
    void  *tx_fz{nullptr};
    size_t tx_fz_sz{0};
    void  *rx_fz{nullptr};
    size_t rx_fz_sz{0};

    /// Wire the hub-side context (audit B13, 2026-05-21; #84 expansion
    /// 2026-06-11).  Every ctx fn ptr is assigned — either to a real
    /// hub-applicable impl (delegating through HubAPI) or to a noop
    /// stub returning the documented sentinel.  Pure-C hub plugins
    /// can call ANY ctx fn without null-checking; the result for
    /// role-only methods (band, allowed_peers, spinlock, queue_*,
    /// slot_*, etc.) is the same "absent / unavailable" sentinel
    /// they'd see if the underlying queue/state were unready.
    /// See HEP-CORE-0028 §4.9 for the full hub-vs-role matrix.
    void wire_hub(hub_host::HubAPI *api)
    {
        assert(api != nullptr && "HubAPI must not be null");

        ctx.short_tag    = "hub";
        ctx.uid         = uid.c_str();
        ctx.name        = name.c_str();
        ctx.channel     = nullptr;
        ctx.out_channel = nullptr;
        ctx.log_level   = log_level.c_str();
        ctx.role_dir    = role_dir.c_str();

        ctx._magic     = PLH_CONTEXT_MAGIC;
        ctx._magic_end = PLH_CONTEXT_MAGIC;
        ctx._core      = nullptr;          // no RoleHostCore on hub side
        ctx._api       = api;              // HubAPI*
        ctx._log_label = log_label.c_str();

        // ── Engine-agnostic + hub-applicable role-side methods ────────
        ctx.log                  = ctx_log;
        ctx.report_metric        = hub_stub_report_metric;
        ctx.clear_custom_metrics = hub_stub_void_role_state;
        ctx.request_stop         = ctx_request_stop_hub;
        ctx.set_critical_error   = hub_stub_set_critical_error;
        ctx.is_critical_error    = hub_stub_is_critical_error;
        ctx.stop_reason          = hub_stub_stop_reason;

        // ── Counters — all stubbed to 0 (no hub equivalent) ───────────
        ctx.out_slots_written  = hub_stub_counter_zero;
        ctx.in_slots_received  = hub_stub_counter_zero;
        ctx.out_drop_count     = hub_stub_counter_zero;
        ctx.script_error_count = hub_stub_counter_zero;
        ctx.loop_overrun_count = hub_stub_counter_zero;
        ctx.last_cycle_work_us = hub_stub_counter_zero;

        // ── Spinlocks — all stubbed (no SHM on hub) ───────────────────
        ctx.spinlock_lock      = hub_stub_spinlock_lock;
        ctx.spinlock_unlock    = hub_stub_spinlock_unlock;
        ctx.spinlock_count     = hub_stub_spinlock_count;
        ctx.spinlock_is_locked = hub_stub_spinlock_is_locked;

        // ── Schema sizes — stubbed (no slots/flexzone on hub) ─────────
        ctx.slot_logical_size     = hub_stub_size_arg2_zero;
        ctx.flexzone_logical_size = hub_stub_size_arg2_zero;

        // ── Role discovery — stubbed (hub has list_roles instead) ─────
        ctx.wait_for_role = hub_stub_wait_for_role;

        // ── Band pub/sub — all stubbed (no role registration on hub) ──
        ctx.band_join            = hub_stub_band_int_arg2;
        ctx.band_leave           = hub_stub_band_int_arg2;
        ctx.band_broadcast       = hub_stub_band_broadcast;
        ctx.band_members         = hub_stub_band_members;
        ctx.band_member_contains = hub_stub_band_member_contains;
        ctx.band_member_count    = hub_stub_band_int_arg2;

        // ── Channel-auth observability — all stubbed (role concept) ───
        ctx.allowed_peers         = hub_stub_allowed_peers;
        ctx.allowed_peer_contains = hub_stub_allowed_peer_contains;
        ctx.allowed_peer_count    = hub_stub_band_int_arg2;
        ctx.consumer_count        = hub_stub_band_int_arg2;
        ctx.producer_count        = hub_stub_band_int_arg2;
        ctx.consumers             = hub_stub_role_uid_visit;
        ctx.producers             = hub_stub_role_uid_visit;
        ctx.is_channel_ready      = hub_stub_is_channel_ready;
        ctx.queue_mechanism       = hub_stub_queue_mechanism;

        // ── Queue diagnostics — all stubbed (no role queue on hub) ────
        ctx.out_capacity = hub_stub_counter_zero;
        ctx.in_capacity  = hub_stub_counter_zero;
        ctx.out_policy   = hub_stub_queue_policy;
        ctx.in_policy    = hub_stub_queue_policy;
        ctx.last_seq     = hub_stub_counter_zero;

        // ── Metrics snapshot — stubbed (hub uses hub_metrics_json) ────
        ctx.metrics_snapshot = hub_stub_metrics_snapshot;
        ctx.metrics_get      = hub_stub_metrics_get;

        // ── Flexzone control + band-membership query — stubbed ────────
        ctx.is_in_band               = hub_stub_is_in_band;
        ctx.update_flexzone_checksum = hub_stub_update_flexzone_checksum;
        ctx.set_verify_checksum      = hub_stub_set_verify_checksum;

        // ── Hub-side surface (API v7) — real impls via HubAPI ─────────
        ctx.hub_metrics_json        = ctx_hub_metrics_json;
        ctx.hub_config_json         = ctx_hub_config_json;
        ctx.hub_query_metrics_json  = ctx_hub_query_metrics_json;
        ctx.hub_list_channels_json  = ctx_hub_list_channels_json;
        ctx.hub_get_channel_json    = ctx_hub_get_channel_json;
        ctx.hub_list_roles_json     = ctx_hub_list_roles_json;
        ctx.hub_get_role_json       = ctx_hub_get_role_json;
        ctx.hub_list_bands_json     = ctx_hub_list_bands_json;
        ctx.hub_get_band_json       = ctx_hub_get_band_json;
        ctx.hub_list_peers_json     = ctx_hub_list_peers_json;
        ctx.hub_get_peer_json       = ctx_hub_get_peer_json;
        ctx.hub_close_channel       = ctx_hub_close_channel;
        ctx.hub_broadcast_channel   = ctx_hub_broadcast_channel;
        ctx.hub_post_event          = ctx_hub_post_event;
        ctx.hub_augment_timeout_ms  = ctx_hub_augment_timeout_ms;
        ctx.hub_set_augment_timeout = ctx_hub_set_augment_timeout;
    }

    void wire(RoleHostCore *core, RoleAPIBase *api)
    {
        assert(core != nullptr && "RoleHostCore must not be null");
        assert(api != nullptr && "RoleAPIBase must not be null");

        // Identity strings.
        ctx.short_tag    = short_tag.c_str();
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

        // Flexzone — cached on storage, populated once from api.
        tx_fz    = api->flexzone(ChannelSide::Tx);
        tx_fz_sz = api->flexzone_size(ChannelSide::Tx);
        rx_fz    = api->flexzone(ChannelSide::Rx);
        rx_fz_sz = api->flexzone_size(ChannelSide::Rx);

        ctx.wait_for_role  = ctx_wait_for_role;

        // Band pub/sub (HEP-CORE-0030) — API v6 visitor + inquiry.
        ctx.band_join             = ctx_band_join;
        ctx.band_leave            = ctx_band_leave;
        ctx.band_broadcast        = ctx_band_broadcast;
        ctx.band_members          = ctx_band_members;
        ctx.band_member_contains  = ctx_band_member_contains;
        ctx.band_member_count     = ctx_band_member_count;

        // Channel-auth observability (HEP-CORE-0036 §I11 + §6.7) — API v6.
        ctx.allowed_peers          = ctx_allowed_peers;
        ctx.allowed_peer_contains  = ctx_allowed_peer_contains;
        ctx.allowed_peer_count     = ctx_allowed_peer_count;
        ctx.consumer_count         = ctx_consumer_count;
        ctx.producer_count         = ctx_producer_count;
        ctx.consumers              = ctx_consumers;
        ctx.producers              = ctx_producers;
        ctx.is_channel_ready       = ctx_is_channel_ready;
        ctx.queue_mechanism        = ctx_queue_mechanism;

        // Queue diagnostic surface — capacity + policy + last_seq.
        ctx.out_capacity = ctx_out_capacity;
        ctx.in_capacity  = ctx_in_capacity;
        ctx.out_policy   = ctx_out_policy;
        ctx.in_policy    = ctx_in_policy;
        ctx.last_seq     = ctx_last_seq;

        // Metrics snapshot (#194) — replaces v5 metrics_json.
        ctx.metrics_snapshot         = ctx_metrics_snapshot;
        ctx.metrics_get              = ctx_metrics_get;

        // Flexzone control + band-membership query.
        ctx.is_in_band               = ctx_is_in_band;
        ctx.update_flexzone_checksum = ctx_update_flexzone_checksum;
        ctx.set_verify_checksum      = ctx_set_verify_checksum;

        // ── Hub-side fn ptrs — role-side noop stubs (API v7) ─────────
        // A role-side ctx has no HubAPI*.  Plugins that mistakenly call
        // hub_* get a documented empty/sentinel return rather than UB.
        ctx.hub_metrics_json        = role_stub_hub_json_no_arg;
        ctx.hub_config_json         = role_stub_hub_json_no_arg;
        ctx.hub_query_metrics_json  = role_stub_hub_json_query;
        ctx.hub_list_channels_json  = role_stub_hub_json_no_arg;
        ctx.hub_get_channel_json    = role_stub_hub_json_query_arg;
        ctx.hub_list_roles_json     = role_stub_hub_json_no_arg;
        ctx.hub_get_role_json       = role_stub_hub_json_query_arg;
        ctx.hub_list_bands_json     = role_stub_hub_json_no_arg;
        ctx.hub_get_band_json       = role_stub_hub_json_query_arg;
        ctx.hub_list_peers_json     = role_stub_hub_json_no_arg;
        ctx.hub_get_peer_json       = role_stub_hub_json_query_arg;
        ctx.hub_close_channel       = role_stub_hub_close_channel;
        ctx.hub_broadcast_channel   = role_stub_hub_broadcast_channel;
        ctx.hub_post_event          = role_stub_hub_post_event;
        ctx.hub_augment_timeout_ms  = role_stub_hub_augment_timeout_ms;
        ctx.hub_set_augment_timeout = role_stub_hub_set_augment_timeout;
    }
};

// ============================================================================
// Constructor / Destructor (in .cpp for Pimpl completeness)
// ============================================================================

NativeEngine::NativeEngine() = default;

NativeEngine::~NativeEngine()
{
    // Match the ordering of finalize_engine_() so abnormal exit paths
    // (exception in build_api_, unhandled in invoke_*, etc.) tear down
    // in the same order as the normal shutdown path:
    //   1. native_finalize() — plugin gets one last call
    //   2. release the host-owned PlhNativeContext + storage strings
    //   3. dlclose the shared library
    // dl_handle_ is the gate for ALL plugin-pointing operations:
    // load_script() nulls dl_handle_ on early-error paths (missing
    // symbol, ABI mismatch, missing required callback) WITHOUT
    // clearing fn_finalize_.  Calling fn_finalize_ at that point
    // is a UAF — the symbol pointed into already-unmapped memory.
    // Guard on dl_handle_, not fn_finalize_.
    // Fresh-eye audit A4, 2026-06-11.
    if (dl_handle_)
    {
        if (fn_finalize_)
            fn_finalize_();
        native_ctx_.reset();
        DL_CLOSE(dl_handle_);
        dl_handle_ = nullptr;
    }
    else
    {
        // Either never loaded, or load_script() failed after a partial
        // dlopen.  native_ctx_ is at most a freshly-allocated storage
        // (no fn_init_ ran), safe to release.
        native_ctx_.reset();
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
    fn_on_init_       = reinterpret_cast<FnVoidNoArgs>(resolve_sym_("on_init"));
    fn_on_stop_       = reinterpret_cast<FnVoidNoArgs>(resolve_sym_("on_stop"));
    fn_on_channel_closing_ =
        reinterpret_cast<FnOnChannelClosing>(resolve_sym_("on_channel_closing"));
    fn_on_consumer_died_ =
        reinterpret_cast<FnOnConsumerDied>(resolve_sym_("on_consumer_died"));
    fn_on_hub_dead_ =
        reinterpret_cast<FnOnHubDead>(resolve_sym_("on_hub_dead"));
    // S4 expansion 2026-05-19 — typed band callbacks.
    fn_on_band_member_joined_ =
        reinterpret_cast<FnOnBandMemberJoined>(resolve_sym_("on_band_member_joined"));
    fn_on_band_member_left_ =
        reinterpret_cast<FnOnBandMemberLeft>(resolve_sym_("on_band_member_left"));
    fn_on_band_message_ =
        reinterpret_cast<FnOnBandMessage>(resolve_sym_("on_band_message"));
    fn_on_band_lost_ =
        reinterpret_cast<FnOnBandLost>(resolve_sym_("on_band_lost"));
    // HEP-CORE-0036 §I11 + §6.5 — producer-side event-driven allowlist
    // refresh.  Same shape as the band typed callbacks above.
    fn_on_allowlist_changed_ =
        reinterpret_cast<FnOnAllowlistChanged>(resolve_sym_("on_allowlist_changed"));
    fn_on_produce_    = reinterpret_cast<FnOnProduce>(resolve_sym_("on_produce"));
    fn_on_consume_    = reinterpret_cast<FnOnConsume>(resolve_sym_("on_consume"));
    fn_on_process_    = reinterpret_cast<FnOnProcess>(resolve_sym_("on_process"));
    fn_on_inbox_      = reinterpret_cast<FnOnInbox>(resolve_sym_("on_inbox"));
    fn_on_heartbeat_  = reinterpret_cast<FnVoidNoArgs>(resolve_sym_("on_heartbeat"));
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
    native_ctx_->short_tag    = api.short_tag();
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
        // NativeEngine's schema-hash path calls
        // `secure().compute_blake2b_array(...)`, so it needs SMS up
        // before its startup runs (HEP-CORE-0043 §2.1 category 1).
        mod.add_dependency("SecureSubsystem");
        mod.set_startup([](const char *, void *) {}, ""); // no-op
        mod.set_shutdown(
            [](const char *, void *) {},
            std::chrono::milliseconds{5000});
        if (pylabhub::utils::RegisterDynamicModule(std::move(mod)))
        {
            // Track the LOAD outcome, not the REGISTER outcome — calling
            // UnloadModule() later on a never-loaded module is a no-op at
            // best, a logged confusion at worst.  Fresh-eye audit A5,
            // 2026-06-11.
            lifecycle_registered_ =
                pylabhub::utils::LoadModule(lifecycle_module_name_.c_str());
            if (!lifecycle_registered_)
                LOGGER_WARN("[{}] lifecycle LoadModule({}) returned false; "
                            "not setting registered flag",
                            log_tag_, lifecycle_module_name_);
            else
                LOGGER_DEBUG("[{}] lifecycle module registered: {}",
                             log_tag_, lifecycle_module_name_);
        }
    }

    return true;
}

// ============================================================================
// build_api_(HubAPI&) — hub-side surface (audit B13, 2026-05-21)
// ============================================================================
//
// Minimum-viable hub-side native engine support.  Allocates a
// PlhNativeContext with the hub-flavoured wiring (log + identity +
// request_stop; role-side function pointers stay nullptr), then
// invokes the plugin's native_init.  No HubAPI metrics or
// list/get/query accessors exposed yet — those can be added later
// as `ctx_*_hub` adapters with the same null-check-and-skip pattern
// already used for role-side counters.
//
// Lifecycle: same as role-side — `native_init` here, `native_finalize`
// in `finalize_engine_()`, lifecycle module registered once for the
// timeout-guarded shutdown.

bool NativeEngine::build_api_(hub_host::HubAPI &api)
{
    native_ctx_ = std::make_unique<NativeContextStorage>();
    native_ctx_->uid        = api.uid();
    native_ctx_->name       = api.name();
    native_ctx_->log_level  = "info";   // hub doesn't expose per-script level today
    native_ctx_->log_label  = "[native hub " + lib_path_.filename().string() + "]";
    native_ctx_->role_dir   = "";       // hub has no per-role dir
    native_ctx_->wire_hub(&api);

    if (!fn_init_(&native_ctx_->ctx))
    {
        LOGGER_ERROR("[{}] native_init returned false (hub-side build_api)",
                     log_tag_);
        return false;
    }

    // Register lifecycle module — same shape as role-side.
    lifecycle_module_name_ = "NativeEngine:" + lib_path_.filename().string();
    {
        pylabhub::utils::ModuleDef mod(lifecycle_module_name_.c_str());
        mod.add_dependency("pylabhub::utils::Logger");
        // NativeEngine's schema-hash path calls
        // `secure().compute_blake2b_array(...)`, so it needs SMS up
        // before its startup runs (HEP-CORE-0043 §2.1 category 1).
        mod.add_dependency("SecureSubsystem");
        mod.set_startup([](const char *, void *) {}, "");
        mod.set_shutdown(
            [](const char *, void *) {},
            std::chrono::milliseconds{5000});
        if (pylabhub::utils::RegisterDynamicModule(std::move(mod)))
        {
            // Audit A5: same fix as the role-side branch above.
            lifecycle_registered_ =
                pylabhub::utils::LoadModule(lifecycle_module_name_.c_str());
            if (!lifecycle_registered_)
                LOGGER_WARN("[{}] hub lifecycle LoadModule({}) returned false; "
                            "not setting registered flag",
                            log_tag_, lifecycle_module_name_);
            else
                LOGGER_DEBUG("[{}] lifecycle module registered (hub): {}",
                             log_tag_, lifecycle_module_name_);
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
    fn_on_channel_closing_ = nullptr;
    fn_on_consumer_died_ = nullptr;
    fn_on_hub_dead_ = nullptr;
    fn_on_band_member_joined_ = nullptr;
    fn_on_band_member_left_ = nullptr;
    fn_on_band_message_ = nullptr;
    fn_on_band_lost_ = nullptr;
    fn_on_allowlist_changed_ = nullptr;
    fn_on_produce_ = nullptr;
    fn_on_consume_ = nullptr;
    fn_on_process_ = nullptr;
    fn_on_inbox_ = nullptr;
    fn_on_heartbeat_ = nullptr;
    fn_is_thread_safe_ = nullptr;

    // Unregister from lifecycle (if registered).
    if (lifecycle_registered_)
    {
        if (!pylabhub::utils::UnloadModule(lifecycle_module_name_.c_str()))
            LOGGER_WARN("[{}] lifecycle UnloadModule({}) returned false",
                        log_tag_, lifecycle_module_name_);
        lifecycle_registered_ = false;
    }
}

// ============================================================================
// has_callback
// ============================================================================

bool NativeEngine::has_callback(const std::string &name) const noexcept
{
    if (name == "on_init")             return fn_on_init_             != nullptr;
    if (name == "on_stop")             return fn_on_stop_             != nullptr;
    if (name == "on_channel_closing")  return fn_on_channel_closing_  != nullptr;
    if (name == "on_consumer_died")    return fn_on_consumer_died_    != nullptr;
    if (name == "on_hub_dead")             return fn_on_hub_dead_             != nullptr;
    if (name == "on_band_member_joined")   return fn_on_band_member_joined_   != nullptr;
    if (name == "on_band_member_left")     return fn_on_band_member_left_     != nullptr;
    if (name == "on_band_message")         return fn_on_band_message_         != nullptr;
    if (name == "on_band_lost")            return fn_on_band_lost_            != nullptr;
    if (name == "on_allowlist_changed")    return fn_on_allowlist_changed_    != nullptr;
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
    // Canonical-name contract (see script_engine.hpp and HEP-0011
    // § "Canonical type names").  NativeEngine historically accepted
    // any name that resolved a `native_sizeof_<NAME>` export, but no
    // production path ever used non-canonical names — the role host
    // only ever calls register_slot_type with the five canonical
    // frame names.  Enforcing the contract here keeps the three
    // engines behaviorally identical at the API surface.
    //
    // Storage design note: type_sizes_ (std::unordered_map<std::string,
    // size_t>) is kept as-is.  Native stores only sizes (plugins own
    // the actual C/C++ struct definitions), so a map is the natural
    // storage shape — simpler than Lua/Python's role-specific
    // readonly-vs-writable py::object / ref slots.  The map also
    // leaves room for future protocol extensions that might introduce
    // new canonical names; adding one is a one-line edit to the
    // validator below.
    if (type_name != "InSlotFrame"
        && type_name != "OutSlotFrame"
        && type_name != "InFlexFrame"
        && type_name != "OutFlexFrame"
        && type_name != "InboxFrame")
    {
        LOGGER_ERROR("[{}] register_slot_type: unknown canonical type_name "
                     "'{}' — must be one of InSlotFrame, OutSlotFrame, "
                     "InFlexFrame, OutFlexFrame, InboxFrame",
                     log_tag_, type_name);
        return false;
    }

    if (!spec.has_schema)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') called with has_schema=false",
                     log_tag_, type_name);
        return false;
    }

    // Compute expected size from schema (infrastructure-authoritative).
    size_t expected_size = hub::compute_schema_size(spec, packing);

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
        fn_on_init_();
}

void NativeEngine::invoke_on_stop()
{
    if (fn_on_stop_)
        fn_on_stop_();
}

void NativeEngine::invoke_on_channel_closing(const std::string &channel,
                                              const std::string &reason)
{
    if (!fn_on_channel_closing_) return;
    // Lifetime contract (see native_invoke_types.h): the args struct
    // and its `const char *` fields are valid only for the duration
    // of this call.  The backing `std::string`s outlive the callee.
    const plh_channel_closing_args_t args{channel.c_str(), reason.c_str()};
    fn_on_channel_closing_(&args);
}

void NativeEngine::invoke_on_consumer_died(const std::string &channel,
                                            const std::string &consumer_uid,
                                            const std::string &reason)
{
    if (!fn_on_consumer_died_) return;
    // Lifetime contract (see native_invoke_types.h): args is valid
    // only for this call; plugin MUST NOT retain pointers.
    const plh_consumer_died_args_t args{channel.c_str(),
                                        consumer_uid.c_str(),
                                        reason.c_str()};
    fn_on_consumer_died_(&args);
}

void NativeEngine::invoke_on_hub_dead(const std::string &source_hub_uid)
{
    if (!fn_on_hub_dead_) return;
    // Lifetime contract (see native_invoke_types.h): args is valid
    // only for this call; plugin MUST NOT retain pointers.
    const plh_hub_dead_args_t args{source_hub_uid.c_str()};
    fn_on_hub_dead_(&args);
}

// ============================================================================
// S4 expansion 2026-05-19 — typed band callbacks (HEP-CORE-0030 §5.3).
// Same lifetime contract: args struct + char* fields valid only for
// the duration of the call.  Body for `on_band_message` is passed
// as a JSON string (plugin parses).
// ============================================================================

void NativeEngine::invoke_on_band_member_joined(const std::string &band,
                                                const std::string &role_uid,
                                                const std::string &role_name)
{
    if (!fn_on_band_member_joined_) return;
    const plh_band_member_joined_args_t args{band.c_str(),
                                             role_uid.c_str(),
                                             role_name.c_str()};
    fn_on_band_member_joined_(&args);
}

void NativeEngine::invoke_on_band_member_left(const std::string &band,
                                              const std::string &role_uid,
                                              const std::string &reason)
{
    if (!fn_on_band_member_left_) return;
    const plh_band_member_left_args_t args{band.c_str(),
                                           role_uid.c_str(),
                                           reason.c_str()};
    fn_on_band_member_left_(&args);
}

void NativeEngine::invoke_on_band_message(const std::string  &band,
                                          const std::string  &sender_role_uid,
                                          const nlohmann::json &body)
{
    if (!fn_on_band_message_) return;
    // Serialize body to JSON string for the C ABI.  Plugin parses
    // (the framework deliberately doesn't ship a JSON parser into
    // the C ABI; plugins use whatever they like).
    const std::string body_str = body.dump();
    const plh_band_message_args_t args{band.c_str(),
                                       sender_role_uid.c_str(),
                                       body_str.c_str()};
    fn_on_band_message_(&args);
}

void NativeEngine::invoke_on_band_lost(const std::string &band,
                                       const std::string &reason)
{
    if (!fn_on_band_lost_) return;
    const plh_band_lost_args_t args{band.c_str(), reason.c_str()};
    fn_on_band_lost_(&args);
}

void NativeEngine::invoke_on_allowlist_changed(
    const std::string &channel,
    const std::vector<AllowedPeer> &allowlist,
    const std::string &reason)
{
    if (!fn_on_allowlist_changed_) return;

    // HEP-CORE-0036 §I11 + §6.5.  Build a transient C ABI args struct
    // + peer array on the stack — same lifetime contract as
    // `invoke_on_band_message` above: pointers valid for the duration
    // of this call only; plugin MUST NOT retain past return; strdup
    // if a copy is needed.
    std::vector<plh_allowed_peer_t> peers_c;
    peers_c.reserve(allowlist.size());
    for (const auto &p : allowlist)
        peers_c.push_back(plh_allowed_peer_t{p.role_uid.c_str(),
                                             p.pubkey.c_str()});

    const plh_allowlist_changed_args_t args{
        channel.c_str(),
        peers_c.empty() ? nullptr : peers_c.data(),
        peers_c.size(),
        reason.c_str()};
    fn_on_allowlist_changed_(&args);
}

InvokeResult NativeEngine::invoke_produce(
    InvokeTx tx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    // Missing callback — structurally unreachable for required
    // callbacks (load_script's required_callback check would have
    // failed earlier).  For optional callbacks, the caller is
    // expected to gate on has_callback (see RoleAPIBase::
    // drain_inbox_sync for on_inbox).  If reached anyway, it's a
    // dispatch bug: log ERROR and return Error to surface loudly.
    // Matches Lua/Python.
    if (!fn_on_produce_)
    {
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_produce called but on_produce is not "
                         "registered — dispatch bug",
                         log_tag_);
        }
        return InvokeResult::Error;
    }
    assert(native_ctx_ && "invoke_produce called without build_api");
    plh_tx_t c_tx{tx.slot, tx.slot_size, native_ctx_->tx_fz, native_ctx_->tx_fz_sz};
    return fn_on_produce_(&c_tx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_consume(
    InvokeRx rx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_consume_)
    {
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_consume called but on_consume is not "
                         "registered — dispatch bug",
                         log_tag_);
        }
        return InvokeResult::Error;
    }
    assert(native_ctx_ && "invoke_consume called without build_api");
    plh_rx_t c_rx{rx.slot, rx.slot_size, native_ctx_->rx_fz, native_ctx_->rx_fz_sz};
    return fn_on_consume_(&c_rx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_process(
    InvokeRx rx, InvokeTx tx,
    std::vector<IncomingMessage> & /*msgs*/)
{
    if (!fn_on_process_)
    {
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_process called but on_process is not "
                         "registered — dispatch bug",
                         log_tag_);
        }
        return InvokeResult::Error;
    }
    assert(native_ctx_ && "invoke_process called without build_api");
    plh_rx_t c_rx{rx.slot, rx.slot_size, native_ctx_->rx_fz, native_ctx_->rx_fz_sz};
    plh_tx_t c_tx{tx.slot, tx.slot_size, native_ctx_->tx_fz, native_ctx_->tx_fz_sz};
    return fn_on_process_(&c_rx, &c_tx) ? InvokeResult::Commit : InvokeResult::Discard;
}

InvokeResult NativeEngine::invoke_on_inbox(
    InvokeInbox msg)
{
    if (!fn_on_inbox_)
    {
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_on_inbox called but on_inbox is not "
                         "registered — caller should gate on has_callback",
                         log_tag_);
        }
        return InvokeResult::Error;
    }
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
        fn_on_heartbeat_();
        return true;
    }
    // Generic invoke: ad-hoc plugin symbol with the FnVoid (JSON-string)
    // ABI.  This is the contract for any plugin-defined callback NOT
    // on the standard lifecycle list — plugin author opts in by
    // writing `void symbol(const char *args_json)`.  Standard lifecycle
    // callbacks (on_init/on_stop/on_channel_closing/on_consumer_died/
    // on_heartbeat) are NOT reached via this fallback — they're cached
    // at load_script with their typed signatures and special-cased above.
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
        // on_heartbeat takes no args (uniform no-args lifecycle shape);
        // args are dropped silently if the caller supplies any.  Plugin
        // authors wanting heartbeat-with-args must use a different
        // symbol via the generic invoke path below.
        fn_on_heartbeat_();
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

InvokeResponse NativeEngine::invoke_returning(const std::string & /*name*/,
                                               const nlohmann::json & /*args*/,
                                               int64_t /*timeout_ms*/)
{
    // Native engines (HEP-CORE-0028 compiled plugins) don't expose a
    // generic named-callback return-value capture surface — their
    // script-side contract is the typed cycle ops (invoke_produce /
    // _consume / _process / _on_inbox).  Returning NotFound makes
    // HubAPI::augment_* a no-op for native-engine hubs (no script-
    // side augmentation), which is the right behaviour given the
    // absent surface.
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
        LOGGER_WARN("[{}] native engine does not export native_abi_info — skipping ABI check", log_tag_);
        return true; // permissive: allow if not exported
    }

    const PlhAbiInfo *info = fn();
    if (!info)
    {
        LOGGER_ERROR("[{}] native_abi_info returned null", log_tag_);
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

    // ── HEP-CORE-0032 extensions ─────────────────────────────────────
    // Fields below were added 2026-04-22.  A plugin compiled against
    // the older PlhAbiInfo reports a smaller struct_size and we
    // silently skip these checks (backward-compat).  A plugin
    // compiled against the new header populates them via the
    // PLH_COMPONENT_* mirrors; we compare against the host's current
    // ComponentVersions.
    //
    // Policy:
    // - ComponentVersions axis mismatches are INFORMATIONAL only
    //   (plugins don't directly cross those surfaces — they use
    //   PlhNativeContext callbacks, not SharedMemoryHeader or the
    //   broker protocol).  Log WARN, don't reject.
    // - build_id mismatch is fatal under PYLABHUB_STRICT_ABI_CHECK or
    //   Debug, matching the main binary's strict mode.  The
    //   motivating bug class (stale-plugin-vs-fresh-library at same
    //   declared axis values) is exactly what build_id catches.
    constexpr size_t kAbiInfoV3Size = sizeof(PlhAbiInfo);
    if (info->struct_size >= kAbiInfoV3Size)
    {
        const auto host = pylabhub::version::current();
        auto warn_if_diff = [this](const char *name, unsigned plugin_v,
                                    unsigned host_v)
        {
            if (plugin_v != host_v)
                LOGGER_WARN("[{}] native engine compiled against different {} "
                            "({} vs host {}) — proceeding (fingerprint only; "
                            "plugin does not cross this surface)",
                            log_tag_, name, plugin_v, host_v);
        };
        warn_if_diff("shm_major",           info->shm_major,           host.shm_major);
        warn_if_diff("shm_minor",           info->shm_minor,           host.shm_minor);
        warn_if_diff("broker_proto_major",  info->broker_proto_major,  host.broker_proto_major);
        warn_if_diff("broker_proto_minor",  info->broker_proto_minor,  host.broker_proto_minor);
        warn_if_diff("zmq_frame_major",     info->zmq_frame_major,     host.zmq_frame_major);
        warn_if_diff("zmq_frame_minor",     info->zmq_frame_minor,     host.zmq_frame_minor);
        warn_if_diff("script_api_major",    info->script_api_major,    host.script_api_major);
        warn_if_diff("script_api_minor",    info->script_api_minor,    host.script_api_minor);
        warn_if_diff("script_engine_major", info->script_engine_major, host.script_engine_major);
        warn_if_diff("script_engine_minor", info->script_engine_minor, host.script_engine_minor);
        warn_if_diff("config_major",        info->config_major,        host.config_major);
        warn_if_diff("config_minor",        info->config_minor,        host.config_minor);

        // build_id: strict in Debug (NDEBUG absent) or explicit opt-in.
#if defined(PYLABHUB_STRICT_ABI_CHECK) || !defined(NDEBUG)
        const char *host_bid = pylabhub::version::build_id();
        if (info->build_id[0] != '\0' && host_bid != nullptr)
        {
            if (std::strncmp(info->build_id, host_bid,
                             sizeof(info->build_id)) != 0)
            {
                LOGGER_ERROR("[{}] native engine build_id mismatch "
                             "(plugin='{}' vs host='{}') — plugin is stale; "
                             "rebuild against current pylabhub-utils",
                             log_tag_, info->build_id, host_bid);
                return false;
            }
        }
        else if (info->build_id[0] == '\0' && host_bid != nullptr)
        {
            LOGGER_WARN("[{}] native engine reports no build_id but host "
                        "has one ('{}') — cannot verify plugin freshness",
                        log_tag_, host_bid);
        }
#endif
    }
    else
    {
        LOGGER_DEBUG("[{}] native engine uses legacy PlhAbiInfo layout "
                     "({}B < {}B); skipping HEP-0032 component/build_id checks",
                     log_tag_, info->struct_size, kAbiInfoV3Size);
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

    auto hash = pylabhub::utils::security::secure().compute_blake2b_array(buf.data(), buf.size());

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
