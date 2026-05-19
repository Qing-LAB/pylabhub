#pragma once
/**
 * @file role_host_core.hpp
 * @brief RoleHostCore — single source of truth for role state, metrics, and messages.
 *
 * All role hosts, engines, and API classes access state through this class.
 * No raw atomic access outside this class — all reads and writes go through
 * proper methods with correct memory ordering decided in one place.
 *
 * ## Ownership
 * - Created by: role host (member variable)
 * - Passed to: engine via initialize(), API class via constructor
 * - Lifetime: must outlive engine and API class
 *
 * ## Thread safety
 * - Atomic state (shutdown, metrics, errors): safe to read/write from any thread
 * - Message queue: mutex-protected, safe from any thread
 * - Init-time state (script_load_ok, in/out fz_spec, in/out schema_fz_size): set once
 *   during worker_main_ init, then read-only
 */

#include "pylabhub_utils_export.h"
#include "utils/schema_types.hpp"        // hub::SchemaSpec, hub::FieldDef
#include "utils/json_fwd.hpp"           // nlohmann::json (project buffer header)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pylabhub::hub { class InboxClient; }

namespace pylabhub::scripting
{

// Wire-event ID for broker notifications carried by IncomingMessage.
//
// Notifications arrive at the BRC's `on_notification(type, body)`
// callback as wire strings (e.g. "CHANNEL_CLOSING_NOTIFY").  Comparing
// strings inside the per-cycle dispatcher loop is wasteful when the
// vocabulary is fixed and small.  This enum gives every recognised
// notification a stable integer ID, parsed once at the BRC enqueue
// boundary so downstream dispatch is an O(1) integer-indexed lookup.
//
// Naming follows the wire `_NOTIFY` suffix and BRC's `on_notification`
// terminology — distinct from the `Lifecycle*` vocabulary used by
// process/thread lifecycle (HEP-CORE-0001 / 0031, LifecycleGuard,
// BinaryLifecycleEnvironment).  These are broker-emitted async events,
// not lifecycle phases.
//
// Versioning:
//   - Append new IDs at the END (preserve ordinal stability across builds).
//   - `Unknown` (0) is the default for messages whose `event` string is
//     not in the catalog (data messages, untracked notifications,
//     etc.) — the dispatcher leaves them in `msgs` for script-side
//     generic scan.
//   - `Count` is the sentinel; every valid ID is strictly less than
//     `Count`, sizing the dispatcher's handler array.
//
// Adding a notification:
//   1. Append the enum value here (before `Count`).
//   2. Map the wire-string in `parse_notification_id` below.
//   3. Add a handler entry in `cycle_ops.hpp` that calls the matching
//      `engine.invoke_on_X(...)`.
//   4. Add the matching pure virtual on `ScriptEngine`.
enum class NotificationId : std::uint8_t
{
    Unknown        = 0,
    ChannelClosing = 1,   ///< CHANNEL_CLOSING_NOTIFY  (HEP-CORE-0011 §callback table)
    ConsumerDied   = 2,   ///< CONSUMER_DIED_NOTIFY    (HEP-CORE-0023 §2.1.1)
    HubDead        = 3,   ///< Synthetic, enqueued by ctrl-thread `on_hub_dead`
                          ///< lambda when ZMTP declares the broker dead.
                          ///< NOT a wire frame (no broker emits this) — the
                          ///< role-side itself synthesizes it as the
                          ///< delivery vehicle for the connection-loss
                          ///< event into the worker thread's dispatcher
                          ///< (D1 audit, 2026-05-18 — uniform with
                          ///< on_channel_closing default-stop pattern).
    Count                 ///< sentinel — must be last
};

/// Parse a wire-string notification `type` into its `NotificationId`.
/// Unknown / non-notification types map to `NotificationId::Unknown`.
///
/// `HUB_DEAD` is NOT a wire-frame type — it's a synthetic event the
/// role-side enqueues itself.  Parsing the string from `HUB_DEAD` is
/// supported anyway so the dispatcher's table-driven path works
/// uniformly with the wire-sourced notifications above.
[[nodiscard]] constexpr NotificationId
parse_notification_id(std::string_view type) noexcept
{
    if (type == "CHANNEL_CLOSING_NOTIFY") return NotificationId::ChannelClosing;
    if (type == "CONSUMER_DIED_NOTIFY")   return NotificationId::ConsumerDied;
    if (type == "HUB_DEAD")               return NotificationId::HubDead;
    return NotificationId::Unknown;
}

struct IncomingMessage
{
    std::string              event;                          ///< wire string (debug + generic scan path)
    NotificationId                 notification_id{NotificationId::Unknown};   ///< parsed once at BRC enqueue; drives dispatch
    std::string              sender;
    /// HEP-CORE-0023 §7 + HEP-CORE-0033 §18.3+§19.4 — origin tag for
    /// dual-hub message disambiguation.  Populated at the BRC
    /// `on_notification` callback from the connection's
    /// `broker_endpoint` (the role-side stable identifier per
    /// HEP-CORE-0033 §19.2 — `(broker_endpoint, broker_pubkey)` is
    /// the dedup key, so endpoint alone is unique among a role's
    /// connections).  Empty for messages that didn't transit a hub
    /// connection (e.g. inbox-side delivery — but inbox writes a
    /// different code path).  Scripts inspect this when a single
    /// processor's `msgs` list aggregates events from multiple hubs;
    /// single-hub roles see one constant value.
    std::string              source_hub_uid;
    std::vector<std::byte>   data;
    nlohmann::json           details;
};

class PYLABHUB_UTILS_EXPORT RoleHostCore
{
  public:
    // ── Message queue ────────────────────────────────────────────────────

