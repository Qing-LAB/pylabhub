#pragma once
/**
 * @file broker_service.hpp
 * @brief Central broker service for channel lifecycle management.
 *
 * BrokerService runs the channel discovery hub: producers register channels,
 * consumers discover them via REG/DISC/DEREG messages over a ZMQ ROUTER socket.
 * Channels start in PendingReady state; the first HEARTBEAT_REQ transitions them
 * to Ready. Dead channels (heartbeat timeout) trigger CHANNEL_CLOSING_NOTIFY to
 * registered consumers AND producer, then are removed.
 *
 * Error taxonomy (see docs/IMPLEMENTATION_GUIDANCE.md § Error Taxonomy):
 *   Cat 1 — invariant violations (schema mismatch, heartbeat timeout): log + notify + shutdown.
 *   Cat 2 — application issues (dead consumer, checksum error): notify + configurable policy.
 *
 * All socket I/O is single-threaded (run() loop); only stop() is thread-safe.
 */
#include "pylabhub_utils_export.h"
#include "utils/channel_access_policy.hpp"
#include "utils/timeout_constants.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{
class HubState;
}

namespace pylabhub::broker
{

class BrokerServiceImpl;

/// Policy for Cat 2 slot data checksum errors reported by producer/consumer.
/// See docs/IMPLEMENTATION_GUIDANCE.md § Error Taxonomy.
enum class ChecksumRepairPolicy
{
    None,       ///< Log the report and ignore (default).
    NotifyOnly, ///< Log + forward report to all channel parties via CHANNEL_EVENT_NOTIFY.
    // Repair — deferred; requires WriteAttach-based slot repair path.
};

/// POD snapshot of a single channel, safe to copy across thread boundaries.
struct ChannelSnapshotEntry
{
    std::string name;
    std::string status;               ///< "Ready" | "PendingReady" | "Closing"
    int         consumer_count{0};
    uint64_t    producer_pid{0};
    std::string schema_hash;
    std::string producer_role_name;
    std::string producer_role_uid;
};

/// Thread-safe snapshot of all channels at a point in time.
struct ChannelSnapshot
{
    std::vector<ChannelSnapshotEntry> channels;

    int count_by_status(const std::string& s) const noexcept
    {
        int n = 0;
        for (const auto& ch : channels)
            if (ch.status == s) ++n;
        return n;
    }
};

/// Broker role-state-machine metrics snapshot (HEP-CORE-0023 §2.5).
///
/// Monotonic counters reported by BrokerService::query_role_state_metrics().
/// Useful for admin-shell introspection and tests that need to assert state
/// transitions occurred without racing on wall-clock timing.
struct RoleStateMetrics
{
    uint64_t ready_to_pending_total{0};       ///< Ready -> Pending demotions.
    uint64_t pending_to_deregistered_total{0};///< Pending -> deregistered (+ CLOSING_NOTIFY).
    uint64_t pending_to_ready_total{0};       ///< Pending -> Ready transitions (first heartbeat or recovery).
};

/// Configuration for one outbound federation peer (HEP-CORE-0022).
/// The hub-side composite (`pylabhub::config::HubFederationConfig` per
/// HEP-CORE-0033 §6.4) parses the `federation.peers[]` block and the
/// hub's main wires the resulting list into `BrokerService::Config`
/// once `plh_hub` is built (HEP-0033 §15 Phase 9).
///
/// HEP-CORE-0035 will extend the peer's HUB_PEER_HELLO with an optional
/// `roles[]` array (peer's `(role_uid, role_pubkey)` list) to support
/// `peer_delegated` federation-trust mode; that augmentation is part of
/// HEP-0035 Phase 5, not Phase 1 of HEP-0033.
struct FederationPeer
{
    std::string              hub_uid;          ///< Peer hub UID
    std::string              broker_endpoint;  ///< Peer's broker ROUTER endpoint
    std::string              pubkey_z85;       ///< Z85 CURVE25519 public key; empty = no CURVE
    std::vector<std::string> channels;         ///< Channels this hub relays TO the peer
};

/// Argument to `BrokerService::Config::on_processing_error`
/// (HEP-CORE-0033 §9.6).  This struct is **append-only** for ABI
/// stability: future fields may be added at the end; existing fields
/// must not be removed or reordered.
struct ProcessingError
{
    /// msg_type the dispatcher saw, or empty if the failure happened
    /// before the msg_type frame was parsed (S1/S2 errors).
    std::string                msg_type;
    /// One of: "malformed_frame" | "malformed_json" | "unknown_msg_type"
    /// | "exception".  See HEP-CORE-0033 §9.3 for the failure-mode
    /// mapping.
    std::string                error_kind;
    /// Free-form detail: exception what(), parse-error description, etc.
    std::string                detail;
    /// Raw ZMQ ROUTER routing identity bytes when available
    /// (request-reply paths); nullopt for inbound peer-DEALER traffic
    /// or pre-dispatch failures with no identity context.
    std::optional<std::string> peer_identity;
};

class PYLABHUB_UTILS_EXPORT BrokerService
{
public:
    struct Config
    {
        std::string endpoint{"tcp://0.0.0.0:5570"};
        bool use_curve{true};

