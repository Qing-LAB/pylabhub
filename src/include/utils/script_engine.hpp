#pragma once
/**
 * @file script_engine.hpp
 * @brief ScriptEngine — abstract interface for pluggable script engines.
 *
 * A ScriptEngine is a stateful callback dispatcher that knows how to:
 *   - Load a script and extract callbacks
 *   - Build typed slot views in the engine's type system
 *   - Invoke callbacks with raw memory pointers
 *   - Interpret return values (commit/discard/error)
 *
 * The engine does NOT know about queues, messengers, or the data loop.
 * The C++ framework (RoleHost) owns infrastructure and calls engine methods
 * at well-defined points from the working thread.
 *
 * ## Threading contract
 *
 * All methods are called from a single thread (the working thread) unless
 * supports_multi_state() returns true.  The engine is created, used, and
 * destroyed on that thread.
 *
 * ## Engine implementations
 *
 *   - LuaEngine:    wraps LuaState (direct pcall, no GIL)
 *   - PythonEngine: wraps py::scoped_interpreter (GIL held for lifetime;
 *                   per-invoke acquire is no-op)
 *   - NativeEngine: wraps dlopen'd plugin (see HEP-CORE-0028)
 *
 * See HEP-CORE-0011 for the abstraction framework and
 * docs/tech_draft/engine_thread_model.md for the threading model.
 */

#include "role_host_core.hpp"
#include "utils/role_api_base.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pylabhub::hub_host
{
class HubAPI;
} // namespace pylabhub::hub_host
  // HEP-0033 Phase 7 / 8

namespace pylabhub::scripting
{

using pylabhub::hub_host::HubAPI;

// Maximum recursion depth for JSON ↔ script type conversion.
// Set via CMake option PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH (default: 20).
#ifndef PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH
#define PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH 20
#endif
inline constexpr int kScriptMaxRecursionDepth = PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH;

// ============================================================================
// InvokeResult — return value from hot-path on_produce / on_process
// ============================================================================

enum class InvokeResult
{
    Commit,  ///< Script returned true/nil — publish the slot.
    Discard, ///< Script returned false — discard the slot, loop continues.
    Error,   ///< Script raised an error — discard, increment error count.
};

// ============================================================================
// InvokeStatus — mechanical result of a generic invoke() / eval() call
// ============================================================================

/// Distinct from InvokeResult which is the semantic data-callback result.
enum class InvokeStatus : uint8_t
{
    Ok = 0,             ///< Function executed successfully.
    NotFound = 1,       ///< Function name not found in script.
    ScriptError = 2,    ///< Script raised an exception.
    EngineShutdown = 3, ///< Engine is finalizing; request rejected.
    TimedOut = 4,       ///< Cross-thread future expired before the
                        ///< owner thread drained (HEP-CORE-0033 §12.2.2
                        ///< augment_timeout); response unchanged.
};

/// Response from a generic invoke() / eval() / invoke_returning() call.
struct InvokeResponse
{
    InvokeStatus status{InvokeStatus::Ok};
    nlohmann::json value; ///< Populated for eval() and invoke_returning();
                          ///< empty for invoke().
};

// RoleContext is eliminated — RoleAPIBase is the single context structure.
// Design: docs/archive/transient-2026-04-05/role_context_simplification.md.
// Architecture: docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md.

// ============================================================================
// InvokeRx / InvokeTx — directional data for invoke calls
// ============================================================================

/// Read-only input direction — data received from upstream.
/// Slot is const (system-managed via SlotRWState). Flexzone is NOT carried
/// here — scripts access it via api.flexzone(ChannelSide::Rx), which returns
/// a cached typed view built once at build_api() time.
/// Passed by value (16 bytes, fits in registers on x86-64).
struct InvokeRx
{
    const void *slot{nullptr}; ///< Read-only input slot.
    size_t slot_size{0};
};

/// Writable output direction — data going downstream.
/// Flexzone: see InvokeRx note — accessed via api.flexzone(ChannelSide::Tx).
struct InvokeTx
{
    void *slot{nullptr}; ///< Writable output slot.
    size_t slot_size{0};
};

/// Inbox message — one-shot peer-to-peer delivery (no flexzone, no ring buffer).
struct InvokeInbox
{
    const void *data{nullptr}; ///< Typed payload (valid until next recv_one()).
    size_t data_size{0};
    std::string sender_uid; ///< Sender's role UID.
    uint64_t seq{0};        ///< Sender's monotonic sequence number.
};

// ============================================================================
// ScriptEngine — abstract interface
// ============================================================================

class ScriptEngine
{
  public:
    virtual ~ScriptEngine() = default;

    // ── Engine state machine ──────────────────────────────────────────────

    enum class EngineState : uint8_t
    {
        Unloaded,
        Initialized,
        ScriptLoaded,
        ApiBuilt,
        Finalized
    };

    [[nodiscard]] EngineState engine_state() const noexcept { return state_; }

    // ── Lifecycle (called from working thread) ───────────────────────────

