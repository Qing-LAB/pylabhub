// src/utils/hub_producer.cpp
#include "utils/hub_producer.hpp"
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
// ProducerImpl — internal state
// ============================================================================

struct ProducerImpl
{
    std::string                        channel_name;
    ChannelPattern                     pattern{ChannelPattern::PubSub};
    bool                               closed{false};

    // Data-plane queue owners.
    std::unique_ptr<QueueWriter> zmq_queue_;
    std::unique_ptr<ShmQueue>    shm_queue_;
    QueueWriter                 *queue_writer_{nullptr};
};

// ============================================================================
// Producer — construction / destruction
// ============================================================================

Producer::Producer(std::unique_ptr<ProducerImpl> impl) : pImpl(std::move(impl)) {}

Producer::~Producer() { close(); }

Producer::Producer(Producer &&) noexcept            = default;
Producer &Producer::operator=(Producer &&) noexcept = default;

// ============================================================================
// Producer::create
// ============================================================================

std::optional<Producer>
Producer::create(const ProducerOptions &opts)
{
    auto impl          = std::make_unique<ProducerImpl>();
    impl->channel_name = opts.channel_name;
    impl->pattern      = opts.pattern;

    if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[producer] data_transport='zmq' but zmq_node_endpoint is empty");
            return std::nullopt;
        }
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
            return std::nullopt;
        if (!impl->zmq_queue_->start())
        {
            LOGGER_ERROR("[producer] ZMQ PUSH socket start() failed for '{}'",
                         opts.zmq_node_endpoint);
            return std::nullopt;
        }
        LOGGER_INFO("[producer] ZMQ PUSH socket created at '{}'", opts.zmq_node_endpoint);
    }

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
            false, false,
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

    if (impl->queue_writer_)
    {
        impl->queue_writer_->set_checksum_policy(opts.checksum_policy);
        impl->queue_writer_->set_flexzone_checksum(opts.flexzone_checksum);
    }

    return Producer(std::move(impl));
}

// ============================================================================
// Introspection
// ============================================================================

bool Producer::is_valid() const { return pImpl && !pImpl->closed; }

const std::string &Producer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->channel_name : kEmpty;
}

ChannelPattern Producer::pattern() const
{
    return pImpl ? pImpl->pattern : ChannelPattern::PubSub;
}

bool Producer::has_shm() const { return pImpl && pImpl->shm_queue_ != nullptr; }

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
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Queue operations
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
// Flexzone / checksum
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
        return;

    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset();
    }
    pImpl->queue_writer_ = nullptr;
    pImpl->shm_queue_.reset();
    pImpl->closed = true;
}

} // namespace pylabhub::hub
