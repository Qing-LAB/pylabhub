// src/utils/hub_consumer.cpp
#include "utils/hub_consumer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "portable_atomic_shared_ptr.hpp"

namespace pylabhub::hub
{

// ============================================================================
// ConsumerImpl — internal state
// ============================================================================

struct ConsumerImpl
{
    std::string                        channel_name;
    ChannelPattern                     pattern{ChannelPattern::PubSub};
    std::string                        data_transport_str{"shm"};
    std::string                        zmq_node_endpoint_str{};
    std::atomic<bool>                  closed{false};

    // User callbacks
    std::function<void()>              on_channel_closing_cb;
    std::function<void()>              on_force_shutdown_cb;
    Consumer::ChannelErrorCallback     on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested_{false};
    std::thread       shm_thread_handle;

    // Guards user-facing callbacks during concurrent close().
    mutable std::mutex callbacks_mu;

    // Real-time handler (RealTime mode): shm_thread calls this in a loop.
    pylabhub::utils::detail::PortableAtomicSharedPtr<InternalReadHandlerFn> m_read_handler;

    // CV used to wake shm_thread from Queue-mode idle sleep.
    std::mutex              m_handler_cv_mu;
    std::condition_variable m_handler_cv;

    // Messaging facade: filled by create(); used by ReadProcessorContext<F,D>.
    ConsumerMessagingFacade facade{};

    // HEP-CORE-0021: ZMQ PULL socket (non-null only when data_transport=="zmq").
    std::unique_ptr<QueueReader> zmq_queue_;

    // Queue abstraction: unified QueueReader.
    std::unique_ptr<ShmQueue>    shm_queue_;
    QueueReader                 *queue_reader_{nullptr};

