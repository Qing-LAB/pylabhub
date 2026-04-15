// src/utils/hub_consumer.cpp
#include "utils/hub_consumer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

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
    bool                               closed{false};

    std::unique_ptr<QueueReader> zmq_queue_;
    std::unique_ptr<ShmQueue>    shm_queue_;
    QueueReader                 *queue_reader_{nullptr};
};

// ============================================================================
// Consumer — construction / destruction
// ============================================================================

Consumer::Consumer(std::unique_ptr<ConsumerImpl> impl) : pImpl(std::move(impl)) {}

Consumer::~Consumer() { close(); }

Consumer::Consumer(Consumer &&) noexcept            = default;
Consumer &Consumer::operator=(Consumer &&) noexcept = default;

// ============================================================================
// Consumer::create
// ============================================================================

std::optional<Consumer>
Consumer::create(const ConsumerOptions &opts)
{
    auto impl                   = std::make_unique<ConsumerImpl>();
    impl->channel_name          = opts.channel_name;
    impl->data_transport_str    = opts.data_transport;
    impl->zmq_node_endpoint_str = opts.zmq_node_endpoint;

    if (opts.data_transport == "zmq")
    {
        const std::string &ep = opts.zmq_node_endpoint;
        if (ep.empty())
        {
            LOGGER_ERROR("[consumer] data_transport='zmq' but zmq_node_endpoint is empty");
            return std::nullopt;
        }
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
            return std::nullopt;
        if (!impl->zmq_queue_->start())
        {
            LOGGER_ERROR("[consumer] ZMQ PULL socket start() failed for '{}'", ep);
            return std::nullopt;
        }
        LOGGER_INFO("[consumer] ZMQ PULL socket connected to '{}'", ep);
    }

    if (!opts.shm_name.empty() && opts.shm_shared_secret != 0 && !opts.zmq_schema.empty())
    {
        impl->shm_queue_ = ShmQueue::create_reader(
            opts.shm_name, opts.shm_shared_secret,
            opts.zmq_schema, opts.zmq_packing,
            opts.channel_name,
            false, false,
            opts.consumer_uid, opts.consumer_name);
        if (impl->shm_queue_)
            impl->queue_reader_ = impl->shm_queue_.get();
    }
    else if (impl->zmq_queue_)
    {
        impl->queue_reader_ = impl->zmq_queue_.get();
    }

    if (impl->queue_reader_)
    {
        impl->queue_reader_->set_checksum_policy(opts.checksum_policy);
        impl->queue_reader_->set_flexzone_checksum(opts.flexzone_checksum);
    }

    return Consumer(std::move(impl));
}

// ============================================================================
// Introspection
// ============================================================================

bool Consumer::is_valid() const { return pImpl && !pImpl->closed; }

const std::string &Consumer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->channel_name : kEmpty;
}

ChannelPattern Consumer::pattern() const
{
    return pImpl ? pImpl->pattern : ChannelPattern::PubSub;
}

bool Consumer::has_shm() const { return pImpl && pImpl->shm_queue_ != nullptr; }

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
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Queue operations
// ============================================================================

QueueReader *Consumer::queue_reader() noexcept
{
    return pImpl ? pImpl->queue_reader_ : nullptr;
}

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
// Flexzone / checksum
// ============================================================================

void *Consumer::flexzone() noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->flexzone() : nullptr;
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
        return;

    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset();
    }
    pImpl->queue_reader_ = nullptr;
    pImpl->shm_queue_.reset();
    pImpl->closed = true;
}

} // namespace pylabhub::hub
