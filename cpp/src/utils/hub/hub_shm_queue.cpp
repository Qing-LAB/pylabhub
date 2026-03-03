// src/utils/hub/hub_shm_queue.cpp
/**
 * @file hub_shm_queue.cpp
 * @brief ShmQueue implementation — shared-memory-backed Queue.
 *
 * Wraps DataBlockConsumer (read) or DataBlockProducer (write).
 * No ZMQ, no protocol, no broker interaction.
 */
#include "utils/hub_shm_queue.hpp"
#include "utils/logger.hpp"

#include <cassert>
#include <cstddef>
#include <span>
#include <utility>

namespace pylabhub::hub
{

// ============================================================================
// ShmQueueImpl — internal state
// ============================================================================

struct ShmQueueImpl
{
    // Owning pointers (from from_consumer / from_producer):
    std::unique_ptr<DataBlockConsumer> dbc;
    std::unique_ptr<DataBlockProducer> dbp;

    // Non-owning pointers (from from_consumer_ref / from_producer_ref):
    DataBlockConsumer* dbc_ref{nullptr};
    DataBlockProducer* dbp_ref{nullptr};

    // Current acquired handles (valid between acquire and release/commit/abort):
    std::unique_ptr<SlotConsumeHandle> read_handle;
    std::unique_ptr<SlotWriteHandle>   write_handle;

    size_t      item_sz{0};
    size_t      fz_sz{0};
    std::string chan_name;

    bool checksum_slot{false};
    bool checksum_fz{false};

    // Accessors that resolve owning vs non-owning.
    DataBlockConsumer* consumer() { return dbc ? dbc.get() : dbc_ref; }
    DataBlockProducer* producer() { return dbp ? dbp.get() : dbp_ref; }
};

// ============================================================================
// Factories
// ============================================================================

std::unique_ptr<ShmQueue>
ShmQueue::from_consumer(std::unique_ptr<DataBlockConsumer> dbc,
                         size_t item_size, size_t flexzone_sz,
                         std::string channel_name)
{
    assert(dbc != nullptr);
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbc          = std::move(dbc);
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<ShmQueue>
ShmQueue::from_producer(std::unique_ptr<DataBlockProducer> dbp,
                         size_t item_size, size_t flexzone_sz,
                         std::string channel_name)
{
    assert(dbp != nullptr);
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbp          = std::move(dbp);
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<ShmQueue>
ShmQueue::from_consumer_ref(DataBlockConsumer& dbc, size_t item_size,
                             size_t flexzone_sz, std::string channel_name)
{
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbc_ref      = &dbc;
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<ShmQueue>
ShmQueue::from_producer_ref(DataBlockProducer& dbp, size_t item_size,
                             size_t flexzone_sz, std::string channel_name)
{
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbp_ref      = &dbp;
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// set_checksum_options
// ============================================================================

void ShmQueue::set_checksum_options(bool slot, bool fz) noexcept
{
    if (pImpl)
    {
        pImpl->checksum_slot = slot;
        pImpl->checksum_fz   = fz;
    }
}

// ============================================================================
// Constructor / destructor / move
// ============================================================================

ShmQueue::ShmQueue(std::unique_ptr<ShmQueueImpl> impl) : pImpl(std::move(impl)) {}

ShmQueue::~ShmQueue()
{
    // Release any outstanding handles before the DataBlock objects are destroyed.
    if (pImpl)
    {
        if (pImpl->read_handle && pImpl->consumer())
        {
            (void)pImpl->consumer()->release_consume_slot(*pImpl->read_handle);
        }
        if (pImpl->write_handle && pImpl->producer())
        {
            // Abort without committing — do not expose partial data.
            (void)pImpl->producer()->release_write_slot(*pImpl->write_handle);
        }
    }
}

ShmQueue::ShmQueue(ShmQueue&&) noexcept            = default;
ShmQueue& ShmQueue::operator=(ShmQueue&&) noexcept = default;

// ============================================================================
// Reading
// ============================================================================

const void* ShmQueue::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || !pImpl->consumer())
        return nullptr;

    pImpl->read_handle = pImpl->consumer()->acquire_consume_slot(
        static_cast<int>(timeout.count()));

    if (!pImpl->read_handle)
        return nullptr;

    std::span<const std::byte> buf = pImpl->read_handle->buffer_span();
    return buf.empty() ? nullptr : buf.data();
}

void ShmQueue::read_release() noexcept
{
    if (!pImpl || !pImpl->consumer() || !pImpl->read_handle)
        return;
    (void)pImpl->consumer()->release_consume_slot(*pImpl->read_handle);
    pImpl->read_handle.reset();
}

const void* ShmQueue::read_flexzone() const noexcept
{
    if (!pImpl || !pImpl->consumer() || pImpl->fz_sz == 0)
        return nullptr;
    std::span<const std::byte> fz = pImpl->consumer()->flexible_zone_span();
    return fz.empty() ? nullptr : fz.data();
}

// ============================================================================
// Writing
// ============================================================================

void* ShmQueue::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || !pImpl->producer())
        return nullptr;

    pImpl->write_handle = pImpl->producer()->acquire_write_slot(
        static_cast<int>(timeout.count()));

    if (!pImpl->write_handle)
        return nullptr;

    std::span<std::byte> buf = pImpl->write_handle->buffer_span();
    return buf.empty() ? nullptr : buf.data();
}

void ShmQueue::write_commit() noexcept
{
    if (!pImpl || !pImpl->producer() || !pImpl->write_handle)
        return;
    (void)pImpl->write_handle->commit(pImpl->item_sz);
    if (pImpl->checksum_slot)
        (void)pImpl->write_handle->update_checksum_slot();
    if (pImpl->checksum_fz)
        (void)pImpl->write_handle->update_checksum_flexible_zone();
    (void)pImpl->producer()->release_write_slot(*pImpl->write_handle);
    pImpl->write_handle.reset();
}

void ShmQueue::write_abort() noexcept
{
    if (!pImpl || !pImpl->producer() || !pImpl->write_handle)
        return;
    // Release without commit — slot is discarded and returned to the ring.
    (void)pImpl->producer()->release_write_slot(*pImpl->write_handle);
    pImpl->write_handle.reset();
}

void* ShmQueue::write_flexzone() noexcept
{
    if (!pImpl || !pImpl->producer() || pImpl->fz_sz == 0)
        return nullptr;
    std::span<std::byte> fz = pImpl->producer()->flexible_zone_span();
    return fz.empty() ? nullptr : fz.data();
}

// ============================================================================
// Metadata
// ============================================================================

size_t ShmQueue::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

size_t ShmQueue::flexzone_size() const noexcept
{
    return pImpl ? pImpl->fz_sz : 0;
}

std::string ShmQueue::name() const
{
    if (!pImpl)
        return "(null)";
    if (!pImpl->chan_name.empty())
        return pImpl->chan_name;
    if (pImpl->consumer())
        return pImpl->consumer()->name();
    if (pImpl->producer())
        return pImpl->producer()->name();
    return "(unnamed)";
}

} // namespace pylabhub::hub