        // ── Role liveness (HEP-CORE-0023 §2.5) ───────────────────────────
        // Single source of truth: effective timeouts are derived from the
        // heartbeat cadence times the miss-heartbeat counts, unless an explicit
        // override is set (useful for tests and relaxed-tolerance deployments).

        /// Expected client heartbeat cadence. Defaults to ::pylabhub::kDefaultHeartbeatIntervalMs (2 Hz).
        std::chrono::milliseconds heartbeat_interval{::pylabhub::kDefaultHeartbeatIntervalMs};

        /// Ready -> Pending demotion after this many consecutive missed heartbeats.
        uint32_t ready_miss_heartbeats{::pylabhub::kDefaultReadyMissHeartbeats};

        /// Pending -> deregistered + CHANNEL_CLOSING_NOTIFY after this many additional missed heartbeats
        /// (counted from the moment the role entered Pending).
        uint32_t pending_miss_heartbeats{::pylabhub::kDefaultPendingMissHeartbeats};

        /// CHANNEL_CLOSING_NOTIFY -> FORCE_SHUTDOWN grace window, in heartbeats.
        uint32_t grace_heartbeats{::pylabhub::kDefaultGraceHeartbeats};

        /// Optional explicit overrides. When set, the value is used verbatim
        /// (including 0, which is meaningful for `grace_override` = "no grace,
        /// FORCE_SHUTDOWN immediately"). When std::nullopt, the effective
        /// timeout is derived as `heartbeat_interval * <miss_heartbeats>`.
        ///
        /// For `ready_timeout_override` / `pending_timeout_override`, a value
        /// of 0 ms means the timeout check in `check_heartbeat_timeouts()` is
        /// skipped (timeout effectively disabled). Tests that want very fast
        /// reclaim should use a small positive value (e.g. 1 ms), not 0.
        std::optional<std::chrono::milliseconds> ready_timeout_override  {};
        std::optional<std::chrono::milliseconds> pending_timeout_override{};
        std::optional<std::chrono::milliseconds> grace_override          {};

        /// Derived effective timeouts used by the heartbeat check loop.
        /// FLOORED at `heartbeat_interval`: a stuck role must always be
        /// reclaimable — a timeout of zero (skip) would create a permanent
        /// dangling state. Misconfiguration is clamped, not honored.
        [[nodiscard]] std::chrono::milliseconds effective_ready_timeout() const noexcept
        {
            const auto v = ready_timeout_override.value_or(
                heartbeat_interval * ready_miss_heartbeats);
            return (v < heartbeat_interval) ? heartbeat_interval : v;
        }
        [[nodiscard]] std::chrono::milliseconds effective_pending_timeout() const noexcept
        {
            const auto v = pending_timeout_override.value_or(
                heartbeat_interval * pending_miss_heartbeats);
            return (v < heartbeat_interval) ? heartbeat_interval : v;
        }
        /// Grace between CHANNEL_CLOSING_NOTIFY and FORCE_SHUTDOWN on the
        /// voluntary-close path. Zero is allowed here ("immediate FORCE_SHUTDOWN")
        /// because voluntary close starts from a live role — consumers already
        /// had the chance to hear the initial CLOSING_NOTIFY.
        [[nodiscard]] std::chrono::milliseconds effective_grace() const noexcept
        {
            return grace_override.value_or(
                heartbeat_interval * grace_heartbeats);
        }

        /// How often broker checks whether registered consumer PIDs are still alive.
        /// Set to 0 to disable liveness checks entirely.
        std::chrono::seconds consumer_liveness_check_interval{5};

        /// Cat 2 policy: what to do when producer/consumer reports a slot checksum error.
        ChecksumRepairPolicy checksum_repair_policy{ChecksumRepairPolicy::None};

        /// Optional: stable broker CurveZMQ keypair from HubVault.
        /// When both fields are non-empty, the broker uses these keys instead of
        /// generating an ephemeral keypair on every startup. Supply via
        /// HubVault::broker_curve_secret_key() / broker_curve_public_key().
        std::string server_secret_key; ///< Z85 secret key (40 chars). Empty = generate ephemeral.
        std::string server_public_key; ///< Z85 public key (40 chars). Empty = generate ephemeral.

