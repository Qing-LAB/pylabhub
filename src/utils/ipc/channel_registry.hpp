#pragma once
/**
 * @file channel_registry.hpp
 * @brief In-memory channel registry for the pylabhub broker.
 *
 * Stores channel metadata (shared memory segment, ZMQ endpoints, heartbeat state).
 *
 * ACCESS DISCIPLINE
 * -----------------
 * This is a private broker-internal class. It is:
 *   - Included only by broker_service.cpp and channel_registry.cpp.
 *   - Never part of the installed public API (not in src/include/).
 *   - Always accessed under BrokerService's m_query_mu mutex.
 *
 * All methods are called exclusively from the BrokerService run() thread
 * (or from query methods that hold m_query_mu). No internal locking is
 * needed or provided — that responsibility lies entirely with the caller.
 *
 * MUTABILITY DISCIPLINE
 * ---------------------
 * Prefer the narrowest API that satisfies each use case:
 *   - For field reads:         find_channel() (returns value copy) or const all_channels()
 *   - For lifecycle mutations: find_channel_mutable() — ONLY for status / deadline / identity
 *                              updates. Do NOT use for read-only field access.
 *   - For full iteration with mutation: all_channels() (mutable overload)
 *   - For full iteration read-only: all_channels() const overload or find_timed_out_channels()
 */
#include "utils/channel_pattern.hpp"

#include "utils/json_fwd.hpp"

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
/// Defined in utils/channel_pattern.hpp; shared between broker and client APIs.
using ChannelPattern = pylabhub::hub::ChannelPattern;

// channel_pattern_to_str() and channel_pattern_from_str() are defined in
// utils/channel_pattern.hpp (included above). Use those directly.

struct ConsumerEntry
{
    uint64_t    consumer_pid{0};
    std::string consumer_hostname;
    /// ZMQ ROUTER identity bytes (as raw string) captured when consumer first contacted broker.
    /// Used to send CHANNEL_CLOSING_NOTIFY to this consumer.
    std::string zmq_identity;

    // ── Identity (Phase 3) ────────────────────────────────────────────────────
    std::string role_name;  ///< Human name if provided in CONSUMER_REG_REQ; empty otherwise.
    std::string role_uid;   ///< Role UID if provided in CONSUMER_REG_REQ; empty otherwise.
    // ── Inbox (Phase 5) ──────────────────────────────────────────────────────
    std::string inbox_endpoint;     ///< ROUTER bind endpoint; empty = no inbox.
    std::string inbox_schema_json;  ///< JSON schema; empty = no inbox.
    std::string inbox_packing;      ///< "aligned" or "packed"; empty = no inbox.
    std::string inbox_checksum;     ///< "enforced", "manual", "none"; empty = enforced.
    std::chrono::system_clock::time_point connected_at{std::chrono::system_clock::now()};
};

struct ChannelEntry
{
    // ── existing fields ────────────────────────────────────────────────────────
    std::string    shm_name;
    std::string    schema_hash;       ///< Hex-encoded (64 chars), as received from producer
    uint32_t       schema_version{0};
    uint64_t       producer_pid{0};
    std::string    producer_hostname;

    // ── Identity (Phase 3) ────────────────────────────────────────────────────
    std::string    producer_role_name; ///< Human name if provided in REG_REQ; empty otherwise.
    std::string    producer_role_uid;  ///< Role UID if provided in REG_REQ; empty otherwise.
    nlohmann::json metadata;          ///< ring_buffer_capacity, policy, etc.
    std::vector<ConsumerEntry> consumers;

    // ── heartbeat / lifecycle ──────────────────────────────────────────────────
    ChannelStatus status{ChannelStatus::PendingReady};
    /// Set to now() at registration; updated on every HEARTBEAT_REQ.
    std::chrono::steady_clock::time_point last_heartbeat;
    /// Timestamp of the most recent status transition (HEP-CORE-0023 §2.5).
    /// For PendingReady entries this is the registration time (or the moment
    /// of Ready -> Pending demotion). Used to enforce pending_timeout.
    std::chrono::steady_clock::time_point state_since;

    /// Default ctor: both timestamps share a single now() sample so the
    /// state-machine invariant `state_since <= last_heartbeat` holds at
    /// construction (no µs-skew between two separate now() calls).
    ChannelEntry()
        : last_heartbeat(std::chrono::steady_clock::now())
        , state_since(last_heartbeat)
    {}

