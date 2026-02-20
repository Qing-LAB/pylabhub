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
 * to Queue. Mode is queryable via `shm_processing_mode()`.
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

// hub_producer.hpp is included for shared types (ShmProcessingMode) and because the
// umbrella header includes both in order. No circular include: producer does not
// include consumer.
#include "utils/hub_producer.hpp"

#include "utils/channel_handle.hpp"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/messenger.hpp"
#include "utils/module_def.hpp"

#include <nlohmann/json.hpp>

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
 *        Consumer::connect_from_parts(); context points to the ConsumerImpl on the heap.
 *
 * This is an implementation detail exposed in the header solely so that the template
 * ReadProcessorContext<F,D> can reference it without knowing ConsumerImpl.
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

    int timeout_ms{5000};
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
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread on CHANNEL_ERROR_NOTIFY (Cat 1) or
    /// CHANNEL_EVENT_NOTIFY (Cat 2).
    using ChannelErrorCallback =
        std::function<void(const std::string &event, const nlohmann::json &details)>;
    void on_channel_error(ChannelErrorCallback cb);

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start data_thread, ctrl_thread, and shm_thread (if has_shm).
     * @return true if started successfully; false if already running or not valid.
     */
    bool start();

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

    /// Returns the current SHM processing mode (Queue or RealTime).
    [[nodiscard]] ShmProcessingMode shm_processing_mode() const noexcept;

    // ── Introspection ─────────────────────────────────────────────────────────

    [[nodiscard]] bool               is_valid() const;
    [[nodiscard]] const std::string &channel_name() const;
    [[nodiscard]] ChannelPattern     pattern() const;
    [[nodiscard]] bool               has_shm() const;
    DataBlockConsumer               *shm() noexcept; ///< nullptr if !has_shm
    ChannelHandle                   &channel_handle();

    /// Returns the Messenger used by this Consumer.
    [[nodiscard]] Messenger &messenger() const;

    /**
     * @brief Deregisters from broker, closes sockets and SHM. Called by destructor.
     * Idempotent.
     */
    void close();

    // ── Internal factory helper (used by template connect<>) ──────────────────

    [[nodiscard]] static std::optional<Consumer>
    connect_from_parts(Messenger &messenger, ChannelHandle channel,
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

    // Connect the ZMQ channel (sends HELLO, gets ConsumerInfo from broker).
    auto ch = messenger.connect_channel(opts.channel_name, opts.timeout_ms,
                                         opts.expected_schema_hash);
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
            shm_consumer = find_datablock_consumer<FlexZoneT, DataBlockT>(
                ch->shm_name(), opts.shm_shared_secret, cfg);
        }
        else
        {
            // No config check requested — use impl with null schemas.
            shm_consumer = find_datablock_consumer_impl(ch->shm_name(),
                                                         opts.shm_shared_secret,
                                                         nullptr, nullptr, nullptr);
        }
        // A nullptr shm_consumer is acceptable — means secret mismatch or SHM unavailable.
        // ZMQ transport still works.
    }

    return Consumer::connect_from_parts(messenger, std::move(*ch), std::move(shm_consumer),
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

    static void s_startup(const char *key);
    static void s_shutdown(const char *key);
};

} // namespace pylabhub::hub