        /// Optional: called from run() after bind() with (bound_endpoint, server_public_key).
        /// Useful for tests using dynamic port assignment (endpoint="tcp://127.0.0.1:0").
        ///
        /// **Lifetime**: run() holds a copy of Config for the duration of the broker loop.
        /// The callback and any objects it captures must outlive the run() call. Typical
        /// pattern: Config is stack-allocated at the call site and run() blocks until
        /// shutdown, so the callback naturally outlives it. Capturing by raw pointer is
        /// safe when the pointed-to object is in the same or outer scope. Capturing by
        /// raw pointer to a heap object that may be destroyed before run() returns is unsafe.
        std::function<void(const std::string& bound_endpoint,
                           const std::string& pubkey)> on_ready;

        // ── Connection policy (Phase 3) ─────────────────────────────────────
        /// Hub-wide connection policy. Per-channel overrides in channel_policies take
        /// precedence (first match wins). Defaults to Open.
        ConnectionPolicy            connection_policy{ConnectionPolicy::Open};

        /// Roles allowed to register when policy is Verified.
        /// Also consulted for logging in Tracked/Required modes.
        std::vector<KnownRole>     known_roles;

        /// Per-channel policy overrides (first matching glob wins).
        std::vector<ChannelPolicy>  channel_policies;

        // ── Schema registry (HEP-CORE-0034) ─────────────────────────────────
        /// Directories to search for hub-global schema JSON files (`*.json`).
        /// When empty, the broker uses
        /// `pylabhub::schema::SchemaLibrary::default_search_dirs()`
        /// (`PYLABHUB_SCHEMA_PATH` env, `~/.pylabhub/schemas`,
        /// `/usr/share/pylabhub/schemas`).  At broker startup, every parsed
        /// entry is translated via `to_hub_schema_record` and inserted into
        /// `HubState.schemas` under `(owner_uid="hub", schema_id)` —
        /// HEP-CORE-0034 §2.4 I2 (single load pipeline).  Set in tests to
        /// an explicit temp directory containing schema fixtures.
        std::vector<std::string>    schema_search_dirs;

        // ── Hub federation (HEP-CORE-0022) ──────────────────────────────────
        /// This hub's UID — included in HUB_PEER_HELLO_ACK and HUB_RELAY_MSG.
        /// Required for federation; empty disables all peer connection/relay.
        std::string                          self_hub_uid;

        /// Outbound peer connections.  The broker creates one DEALER socket per
        /// entry and sends HUB_PEER_HELLO after the CURVE handshake completes.
        std::vector<FederationPeer> peers;

        /// Called (from run() thread) when a peer completes the HELLO handshake.
        /// @param hub_uid  UID reported by the peer in HUB_PEER_HELLO.
        /// **Lifetime**: same as on_ready — callback must outlive run(). See on_ready doc.
        std::function<void(const std::string& hub_uid)> on_hub_connected;

        /// Called (from run() thread) when a peer sends BYE or times out.
        /// @param hub_uid  UID of the disconnecting peer.
        /// **Lifetime**: same as on_ready — callback must outlive run(). See on_ready doc.
        std::function<void(const std::string& hub_uid)> on_hub_disconnected;

        /// Called (from run() thread) when a HUB_TARGETED_MSG arrives for this hub.
        /// @param channel       Context channel name from the message.
        /// @param payload       Raw payload bytes.
        /// @param source_hub_uid UID of the sending hub.
        /// **Lifetime**: same as on_ready — callback must outlive run(). See on_ready doc.
        std::function<void(const std::string& channel,
                           const std::string& payload,
                           const std::string& source_hub_uid)> on_hub_message;

        /// Hook fired when broker message processing hits an error
        /// (HEP-CORE-0033 §9.6).  Opt-in.  Invoked AFTER counter bumps
        /// (so a handler reading HubState::counters() sees fresh state),
        /// synchronously on the broker run() thread.
        ///
        /// **Hook contract:**
        /// - May throw — broker swallows.
        /// - Must be fast / non-blocking — for slow work, enqueue and return.
        /// - May call back into BrokerService / HubState (no broker locks
        ///   are held during invocation).
        ///
        /// **Lifetime**: same as on_ready.
        std::function<void(const ProcessingError&)> on_processing_error;
    };

    explicit BrokerService(Config cfg);
    ~BrokerService();

    BrokerService(const BrokerService&) = delete;
    BrokerService& operator=(const BrokerService&) = delete;

    /**
     * @brief Server public key (Z85-encoded, 40 chars).
     * Logged at startup; clients pass this to BrokerRequestComm for CURVE auth.
     */
    [[nodiscard]] const std::string& server_public_key() const;