    /**
     * @brief Create the interpreter/state and apply sandbox.
     *
     * Sets owner_thread_id_. State transitions to Initialized on success,
     * but accepting_ stays false until build_api() completes.
     * On failure, state remains Unloaded. Calls init_engine_() for specifics.
     *
     * @param log_tag Tag for log messages (e.g., "prod").
     * @param core    Pointer to RoleHostCore — provides metrics counters
     *                (script_errors, etc.) and shutdown flags. Must remain
     *                valid for the engine's lifetime.
     * @return true on success.
     */
    bool initialize(const std::string &log_tag, RoleHostCore *core)
    {
        owner_thread_id_ = std::this_thread::get_id();
        if (!init_engine_(log_tag, core))
            return false;
        state_ = EngineState::Initialized;
        return true;
    }

    /**
     * @brief Load script file and extract callbacks.
     * @param script_dir Directory containing the script (e.g., .../script/lua/).
     * @param entry_point Script entry point filename (e.g., "init.lua", "__init__.py").
     *        Lua loads this file directly. Python uses package-based import derived
     *        from script_dir and ignores entry_point (always imports __init__.py).
     * @param required_callback Name of the required callback (e.g., "on_produce").
     * @return true if script loaded and required callback exists.
     */
    virtual bool load_script(const std::filesystem::path &script_dir,
                             const std::string &entry_point,
                             const std::string &required_callback) = 0;

    /**
     * @brief Build the API object/table using role-specific context.
     *
     * Called after load_script() and before invoke_on_init().  Sets api_
     * on the base class, calls engine-specific build_api_(), then sets
     * accepting_=true on success. Returns false if build_api_() fails
     * (e.g. unknown short_tag) — engine remains non-accepting.
     */
    [[nodiscard]] bool build_api(RoleAPIBase &api)
    {
        api_ = &api;
        if (!build_api_(api))
            return false;
        state_ = EngineState::ApiBuilt;
        accepting_.store(true, std::memory_order_release);
        return true;
    }

    /**
     * @brief Hub-side overload of @ref build_api.
     *
     * Same lifecycle bookkeeping as the role-side wrapper but stores the
     * pointer in @ref hub_api_ and dispatches to the sibling virtual
     * @ref build_api_(HubAPI &).  A given engine instance is bound to ONE
     * API surface (role or hub) for its lifetime — either @ref api_
     * (role) or @ref hub_api_ (hub) is non-null after this returns true,
     * never both.  Hub-side concrete engines override `build_api_(HubAPI&)`;
     * role-only engines use the no-op default and never reach this path
     * (no caller invokes it).
     */
    [[nodiscard]] bool build_api(HubAPI &api)
    {
        hub_api_ = &api;
        if (!build_api_(api))
            return false;
        state_ = EngineState::ApiBuilt;
        accepting_.store(true, std::memory_order_release);
        return true;
    }

    /**
     * @brief Stop accepting new invoke() requests from non-owner threads.
     *
     * Called by the role host before invoke_on_stop(). After this call,
     * all non-owner invoke() calls return false immediately (including
     * child engines that check the owner's flag). Owner-thread hot-path
     * methods (invoke_on_stop, invoke_produce, etc.) still work.
     */
    void stop_accepting() noexcept { accepting_.store(false, std::memory_order_release); }

    /**
     * @brief Check if the engine is accepting invoke() requests.
     *
     * Child engines delegate to the owner's flag. Owner engines check their own.
     * All invoke()/eval() paths call this before executing.
     */
    [[nodiscard]] bool is_accepting() const noexcept
    {
        if (owner_engine_)
            return owner_engine_->is_accepting();
        return accepting_.load(std::memory_order_acquire);
    }

    /**
     * @brief Set the owner engine pointer (for child/thread-local states).
     * Child engines delegate is_accepting() to the owner.
     */
    void set_owner_engine(ScriptEngine *owner) noexcept { owner_engine_ = owner; }

    /**
     * @brief Release all script objects, close interpreter/state.
     *
     * Sets accepting_=false, then calls engine-specific finalize_engine_().
     * Called on the owner thread after invoke_on_stop() has returned.
     * After this call, the engine is in a destroyed state — no further
     * calls are valid. Must be called BEFORE infrastructure teardown.
     */
    void finalize()
    {
        if (state_ == EngineState::Finalized)
            return; // idempotent
        stop_accepting();
        finalize_engine_(); // Engine guards against double-finalize internally.
        state_ = EngineState::Finalized;
    }

    // ── Queries ──────────────────────────────────────────────────────────

    /**
     * @brief Is `name` registered as a script-side callback?
     *
     * **THREAD-SAFETY (HEP-CORE-0011 §"Engine Thread Affinity"): any
     * thread.**  Callers include the worker thread (per-frame cycle
     * gating), the broker control thread
     * (`RoleAPIBase::on_heartbeat_tick_`), and admin RPC handler
     * threads (`HubAPI::augment_*`).  This default implementation
     * reads the pre-populated `standard_callback_presence_` cache —
     * plain `unordered_map<string, bool>` lookup, no language-runtime
     * access (no Python GIL, no `lua_State`).
     *
     * Subclasses populate the cache during `load_script()` (worker
     * thread, runtime lock held) by calling
     * `set_standard_callback_present(name, found)` for each name in
     * the standard set, then `freeze_standard_callback_cache()` to
     * publish.  `NativeEngine` overrides this method instead — its
     * function pointers ARE atomic-safe to read, so no cache needed.
     *
     * Subclasses MAY override to provide faster lookups (NativeEngine
     * pattern) but the override MUST also be thread-safe — touching
     * py::object / lua_State here is a contract violation.
     *
     * @see HEP-CORE-0011 §"Engine Thread Affinity"
     * @see docs/tech_draft/engine_callback_tiers.md (Tier 1)
     */
    [[nodiscard]] virtual bool has_callback(const std::string &name) const noexcept
    {
        if (name.empty())
            return false;
        if (standard_callback_cache_ready_.load(std::memory_order_acquire))
        {
            // Map is read-only after the flag flips (publish-once
            // pattern); safe to read concurrently from any thread
            // without a lock.
            auto it = standard_callback_presence_.find(name);
            if (it != standard_callback_presence_.end())
                return it->second;
        }
        // Name not in the standard cache.  Defer to the subclass
        // hook for arbitrary-name probing.  The default implementation
        // returns false (safe across all threads); subclasses MAY
        // override IFF they can probe safely — typically by guarding
        // on the calling thread being the owner thread (where the
        // language-runtime lock is held), and returning false from any
        // other thread.
        return probe_uncached_callback_(name);
    }

