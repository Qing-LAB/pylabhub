// src/utils/hub_producer.cpp
#include "utils/hub_producer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <optional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include "portable_atomic_shared_ptr.hpp"

namespace pylabhub::hub
{

// ============================================================================
// ProducerImpl — internal state (defined in .cpp for pImpl idiom)
// ============================================================================

struct ProducerImpl
{
    std::string                        channel_name;
    ChannelPattern                     pattern{ChannelPattern::PubSub};
    std::atomic<bool>                  closed{false};

    // Guards user-facing callbacks during concurrent close().
    mutable std::mutex callbacks_mu;

    // User callbacks
    std::function<void()>                on_channel_closing_cb;
    std::function<void()>                on_force_shutdown_cb;
    Producer::ConsumerDiedCallback       on_consumer_died_cb;
    Producer::ChannelErrorCallback       on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::thread       write_thread_handle;

    // Write job queue (Queue mode): push() enqueues; write_thread dequeues and executes.
    std::mutex                        write_queue_mu;
    std::condition_variable           write_queue_cv;
    std::queue<std::function<void()>> write_queue;
    std::atomic<bool>                 write_stop{false};

    // Real-time handler (RealTime mode): write_thread calls this in a loop.
    pylabhub::utils::detail::PortableAtomicSharedPtr<InternalWriteHandlerFn> m_write_handler;

    // Messaging facade: filled by create(); used by WriteProcessorContext<F,D>.
    ProducerMessagingFacade facade{};

    // HEP-CORE-0021: ZMQ PUSH socket (non-null only when data_transport=="zmq").
    std::unique_ptr<QueueWriter> zmq_queue_;

    // Queue abstraction: unified QueueWriter.
    std::unique_ptr<ShmQueue>    shm_queue_;
    QueueWriter                 *queue_writer_{nullptr};

    void run_write_thread();
};

// ============================================================================
// write_thread: dequeues WriteJobs, acquires slots, executes, commits
// ============================================================================

void ProducerImpl::run_write_thread()
{
    while (true)
    {
        // ── Real-time mode: handler installed ──────────────────────────────
        auto handler = m_write_handler.load(std::memory_order_acquire);
        if (handler)
        {
            if (write_stop.load(std::memory_order_relaxed))
                break; // Exit real-time loop on stop signal
            try
            {
                (*handler)(facade);
            }
            catch (...)
            {
                // Handler threw — continue loop; handler should check is_stopping()
            }
            continue; // Re-check write_stop and handler on every iteration
        }

        // ── Queue mode: wait for a job, a handler install, or stop ─────────
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(write_queue_mu);
            write_queue_cv.wait(lock, [this] {
                return write_stop.load(std::memory_order_relaxed) ||
                       !write_queue.empty() ||
                       m_write_handler.load(std::memory_order_relaxed) != nullptr;
            });

            if (write_stop.load(std::memory_order_relaxed) && write_queue.empty())
            {
                break; // Shutdown — no remaining jobs, exit cleanly
            }
            if (write_queue.empty())
            {
                continue; // Woken by handler install or spurious wake — re-check
            }
            job = std::move(write_queue.front());
            write_queue.pop();
        }

        try
        {
            job(); // Fully-applied closure; manages its own slot acquire/release
        }
        catch (...)
        {
            // Job threw — exception path releases slot inside the closure
        }
    }
}

// ============================================================================
// Producer — construction / destruction
// ============================================================================

Producer::Producer(std::unique_ptr<ProducerImpl> impl) : pImpl(std::move(impl)) {}

Producer::~Producer()
{
    close();
}

Producer::Producer(Producer &&) noexcept            = default;
Producer &Producer::operator=(Producer &&) noexcept = default;

// ============================================================================
// Producer::create — non-template factory
// ============================================================================

