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

    /** @brief Connects to the broker, starts the worker thread.
     *  @return true if connection was established. */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] bool connect(const std::string &endpoint, const std::string &server_key);

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
                   int                timeout_ms        = 5000);

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
                    int                timeout_ms  = 5000,
                    const std::string &schema_hash = {});

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
