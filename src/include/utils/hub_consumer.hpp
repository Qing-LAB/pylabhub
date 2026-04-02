#pragma once
/**
 * @file hub_consumer.hpp
 * @brief Active consumer service: owns both ZMQ transport (ChannelHandle) and
 *        shared memory (DataBlockConsumer), with dedicated internal threads.
 *
 * Consumer is an active service object. It manages:
 *   - data_thread:  polls the SUB/PULL data socket for ZMQ data frames.
 *   - ctrl_thread:  polls the DEALER ctrl socket for control frames from the producer.
 *   - shm_thread:   polls the DataBlock ring buffer for new slots (if has_shm).
 *
 * ## SHM Processing Modes
 *
 * **Queue mode** (default): shm_thread sleeps; caller acquires slots directly.
 *   - `pull<F,D>(job)` — sync, blocks caller until slot available and job completes.
 *                        Does not require start(). Called from the caller's thread.
 *
 * **Real-time mode**: shm_thread drives a continuous processing loop.
 *   - `set_read_handler<F,D>(fn)` — install handler; thread loops calling fn per slot.
 *   - `set_read_handler<F,D>(nullptr)` — remove handler; returns to Queue mode.
 *
 * Mode is selected implicitly: installing a handler enters Real-time; removing it returns
 * to Queue mode. Queryable via `has_realtime_handler()`.
 *
 * Both modes receive a fully-typed `ReadProcessorContext<FlexZoneT, DataBlockT>` that
 * bundles: typed const FlexZone access, the full ReadTransactionContext, ctrl messaging,
 * and a shutdown signal. Type safety is enforced at the call site via template parameters.
 *
 * One Consumer instance per channel per process. Use with LifecycleGuard (ManagedConsumer)
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

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

namespace pylabhub::hub
{

// ============================================================================
// ConsumerMessagingFacade — type-erased messaging bridge (internal use)
// ============================================================================

/**
 * @struct ConsumerMessagingFacade
 * @brief ABI-stable bridge between ReadProcessorContext<F,D> (header template) and
 *        ConsumerImpl internals (defined in .cpp). Function pointers are filled by
 *        Consumer::establish_channel(); context points to the ConsumerImpl on the heap.
 *
 * **Internal use only — do not use directly.**
 * This struct is exposed in the header solely so that the template ReadProcessorContext<F,D>
 * can reference it without knowing ConsumerImpl. It is an implementation detail, not part
 * of the public API.
 *
 * **ABI note:** The function pointer fields and their signatures are **frozen** for the
 * lifetime of the pylabhub-utils shared library ABI. Any change to field order, types, or
 * count requires a SOVERSION bump. New fields must be appended at the end, never inserted.
 * Callers may rely on all fields being null-initialized (defaulted to nullptr) in the struct.
 */
struct PYLABHUB_UTILS_EXPORT ConsumerMessagingFacade
{
    /// Returns the DataBlockConsumer* (nullptr if SHM not configured).
    DataBlockConsumer *(*fn_get_shm)(void *ctx){nullptr};
    /// Sends a typed ctrl frame to the producer (queued through ctrl_thread when running).
    bool (*fn_send_ctrl)(void *ctx, const char *type, const void *data, size_t size){nullptr};
    /// Returns true when the consumer's stop flag is set (running == false).
    bool (*fn_is_stopping)(void *ctx){nullptr};
    /// Returns the Messenger* used by this Consumer.
    Messenger *(*fn_messenger)(void *ctx){nullptr};
    /// Returns the channel name string.
    const std::string &(*fn_channel_name)(void *ctx){nullptr};
    /// Opaque pointer to ConsumerImpl.
    void *context{nullptr};
};

/// Internal handler type stored in ConsumerImpl for the real-time read loop.
/// Receives the facade by reference each invocation; captures typed F,D in the closure.
using InternalReadHandlerFn = std::function<void(ConsumerMessagingFacade &)>;

