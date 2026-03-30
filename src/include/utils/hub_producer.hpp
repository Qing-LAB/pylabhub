#pragma once
/**
 * @file hub_producer.hpp
 * @brief Active producer service: owns both ZMQ transport (ChannelHandle) and
 *        shared memory (DataBlockProducer), with dedicated internal threads.
 *
 * Producer is an active service object. It manages:
 *   - peer_thread: monitors the ROUTER ctrl socket for consumer HELLO/BYE/messages.
 *   - write_thread: drives SHM slot processing in either Queue or RealTime mode.
 *
 * ## SHM Processing Modes
 *
 * **Queue mode** (default): write_thread sleeps until the caller submits a job.
 *   - `push<F,D>(job)`         — async, non-blocking; write_thread acquires slot, calls job.
 *   - `synced_write<F,D>(job)` — sync, blocks caller until slot acquired and job completes.
 *
 * **Real-time mode**: write_thread drives a continuous processing loop.
 *   - `set_write_handler<F,D>(fn)` — install handler; thread loops calling fn per slot cycle.
 *   - `set_write_handler<F,D>(nullptr)` — remove handler; returns to Queue mode.
 *
 * Mode is selected implicitly: installing a handler enters Real-time; removing it returns
 * to Queue mode. Queryable via `has_realtime_handler()`.
 *
 * Both modes receive a fully-typed `WriteProcessorContext<FlexZoneT, DataBlockT>` that
 * bundles: typed FlexZone access, the full WriteTransactionContext, peer messaging, and
 * a shutdown signal. Type safety is enforced at the call site via template parameters.
 *
 * One Producer instance per channel per process. Use with LifecycleGuard (ManagedProducer)
 * or manage lifetime manually.
 *
 * **Thread safety**: All public methods are thread-safe unless documented otherwise.
 */
#include "pylabhub_utils_export.h"
#include "utils/channel_handle.hpp"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/loop_timing_policy.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqQueue — returned by queue() accessor (HEP-CORE-0021)
#include "utils/messenger.hpp"
#include "utils/module_def.hpp"
#include "utils/schema_library.hpp" // validate_named_schema_from_env (Phase 2)

#include "utils/json_fwd.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// ProducerMessagingFacade — type-erased messaging bridge (internal use)
// ============================================================================

/**
 * @struct ProducerMessagingFacade
 * @brief ABI-stable bridge between WriteProcessorContext<F,D> (header template) and
 *        ProducerImpl internals (defined in .cpp). Function pointers are filled by
 *        Producer::establish_channel(); context points to the ProducerImpl on the heap.
 *
 * **Internal use only — do not use directly.**
 * This struct is exposed in the header solely so that the template WriteProcessorContext<F,D>
 * can reference it without knowing ProducerImpl. It is an implementation detail, not part
 * of the public API.
 *
 * **ABI note:** The function pointer fields and their signatures are **frozen** for the
 * lifetime of the pylabhub-utils shared library ABI. Any change to field order, types, or
 * count requires a SOVERSION bump. New fields must be appended at the end, never inserted.
 * Callers may rely on all fields being null-initialized (defaulted to nullptr) in the struct.
 */
struct PYLABHUB_UTILS_EXPORT ProducerMessagingFacade
{
    /// Returns the DataBlockProducer* (nullptr if SHM not configured).
    DataBlockProducer *(*fn_get_shm)(void *ctx){nullptr};
    /// Returns current consumer ZMQ identities.
    std::vector<std::string> (*fn_consumers)(void *ctx){nullptr};
    /// Broadcasts raw bytes to all consumers on the data socket.
    bool (*fn_broadcast)(void *ctx, const void *data, size_t size){nullptr};
    /// Sends raw bytes to a specific consumer via ROUTER identity (queued through peer_thread).
    bool (*fn_send_to)(void *ctx, const std::string &identity, const void *data,
                       size_t size){nullptr};
    /// Returns true when the producer's write_thread stop flag is set.
    bool (*fn_is_stopping)(void *ctx){nullptr};
    /// Returns the Messenger* used by this Producer.
    Messenger *(*fn_messenger)(void *ctx){nullptr};
    /// Returns the channel name string.
    const std::string &(*fn_channel_name)(void *ctx){nullptr};
    /// Opaque pointer to ProducerImpl.
    void *context{nullptr};
};