    static constexpr size_t kMaxIncomingQueue = 64;

    void enqueue_message(IncomingMessage msg);
    std::vector<IncomingMessage> drain_messages();
    void notify_incoming() noexcept;
    void wait_for_incoming(int timeout_ms) noexcept;

    // ── External shutdown flag (from main) ────────────────────────────────

    void set_shutdown_flag(std::atomic<bool> *flag) noexcept { g_shutdown_ = flag; }

    // ── Plain state (set once during init, read during loop) ─────────────

    void set_validate_only(bool v) noexcept { validate_only_ = v; }
    [[nodiscard]] bool is_validate_only() const noexcept { return validate_only_; }

    void set_script_load_ok(bool v) noexcept
    {
        script_load_ok_.store(v, std::memory_order_release);
    }
    [[nodiscard]] bool is_script_load_ok() const noexcept
    {
        return script_load_ok_.load(std::memory_order_acquire);
    }

    // ── Directional slot schema + logical size (set once during init) ──────

    void set_in_slot_spec(hub::SchemaSpec spec, size_t logical_size) noexcept
    {
        in_slot_spec_         = std::move(spec);
        in_slot_logical_size_ = logical_size;
    }
    void set_out_slot_spec(hub::SchemaSpec spec, size_t logical_size) noexcept
    {
        out_slot_spec_         = std::move(spec);
        out_slot_logical_size_ = logical_size;
    }
    [[nodiscard]] const hub::SchemaSpec &in_slot_spec()         const noexcept { return in_slot_spec_; }
    [[nodiscard]] const hub::SchemaSpec &out_slot_spec()        const noexcept { return out_slot_spec_; }
    [[nodiscard]] size_t  in_slot_logical_size()  const noexcept { return in_slot_logical_size_; }
    [[nodiscard]] size_t  out_slot_logical_size() const noexcept { return out_slot_logical_size_; }
    [[nodiscard]] bool    has_in_slot()           const noexcept { return in_slot_spec_.has_schema; }
    [[nodiscard]] bool    has_out_slot()          const noexcept { return out_slot_spec_.has_schema; }

    // ── Directional flexzone schema + physical size (set once during init) ──

    void set_in_fz_spec(hub::SchemaSpec spec, size_t fz_size) noexcept
    {
        in_fz_spec_        = std::move(spec);
        in_schema_fz_size_ = fz_size;
    }
    void set_out_fz_spec(hub::SchemaSpec spec, size_t fz_size) noexcept
    {
        out_fz_spec_        = std::move(spec);
        out_schema_fz_size_ = fz_size;
    }
    [[nodiscard]] const hub::SchemaSpec &in_fz_spec()        const noexcept { return in_fz_spec_; }
    [[nodiscard]] const hub::SchemaSpec &out_fz_spec()       const noexcept { return out_fz_spec_; }
    [[nodiscard]] size_t  in_schema_fz_size()  const noexcept { return in_schema_fz_size_; }
    [[nodiscard]] size_t  out_schema_fz_size() const noexcept { return out_schema_fz_size_; }
    [[nodiscard]] bool    has_rx_fz()          const noexcept { return in_fz_spec_.has_schema; }
    [[nodiscard]] bool    has_tx_fz()         const noexcept { return out_fz_spec_.has_schema; }

