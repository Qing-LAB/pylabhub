#pragma once
/**
 * @file hub_producer.hpp
 * @brief Active producer service: owns shared memory (DataBlockProducer) and/or ZMQ
 *        transport, with dedicated internal threads for data writing.
 *
 * Producer is an active service object. It manages:
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
 * bundles: typed FlexZone access, the full WriteTransactionContext, and a shutdown signal.
 * Type safety is enforced at the call site via template parameters.
 *
 * One Producer instance per channel per process.
 *
 * **Thread safety**: All public methods are thread-safe unless documented otherwise.
 */
#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/loop_timing_policy.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqQueue — returned by queue() accessor (HEP-CORE-0021)
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
 */
struct PYLABHUB_UTILS_EXPORT ProducerMessagingFacade
{
    /// Returns the DataBlockProducer* (nullptr if SHM not configured).
    DataBlockProducer *(*fn_get_shm)(void *ctx){nullptr};
    /// Returns true when the producer's write_thread stop flag is set.
    bool (*fn_is_stopping)(void *ctx){nullptr};
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

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    /// Optional named schema ID (e.g. `"lab.sensors.temperature.raw@1"`).
    std::string schema_id{};

    // ── HEP-CORE-0021: ZMQ Endpoint Registry ──────────────────────────────────
    /// Data transport type: "shm" (default) or "zmq".
    std::string data_transport{"shm"};
    /// Bind address for the ZMQ PUSH socket (used only when data_transport=="zmq").
    std::string zmq_node_endpoint{};
    /// If true, PUSH socket binds to zmq_node_endpoint; otherwise connects (default: bind).
    bool zmq_bind{true};
    /// Schema for ZMQ PUSH frames (required when data_transport=="zmq").
    std::vector<ZmqSchemaField> zmq_schema{};
    /// "aligned" (ctypes.LittleEndianStructure default) or "packed" (no padding).
    std::string zmq_packing{"aligned"};
    /// Flexzone schema fields (empty = no flexzone). Used by ShmQueue to compute fz size.
    std::vector<ZmqSchemaField> fz_schema{};
    /// Flexzone packing (default "aligned"). Must match the receiver.
    std::string fz_packing{"aligned"};
    /// Internal send-buffer depth for the ZmqQueue PUSH ring (write side).
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    /// Overflow policy for the ZmqQueue PUSH send ring.
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // ── Queue abstraction ────────────────────────────────────────────────────
    /// Checksum policy for this channel. Applied to all queues via set_checksum_policy().
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    /// Checksum flexzone (SHM-specific). Applied via set_flexzone_checksum().
    bool flexzone_checksum{true};
    /// Zero the slot buffer on write_acquire() (SHM only). Default true for safety.
    bool always_clear_slot{true};
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
 * Created via Producer::create().
 * Optional active mode: call start() to launch write_thread (SHM slot processing).
 */
class PYLABHUB_UTILS_EXPORT Producer
{
  public:
    // ── Factories ──────────────────────────────────────────────────────────────

    /**
     * @brief Non-template factory: creates Producer with the given options.
     *        Establishes local channel resources (queues, SHM).
     */
    [[nodiscard]] static std::optional<Producer>
    create(const ProducerOptions &opts);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~Producer();
    Producer(Producer &&) noexcept;
    Producer &operator=(Producer &&) noexcept;
    Producer(const Producer &) = delete;
    Producer &operator=(const Producer &) = delete;

    // ── Callbacks — set BEFORE start() ────────────────────────────────────────

    /// Called from Messenger worker thread when broker sends CHANNEL_CLOSING_NOTIFY.
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends FORCE_SHUTDOWN.
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

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start write_thread (SHM).
     * @return true if started successfully; false if already running or not valid.
     */
    bool start();

    /**
     * @brief Graceful stop: joins write_thread. Idempotent.
     *        Sets the is_stopping() flag before joining; handlers should poll it.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief True when stop() has been called (write_stop flag is set).
     */
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── DataBlock write (SHM) — Queue mode ────────────────────────────────────

    template <typename FlexZoneT, typename DataBlockT>
    bool push(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job);

    template <typename FlexZoneT, typename DataBlockT>
    bool synced_write(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job,
                      int timeout_ms = detail::kDefaultWriteSlotTimeoutMs);

    // ── DataBlock write (SHM) — Real-time mode ────────────────────────────────

    template <typename FlexZoneT, typename DataBlockT>
    void set_write_handler(
        std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> fn);

    /// Returns true when a real-time write handler has been installed via set_write_handler().
    [[nodiscard]] bool has_realtime_handler() const noexcept;

    // ── Introspection ─────────────────────────────────────────────────────────

    [[nodiscard]] bool               is_valid() const;
    [[nodiscard]] const std::string &channel_name() const;
    [[nodiscard]] ChannelPattern     pattern() const;
    [[nodiscard]] bool               has_shm() const;

    // ── SHM identity (delegated to internal DataBlock via ShmQueue) ───────────

    [[nodiscard]] uint32_t spinlock_count() const noexcept;
    [[nodiscard]] SharedSpinLock get_spinlock(size_t index);
    [[nodiscard]] std::string hub_uid() const noexcept;
    [[nodiscard]] std::string hub_name() const noexcept;
    [[nodiscard]] std::string producer_uid() const noexcept;
    [[nodiscard]] std::string producer_name() const noexcept;

    // ── HEP-CORE-0021: ZMQ Endpoint Registry ──────────────────────────────────

    [[nodiscard]] ZmqQueue *queue() noexcept;

    // ── Queue data operations (delegated to internal QueueWriter) ──────────

    [[nodiscard]] void *write_acquire(std::chrono::milliseconds timeout) noexcept;
    void write_commit() noexcept;
    void write_discard() noexcept;
    [[nodiscard]] size_t queue_item_size() const noexcept;
    [[nodiscard]] size_t queue_capacity() const noexcept;
    [[nodiscard]] QueueMetrics queue_metrics() const noexcept;
    void reset_queue_metrics() noexcept;

    // ── Queue lifecycle ─────────────────────────────────────────────────────

    bool start_queue();
    void stop_queue();

    // ── Channel data operations (flexzone, checksum — SHM-specific) ─────────

    [[nodiscard]] void *write_flexzone() noexcept;
    [[nodiscard]] const void *read_flexzone() const noexcept;
    [[nodiscard]] size_t flexzone_size() const noexcept;
    void set_checksum_options(bool slot, bool fz) noexcept;
    void set_always_clear_slot(bool enable) noexcept;
    void sync_flexzone_checksum() noexcept;
    [[nodiscard]] std::string queue_policy_info() const;

    /**
     * @brief Closes queues and SHM. Called by destructor. Idempotent.
     */
    void close();

  private:
    explicit Producer(std::unique_ptr<ProducerImpl> impl);
    std::unique_ptr<ProducerImpl> pImpl;

    // ── Non-template helpers for template method implementations ──────────────

    [[nodiscard]] bool                   _has_shm() const noexcept;
    [[nodiscard]] bool                   _is_started_and_has_shm() const noexcept;
    [[nodiscard]] ProducerMessagingFacade &_messaging_facade() const;
    void _enqueue_write_job(std::function<void()> job);
    void _store_write_handler(std::shared_ptr<InternalWriteHandlerFn> h) noexcept;
};

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

} // namespace pylabhub::hub
