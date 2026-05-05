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
#include "utils/timeout_constants.hpp"   // kDefaultAugmentTimeoutHeartbeats

#include <atomic>
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
        , augment_timeout_ms(
              static_cast<int64_t>(pylabhub::kDefaultAugmentTimeoutHeartbeats)
              * static_cast<int64_t>(pylabhub::kDefaultHeartbeatIntervalMs))
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
    /// Lifetime contract: outlives any caller of `augment_*` because
    /// HEP-CORE-0033 §4.2 step 2 drains admin (and broker, when
    /// HUB_TARGETED_ACK lands) BEFORE the runner destroys the engine
    /// + this HubAPI.  Wired in `set_engine`, never reset.
    scripting::ScriptEngine       *engine{nullptr};

    /// HEP-CORE-0033 §12.2.2 augment timeout — atomic so the worker-
    /// thread setter (`set_augment_timeout`) and the admin/broker
    /// reader (`augment_timeout_ms`) interleave safely without a
    /// mutex.  Semantics: -1=infinite, 0=non-blocking, >0=N ms.
    std::atomic<int64_t>           augment_timeout_ms;
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

// ============================================================================
// Augmentation timeout (HEP-CORE-0033 §12.2.2)
// ============================================================================

int64_t HubAPI::augment_timeout_ms() const noexcept
{
    return impl_->augment_timeout_ms.load(std::memory_order_acquire);
}

void HubAPI::set_augment_timeout(int64_t ms) noexcept
{
    impl_->augment_timeout_ms.store(ms, std::memory_order_release);
}

// ============================================================================
// Events (HEP-CORE-0033 §12.2.3 user-posted events)
// ============================================================================

namespace
{

/// Local C-identifier check for `post_event` name argument.  We don't
/// reuse `pylabhub::hub::is_valid_identifier` here because that grammar
/// allows dotted forms (Channel) and reserved sigils (Band, Schema) —
/// neither makes sense for a callback-name suffix.  An event name
/// becomes part of the dispatch lookup `on_app_<name>`, so the
/// constraint is simply "valid C identifier" — leading letter or
/// underscore, then alphanumeric + underscore.
bool is_valid_event_name(std::string_view s) noexcept
{
    if (s.empty())
        return false;
    auto leading = static_cast<unsigned char>(s.front());
    const bool leading_ok =
        (leading >= 'a' && leading <= 'z') ||
        (leading >= 'A' && leading <= 'Z') ||
        (leading == '_');
    if (!leading_ok)
        return false;
    for (size_t i = 1; i < s.size(); ++i)
    {
        auto c = static_cast<unsigned char>(s[i]);
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            (c == '_');
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

void HubAPI::post_event(const std::string &name,
                         const nlohmann::json &data)
{
    if (!is_valid_event_name(name))
        throw std::invalid_argument(
            "HubAPI::post_event: invalid event name '" + name +
            "' (must be a valid C identifier)");

    // Build the IncomingMessage.  Event prefix `app_` reserves the
    // user-posted namespace away from built-in events
    // (`channel_closed`, `role_registered`, …) — see HEP-CORE-0033
    // §12.2.3.  Worker-side dispatch_event prepends `on_`, so the
    // script's callback name is `on_app_<name>`.
    scripting::IncomingMessage m;
    m.event   = "app_" + name;
    m.sender  = impl_->uid;     // hub uid; consistent with broker-posted events
    m.details = data;

    impl_->core.enqueue_message(std::move(m));
    // enqueue_message already calls notify_incoming() internally; no
    // additional wake needed (verified against role_host_core.cpp).
}

// ============================================================================
// Response augmentation (HEP-CORE-0033 §12.2.2)
// ============================================================================
//
// Each method follows the same pattern:
//   1. Probe `engine.has_callback("on_<rpc>")`.  Cheap; reads a single
//      cached py::object / lua ref via the existing has_callback path.
//   2. If absent, return — the caller ships the default response.
//   3. Otherwise call `engine.invoke_returning(...)` with the params
//      and prepared response.  The engine routes to the worker thread
//      (cross-thread queue + future for non-W callers) so the script
//      callback runs with full state visibility (§12.4 invariant).
//   4. If the script returned a non-null value, replace the response;
//      null/None means "keep the default response".
//
// Errors (script raised, engine shutdown) leave the response unchanged —
// the wire reply ships normally; the error is logged and counted as a
// script error inside the engine (§12.2.4).

namespace
{

/// Common fold: dispatch to the script callback if it exists, replace
/// `response` if the script returned a non-null value.  Centralises the
/// has_callback probe + null-return discrimination so each augment_*
/// method below is just a one-line build-args + this call.
///
/// `timeout_ms` is the cross-thread wait bound carried into the engine
/// (HEP-CORE-0033 §12.2.2): -1=infinite, 0=non-blocking, >0=N ms.
/// On TimedOut / ScriptError / EngineShutdown the response is left
/// unchanged — caller ships the default it built.
void run_augment(scripting::ScriptEngine *engine,
                 const std::string         &callback,
                 const nlohmann::json      &args,
                 nlohmann::json            &response,
                 int64_t                    timeout_ms)
{
    if (!engine)
        return;
    if (!engine->has_callback(callback))
        return;

    auto resp = engine->invoke_returning(callback, args, timeout_ms);
    if (resp.status != scripting::InvokeStatus::Ok)
        return;  // script error / engine shutdown / timeout — keep default
    if (resp.value.is_null())
        return;  // script returned None/nil — keep default response
    response = std::move(resp.value);
}

} // namespace

void HubAPI::augment_query_metrics(const nlohmann::json &params,
                                    nlohmann::json &response)
{
    nlohmann::json args = nlohmann::json::object();
    args["params"]   = params;
    args["response"] = response;
    run_augment(impl_->engine, "on_query_metrics", args, response,
                augment_timeout_ms());
}

void HubAPI::augment_list_roles(nlohmann::json &response)
{
    nlohmann::json args = nlohmann::json::object();
    args["response"] = response;
    run_augment(impl_->engine, "on_list_roles", args, response,
                augment_timeout_ms());
}

void HubAPI::augment_get_channel(const std::string &name,
                                  nlohmann::json &response)
{
    nlohmann::json args = nlohmann::json::object();
    args["name"]     = name;
    args["response"] = response;
    run_augment(impl_->engine, "on_get_channel", args, response,
                augment_timeout_ms());
}

void HubAPI::augment_peer_message(const std::string &peer_uid,
                                   const nlohmann::json &msg,
                                   nlohmann::json &response)
{
    nlohmann::json args = nlohmann::json::object();
    args["peer_uid"] = peer_uid;
    args["msg"]      = msg;
    args["response"] = response;
    run_augment(impl_->engine, "on_peer_message", args, response,
                augment_timeout_ms());
}

} // namespace pylabhub::hub_host