/// Internal handler type stored in ProducerImpl for the real-time write loop.
/// Receives the facade by reference each invocation; captures typed F,D in the closure.
using InternalWriteHandlerFn = std::function<void(ProducerMessagingFacade &)>;

// ============================================================================
// WriteProcessorContext<FlexZoneT, DataBlockT>
// ============================================================================

/**
 * @struct WriteProcessorContext
 * @brief Fully-typed context passed to write handlers and write jobs.
 *
 * Bundles:
 *   - `txn`      — WriteTransactionContext<FlexZoneT, DataBlockT> for slot + flexzone access.
 *   - `flexzone()` — convenience typed flexzone accessor (when FlexZoneT != void).
 *   - `is_stopping()` — shutdown signal (check at natural loop checkpoints).
 *   - Peer messaging: `broadcast`, `send_to`, `connected_consumers`.
 *   - Broker access: `messenger()`, `report_checksum_error`.
 *
 * FlexZone and DataBlock types are fixed at `Producer::create<FlexZoneT, DataBlockT>()`
 * time and validated against the channel schema at establishment. By the time any handler
 * or job executes, the types are guaranteed consistent across all channel participants.
 *
 * FlexZone synchronization is managed by the DataBlock framework. Consistency is
 * guaranteed when the producer updates FlexZone within slot transactions (the slot-commit
 * atomic transition acts as the happens-before barrier).
 */
template <typename FlexZoneT, typename DataBlockT>
struct WriteProcessorContext
{
    WriteTransactionContext<FlexZoneT, DataBlockT> &txn;
    ProducerMessagingFacade                        &facade;

    // ── FlexZone access ───────────────────────────────────────────────────────

    /**
     * @brief Typed FlexZone access. Only available when FlexZoneT is not void.
     * @return Reference to the FlexZone in shared memory (producer-owned, read/write).
     */
    template <typename F = FlexZoneT>
    auto flexzone() noexcept -> std::enable_if_t<!std::is_void_v<F>, F &>
    {
        return txn.flexzone().get();
    }

    // ── Shutdown signal ───────────────────────────────────────────────────────

    /**
     * @brief True when the Producer is stopping (stop() has been called).
     * Check at natural processing checkpoints in the handler loop.
     * Always false in synced_write() unless stop() races with it.
     */
    [[nodiscard]] bool is_stopping() const noexcept
    {
        return facade.fn_is_stopping(facade.context);
    }

    // ── Peer messaging ────────────────────────────────────────────────────────

    /// Broadcast raw bytes to all connected consumers on the data socket.
    bool broadcast(const void *data, size_t size)
    {
        return facade.fn_broadcast(facade.context, data, size);
    }

    /// Send raw bytes to a specific consumer via ZMQ ROUTER identity.
    bool send_to(const std::string &identity, const void *data, size_t size)
    {
        return facade.fn_send_to(facade.context, identity, data, size);
    }

    /// Returns ZMQ identities of currently connected consumers.
    [[nodiscard]] std::vector<std::string> connected_consumers() const
    {
        return facade.fn_consumers(facade.context);
    }

    // ── Broker channel ────────────────────────────────────────────────────────

    /// Full Messenger access for advanced use (additional registrations, broker reporting).
    [[nodiscard]] Messenger &messenger() const { return *facade.fn_messenger(facade.context); }

    /// Report a Cat 2 slot checksum error to the broker (fire-and-forget).
    void report_checksum_error(int32_t slot_idx, std::string_view desc)
    {
        facade.fn_messenger(facade.context)
            ->report_checksum_error(facade.fn_channel_name(facade.context), slot_idx, desc);
    }
};

