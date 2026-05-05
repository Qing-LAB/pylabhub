/**
 * @file hub_api.cpp
 * @brief HubAPI implementation — script-visible surface for hub-side scripts.
 *
 * Three surface groups:
 *   1. EngineHost-contract members (ctor, thread_manager, core).
 *   2. Lifecycle / dispatch (set_host, set_engine, dispatch_event,
 *      dispatch_tick, log, metrics, uid).
 *   3. Read accessors + control delegates (HEP-CORE-0033 §12.3) —
 *      delegate to `host_->state()` + `hub_state_json` serializers
 *      / `host_->broker().request_*` / `host_->request_shutdown()`.
 *
 * See hub_api.hpp for the full surface summary and lifetime contract.
 */

#include "utils/hub_api.hpp"

#include "utils/broker_service.hpp"      // query_metrics for HubAPI::metrics()
#include "utils/config/hub_config.hpp"   // host.config().raw() / .identity()
#include "utils/hub_host.hpp"            // backref for metrics path
#include "utils/hub_metrics_filter.hpp"  // empty MetricsFilter = all categories
#include "utils/hub_state.hpp"           // HubState::channel/role/band/peer/snapshot
#include "utils/hub_state_json.hpp"      // channel_to_json / role_to_json / etc.
#include "utils/logger.hpp"              // LOGGER_INFO/WARN/ERROR/DEBUG
#include "utils/role_host_core.hpp"      // IncomingMessage, RoleHostCore
#include "utils/script_engine.hpp"       // ScriptEngine::invoke / eval
#include "utils/thread_manager.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace pylabhub::hub_host
{

struct HubAPI::Impl
{
    Impl(scripting::RoleHostCore &core,
         std::string              role_tag,
         std::string              uid)
        : core(core)
        , role_tag(std::move(role_tag))
        , uid(std::move(uid))
        , thread_mgr(this->role_tag, this->uid)
    {}

    scripting::RoleHostCore       &core;             // owned by EngineHost
    std::string                    role_tag;         // "hub"
    std::string                    uid;              // hub instance uid

    /// HubAPI's own ThreadManager — auto-registered as
    /// "ThreadManager:hub:<uid>" dynamic LifecycleGuard module.  Same
    /// shape RoleAPIBase uses; bounded-join shutdown applies uniformly.
    utils::ThreadManager           thread_mgr;

    /// Backref to the outer HubHost.  Set by HubScriptRunner immediately
    /// after EngineHost::startup_() constructs HubAPI.  Read accessors
    /// (`list_channels`, `get_role`, …) and control delegates
    /// (`close_channel`, …) read state and call mutators through this.
    /// Non-owning.
    HubHost                       *host{nullptr};

    /// Script engine pointer — set by HubScriptRunner before any
    /// dispatch call.  Non-owning (engine owned by EngineHost).
    scripting::ScriptEngine       *engine{nullptr};
};

// ============================================================================
// Construction / destruction
// ============================================================================

HubAPI::HubAPI(scripting::RoleHostCore &core,
                std::string             role_tag,
                std::string             uid)
{
    if (role_tag.empty())
        throw std::invalid_argument("HubAPI: role_tag must be non-empty");
    if (uid.empty())
        throw std::invalid_argument("HubAPI: uid must be non-empty");

    impl_ = std::make_unique<Impl>(core, std::move(role_tag), std::move(uid));
}

HubAPI::~HubAPI() = default;

// ============================================================================
// EngineHost contract
// ============================================================================

utils::ThreadManager &HubAPI::thread_manager()
{
    return impl_->thread_mgr;
}

scripting::RoleHostCore *HubAPI::core() const noexcept
{
    return &impl_->core;
}

// ============================================================================
// Wiring (called by HubScriptRunner immediately post-construction)
// ============================================================================

void HubAPI::set_host(HubHost &host) noexcept
{
    impl_->host = &host;
}

void HubAPI::set_engine(scripting::ScriptEngine &engine) noexcept
{
    impl_->engine = &engine;
}

// ============================================================================
// Dispatch surface
// ============================================================================

void HubAPI::dispatch_event(const scripting::IncomingMessage &msg)
{
    if (!impl_->engine)
        return;

    // Map the event name (`channel_opened` / `role_registered` / ...) to
    // the script-side callback name (`on_channel_opened` etc.) and
    // invoke with the JSON payload.  `invoke(name, args)` is the
    // existing ScriptEngine virtual; engines that lack the named
    // callback no-op (returns false; we don't surface the missing-
    // callback signal as an error — many scripts only register a subset
    // of events).
    const std::string callback = "on_" + msg.event;
    (void) impl_->engine->invoke(callback, msg.details);
}

void HubAPI::dispatch_tick()
{
    if (!impl_->engine)
        return;
    (void) impl_->engine->invoke("on_tick");
}

// ============================================================================
// Script-visible API — log / metrics / uid
// ============================================================================
//
// Mirrors RoleAPIBase signatures.  Each method is independently
// testable; metrics() additionally requires a wired HubHost backref
// (set_host) and a started broker — exercised end-to-end by the
// L3 hub integration tests (test_hub_lua_integration /
// test_hub_python_integration).

void HubAPI::log(const std::string &level, const std::string &msg)
{
    // Same level-token mapping as RoleAPIBase::log (case-insensitive
    // for "Warn"/"Error" too).  Anything we don't recognize routes to
    // LOGGER_INFO — never silently drops the message.  Prefix
    // "[hub/<uid>]" matches role's "[<role_tag>/<uid>]" so log
    // analysis tooling can parse both cases with one regex.
    const std::string_view lv = level;
    if (lv == "debug" || lv == "Debug")
        LOGGER_DEBUG("[hub/{}] {}", impl_->uid, msg);
    else if (lv == "warn" || lv == "Warn" || lv == "warning")
        LOGGER_WARN("[hub/{}] {}", impl_->uid, msg);
    else if (lv == "error" || lv == "Error")
        LOGGER_ERROR("[hub/{}] {}", impl_->uid, msg);
    else
        LOGGER_INFO("[hub/{}] {}", impl_->uid, msg);
}

nlohmann::json HubAPI::metrics() const
{
    // Defended path: if set_host was never called we return an empty
    // object rather than nullptr-deref.  Production code path always
    // sets host immediately post-construction (HubScriptRunner does
    // it on the worker thread before subscribing to events) so this
    // branch should not fire in normal operation.
    if (!impl_->host)
        return nlohmann::json::object();
    // Empty filter ⇒ all categories.  Same JSON shape AdminService
    // emits for query_metrics — single source of truth via the shared
    // `hub_state_json` serializers (channel_to_json / role_to_json /
    // band_to_json / peer_to_json / broker_counters_to_json).
    return impl_->host->broker().query_metrics(pylabhub::hub::MetricsFilter{});
}

const std::string &HubAPI::uid() const noexcept
{
    return impl_->uid;
}

// ============================================================================
// Read accessors (HEP-CORE-0033 §12.3 read block)
// ============================================================================
//
// All methods defend against `host == nullptr` (pre-`set_host` call) so a
// script can call them at any point without nullptr-deref; the empty
// fallback is documented in the header.  In production the runner wires
// host immediately after EngineHost constructs HubAPI, so the fallback
// only fires in unit tests that exercise HubAPI in isolation.
//
// Each method delegates to `host_->state().*` (HubState lookup) and
// serializes via the shared `pylabhub::hub::*_to_json` helpers.  The
// list_*  methods snapshot the whole map and serialize each entry;
// the get_*  methods use the typed lookup to avoid copying the full
// snapshot.

const std::string &HubAPI::name() const noexcept
{
    static const std::string empty;
    if (!impl_->host)
        return empty;
    return impl_->host->config().identity().name;
}

nlohmann::json HubAPI::config() const
{
    if (!impl_->host)
        return nlohmann::json::object();
    // HubConfig::raw() returns the parsed JSON (post-default-merge).
    // Read-only — scripts inspect; mutations go through curated admin
    // RPCs and the control delegates below.
    return impl_->host->config().raw();
}

nlohmann::json HubAPI::list_channels() const
{
    if (!impl_->host)
        return nlohmann::json::array();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &kv : impl_->host->state().snapshot().channels)
        arr.push_back(pylabhub::hub::channel_to_json(kv.second));
    return arr;
}

