#pragma once
/**
 * @file messenger.hpp
 * @brief ZeroMQ-based async messaging with the central broker.
 *
 * Messenger is a singleton that manages communication with the broker via an
 * internal worker thread. Public methods are thread-safe.
 *
 * Low-level API (fire-and-forget / single-shot):
 *   register_producer / register_consumer  — fire-and-forget; errors logged by worker.
 *   discover_producer                       — enqueues request and blocks on broker reply;
 *                                             retries on CHANNEL_NOT_READY within timeout.
 *   connect / disconnect                    — synchronous; guarded by internal mutex.
 *
 * High-level channel API:
 *   create_channel   — synchronous; binds P2C ZMQ sockets, registers with broker,
 *                      starts periodic heartbeat.  Returns a ChannelHandle.
 *   connect_channel  — synchronous; discovers (retries until Ready), connects P2C
 *                      sockets, registers consumer.  Returns a ChannelHandle.
 *   on_channel_closing — register callback invoked when broker pushes
 *                        CHANNEL_CLOSING_NOTIFY.
 */
#include "pylabhub_utils_export.h"

#include "utils/channel_handle.hpp"
#include "utils/channel_pattern.hpp"
#include "utils/module_def.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::hub
{

class MessengerImpl;

// ChannelHandle is defined in channel_handle.hpp (included above).
// ChannelPattern is defined in channel_pattern.hpp and imported here.
// pylabhub::broker::ChannelPattern is a type alias for pylabhub::hub::ChannelPattern.

/// Schema information returned by Messenger::query_channel_schema() (HEP-CORE-0016 Phase 3).
struct ChannelSchemaInfo
{
    std::string schema_id; ///< Named schema ID; empty = anonymous channel
    std::string blds;      ///< BLDS string; empty if producer did not provide PYLABHUB_SCHEMA macros
    std::string hash_hex;  ///< Hex-encoded 64-char BLAKE2b-256 hash; empty if no schema
};

struct ProducerInfo
{
    // ── existing ──────────────────────────────────────────────────────────────
    std::string shm_name;
    uint64_t    producer_pid{0};
    std::string schema_hash;
    uint32_t    schema_version{0};
    // ── new (P2C transport) ───────────────────────────────────────────────────
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint; ///< Producer ROUTER endpoint
    std::string    zmq_data_endpoint; ///< Producer XPUB/PUSH endpoint; empty for Bidir
    std::string    zmq_pubkey;        ///< Producer CurveZMQ public key (Z85, 40 chars)
    // ── actor identity (Phase 2) ─────────────────────────────────────────────
    std::string actor_name; ///< Human-readable actor name; empty = anonymous
    std::string actor_uid;  ///< Actor UUID4 or ACTOR-{NAME}-{8HEX}; empty = anonymous
};

struct ConsumerInfo
{
    // ── existing ──────────────────────────────────────────────────────────────
    std::string shm_name;
    std::string schema_hash;
    uint32_t    schema_version{0};
    // ── new (filled from DISC_ACK) ────────────────────────────────────────────
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey;
    uint32_t       consumer_count{0};
    // ── actor identity (Phase 2) ─────────────────────────────────────────────
    std::string consumer_uid;  ///< Consumer actor UUID4; empty = anonymous
    std::string consumer_name; ///< Human-readable consumer actor name; empty = anonymous
};

/**
 * @class Messenger
 * @brief Manages communication with the central broker.
 *
 * All public methods are thread-safe. ZMQ broker socket access is single-threaded
 * (internal worker thread only). Async queue decouples callers from socket I/O.
 *
 * P2C (producer-to-consumer) sockets owned by ChannelHandle objects are created
 * in the calling thread and are NOT shared with the Messenger worker thread.
 */
class PYLABHUB_UTILS_EXPORT Messenger
{
  public:
    Messenger();
    ~Messenger();

    Messenger(const Messenger &) = delete;
    Messenger &operator=(const Messenger &) = delete;
    Messenger(Messenger &&) noexcept;
    Messenger &operator=(Messenger &&) noexcept;

    // ── Broker connection ──────────────────────────────────────────────────────

    /**
     * @brief Connects to the broker, starts the worker thread.
     *
     * @param endpoint     Broker ZMQ endpoint (e.g. "tcp://127.0.0.1:5570"). Required.
     * @param server_key   Broker CurveZMQ public key (Z85, 40 chars).
     *                     Empty string = plain TCP (no encryption).
     * @param client_pubkey Actor's own CurveZMQ public key (Z85, 40 chars).
     *                     Used only when server_key is non-empty.
     *                     Empty = generate an ephemeral keypair for this connection.
     * @param client_seckey Actor's own CurveZMQ secret key (Z85, 40 chars).
     *                     Must be paired with client_pubkey.
     * @return true if connection was established.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] bool connect(const std::string &endpoint,
                               const std::string &server_key   = {},
                               const std::string &client_pubkey = {},
                               const std::string &client_seckey = {});

    /** @brief Closes the broker connection. Worker thread remains running. */
    void disconnect();

    // ── Low-level producer/consumer API (existing) ─────────────────────────────

    /** @brief Registers a producer (fire-and-forget). Errors logged by worker.
     *         After successful REG_ACK the worker automatically sends one
     *         HEARTBEAT_REQ so the channel transitions to Ready. */
    void register_producer(const std::string &channel, const ProducerInfo &info);

    /** @brief Registers this process as a consumer (fire-and-forget). */
    void register_consumer(const std::string &channel, const ConsumerInfo &info);

    /** @brief Deregisters this process as a consumer (fire-and-forget). */
    void deregister_consumer(const std::string &channel);

    /** @brief Discovers a producer via the broker (synchronous).
     *         Retries on CHANNEL_NOT_READY within @p timeout_ms.
     *  @return ConsumerInfo on success, nullopt on timeout/error/not-connected. */
    [[nodiscard]] std::optional<ConsumerInfo> discover_producer(const std::string &channel,
                                                                 int timeout_ms = 5000);

    // ── High-level channel API (new) ───────────────────────────────────────────

    /**
     * @brief Producer side: bind P2C ZMQ sockets, register channel with broker,
     *        start periodic heartbeat.
     *
     * Binds the ROUTER ctrl socket (and XPUB/PUSH data socket unless Bidir).
     * Sends REG_REQ and waits for REG_ACK. Starts heartbeat timer.
     * Creates a DataBlock segment if @p has_shared_memory is true.
     *
     * @param channel_name    Logical channel name.
     * @param pattern         ZMQ socket pattern (PubSub / Pipeline / Bidir).
     * @param has_shared_memory  Whether to also allocate a DataBlock segment.
     * @param schema_hash     Schema hash (raw 32 bytes); empty = all-zeros.
     * @param schema_version  Schema version.
     * @param timeout_ms      Max time to wait for broker REG_ACK.
     * @return ChannelHandle on success, nullopt on error/timeout.
     */
    [[nodiscard]] std::optional<ChannelHandle>
    create_channel(const std::string &channel_name,
                   ChannelPattern     pattern           = ChannelPattern::PubSub,
                   bool               has_shared_memory = false,
                   const std::string &schema_hash       = {},
                   uint32_t           schema_version    = 0,
                   int                timeout_ms        = 5000,
                   const std::string &actor_name        = {},
                   const std::string &actor_uid         = {},
                   const std::string &schema_id         = {},
                   const std::string &schema_blds       = {});

    /**
     * @brief Consumer side: discover channel (retrying until Ready), connect P2C
     *        ZMQ sockets, register consumer with broker.
     *
     * Retries DISC_REQ on CHANNEL_NOT_READY until the channel is Ready or
     * @p timeout_ms expires. Connects DEALER ctrl + SUB/PULL data sockets.
     * For Bidir, performs the HELLO/HELLO_ACK identity handshake.
     * Attaches to DataBlock if the channel has shared memory.
     *
     * @param channel_name  Logical channel name.
     * @param timeout_ms    Max total time to wait for Ready + connect.
     * @param schema_hash   Expected schema hash (raw 32 bytes; empty = accept any).
     * @return ChannelHandle on success, nullopt on error/timeout.
     */
    [[nodiscard]] std::optional<ChannelHandle>
    connect_channel(const std::string &channel_name,
                    int                timeout_ms         = 5000,
                    const std::string &schema_hash        = {},
                    const std::string &consumer_uid       = {},
                    const std::string &consumer_name      = {},
                    const std::string &expected_schema_id = {});

    /**
     * @brief Query the broker for schema information about a registered channel.
     *
     * Sends SCHEMA_REQ to the broker and waits for SCHEMA_ACK.
     * Returns the schema_id (named or ""), BLDS string, and hex hash for the channel.
     *
     * @param channel_name  Channel to query.
     * @param timeout_ms    Max time to wait for broker response.
     * @return ChannelSchemaInfo on success; nullopt if channel not found or not connected.
     */
    [[nodiscard]] std::optional<ChannelSchemaInfo>
    query_channel_schema(const std::string &channel_name, int timeout_ms = 5000);

    /**
     * @brief Register a global callback invoked when the broker pushes CHANNEL_CLOSING_NOTIFY.
     *        Fires for any channel when no per-channel callback is registered.
     *        The callback is called from the Messenger worker thread.
     */
    void on_channel_closing(std::function<void(const std::string &channel)> cb);

    /**
     * @brief Register a per-channel callback for CHANNEL_CLOSING_NOTIFY.
     *        Per-channel takes priority over the global callback.
     *        Pass nullptr to deregister.
     */
    void on_channel_closing(const std::string &channel, std::function<void()> cb);

    /**
     * @brief Register a per-channel callback for CONSUMER_DIED_NOTIFY (Cat 2).
     *        Pass nullptr to deregister. Called from the Messenger worker thread.
     */
    void on_consumer_died(const std::string &channel,
                          std::function<void(uint64_t consumer_pid, std::string reason)> cb);

    /**
     * @brief Register a per-channel callback for CHANNEL_ERROR_NOTIFY (Cat 1) and
     *        CHANNEL_EVENT_NOTIFY (Cat 2). Pass nullptr to deregister.
     */
    void on_channel_error(const std::string &channel,
                          std::function<void(std::string event, nlohmann::json details)> cb);

    /**
     * @brief Remove channel from heartbeat list and send DEREG_REQ to broker.
     *        Fire-and-forget — errors logged by worker.
     */
    void unregister_channel(const std::string &channel);

    // ── Phase 3: actor zmq_thread_ heartbeat integration ──────────────────────

    /**
     * @brief Suppress or restore the periodic heartbeat for @p channel (thread-safe,
     *        fire-and-forget).
     *
     * When @p suppress is true, the Messenger worker skips @p channel in its
     * periodic heartbeat loop. The caller (actor zmq_thread_) is responsible for
     * sending application-level HEARTBEAT_REQ via enqueue_heartbeat() instead.
     *
     * No-op if @p channel is not registered for heartbeat (e.g., consumer roles)
     * or if the worker is not running.
     */
    void suppress_periodic_heartbeat(const std::string &channel,
                                     bool               suppress = true) noexcept;

    /**
     * @brief Enqueue an immediate HEARTBEAT_REQ for @p channel (thread-safe,
     *        fire-and-forget).
     *
     * Used by the actor's zmq_thread_ to deliver application-level heartbeats
     * when @c iteration_count_ has advanced, proving the Python loop is progressing.
     *
     * No-op if @p channel is not registered for heartbeat, or if not connected.
     */
    void enqueue_heartbeat(const std::string &channel) noexcept;

    /**
     * @brief Report a Cat 2 slot checksum error to broker (fire-and-forget).
     *        Broker's ChecksumRepairPolicy determines further action.
     */
    void report_checksum_error(const std::string &channel, int32_t slot_index,
                                std::string_view error_description);

    // ── Singleton ─────────────────────────────────────────────────────────────

    /** @brief Returns the lifecycle-managed singleton Messenger instance. */
    static Messenger &get_instance();

  private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<MessengerImpl> pImpl;
};

/**
 * @brief Returns true if the Data Exchange Hub lifecycle module is initialized.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool lifecycle_initialized() noexcept;

/**
 * @brief Factory function for the Data Exchange Hub lifecycle module.
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetLifecycleModule();

} // namespace pylabhub::hub