    // ── Inbox cache (role-level, shared across engine states) ────────────

    struct InboxCacheEntry
    {
        std::shared_ptr<hub::InboxClient> client;
        std::string type_name;     ///< FFI/ctypes type name for the inbox slot.
        size_t      item_size{0};  ///< Size of one inbox slot in bytes.
    };

    /// Get a cached inbox entry, or nullopt if not cached.
    std::optional<InboxCacheEntry> get_inbox_entry(const std::string &target_uid) const;

    /// Atomically check cache → if miss, call creator → store result.
    /// The mutex is held for the entire operation — no TOCTOU race.
    /// Creator returns nullopt on failure (entry not stored).
    using InboxCreator = std::function<std::optional<InboxCacheEntry>()>;
    std::optional<InboxCacheEntry> open_inbox(
        const std::string &target_uid, InboxCreator creator);

    /// Stop and remove all cached inbox clients.
    void clear_inbox_cache();

    // ── Custom metrics (HEP-CORE-0019) ──────────────────────────────────

    /// Store or overwrite a custom metric by key.
    void report_metric(const std::string &key, double value)
    {
        std::lock_guard<std::mutex> lk(custom_metrics_mu_);
        custom_metrics_[key] = value;
    }

    /// Store multiple custom metrics at once.
    void report_metrics(const std::unordered_map<std::string, double> &kv)
    {
        std::lock_guard<std::mutex> lk(custom_metrics_mu_);
        for (auto &[k, v] : kv)
            custom_metrics_[k] = v;
    }

    /// Remove all custom metrics.
    void clear_custom_metrics()
    {
        std::lock_guard<std::mutex> lk(custom_metrics_mu_);
        custom_metrics_.clear();
    }

    /// Thread-safe snapshot of all custom metrics.
    [[nodiscard]] std::unordered_map<std::string, double> custom_metrics_snapshot() const
    {
        std::lock_guard<std::mutex> lk(custom_metrics_mu_);
        return custom_metrics_;
    }

    // ── Shared script data (Lua: C++ map; Python: uses native dict) ────

    using StateValue = std::variant<int64_t, double, bool, std::string>;

    /// Get a state value by key, or nullopt if not found.
    std::optional<StateValue> get_shared_data(const std::string &key) const;

    /// Set a state value. Creates the key if it doesn't exist.
    void set_shared_data(const std::string &key, StateValue value);

    /// Remove a state key. No-op if key doesn't exist.
    void remove_shared_data(const std::string &key);

    /// Clear all state.
    void clear_shared_data();

    // ── Stop reason enum ─────────────────────────────────────────────────

    enum class StopReason : int
    {
        Normal        = 0,
        PeerDead      = 1,
        HubDead       = 2,
        CriticalError = 3,
        ChannelClosed = 4,   ///< Audit D1 (2026-05-18) — framework's
                             ///< default stop when CHANNEL_CLOSING_NOTIFY
                             ///< arrives with no `on_channel_closing`
                             ///< script callback to override.  Set
                             ///< before `request_stop()` in
                             ///< `cycle_ops.hpp::default_channel_closing`.
        ScriptError   = 5,   ///< Audit S2 (2026-05-18) — framework's
                             ///< stop when a script callback raised /
                             ///< errored AND `stop_on_script_error=true`.
                             ///< Distinct from `CriticalError` (which is
                             ///< reserved for `set_critical_error()` —
                             ///< framework-internal unrecoverable or
                             ///< explicit script `api.set_critical_error()`).
                             ///< Mirrors the existing `script_error_count_`
                             ///< counter: codebase has historically treated
                             ///< "script bug" and "framework critical" as
                             ///< distinct categories.  Set before
                             ///< `request_stop()` in `cycle_ops.hpp`
                             ///< Producer/Consumer/Processor invoke_and_commit
                             ///< stop_on_error_ paths, and in each engine's
                             ///< `on_X_error_` helper.
    };