nlohmann::json HubAPI::get_channel(const std::string &name) const
{
    if (!impl_->host)
        return nullptr;
    const auto opt = impl_->host->state().channel(name);
    if (!opt.has_value())
        return nullptr;
    return pylabhub::hub::channel_to_json(*opt);
}

nlohmann::json HubAPI::list_roles() const
{
    if (!impl_->host)
        return nlohmann::json::array();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &kv : impl_->host->state().snapshot().roles)
        arr.push_back(pylabhub::hub::role_to_json(kv.second));
    return arr;
}

nlohmann::json HubAPI::get_role(const std::string &role_uid) const
{
    if (!impl_->host)
        return nullptr;
    const auto opt = impl_->host->state().role(role_uid);
    if (!opt.has_value())
        return nullptr;
    return pylabhub::hub::role_to_json(*opt);
}

nlohmann::json HubAPI::list_bands() const
{
    if (!impl_->host)
        return nlohmann::json::array();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &kv : impl_->host->state().snapshot().bands)
        arr.push_back(pylabhub::hub::band_to_json(kv.second));
    return arr;
}

nlohmann::json HubAPI::get_band(const std::string &name) const
{
    if (!impl_->host)
        return nullptr;
    const auto opt = impl_->host->state().band(name);
    if (!opt.has_value())
        return nullptr;
    return pylabhub::hub::band_to_json(*opt);
}