// ============================================================================
// ProducerOptions
// ============================================================================

/**
 * @struct ProducerOptions
 * @brief Configuration for creating a Producer active service.
 */
struct ProducerOptions
{
    std::string    channel_name;
    ChannelPattern pattern{ChannelPattern::PubSub};

    bool            has_shm{false};
    DataBlockConfig shm_config{}; ///< shm_config.name ignored; derived from channel_name.
                                  ///< shm_config.shared_secret used as SHM secret.

    /// Schema info (auto-derived from template params when using template factory).
    std::string schema_hash{};
    uint32_t    schema_version{0};

    int timeout_ms{5000};

    // ── role identity (Phase 2) ───────────────────────────────────────────────
    std::string role_name{}; ///< Human-readable role name; empty = anonymous
    std::string role_uid{};  ///< Role UID (PROD-{NAME}-{8HEX}); empty = anonymous

    // ── Loop timing (HEP-CORE-0008) ─────────────────────────────────────────
    LoopTimingParams timing{};  ///< Policy + period + io_wait_ratio. Set from config.

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    /// Optional named schema ID (e.g. `"lab.sensors.temperature.raw@1"`).
    /// When non-empty, `create<FlexZoneT, DataBlockT>()` validates sizeof and BLDS hash
    /// against the schema loaded from PYLABHUB_SCHEMA_PATH (or default search dirs).
    /// Throws SchemaValidationException on mismatch. No-op when empty.
    std::string schema_id{};

    // ── HEP-CORE-0021: ZMQ Virtual Channel Node ───────────────────────────────
    /// Data transport type: "shm" (default) or "zmq".
    /// When "zmq", the producer registers a ZMQ Virtual Channel Node with the broker,
    /// advertising @p zmq_node_endpoint. Consumers discover this endpoint via DISC_ACK.
    std::string data_transport{"shm"};
    /// Bind address for the ZMQ PUSH socket (used only when data_transport=="zmq").
    /// Example: "tcp://127.0.0.1:5580"
    std::string zmq_node_endpoint{};
    /// If true, PUSH socket binds to zmq_node_endpoint; otherwise connects (default: bind).
    bool zmq_bind{true};
    /// Inbox endpoint registered with the broker. Empty = no inbox.
    /// Set this to InboxQueue::actual_endpoint() before calling Producer::create().
    std::string inbox_endpoint{};
    /// JSON-serialized ZmqSchemaField list for the inbox (Phase 4). Empty = no inbox.
    /// Stored by broker and returned by ROLE_INFO_REQ for InboxClient discovery.
    std::string inbox_schema_json{};
    /// Packing for inbox schema (Phase 4): "aligned" or "packed". Empty = no inbox.
    std::string inbox_packing{};
    /// Schema for ZMQ PUSH frames (required when data_transport=="zmq").
    /// Empty schema → LOGGER_ERROR + Producer::create returns nullopt.
    /// Use {{"bytes",1,N}} as a single-blob schema for opaque N-byte payloads.
    std::vector<ZmqSchemaField> zmq_schema{};
    /// "aligned" (ctypes.LittleEndianStructure default) or "packed" (no padding).
    /// Must match the receiver's packing.
    std::string zmq_packing{"aligned"};
    /// Internal send-buffer depth for the ZmqQueue PUSH ring (write side).
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    /// Overflow policy for the ZmqQueue PUSH send ring.
    /// Drop (default): write_acquire() returns nullptr immediately when ring is full.
    /// Block: write_acquire() waits up to the caller's timeout for a free slot.
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // ── Queue abstraction (Phase 2) ──────────────────────────────────────────
    /// Slot data size in bytes (from engine type_sizeof("SlotFrame")). Required for ShmQueue wrapper.
    size_t item_size{0};
    /// Flexzone size in bytes (page-aligned). 0 = no flexzone.
    size_t flexzone_size{0};
    /// Checksum policy for this channel. Applied to all queues via set_checksum_policy().
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    /// Checksum flexzone (SHM-specific). Applied via set_flexzone_checksum().
    bool flexzone_checksum{true};
    /// Zero the slot buffer on write_acquire() (SHM only). Default true for safety
    /// (prevents historical data leaks, ensures deterministic padding). Set false
    /// for performance when the writer guarantees full field writes.
    bool always_clear_slot{true};

