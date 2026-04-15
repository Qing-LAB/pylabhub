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
 *   - On destruction (or on explicit join_all()), joins each thread with a
 *     bounded wait. On timeout: ERROR log + detach + continue.
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

    /// @param owner_tag  Identifier for this owner. Becomes the dynamic-module
    ///     name via "ThreadManager:" + owner_tag. Examples: "prod",
    ///     "BRC:PROD-SENSOR-0001", "BrokerService:tcp://*:5555", "Logger".
    /// @param aggregate_shutdown_timeout  Lifecycle-layer ceiling on the
    ///     entire join_all() call. Should be >= the sum of per-thread
    ///     SpawnOptions.join_timeout used via spawn(). Defaults to 2×
    ///     `pylabhub::kMidTimeoutMs` (= 10 s) so a manager with a couple of
    ///     heavyweight threads each taking up to kMidTimeoutMs to drain is
    ///     fully covered before the lifecycle layer's timedShutdown safety
    ///     net kicks in.
    explicit ThreadManager(
        std::string owner_tag,
        std::chrono::milliseconds aggregate_shutdown_timeout
            = std::chrono::milliseconds{2 * pylabhub::kMidTimeoutMs});

    /// Destructor calls join_all() (idempotent) and deregisters the dynamic
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
    /// @return true if spawned; false if join_all() has already run.
    /// Spawn with explicit options.
    bool spawn(const std::string &    name,
               std::function<void()>  body,
               SpawnOptions           opts);

    /// Spawn with default join timeout (5 seconds per-thread).
    bool spawn(const std::string &    name,
               std::function<void()>  body);

    /// Bounded join of all managed threads in reverse spawn order. Each
    /// thread that doesn't exit within its SpawnOptions.join_timeout is
    /// detached with an ERROR log identifying the thread by owner_tag + name.
    /// Idempotent; second and subsequent calls are no-ops.
    void join_all();

    /// Number of threads that have been spawned and not yet joined or
    /// detached (i.e., still tracked as "owned"). Goes to 0 after join_all().
    [[nodiscard]] std::size_t active_count() const;

    /// Diagnostic snapshot. Thread-safe. Used by admin-shell / health-check
    /// endpoints to enumerate every active thread in the process with its
    /// owner and elapsed-since-spawn.
    [[nodiscard]] std::vector<ThreadInfo> snapshot() const;

    /// Owner tag passed at construction.
    [[nodiscard]] const std::string &owner_tag() const noexcept;

    /// Lifecycle module name: "ThreadManager:" + owner_tag.
    [[nodiscard]] std::string module_name() const;

    /// Implementation state. Declared public so the free-function lifecycle
    /// thunks (in thread_manager.cpp) can dispatch against it. Still opaque
    /// — struct definition lives in the .cpp — so callers cannot access
    /// fields directly.
    struct Impl;

  private:
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
