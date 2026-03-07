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
#include <vector>

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

/**
 * @brief Channel connection details returned to a consumer by DISC_ACK.
 *
 * Naming note: "ConsumerInfo" means "information FOR the consumer" — it describes
 * what the consumer needs to attach to the channel (SHM name, ZMQ endpoints, schema).
 * It is the consumer's view of a channel, not a description of a consumer identity.
 * Compare with ProducerInfo (the producer's own registration record).
 *
 * DiscoverProducerCmd returns ConsumerInfo because the consumer sends the discovery
 * request and receives this struct in response — it is the consumer-facing channel
 * descriptor returned by the broker's DISC_ACK.
 */
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
    // ── ZMQ Virtual Channel Node (HEP-CORE-0021) ─────────────────────────────
    /// Data transport type: "shm" (default) or "zmq".
    /// Set by the broker in DISC_ACK based on the producer's REG_REQ registration.
    std::string data_transport{"shm"};
    /// For data_transport="zmq": the PUSH endpoint to connect a PULL socket to.
    /// Empty when data_transport="shm".
    std::string zmq_node_endpoint;
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

/**
 * @brief Options for Messenger::create_channel().
 *
 * Grouping the 12 registration parameters into a struct avoids a positional call
 * that is both error-prone and hard to read at call sites.
 * All fields have sensible defaults; set only what you need.
 *
 * Declared at namespace scope (not nested in Messenger) so that GCC can aggregate-
 * initialize it from `{}` in default argument expressions.
 */
struct PYLABHUB_UTILS_EXPORT ChannelRegistrationOptions
{
    ChannelPattern pattern{ChannelPattern::PubSub};
    bool           has_shared_memory{false};
    std::string    schema_hash;       ///< Raw 32 bytes; empty = all-zeros
    uint32_t       schema_version{0};
    int            timeout_ms{5000};
    std::string    actor_name;
    std::string    actor_uid;
    std::string    schema_id;         ///< Named schema ID (e.g. "lab.temp.raw@1")
    std::string    schema_blds;       ///< BLDS raw bytes for schema registry
    std::string    data_transport;    ///< "shm" (default) or "zmq"
    std::string    zmq_node_endpoint; ///< ZMQ virtual channel PUSH endpoint
};

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
     * @brief Producer side: register a new channel with the broker.
     *
     * Binds the ROUTER ctrl socket (and XPUB/PUSH data socket unless Bidir).
     * Sends REG_REQ and waits for REG_ACK. Starts heartbeat timer.
     * Creates a DataBlock segment if @p opts.has_shared_memory is true.
     *
     * @param channel_name  Logical channel name.
     * @param opts          Registration options (see ChannelRegistrationOptions).
     * @return ChannelHandle on success, nullopt on error/timeout.
     */
    [[nodiscard]] std::optional<ChannelHandle>
    create_channel(const std::string              &channel_name,
                   const ChannelRegistrationOptions &opts = {});

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
     * @brief Register a per-channel callback for FORCE_SHUTDOWN.
     *        Broker sends this when the grace period expires after CHANNEL_CLOSING_NOTIFY.
     *        The callback should bypass any message queues and force immediate shutdown.
     *        Pass nullptr to deregister. Called from the Messenger worker thread.
     */
    void on_force_shutdown(const std::string &channel, std::function<void()> cb);

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
     * @brief Enqueue an immediate HEARTBEAT_REQ with metrics payload (HEP-CORE-0019).
     *
     * The metrics JSON is piggybacked on the HEARTBEAT_REQ and stored by the broker's
     * MetricsStore. Backward-compatible: older brokers ignore the extra field.
     */
    void enqueue_heartbeat(const std::string &channel,
                           nlohmann::json     metrics) noexcept;

    /**
     * @brief Enqueue a METRICS_REPORT_REQ to the broker (fire-and-forget, HEP-CORE-0019).
     *
     * Used by consumers (who don't send heartbeats with metrics) to report
     * their metrics to the broker. The broker's MetricsStore stores the data.
     */
    void enqueue_metrics_report(const std::string &channel,
                                const std::string &uid,
                                nlohmann::json     metrics) noexcept;

    /**
     * @brief Send a CHANNEL_NOTIFY_REQ to the broker (fire-and-forget).
     *
     * The broker relays the notification to the target channel's producer as
     * a CHANNEL_EVENT_NOTIFY. The producer's on_channel_error callback receives
     * the event, which then appears as an event dict in the Python msgs list.
     *
     * @param target_channel  Channel whose producer should receive the notification.
     * @param sender_uid      UID of the sending role.
     * @param event           Event name string (e.g. "consumer_ready", "pipeline_ready").
     * @param data            Optional user data string (passed through transparently).
     */
    void enqueue_channel_notify(const std::string &target_channel,
                                const std::string &sender_uid,
                                const std::string &event,
                                const std::string &data = {}) noexcept;

    /**
     * @brief Send a CHANNEL_BROADCAST_REQ to the broker (fire-and-forget).
     *
     * The broker fans out the message as CHANNEL_BROADCAST_NOTIFY to ALL members
     * of the target channel (producer + all consumers). Each member receives the
     * broadcast in its on_channel_error callback as a "broadcast" event, which
     * flows into the Python msgs list as a dict with event="broadcast".
     *
     * @param target_channel  Channel whose members should receive the broadcast.
     * @param sender_uid      UID of the sending role.
     * @param message         Application-level message string (e.g. "start", "stop").
     * @param data            Optional user data string (passed through transparently).
     */
    void enqueue_channel_broadcast(const std::string &target_channel,
                                   const std::string &sender_uid,
                                   const std::string &message,
                                   const std::string &data = {}) noexcept;

    /**
     * @brief Query the broker for the list of registered channels (synchronous).
     *
     * Sends CHANNEL_LIST_REQ and waits for CHANNEL_LIST_ACK. Returns a vector
     * of JSON objects, each containing: name, status, schema_id, producer_uid,
     * consumer_count.
     *
     * @param timeout_ms  Max time to wait for broker response.
     * @return Vector of JSON objects (empty on error or timeout).
     */
    [[nodiscard]] std::vector<nlohmann::json> list_channels(int timeout_ms = 5000);

    /**
     * @brief Query the broker for SHM block topology and DataBlockMetrics.
     *
     * Sends SHM_BLOCK_QUERY_REQ to the broker; the broker opens each SHM segment
     * read-only, reads DataBlockMetrics directly from the header (no lock needed —
     * relaxed-atomic reads), and responds with SHM_BLOCK_QUERY_ACK.
     *
     * @param channel     Channel name to query; empty = all SHM-enabled channels.
     * @param timeout_ms  Max time to wait for broker response.
     * @return JSON string with block topology and metrics (empty on error/timeout).
     */
    [[nodiscard]] std::string query_shm_blocks(const std::string& channel = {},
                                               int timeout_ms = 5000);

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
