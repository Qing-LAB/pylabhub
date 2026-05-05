#pragma once
/**
 * @file hub_api.hpp
 * @brief HubAPI — script-visible surface for hub-side scripts (HEP-CORE-0033 §12.3).
 *
 * Hub-side parallel of `RoleAPIBase`.  Carried into the script engine
 * via `ScriptEngine::build_api(HubAPI &)` — the sibling overload of
 * `build_api(RoleAPIBase &)`.  Pybind11 / Lua bindings live in
 * `pylabhub-scripting` (parallels `python_engine.cpp` / `lua_engine.cpp`
 * binding `RoleAPIBase`).
 *
 * Contract on the EngineHost<HubAPI> instantiation:
 *   - Constructor `HubAPI(scripting::RoleHostCore &, std::string role_tag,
 *                         std::string uid)` — same shape as RoleAPIBase
 *     so EngineHost's lazy-construction path in startup_() works for
 *     both ApiT specializations without conditional logic.  RoleHostCore
 *     is reused as-is (its cross-thread message queue + wakeup primitives
 *     are exactly what the hub script runner needs; queue-iteration
 *     counters stay zero on the hub side, no harm).
 *   - `ThreadManager &thread_manager()` accessor — required by
 *     EngineHost contract for spawning custom worker threads.
 *
 * Surface summary:
 *   - **Lifecycle / dispatch** (called by `HubScriptRunner`):
 *     `dispatch_event` (forwards drained queue events to
 *     `engine.invoke("on_<event>", details)`), `dispatch_tick`
 *     (forwards `on_tick` per `LoopTimingPolicy` deadline).
 *   - **Read accessors** (script-callable): `name`, `uid`, `config`,
 *     `metrics`, `query_metrics(categories)`, `list_channels` /
 *     `get_channel`, `list_roles` / `get_role`, `list_bands` /
 *     `get_band`, `list_peers` / `get_peer`.  All delegate to
 *     `host_->state()` + the shared `hub_state_json::*` serializers
 *     (single source of truth shared with AdminService).
 *   - **Control delegates** (script-callable): `close_channel`,
 *     `broadcast_channel`, `request_shutdown`.  Each delegates to
 *     `host_->broker().request_*` / `host_->request_shutdown()` —
 *     same paths AdminService's curated admin RPCs use.
 *   - **Veto hooks** (sync, bool return): not yet wired (HEP-0033
 *     §12.2 "veto hooks" block — broker thread blocks on script
 *     thread for a sync `bool`; needs broker hook points).
 *
 * Lifetime:
 *   - Constructed by EngineHost<HubAPI>::startup_() (lazy, on the
 *     runner's worker thread) so the owned `ThreadManager` is parented
 *     to the right thread for bounded-join shutdown.
 *   - Destroyed by EngineHost::shutdown_() before the engine is dropped.
 *   - The HubHost backref must outlive HubAPI; per HEP-0033 §4.2 the
 *     ScriptRunner is stopped (step 2) BEFORE HubHost destruction begins.
 */

#include "pylabhub_utils_export.h"
#include "utils/engine_host.hpp"     // script_host_traits<ApiT> primary template

#include <memory>
#include <string>
#include <string_view>
#include <variant>                   // std::monostate for the trait specialization
#include <vector>                    // query_metrics(categories)

namespace pylabhub::utils        { class ThreadManager; }
namespace pylabhub::hub_host     { class HubHost; }
namespace pylabhub::scripting    { class RoleHostCore;
                                   class ScriptEngine;
                                   struct IncomingMessage; }

