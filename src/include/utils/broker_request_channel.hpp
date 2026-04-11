#pragma once
/**
 * @file broker_request_channel.hpp
 * @brief BrokerRequestChannel — role-to-broker ZMQ DEALER protocol.
 *
 * Clean replacement for the broker protocol portion of Messenger.
 * One DEALER socket connected to the broker's ROUTER. All messages are
 * JSON over a 3-frame wire format: ['C'] [msg_type] [json_body].
 *
 * Thread model: owns one dedicated thread (managed by RoleAPIBase's
 * thread manager) that polls the DEALER socket, drains the command queue,
 * dispatches incoming notifications, and fires periodic heartbeats.
 *
 * API is thread-safe: callers enqueue commands via MonitoredQueue;
 * the broker thread sends them during its poll cycle.
 *
 * See docs/tech_draft/broker_and_comm_channel_design.md §2.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

class PYLABHUB_UTILS_EXPORT BrokerRequestChannel
{
  public:
    BrokerRequestChannel();
    ~BrokerRequestChannel();

    BrokerRequestChannel(BrokerRequestChannel &&) noexcept;
    BrokerRequestChannel &operator=(BrokerRequestChannel &&) noexcept;
    BrokerRequestChannel(const BrokerRequestChannel &) = delete;
    BrokerRequestChannel &operator=(const BrokerRequestChannel &) = delete;

    // ── Configuration ────────────────────────────────────────────────────

    struct Config
    {
        std::string broker_endpoint;
        std::string broker_pubkey;    ///< Z85, empty = plain TCP
        std::string client_pubkey;    ///< Z85, empty = ephemeral keypair
        std::string client_seckey;    ///< Z85
        std::string role_uid;         ///< Role UID for channel join/leave
        std::string role_name;        ///< Role display name
    };

    /// Connect DEALER socket to broker. Must be called before run_poll_loop().
    [[nodiscard]] bool connect(const Config &cfg);

    /// Disconnect and close the DEALER socket.
    void disconnect();

    /// True after successful connect(), false after disconnect().
    [[nodiscard]] bool is_connected() const noexcept;

    // ── Notification callbacks (set before run_poll_loop) ────────────────

    /// Called from the broker thread for each unsolicited broker notification.
    /// The callback receives (msg_type, json_body) and should route to
    /// core_.enqueue_message() or handle directly.
    using NotificationCallback = std::function<void(const std::string &msg_type,
                                                     const nlohmann::json &body)>;
    void on_notification(NotificationCallback cb);

    /// Called when the broker connection is lost (ZMTP heartbeat timeout →
    /// ZMQ_EVENT_DISCONNECTED on the socket monitor).
    void on_hub_dead(std::function<void()> cb);

    // ── Periodic tasks (set before run_poll_loop) ──────────────────────────

    /// Add a periodic task to the broker thread's poll loop.
    /// Call before run_poll_loop(). Tasks fire during each poll cycle.
    /// @param action    Callback to execute periodically.
    /// @param interval_ms  Minimum interval between firings.
    /// @param get_iteration  Optional iteration gate (nullptr = time-only).
    void set_periodic_task(std::function<void()> action,
                           int interval_ms,
                           std::function<uint64_t()> get_iteration = nullptr);

    // ── Poll loop (runs on the broker thread) ────────────────────────────

    /// Blocking poll loop. Call from the thread manager's thread body.
    /// Returns when stop() is called or the shutdown predicate fires.
    void run_poll_loop(std::function<bool()> should_run);

    /// Signal the poll loop to exit. Safe to call from any thread.
    void stop() noexcept;

    // ── Fire-and-forget messages (thread-safe, enqueued) ─────────────────

    void send_heartbeat(const std::string &channel,
                        const nlohmann::json &metrics);
    void send_metrics_report(const std::string &channel,
                             const std::string &uid,
                             const nlohmann::json &metrics);
    void send_notify(const std::string &target,
                     const std::string &sender_uid,
                     const std::string &event,
                     const std::string &data);
    void send_broadcast(const std::string &target,
                        const std::string &sender_uid,
                        const std::string &msg,
                        const std::string &data);
    void send_checksum_error(const nlohmann::json &report);
    void send_endpoint_update(const std::string &channel,
                              const std::string &key,
                              const std::string &endpoint);

    // ── Request-reply (thread-safe, blocks until reply or timeout) ───────
    //
    // These enqueue a request and wait for the broker thread to send it
    // and receive the matching reply. The caller blocks on a condition
    // variable; the broker thread signals it when the reply arrives.

    [[nodiscard]] std::optional<nlohmann::json>
    register_channel(const nlohmann::json &opts, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    discover_channel(const std::string &channel,
                     const nlohmann::json &opts, int timeout_ms = 10000);

    [[nodiscard]] std::optional<nlohmann::json>
    register_consumer(const nlohmann::json &opts, int timeout_ms = 5000);

    [[nodiscard]] bool
    deregister_channel(const std::string &channel, int timeout_ms = 5000);

    [[nodiscard]] bool
    deregister_consumer(const std::string &channel, int timeout_ms = 5000);

    [[nodiscard]] bool
    query_role_presence(const std::string &uid, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    query_role_info(const std::string &uid, int timeout_ms = 5000);

    [[nodiscard]] std::vector<nlohmann::json>
    list_channels(int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    query_shm_info(const std::string &channel, int timeout_ms = 5000);

    // ── Channel pub/sub messaging (HEP-CORE-0030) ───────────────────────

    /// Join a channel (auto-creates if it doesn't exist). Returns member list.
    [[nodiscard]] std::optional<nlohmann::json>
    join_channel(const std::string &channel, int timeout_ms = 5000);

    /// Leave a channel.
    bool leave_channel(const std::string &channel, int timeout_ms = 5000);

    /// Send JSON message to all channel members (fire-and-forget).
    void send_channel_msg(const std::string &channel,
                          const nlohmann::json &body);

    /// Query current channel member list.
    [[nodiscard]] std::optional<nlohmann::json>
    query_channel_members(const std::string &channel, int timeout_ms = 5000);

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::hub