    // ── Schema / type building ───────────────────────────────────────────

    /**
     * @brief Register a slot type from SchemaSpec.
     *
     * `type_name` MUST be one of the five canonical frame names:
     *   "InSlotFrame"  — input slot (readonly)
     *   "OutSlotFrame" — output slot (writable)
     *   "InFlexFrame"  — input flexzone (mutable, HEP-0002)
     *   "OutFlexFrame" — output flexzone (mutable)
     *   "InboxFrame"   — inbox (readonly; uses from_buffer_copy for Python)
     *
     * These correspond to the library's fixed role-frame contract with
     * the role host.  Any other `type_name` is a programming error and
     * MUST be rejected by implementations (return false + LOGGER_ERROR).
     * There is no user-extensible type namespace — scripts and role
     * hosts work only with these canonical frames.
     *
     * Re-registration under the SAME canonical name is allowed — it
     * overwrites the previous cached type with a new schema/packing.
     * This is primarily a test-side convenience (e.g. verifying that
     * the packing argument affects layout by re-registering with
     * different packings); production role hosts call
     * register_slot_type once per canonical name at startup.
     *
     * Engines create "SlotFrame"/"FlexFrame" aliases for single-
     * direction roles (producer/consumer) at build_api() time.
     * Processor has both directions, so no bare aliases.
     *
     * All registered types MUST be cached by the engine for hot-path
     * use.  invoke_on_inbox() requires the cached InboxFrame — null
     * cache is an error.
     *
     * @param spec      The schema specification.
     * @param type_name MUST be one of the five canonical names above.
     * @param packing   "aligned" or "packed".
     * @return true on success; false on validation / build failure
     *         or unknown type_name.
     */
    virtual bool register_slot_type(const hub::SchemaSpec &spec, const std::string &type_name,
                                    const std::string &packing) = 0;

    /**
     * @brief Query the size of a registered type.
     * @return Size in bytes, or 0 on error.
     */
    [[nodiscard]] virtual size_t type_sizeof(const std::string &type_name) const = 0;

    // ── Generic invoke (thread-safe — engine handles internal locking) ────

    /**
     * @brief Invoke a named script function with no arguments.
     *
     * Safe to call from any thread. The engine handles all internal locking,
     * queue management, and thread dispatch. Owner thread gets priority.
     * @return false if function not found, engine shutting down, or error.
     */
    virtual bool invoke(const std::string &name) = 0;

    /**
     * @brief Invoke a named script function with JSON arguments.
     *
     * The engine unpacks args into the script's native format (Python dict,
     * Lua table) before calling. Same thread-safety contract as invoke(name).
     */
    virtual bool invoke(const std::string &name, const nlohmann::json &args) = 0;

    /**
     * @brief Evaluate a script code string. For admin/debug use.
     *
     * Same thread-safety contract as invoke(). Returns InvokeResponse with
     * status (Ok/ScriptError/EngineShutdown) and value (JSON result or empty).
     */
    virtual InvokeResponse eval(const std::string &code) = 0;

    /**
     * @brief Invoke a named callback and capture its return value.
     *
     * Like @ref invoke but populates @ref InvokeResponse::value with the
     * (JSON-converted) return value of the script function.  HEP-CORE-0033
     * §12.2.2 augmentation hooks use this: the script callback receives
     * the prepared response (mutable dict / table) and returns the
     * (possibly mutated) response — C++ ships whatever comes back.
     *
     * Threading: same contract as @ref invoke — owner thread runs inline,
     * non-owner threads queue to the owner's pending drain (Python:
     * existing request_queue_; Lua: same shape, hub-side single-state
     * pending queue) and block on a future until the owner drains.
     *
     * Cross-thread callers MUST call from a thread that is allowed to
     * block — the admin/broker thread paths in HEP-CORE-0033 §12.4 do
     * this between recv'ing a request and replying.
     *
     * @param name Function name (must be defined as a script-side
     *             callable; missing function returns NotFound, value
     *             is null).
     * @param args JSON args, unpacked as the engine's native call shape
     *             (Python: keyword args; Lua: single table arg).
     * @param timeout_ms  Cross-thread wait bound (project convention:
     *             -1 = infinite, 0 = non-blocking, >0 = wait N ms).
     *             Owner-thread direct execution ignores it (no future
     *             involved).  On timeout the response is
     *             @ref InvokeStatus::TimedOut with empty value; the
     *             worker may still complete the callback later
     *             (its result lands in the now-detached future).
     *             Hub usage typically passes the script-tunable
     *             timeout from `HubAPI::augment_timeout_ms()`.
     * @return Status + the function's return value as JSON.  Empty value
     *         (not error) when the function returns None / nil.
     */
    virtual InvokeResponse invoke_returning(const std::string &name, const nlohmann::json &args,
                                            int64_t timeout_ms = -1) = 0;