    /**
     * @brief Access the hub's `HubState` aggregate (HEP-CORE-0033 §8).
     *
     * Exposes the hub's channel/role/band/peer/shm/counter state to
     * HubAPI / AdminService and tests. The caller receives a const
     * reference; mutation happens exclusively via BrokerService's own
     * handlers (friend-access path). Subscribe to state events via
     * `HubState::subscribe_*` — the subscription API is thread-safe.
     *
     * Lifetime: the `HubState` is owned by the `BrokerServiceImpl` and
     * lives for as long as this BrokerService instance. Callers must
     * ensure their subscription handlers outlive the subscriber (or
     * `unsubscribe()` before destruction).
     *
     * Include `utils/hub_state.hpp` to name the returned type.
     */
    [[nodiscard]] const pylabhub::hub::HubState& hub_state() const;

    /**
     * @brief Main event loop. Blocks until stop() is called.
     * Polls ROUTER socket with 100ms timeout; checks heartbeat timeouts each cycle.
     */
    void run();

    /**
     * @brief Signal the run() loop to exit. Thread-safe.
     */
    void stop();

    /**
     * @brief Returns a JSON string listing all currently active channels.
     *
     * Thread-safe: may be called from any thread while run() is executing.
     * The response is a JSON array; each element has:
     *   "name", "schema_hash", "consumer_count", "producer_pid", "status"
     *
     * Example return value:
     * @code
     * [{"name":"sensor_data","schema_hash":"abc123","consumer_count":2,"producer_pid":1234,"status":"Ready"}]
     * @endcode
     */
    [[nodiscard]] std::string list_channels_json_str() const;

    /**
     * @brief Returns a typed snapshot of all channels.
     *
     * Thread-safe: locks the internal query mutex briefly to copy channel data.
     * Prefer this over list_channels_json_str() when strongly-typed access is needed
     * (e.g. from HubScript tick thread).
     */
    [[nodiscard]] ChannelSnapshot query_channel_snapshot() const;

    /**
     * @brief Thread-safe snapshot of role state-machine counters.
     *        Returns monotonic totals since broker startup.
     */
    [[nodiscard]] RoleStateMetrics query_role_state_metrics() const;

    /**
     * @brief Request that the broker close a channel by name.
     *
     * Thread-safe.  The request is queued and drained during the next poll
     * iteration of the broker run() loop.  The broker will send
     * CHANNEL_CLOSING_NOTIFY to all parties and remove the channel from the
     * registry, exactly as a heartbeat timeout would.
     *
     * @param name  Channel name to close.  Silently ignored if not registered.
     */
    void request_close_channel(const std::string& name);

    /**
     * @brief Broadcast a message to all members of a channel.
     *
     * Thread-safe.  The request is queued and drained during the next poll
     * iteration of the broker run() loop.  The broker will send
     * CHANNEL_BROADCAST_NOTIFY to both producer and all consumers of the
     * named channel, exactly as an incoming CHANNEL_BROADCAST_REQ would.
     *
     * @param channel  Target channel name.
     * @param message  Broadcast message tag (e.g., "start", "stop").
     * @param data     Optional payload string (JSON or plain text).
     */
    void request_broadcast_channel(const std::string& channel,
                                   const std::string& message,
                                   const std::string& data = {});

    /**
     * @brief Query aggregated metrics from the MetricsStore (HEP-CORE-0019).
     *
     * Thread-safe: locks the internal query mutex briefly.
     *
     * @param channel  Channel name (empty = all channels).
     * @return JSON string with the METRICS_ACK-format response.
     */
    [[nodiscard]] std::string query_metrics_json_str(const std::string& channel = {}) const;

    /**
     * @brief Query SHM block topology and DataBlockMetrics for all channels (or one).
     *
     * Thread-safe: locks the query mutex to copy registry data, then reads each
     * SHM segment directly via datablock_get_metrics() — opens read-only, reads
     * the fixed-layout DataBlockMetrics from the shared memory header, closes.
     * No lock on the SHM side is needed; metrics fields are relaxed-atomic reads.
     *
     * @param channel  Channel name (empty = all channels with SHM).
     * @return JSON string: {"status":"success","blocks":[{channel, shm_name,
     *         producer:{pid,uid,name}, consumers:[{pid,uid,name}],
     *         shm_metrics:{...} or null}, ...]}
     */
    [[nodiscard]] std::string collect_shm_info_json(const std::string& channel = {}) const;

    /**
     * @brief Send a hub-targeted message to a direct federation peer (HEP-CORE-0022).
     *
     * Thread-safe. The request is queued and sent during the next broker run() poll
     * iteration.  If the target hub UID is not a known connected peer, the message
     * is silently dropped with a warning log.
     *
     * @param target_hub_uid  UID of the destination hub (must be a direct peer).
     * @param channel         Context channel name (informational, not filtered).
     * @param payload         Raw payload bytes.
     */
    void send_hub_targeted_msg(const std::string& target_hub_uid,
                               const std::string& channel,
                               const std::string& payload);

private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<BrokerServiceImpl> pImpl;
};

} // namespace pylabhub::broker