// ============================================================================
// ReadProcessorContext<FlexZoneT, DataBlockT>
// ============================================================================

/**
 * @struct ReadProcessorContext
 * @brief Fully-typed context passed to read handlers and pull jobs.
 *
 * Bundles:
 *   - `txn`      — ReadTransactionContext<FlexZoneT, DataBlockT> for slot + flexzone access.
 *   - `flexzone()` — convenience const typed flexzone accessor (when FlexZoneT != void).
 *   - `is_stopping()` — shutdown signal (check at natural loop checkpoints in handlers).
 *   - Ctrl messaging: `send_ctrl` to producer.
 *   - Broker access: `messenger()`, `report_checksum_error`.
 *
 * FlexZoneT is const-access only (consumer never modifies the shared FlexZone header).
 * FlexZone and DataBlock types are fixed at `Consumer::connect<FlexZoneT, DataBlockT>()`
 * time and validated against the channel schema at attachment.
 */
template <typename FlexZoneT, typename DataBlockT>
struct ReadProcessorContext
{
    ReadTransactionContext<FlexZoneT, DataBlockT> &txn;
    ConsumerMessagingFacade                       &facade;

    // ── FlexZone access (const — consumer reads only) ─────────────────────────

    /**
     * @brief Typed const FlexZone access. Only available when FlexZoneT is not void.
     * @return Const reference to the FlexZone in shared memory (producer-written).
     */
    template <typename F = FlexZoneT>
    auto flexzone() const noexcept -> std::enable_if_t<!std::is_void_v<F>, const F &>
    {
        return txn.flexzone().get();
    }

    // ── Shutdown signal ───────────────────────────────────────────────────────

    /**
     * @brief True when the Consumer is stopping (stop() has been called).
     * Check at natural processing checkpoints in real-time handler loops.
     */
    [[nodiscard]] bool is_stopping() const noexcept
    {
        return facade.fn_is_stopping(facade.context);
    }

    // ── Ctrl messaging (to producer) ──────────────────────────────────────────