    /**
     * @brief Drain pending cross-thread invoke / eval / invoke_returning
     *        requests on the owner thread.
     *
     * The hub's @ref HubScriptRunner main loop calls this between the
     * event-drain phase and the tick gate — see HEP-CORE-0033 §12.4
     * loop body.  Engines that don't queue cross-thread work (no
     * implementations today, but the default protects future engines
     * that route everything through a sync API) override this with a
     * no-op.
     *
     * Safe to call from the owner thread only; non-owner calls are a
     * no-op.  PythonEngine: drains the existing request_queue_ via
     * process_pending_().  LuaEngine: drains the Phase 8c
     * single-state pending queue.
     */
    virtual void process_pending() {}

    /**
     * @brief Instantaneous depth of the engine's admin dispatch queue.
     *
     * Status probe (not a metric — no accumulated statistics attached).
     * Non-owner-thread calls to invoke() / eval() on script engines that
     * dispatch synchronously to a single owner thread (e.g. PythonEngine,
     * which holds the GIL on the owner and queues for off-thread callers)
     * are enqueued and drained when the owner next reaches
     * process_pending_() on a hot-path invoke. This returns the number
     * of requests currently awaiting such dispatch.
     *
     * ## What "pending" counts
     *
     * This reflects the **pre-drain backlog** — requests that have been
     * pushed onto the engine's queue but have not yet been pulled into
     * an in-flight drain batch.  It is NOT the count of requests that
     * are executing or in-flight on the owner thread.
     *
     * PythonEngine's process_pending_() swaps the whole queue into a
     * local deque under queue_mu_ (one-shot) and then drains the local
     * copy without holding the lock — so while a drain is in progress
     * this accessor returns 0 even though those requests are still
     * executing.  The honest meaning is "work not yet started."  A
     * separate "in-flight" accessor would be needed for "work not yet
     * finished"; this is not that.
     *
     * Engines that do NOT use an admin dispatch queue (LuaEngine, with
     * supports_multi_state() == true, uses per-thread lua_State and
     * dispatches directly; native C plugins are synchronous) return 0.
     * A zero return therefore can mean "no pre-drain backlog", "drain
     * in progress", OR "this engine does not use a dispatch queue" —
     * the caller should treat 0 as "no backlog to worry about"
     * regardless of which case applies.
     *
     * If a user wants this value recorded as a time-series metric, they
     * may sample it from a script and emit via api.report_metric(...) —
     * this accessor is deliberately not wired into snapshot_metrics_json().
     *
     * Thread-safe.
     */
    virtual size_t pending_script_engine_request_count() const noexcept { return 0; }

    // ── Hot-path invocation (owner thread — data loop) ──────────────────

    /**
     * @brief Loop-ready status returned by `invoke_on_init` — the value
     *        the script's `on_init` produces, coerced by each engine's
     *        binding per HEP-CORE-0011 §"Loop-ready gate" return-value
     *        table.  Composed with the per-role framework default via
     *        AND inside `run_data_loop`; the loop enters `acquire()`
     *        only when both sides return `Ready`.  A script that does
     *        not define `on_init`, or returns `void` / `None` / `nil`,
     *        is treated as `Ready` (back-compat).
     */
    enum class InitStatus : uint8_t
    {
        NotReady = 0,
        Ready = 1,
    };

    virtual InitStatus invoke_on_init() = 0;
    virtual void invoke_on_stop() = 0;

    /**
     * @brief Invoke on_channel_closing(channel, reason, api) if the
     *        script defines it.
     *
     * Optional script-side callback (HEP-CORE-0011 callback table).
     * The role host calls this from the data loop when a
     * `CHANNEL_CLOSING_NOTIFY` arrives in the drained per-cycle
     * messages list AND the script has defined the callback (probed
     * via `has_callback("on_channel_closing")`).  When the callback
     * fires, the corresponding entry is consumed (removed from the
     * messages list before `on_produce`/`on_consume` runs).
     *
     * If the script does NOT define this callback, the role host
     * leaves the notify in the messages list — the script may still
     * react by scanning `msgs` inside `on_produce`/`on_consume`.
     * Either path is supported; the dedicated callback is the
     * ergonomic option.
     *
     * Same threading contract as `invoke_on_stop` — fires on the
     * owner (worker) thread; the engine handles internal locking.
     * Implementations log + suppress script exceptions (do not
     * propagate); the data loop continues.
     *
     * @param channel  Closed channel's name.
     * @param reason   Reason string from the wire payload
     *                 (e.g. "producer_deregistered",
     *                 "pending_timeout", "script_requested").
     */
    virtual void invoke_on_channel_closing(const std::string &channel,
                                           const std::string &reason) = 0;