namespace pylabhub::hub_host
{

/// Script-visible surface for hub-side scripts.  See file-header
/// docstring for the surface summary; method-level docstrings below
/// for individual contracts.
class PYLABHUB_UTILS_EXPORT HubAPI
{
public:
    /// EngineHost<HubAPI>::startup_() constructs HubAPI lazily on the
    /// runner's worker thread.  All three params are required:
    ///
    /// @param core      RoleHostCore owned by EngineHost (lifetime > api).
    ///                  Used for cross-thread message queue + shutdown
    ///                  flag wiring.
    /// @param role_tag  Always "hub" for the hub-side instantiation;
    ///                  carried verbatim through to log prefixes.
    /// @param uid       Hub instance uid (e.g. "hub.lab1.uid00000001").
    HubAPI(scripting::RoleHostCore &core,
           std::string              role_tag,
           std::string              uid);
    ~HubAPI();

    HubAPI(const HubAPI &)            = delete;
    HubAPI &operator=(const HubAPI &) = delete;
    HubAPI(HubAPI &&)                 = delete;
    HubAPI &operator=(HubAPI &&)      = delete;

    // ── EngineHost contract ───────────────────────────────────────────────

    /// Required by EngineHost<HubAPI> for spawning custom worker threads.
    /// Owns "ThreadManager:hub:<uid>" — same auto-registered dynamic
    /// LifecycleGuard module pattern roles use.
    [[nodiscard]] utils::ThreadManager &thread_manager();

    /// Hub-side analogue of `RoleAPIBase::core()`.  Returns the
    /// `RoleHostCore` HubAPI was constructed against — used by
    /// `ScriptEngine::on_pcall_error_` to bump the script-error counter
    /// + request shutdown via the SAME core path the role engines use,
    /// so the `current_core()`-resolved access is uniform across both
    /// ApiT instantiations.
    [[nodiscard]] scripting::RoleHostCore *core() const noexcept;

    // ── Script-visible API (HEP-CORE-0033 §12.3) ──────────────────────────
    //
    // Mirrors RoleAPIBase signatures verbatim where the hub equivalent
    // makes sense:
    //   - `log(level, msg)` — same param order, same level tokens
    //     ("info" / "warn" / "error" / "debug"; default = info).
    //   - `metrics()` is the hub equivalent of role's
    //     `snapshot_metrics_json()` — no parameter, returns a JSON
    //     snapshot.  For hub the snapshot is the broker's full
    //     metrics view (HEP-CORE-0019 + §9.4); for role it was the
    //     role's own queue + custom metrics.
    //   - `uid()` is the hub equivalent of role's `uid()` accessor;
    //     `role_tag()` is omitted because it's always "hub".
    //
    // Read accessors below (list_channels / get_channel / etc.) and
    // control delegates (close_channel / etc.) follow the same pattern:
    // one C++ method here + one binding line in PythonEngine + one
    // closure entry in LuaEngine.

    /// Emit a log line through the process Logger sink with a
    /// `[hub/<uid>]` prefix.  Level tokens follow the role-side
    /// `RoleAPIBase::log` convention: "debug" / "warn"/"warning" /
    /// "error" route to LOGGER_DEBUG / WARN / ERROR; anything else
    /// routes to LOGGER_INFO.
    void log(const std::string &level, const std::string &msg);

    /// Snapshot of the broker's metrics — channel/role/band/peer
    /// aggregates plus broker counters.  Same JSON shape AdminService
    /// emits for `query_metrics` (single source of truth via the
    /// shared `hub_state_json` serializers).  Empty filter = all
    /// categories.
    ///
    /// Returns an empty object (not an error) when called before
    /// `set_host` has wired the HubHost backref — should not happen
    /// in production (HubScriptRunner sets host immediately after
    /// construction) but the path is defended for safety.
    [[nodiscard]] nlohmann::json metrics() const;

    /// Hub instance uid (e.g. "hub.lab1.uid00000001").  Captured at
    /// construction; stable for the life of the HubAPI instance.
    [[nodiscard]] const std::string &uid() const noexcept;