    /// Max depth of P2P ctrl send queue before oldest items are dropped. 0 = unbounded.
    size_t ctrl_queue_max_depth{256};
    /// Peer silence timeout before on_peer_dead fires (ms). 0 = disabled.
    int    peer_dead_timeout_ms{30000};
};

// ============================================================================
// Producer
// ============================================================================

struct ProducerImpl;

/// Default timeouts (accessible from template code in this header).
namespace detail
{
inline constexpr int kDefaultWriteSlotTimeoutMs = 5000; ///< Timeout for queue/synced jobs
inline constexpr int kRealtimeWritePollMs        = 50;  ///< Slot poll interval in real-time mode
} // namespace detail

/**
 * @class Producer
 * @brief Active producer service managing a published channel.
 *
 * Created via Producer::create() or Producer::create<FlexZoneT, DataBlockT>().
 * Optional active mode: call start() to launch peer_thread (consumer tracking) and
 * write_thread (SHM slot processing).
 */
class PYLABHUB_UTILS_EXPORT Producer
{
  public:
    // ── Factories ──────────────────────────────────────────────────────────────

    /**
     * @brief Non-template factory: no compile-time schema validation.
     *        SHM created without schema type association.
     */
    [[nodiscard]] static std::optional<Producer>
    create(Messenger &messenger, const ProducerOptions &opts);

    /**
     * @brief Template factory: derives schemas from FlexZoneT and DataBlockT.
     *        Stores both schemas in the DataBlock header for consumer validation.
     */
    template <typename FlexZoneT, typename DataBlockT>
    [[nodiscard]] static std::optional<Producer>
    create(Messenger &messenger, const ProducerOptions &opts);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~Producer();
    Producer(Producer &&) noexcept;
    Producer &operator=(Producer &&) noexcept;
    Producer(const Producer &) = delete;
    Producer &operator=(const Producer &) = delete;

    // ── Callbacks — set BEFORE start() ────────────────────────────────────────

    /// Called from peer_thread when a consumer connects (sends HELLO).
    using ConsumerCallback = std::function<void(const std::string &identity)>;
    void on_consumer_joined(ConsumerCallback cb);
    void on_consumer_left(ConsumerCallback cb);

    /// Called from peer_thread when a consumer sends a non-HELLO/BYE ctrl message.
    using MessageCallback =
        std::function<void(const std::string &identity, std::span<const std::byte> data)>;
    void on_consumer_message(MessageCallback cb);