    // ── ZMQ P2C transport ──────────────────────────────────────────────────────
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint; ///< Producer ROUTER endpoint (ctrl + heartbeat + Bidir data)
    std::string    zmq_data_endpoint; ///< Producer XPUB/PUSH endpoint; empty for Bidir
    std::string    zmq_pubkey;        ///< Producer CurveZMQ public key (Z85, 40 chars)

    // ── ZMQ Endpoint Registry (HEP-CORE-0021) ─────────────────────────────────
    /// Data transport type: "shm" (default) or "zmq" (peer-to-peer endpoint
    /// registered with the broker for discovery; broker does not relay data).
    std::string    data_transport{"shm"};
    /// For data_transport="zmq": the bind endpoint registered by the producer's PUSH socket.
    /// Empty when data_transport="shm". Returned verbatim in DISC_ACK for peer discovery.
    std::string    zmq_node_endpoint;
    /// Role inbox endpoint registered in REG_REQ (Phase 3). Empty if no inbox configured.
    std::string    inbox_endpoint;
    /// JSON-serialized ZmqSchemaField array for the inbox (Phase 4). Empty if no inbox.
    /// Format: [{"type":"float64","count":1,"length":0}, ...]
    std::string    inbox_schema_json;
    /// Packing for the inbox schema (Phase 4): "aligned" or "packed". Empty if no inbox.
    std::string    inbox_packing;
    /// Checksum policy for the inbox: "enforced", "manual", "none". Empty = enforced.
    std::string    inbox_checksum;

    // ── Schema identity (HEP-CORE-0016 Phase 3) ───────────────────────────────
    /// Named schema ID set by producer in REG_REQ or annotated by broker via reverse hash
    /// lookup. Empty string = anonymous channel (no named schema confirmed).
    std::string    schema_id;
    /// BLDS string provided by producer in REG_REQ; empty when producer lacks PYLABHUB_SCHEMA
    /// macros.  Returned verbatim in SCHEMA_ACK responses.
    std::string    schema_blds;

    // ── broker → producer notification ────────────────────────────────────────
    /// ZMQ ROUTER identity bytes captured when producer sent REG_REQ.
    /// Used to push unsolicited notifications (CHANNEL_CLOSING_NOTIFY, CHANNEL_ERROR_NOTIFY).
    std::string    producer_zmq_identity;

    // ── graceful shutdown (two-tier) ──────────────────────────────────────────
    /// When status == Closing, this is the deadline after which the broker
    /// escalates to FORCE_SHUTDOWN for any members still registered.
    std::chrono::steady_clock::time_point closing_deadline{};
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
     * @brief Ready roles whose last_heartbeat is older than `timeout`.
     *        Caller should demote each to PendingReady (HEP-CORE-0023 §2.5).
     */
    [[nodiscard]] std::vector<std::string>
        find_timed_out_ready(std::chrono::steady_clock::duration timeout) const;

    /**
     * @brief PendingReady roles whose `state_since` is older than `timeout`.
     *        Caller sends CHANNEL_CLOSING_NOTIFY and transitions the entry to
     *        Closing with `closing_deadline`; the actual deregister happens
     *        later in `check_closing_deadlines` once the grace window elapses.
     */
    [[nodiscard]] std::vector<std::string>
        find_timed_out_pending(std::chrono::steady_clock::duration timeout) const;

    [[nodiscard]] std::vector<std::string> list_channels() const;
    [[nodiscard]] size_t size() const;

    /**
     * @brief Mutable pointer to an entry for in-place lifecycle field updates.
     *
     * Use ONLY for mutations: status transitions, closing_deadline, producer_zmq_identity.
     * For read-only field access use find_channel() instead — it returns a const value copy
     * and makes the intent clear.
     *
     * @return nullptr if not found.
     */
    ChannelEntry* find_channel_mutable(const std::string& channel_name) noexcept;

    /**
     * @brief Mutable access to all entries — for liveness iteration that modifies entries
     *        (e.g. removing dead consumers, transitioning status to Closing).
     *        Caller must not add/remove entries during iteration (single-threaded invariant).
     */
    std::unordered_map<std::string, ChannelEntry>& all_channels() noexcept;

    /**
     * @brief Read-only access to all entries — for snapshot / reporting iteration.
     *        Prefer this overload whenever no mutation is needed.
     */
    const std::unordered_map<std::string, ChannelEntry>& all_channels() const noexcept;

private:
    std::unordered_map<std::string, ChannelEntry> m_channels;
};

} // namespace pylabhub::broker