    // ── Read accessors (HEP-CORE-0033 §12.3 read block) ───────────────────
    //
    // Each delegates to `host_->state().*` and serializes via the shared
    // `utils/hub_state_json.hpp` helpers (single source of truth shared
    // with AdminService — same JSON shape operators see through the
    // admin RPC).  All return nlohmann::json so the pybind11 / Lua
    // bindings convert via the shared `json_to_py` / `json_to_lua`
    // helpers without per-method binding logic.
    //
    // All methods return an empty array / null / empty object (NOT an
    // error) when called before `set_host` has wired the HubHost
    // backref — should not happen in production but the path is
    // defended for safety.  Caller can distinguish "no host" from
    // "no entries" by the host wiring contract (post-startup the
    // host is always set).

    /// Hub display name (`cfg.identity().name`) — distinct from `uid()`
    /// which is the canonical instance identifier.
    [[nodiscard]] const std::string &name() const noexcept;

    /// Full hub config snapshot — equivalent to `HubConfig::raw()`.
    /// Returns the JSON the operator wrote into hub.json (post-default-
    /// merge).  Read-only — scripts inspect, they don't mutate.
    [[nodiscard]] nlohmann::json config() const;

    /// List of all currently-registered channels, each serialized via
    /// `channel_to_json`.  Order follows HubState's internal map order
    /// (unordered — scripts that need stable ordering should sort).
    [[nodiscard]] nlohmann::json list_channels() const;

    /// Single channel lookup by name.  Returns the channel JSON if
    /// registered, or `null` (JSON null) if not — scripts can test
    /// truthiness in their language.
    [[nodiscard]] nlohmann::json get_channel(const std::string &name) const;

    /// List of all roles, serialized via `role_to_json`.
    [[nodiscard]] nlohmann::json list_roles() const;

    /// Single role lookup by uid.  Returns `null` if not found.
    [[nodiscard]] nlohmann::json get_role(const std::string &role_uid) const;

    /// List of all bands (HEP-CORE-0030), serialized via `band_to_json`.
    [[nodiscard]] nlohmann::json list_bands() const;

    /// Single band lookup by name.  Returns `null` if not found.
    [[nodiscard]] nlohmann::json get_band(const std::string &name) const;

    /// List of all federation peers, serialized via `peer_to_json`.
    [[nodiscard]] nlohmann::json list_peers() const;

    /// Single peer lookup by hub_uid.  Returns `null` if not found.
    [[nodiscard]] nlohmann::json get_peer(const std::string &hub_uid) const;

    /// Filtered metrics — optional category list (e.g.
    /// `{"channels", "counters"}`); empty/absent = all categories.
    /// Same JSON shape `metrics()` produces, but with the
    /// `MetricsFilter::categories` set populated from `categories`.
    [[nodiscard]] nlohmann::json
    query_metrics(const std::vector<std::string> &categories) const;

    // ── Control delegates (HEP-CORE-0033 §12.3 control block) ─────────────
    //
    // Fire-and-forget mutators.  Each delegates to an existing
    // `host_->broker().request_*` or `host_->request_shutdown()` —
    // same code path AdminService's `handle_close_channel` /
    // `handle_broadcast_channel` / `handle_request_shutdown` use.
    // The script-side wrappers don't return errors; the broker is
    // responsible for tolerating unknown-channel inputs idempotently
    // (matches the admin RPC accept-ack semantics — the response is
    // the queue accept, not a completion signal).
    //
    // No-op (silently discarded) when called before `set_host` has
    // wired the HubHost backref.

    /// Request the broker to close a channel (drains in-flight slots,
    /// notifies consumers, removes from `HubState`).  Idempotent for
    /// unknown channel names.
    void close_channel(const std::string &name);

    /// Send a control-plane broadcast frame to all consumers of a
    /// channel.  `data` is optional — empty string by default for
    /// pure control messages.
    void broadcast_channel(const std::string &channel,
                            const std::string &message,
                            const std::string &data = "");