std::optional<Producer>
Producer::create(const ProducerOptions &opts)
{
    auto impl          = std::make_unique<ProducerImpl>();
    impl->channel_name = opts.channel_name;
    impl->pattern      = opts.pattern;
    impl->closed       = false;

    // Fill the messaging facade.
    // ABI guard: ProducerMessagingFacade is exported across the shared library boundary.
    // 4 pointers × 8 bytes = 32 bytes on LP64/LLP64.
    static_assert(sizeof(ProducerMessagingFacade) == 32,
                  "ProducerMessagingFacade size changed — ABI break! "
                  "Append new fields at the end and bump SOVERSION.");

    ProducerImpl *raw = impl.get();
    impl->facade.context = raw;

    impl->facade.fn_get_shm = [](void *ctx) -> DataBlockProducer * {
        auto *sq = static_cast<ProducerImpl *>(ctx)->shm_queue_.get();
        return sq ? sq->raw_producer() : nullptr;
    };
    impl->facade.fn_is_stopping = [](void *ctx) -> bool {
        return static_cast<ProducerImpl *>(ctx)->write_stop.load(std::memory_order_relaxed);
    };
    impl->facade.fn_channel_name = [](void *ctx) -> const std::string & {
        return static_cast<ProducerImpl *>(ctx)->channel_name;
    };

    // HEP-CORE-0021: create ZMQ PUSH socket when data_transport == "zmq".
    if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[producer] data_transport='zmq' but zmq_node_endpoint is empty");
            return std::nullopt;
        }
        // Derive 8-byte schema tag from the first 8 bytes of schema_hash (binary).
        std::optional<std::array<uint8_t, 8>> schema_tag;
        if (opts.schema_hash.size() >= 8)
        {
            std::array<uint8_t, 8> tag{};
            std::memcpy(tag.data(), opts.schema_hash.data(), 8);
            schema_tag = tag;
        }
        impl->zmq_queue_ = ZmqQueue::push_to(
            opts.zmq_node_endpoint, opts.zmq_schema, opts.zmq_packing, opts.zmq_bind, schema_tag,
            /*sndhwm=*/0, opts.zmq_buffer_depth, opts.zmq_overflow_policy);
        if (!impl->zmq_queue_)
        {
            return std::nullopt;
        }
        if (!impl->zmq_queue_->start())
        {
            LOGGER_ERROR("[producer] ZMQ PUSH socket start() failed for '{}'",
                         opts.zmq_node_endpoint);
            return std::nullopt;
        }
        LOGGER_INFO("[producer] ZMQ PUSH socket created at '{}'", opts.zmq_node_endpoint);
    }

    // Queue abstraction: create unified QueueWriter.
    if (opts.has_shm && !opts.zmq_schema.empty())
    {
        impl->shm_queue_ = ShmQueue::create_writer(
            opts.channel_name,
            opts.zmq_schema, opts.zmq_packing,
            opts.fz_schema, opts.fz_packing,
            opts.shm_config.ring_buffer_capacity,
            opts.shm_config.physical_page_size,
            opts.shm_config.shared_secret,
            opts.shm_config.policy,
            opts.shm_config.consumer_sync_policy,
            opts.shm_config.checksum_policy,
            false, false,  // checksum flags: set via set_checksum_policy() below
            opts.always_clear_slot,
            opts.shm_config.hub_uid,
            opts.shm_config.hub_name,
            nullptr, nullptr,
            opts.shm_config.producer_uid,
            opts.shm_config.producer_name);
        if (!impl->shm_queue_)
        {
            LOGGER_ERROR("[producer] ShmQueue creation failed for '{}'", opts.channel_name);
            return std::nullopt;
        }
        impl->queue_writer_ = impl->shm_queue_.get();
    }
    else if (impl->zmq_queue_)
    {
        impl->queue_writer_ = impl->zmq_queue_.get();
    }

    // Set checksum policy on the queue (single path for both SHM and ZMQ).
    if (impl->queue_writer_)
    {
        impl->queue_writer_->set_checksum_policy(opts.checksum_policy);
        impl->queue_writer_->set_flexzone_checksum(opts.flexzone_checksum);
    }

    return Producer(std::move(impl));
}

// ============================================================================
// Producer — callback registration
// ============================================================================

void Producer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Producer::on_force_shutdown(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_force_shutdown_cb = std::move(cb);
    }
}

void Producer::on_consumer_died(ConsumerDiedCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_consumer_died_cb = std::move(cb);
    }
}

void Producer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_error_cb = std::move(cb);
    }
}

// ============================================================================
// Producer — active mode
// ============================================================================

bool Producer::start()
{
    if (!pImpl || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.exchange(true, std::memory_order_acq_rel))
    {
        return false; // Already running
    }

    pImpl->write_stop.store(false, std::memory_order_relaxed);

    if (pImpl->shm_queue_)
    {
        auto *impl_ptr = pImpl.get();
        pImpl->write_thread_handle = std::thread([impl_ptr] { impl_ptr->run_write_thread(); });
    }

    return true;
}

void Producer::stop()
{
    if (!pImpl)
    {
        return;
    }

    if (!pImpl->running.exchange(false, std::memory_order_acq_rel))
    {
        return; // Was not running
    }

    // Signal write_thread to stop after draining the queue
    {
        std::lock_guard<std::mutex> lock(pImpl->write_queue_mu);
        pImpl->write_stop.store(true, std::memory_order_relaxed);
    }
    pImpl->write_queue_cv.notify_all();

    if (pImpl->write_thread_handle.joinable())
    {
        pImpl->write_thread_handle.join();
    }

    // Stop ZMQ PUSH socket after threads join.
    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset();
    }
}

bool Producer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
}

// ============================================================================
// Producer — non-template helpers for template method implementations
// ============================================================================

bool Producer::_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm_queue_ != nullptr;
}

bool Producer::_is_started_and_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm_queue_ != nullptr &&
           pImpl->running.load(std::memory_order_relaxed);
}

