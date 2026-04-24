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
#include "utils/timeout_constants.hpp"     // kMidTimeoutMs (join/shutdown defaults)

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::utils
{

class PYLABHUB_UTILS_EXPORT ThreadManager
{
  public:
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
    };

    /// Diagnostic snapshot entry.
    struct ThreadInfo
    {
        std::string                          name;
        bool                                 alive;   ///< joinable() at snapshot time
        std::chrono::steady_clock::duration  elapsed; ///< wall time since spawn()
        std::chrono::milliseconds            join_timeout;
    };

    /// @param owner_tag  Owner tag / category (e.g., "prod", "cons", "proc",
    ///     "ZmqQueue", "BrokerService", "hubshell"). Identifies the owner's
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
    ThreadManager(std::string owner_tag,
                  std::string owner_id,
                  std::chrono::milliseconds aggregate_shutdown_timeout
                      = std::chrono::milliseconds{2 * pylabhub::kMidTimeoutMs});

    /// Destructor calls drain() (idempotent) and deregisters the dynamic
    /// lifecycle module. Safe to call from destructor chains; does not throw.
    ~ThreadManager();

    // Non-copyable, non-movable: fixed identity + lifecycle module binding.
    ThreadManager(const ThreadManager &)            = delete;
    ThreadManager &operator=(const ThreadManager &) = delete;
    ThreadManager(ThreadManager &&)                 = delete;
    ThreadManager &operator=(ThreadManager &&)      = delete;

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
    /// Spawn with explicit options.
    bool spawn(const std::string &    name,
               std::function<void()>  body,
               SpawnOptions           opts);

    /// Spawn with default join timeout (5 seconds per-thread).
    bool spawn(const std::string &    name,
               std::function<void()>  body);

    /// Drain all managed threads — per-slot bounded join in reverse spawn
    /// order (LIFO). For each slot: poll the thread's own `done` flag with
    /// 10 ms granularity up to SpawnOptions.join_timeout; on expiry, detach
    /// with an ERROR log that identifies the thread by owner + name. Already-
    /// joined or already-detached slots are no-ops, so the call is naturally
    /// idempotent — repeat invocations walk an empty slot list and return 0.
    /// After the first call, spawn() refuses new threads (the manager is in
    /// its closing phase).
    /// @return number of threads that HAD to be detached (timed out). A
    ///   non-zero return value means the shutdown was NOT clean and one or
    ///   more threads are leaked (still running, detached from std::thread
    ///   ownership). Callers — especially tests — MUST check this rather
    ///   than treating a `void` return as evidence of clean teardown.
    std::size_t drain();

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