    /// Request hub shutdown.  Sets the host's shutdown flag and wakes
    /// any thread blocked on `host.run_main_loop()` (or an equivalent
    /// wait-for-shutdown caller).  The orderly teardown sequence
    /// (runner → admin → broker → drain) runs on the main thread when
    /// `host.shutdown()` is invoked next.  Idempotent.
    void request_shutdown() noexcept;

    // ── Host wiring (called once by HubScriptRunner after construction) ───

    /// Bind to the HubHost backref.  Required before any state-reading
    /// or mutator-delegating method is invoked.  Not in the ctor
    /// because HubAPI is constructed lazily inside EngineHost's
    /// startup_() path — at that point HubHost is the OUTER caller, so
    /// passing `*this` from inside HubScriptRunner's worker_main_()
    /// post-construction is the cleanest sequencing.
    void set_host(HubHost &host) noexcept;

    // ── Dispatch surface ──────────────────────────────────────────────────

    /// Forward a HubState event (drained from the runner's queue) to
    /// the script's named callback.  Looks up `on_<event>` (e.g.
    /// `on_channel_opened`) in the script namespace; if not defined,
    /// the engine no-ops.  Payload is the JSON serialization produced
    /// on the broker thread by `channel_to_json` / `role_to_json` /
    /// etc. (the shared `hub_state_json` serializers).
    ///
    /// Called by HubScriptRunner::worker_main_() on the runner thread.
    void dispatch_event(const scripting::IncomingMessage &msg);

    /// Fire the periodic `on_tick` callback.  Called by the runner when
    /// the configured LoopTimingPolicy deadline elapses.  No-op if the
    /// script does not define `on_tick`.
    void dispatch_tick();

    // ── Engine wiring (called once by HubScriptRunner before dispatch) ────

    /// Set the script engine pointer used by dispatch_event/tick.  The
    /// engine is owned by EngineHost; HubAPI holds a non-owning pointer.
    void set_engine(scripting::ScriptEngine &engine) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::hub_host

// ============================================================================
// script_host_traits<HubAPI> specialization
// ============================================================================
//
// Located here (alongside HubAPI's public declaration) rather than in
// the private hub_script_runner.hpp so that engine_host.cpp can include
// it directly to instantiate `template class EngineHost<HubAPI>;`.
//
// **ConfigT = std::monostate** — single-config-owner design:
//   - HubHost is the SOLE owner of HubConfig (one config per hub
//     instance — see HEP-CORE-0033 §4).
//   - HubScriptRunner is a SUBSYSTEM under HubHost, not its own
//     top-level container.  It does not own a config; it reads what
//     it needs (uid + timing) through its `host_` backref.
//   - The runner therefore stores nothing config-shaped of its own.
//     `EngineHost<HubAPI>::config()` returns a `const std::monostate&`
//     and is intentionally never called on the hub side — verified by
//     code search.
//
// Contrast with the role-side specialization (`ConfigT = RoleConfig`):
// the role binary IS the top-level container; its EngineHost
// instantiation is the sole owner of RoleConfig.  EngineHost itself
// doesn't reach into ConfigT — uid is passed as a separate ctor param
// (D2.1) — so the same template body works for both shapes.

namespace pylabhub::scripting
{

template <>
struct script_host_traits<pylabhub::hub_host::HubAPI>
{
    using ConfigT = std::monostate;
    /// `monostate` carries no fields — the hub uid is owned by HubHost,
    /// and `HubScriptRunner` populates it via `set_uid()` in its ctor
    /// body using the host backref (`host_.config().identity().uid`).
    /// Returning empty here lets the EngineHost ctor complete without
    /// reaching into anything that doesn't exist; the uid is overwritten
    /// before startup_() reads it.  Trait contract documented in
    /// `engine_host.hpp`.
    static std::string uid_from_config(const std::monostate &)
    {
        return std::string{};
    }
};

} // namespace pylabhub::scripting