    /**
     * @brief Invoke on_consumer_died(channel, consumer_uid, reason, api)
     *        if the script defines it.
     *
     * Optional script-side callback (HEP-CORE-0011 callback table).
     * The role host calls this from the data loop when a
     * `CONSUMER_DIED_NOTIFY` arrives in the drained per-cycle messages
     * list AND the script has defined the callback (probed via
     * `has_callback("on_consumer_died")`).  Single-delivery contract:
     * when the callback fires, the entry is consumed (removed from
     * the messages list before `on_produce`/`on_consume` runs);
     * scripts that prefer to scan `msgs` won't see the same notify
     * twice.
     *
     * If the script does NOT define this callback, the role host
     * leaves the notify in `msgs` — the script may still scan it
     * inside `on_produce`/`on_consume`.
     *
     * Same threading contract as `invoke_on_channel_closing`.
     * Implementations log + suppress script exceptions (do not
     * propagate); the data loop continues.
     *
     * Sent by the broker (HEP-CORE-0023 §2.1) when a consumer
     * presence transitions Pending→Disconnected on the heartbeat
     * FSM — `reason="heartbeat_timeout"`, now the sole
     * consumer-liveness mechanism.  Producer scripts receive this
     * callback so
     * they can drop per-consumer bookkeeping (open inbox slots,
     * addressed-message state) symmetrically.
     *
     * @param channel       Channel that the dead consumer was
     *                      registered on.
     * @param consumer_uid  UID of the consumer presence that died.
     * @param reason        Reason string from the wire payload
     *                      ("heartbeat_timeout").
     */
    virtual void invoke_on_consumer_died(const std::string &channel,
                                         const std::string &consumer_uid,
                                         const std::string &reason) = 0;

    /**
     * @brief Invoke `on_hub_dead(source_hub_uid, api)` if the script
     *        defines it.
     *
     * Audit D1 (2026-05-18) — uniform "callback replaces default
     * `api.stop()`" pattern.  The role-side ctrl-thread `on_hub_dead`
     * lambda (`role_api_base.cpp` Phase 2) enqueues a synthetic
     * HUB_DEAD `IncomingMessage` when ZMTP declares the broker dead.
     * The worker-thread dispatcher (`dispatch_notifications` in
     * `cycle_ops.hpp`) routes it here.  Per the design call locked
     * on 2026-05-15T03:29:
     *
     *   - If the script defines `on_hub_dead`, it REPLACES the
     *     framework's default `api.stop()` action.  The script may
     *     call `api.stop()` itself, keep the role alive with
     *     internal state, attempt reconnection logic on later
     *     iterations, or do nothing.
     *   - If NOT defined, the framework calls `core.request_stop()`
     *     (equivalent to `api.stop()`) so the role exits cleanly
     *     instead of zombieing on a dead broker.
     *
     * Same threading contract as `invoke_on_channel_closing` — fires
     * on the worker thread; engine handles internal locking;
     * implementations log + suppress script exceptions (do not
     * propagate); the data loop continues.
     *
     * @param source_hub_uid  Broker endpoint of the dead hub
     *                        (the role's stable identifier for
     *                        this connection per HEP-CORE-0033
     *                        §19.2 — `(broker_endpoint,
     *                        broker_pubkey)` dedup key; endpoint
     *                        alone is unique among a role's
     *                        connections).
     */
    virtual void invoke_on_hub_dead(const std::string &source_hub_uid) = 0;

    /**
     * @brief Invoke on_band_member_joined(band, role_uid, role_name, api)
     *        for `BAND_JOIN_NOTIFY` (HEP-CORE-0030 §5.3).
     *
     * A peer role joined a band this role is a member of.  Fire-and-
     * observe — scripts may collect/log the event but the framework
     * does no automatic state change beyond ensuring the message is
     * removed from `msgs` once dispatched.
     *
     * Same threading contract as `invoke_on_channel_closing`.
     *
     * @param band       Wire `band` field (e.g. "!control").
     * @param role_uid   Joining role's `(prod|cons|proc).<name>.<unique>`.
     * @param role_name  Joining role's display name (may be empty).
     */
    virtual void invoke_on_band_member_joined(const std::string &band, const std::string &role_uid,
                                              const std::string &role_name) = 0;

    /**
     * @brief Invoke on_band_member_left(band, role_uid, reason, api)
     *        for `BAND_LEAVE_NOTIFY` (HEP-CORE-0030 §5.3).
     *
     * @param band       Wire `band` field.
     * @param role_uid   Leaving role's uid.
     * @param reason     `"voluntary"` or `"heartbeat_timeout"`
     *                   (HEP-CORE-0023 §2.1 reasons; the wire
     *                   carries the broker's verdict).
     */
    virtual void invoke_on_band_member_left(const std::string &band, const std::string &role_uid,
                                            const std::string &reason) = 0;

    /**
     * @brief Invoke on_band_message(band, sender_role_uid, body, api)
     *        for `BAND_BROADCAST_DELIVER_NOTIFY` (HEP-CORE-0030 §5.3).
     *
     * @param band             Wire `band` field.
     * @param sender_role_uid  Wire `role_uid` field (the broadcasting
     *                         role's uid; broker enforces sender-must-
     *                         be-member, so the role IS in the band
     *                         from the broker's view at emission).
     * @param body             Application JSON (passthrough).
     */
    virtual void invoke_on_band_message(const std::string &band, const std::string &sender_role_uid,
                                        const nlohmann::json &body) = 0;