    /// Called from Messenger worker thread when broker sends CHANNEL_CLOSING_NOTIFY.
    /// This is a graceful notification — the script should finish pending work then call
    /// api.stop(). The broker will escalate to FORCE_SHUTDOWN after a grace period.
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends FORCE_SHUTDOWN.
    /// Grace period expired — the handler should force immediate shutdown.
    void on_force_shutdown(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends CONSUMER_DIED_NOTIFY (Cat 2).
    using ConsumerDiedCallback =
        std::function<void(uint64_t consumer_pid, const std::string &reason)>;
    void on_consumer_died(ConsumerDiedCallback cb);

    /// Called from Messenger worker thread on CHANNEL_ERROR_NOTIFY (Cat 1) or
    /// CHANNEL_EVENT_NOTIFY (Cat 2).
    using ChannelErrorCallback =
        std::function<void(const std::string &event, const nlohmann::json &details)>;
    void on_channel_error(ChannelErrorCallback cb);

    /// Callback fired when the peer (consumer) has been silent for peer_dead_timeout_ms.
    /// Called from peer_thread — callback must be thread-safe.
    void on_peer_dead(std::function<void()> cb);

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start peer_thread (ctrl monitor) and write_thread (SHM).
     * @return true if started successfully; false if already running or not valid.
     */
    bool start();

    /**
     * @brief Embedded mode: set running=true WITHOUT launching peer_thread/write_thread.
     * Use when the caller (role ZMQ thread) drives all ZMQ polling itself via
     * peer_ctrl_socket_handle() + handle_peer_events_nowait().
     * @return true if successfully transitioned to running; false if already running or invalid.
     */
    [[nodiscard]] bool start_embedded() noexcept;

    /**
     * @brief Returns the raw ZMQ ROUTER ctrl socket handle for use in zmq_pollitem_t.
     * MUST be called only in embedded mode (peer_thread not running).
     * Returns nullptr if not valid or no ctrl socket.
     */
    [[nodiscard]] void *peer_ctrl_socket_handle() const noexcept;

    /**
     * @brief Non-blocking: drain ctrl send queue + process all pending POLLIN on ctrl socket.
     *
     * Fires on_consumer_joined / on_consumer_left / on_consumer_message callbacks synchronously.
     * MUST be called from the socket-owning thread (embedded-mode ZMQ thread only).
     * No-op if not valid.
     *
     * Internally limits recv dispatch to 100 messages per call to prevent infinite loops
     * from unexpected ZMQ frame patterns (see HEP-CORE-0007 §12.3).
     */
    void handle_peer_events_nowait() noexcept;

    /**
     * @brief Graceful stop: joins peer_thread and write_thread. Idempotent.
     *        Sets the is_stopping() flag before joining; handlers should poll it.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief True when stop() has been called (write_stop flag is set).
     *        Primarily useful inside write handlers registered via set_write_handler().
     */
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── ZMQ messaging ─────────────────────────────────────────────────────────

    /// Broadcast raw data bytes to all consumers on the data socket.
    bool send(const void *data, size_t size);

    /// Send raw data bytes to a specific consumer via ROUTER (Bidir pattern).
    bool send_to(const std::string &identity, const void *data, size_t size);

    /// Send a typed ctrl frame to a specific consumer (queued through peer_thread).
    bool send_ctrl(const std::string &identity, std::string_view type,
                   const void *data, size_t size);

    // ── DataBlock write (SHM) — Queue mode ────────────────────────────────────

    /**
     * @brief Async: enqueue a write job; write_thread acquires slot, calls job, commits.
     *        Requires start() to have been called (write_thread must be running).
     *        Non-blocking for caller. Returns false if not started or no SHM.
     *
     * @tparam FlexZoneT  Flex zone type (must match channel schema; use void if none).
     * @tparam DataBlockT Slot data type (must match channel schema).
     */
    template <typename FlexZoneT, typename DataBlockT>
    bool push(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job);

    /**
     * @brief Sync: acquire slot and run job in the calling thread.
     *        Does not require start(). Blocks caller until slot acquired and job done.
     *        Returns false on no SHM or closed producer.
     *
     * @param timeout_ms  Slot-acquire timeout passed to the transaction context.
     */
    template <typename FlexZoneT, typename DataBlockT>
    bool synced_write(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job,
                      int timeout_ms = detail::kDefaultWriteSlotTimeoutMs);

    // ── DataBlock write (SHM) — Real-time mode ────────────────────────────────

    /**
     * @brief Install a persistent write handler; write_thread drives a continuous loop.
     *        Pass nullptr to remove the handler and return to Queue mode.
     *        Hot-swappable: the next write_thread iteration picks up the new handler.
     *        Logs LOGGER_INFO on every install and removal.
     *
     * @tparam FlexZoneT  Flex zone type (must match channel schema; use void if none).
     * @tparam DataBlockT Slot data type (must match channel schema).
     *
     * In the handler:
     *   - `ctx.is_stopping()` — check at natural loop checkpoints; return when true.
     *   - `ctx.txn.slots(timeout)` — iterate to acquire write slots.
     *   - `ctx.txn.publish()` — commit the current slot.
     *   - `ctx.flexzone()` — typed FlexZone access (when FlexZoneT != void).
     *   - Peer messaging via ctx.broadcast / ctx.send_to / ctx.connected_consumers.
     *
     * Handlers that block indefinitely will block stop(). Respect ctx.is_stopping().
     */
    template <typename FlexZoneT, typename DataBlockT>
    void set_write_handler(
        std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> fn);

    /// Returns true when a real-time write handler has been installed via set_write_handler().
    [[nodiscard]] bool has_realtime_handler() const noexcept;

    // ── Consumer list (thread-safe) ────────────────────────────────────────────

    /// Returns ZMQ identities of currently connected consumers (from HELLO/BYE tracking).
    [[nodiscard]] std::vector<std::string> connected_consumers() const;

    // ── Introspection ─────────────────────────────────────────────────────────

    [[nodiscard]] bool               is_valid() const;
    [[nodiscard]] const std::string &channel_name() const;
    [[nodiscard]] ChannelPattern     pattern() const;
    [[nodiscard]] bool               has_shm() const;
    DataBlockProducer               *shm() noexcept; ///< nullptr if !has_shm
    ChannelHandle                   &channel_handle();

    // ── HEP-CORE-0021: ZMQ Virtual Channel Node ───────────────────────────────

    /**
     * @brief Returns the ZmqQueue PUSH socket owned by this Producer (HEP-CORE-0021).
     * Non-null only when ProducerOptions::data_transport=="zmq" at create() time.
     * Null when data_transport=="shm". Lifetime is tied to this Producer.
     */
    [[nodiscard]] ZmqQueue *queue() noexcept;
    /// Number of ctrl queue items dropped due to max_depth overflow.
    [[nodiscard]] uint64_t ctrl_queue_dropped() const;

    // ── Queue data operations (delegated to internal QueueWriter) ──────────

    /// Acquire a writable slot buffer. Returns nullptr on timeout or no queue.
    [[nodiscard]] void *write_acquire(std::chrono::milliseconds timeout) noexcept;
    /// Publish the acquired slot (makes it visible to readers).
    void write_commit() noexcept;
    /// Discard the acquired slot without publishing. Loop continues.
    void write_discard() noexcept;
    /// Size of one data slot in bytes.
    [[nodiscard]] size_t queue_item_size() const noexcept;
    /// Ring/send buffer capacity (slot count).
    [[nodiscard]] size_t queue_capacity() const noexcept;
    /// Diagnostic counter snapshot. Thread-safe.
    [[nodiscard]] QueueMetrics queue_metrics() const noexcept;
    /// Reset all queue metric counters.
    void reset_queue_metrics() noexcept;

    // ── Queue lifecycle ─────────────────────────────────────────────────────

    /// Start the internal data queue. Idempotent.
    bool start_queue();
    /// Stop the internal data queue. Idempotent.
    void stop_queue();

    // ── Channel data operations (flexzone, checksum — SHM-specific) ─────────

    /// Writable pointer to the shared flexzone. nullptr if no flexzone or ZMQ transport.
    [[nodiscard]] void *write_flexzone() noexcept;
    /// Read-only pointer to the shared flexzone. nullptr if no flexzone or ZMQ transport.
    [[nodiscard]] const void *read_flexzone() const noexcept;
    /// Flexzone size in bytes. 0 if not configured or ZMQ transport.
    [[nodiscard]] size_t flexzone_size() const noexcept;
    /// Runtime toggle: enable/disable BLAKE2b checksum on write_commit(). No-op for ZMQ.
    void set_checksum_options(bool slot, bool fz) noexcept;
    /// Runtime toggle: enable/disable zero-fill of slot buffer on write_acquire() (SHM only).
    void set_always_clear_slot(bool enable) noexcept;
    /// Stamp the flexzone checksum after on_init() writes initial content. No-op for ZMQ.
    void sync_flexzone_checksum() noexcept;
    /// Overflow policy description for diagnostics (e.g. "shm_write", "zmq_push_drop").
    [[nodiscard]] std::string queue_policy_info() const;

    /// Returns the Messenger used by this Producer.
    [[nodiscard]] Messenger &messenger() const;

    /**
     * @brief Deregisters from broker, closes sockets and SHM. Called by destructor.
     * Idempotent.
     */
    void close();

    // ── Internal: establish local channel resources (used by create<>) ─────────

    /**
     * @brief Establish local channel resources: wire callbacks, create queues,
     *        resolve ephemeral endpoints (internal use by create() factories).
     */
    [[nodiscard]] static std::optional<Producer>
    establish_channel(Messenger &messenger, ChannelHandle channel,
                      std::unique_ptr<DataBlockProducer> shm_producer,
                      const ProducerOptions &opts);

  private:
    explicit Producer(std::unique_ptr<ProducerImpl> impl);
    std::unique_ptr<ProducerImpl> pImpl;

    // ── Non-template helpers for template method implementations ──────────────
    // Declared here, defined in .cpp; avoid exposing ProducerImpl details to header.

    [[nodiscard]] bool                   _has_shm() const noexcept;
    [[nodiscard]] bool                   _is_started_and_has_shm() const noexcept;
    [[nodiscard]] ProducerMessagingFacade &_messaging_facade() const;
    void _enqueue_write_job(std::function<void()> job);
    void _store_write_handler(std::shared_ptr<InternalWriteHandlerFn> h) noexcept;
};

// ============================================================================
// Producer template factory implementation
// ============================================================================

template <typename FlexZoneT, typename DataBlockT>
std::optional<Producer>
Producer::create(Messenger &messenger, const ProducerOptions &opts)
{
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    // Throws SchemaValidationException on size or BLDS hash mismatch; no-op if empty.
    schema::validate_named_schema_from_env<DataBlockT, FlexZoneT>(opts.schema_id);

    // Validate SHM sizes at compile time against the config
    if (opts.has_shm)
    {
        if constexpr (!std::is_void_v<FlexZoneT>)
        {
            if (opts.shm_config.flex_zone_size < sizeof(FlexZoneT))
            {
                return std::nullopt;
            }
        }
        size_t slot_size = opts.shm_config.effective_logical_unit_size();
        if (slot_size < sizeof(DataBlockT))
        {
            return std::nullopt;
        }
    }

    // ── Compute BLDS for schema annotation (HEP-CORE-0016 Phase 3) ───────────
    std::string schema_blds_str;
    if constexpr (schema::has_schema_registry_v<DataBlockT>)
    {
        schema_blds_str =
            schema::generate_schema_info<DataBlockT>("", schema::SchemaVersion{}).blds;
    }

    // Create ZMQ channel
    ChannelRegistrationOptions ch_opts;
    ch_opts.pattern           = opts.pattern;
    ch_opts.has_shared_memory = opts.has_shm;
    ch_opts.schema_hash       = opts.schema_hash;
    ch_opts.schema_version    = opts.schema_version;
    ch_opts.timeout_ms        = opts.timeout_ms;
    ch_opts.role_name        = opts.role_name;
    ch_opts.role_uid         = opts.role_uid;
    ch_opts.schema_id         = opts.schema_id;
    ch_opts.schema_blds       = schema_blds_str;
    ch_opts.data_transport    = opts.data_transport;
    ch_opts.zmq_node_endpoint = opts.zmq_node_endpoint;
    ch_opts.inbox_endpoint    = opts.inbox_endpoint;
    auto ch = messenger.create_channel(opts.channel_name, ch_opts);
    if (!ch.has_value())
    {
        return std::nullopt;
    }

    // Create typed DataBlock if requested
    std::unique_ptr<DataBlockProducer> shm_producer;
    if (opts.has_shm)
    {
        shm_producer = create_datablock_producer<FlexZoneT, DataBlockT>(
            opts.channel_name, DataBlockPolicy::RingBuffer, opts.shm_config);
        if (!shm_producer)
        {
            return std::nullopt;
        }
    }

    return Producer::establish_channel(messenger, std::move(*ch), std::move(shm_producer), opts);
}

// ============================================================================
// Producer SHM template method implementations
// ============================================================================

template <typename FlexZoneT, typename DataBlockT>
bool Producer::push(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job)
{
    if (!_is_started_and_has_shm())
        return false;
    ProducerMessagingFacade *fac = &_messaging_facade();
    _enqueue_write_job([job = std::move(job), fac]() {
        DataBlockProducer *shm = fac->fn_get_shm(fac->context);
        if (!shm)
            return;
        shm->with_transaction<FlexZoneT, DataBlockT>(
            std::chrono::milliseconds(detail::kDefaultWriteSlotTimeoutMs),
            [&job, fac](WriteTransactionContext<FlexZoneT, DataBlockT> &txn) {
                WriteProcessorContext<FlexZoneT, DataBlockT> ctx{txn, *fac};
                job(ctx);
            });
    });
    return true;
}

template <typename FlexZoneT, typename DataBlockT>
bool Producer::synced_write(
    std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job, int timeout_ms)
{
    if (!_has_shm())
        return false;
    ProducerMessagingFacade *fac = &_messaging_facade();
    DataBlockProducer       *shm = fac->fn_get_shm(fac->context);
    if (!shm)
        return false;
    shm->with_transaction<FlexZoneT, DataBlockT>(
        std::chrono::milliseconds(timeout_ms),
        [&job, fac](WriteTransactionContext<FlexZoneT, DataBlockT> &txn) {
            WriteProcessorContext<FlexZoneT, DataBlockT> ctx{txn, *fac};
            job(ctx);
        });
    return true;
}

template <typename FlexZoneT, typename DataBlockT>
void Producer::set_write_handler(
    std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> fn)
{
    if (!fn)
    {
        _store_write_handler(nullptr);
        return;
    }
    auto wrapped = std::make_shared<InternalWriteHandlerFn>(
        [fn = std::move(fn)](ProducerMessagingFacade &fac) {
            DataBlockProducer *shm = fac.fn_get_shm(fac.context);
            if (!shm)
                return;
            shm->with_transaction<FlexZoneT, DataBlockT>(
                std::chrono::milliseconds(detail::kRealtimeWritePollMs),
                [&fn, &fac](WriteTransactionContext<FlexZoneT, DataBlockT> &txn) {
                    WriteProcessorContext<FlexZoneT, DataBlockT> ctx{txn, fac};
                    fn(ctx);
                });
        });
    _store_write_handler(std::move(wrapped));
}

// ============================================================================
// ManagedProducer — lifecycle-integrated wrapper
// ============================================================================

/**
 * @class ManagedProducer
 * @brief Wraps a Producer for registration with LifecycleGuard.
 *
 * get_module_def() returns a ModuleDef that, when the lifecycle system starts it,
 * creates the Producer (calling start()) and on shutdown calls stop() + close().
 */
class PYLABHUB_UTILS_EXPORT ManagedProducer
{
  public:
    explicit ManagedProducer(Messenger &messenger, ProducerOptions opts);
    ~ManagedProducer();
    ManagedProducer(ManagedProducer &&) noexcept;
    ManagedProducer &operator=(ManagedProducer &&) noexcept;
    ManagedProducer(const ManagedProducer &) = delete;
    ManagedProducer &operator=(const ManagedProducer &) = delete;

    /**
     * @brief Returns a ModuleDef for this producer.
     * MUST be called before LifecycleGuard construction.
     * Adds dependency on "pylabhub::hub::DataExchangeHub" automatically.
     */
    [[nodiscard]] pylabhub::utils::ModuleDef get_module_def();

    /// Returns the Producer after lifecycle startup has run.
    Producer &get();

    [[nodiscard]] bool is_initialized() const noexcept;

  private:
    Messenger      *messenger_;
    ProducerOptions opts_;
    std::optional<Producer> producer_;
    std::string     module_key_;

    static void s_startup(const char *key);
    static void s_shutdown(const char *key);
};

} // namespace pylabhub::hub
