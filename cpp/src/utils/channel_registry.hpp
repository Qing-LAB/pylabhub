#pragma once
/**
 * @file channel_registry.hpp
 * @brief In-memory channel registry for the pylabhub broker.
 *
 * Stores channel metadata (shared memory segment, ZMQ endpoints, heartbeat state).
 * Single-threaded access only — all methods are called exclusively from the
 * BrokerService run() thread.
 *
 * This is a private implementation header — not part of the installed public API.
 */
#include "utils/channel_pattern.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::broker
{

/// Channel lifecycle state used for heartbeat-gated discovery.
enum class ChannelStatus
{
    PendingReady, ///< Registered but first heartbeat not yet received.
    Ready,        ///< Producer has sent ≥1 heartbeat; consumer discover allowed.
    Closing,      ///< Marked for removal (heartbeat timeout or explicit close).
};

/// Import ChannelPattern from the canonical hub definition.
/// Defined in utils/channel_pattern.hpp; shared between broker and Messenger API.
using ChannelPattern = pylabhub::hub::ChannelPattern;

struct ConsumerEntry
{
    uint64_t    consumer_pid{0};
    std::string consumer_hostname;
    /// ZMQ ROUTER identity bytes (as raw string) captured when consumer first contacted broker.
    /// Used to send CHANNEL_CLOSING_NOTIFY to this consumer.
    std::string zmq_identity;
};

struct ChannelEntry
{
    // ── existing fields ────────────────────────────────────────────────────────
    std::string    shm_name;
    std::string    schema_hash;       ///< Hex-encoded (64 chars), as received from producer
    uint32_t       schema_version{0};
    uint64_t       producer_pid{0};
    std::string    producer_hostname;
    nlohmann::json metadata;          ///< ring_buffer_capacity, policy, etc.
    std::vector<ConsumerEntry> consumers;

    // ── heartbeat / lifecycle ──────────────────────────────────────────────────
    ChannelStatus status{ChannelStatus::PendingReady};
    /// Set to now() at registration; updated on every HEARTBEAT_REQ.
    std::chrono::steady_clock::time_point last_heartbeat{std::chrono::steady_clock::now()};

    // ── ZMQ P2C transport ──────────────────────────────────────────────────────
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint; ///< Producer ROUTER endpoint (ctrl + heartbeat + Bidir data)
    std::string    zmq_data_endpoint; ///< Producer XPUB/PUSH endpoint; empty for Bidir
    std::string    zmq_pubkey;        ///< Producer CurveZMQ public key (Z85, 40 chars)

    // ── broker → producer notification ────────────────────────────────────────
    /// ZMQ ROUTER identity bytes captured when producer sent REG_REQ.
    /// Used to push unsolicited notifications (CHANNEL_CLOSING_NOTIFY, CHANNEL_ERROR_NOTIFY).
    std::string    producer_zmq_identity;
};

/**
 * @class ChannelRegistry
 * @brief Thread-unsafe in-memory registry mapping channel names to ChannelEntry.
 *
 * No mutex required: only the BrokerService run() thread accesses this registry.
 */
class ChannelRegistry
{
public:
    /**
     * @brief Register or update a channel.
     * @return true if registration succeeded (new channel, or same schema_hash for re-registration).
     *         false if schema_hash differs from an existing entry (caller should reply SCHEMA_MISMATCH).
     */
    bool register_channel(const std::string& channel_name, ChannelEntry entry);

    /**
     * @brief Look up a channel by name.
     * @return The ChannelEntry, or std::nullopt if not found.
     */
    [[nodiscard]] std::optional<ChannelEntry> find_channel(const std::string& channel_name) const;

    /**
     * @brief Remove a channel from the registry.
     * @return true if the channel was found and the producer_pid matches; false otherwise.
     */
    bool deregister_channel(const std::string& channel_name, uint64_t producer_pid);

    /**
     * @brief Register a consumer for a channel, storing the ZMQ identity for later notify.
     * @return false if the channel is not found.
     */
    bool register_consumer(const std::string& channel_name, ConsumerEntry entry);

    /**
     * @brief Remove a consumer entry by pid.
     * @return false if the channel is not found or the pid is not registered.
     */
    bool deregister_consumer(const std::string& channel_name, uint64_t consumer_pid);

    /**
     * @brief Returns all consumers for a channel (empty vector if channel not found).
     */
    [[nodiscard]] std::vector<ConsumerEntry>
        find_consumers(const std::string& channel_name) const;

    /**
     * @brief Update heartbeat timestamp and transition channel to Ready.
     * @return false if the channel is not found.
     */
    bool update_heartbeat(const std::string& channel_name);

    /**
     * @brief Returns names of channels whose last_heartbeat is older than timeout.
     *        Only channels in Ready status are considered (PendingReady channels have
     *        their registration time as the baseline, checked separately).
     */
    [[nodiscard]] std::vector<std::string>
        find_timed_out_channels(std::chrono::steady_clock::duration timeout) const;

    [[nodiscard]] std::vector<std::string> list_channels() const;
    [[nodiscard]] size_t size() const;

    /**
     * @brief Mutable pointer to an entry for in-place field updates (e.g. producer_zmq_identity).
     * @return nullptr if not found.
     */
    ChannelEntry* find_channel_mutable(const std::string& channel_name) noexcept;

    /**
     * @brief Mutable access to all entries for liveness iteration.
     *        Caller must not add/remove entries during iteration (single-threaded invariant).
     */
    std::unordered_map<std::string, ChannelEntry>& all_channels() noexcept;

private:
    std::unordered_map<std::string, ChannelEntry> m_channels;
};

} // namespace pylabhub::broker