    // ────────────────────────────────────────────────────────────────────
    // Flag contract (V1 audit, 2026-05-18)
    // ────────────────────────────────────────────────────────────────────
    //
    // RoleHostCore carries four atomic flags that callers MUST understand
    // by name, intent, transition timing, and audience.  Past audits
    // surfaced confusion about which flag means "safe to access role
    // state" — the table below makes that explicit so future readers
    // don't have to reverse-engineer it from teardown ordering.
    //
    // | Flag                | Initial | Transitions                                          | One-way? | Who reads it                       | What "true" means                           |
    // |---------------------|---------|------------------------------------------------------|----------|-------------------------------------|---------------------------------------------|
    // | running_threads_    | false   | startup → true; teardown Step 12 → false             | NO       | data loop's `should_continue_loop`  | "worker thread is actively processing"      |
    // | shutdown_requested_ | false   | request_stop() → true; set_critical_error() → true   | YES (1)  | data loop + script callbacks        | "someone has asked the role to stop"        |
    // | critical_error_     | false   | set_critical_error() → true                          | YES (1)  | data loop + diagnostic              | "an unrecoverable error fired StopReason"   |
    // | context_valid_      | true    | stop_handler_threads Phase 3a → false                | YES (1)  | ctrl-thread callbacks (V1 audit)    | "role-side data structures are still alive" |
    //
    // (1) One-way latches by class design: there is no public setter
    //     to flip these back to their initial value.  Reset on a fresh
    //     `RoleHostCore` instance only (via construction).
    //
    // The distinction between `is_running()` and `context_valid()` is
    // the key insight from the V1 audit (2026-05-18):
    //
    //   `is_running()`     — answers: "is the worker thread's data loop
    //                        still iterating?"  Used by the data loop
    //                        itself to decide whether to keep running.
    //                        Transitions during normal teardown's
    //                        Step 12 (well BEFORE handler / BRC
    //                        destruction at Step 13).  Today this
    //                        happens to flip before destructive work,
    //                        but that's an ORDERING ACCIDENT, not an
    //                        encoded contract.  Callers who infer
    //                        "structures may be destroyed" from
    //                        `is_running() == false` are relying on an
    //                        invariant the flag's name does not promise.
    //
    //   `context_valid()`  — answers: "is it safe for a ctrl-thread
    //                        callback (fired asynchronously from a
    //                        broker monitor, periodic task, etc.) to
    //                        dereference role-side state — handler,
    //                        BRCs, presences, band index?"  Transitions
    //                        ONCE, in `stop_handler_threads` Phase 3a,
    //                        between `wait_for_quiescence` (which may
    //                        time out) and the start of destructive
    //                        Phase 4 (`handler_->reset()`, etc.).
    //                        Callbacks gate on `context_valid()` so
    //                        that a slow-waker thread that fires a
    //                        callback AFTER the wait timed out (i.e.,
    //                        when destruction is imminent or has
    //                        begun) sees `false` and bails with a
    //                        WARN log — instead of dereferencing a
    //                        destroyed handler and producing a
    //                        use-after-free crash.
    //
    // Teardown timeline (role_host_lifecycle.cpp + role_api_base.cpp):
    //
    //   Step 12   : core.set_running(false)            ← `is_running()` flips
    //   Step 12.5 : wait_for_quiescence(role-level, 5s)
    //   Step 13   : teardown_infrastructure()
    //               └─ stop_handler_threads():
    //                   Phase 2 : brc.stop() per connection
    //                   Phase 3 : wait_for_quiescence(handler-level, 5s)
    //                   Phase 3a: core.set_context_invalid()  ← `context_valid()` flips
    //                   Phase 4 : stop_connections() + handler_.reset()
    //
    // The flag is flipped as LATE as possible — after both waits — so
    // that callbacks firing during normal teardown still do useful
    // work (DEREG, final state updates, logs).  Only callbacks that
    // race in after Phase 3a — the rare slow-waker case — bail out.
    //
    // What `context_valid()` does NOT promise:
    //   (a) Atomic protection of the check + first dereference.  A
    //       callback that has already passed the gate and is mid-body
    //       when teardown flips the flag will proceed into the
    //       destroyed memory.  The window is microseconds; the
    //       slow-waker scenario is seconds.  Eliminating the window
    //       entirely would require reference-counted ownership of the
    //       context — a larger structural change deferred to a future
    //       audit if the residual risk ever bites.
    //   (b) Safety for threads NOT under ThreadManager.  The flag's
    //       contract assumes callers are spawned threads that observe
    //       the established teardown sequence.  External threads
    //       (signal handlers, third-party callbacks) are out of scope.
    //
    // ── Shutdown / error control ─────────────────────────────────────────
    //
    // All writers use release ordering. The loop condition uses
    // should_exit_loop() which acquires once, then inner checks
    // use relaxed (the acquire at loop top establishes happens-before).