nlohmann::json HubAPI::list_peers() const
{
    if (!impl_->host)
        return nlohmann::json::array();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &kv : impl_->host->state().snapshot().peers)
        arr.push_back(pylabhub::hub::peer_to_json(kv.second));
    return arr;
}

nlohmann::json HubAPI::get_peer(const std::string &hub_uid) const
{
    if (!impl_->host)
        return nullptr;
    const auto opt = impl_->host->state().peer(hub_uid);
    if (!opt.has_value())
        return nullptr;
    return pylabhub::hub::peer_to_json(*opt);
}

nlohmann::json HubAPI::query_metrics(const std::vector<std::string> &categories) const
{
    if (!impl_->host)
        return nlohmann::json::object();
    pylabhub::hub::MetricsFilter filter;
    for (const auto &c : categories)
        filter.categories.insert(c);
    return impl_->host->broker().query_metrics(filter);
}

// ============================================================================
// Control delegates (HEP-CORE-0033 §12.3 control block)
// ============================================================================
//
// All methods are fire-and-forget — the broker queue absorbs the
// request and processes asynchronously.  No return value because there's
// nothing to wait for; the broker handles validation (unknown channel
// names → idempotent drop) and schedule.  No-op if the HubHost backref
// isn't wired yet (defended for unit tests in isolation).

void HubAPI::close_channel(const std::string &name)
{
    if (!impl_->host)
        return;
    impl_->host->broker().request_close_channel(name);
}

void HubAPI::broadcast_channel(const std::string &channel,
                                const std::string &message,
                                const std::string &data)
{
    if (!impl_->host)
        return;
    impl_->host->broker().request_broadcast_channel(channel, message, data);
}

void HubAPI::request_shutdown() noexcept
{
    if (!impl_->host)
        return;
    impl_->host->request_shutdown();
}

} // namespace pylabhub::hub_host
