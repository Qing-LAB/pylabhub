/**
 * @file hub_api.cpp
 * @brief HubAPI implementation — Phase 7 dispatch surface only.
 *
 * Phase 7 Commit C ships the minimal class — event/tick forwarding to
 * the script engine and the EngineHost-contract members (ctor + thread
 * manager).  Phase 8 will add the rich state-read + control-op methods
 * that scripts call from inside their callbacks.
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
    /// after EngineHost::startup_() constructs HubAPI.  Phase 8 dispatch
    /// methods (list_channels / close_channel etc.) read state and call
    /// mutators through this.  Non-owning.
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
// Dispatch surface (Phase 7)
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
// Script-visible API (Phase 7 minimum) — log / metrics / uid
// ============================================================================
//
// Mirrors RoleAPIBase signatures.  Each method is independently
// testable; metrics() additionally requires a wired HubHost backref
// (set_host) and a started broker — that path is exercised at the
// integration level in Phase 7 Commit D L3 tests.

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
    // emits for query_metrics — single source of truth via Phase 6.2b's
    // hub_state_json serializers (channel_to_json / role_to_json /
    // band_to_json / peer_to_json / broker_counters_to_json).
    return impl_->host->broker().query_metrics(pylabhub::hub::MetricsFilter{});
}

const std::string &HubAPI::uid() const noexcept
{
    return impl_->uid;
}

// ============================================================================
// Phase 8a — Read accessors (HEP-CORE-0033 §12.3 read block)
// ============================================================================
//
// All methods defend against `host == nullptr` (pre-`set_host` call) so a
// script can call them at any point without nullptr-deref; the empty
// fallback is documented in the header.  In production the runner wires
// host immediately after EngineHost constructs HubAPI, so the fallback
// only fires in unit tests that exercise HubAPI in isolation.
//
// Each method delegates to `host_->state().*` (HubState lookup) and
// serializes via the shared `hub_state_json::*` helpers.  The List
// methods snapshot the whole map and serialize each entry; the Get
// methods use the typed lookup to avoid copying the full snapshot.

namespace
{
    // Cached static empty value to avoid allocating per-call when the
    // host backref is missing.  Read-only.
    const std::string &empty_string()
    {
        static const std::string e;
        return e;
    }
}

const std::string &HubAPI::name() const noexcept
{
    if (!impl_->host)
        return empty_string();
    return impl_->host->config().identity().name;
}

nlohmann::json HubAPI::config() const
{
    if (!impl_->host)
        return nlohmann::json::object();
    // HubConfig::raw() returns the parsed JSON (post-default-merge).
    // Read-only — scripts inspect; mutations go through the curated
    // admin RPCs / Phase 8b control delegates.
    return impl_->host->config().raw();
}

nlohmann::json HubAPI::list_channels() const
{
    if (!impl_->host)
        return nlohmann::json::array();
    namespace js = pylabhub::hub;
    nlohmann::json arr = nlohmann::json::array();
    const auto snap = impl_->host->state().snapshot();
    for (const auto &[name, ch] : snap.channels)
        arr.push_back(js::channel_to_json(ch));
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
    namespace js = pylabhub::hub;
    nlohmann::json arr = nlohmann::json::array();
    const auto snap = impl_->host->state().snapshot();
    for (const auto &[uid, r] : snap.roles)
        arr.push_back(js::role_to_json(r));
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
    namespace js = pylabhub::hub;
    nlohmann::json arr = nlohmann::json::array();
    const auto snap = impl_->host->state().snapshot();
    for (const auto &[name, b] : snap.bands)
        arr.push_back(js::band_to_json(b));
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
    namespace js = pylabhub::hub;
    nlohmann::json arr = nlohmann::json::array();
    const auto snap = impl_->host->state().snapshot();
    for (const auto &[uid, p] : snap.peers)
        arr.push_back(js::peer_to_json(p));
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

} // namespace pylabhub::hub_host
