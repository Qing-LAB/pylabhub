#pragma once
/**
 * @file zmq_poll_loop.hpp
 * @brief ZmqPollLoop — shared ZMQ event loop for role script hosts.
 *
 * Extracts the common poll+dispatch+heartbeat logic from the three role
 * binaries (producer, consumer, processor) into a reusable internal API.
 * Each role's run_zmq_thread_() becomes ~10 lines of setup.
 *
 * Header-only: small (~90 lines), used only by 3 internal .cpp files.
 *
 * See HEP-CORE-0011 §13 (ZMQ Poll Loop Utility).
 * See HEP-CORE-0007 §12.3 (shutdown pitfalls — the bug that motivated this).
 */

#include "role_host_core.hpp"

#include "plh_service.hpp" // LOGGER_WARN

#include <zmq.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// HeartbeatTracker — periodic action driven by iteration progress
// ============================================================================

struct HeartbeatTracker
{
    std::function<void()>                  action;
    std::chrono::milliseconds              interval;
    uint64_t                               last_iter{0};
    std::chrono::steady_clock::time_point  last_sent;

    HeartbeatTracker(std::function<void()> a, int interval_ms)
        : action{std::move(a)}
        , interval{interval_ms > 0 ? interval_ms : 2000}
        , last_sent{std::chrono::steady_clock::now() - interval}
    {}

    /// Call once per poll cycle. Fires action if iteration advanced AND interval elapsed.
    void tick(uint64_t current_iteration)
    {
        if (current_iteration == last_iter)
            return;
        last_iter = current_iteration;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_sent >= interval)
        {
            action();
            last_sent = now;
        }
    }
};

// ============================================================================
// ZmqPollEntry — one socket + its dispatch handler
// ============================================================================

struct ZmqPollEntry
{
    void                  *socket{nullptr};
    std::function<void()>  dispatch;
};

// ============================================================================
// ZmqPollLoop — configurable poll loop object
// ============================================================================

struct ZmqPollLoop
{
    // --- Required (set by constructor) ---
    RoleHostCore &core;
    std::string   log_prefix;

    // --- Sockets to poll ---
    std::vector<ZmqPollEntry> sockets;

    // --- Periodic tasks (driven by iteration progress) ---
    std::function<uint64_t()>    get_iteration;
    std::vector<HeartbeatTracker> periodic_tasks;

    // --- Tuning ---
    int poll_interval_ms{5};

    ZmqPollLoop(RoleHostCore &c, std::string prefix)
        : core{c}, log_prefix{std::move(prefix)}
    {}

    /// Execute the poll loop (blocks until shutdown).
    void run()
    {
        // Filter nullptr sockets, build pollitem array.
        std::vector<zmq_pollitem_t> items;
        std::vector<std::function<void()> *> dispatchers;
        items.reserve(sockets.size());
        dispatchers.reserve(sockets.size());

        for (auto &entry : sockets)
        {
            if (entry.socket == nullptr)
                continue;
            zmq_pollitem_t pi{};
            pi.socket = entry.socket;
            pi.events = ZMQ_POLLIN;
            items.push_back(pi);
            dispatchers.push_back(&entry.dispatch);
        }

        if (items.empty())
        {
            LOGGER_INFO("[{}/zmq_thread] no sockets to poll — skipping", log_prefix);
            return;
        }

        const int nfds = static_cast<int>(items.size());

        LOGGER_INFO("[{}/zmq_thread] started (polling {} socket{}, {} periodic task{})",
                    log_prefix, nfds, nfds == 1 ? "" : "s",
                    periodic_tasks.size(),
                    periodic_tasks.size() == 1 ? "" : "s");

        while (core.running_threads.load(std::memory_order_relaxed) &&
               !core.shutdown_requested.load(std::memory_order_relaxed))
        {
            const int rc = zmq_poll(items.data(), nfds, poll_interval_ms);
            if (rc < 0)
            {
                if (errno == EINTR)
                    continue;
                LOGGER_WARN("[{}/zmq_thread] zmq_poll error: {}", log_prefix,
                            zmq_strerror(errno));
                break;
            }

            if (rc > 0)
            {
                for (int i = 0; i < nfds; ++i)
                {
                    if (items[static_cast<size_t>(i)].revents & ZMQ_POLLIN)
                        (*dispatchers[static_cast<size_t>(i)])();
                }
            }

            if (get_iteration)
            {
                const uint64_t iter = get_iteration();
                for (auto &task : periodic_tasks)
                    task.tick(iter);
            }
        }

        LOGGER_INFO("[{}/zmq_thread] exiting", log_prefix);
    }
};

} // namespace pylabhub::scripting