ProducerMessagingFacade &Producer::_messaging_facade() const
{
    assert(pImpl);
    return pImpl->facade;
}

void Producer::_enqueue_write_job(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(pImpl->write_queue_mu);
        pImpl->write_queue.push(std::move(job));
    }
    pImpl->write_queue_cv.notify_one();
}

void Producer::_store_write_handler(std::shared_ptr<InternalWriteHandlerFn> h) noexcept
{
    if (pImpl)
    {
        pImpl->m_write_handler.store(std::move(h), std::memory_order_release);
        // Wake the write_thread so it transitions between Queue and RealTime modes.
        pImpl->write_queue_cv.notify_all();
    }
}

bool Producer::is_stopping() const noexcept
{
    return pImpl && pImpl->write_stop.load(std::memory_order_relaxed);
}

bool Producer::has_realtime_handler() const noexcept
{
    return pImpl && pImpl->m_write_handler.load(std::memory_order_relaxed) != nullptr;
}

// ============================================================================
// Producer — introspection
// ============================================================================

bool Producer::is_valid() const
{
    return pImpl && !pImpl->closed;
}

const std::string &Producer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->channel_name : kEmpty;
}

ChannelPattern Producer::pattern() const
{
    return pImpl ? pImpl->pattern : ChannelPattern::PubSub;
}

bool Producer::has_shm() const
{
    return pImpl && pImpl->shm_queue_ != nullptr;
}

uint32_t Producer::spinlock_count() const noexcept
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    return dbp ? dbp->spinlock_count() : 0u;
}

SharedSpinLock Producer::get_spinlock(size_t index)
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    if (!dbp)
        throw std::runtime_error("get_spinlock: SHM not connected");
    return dbp->get_spinlock(index);
}

std::string Producer::hub_uid() const noexcept
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    return dbp ? dbp->hub_uid() : std::string{};
}

std::string Producer::hub_name() const noexcept
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    return dbp ? dbp->hub_name() : std::string{};
}

std::string Producer::producer_uid() const noexcept
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    return dbp ? dbp->producer_uid() : std::string{};
}

std::string Producer::producer_name() const noexcept
{
    auto *dbp = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_producer() : nullptr;
    return dbp ? dbp->producer_name() : std::string{};
}

ZmqQueue *Producer::queue() noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Producer — Queue data operations (forwarded to internal QueueWriter)
// ============================================================================

QueueWriter *Producer::queue_writer() noexcept
{
    return pImpl ? pImpl->queue_writer_ : nullptr;
}

void *Producer::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    auto *q = pImpl ? pImpl->queue_writer_ : nullptr;
    return q ? q->write_acquire(timeout) : nullptr;
}

void Producer::write_commit() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->write_commit();
}

void Producer::write_discard() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->write_discard();
}

size_t Producer::queue_item_size() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->item_size() : 0;
}

size_t Producer::queue_capacity() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->capacity() : 0;
}

QueueMetrics Producer::queue_metrics() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->metrics() : QueueMetrics{};
}

void Producer::reset_queue_metrics() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->init_metrics();
}

bool Producer::start_queue()
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->start() : false;
}

void Producer::stop_queue()
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->stop();
}

// ============================================================================
// Producer — Channel data operations (flexzone, checksum)
// ============================================================================

void *Producer::flexzone() noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->flexzone() : nullptr;
}

size_t Producer::flexzone_size() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->flexzone_size() : 0;
}

void Producer::set_checksum_options(bool slot, bool fz) noexcept
{
    auto *sq = pImpl ? static_cast<ShmQueue *>(pImpl->shm_queue_.get()) : nullptr;
    if (sq) sq->set_checksum_options(slot, fz);
}

void Producer::set_always_clear_slot(bool enable) noexcept
{
    auto *sq = pImpl ? static_cast<ShmQueue *>(pImpl->shm_queue_.get()) : nullptr;
    if (sq) sq->set_always_clear_slot(enable);
}

void Producer::sync_flexzone_checksum() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->sync_flexzone_checksum();
}

std::string Producer::queue_policy_info() const
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->policy_info() : std::string{};
}

// ============================================================================
// Producer::close — idempotent teardown
// ============================================================================

void Producer::close()
{
    if (!pImpl || pImpl->closed)
    {
        return;
    }
    stop();

    // Clear all user-facing callbacks.
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb  = nullptr;
        pImpl->on_force_shutdown_cb   = nullptr;
        pImpl->on_consumer_died_cb    = nullptr;
        pImpl->on_channel_error_cb    = nullptr;
    }

    // Reset queue abstraction — ShmQueue owns DataBlock, destroyed with it.
    pImpl->queue_writer_ = nullptr;
    pImpl->shm_queue_.reset();
    pImpl->zmq_queue_.reset();
    pImpl->closed = true;
}

} // namespace pylabhub::hub
