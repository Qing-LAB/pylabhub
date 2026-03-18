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

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    std::string producer_actor_name;
    std::string producer_actor_uid;
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

/// Configuration for one outbound federation peer (HEP-CORE-0022).
/// Mirrors pylabhub::HubPeerConfig but lives in broker namespace to avoid pulling in hub_config.hpp.
struct FederationPeer
{
    std::string              hub_uid;          ///< Peer hub UID
    std::string              broker_endpoint;  ///< Peer's broker ROUTER endpoint
    std::string              pubkey_z85;       ///< Z85 CURVE25519 public key; empty = no CURVE
    std::vector<std::string> channels;         ///< Channels this hub relays TO the peer
};

class PYLABHUB_UTILS_EXPORT BrokerService
{
public:
    struct Config
    {
        std::string endpoint{"tcp://0.0.0.0:5570"};
        bool use_curve{true};

        /// Timeout for dead channel detection. A channel that has not sent a
        /// HEARTBEAT_REQ within this window is closed and consumers notified.
        std::chrono::seconds channel_timeout{10};

        /// How often broker checks whether registered consumer PIDs are still alive.
        /// Set to 0 to disable liveness checks entirely.
        std::chrono::seconds consumer_liveness_check_interval{5};

        /// Cat 2 policy: what to do when producer/consumer reports a slot checksum error.
        ChecksumRepairPolicy checksum_repair_policy{ChecksumRepairPolicy::None};

        /// Grace period for graceful channel shutdown (two-tier protocol).
        /// After CHANNEL_CLOSING_NOTIFY is sent, the broker waits this long for
        /// clients to deregister before escalating to FORCE_SHUTDOWN.
        std::chrono::seconds channel_shutdown_grace{5};

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

        /// Actors allowed to register when policy is Verified.
        /// Also consulted for logging in Tracked/Required modes.
        std::vector<KnownActor>     known_actors;

        /// Per-channel policy overrides (first matching glob wins).
        std::vector<ChannelPolicy>  channel_policies;

        // ── Schema registry (HEP-CORE-0016 Phase 3) ────────────────────────
        /// Directories to search for named schema JSON files (*.json).
        /// When empty, the broker uses SchemaLibrary::default_search_dirs()
        /// (PYLABHUB_SCHEMA_PATH env, ~/.pylabhub/schemas, /usr/share/pylabhub/schemas).
        /// Set in tests to an explicit temp directory containing schema fixtures.
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
    };

    explicit BrokerService(Config cfg);
    ~BrokerService();

    BrokerService(const BrokerService&) = delete;
    BrokerService& operator=(const BrokerService&) = delete;

    /**
     * @brief Server public key (Z85-encoded, 40 chars).
     * Logged at startup; clients pass this to Messenger::connect().
     */
    [[nodiscard]] const std::string& server_public_key() const;

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
    [[nodiscard]] std::string query_shm_blocks_json_str(const std::string& channel = {}) const;

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