    /**
     * @brief Invoke on_band_lost(band, reason, api).
     *
     * Synthetic event — the framework enqueues this when the role's
     * band routing is invalidated (currently: hub-dead drops every
     * `band_index_` entry whose Presence is on the dead connection).
     * NOT a wire frame.
     *
     * @param band    Band name whose routing was lost.
     * @param reason  `"hub_dead"` today.  Reserved for future
     *                additions (e.g. `"evicted"` if the broker emits
     *                an unsolicited eviction NOTIFY).
     */
    virtual void invoke_on_band_lost(const std::string &band, const std::string &reason) = 0;

    /**
     * @brief Invoke on_allowlist_changed(channel, allowlist, reason, api)
     *        if the script defines it (HEP-CORE-0036 §I11 + §6.5).
     *
     * The framework calls this AFTER `ZmqQueue::set_peer_allowlist`
     * has returned success — the snapshot the script observes is the
     * same one now installed in the ZAP cache.  Engine-parity surface:
     * the SAME callback name, signature, and shape across Lua, Python,
     * and Native engines.  Scripts cannot mutate the allowlist via
     * this callback or any other API; the framework remains the only
     * mutator (HEP §I11 audit S3 guardrail).
     *
     * Same threading contract as `invoke_on_channel_closing`:
     * worker-thread only; implementations log + suppress script
     * exceptions; data loop continues.  Exceptions raised by the
     * script callback do NOT roll back the cache update — the
     * enforcement state remains correct regardless.
     *
     * @param channel    Channel whose allowlist changed.
     * @param allowlist  Current authoritative list of authorized
     *                   peers as `{role_uid, pubkey}` records (per
     *                   §6.5 GET_CHANNEL_AUTH_ACK.allowlist shape).
     * @param reason     Wire-string reason from the triggering
     *                   `CHANNEL_AUTH_CHANGED_NOTIFY`
     *                   (e.g. "consumer_joined", "consumer_left").
     */
    virtual void invoke_on_allowlist_changed(const std::string &channel,
                                             const std::vector<AllowedPeer> &allowlist,
                                             const std::string &reason) = 0;