    /// Send a typed ctrl frame to the producer (queued through ctrl_thread when running).
    bool send_ctrl(const char *type, const void *data, size_t size)
    {
        return facade.fn_send_ctrl(facade.context, type, data, size);
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
// ConsumerOptions
// ============================================================================

/**
 * @struct ConsumerOptions
 * @brief Configuration for connecting a Consumer active service.
 */
struct ConsumerOptions
{
    std::string channel_name;

    /// Expected schema hash (raw hex string); empty = accept any.
    std::string expected_schema_hash{};

    /// SHM attachment: must match producer's shm_config.shared_secret (0 = skip SHM).
    uint64_t shm_shared_secret{0};

    /// SHM config validation (nullopt = no layout check beyond secret).
    std::optional<DataBlockConfig> expected_shm_config{};

    /// Consumer identity — written into the SHM heartbeat slot (empty = anonymous).
    std::string consumer_uid{};   ///< Unique ID (hex string, max 39 chars)
    std::string consumer_name{};  ///< Human-readable name (max 31 chars)

    int timeout_ms{5000};

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    /// Optional named schema ID (e.g. `"lab.sensors.temperature.raw@1"`).
    /// When non-empty, `connect<FlexZoneT, DataBlockT>()` validates sizeof and BLDS hash
    /// against the schema loaded from PYLABHUB_SCHEMA_PATH (or default search dirs).
    /// Throws SchemaValidationException on mismatch. No-op when empty.
    std::string expected_schema_id{};

    // ── HEP-CORE-0021: ZMQ Virtual Channel Node ───────────────────────────────
    /// Schema for ZMQ PULL frames (required when data_transport=="zmq").
    /// Empty schema → LOGGER_ERROR + Consumer::connect returns nullopt.
    /// Use {{"bytes",1,N}} as a single-blob schema for opaque N-byte payloads.
    std::vector<ZmqSchemaField> zmq_schema{};
    /// "aligned" (ctypes.LittleEndianStructure default) or "packed" (no padding).
    /// Must match the producer's packing.
    std::string zmq_packing{"aligned"};
    /// Internal receive-buffer depth for ZmqQueue PULL.
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    // ── Inbox (advertised to broker for ROLE_INFO_REQ discovery) ──────────────
    std::string inbox_endpoint{};       ///< ROUTER bind endpoint. Empty = no inbox.
    std::string inbox_schema_json{};    ///< JSON schema for ROLE_INFO_REQ.
    std::string inbox_packing{};        ///< "aligned" or "packed".
    std::string inbox_checksum{};       ///< "enforced", "manual", "none".

    // ── Queue abstraction (Phase 2) ──────────────────────────────────────────
    /// Slot data size in bytes (from engine type_sizeof("InSlotFrame")). Required for ShmQueue wrapper.
    size_t item_size{0};
    /// Flexzone size in bytes (page-aligned). 0 = no flexzone.
    size_t flexzone_size{0};
    /// Checksum policy for this channel. Applied to all queues via set_checksum_policy().
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    /// Verify flexzone checksum (SHM-specific). Applied via set_flexzone_checksum().
    bool flexzone_checksum{true};

    /// Max depth of ctrl send queue before oldest items are dropped. 0 = unbounded.
    size_t ctrl_queue_max_depth{256};
    /// Peer silence timeout before on_peer_dead fires (ms). 0 = disabled.
    int    peer_dead_timeout_ms{30000};

    // ── Transport arbitration (Phase 6/7) ────────────────────────────────────
    /// Declares the consumer's intended data transport to the broker.
    /// "shm" — consumer expects SHM data; broker rejects if channel uses ZMQ.
    /// "zmq" — consumer expects ZMQ data; broker rejects if channel uses SHM.
    /// ""    — no declaration; broker skips transport mismatch check (processor use-case:
    ///         transport is auto-discovered from CONSUMER_REG_ACK data_transport field).
    /// Set this explicitly; do NOT rely on zmq_schema being set as a proxy.
    std::string queue_type{};
};

// ============================================================================
// Consumer
// ============================================================================

struct ConsumerImpl;

/// Default timeouts (accessible from template code in this header).
namespace detail
{
inline constexpr int kDefaultReadSlotTimeoutMs = 5000; ///< Timeout for pull() jobs
inline constexpr int kRealtimeReadPollMs        = 50;  ///< Slot poll interval in real-time mode
} // namespace detail

/**
 * @class Consumer
 * @brief Active consumer service subscribing to a published channel.
 *
 * Created via Consumer::connect() or Consumer::connect<FlexZoneT, DataBlockT>().
 * Optional active mode: call start() to launch data_thread (ZMQ data),
 * ctrl_thread (ZMQ ctrl messages from producer), and shm_thread (DataBlock polling).
 */
class PYLABHUB_UTILS_EXPORT Consumer
{
  public:
    // ── Factories ──────────────────────────────────────────────────────────────

    /**
     * @brief Non-template factory: no compile-time schema validation.
     *        SHM attached without schema type association.
     */
    [[nodiscard]] static std::optional<Consumer>
    connect(Messenger &messenger, const ConsumerOptions &opts);

    /**
     * @brief Template factory: validates SHM layout against FlexZoneT/DataBlockT sizes.
     */
    template <typename FlexZoneT, typename DataBlockT>
    [[nodiscard]] static std::optional<Consumer>
    connect(Messenger &messenger, const ConsumerOptions &opts);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~Consumer();
    Consumer(Consumer &&) noexcept;
    Consumer &operator=(Consumer &&) noexcept;
    Consumer(const Consumer &) = delete;
    Consumer &operator=(const Consumer &) = delete;

    // ── Callbacks — set BEFORE start() ────────────────────────────────────────

    /// Called from data_thread when a ZMQ data frame arrives from the producer.
    using DataCallback = std::function<void(std::span<const std::byte> data)>;
    void on_zmq_data(DataCallback cb);

    /// Called from ctrl_thread when the producer sends a control message.
    using CtrlCallback =
        std::function<void(std::string_view type, std::span<const std::byte> data)>;
    void on_producer_message(CtrlCallback cb);

    /// Called from Messenger worker thread when broker sends CHANNEL_CLOSING_NOTIFY.
    /// Graceful notification — the script should finish pending work then call api.stop().
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends FORCE_SHUTDOWN.
    /// Grace period expired — force immediate shutdown.
    void on_force_shutdown(std::function<void()> cb);

    /// Called from Messenger worker thread on CHANNEL_ERROR_NOTIFY (Cat 1) or
    /// CHANNEL_EVENT_NOTIFY (Cat 2).
    using ChannelErrorCallback =
        std::function<void(const std::string &event, const nlohmann::json &details)>;
    void on_channel_error(ChannelErrorCallback cb);

    /// Callback fired when the peer (producer) has been silent for peer_dead_timeout_ms.
    /// Called from ctrl_thread — callback must be thread-safe.
    void on_peer_dead(std::function<void()> cb);

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start data_thread, ctrl_thread, and shm_thread (if has_shm).
     * @return true if started successfully; false if already running or not valid.
     */
    bool start();

    /**
     * @brief Embedded mode: set running=true WITHOUT launching data_thread/ctrl_thread/shm_thread.
     * Use when the caller (role ZMQ thread) drives all ZMQ polling itself via
     * data_zmq_socket_handle() / ctrl_zmq_socket_handle() + handle_*_events_nowait().
     * @return true if successfully transitioned to running; false if already running or invalid.
     */
    [[nodiscard]] bool start_embedded() noexcept;

    /**
     * @brief Returns the raw ZMQ SUB/PULL data socket handle for use in zmq_pollitem_t.
     * Returns nullptr for Bidir pattern (data arrives via ctrl socket) or if invalid.
     * MUST be called only in embedded mode.
     */
    [[nodiscard]] void *data_zmq_socket_handle() const noexcept;

    /**
     * @brief Returns the raw ZMQ DEALER ctrl socket handle for use in zmq_pollitem_t.
     * Returns nullptr if not valid.
     * MUST be called only in embedded mode.
     */
    [[nodiscard]] void *ctrl_zmq_socket_handle() const noexcept;

    /**
     * @brief Non-blocking: process all pending POLLIN on the data socket.
     * Fires on_zmq_data callback for each received data frame.
     * MUST be called from the socket-owning thread (role ZMQ thread only).
     */
    void handle_data_events_nowait() noexcept;

    /**
     * @brief Non-blocking: drain ctrl send queue + process all pending POLLIN on ctrl socket.
     * Fires on_producer_message (and on_zmq_data for Bidir) callbacks.
     * MUST be called from the socket-owning thread (role ZMQ thread only).
     */
    void handle_ctrl_events_nowait() noexcept;

    /**
     * @brief Graceful stop: joins all threads. Idempotent.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief True when stop() has been called (running flag is false).
     *        Primarily useful inside read handlers registered via set_read_handler().
     */
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── ZMQ messaging (to producer) ────────────────────────────────────────────

    /// Send a data frame to the producer (Bidir pattern only).
    bool send(const void *data, size_t size);

    /// Send a ctrl frame to the producer.
    bool send_ctrl(std::string_view type, const void *data, size_t size);

    // ── DataBlock read (SHM) — Queue mode ─────────────────────────────────────

    /**
     * @brief Sync: acquire slot and run job in the calling thread.
     *        Does not require start(). Blocks caller until slot available and job done.
     *        Returns false on no SHM or closed consumer.
     *
     * @tparam FlexZoneT  Flex zone type (must match channel schema; use void if none).
     * @tparam DataBlockT Slot data type (must match channel schema).
     * @param timeout_ms  Slot-acquire timeout passed to the transaction context.
     */
    template <typename FlexZoneT, typename DataBlockT>
    bool pull(std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> job,
              int timeout_ms = detail::kDefaultReadSlotTimeoutMs);

    // ── DataBlock read (SHM) — Real-time mode ─────────────────────────────────

    /**
     * @brief Install a persistent read handler; shm_thread drives a continuous loop.
     *        Pass nullptr to remove the handler and return to Queue mode.
     *        Hot-swappable: the next shm_thread iteration picks up the new handler.
     *
     * @tparam FlexZoneT  Flex zone type (must match channel schema; use void if none).
     * @tparam DataBlockT Slot data type (must match channel schema).
     *
     * In the handler:
     *   - `ctx.is_stopping()` — check at natural loop checkpoints; return when true.
     *   - `ctx.txn.slots(timeout)` — iterate to acquire read slots.
     *   - `ctx.flexzone()` — typed const FlexZone access (when FlexZoneT != void).
     *   - Ctrl messaging via ctx.send_ctrl.
     *
     * Handlers that block indefinitely will block stop(). Respect ctx.is_stopping().
     */
    template <typename FlexZoneT, typename DataBlockT>
    void set_read_handler(
        std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> fn);

    /// Returns true when a real-time read handler has been installed via set_read_handler().
    [[nodiscard]] bool has_realtime_handler() const noexcept;

    // ── Introspection ─────────────────────────────────────────────────────────

    [[nodiscard]] bool               is_valid() const;
    [[nodiscard]] const std::string &channel_name() const;
    [[nodiscard]] ChannelPattern     pattern() const;
    [[nodiscard]] bool               has_shm() const;
    DataBlockConsumer               *shm() noexcept; ///< nullptr if !has_shm
    ChannelHandle                   &channel_handle();

    // ── HEP-CORE-0021: ZMQ Virtual Channel Node ───────────────────────────────

    /**
     * @brief Data transport type discovered from broker: "shm" or "zmq".
     * Set from the broker's DISC_ACK when connect() completes.
     */
    [[nodiscard]] const std::string &data_transport() const noexcept;

    /**
     * @brief ZMQ PUSH bind endpoint for the ZMQ virtual channel (HEP-CORE-0021).
     * Non-empty only when data_transport()=="zmq". Discovered from the broker's DISC_ACK.
     * Use this endpoint to create a ZmqQueue PULL (connect) socket.
     */
    [[nodiscard]] const std::string &zmq_node_endpoint() const noexcept;

    /**
     * @brief Returns the ZmqQueue PULL socket owned by this Consumer (HEP-CORE-0021).
     * Non-null only when data_transport()=="zmq" at connect() time.
     * Null when data_transport()=="shm". Lifetime is tied to this Consumer.
     */
    [[nodiscard]] ZmqQueue *queue() noexcept;
    /// Number of ctrl queue items dropped due to max_depth overflow.
    [[nodiscard]] uint64_t ctrl_queue_dropped() const;

    // ── Queue data operations (delegated to internal QueueReader) ──────────

    /// Acquire the next readable slot. Returns nullptr on timeout or no queue.
    [[nodiscard]] const void *read_acquire(std::chrono::milliseconds timeout) noexcept;
    /// Release the slot acquired by read_acquire().
    void read_release() noexcept;
    /// Monotonic sequence number of the last acquired slot.
    [[nodiscard]] uint64_t last_seq() const noexcept;
    /// Size of one data slot in bytes.
    [[nodiscard]] size_t queue_item_size() const noexcept;
    /// Ring/recv buffer capacity (slot count).
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

    /// Read-only pointer to the shared flexzone. nullptr if no flexzone or ZMQ transport.
    [[nodiscard]] const void *read_flexzone() const noexcept;
    /// Flexzone size in bytes. 0 if not configured or ZMQ transport.
    [[nodiscard]] size_t flexzone_size() const noexcept;
    /// Runtime toggle: enable/disable BLAKE2b verification on read_acquire(). No-op for ZMQ.
    void set_verify_checksum(bool slot, bool fz) noexcept;
    /// Overflow policy description for diagnostics (e.g. "shm_read", "zmq_pull_ring_64").
    [[nodiscard]] std::string queue_policy_info() const;

    /// Returns the Messenger used by this Consumer.
    [[nodiscard]] Messenger &messenger() const;

    /**
     * @brief Deregisters from broker, closes sockets and SHM. Called by destructor.
     * Idempotent.
     */
    void close();

    // ── Internal: establish local channel resources (used by connect<>) ─────────

    [[nodiscard]] static std::optional<Consumer>
    establish_channel(Messenger &messenger, ChannelHandle channel,
                       std::unique_ptr<DataBlockConsumer> shm_consumer,
                       const ConsumerOptions &opts);

  private:
    explicit Consumer(std::unique_ptr<ConsumerImpl> impl);
    std::unique_ptr<ConsumerImpl> pImpl;

    // ── Non-template helpers for template method implementations ──────────────
    // Declared here, defined in .cpp; avoid exposing ConsumerImpl details to header.

    [[nodiscard]] bool                    _has_shm() const noexcept;
    [[nodiscard]] ConsumerMessagingFacade &_messaging_facade() const;
    void _store_read_handler(std::shared_ptr<InternalReadHandlerFn> h) noexcept;
};

// ============================================================================
// Consumer template factory implementation
// ============================================================================

template <typename FlexZoneT, typename DataBlockT>
std::optional<Consumer>
Consumer::connect(Messenger &messenger, const ConsumerOptions &opts)
{
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    // Throws SchemaValidationException on size or BLDS hash mismatch; no-op if empty.
    schema::validate_named_schema_from_env<DataBlockT, FlexZoneT>(opts.expected_schema_id);

    // Connect the ZMQ channel (sends HELLO, gets ConsumerInfo from broker).
    const std::string queue_type_str = opts.queue_type;
    auto ch = messenger.connect_channel(opts.channel_name, opts.timeout_ms,
                                         opts.expected_schema_hash,
                                         opts.consumer_uid, opts.consumer_name,
                                         opts.expected_schema_id, queue_type_str);
    if (!ch.has_value())
    {
        return std::nullopt;
    }

    // Attach to SHM if producer has it and secret is provided.
    std::unique_ptr<DataBlockConsumer> shm_consumer;
    if (ch->has_shm() && opts.shm_shared_secret != 0)
    {
        if (opts.expected_shm_config.has_value())
        {
            const DataBlockConfig &cfg = *opts.expected_shm_config;

            // Validate SHM sizes at compile time against the expected config.
            if constexpr (!std::is_void_v<FlexZoneT>)
            {
                if (cfg.flex_zone_size < sizeof(FlexZoneT))
                {
                    return std::nullopt;
                }
            }
            size_t slot_size = cfg.effective_logical_unit_size();
            if (slot_size < sizeof(DataBlockT))
            {
                return std::nullopt;
            }

            // Template factory validates both schema types + expected config.
            const char *uid  = opts.consumer_uid.empty()  ? nullptr : opts.consumer_uid.c_str();
            const char *cnam = opts.consumer_name.empty() ? nullptr : opts.consumer_name.c_str();
            shm_consumer = find_datablock_consumer<FlexZoneT, DataBlockT>(
                ch->shm_name(), opts.shm_shared_secret, cfg, uid, cnam);
        }
        else
        {
            // No config check requested — use impl with null schemas.
            const char *uid  = opts.consumer_uid.empty()  ? nullptr : opts.consumer_uid.c_str();
            const char *cnam = opts.consumer_name.empty() ? nullptr : opts.consumer_name.c_str();
            shm_consumer = find_datablock_consumer_impl(ch->shm_name(),
                                                         opts.shm_shared_secret,
                                                         nullptr, nullptr, nullptr, uid, cnam);
        }
        // A nullptr shm_consumer is acceptable — means secret mismatch or SHM unavailable.
        // ZMQ transport still works.
    }

    return Consumer::establish_channel(messenger, std::move(*ch), std::move(shm_consumer),
                                         opts);
}

// ============================================================================
// Consumer SHM template method implementations
// ============================================================================

template <typename FlexZoneT, typename DataBlockT>
bool Consumer::pull(std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> job,
                    int timeout_ms)
{
    if (!_has_shm())
        return false;
    ConsumerMessagingFacade *fac = &_messaging_facade();
    DataBlockConsumer       *shm = fac->fn_get_shm(fac->context);
    if (!shm)
        return false;
    shm->with_transaction<FlexZoneT, DataBlockT>(
        std::chrono::milliseconds(timeout_ms),
        [&job, fac](ReadTransactionContext<FlexZoneT, DataBlockT> &txn) {
            ReadProcessorContext<FlexZoneT, DataBlockT> ctx{txn, *fac};
            job(ctx);
        });
    return true;
}

template <typename FlexZoneT, typename DataBlockT>
void Consumer::set_read_handler(
    std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> fn)
{
    if (!fn)
    {
        _store_read_handler(nullptr);
        return;
    }
    auto wrapped = std::make_shared<InternalReadHandlerFn>(
        [fn = std::move(fn)](ConsumerMessagingFacade &fac) {
            DataBlockConsumer *shm = fac.fn_get_shm(fac.context);
            if (!shm)
                return;
            shm->with_transaction<FlexZoneT, DataBlockT>(
                std::chrono::milliseconds(detail::kRealtimeReadPollMs),
                [&fn, &fac](ReadTransactionContext<FlexZoneT, DataBlockT> &txn) {
                    ReadProcessorContext<FlexZoneT, DataBlockT> ctx{txn, fac};
                    fn(ctx);
                });
        });
    _store_read_handler(std::move(wrapped));
}

// ============================================================================
// ManagedConsumer — lifecycle-integrated wrapper
// ============================================================================

/**
 * @class ManagedConsumer
 * @brief Wraps a Consumer for registration with LifecycleGuard.
 *
 * get_module_def() returns a ModuleDef that, when the lifecycle system starts it,
 * creates the Consumer (calling start()) and on shutdown calls stop() + close().
 */
class PYLABHUB_UTILS_EXPORT ManagedConsumer
{
  public:
    explicit ManagedConsumer(Messenger &messenger, ConsumerOptions opts);
    ~ManagedConsumer();
    ManagedConsumer(ManagedConsumer &&) noexcept;
    ManagedConsumer &operator=(ManagedConsumer &&) noexcept;
    ManagedConsumer(const ManagedConsumer &) = delete;
    ManagedConsumer &operator=(const ManagedConsumer &) = delete;

    /**
     * @brief Returns a ModuleDef for this consumer.
     * MUST be called before LifecycleGuard construction.
     * Adds dependency on "pylabhub::hub::DataExchangeHub" automatically.
     */
    [[nodiscard]] pylabhub::utils::ModuleDef get_module_def();

    /// Returns the Consumer after lifecycle startup has run.
    Consumer &get();

    [[nodiscard]] bool is_initialized() const noexcept;

  private:
    Messenger      *messenger_;
    ConsumerOptions opts_;
    std::optional<Consumer> consumer_;
    std::string     module_key_;

    static void s_startup(const char *key, void * /*userdata*/);
    static void s_shutdown(const char *key, void * /*userdata*/);
};

} // namespace pylabhub::hub
