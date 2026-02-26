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
#include "utils/connection_policy.hpp"

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

        /// Optional: stable broker CurveZMQ keypair from HubVault.
        /// When both fields are non-empty, the broker uses these keys instead of
        /// generating an ephemeral keypair on every startup. Supply via
        /// HubVault::broker_curve_secret_key() / broker_curve_public_key().
        std::string server_secret_key; ///< Z85 secret key (40 chars). Empty = generate ephemeral.
        std::string server_public_key; ///< Z85 public key (40 chars). Empty = generate ephemeral.

        /// Optional: called from run() after bind() with (bound_endpoint, server_public_key).
        /// Useful for tests using dynamic port assignment (endpoint="tcp://127.0.0.1:0").
        std::function<void(const std::string& bound_endpoint,
                           const std::string& pubkey)> on_ready;

        // ── Connection policy (Phase 3) ─────────────────────────────────────
        /// Hub-wide connection policy. Per-channel overrides in channel_policies take
        /// precedence (first match wins). Defaults to Open (backward compatible).
        ConnectionPolicy            connection_policy{ConnectionPolicy::Open};

        /// Actors allowed to register when policy is Verified.
        /// Also consulted for logging in Tracked/Required modes.
        std::vector<KnownActor>     known_actors;

        /// Per-channel policy overrides (first matching glob wins).
        std::vector<ChannelPolicy>  channel_policies;
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

private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<BrokerServiceImpl> pImpl;
};

} // namespace pylabhub::broker
