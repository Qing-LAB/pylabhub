#pragma once
/**
 * @file zmq_poll_loop.hpp
 * @brief ZmqPollLoop — generic ZMQ event loop with inproc wake-up and periodic tasks.
 *
 * Provides a reusable poll loop for any module that owns ZMQ sockets:
 * - broker_request_comm (DEALER to broker)
 * - role_communication_channel (ROUTER/XPUB or DEALER/SUB)
 *
 * Features:
 * - Inproc PAIR wake-up: MonitoredQueue push callback wakes the poll loop
 *   immediately (zero latency vs fixed-interval polling)
 * - Time-based periodic tasks with optional iteration gating
 * - Generalized shutdown predicate
 * - cppzmq throughout for RAII and type safety
 *
 * See docs/tech_draft/broker_and_comm_channel_design.md §5.1.
 */

#include "plh_service.hpp" // LOGGER_INFO, LOGGER_WARN

#include "cppzmq/zmq.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// PeriodicTask — time-based periodic action with optional iteration gate
// ============================================================================

/**
 * @brief Periodic action that fires when its interval elapses.
 *
 * Two modes:
 * - **Iteration-gated** (get_iteration != nullptr): fires only when
 *   iteration_count has advanced AND interval has elapsed. Used for heartbeat:
 *   a stuck script stops iteration → heartbeat stops → broker detects dead.
 * - **Time-only** (get_iteration == nullptr): fires whenever interval elapses,
 *   regardless of iteration progress. Used for metrics reports.
 */
struct PeriodicTask
{
    std::function<void()>                  action;
    std::chrono::milliseconds              interval;
    std::chrono::steady_clock::time_point  last_fired;

    /// Optional iteration gate. When set, task only fires if iteration advanced.
    std::function<uint64_t()>              get_iteration;
    uint64_t                               last_iter{0};

    PeriodicTask(std::function<void()> a, int interval_ms,
                 std::function<uint64_t()> iter_fn = nullptr)
        : action{std::move(a)}
        , interval{interval_ms > 0 ? interval_ms : 2000}
        , last_fired{std::chrono::steady_clock::now() - interval} // fire on first tick
        , get_iteration{std::move(iter_fn)}
    {}

    /// Check and fire. Returns time until next fire (for poll timeout calculation).
    std::chrono::milliseconds tick()
    {
        if (get_iteration)
        {
            uint64_t iter = get_iteration();
            if (iter == last_iter)
            {
                // No iteration progress — skip.
                auto elapsed = std::chrono::steady_clock::now() - last_fired;
                auto remaining = interval -
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return std::max(remaining, std::chrono::milliseconds{0});
            }
            last_iter = iter;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_fired);
        if (elapsed >= interval)
        {
            action();
            last_fired = now;
            return interval;
        }
        return interval - elapsed;
    }
};

// ============================================================================
// ZmqPollEntry — one socket + its dispatch handler
// ============================================================================

/**
 * @brief A non-owning socket reference paired with its POLLIN dispatch callback.
 *
 * Uses zmq::socket_ref (cppzmq non-owning reference) for type safety.
 * The owning module holds the zmq::socket_t; this entry references it.
 */
struct ZmqPollEntry
{
    zmq::socket_ref        socket;
    std::function<void()>  dispatch;
};

// ============================================================================
// ZmqPollLoop — generic ZMQ event loop with wake-up and periodic tasks
// ============================================================================

/**
 * @brief Configurable ZMQ poll loop.
 *
 * The caller constructs this, populates sockets/tasks, then calls run().
 * run() blocks until should_run() returns false.
 *
 * Signal socket: when a MonitoredQueue push fires its on_push_signal callback,
 * the callback sends 1 byte on the write end of an inproc PAIR. The read end
 * is set as signal_socket here, waking the poll loop immediately to drain
 * the command queue.
 */
struct ZmqPollLoop
{
    // --- Shutdown predicate (required) ---
    std::function<bool()> should_run;
    std::string           log_prefix;

    // --- Sockets to poll for POLLIN ---
    std::vector<ZmqPollEntry> sockets;

    // --- Inproc wake-up socket (optional, READ end of PAIR) ---
    zmq::socket_ref signal_socket;

    // --- Command queue drain callback (optional) ---
    // Called after signal_socket is readable. Should drain the MonitoredQueue
    // and send messages on the owned socket(s).
    std::function<void()> drain_commands;

    // --- Periodic tasks ---
    std::vector<PeriodicTask> periodic_tasks;

    ZmqPollLoop(std::function<bool()> run_pred, std::string prefix)
        : should_run{std::move(run_pred)}, log_prefix{std::move(prefix)}
    {}

    /// Execute the poll loop (blocks until should_run() returns false).
    void run()
    {
        // Build pollitem array: data sockets + optional signal socket.
        std::vector<zmq::pollitem_t> items;
        std::vector<std::function<void()> *> dispatchers;

        for (auto &entry : sockets)
        {
            if (!entry.socket)
            {
                continue;
            }
            zmq::pollitem_t pi{};
            pi.socket = entry.socket.handle();
            pi.events = ZMQ_POLLIN;
            items.push_back(pi);
            dispatchers.push_back(&entry.dispatch);
        }

        // Signal socket is always last in the array.
        const int signal_idx = signal_socket
            ? static_cast<int>(items.size())
            : -1;
        if (signal_socket)
        {
            zmq::pollitem_t pi{};
            pi.socket = signal_socket.handle();
            pi.events = ZMQ_POLLIN;
            items.push_back(pi);
        }

        if (items.empty())
        {
            LOGGER_INFO("[{}] no sockets to poll — skipping", log_prefix);
            return;
        }

        const auto nfds = items.size();
        const auto n_data = dispatchers.size();

        LOGGER_INFO("[{}] started (polling {} socket{}, {} periodic task{})",
                    log_prefix, nfds, nfds == 1 ? "" : "s",
                    periodic_tasks.size(),
                    periodic_tasks.size() == 1 ? "" : "s");

        while (should_run())
        {
            // Compute poll timeout from periodic tasks.
            auto timeout = std::chrono::milliseconds{200}; // default cap
            for (auto &task : periodic_tasks)
            {
                auto remaining = task.tick();
                if (remaining < timeout)
                {
                    timeout = std::max(remaining,
                                       std::chrono::milliseconds{1}); // floor 1ms
                }
            }

            try
            {
                zmq::poll(items, timeout);
            }
            catch (const zmq::error_t &e)
            {
                if (e.num() == EINTR)
                {
                    continue;
                }
                LOGGER_WARN("[{}] zmq::poll error: {}", log_prefix, e.what());
                break;
            }

            // Dispatch data sockets.
            for (size_t i = 0; i < n_data; ++i)
            {
                if (items[i].revents & ZMQ_POLLIN)
                {
                    (*dispatchers[i])();
                }
            }

            // Signal socket: drain wake-up bytes, then drain command queue.
            if (signal_idx >= 0 &&
                (items[static_cast<size_t>(signal_idx)].revents & ZMQ_POLLIN))
            {
                // Consume all pending signal bytes.
                zmq::message_t discard;
                while (true)
                {
                    auto result = signal_socket.recv(discard,
                                                     zmq::recv_flags::dontwait);
                    if (!result.has_value())
                    {
                        break;
                    }
                }

                if (drain_commands)
                {
                    drain_commands();
                }
            }
        }

        LOGGER_INFO("[{}] exiting", log_prefix);
    }
};

} // namespace pylabhub::scripting