    void request_stop() noexcept
    {
        shutdown_requested_.store(true, std::memory_order_release);
    }

    void set_stop_reason(StopReason r) noexcept
    {
        stop_reason_.store(static_cast<int>(r), std::memory_order_relaxed);
    }

    void set_critical_error() noexcept
    {
        critical_error_.store(true, std::memory_order_release);
        stop_reason_.store(static_cast<int>(StopReason::CriticalError),
                           std::memory_order_relaxed);
        shutdown_requested_.store(true, std::memory_order_release);
    }

    void set_running(bool v) noexcept
    {
        running_threads_.store(v, std::memory_order_release);
    }

    /// V1 audit (2026-05-18) — invalidate the role's callback-safety
    /// beacon.  One-way latch: there is no inverse setter.  Called
    /// exactly once during teardown, in `stop_handler_threads`
    /// Phase 3a, AFTER `wait_for_quiescence` and BEFORE the start of
    /// Phase 4's destructive work (`stop_connections()` +
    /// `handler_.reset()`).  See the class-header "Flag contract"
    /// block for the full transition timeline and rationale.
    ///
    /// Idempotent: calling multiple times is safe (the flag is a
    /// one-way latch).  Release ordering pairs with the
    /// `acquire`-ordered load in `context_valid()` so a callback that
    /// reads `false` observes everything the teardown thread did
    /// before flipping the flag.
    void set_context_invalid() noexcept
    {
        context_valid_.store(false, std::memory_order_release);
    }

    // ── Shutdown / error queries ─────────────────────────────────────────