    void run_shm_thread();
};

// ============================================================================
// shm_thread: drives DataBlock SHM reading in Queue or Real-time mode
// ============================================================================

void ConsumerImpl::run_shm_thread()
{
    if (!shm_queue_)
    {
        return;
    }

    while (running.load(std::memory_order_relaxed))
    {
        // ── Real-time mode: handler installed ──────────────────────────────
        auto handler = m_read_handler.load(std::memory_order_acquire);
        if (handler)
        {
            try
            {
                (*handler)(facade);
            }
            catch (...)
            {
                // Handler threw — continue loop; handler should check is_stopping()
            }
            continue; // Re-check running and handler on every iteration
        }

        // ── Queue mode: sleep until handler installed or stop ───────────────
        std::unique_lock<std::mutex> lock(m_handler_cv_mu);
        m_handler_cv.wait(lock, [this] {
            return !running.load(std::memory_order_relaxed) ||
                   m_read_handler.load(std::memory_order_relaxed) != nullptr;
        });
    }
}

// ============================================================================
// Consumer — construction / destruction
// ============================================================================

Consumer::Consumer(std::unique_ptr<ConsumerImpl> impl) : pImpl(std::move(impl)) {}

Consumer::~Consumer()
{
    close();
}

Consumer::Consumer(Consumer &&) noexcept            = default;
Consumer &Consumer::operator=(Consumer &&) noexcept = default;

// ============================================================================
// Consumer::create — non-template factory
// ============================================================================

std::optional<Consumer>
Consumer::create(const ConsumerOptions &opts)
{
    auto impl             = std::make_unique<ConsumerImpl>();
    impl->channel_name    = opts.channel_name;
    impl->data_transport_str = opts.data_transport;
    impl->zmq_node_endpoint_str = opts.zmq_node_endpoint;
    impl->closed          = false;

    // ABI guard: ConsumerMessagingFacade is exported across the shared library boundary.
    // 4 pointers × 8 bytes = 32 bytes on LP64/LLP64.
    static_assert(sizeof(ConsumerMessagingFacade) == 32,
                  "ConsumerMessagingFacade size changed — ABI break! "
                  "Append new fields at the end and bump SOVERSION.");

    // Fill the messaging facade.
    ConsumerImpl *raw    = impl.get();
    impl->facade.context = raw;

    impl->facade.fn_get_shm = [](void *ctx) -> DataBlockConsumer * {
        auto *sq = static_cast<ConsumerImpl *>(ctx)->shm_queue_.get();
        return sq ? sq->raw_consumer() : nullptr;
    };
    impl->facade.fn_is_stopping = [](void *ctx) -> bool {
        return !static_cast<ConsumerImpl *>(ctx)->running.load(std::memory_order_relaxed);
    };
    impl->facade.fn_channel_name = [](void *ctx) -> const std::string & {
        return static_cast<ConsumerImpl *>(ctx)->channel_name;
    };

    // HEP-CORE-0021: create ZMQ PULL socket when data_transport=="zmq".
    if (opts.data_transport == "zmq")
    {
        const std::string &ep = opts.zmq_node_endpoint;
        if (ep.empty())
        {
            LOGGER_ERROR("[consumer] data_transport='zmq' but zmq_node_endpoint is empty");
            return std::nullopt;
        }
        // Derive 8-byte schema tag from the first 8 bytes of expected_schema_hash (binary).
        std::optional<std::array<uint8_t, 8>> schema_tag;
        if (opts.expected_schema_hash.size() >= 8)
        {
            std::array<uint8_t, 8> tag{};
            std::memcpy(tag.data(), opts.expected_schema_hash.data(), 8);
            schema_tag = tag;
        }
        impl->zmq_queue_ = ZmqQueue::pull_from(ep, opts.zmq_schema, opts.zmq_packing,
                                                /*bind=*/false, opts.zmq_buffer_depth, schema_tag);
        if (!impl->zmq_queue_)
        {
            return std::nullopt;
        }
        if (!impl->zmq_queue_->start())
        {
            LOGGER_ERROR("[consumer] ZMQ PULL socket start() failed for '{}'", ep);
            return std::nullopt;
        }
        LOGGER_INFO("[consumer] ZMQ PULL socket connected to '{}'", ep);
    }

    // Queue abstraction: create unified QueueReader.
    if (!opts.shm_name.empty() && opts.shm_shared_secret != 0 && !opts.zmq_schema.empty())
    {
        impl->shm_queue_ = ShmQueue::create_reader(
            opts.shm_name, opts.shm_shared_secret,
            opts.zmq_schema, opts.zmq_packing,
            opts.channel_name,
            false, false,  // checksum flags: set via set_checksum_policy() below
            opts.consumer_uid, opts.consumer_name);
        if (impl->shm_queue_)
            impl->queue_reader_ = impl->shm_queue_.get();
    }
    else if (impl->zmq_queue_)
    {
        impl->queue_reader_ = impl->zmq_queue_.get();
    }

    // Set checksum policy on the queue (single path for both SHM and ZMQ).
    if (impl->queue_reader_)
    {
        impl->queue_reader_->set_checksum_policy(opts.checksum_policy);
        impl->queue_reader_->set_flexzone_checksum(opts.flexzone_checksum);
    }

    return Consumer(std::move(impl));
}

// ============================================================================
// Consumer — callback registration
// ============================================================================

void Consumer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Consumer::on_force_shutdown(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_force_shutdown_cb = std::move(cb);
    }
}

void Consumer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_error_cb = std::move(cb);
    }
}

// ============================================================================
// Consumer — active mode
// ============================================================================

bool Consumer::start()
{
    if (!pImpl || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.exchange(true, std::memory_order_acq_rel))
    {
        return false; // Already running
    }
    pImpl->stop_requested_.store(false, std::memory_order_relaxed);

    // shm_thread only when SHM is attached
    if (pImpl->shm_queue_)
    {
        auto *impl_ptr = pImpl.get();
        pImpl->shm_thread_handle = std::thread([impl_ptr] { impl_ptr->run_shm_thread(); });
    }

    return true;
}

void Consumer::stop()
{
    if (!pImpl)
    {
        return;
    }
    if (!pImpl->running.exchange(false, std::memory_order_acq_rel))
    {
        return; // Was not running
    }
    pImpl->stop_requested_.store(true, std::memory_order_relaxed);

    // Wake shm_thread if it is sleeping in Queue-mode idle wait.
    pImpl->m_handler_cv.notify_all();

    if (pImpl->shm_thread_handle.joinable())
    {
        pImpl->shm_thread_handle.join();
    }

    // Stop ZMQ PULL socket after threads join.
    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset();
    }
}

bool Consumer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
}

// ============================================================================
// Consumer — non-template helpers for template method implementations
// ============================================================================

bool Consumer::_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm_queue_ != nullptr;
}

ConsumerMessagingFacade &Consumer::_messaging_facade() const
{
    assert(pImpl);
    return pImpl->facade;
}