    /**
     * @brief Invoke on_produce(tx, msgs, api).
     * @param tx   Output direction (writable slot + flexzone).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error.
     */
    virtual InvokeResult invoke_produce(InvokeTx tx, std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_consume(rx, msgs, api).
     * @param rx   Input direction (read-only slot + flexzone).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error. Currently ignored by the consumer
     *         data loop — reserved for future flow control.
     */
    virtual InvokeResult invoke_consume(InvokeRx rx, std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_process(rx, tx, msgs, api).
     *
     * Each direction pairs a slot with its flexzone. Either flexzone may
     * be nullptr if not configured.
     *
     * @param rx   Input direction (read-only slot + flexzone from upstream).
     * @param tx   Output direction (writable slot + flexzone to downstream).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error.
     */
    virtual InvokeResult invoke_process(InvokeRx rx, InvokeTx tx,
                                        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_inbox(msg, api).
     *
     * @param msg  Inbox message with typed payload, sender UID, and sequence.
     * @return Commit, Discard, or Error. Currently ignored by the inbox loop.
     *
     * ## Invariants
     *
     * - Inbox only exists when inbox_schema is configured (no schemaless inbox).
     * - "InboxFrame" type is registered via register_slot_type() at startup.
     * - Data lifetime: valid only until the next InboxQueue::recv_one() call.
     *   Python must use from_buffer_copy (not from_buffer). Lua ffi.cast is
     *   safe because the callback completes before the next recv_one().
     */
    virtual InvokeResult invoke_on_inbox(InvokeInbox msg) = 0;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] virtual uint64_t script_error_count() const noexcept = 0;

    // ── Threading capability ─────────────────────────────────────────────

    /**
     * @brief True if the engine handles multi-threaded invoke() internally.
     *
     * When true (Lua): non-owner threads get independent thread-local states.
     * When false (Python): non-owner requests are queued to the owner thread.
     * The engine manages threading internally — callers just call invoke().
     */
    [[nodiscard]] virtual bool supports_multi_state() const noexcept = 0;

    /**
     * @brief Should the worker loop release the engine's global
     *        interpreter lock during idle waits?
     *
     * **Default: false.**  Multi-state engines (LuaEngine — independent
     * `lua_State` per thread, no GIL-equivalent) and engines without an
     * interpreter (NativeEngine) MUST keep this false.  Single-state
     * engines (PythonEngine — one shared interpreter, GIL gates every
     * touch) MAY override to return true when the user has set
     * `script.release_global_lock_during_wait = true` in config.
     *
     * **Effect when true:** the worker loop wraps its idle-wait sites
     * (queue I/O wait, deadline sleep, hub event wait) with
     * `EngineGlobalLockRelease` — the GIL is released across the wait
     * and reacquired before any subsequent engine touch.  This lets
     * cooperative sub-threads on the same interpreter (e.g. a Flask
     * server, asyncio event loop, `threading.Thread`) make progress.
     *
     * **Effect when false (default):** the worker holds the GIL for
     * the entire iteration including the idle wait.  Sub-threads on
     * the same Python interpreter are starved.  This is the
     * historical behaviour and the cheapest hot path.
     *
     * **Threading: any thread.**  Read once per worker startup and
     * cached as a local bool in the loop frame; this method is not
     * called per iteration.
     */
    [[nodiscard]] virtual bool release_global_lock_during_wait() const noexcept { return false; }

    /**
     * @brief Capability flag — does this engine support DYNAMIC callback
     *        registration from script code at runtime?
     *
     * **Default: false.**  pylabhub's standard callback set
     * (`on_init`, `on_stop`, `on_produce`, `on_consume`, `on_process`,
     * `on_inbox`, `on_heartbeat`, `augment_*`) is **discovered by
     * `load_script()` for every engine**, regardless of this flag.
     * The standard set covers the cycle ops + lifecycle + hub
     * augmentation hooks, and is fixed by pylabhub's
     * inter-component protocol; growing it is a coordinated change
     * across HEPs.
     *
     * When this flag is true, the engine ALSO exposes a script-side
     * binding (e.g. Python's `api.register_callback(name, fn)`) that
     * lets scripts register arbitrary additional callback names at
     * runtime, plus a generic C++-side invocation surface
     * (`invoke_event(name, args)` / `invoke_query(name, args, timeout)`)
     * for content-agnostic event dispatch.  This is the "Tier 2"
     * extensibility surface described in
     * `docs/tech_draft/engine_callback_tiers.md` (HEP-CORE-0011 §"Engine
     * Thread Affinity" + §"Callback Tiers — Standard vs Dynamic" once
     * promoted).
     *
     * **Currently: every engine returns false — Tier 2 is reserved.**
     * The flag is plumbed now as the public capability marker so callers
     * can already write `if (engine->supports_dynamic_callbacks())` once
     * a real consumer materialises (admin RPC handler with custom
     * commands, hot-loadable script extensions, etc.).  When that
     * happens, the engine that opts in (likely `PythonEngine` first)
     * flips this to `true` and adds the registry + binding surface.
     */
    [[nodiscard]] virtual bool supports_dynamic_callbacks() const noexcept { return false; }

    // ── Script reload (future) ────────────────────────────────────────────

    /**
     * @brief Reload the script from disk without destroying engine state.
     *
     * Re-executes the script file and re-extracts callback function references.
     * The C++ infrastructure (queues, messenger, API table, FFI/ctypes types)
     * is NOT rebuilt — only the script's function bodies are updated.
     *
     * ## Semantics
     *
     * - Called on the working thread, between data loop cycles.
     * - The engine re-loads the same file path used in load_script().
     * - Old callback refs are released; new ones extracted from the re-executed script.
     * - Cached FFI/ctypes type objects remain valid (schema is immutable).
     * - The API table/object remains valid (C++ pointers unchanged).
     * - Script-side global state: preserved or reset depending on what the
     *   new script file does. This is the script author's responsibility.
     *
     * ## Error handling
     *
     * If the new script has syntax errors or is missing the required callback:
     * - Returns false.
     * - The old callbacks remain active (the engine continues with the
     *   previous version of the script).
     * - The error is logged.
     *
     * ## Trigger mechanism (not yet defined)
     *
     * How reload is triggered is a protocol-level decision:
     * - `api.reload_script()` called from within the script itself
     * - External control message via inbox or broker command
     * - File-system watcher detecting script modification
     * - Interactive signal (e.g., SIGUSR1)
     *
     * The trigger mechanism and the protocol for coordinating reload across
     * a pipeline (e.g., reload producer script without disrupting consumers)
     * are deferred to a future HEP.
     *
     * ## Implementation notes
     *
     * - Lua: `loadfile(path) + pcall()` on the existing lua_State.
     *   Overwrites global functions. No state destruction.
     * - Python: `importlib.reload(module)`. Re-executes the module.
     *   Existing py::objects for callbacks are replaced.
     *
     * @return true if reload succeeded, false on error (old script remains active).
     */
    virtual bool reload_script() { return false; } // default: not implemented

    // ── Thread lifecycle ─────────────────────────────────────────────────

    /**
     * @brief Release any thread-local resources for the calling thread.
     *
     * Called when a non-owner thread is done using this engine. The engine
     * cleans up thread-specific state:
     *   - Lua: destroys the thread-local LuaState from the cache
     *   - Python: no-op (single-state, no per-thread resources)
     *   - Native: no-op (native engine manages its own threads)
     *
     * Safe to call from any thread. No-op if the thread has no state.
     */
    virtual void release_thread() {}

  protected:
    // ── Engine-specific overrides (Template Method pattern) ──────────────

    /// Create the interpreter/state. Called by initialize().
    virtual bool init_engine_(const std::string &log_tag, RoleHostCore *core) = 0;

    /// Build engine-specific API (Lua table / Python pybind11). Called by build_api().
    /// Returns true on success. If false, build_api() does NOT set accepting_=true.
    virtual bool build_api_(RoleAPIBase &api) = 0;

    /// Hub-side sibling overload — added in HEP-CORE-0033 Phase 7
    /// Commit A.  No-op default returning `false` so existing role-only
    /// engines (LuaEngine / PythonEngine / NativeEngine) need not change.
    /// Hub-side concrete engines (Phase 7+8) override this to bind the
    /// HubAPI surface (events + tick + state reads) into the script
    /// namespace.
    ///
    /// Default-false rationale: a role-only engine that ever receives a
    /// `build_api(HubAPI &)` call should fail loudly rather than silently
    /// accepting a binding it has no implementation for — `build_api`'s
    /// wrapper sees `false` and does NOT set `accepting_=true`, so any
    /// subsequent invoke is rejected.
    virtual bool build_api_(HubAPI &) { return false; }

    /// Release all script objects, close interpreter. Called by finalize().
    virtual void finalize_engine_() = 0;

    /// Thread that called initialize(). Engines use this to detect whether
    /// invoke() is called from the owner (hot path) or another thread.
    std::thread::id owner_thread_id_;

    // ── Standard callback presence cache (HEP-CORE-0011 §"Engine
    //    Thread Affinity"; Tier 1) ─────────────────────────────────────
    //
    // Populated ONCE during `load_script()` on the worker thread under
    // the runtime's lock (Python GIL, Lua state mutex).  Read by any
    // thread thereafter via `has_callback()` — plain map lookup, no
    // language-runtime access.  After
    // `freeze_standard_callback_cache()` flips the ready flag, the map
    // is read-only for the engine's lifetime; readers see the
    // already-populated map under the release/acquire pair on the
    // ready flag.
    //
    // Subclass usage from `load_script()` (Python: under GIL via
    // PythonGilLease; Lua: under state mutex):
    //
    //     set_standard_callback_present("on_init", is_callable(py_on_init_));
    //     set_standard_callback_present("on_stop", is_callable(py_on_stop_));
    //     ... (the seven cycle/lifecycle hooks + augment_query_* family)
    //     freeze_standard_callback_cache();
    //
    // See `docs/tech_draft/engine_callback_tiers.md` for the standard
    // callback set and the rationale for Tier 1 vs Tier 2.

    /// Set whether callback `name` is present.  Called by subclasses
    /// during `load_script()` BEFORE `freeze_standard_callback_cache`.
    /// Idempotent; safe to call multiple times for the same name (last
    /// write wins).
    void set_standard_callback_present(std::string name, bool present)
    {
        standard_callback_presence_[std::move(name)] = present;
    }

    /// Publish the standard callback cache.  After this returns, every
    /// `has_callback()` reader sees the populated map via release /
    /// acquire ordering on the ready flag.
    void freeze_standard_callback_cache() noexcept
    {
        standard_callback_cache_ready_.store(true, std::memory_order_release);
    }

    /// Reset the cache (used on script reload to re-populate from
    /// scratch).  Subclass calls this BEFORE re-running its
    /// `set_standard_callback_present()` block on a reload path.
    void reset_standard_callback_cache() noexcept
    {
        standard_callback_cache_ready_.store(false, std::memory_order_release);
        standard_callback_presence_.clear();
    }

    /**
     * @brief Subclass hook for `has_callback()` arbitrary-name probing.
     *
     * Called by `has_callback()` when @p name is NOT in the standard
     * callback cache.  Default implementation returns false — safe on
     * any thread.
     *
     * **Subclass override contract:** an override MAY probe the
     * language runtime IFF the calling thread is the engine's owner
     * thread (i.e., currently holds the runtime lock — Python's GIL
     * via `PythonGilLease`, Lua's state mutex via the worker's
     * scope).  Off-owner-thread calls MUST return false.  A typical
     * impl guards on `std::this_thread::get_id() == owner_thread_id_`.
     *
     * This keeps `has_callback()` any-thread-safe (the principle
     * established by HEP-CORE-0011 §"Engine Thread Affinity") while
     * preserving the historical convenience that worker-thread
     * callers can ask about user-defined script functions outside the
     * pre-cached standard set.
     */
    [[nodiscard]] virtual bool probe_uncached_callback_(const std::string &) const noexcept
    {
        return false;
    }

  private:
    std::unordered_map<std::string, bool> standard_callback_presence_;
    std::atomic<bool> standard_callback_cache_ready_{false};

  protected:
    /// Role API — single source of truth for identity, infrastructure, and operations.
    /// Non-owning pointer, set by `build_api(RoleAPIBase&)`.  Owned by role host.
    /// Mutually exclusive with @ref hub_api_ (each engine instance is
    /// bound to exactly one API surface for its lifetime).
    RoleAPIBase *api_{nullptr};

    /// Hub API — script-visible surface for hub-side scripts (HubScriptRunner).
    /// Non-owning pointer, set by `build_api(HubAPI&)`.  Owned by HubScriptRunner.
    /// Null for role-side engines.  Mutually exclusive with @ref api_.
    HubAPI *hub_api_{nullptr};

  private:
    /// Accepting flag — true only between successful build_api() and stop_accepting().
    /// Child engines delegate to owner's flag via owner_engine_ pointer.
    std::atomic<bool> accepting_{false};

    /// Non-null for child engines (thread-local states). Points to the owner
    /// engine that owns the lifecycle. is_accepting() checks owner's flag.
    ScriptEngine *owner_engine_{nullptr};

    EngineState state_{EngineState::Unloaded};
};

/**
 * @brief RAII guard for non-owner thread engine lifecycle.
 *
 * Construct at thread start, destructor calls engine.release_thread()
 * to clean up thread-local resources (Lua thread-state, etc.).
 */
class ThreadEngineGuard
{
  public:
    explicit ThreadEngineGuard(ScriptEngine &engine) : engine_(engine) {}
    ~ThreadEngineGuard() { engine_.release_thread(); }

    ThreadEngineGuard(const ThreadEngineGuard &) = delete;
    ThreadEngineGuard &operator=(const ThreadEngineGuard &) = delete;

  private:
    ScriptEngine &engine_;
};

} // namespace pylabhub::scripting