    [[nodiscard]] bool is_running() const noexcept
    {
        return running_threads_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_shutdown_requested() const noexcept
    {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_critical_error() const noexcept
    {
        return critical_error_.load(std::memory_order_acquire);
    }

    /// V1 audit (2026-05-18) — read the role's callback-safety
    /// beacon.  Returns `true` while the role's data structures
    /// (handler_, BRCs, presences, band index) are valid to access;
    /// returns `false` once teardown has reached Phase 3a in
    /// `stop_handler_threads` (after the quiescence wait, before any
    /// destructive work).
    ///
    /// **EVERY ctrl-thread callback (on_notification, on_hub_dead,
    /// periodic ticks) MUST open with this check** and bail with a
    /// WARN log if it returns `false`.  See class-header "Flag
    /// contract" block for the rationale.  Acquire ordering pairs
    /// with the `release`-ordered store in `set_context_invalid()`.
    [[nodiscard]] bool context_valid() const noexcept
    {
        return context_valid_.load(std::memory_order_acquire);
    }

    /// Check all loop exit conditions in one call.
    /// Used as the data loop's while condition.
    [[nodiscard]] bool should_continue_loop() const noexcept
    {
        return running_threads_.load(std::memory_order_acquire) &&
               !shutdown_requested_.load(std::memory_order_acquire) &&
               !critical_error_.load(std::memory_order_relaxed);
    }

    /// Relaxed check for inner retry loops (happens-before established by
    /// should_continue_loop() at the outer loop top).
    [[nodiscard]] bool should_exit_inner() const noexcept
    {
        return !running_threads_.load(std::memory_order_relaxed) ||
               shutdown_requested_.load(std::memory_order_relaxed) ||
               critical_error_.load(std::memory_order_relaxed);
    }

    /// Check if external signal handler requested process exit.
    [[nodiscard]] bool is_process_exit_requested() const noexcept
    {
        return g_shutdown_ && g_shutdown_->load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string stop_reason_string() const noexcept
    {
        switch (stop_reason_.load(std::memory_order_relaxed))
        {
        case 1: return "peer_dead";
        case 2: return "hub_dead";
        case 3: return "critical_error";
        case 4: return "channel_closed";
        case 5: return "script_error";
        default: return "normal";
        }
    }

    // ── Metric accessors (read) ──────────────────────────────────────────

    /// Slots successfully committed to output queue (producer/processor only).
    [[nodiscard]] uint64_t out_slots_written() const noexcept { return out_slots_written_.load(std::memory_order_relaxed); }
    /// Slots successfully read from input queue (consumer/processor only).
    [[nodiscard]] uint64_t in_slots_received() const noexcept { return in_slots_received_.load(std::memory_order_relaxed); }
    /// Output slots discarded: on_produce returned False, or output acquire failed (producer/processor only).
    [[nodiscard]] uint64_t out_drop_count()    const noexcept { return out_drop_count_.load(std::memory_order_relaxed); }
    /// Script callback errors (all roles): exceptions, None returns, type mismatches.
    [[nodiscard]] uint64_t script_error_count() const noexcept { return script_error_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t iteration_count()   const noexcept { return iteration_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t last_cycle_work_us()  const noexcept { return last_cycle_work_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept { return loop_overrun_count_.load(std::memory_order_relaxed); }
    /// Total queue acquire attempts across all retry_acquire calls (all roles).
    [[nodiscard]] uint64_t acquire_retry_count() const noexcept { return acquire_retry_count_.load(std::memory_order_relaxed); }

    /// Snapshot loop metrics into a caller-provided struct.
    struct LoopMetricsSnapshot
    {
        uint64_t iteration_count{0};
        uint64_t loop_overrun_count{0};
        uint64_t last_cycle_work_us{0};
        uint64_t configured_period_us{0};
        uint64_t acquire_retry_count{0};
    };
    [[nodiscard]] LoopMetricsSnapshot loop_metrics() const noexcept
    {
        return {iteration_count(), loop_overrun_count(), last_cycle_work_us(),
                configured_period_us_.load(std::memory_order_relaxed),
                acquire_retry_count()};
    }

    /// Set loop target period (informational — reported in loop metrics).
    void set_configured_period(uint64_t us) noexcept
    {
        configured_period_us_.store(us, std::memory_order_relaxed);
    }

    // ── Metric mutators (write) ──────────────────────────────────────────

    void inc_out_slots_written() noexcept { out_slots_written_.fetch_add(1, std::memory_order_relaxed); }
    void inc_in_slots_received() noexcept { in_slots_received_.fetch_add(1, std::memory_order_relaxed); }
    void inc_out_drop_count()    noexcept { out_drop_count_.fetch_add(1, std::memory_order_relaxed); }
    void inc_script_error_count() noexcept { script_error_count_.fetch_add(1, std::memory_order_relaxed); }
    void inc_iteration_count()   noexcept { iteration_count_.fetch_add(1, std::memory_order_relaxed); }

    void set_last_cycle_work_us(uint64_t v) noexcept { last_cycle_work_us_.store(v, std::memory_order_relaxed); }
    void inc_loop_overrun()    noexcept { loop_overrun_count_.fetch_add(1, std::memory_order_relaxed); }
    void inc_acquire_retry() noexcept { acquire_retry_count_.fetch_add(1, std::memory_order_relaxed); }

#ifdef PYLABHUB_BUILD_TESTS
    /// Test-only: directly set counter values for test setup.
    /// Production code should use inc_*() only.
    void test_set_out_slots_written(uint64_t v) noexcept { out_slots_written_.store(v, std::memory_order_relaxed); }
    void test_set_in_slots_received(uint64_t v) noexcept { in_slots_received_.store(v, std::memory_order_relaxed); }
    void test_set_out_drop_count(uint64_t v)    noexcept { out_drop_count_.store(v, std::memory_order_relaxed); }
#endif

  private:
    // ── Shutdown / error state ────────────────────────────────────────────
    //
    // See the class-header "Flag contract" block for what each field
    // means, when it transitions, and which callers gate on it.
    std::atomic<bool>     running_threads_{false};   ///< Worker-loop liveness (re-flippable).
    std::atomic<bool>     shutdown_requested_{false}; ///< Stop request received (one-way).
    std::atomic<bool>     critical_error_{false};    ///< Unrecoverable error fired (one-way).
    std::atomic<int>      stop_reason_{0};
    /// V1 audit (2026-05-18) — callback-safety beacon for ctrl-thread
    /// callbacks.  Default `true` (handler/BRCs/presences alive); set
    /// to `false` exactly once in `stop_handler_threads` Phase 3a,
    /// before destructive Phase 4 begins.  See class-header "Flag
    /// contract" + `context_valid()` / `set_context_invalid()` for
    /// the full discipline.
    std::atomic<bool>     context_valid_{true};

    // ── Metrics ───────────────────────────────────────────────────────────
    std::atomic<uint64_t> script_error_count_{0};  ///< Script callback errors (all roles).
    std::atomic<uint64_t> out_slots_written_{0};  ///< Output slots committed (producer/processor).
    std::atomic<uint64_t> in_slots_received_{0};  ///< Input slots consumed (consumer/processor).
    std::atomic<uint64_t> out_drop_count_{0};     ///< Output slots discarded (producer/processor).
    std::atomic<uint64_t> iteration_count_{0};
    std::atomic<uint64_t> last_cycle_work_us_{0};
    std::atomic<uint64_t> loop_overrun_count_{0}; ///< Cycles where now > deadline (set by main loop).
    std::atomic<uint64_t> acquire_retry_count_{0}; ///< Total queue acquire attempts (all retry_acquire calls).
    std::atomic<uint64_t> configured_period_us_{0}; ///< Target loop period (µs). Set at startup from config.

    // ── Message queue ─────────────────────────────────────────────────────
    std::vector<IncomingMessage> incoming_queue_;
    std::mutex                   incoming_mu_;
    std::condition_variable      incoming_cv_;

    // ── External shutdown flag ───────────────────────────────────────────
    std::atomic<bool> *g_shutdown_{nullptr};

    // ── Init-time state (set once, read during loop) ─────────────────────
    bool              validate_only_{false};
    std::atomic<bool> script_load_ok_{false};
    hub::SchemaSpec in_slot_spec_;
    hub::SchemaSpec out_slot_spec_;
    size_t     in_slot_logical_size_{0};
    size_t     out_slot_logical_size_{0};
    hub::SchemaSpec in_fz_spec_;
    hub::SchemaSpec out_fz_spec_;
    size_t     in_schema_fz_size_{0};
    size_t     out_schema_fz_size_{0};

    // ── Inbox cache ──────────────────────────────────────────────────────
    mutable std::unordered_map<std::string, InboxCacheEntry> inbox_cache_;
    mutable std::mutex inbox_cache_mu_;

    // ── Shared script data ──────────────────────────────────────────────
    std::unordered_map<std::string, StateValue> shared_data_;
    mutable std::shared_mutex shared_data_mu_;

    // ── Custom metrics (HEP-CORE-0019) ──────────────────────────────────
    std::unordered_map<std::string, double> custom_metrics_;
    mutable std::mutex custom_metrics_mu_;
};

// ─────────────────────────────────────────────────────────────────────────
// StopRequestor — narrow capability handle exposing ONLY the lifecycle
// stop API.  Used by notification-dispatch defaults
// (`cycle_ops.hpp::default_on_*`) to request role shutdown without
// being handed a full `RoleHostCore &`.
//
// Why this exists (audit D1/D2 design call, 2026-05-18):
//   Defaults need to request stop with a typed StopReason, but they
//   should NOT be able to call arbitrary RoleHostCore methods
//   (`enqueue_message`, `set_running(false)`, metric mutators, …).  A
//   bare `RoleHostCore &` would expose ~30 methods; this exposes ONE.
//   Smaller surface ⇒ less misuse risk ⇒ less re-entrance hazard
//   (no `enqueue_message` access means defaults can't mutate the
//   very `msgs` vector they're being called from).
//
// Thread-safety: this is a pass-through over RoleHostCore atomics
// (`set_stop_reason` + `request_stop`), both safe from any thread.
// The dispatcher fires on the worker thread today; this contract
// holds if a future caller fires from elsewhere too.
class StopRequestor
{
  public:
    explicit StopRequestor(RoleHostCore &core) noexcept : core_(core) {}

    /// Set StopReason then call request_stop().  Matches the
    /// (reason-first, then stop) sequencing in role_api_base.cpp
    /// A2-master path and cycle_ops handlers.
    void request(RoleHostCore::StopReason reason) const noexcept
    {
        core_.set_stop_reason(reason);
        core_.request_stop();
    }

  private:
    RoleHostCore &core_;
};

} // namespace pylabhub::scripting

/**
 * @brief Canonical field list for RoleHostCore::LoopMetricsSnapshot serialization.
 * Same X-macro pattern as PYLABHUB_QUEUE_METRICS_FIELDS (see hub_queue.hpp).
 */
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define PYLABHUB_LOOP_METRICS_FIELDS(X) \
    X(iteration_count)                  \
    X(loop_overrun_count)               \
    X(last_cycle_work_us)               \
    X(configured_period_us)             \
    X(acquire_retry_count)
// NOLINTEND(cppcoreguidelines-macro-usage)
