#pragma once
/**
 * @file broker_request_comm.hpp
 * @brief BrokerRequestComm — role-to-broker ZMQ DEALER protocol.
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

class PYLABHUB_UTILS_EXPORT BrokerRequestComm
{
  public:
    BrokerRequestComm();
    ~BrokerRequestComm();

    BrokerRequestComm(BrokerRequestComm &&) noexcept;
    BrokerRequestComm &operator=(BrokerRequestComm &&) noexcept;
    BrokerRequestComm(const BrokerRequestComm &) = delete;
    BrokerRequestComm &operator=(const BrokerRequestComm &) = delete;

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

    /// True if libzmq auto-reconnect is DISABLED on the DEALER socket
    /// (`ZMQ_RECONNECT_IVL == -1`).  pylabhub policy is "disconnect
    /// is terminal" (HEP-CORE-0023 §2.5.3) — every BRC sets this at
    /// connect() time.  Exposed for: (a) production diagnostics
    /// (e.g. a `/admin/status` endpoint reporting socket policy);
    /// (b) tests pinning the policy via direct readback.  Returns
    /// false if the dealer socket isn't initialized (pre-connect or
    /// post-disconnect).
    [[nodiscard]] bool reconnect_disabled() const noexcept;

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

    /// Send HEARTBEAT_REQ for a specific (channel, uid, role_type)
    /// presence — see HEP-CORE-0019 §4.1 (Phase 6 wire format) and
    /// HEP-CORE-0033 §18 (Class A — channel-bound) + §19 (per-presence
    /// emission).
    ///
    /// `role_type` is the WIRE value: `"producer"` for a producer-kind
    /// presence's heartbeat, `"consumer"` for a consumer-kind
    /// presence's heartbeat.  A processor role emits two heartbeats
    /// per cycle (post-Phase-6) — one for each presence — each with
    /// its own role_type.  Pre-Phase-6 single-tick installation
    /// (today, before role_host migration M2) sends one heartbeat
    /// with the role_tag-derived role_type; broker-side handler
    /// changes (M1) consume the new fields.
    ///
    /// `metrics` is optional — pass empty json `{}` when nothing
    /// new to report.
    void send_heartbeat(const std::string &channel,
                        const std::string &role_uid,
                        const std::string &role_type,
                        const nlohmann::json &metrics);
    // M1.4 (2026-05-11): `send_metrics_report` retired.  Metrics
    // piggyback on `send_heartbeat(..., metrics)` per HEP-CORE-0019
    // §2.3 Phase 6.
    //
    // Audit O1 (2026-05-17): `send_notify` removed — zero callers
    // anywhere.  The matching CHANNEL_NOTIFY_REQ broker handler is
    // kept for HEP-CORE-0022 federation peer-relay traffic.  See
    // HEP-CORE-0030 §9.1 for the channel-bound family coexistence.
    void send_broadcast(const std::string &target,
                        const std::string &sender_uid,
                        const std::string &msg,
                        const std::string &data);
    void send_checksum_error(const nlohmann::json &report);

    /// Update a producer's endpoint registration on the broker.
    ///
    /// Sync Request/Response per HEP-CORE-0007 §12.2.1 + HEP-CORE-0021
    /// §16.3.  Blocks up to `timeout_ms` waiting for
    /// `ENDPOINT_UPDATE_ACK` (success) or `ERROR` (rejection).  Returns
    /// the broker reply body on either outcome, or `nullopt` on
    /// timeout / transport failure.
    ///
    /// **Caller contract**:
    ///   - reply `status == "success"` → broker has durably updated
    ///     `ProducerEntry.zmq_node_endpoint`; subsequent DISC_REQs
    ///     from any client are guaranteed to observe the new endpoint.
    ///     Safe to proceed (start data flow, become discoverable).
    ///   - reply `status == "error"` → broker rejected (typed `error`
    ///     code, e.g. `NOT_CHANNEL_OWNER`).  Producer must abort
    ///     startup; do NOT proceed to data flow.
    ///   - `nullopt` → broker unreachable, BRC disconnected, or no
    ///     reply within `timeout_ms`.  Treat as transient failure;
    ///     producer must NOT assume the mutation took effect.
    ///
    /// History: was `void` (fire-and-forget) through 2026-05-21.  The
    /// wire ACK existed (`broker_service.cpp:1025`) but the BRC dropped
    /// it — a half-mix that HEP-0007 §12.2.1 now explicitly prohibits.
    /// Switched to sync REQ/REP per the design clarification.
    std::optional<nlohmann::json>
    send_endpoint_update(const std::string &channel,
                         const std::string &endpoint_type,
                         const std::string &endpoint,
                         int timeout_ms = 5000);

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

    // ─── Harmonized return shape (HEP-CORE-0007 §12.3 + Stage 2 contract) ──
    //
    // All request-reply methods return `optional<json>`:
    //   - `nullopt`               — transport failure (timeout, disconnect).
    //   - `result->status=="success"` — broker accepted request; success
    //     body fields per the §12.3 ACK shape.
    //   - `result->status=="error"`  — broker rejected; inspect
    //     `result->error_code` per the §12.4a taxonomy + `result->message`.
    [[nodiscard]] std::optional<nlohmann::json>
    deregister_channel(const std::string &channel, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    deregister_consumer(const std::string &channel, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    query_role_presence(const std::string &uid, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    query_role_info(const std::string &uid, int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    list_channels(int timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    query_shm_info(const std::string &channel, int timeout_ms = 5000);

    // ── Band pub/sub messaging (HEP-CORE-0030) ────────────────────────
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.  Band names are
    // `!`-prefixed identifiers (HEP-CORE-0030 §3 grammar).

    /// Join a band (auto-creates if it doesn't exist). Returns member list.
    [[nodiscard]] std::optional<nlohmann::json>
    band_join(const std::string &band, int timeout_ms = 5000);

    /// Leave a band.  Returns the broker's response body (success or
    /// error per HEP-CORE-0007 §12.3) or `nullopt` on transport failure.
    [[nodiscard]] std::optional<nlohmann::json>
    band_leave(const std::string &band, int timeout_ms = 5000);

    /// Broadcast JSON message to all band members (fire-and-forget).
    void band_broadcast(const std::string &band,
                        const nlohmann::json &body);

    /// Query current band member list.
    [[nodiscard]] std::optional<nlohmann::json>
    band_members(const std::string &band, int timeout_ms = 5000);

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::hub
