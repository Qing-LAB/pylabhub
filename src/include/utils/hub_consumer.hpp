#pragma once
/**
 * @file hub_consumer.hpp
 * @brief Active consumer service: owns shared memory (DataBlockConsumer) and/or ZMQ
 *        transport, with dedicated internal threads for data reading.
 *
 * Consumer is an active service object. It manages:
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
 * bundles: typed const FlexZone access, the full ReadTransactionContext, and a shutdown
 * signal. Type safety is enforced at the call site via template parameters.
 *
 * One Consumer instance per channel per process.
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
 */
struct PYLABHUB_UTILS_EXPORT ConsumerMessagingFacade
{
    /// Returns the DataBlockConsumer* (nullptr if SHM not configured).
    DataBlockConsumer *(*fn_get_shm)(void *ctx){nullptr};
    /// Returns true when the consumer's stop flag is set (running == false).
    bool (*fn_is_stopping)(void *ctx){nullptr};
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
 */
template <typename FlexZoneT, typename DataBlockT>
struct ReadProcessorContext
{
    ReadTransactionContext<FlexZoneT, DataBlockT> &txn;
    ConsumerMessagingFacade                       &facade;

    // ── FlexZone access (const — consumer reads only) ─────────────────────────

    template <typename F = FlexZoneT>
    auto flexzone() const noexcept -> std::enable_if_t<!std::is_void_v<F>, const F &>
    {
        return txn.flexzone().get();
    }

    // ── Shutdown signal ───────────────────────────────────────────────────────

    [[nodiscard]] bool is_stopping() const noexcept
    {
        return facade.fn_is_stopping(facade.context);
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
    std::string consumer_uid{};
    std::string consumer_name{};

    int timeout_ms{5000};

    // ── Named schema validation (HEP-CORE-0016 Phase 2) ──────────────────────
    std::string expected_schema_id{};

    // ── HEP-CORE-0021: ZMQ Endpoint Registry ──────────────────────────────────
    std::vector<ZmqSchemaField> zmq_schema{};
    std::string zmq_packing{"aligned"};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    // ── Queue abstraction ────────────────────────────────────────────────────
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};

    // ── Transport arbitration (Phase 6/7) ────────────────────────────────────
    std::string queue_type{};

    /// SHM name for the data block (discovered from broker or configured directly).
    std::string shm_name{};

    /// Data transport discovered from broker: "shm" or "zmq".
    std::string data_transport{"shm"};

    /// ZMQ PUSH endpoint discovered from broker (for ZMQ transport).
    std::string zmq_node_endpoint{};
};

// ============================================================================
// Consumer
// ============================================================================

struct ConsumerImpl;

/// Default timeouts (accessible from template code in this header).
namespace detail
{
inline constexpr int kDefaultReadSlotTimeoutMs = 5000;
inline constexpr int kRealtimeReadPollMs        = 50;
} // namespace detail

/**
 * @class Consumer
 * @brief Active consumer service subscribing to a published channel.
 *
 * Created via Consumer::connect().
 * Optional active mode: call start() to launch shm_thread (DataBlock polling).
 */
class PYLABHUB_UTILS_EXPORT Consumer
{
  public:
    // ── Factories ──────────────────────────────────────────────────────────────

    /**
     * @brief Non-template factory: creates Consumer with the given options.
     *        Establishes local channel resources (queues, SHM attachment).
     */
    [[nodiscard]] static std::optional<Consumer>
    create(const ConsumerOptions &opts);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~Consumer();
    Consumer(Consumer &&) noexcept;
    Consumer &operator=(Consumer &&) noexcept;
    Consumer(const Consumer &) = delete;
    Consumer &operator=(const Consumer &) = delete;

    // ── Callbacks — set BEFORE start() ────────────────────────────────────────

    /// Called from Messenger worker thread when broker sends CHANNEL_CLOSING_NOTIFY.
    void on_channel_closing(std::function<void()> cb);

    /// Called from Messenger worker thread when broker sends FORCE_SHUTDOWN.
    void on_force_shutdown(std::function<void()> cb);

    /// Called from Messenger worker thread on CHANNEL_ERROR_NOTIFY (Cat 1) or
    /// CHANNEL_EVENT_NOTIFY (Cat 2).
    using ChannelErrorCallback =
        std::function<void(const std::string &event, const nlohmann::json &details)>;
    void on_channel_error(ChannelErrorCallback cb);

    // ── Active mode ───────────────────────────────────────────────────────────

    /**
     * @brief Start shm_thread (if has_shm).
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
     */
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── DataBlock read (SHM) — Queue mode ─────────────────────────────────────

    template <typename FlexZoneT, typename DataBlockT>
    bool pull(std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> job,
              int timeout_ms = detail::kDefaultReadSlotTimeoutMs);

    // ── DataBlock read (SHM) — Real-time mode ─────────────────────────────────

    template <typename FlexZoneT, typename DataBlockT>
    void set_read_handler(
        std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> fn);

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
    [[nodiscard]] std::string consumer_uid() const noexcept;
    [[nodiscard]] std::string consumer_name() const noexcept;

    // ── HEP-CORE-0021: ZMQ Endpoint Registry ──────────────────────────────────

    [[nodiscard]] const std::string &data_transport() const noexcept;
    [[nodiscard]] const std::string &zmq_node_endpoint() const noexcept;
    [[nodiscard]] ZmqQueue *queue() noexcept;

    // ── Direct accessor to the unified QueueReader handle (L3.γ bridge) ─────
    //
    // Transient accessor added so RoleAPIBase::Impl can hold a QueueReader *
    // directly and forward data-plane calls without going through
    // hub::Consumer. Deleted along with hub::Consumer itself in the final
    // phase of L3.γ.
    [[nodiscard]] QueueReader *queue_reader() noexcept;

    // ── Queue data operations (delegated to internal QueueReader) ──────────

    [[nodiscard]] const void *read_acquire(std::chrono::milliseconds timeout) noexcept;
    void read_release() noexcept;
    [[nodiscard]] uint64_t last_seq() const noexcept;
    [[nodiscard]] size_t queue_item_size() const noexcept;
    [[nodiscard]] size_t queue_capacity() const noexcept;
    [[nodiscard]] QueueMetrics queue_metrics() const noexcept;
    void reset_queue_metrics() noexcept;

    // ── Queue lifecycle ─────────────────────────────────────────────────────

    bool start_queue();
    void stop_queue();

    // ── Channel data operations (flexzone, checksum — SHM-specific) ─────────

    [[nodiscard]] void *flexzone() noexcept;
    [[nodiscard]] size_t flexzone_size() const noexcept;
    void set_verify_checksum(bool slot, bool fz) noexcept;
    [[nodiscard]] std::string queue_policy_info() const;

    /**
     * @brief Closes queues and SHM. Called by destructor. Idempotent.
     */
    void close();

  private:
    explicit Consumer(std::unique_ptr<ConsumerImpl> impl);
    std::unique_ptr<ConsumerImpl> pImpl;

    // ── Non-template helpers for template method implementations ──────────────

    [[nodiscard]] bool                    _has_shm() const noexcept;
    [[nodiscard]] ConsumerMessagingFacade &_messaging_facade() const;
    void _store_read_handler(std::shared_ptr<InternalReadHandlerFn> h) noexcept;
};

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

} // namespace pylabhub::hub
