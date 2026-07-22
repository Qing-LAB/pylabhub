#pragma once
/**
 * @file thread_manager.hpp
 * @brief Per-owner bounded-join thread lifecycle utility.
 *
 * See docs/tech_draft/thread_manager_design.md for the full design rationale.
 *
 * ThreadManager is a value-composed utility owned by any component that
 * spawns background threads. Each instance:
 *   - Registers as a dynamic lifecycle module "ThreadManager:" + owner_tag
 *     so process-global teardown goes through LifecycleGuard's existing
 *     topological-sort + timedShutdown safety net.
 *   - Tracks named threads with per-thread join timeouts.
 *   - On destruction (or on explicit drain()), walks the tracked threads in
 *     reverse spawn order (LIFO), joining each with its own bounded wait.
 *     On per-slot timeout: ERROR log + detach + continue to the next slot.
 *
 * ThreadManager does NOT own the stop signal. The owner component keeps its
 * own stop atomic/cv and captures it into the thread body lambda passed to
 * spawn(). ThreadManager handles only the join half of the shutdown.
 */

#include "pylabhub_utils_export.h"
#include "utils/timeout_constants.hpp" // kMidTimeoutMs (join/shutdown defaults)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::utils
{

class PYLABHUB_UTILS_EXPORT ThreadManager
{
  public:
    /// Per-slot context handle passed into the thread body (HEP-CORE-0031
    /// §4.1 Thread Shutdown Contract).  Provides two coexisting APIs the
    /// thread can use to declare when it's inside a pImpl-touching
    /// critical region; the teardown caller uses ThreadManager-side
    /// queries (`wait_for_quiescence`, `wait_for_active_loop_exit`) to
    /// honor the contract before destroying shared resources.
    ///
    /// **Two API families — pick one per thread, do not mix:**
    ///
    /// 1. **Transactional bracket** (default-safe — preferred for
    ///    production critical regions): `with_active_loop(body)` runs
    ///    `body` inside a scope where the slot's `active_loop_depth`
    ///    counter is positive; the counter is incremented on entry
    ///    and decremented on exit (RAII, including throw).  The
    ///    counter naturally supports nested `with_active_loop` calls
    ///    on the same `SlotContext` — depth stays positive as long
    ///    as ANY frame is alive.  If shutdown has been requested for
    ///    this slot before entry, the body is skipped entirely.  A
    ///    thread that never calls `with_active_loop` stays
    ///    `active_loop_depth == 0` forever — quiescence is automatic.
    ///
    ///    Wait this family from the caller via
    ///    `ThreadManager::wait_for_quiescence(timeout)`.
    ///
    /// 2. **Monotonic mark** (handy-case — for one-shot signaling):
    ///    `mark_active_loop_exited()` flips the slot's `active_loop_exited`
    ///    flag true once and forever.  Default-unsafe (defaults to false;
    ///    the spawn wrapper auto-flips at body exit as a safety net).
    ///
    ///    Wait this family from the caller via
    ///    `ThreadManager::wait_for_active_loop_exit(name, timeout)`.
    ///
    /// **Don't mix the two on the same thread.**  Both flags exist
    /// independently per slot; mixing would produce two queues of
    /// observers waiting on different signals.
    ///
    /// SlotContext is non-copyable, non-movable — it lives on the
    /// thread's stack frame inside the spawn wrapper for the body's
    /// duration.
    struct PYLABHUB_UTILS_EXPORT SlotContext
    {
        /// Transactional bracket — see SlotContext docstring above for
        /// semantics.  Body invoked iff the slot's `shutdown_requested`
        /// flag is still false at entry.  Counter bookkeeping is RAII —
        /// `active_loop_depth` is decremented on body return (including
        /// throw), so nested `with_active_loop` calls on the same
        /// SlotContext are safe and the exception propagates to the
        /// spawn wrapper's outer try/catch.
        ///
        /// Inline template to avoid std::function allocation on a hot
        /// shutdown path.
        template <typename F> void with_active_loop(F &&body)
        {
            // Defensive null checks: a SlotContext constructed without
            // the ThreadManager spawn-wrapper wiring (e.g. unit-test
            // harnesses synthesizing one directly) would have null
            // shared_ptrs.  Treat "no wiring" as "treat as shutdown"
            // and skip the body — safer than running it without the
            // per-slot signal/quiescence machinery.  Production spawn
            // always wires both pointers, so this branch never fires
            // there.
            if (!shutdown_requested_ || !active_loop_depth_)
                return;
            if (shutdown_requested_->load(std::memory_order_acquire))
                return;
            // Counter (NOT bool) so nested `with_active_loop` calls on
            // the SAME `SlotContext` are safe: each frame increments on
            // entry and decrements on exit; the depth is positive
            // exactly while ANY frame is alive.  A bool would let the
            // innermost RAII reset clear the flag while an outer frame
            // is still inside its body — a concurrent
            // `wait_for_quiescence` would then mis-report quiescence
            // and the teardown caller could destroy state out from
            // under the outer body.
            active_loop_depth_->fetch_add(1, std::memory_order_acq_rel);
            struct DepthGuard
            {
                std::atomic<int> *d;
                ~DepthGuard() { d->fetch_sub(1, std::memory_order_release); }
            };
            DepthGuard guard{active_loop_depth_.get()};
            std::forward<F>(body)();
        }

        /// Thread-side poll for the per-slot shutdown_requested flag.
        /// Useful inside long-running active loops that want to bail
        /// out promptly without waiting for their own class-level stop
        /// signal to flip.
        [[nodiscard]] bool shutdown_requested() const noexcept;

        /// Monotonic-mark API (HEP-CORE-0031 §4.1; handy-case).  Sets
        /// this slot's `active_loop_exited` flag to true.  Idempotent;
        /// thread-safe.  **Do not mix with `with_active_loop` on the
        /// same thread** — the two APIs track different flags.
        void mark_active_loop_exited() noexcept;

        SlotContext(const SlotContext &) = delete;
        SlotContext &operator=(const SlotContext &) = delete;
        SlotContext(SlotContext &&) = delete;
        SlotContext &operator=(SlotContext &&) = delete;

      private:
        friend class ThreadManager;
        SlotContext(std::shared_ptr<std::atomic<bool>> exited,
                    std::shared_ptr<std::atomic<int>> depth,
                    std::shared_ptr<std::atomic<bool>> shutdown) noexcept
            : active_loop_exited_(std::move(exited)), active_loop_depth_(std::move(depth)),
              shutdown_requested_(std::move(shutdown))
        {
        }
        std::shared_ptr<std::atomic<bool>> active_loop_exited_;
        std::shared_ptr<std::atomic<int>> active_loop_depth_;
        std::shared_ptr<std::atomic<bool>> shutdown_requested_;
    };

    /// Per-thread spawn options.
    struct SpawnOptions
    {
        /// Bounded-join deadline for this individual thread. After this
        /// elapses without the thread exiting, ThreadManager logs ERROR +
        /// detaches.  Defaults to `pylabhub::kMidTimeoutMs` (5 s) — the
        /// canonical tier for heavyweight services (ZMQ pollers, Python
        /// worker threads, inbox ROUTER). Use `pylabhub::kShortTimeoutMs`
        /// for lightweight threads (plain CV-wait loops); use
        /// `pylabhub::kLongTimeoutMs` only for genuine long-running cases.
        std::chrono::milliseconds join_timeout{pylabhub::kMidTimeoutMs};

        /// **Master/peer shutdown ordering (HEP-CORE-0031 §4.2 — added
        /// post-MD1.5 to fix a deterministic UAF in role-host
        /// teardown).**
        ///
        /// Most managers have N peer threads that can be drained in
        /// any order.  Some have ONE thread that provides a
        /// _service_ to the others during their teardown — for role
        /// hosts, the BRC ctrl thread sends DEREG_REQ on behalf of
        /// the worker thread's `do_role_teardown` Step 9a.  If the
        /// ctrl thread exits before the worker is done dereg'ing, the
        /// worker's `do_request` blocks waiting for a reply that
        /// never comes; the worker times out, gets detached, and is
        /// then used-after-free when the owner's `api_.reset()`
        /// destroys the underlying pImpl.
        ///
        /// Setting `is_master = true` on a slot tells ThreadManager
        /// to:
        ///   1. NOT signal it from `request_shutdown_all()` (the
        ///      slot's `shutdown_requested` stays false; peers get
        ///      signaled normally).
        ///   2. In `drain()`: wait for every peer slot's `done` flag
        ///      first (Phase 1).  Only after every peer has fully
        ///      exited does drain set the master's per-slot
        ///      `shutdown_requested` (Phase 2) and wait for the
        ///      master's `done` (Phase 3).  Phase 4 joins everyone.
        ///
        /// The master is THE thread whose lifetime must envelope
        /// every peer's runtime.  At most one master per
        /// ThreadManager.  If none is marked, drain falls back to
        /// the non-orchestrated LIFO path used pre-MD1.5.
        ///
        /// A master thread should use `ctx.shutdown_requested()` as
        /// its exit condition rather than a globally-shared stop
        /// flag — the global flag would fire too early (before peers
        /// are done with the master) and re-introduce the bug class
        /// this option was added to prevent.
        bool is_master{false};
    };

    /// Diagnostic snapshot entry.
    struct ThreadInfo
    {
        std::string name;
        bool alive;                                  ///< joinable() at snapshot time
        std::chrono::steady_clock::duration elapsed; ///< wall time since spawn()
        std::chrono::milliseconds join_timeout;
    };

    /// @param owner_tag  Owner tag / category (e.g., "prod", "cons", "proc",
    ///     "hub", "ZmqQueue", "BrokerService"). Identifies the owner's
    ///     CLASS/ROLE. Must be non-empty.
    /// @param owner_id   Owner instance identifier (e.g., role uid
    ///     "prod.sensor.uid00000001", queue channel name, broker endpoint).
    ///     Identifies the SPECIFIC instance within that tag. Must be non-empty.
    /// @param aggregate_shutdown_timeout  Lifecycle-layer ceiling on the
    ///     entire drain() call. Should be >= the sum of per-thread
    ///     SpawnOptions.join_timeout used via spawn(). Defaults to 2×
    ///     `pylabhub::kMidTimeoutMs` (= 10 s) so a manager with a couple of
    ///     heavyweight threads each taking up to kMidTimeoutMs to drain is
    ///     fully covered before the lifecycle layer's timedShutdown safety
    ///     net kicks in.
    ///
    /// Composed identity: `owner_tag + ":" + owner_id`.
    /// Lifecycle module name: `"ThreadManager:" + owner_tag + ":" + owner_id`.
    ///
    /// Throws std::invalid_argument if either string is empty — forgetting
    /// to provide identity is caught at construction, not runtime access.
    ThreadManager(std::string owner_tag, std::string owner_id,
                  std::chrono::milliseconds aggregate_shutdown_timeout = std::chrono::milliseconds{
                      2 * pylabhub::kMidTimeoutMs});

    /// Destructor calls drain() (idempotent) and deregisters the dynamic
    /// lifecycle module. Safe to call from destructor chains; does not throw.
    ~ThreadManager();

    // Non-copyable, non-movable: fixed identity + lifecycle module binding.
    ThreadManager(const ThreadManager &) = delete;
    ThreadManager &operator=(const ThreadManager &) = delete;
    ThreadManager(ThreadManager &&) = delete;
    ThreadManager &operator=(ThreadManager &&) = delete;

    /// Spawn a named thread. The body lambda MUST periodically check the
    /// caller's stop condition and return when shutdown is requested.
    /// ThreadManager does NOT signal the thread — only joins it.
    ///
    /// @param name  Thread identifier for logs + snapshot. Unique within a
    ///     single owner is recommended but not enforced.
    /// @param body  Thread entry point.
    /// @param opts  Per-thread options (join timeout).
    /// @return true if spawned; false if drain() has already run (the
    ///   manager is in its closing phase and rejects new work).
    /// Spawn with explicit options.  Contract-aware overload — the body
    /// receives a `SlotContext &` it uses to call `mark_active_loop_exited()`
    /// at the moment it leaves its active loop (per HEP-CORE-0031 §4.1).
    bool spawn(const std::string &name, std::function<void(SlotContext &)> body, SpawnOptions opts);

    /// Spawn with default join timeout (5 seconds per-thread).  Contract-
    /// aware overload.
    bool spawn(const std::string &name, std::function<void(SlotContext &)> body);

    /// Backwards-compatible overload — body takes no arguments.  Internally
    /// wraps the body in a SlotContext-ignoring trampoline.  For threads
    /// that do not need to mark active-loop-exit explicitly: the wrapper
    /// marks the flag automatically right before the wrapper exits (so
    /// `is_active_loop_exited` returns true immediately before `done` is
    /// set).  Use the SlotContext overload above if the thread needs to
    /// signal loop exit BEFORE body return (e.g., to let the teardown
    /// caller release resources while the thread does post-loop cleanup).
    bool spawn(const std::string &name, std::function<void()> body, SpawnOptions opts);

    /// Spawn with default join timeout (5 seconds per-thread).  Backwards-
    /// compatible overload.
    bool spawn(const std::string &name, std::function<void()> body);

    /// Drain all managed threads (HEP-CORE-0031 §4.2 — master/peer
    /// orchestration added post-MD1.5).
    ///
    /// **No master designated** (default): per-slot bounded join in
    /// reverse spawn order (LIFO).  For each slot: poll the thread's
    /// own `done` flag with 10 ms granularity up to
    /// SpawnOptions.join_timeout; on expiry, detach with an ERROR log
    /// that identifies the thread by owner + name.
    ///
    /// **Master designated** (a slot was spawned with
    /// `SpawnOptions::is_master = true`): four-phase orchestrated
    /// teardown — see @ref SpawnOptions::is_master for the rationale
    /// (post-MD1.5 fix for the role-host dereg UAF).
    ///   1. Signal every peer (set their `shutdown_requested`); do
    ///      NOT signal the master yet.
    ///   2. Wait for every peer's `done` flag to flip true, bounded
    ///      by each peer's `join_timeout`.  Peers that ALREADY
    ///      received their stop signal via a class-level mechanism
    ///      (e.g. the role-host worker's `bc->stop()` already woke
    ///      ctrl) reach `done` quickly; this phase is the bounded
    ///      wait for the slowest peer.
    ///   3. Signal the master (set its `shutdown_requested`).  The
    ///      master's body — which must consult
    ///      `ctx.shutdown_requested()` as its exit condition —
    ///      observes the signal and returns.
    ///   4. Per-slot bounded join (same algorithm as the no-master
    ///      path), now joining everyone in LIFO.
    ///
    /// Both paths: already-joined or already-detached slots are
    /// no-ops, so the call is naturally idempotent — repeat
    /// invocations walk an empty slot list and return 0.  After the
    /// first call, spawn() refuses new threads (the manager is in
    /// its closing phase).
    ///
    /// @return number of threads that HAD to be detached (timed out).
    ///   A non-zero return value means the shutdown was NOT clean
    ///   and one or more threads are leaked (still running, detached
    ///   from std::thread ownership). Callers — especially tests —
    ///   MUST check this rather than treating a `void` return as
    ///   evidence of clean teardown.  An owner that intends to
    ///   destroy state the detached threads might still be touching
    ///   (e.g. `api_.reset()`) MUST refuse to proceed when this
    ///   return is non-zero.
    std::size_t drain();

    /// Signal + wait + join (and remove) a single managed thread by
    /// name.  Internally calls `request_shutdown(name)` to flip the
    /// slot's per-slot shutdown_requested flag (so a subsequent
    /// `with_active_loop` entry on that thread skips its body), then
    /// runs the same two-stage bounded-join as @ref drain (Stage 1:
    /// wait for `active_loop_depth == 0`; Stage 2: wait for `done ==
    /// true`; detach with ERROR log on timeout).
    ///
    /// The internal per-slot signal does NOT replace class-level
    /// signaling that wakes the thread from blocked operations
    /// (sockets, condition variables, etc.).  Callers whose thread
    /// blocks must still emit their class-level wake-up (e.g.,
    /// `admin_svc->stop()`, `broker_comm->stop()`) before calling
    /// `join_named` — the slot flag alone doesn't unblock a thread
    /// stuck in `zmq_poll`.
    ///
    /// Idempotent: a second call with the same name is a no-op (slot
    /// already removed).  After successful join the named slot is
    /// removed from the tracked list, so a subsequent @ref drain
    /// skips it.
    ///
    /// Use case: dependency-ordered shutdown where one subsystem must
    /// drain before another's lifetime ends.  HEP-CORE-0033 §4.2 step 2
    /// uses this to drain admin BEFORE the script runner destroys
    /// HubAPI.
    ///
    /// @param name Thread identifier passed at @ref spawn time.
    /// @return true if a slot named @p name was found and joined cleanly;
    ///   false if no such slot existed, drain() has already run, or the
    ///   slot was detached after timeout.
    [[nodiscard]] bool join_named(std::string_view name);

    /// Set the per-slot `shutdown_requested` flag for the named slot.
    /// The thread can observe this via `SlotContext::shutdown_requested()`
    /// or `with_active_loop` (which skips its body if the flag is true
    /// at entry).  Does NOT touch the manager-wide `closing` flag —
    /// other threads can still spawn and operate normally.
    ///
    /// Does NOT wake blocked threads (no class-level signal).  Use
    /// with class-level signaling (e.g., wake-up socket) when the
    /// target thread might be in a blocking poll/wait.
    ///
    /// @return true iff a slot named @p name was found and its flag
    ///   was set.  false if no such slot (silent no-op).
    [[nodiscard]] bool request_shutdown(std::string_view name) noexcept;

    /// Manager-wide shutdown signal: sets the `closing` flag (which
    /// then rejects new spawns), AND sets every **peer** slot's
    /// `shutdown_requested` flag.  A slot marked `is_master = true`
    /// at spawn time is intentionally NOT signaled here — see
    /// @ref SpawnOptions::is_master for the rationale.  The master's
    /// shutdown_requested is set later, inside @ref drain, only
    /// after every peer's `done` is true.
    ///
    /// Does NOT wait — callers should follow with
    /// `wait_for_quiescence(timeout)` (to honor every thread's
    /// shutdown contract before destructive teardown) and/or
    /// `drain()` (to join thread bodies).
    ///
    /// @return number of slots whose flag was newly set (peers only).
    std::size_t request_shutdown_all() noexcept;

    /// Wait for every spawned slot (except the calling thread, which
    /// is auto-excluded to avoid self-deadlock) to leave every
    /// `with_active_loop` bracket — i.e., `active_loop_depth == 0`.
    /// Returns the number of slots that did NOT reach quiescence
    /// within the timeout.  Zero means every other thread is outside
    /// its critical region; the caller is free to destroy shared
    /// resources those threads had been touching inside their bracket.
    ///
    /// Threads that never call `with_active_loop` stay
    /// `active_loop_depth == 0` by default and are always counted as
    /// quiescent — they pass this wait instantly.
    ///
    /// Polls in 10 ms granularity; the timeout bound is on wall-clock,
    /// not per-slot, so total wait never exceeds @p timeout regardless
    /// of slot count.
    [[nodiscard]] std::size_t wait_for_quiescence(std::chrono::milliseconds timeout) noexcept;

    /// Query whether the named slot's active-loop-exited flag is set
    /// (per HEP-CORE-0031 §4.1).  Returns true iff a slot with this name
    /// exists AND its thread has called
    /// `SlotContext::mark_active_loop_exited()` (or the spawn wrapper has
    /// done it automatically after body return for old-overload spawns).
    /// Returns false if no such slot exists.  Thread-safe (atomic load).
    [[nodiscard]] bool is_active_loop_exited(std::string_view name) const noexcept;

    /// Block until the named slot's active-loop-exited flag becomes true,
    /// or until the timeout elapses.  Returns true iff the flag was set
    /// within the timeout (success).  Returns false on timeout OR if no
    /// slot with this name exists.  Polls in 10 ms granularity.
    /// Thread-safe; callable from any thread.
    ///
    /// Used by the role-side teardown caller (HEP-CORE-0011 §"Role Host
    /// worker_main_() Steps", Step 12.5) to honor the BRC ctrl thread's
    /// shutdown contract before destroying `broker_comm_`.  Generalises
    /// to any externally-threaded class whose `pImpl` the caller intends
    /// to destroy.
    [[nodiscard]] bool wait_for_active_loop_exit(std::string_view name,
                                                 std::chrono::milliseconds timeout) noexcept;

    /// Count of threads that were detached during the most recent drain()
    /// call (0 if no drain yet or last call was fully clean).
    /// Intended for test assertions and process-exit policy:
    ///
    ///     int main(int argc, char **argv) {
    ///         ... construct roles / services ...
    ///         LifecycleGuard guard(...);
    ///         // guard dtor runs ThreadManager dtors (→ drain).
    ///         return /* aggregate */ leaked_threads > 0 ? 2 : 0;
    ///     }
    [[nodiscard]] std::size_t detached_count_last_drain() const;

    /// Best-effort liveness check on threads detached during the most
    /// recent `drain()`.  Each detached slot's `done` shared_ptr is
    /// retained; this query returns true iff every retained flag has
    /// flipped true (i.e. every runaway body has returned), or when no
    /// thread was detached.  Designed for owners that need to delay
    /// destructive teardown until the runaway threads are gone:
    ///
    ///     // Inside owner's shutdown, after drain():
    ///     if (tm.detached_count_last_drain() > 0) {
    ///         const auto deadline = now() + grace;
    ///         while (!tm.all_detached_done() && now() < deadline)
    ///             sleep_for(50ms);
    ///         if (!tm.all_detached_done()) {
    ///             // grace exceeded — leak rather than UAF.
    ///         }
    ///     }
    ///
    /// Thread-safe; locks the manager mutex for the read.  Cleared at
    /// the start of every `drain()` (the retained list reflects only
    /// the most recent drain's detaches, mirroring
    /// `detached_count_last_drain`).
    [[nodiscard]] bool all_detached_done() const noexcept;

    /// Number of threads that have been spawned and not yet joined or
    /// detached (i.e., still tracked as "owned"). Goes to 0 after drain().
    [[nodiscard]] std::size_t active_count() const;

    /// Diagnostic snapshot. Thread-safe. Used by admin-shell / health-check
    /// endpoints to enumerate every active thread in the process with its
    /// owner and elapsed-since-spawn.
    [[nodiscard]] std::vector<ThreadInfo> snapshot() const;

    /// Owner tag (class/category) passed at construction.
    [[nodiscard]] const std::string &owner_tag() const noexcept;

    /// Owner instance id passed at construction.
    [[nodiscard]] const std::string &owner_id() const noexcept;

    /// Composed identity: `owner_tag + ":" + owner_id`. Used for log
    /// prefixes; matches the second half of module_name().
    [[nodiscard]] const std::string &composed_identity() const noexcept;

    /// Lifecycle module name: `"ThreadManager:" + owner_tag + ":" + owner_id`.
    [[nodiscard]] std::string module_name() const;

    // ── Process-wide unclean-shutdown counter ────────────────────────────
    //
    // Every ThreadManager instance in this process contributes to a single
    // thread-safe counter on detach-timeout. Callers that need to decide
    // "can I report success?" (production main(), gtest event listeners,
    // admin health endpoints) query this.
    //
    // The counter is monotonic and resets via
    // reset_process_detached_count_for_testing() — test fixtures that
    // deliberately exercise the timeout path (e.g., the bounded-join unit
    // test) use this to scope the expected-leak window.

    /// Total threads detached by any ThreadManager in this process since
    /// startup (or last reset). 0 means every ThreadManager that has run
    /// drain() so far completed cleanly.
    [[nodiscard]] static std::size_t process_detached_count() noexcept;

    /// Reset the process-wide detached counter to 0. Intended only for
    /// unit tests that exercise the timeout-detach path deliberately and
    /// then need a clean slate before subsequent tests measure it.
    static void reset_process_detached_count_for_testing() noexcept;

    /// Implementation state. Declared public so the free-function lifecycle
    /// thunks (in thread_manager.cpp) can dispatch against it. Still opaque
    /// — struct definition lives in the .cpp — so callers cannot access
    /// fields directly.
    struct Impl;

  private:
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
