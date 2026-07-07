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
#include "utils/security/key_store.hpp"

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
        /// Hub's CURVE server pubkey (Z85).  REQUIRED per HEP-CORE-0035 §2
        /// (CURVE is unconditional); `connect()` returns false on empty.
        std::string broker_pubkey;
        /// HEP-CORE-0040 §172: the role's CURVE keypair is NOT carried
        /// here — `connect()` reads it on-site from `secure().keys()` by
        /// the name below.  `KeyStore::add_identity_from_z85(keystore_name, ...)`
        /// must have been called (typically by `RoleConfig::load_keypair`
        /// for production, or by the test fixture) before `connect()`
        /// runs; absence → `connect()` returns false.
        ///
        /// Default `kRoleIdentityName` (canonical constant) matches what
        /// `RoleConfig::load_keypair` seeds for production.  L3 tests
        /// that spin up multiple BRC instances under different role
        /// identities override this per-instance to e.g.
        /// `"role.test.uid_a"`.
        std::string keystore_name{pylabhub::utils::security::kRoleIdentityName};
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

    /// HEP-CORE-0007 §12.2.1 shape-conformance observability.
    ///
    /// Returns the number of reply-shape messages (`*_ACK` or `ERROR`)
    /// the BRC received with no matching pending request — i.e. the
    /// broker emitted a reply nobody was waiting for.  A non-zero
    /// reading indicates a shape-contract violation: typically the
    /// broker is sending an `_ACK` for a `_REQ` that the §12.2.1
    /// catalog declares fire-and-forget (or the BRC client sent a
    /// request via `cmd_queue.push` that should have been a sync
    /// `do_request`).  See the receive loop in `broker_request_comm.cpp`.
    ///
    /// Thread-safe (atomic load).  Useful for tests
    /// (`ReqShapeConformance_FireAndForgetReqsHaveNoReply`) and for
    /// production monitoring — a rising counter on a running hub is
    /// a useful diagnostic signal.
    [[nodiscard]] size_t unmatched_replies() const noexcept;

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
    /// with the short_tag-derived role_type; broker-side handler
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

    /// HEP-CORE-0036 §6.5 notify-then-pull — fetch the channel's current
    /// authorized-consumer pubkey allowlist from the broker.  Producer-
    /// side only; broker rejects with `PRODUCER_NOT_AUTHORIZED` if
    /// `role_uid` is not a registered producer of `channel`.
    ///
    /// Returns the broker reply body on either outcome
    /// (status="success" + allowlist[] / status="error" + error_code), or
    /// `nullopt` on transport failure / timeout.  Caller applies the
    /// allowlist via `ZmqQueue::set_peer_allowlist`.
    [[nodiscard]] std::optional<nlohmann::json>
    get_channel_auth(const std::string &channel,
                     const std::string &role_uid,
                     int timeout_ms = 5000);

    /// HEP-CORE-0042 §5.5.2 — producer signals the broker that its
    /// per-connection ZAP cache has been updated to `applied_version`.
    /// The broker advances `confirmed_version[K][P] = max(current,
    /// applied_version)` and drains any pending wait-path attach
    /// requests waiting on this producer (§5.4 step d).
    ///
    /// Called after every successful `apply_master_approval` on the
    /// producer side:
    ///   (1) initial `apply_producer_reg_ack` — applied_version =
    ///       REG_ACK.snapshot_version (§5.5.4).
    ///   (2) NOTIFY-driven pull in `handle_channel_auth_notifies` —
    ///       applied_version = GET_CHANNEL_AUTH_ACK.snapshot_version.
    ///
    /// `instance_id` is the value captured from the producer's most
    /// recent `PRODUCER_REG_ACK` (§5.5.3).  Broker's stale-instance
    /// guard rejects with ERROR / error_code="STALE_INSTANCE" if the
    /// value doesn't match its current `instance[P]` — that's a race
    /// with a concurrent re-REG and safely handled by the next
    /// NOTIFY cycle.
    ///
    /// Returns the broker reply body on either outcome (success:
    /// `{status="ok", channel_name, applied_version}`; stale-instance:
    /// `{status="error", error_code="STALE_INSTANCE", ...}`), or
    /// `nullopt` on transport failure / timeout.  Default timeout
    /// matches `applied_ack_wait_ms` from HEP-0042 §5.6.
    [[nodiscard]] std::optional<nlohmann::json>
    channel_auth_applied(const std::string &channel,
                          const std::string &role_uid,
                          std::uint64_t      applied_version,
                          std::uint64_t      instance_id,
                          int                timeout_ms = 1000);

    /// HEP-CORE-0041 §9 D4 pre-attach broker confirmation.  Producer
    /// asks the broker whether one specific consumer is currently
    /// authorized for `channel` before handing over the SHM capability
    /// fd.  Producer-side only; broker rejects with
    /// `PRODUCER_NOT_AUTHORIZED` if `producer_role_uid` is not a
    /// registered producer of the channel.
    ///
    /// Returns the broker reply body:
    ///   success: `{status="success", channel_name, consumer_pubkey, corr_id}`
    ///   denied : `{status="denied",  channel_name, consumer_pubkey,
    ///              denial_reason, corr_id}`
    ///   error  : `{status="error",   error_code, message, corr_id}`
    /// Returns `nullopt` on transport failure / timeout.
    ///
    /// "denied" is a NORMAL auth outcome (HEP-0041 §9 D4 cached-allowlist
    /// table) — distinct from `status="error"` which indicates a
    /// protocol-level failure.  The caller (substep 1e producer L2
    /// AttachProtocol) uses the distinction to drive the cache-divergence
    /// WARN logic.
    /// HEP-CORE-0042 §5.5.1 + §6.2.1 pre-attach coordination for ZMQ.
    /// Consumer-side counterpart to the SHM `consumer_attach` below —
    /// same coordination principle (broker mediates the attach) but a
    /// distinct wire envelope and dispatch path.  Called once per
    /// declared producer inside `apply_consumer_reg_ack` (§7.1 loop)
    /// BEFORE the consumer dials the ZMQ endpoint; on `status="success"`
    /// the consumer proceeds to `set_producer_peers` with the admitted
    /// uid, on `denied` / `timeout` the uid is excluded from the dial
    /// set.
    ///
    /// Broker semantics:
    ///   - Fast-path (confirmed[K][P] >= channel_version[K]): reply
    ///     immediately with `status="success"`.
    ///   - Wait-path (confirmed[K][P] < channel_version[K]): enqueue
    ///     under §5.4 step 5, fire NOTIFY to P, deferred-reply on P's
    ///     APPLIED_REQ drain / P disconnect / channel close / sweep
    ///     timeout.  The broker's ACK carries §5.6 reason strings on
    ///     denied / timeout — the closed set includes
    ///     `producer_did_not_confirm_within_budget`.
    ///
    /// Return semantics:
    ///   - `optional<json>` reply body on any broker-observed outcome
    ///     (success / denied / timeout — all `status` values are
    ///     terminal from the consumer's perspective).
    ///   - `nullopt` on client-side BRC transport failure or
    ///     BRC-observed timeout.  Per §7.1 the caller MUST synthesize
    ///     a `{status="timeout", reason=
    ///     "producer_did_not_confirm_within_budget"}` body — same
    ///     reason string as the broker-observed timeout drain — to
    ///     keep the reason enum closed to §5.6 taxonomy across both
    ///     failure modes.
    ///
    /// Default timeout matches HEP-0042 §5.6 `attach_ack_wait_ms`
    /// (5000ms).  §5.6 invariant: this MUST be > `producer_apply_wait_ms`
    /// so a broker-observed timeout wins over a client-observed one.
    [[nodiscard]] std::optional<nlohmann::json>
    consumer_attach_zmq(const std::string &channel,
                         const std::string &consumer_role_uid,
                         const std::string &consumer_pubkey,
                         const std::string &producer_role_uid,
                         int                timeout_ms = 5000);

    [[nodiscard]] std::optional<nlohmann::json>
    consumer_attach(const std::string &channel,
                    const std::string &consumer_pubkey,
                    const std::string &consumer_role_uid,
                    const std::string &producer_role_uid,
                    int timeout_ms = 5000);

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