void Consumer::_store_read_handler(std::shared_ptr<InternalReadHandlerFn> h) noexcept
{
    if (pImpl)
    {
        pImpl->m_read_handler.store(std::move(h), std::memory_order_release);
        pImpl->m_handler_cv.notify_all();
    }
}

bool Consumer::is_stopping() const noexcept
{
    return pImpl && pImpl->stop_requested_.load(std::memory_order_relaxed);
}

bool Consumer::has_realtime_handler() const noexcept
{
    return pImpl && pImpl->m_read_handler.load(std::memory_order_relaxed) != nullptr;
}

// ============================================================================
// Consumer — introspection
// ============================================================================

bool Consumer::is_valid() const
{
    return pImpl && !pImpl->closed;
}

const std::string &Consumer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->channel_name : kEmpty;
}

ChannelPattern Consumer::pattern() const
{
    return pImpl ? pImpl->pattern : ChannelPattern::PubSub;
}

bool Consumer::has_shm() const
{
    return pImpl && pImpl->shm_queue_ != nullptr;
}

uint32_t Consumer::spinlock_count() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->spinlock_count() : 0u;
}

SharedSpinLock Consumer::get_spinlock(size_t index)
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    if (!dbc)
        throw std::runtime_error("get_spinlock: SHM not connected");
    return dbc->get_spinlock(index);
}

std::string Consumer::hub_uid() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->hub_uid() : std::string{};
}

std::string Consumer::hub_name() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->hub_name() : std::string{};
}

std::string Consumer::producer_uid() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->producer_uid() : std::string{};
}

std::string Consumer::producer_name() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->producer_name() : std::string{};
}

std::string Consumer::consumer_uid() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->consumer_uid() : std::string{};
}

std::string Consumer::consumer_name() const noexcept
{
    auto *dbc = pImpl && pImpl->shm_queue_ ? pImpl->shm_queue_->raw_consumer() : nullptr;
    return dbc ? dbc->consumer_name() : std::string{};
}

const std::string &Consumer::data_transport() const noexcept
{
    static const std::string kShm{"shm"};
    return pImpl ? pImpl->data_transport_str : kShm;
}

const std::string &Consumer::zmq_node_endpoint() const noexcept
{
    static const std::string kEmpty;
    return pImpl ? pImpl->zmq_node_endpoint_str : kEmpty;
}

ZmqQueue *Consumer::queue() noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Consumer — Queue data operations (forwarded to internal QueueReader)
// ============================================================================

const void *Consumer::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    auto *q = pImpl ? pImpl->queue_reader_ : nullptr;
    return q ? q->read_acquire(timeout) : nullptr;
}

void Consumer::read_release() noexcept
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->read_release();
}

uint64_t Consumer::last_seq() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->last_seq() : 0;
}

size_t Consumer::queue_item_size() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->item_size() : 0;
}

size_t Consumer::queue_capacity() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->capacity() : 0;
}

QueueMetrics Consumer::queue_metrics() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->metrics() : QueueMetrics{};
}

void Consumer::reset_queue_metrics() noexcept
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->init_metrics();
}

bool Consumer::start_queue()
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->start() : false;
}

void Consumer::stop_queue()
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->stop();
}

// ============================================================================
// Consumer — Channel data operations (flexzone, checksum)
// ============================================================================

const void *Consumer::read_flexzone() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->read_flexzone() : nullptr;
}

size_t Consumer::flexzone_size() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->flexzone_size() : 0;
}

void Consumer::set_verify_checksum(bool slot, bool fz) noexcept
{
    auto *sq = pImpl ? static_cast<ShmQueue *>(pImpl->shm_queue_.get()) : nullptr;
    if (sq) sq->set_verify_checksum(slot, fz);
}

std::string Consumer::queue_policy_info() const
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->policy_info() : std::string{};
}

// ============================================================================
// Consumer::close — idempotent teardown
// ============================================================================

void Consumer::close()
{
    if (!pImpl || pImpl->closed)
    {
        return;
    }

    stop();

    // Clear all user-facing callbacks.
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb = nullptr;
        pImpl->on_force_shutdown_cb  = nullptr;
        pImpl->on_channel_error_cb   = nullptr;
    }

    // Reset queue abstraction — ShmQueue owns DataBlock, destroyed with it.
    pImpl->queue_reader_ = nullptr;
    pImpl->shm_queue_.reset();
    pImpl->zmq_queue_.reset();
    pImpl->closed = true;
}

} // namespace pylabhub::hub
