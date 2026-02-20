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
 * to Queue. Mode is queryable via `shm_processing_mode()`.
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
#include "utils/messenger.hpp"
#include "utils/module_def.hpp"

#include <nlohmann/json.hpp>

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
// ShmProcessingMode
// ============================================================================

/**
 * @enum ShmProcessingMode
 * @brief Indicates whether the producer/consumer SHM thread is in Queue or Real-time mode.
 */
enum class ShmProcessingMode
{
    Queue,   ///< Caller-driven: push() / synced_write() / pull()
    RealTime ///< Framework-driven: set_write_handler() / set_read_handler() continuous loop
};

// ============================================================================
// ProducerMessagingFacade — type-erased messaging bridge (internal use)
// ============================================================================

/**
 * @struct ProducerMessagingFacade
 * @brief ABI-stable bridge between WriteProcessorContext<F,D> (header template) and
 *        ProducerImpl internals (defined in .cpp). Function pointers are filled by
 *        Producer::create_from_parts(); context points to the ProducerImpl on the heap.
 *
 * This is an implementation detail exposed in the header solely so that the template
 * WriteProcessorContext<F,D> can reference it without knowing ProducerImpl.
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
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends CONSUMER_DIED_NOTIFY (Cat 2).
    using ConsumerDiedCallback =
        std::function<void(uint64_t consumer_pid, const std::string &reason)>;
    void on_consumer_died(ConsumerDiedCallback cb);

    /// Called from Messenger worker thread on CHANNEL_ERROR_NOTIFY (Cat 1) or
    /// CHANNEL_EVENT_NOTIFY (Cat 2).
    using ChannelErrorCallback =
        std::function<void(const std::string &event, const nlohmann::json &details)>;
    void on_channel_error(ChannelErrorCallback cb);

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start peer_thread (ctrl monitor) and write_thread (SHM).
     * @return true if started successfully; false if already running or not valid.
     */
    bool start();

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

    /// Returns the current SHM processing mode (Queue or RealTime).
    [[nodiscard]] ShmProcessingMode shm_processing_mode() const noexcept;

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

    /// Returns the Messenger used by this Producer.
    [[nodiscard]] Messenger &messenger() const;

    /**
     * @brief Deregisters from broker, closes sockets and SHM. Called by destructor.
     * Idempotent.
     */
    void close();

    // ── Internal factory helper (used by template create<>) ───────────────────

    /**
     * @brief Assemble a Producer from pre-created parts (internal use by templates).
     */
    [[nodiscard]] static std::optional<Producer>
    create_from_parts(Messenger &messenger, ChannelHandle channel,
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

    // Create ZMQ channel
    auto ch = messenger.create_channel(opts.channel_name, opts.pattern, opts.has_shm,
                                        opts.schema_hash, opts.schema_version, opts.timeout_ms);
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

    return Producer::create_from_parts(messenger, std::move(*ch), std::move(shm_producer), opts);
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
