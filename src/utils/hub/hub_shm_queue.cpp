// src/utils/hub/hub_shm_queue.cpp
/**
 * @file hub_shm_queue.cpp
 * @brief ShmQueue implementation — shared-memory-backed QueueReader/QueueWriter.
 *
 * Wraps DataBlockConsumer (read) or DataBlockProducer (write).
 * No ZMQ, no protocol, no broker interaction.
 */
#include "utils/hub_shm_queue.hpp"
#include "utils/logger.hpp"

#include <cassert>
#include <cstddef>
#include <span>
#include <stdexcept>
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

    // Current acquired handles (valid between acquire and release/commit/discard):
    std::unique_ptr<SlotConsumeHandle> read_handle;
    std::unique_ptr<SlotWriteHandle>   write_handle;

    size_t      item_sz{0};
    size_t      fz_sz{0};
    std::string chan_name;

    // Write-side checksum flags.
    bool checksum_slot{false};
    bool checksum_fz{false};

    // Read-side checksum verification flags.
    // mutable: set_verify_checksum() is const on the QueueReader interface
    // (ConsumerAPI holds a const pointer; these are behavior flags, not queue state).
    mutable bool verify_slot{false};
    mutable bool verify_fz{false};

    // Last slot id from read_acquire() (monotonic slot id / commit_index).
    uint64_t last_seq{0};

    // Accessors that resolve owning vs non-owning.
    DataBlockConsumer* consumer() { return dbc ? dbc.get() : dbc_ref; }
    DataBlockProducer* producer() { return dbp ? dbp.get() : dbp_ref; }
    const DataBlockConsumer* consumer() const { return dbc ? dbc.get() : dbc_ref; }
    const DataBlockProducer* producer() const { return dbp ? dbp.get() : dbp_ref; }
};

// ============================================================================
// Factories
// ============================================================================

std::unique_ptr<QueueReader>
ShmQueue::from_consumer(std::unique_ptr<DataBlockConsumer> dbc,
                        size_t item_size, size_t flexzone_sz,
                        std::string channel_name,
                        bool verify_slot, bool verify_fz,
                        uint64_t configured_period_us)
{
    assert(dbc != nullptr);
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbc          = std::move(dbc);
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    impl->verify_slot  = verify_slot;
    impl->verify_fz    = verify_fz;
    if (configured_period_us > 0 && impl->consumer())
        impl->consumer()->set_loop_policy(LoopPolicy::FixedRate,
            std::chrono::microseconds{configured_period_us});
    return std::unique_ptr<QueueReader>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<QueueWriter>
ShmQueue::from_producer(std::unique_ptr<DataBlockProducer> dbp,
                        size_t item_size, size_t flexzone_sz,
                        std::string channel_name,
                        bool checksum_slot, bool checksum_fz,
                        uint64_t configured_period_us)
{
    assert(dbp != nullptr);
    auto impl            = std::make_unique<ShmQueueImpl>();
    impl->dbp            = std::move(dbp);
    impl->item_sz        = item_size;
    impl->fz_sz          = flexzone_sz;
    impl->chan_name       = std::move(channel_name);
    impl->checksum_slot  = checksum_slot;
    impl->checksum_fz    = checksum_fz;
    if (configured_period_us > 0 && impl->producer())
        impl->producer()->set_loop_policy(LoopPolicy::FixedRate,
            std::chrono::microseconds{configured_period_us});
    return std::unique_ptr<QueueWriter>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<QueueReader>
ShmQueue::from_consumer_ref(DataBlockConsumer& dbc, size_t item_size,
                             size_t flexzone_sz, std::string channel_name,
                             bool verify_slot, bool verify_fz,
                             uint64_t configured_period_us)
{
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbc_ref      = &dbc;
    impl->item_sz      = item_size;
    impl->fz_sz        = flexzone_sz;
    impl->chan_name     = std::move(channel_name);
    impl->verify_slot  = verify_slot;
    impl->verify_fz    = verify_fz;
    if (configured_period_us > 0)
        dbc.set_loop_policy(LoopPolicy::FixedRate,
            std::chrono::microseconds{configured_period_us});
    return std::unique_ptr<QueueReader>(new ShmQueue(std::move(impl)));
}

std::unique_ptr<QueueWriter>
ShmQueue::from_producer_ref(DataBlockProducer& dbp, size_t item_size,
                             size_t flexzone_sz, std::string channel_name,
                             bool checksum_slot, bool checksum_fz,
                             uint64_t configured_period_us)
{
    auto impl            = std::make_unique<ShmQueueImpl>();
    impl->dbp_ref        = &dbp;
    impl->item_sz        = item_size;
    impl->fz_sz          = flexzone_sz;
    impl->chan_name       = std::move(channel_name);
    impl->checksum_slot  = checksum_slot;
    impl->checksum_fz    = checksum_fz;
    if (configured_period_us > 0)
        dbp.set_loop_policy(LoopPolicy::FixedRate,
            std::chrono::microseconds{configured_period_us});
    return std::unique_ptr<QueueWriter>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// set_checksum_options  (QueueWriter interface)
// ============================================================================

void ShmQueue::set_checksum_options(bool slot, bool fz) noexcept
{
    if (pImpl)
    {
        pImpl->checksum_slot = slot;
        pImpl->checksum_fz   = fz;
    }
}

void ShmQueue::sync_flexzone_checksum() noexcept
{
    if (pImpl && pImpl->producer() && pImpl->fz_sz > 0)
        (void)pImpl->producer()->update_checksum_flexible_zone();
}

// ============================================================================
// set_verify_checksum  (QueueReader interface)
// ============================================================================

void ShmQueue::set_verify_checksum(bool slot, bool fz) const noexcept
{
    if (pImpl)
    {
        pImpl->verify_slot = slot;
        pImpl->verify_fz   = fz;
    }
}

// ============================================================================
// Constructor / destructor / move
// ============================================================================

bool ShmQueue::is_running() const noexcept
{
    // Returns false on a moved-from (null-pImpl) instance.
    // A live ShmQueue always has a valid DataBlock; the "running" state is
    // implicit (no start() / stop() lifecycle, no background thread).
    return pImpl && (pImpl->consumer() != nullptr || pImpl->producer() != nullptr);
}

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

    // Store monotonic slot id for last_seq().
    pImpl->last_seq = pImpl->read_handle->slot_id();

    // Optional checksum verification.
    if (pImpl->verify_slot && !pImpl->read_handle->verify_checksum_slot())
    {
        LOGGER_ERROR("[ShmQueue] slot checksum mismatch on slot {} channel '{}'",
                     pImpl->last_seq, pImpl->chan_name);
        (void)pImpl->consumer()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
        return nullptr;
    }
    if (pImpl->verify_fz && !pImpl->read_handle->verify_checksum_flexible_zone())
    {
        LOGGER_ERROR("[ShmQueue] flexzone checksum mismatch on slot {} channel '{}'",
                     pImpl->last_seq, pImpl->chan_name);
        (void)pImpl->consumer()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
        return nullptr;
    }

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

uint64_t ShmQueue::last_seq() const noexcept
{
    return pImpl ? pImpl->last_seq : 0;
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
    // Compute checksums BEFORE commit() marks the slot as written.
    // commit() sets the slot size header; a reader racing past release_write_slot()
    // must see a consistent checksum.  CR-03: always checksum before publishing.
    if (pImpl->checksum_slot)
        (void)pImpl->write_handle->update_checksum_slot();
    if (pImpl->checksum_fz)
        (void)pImpl->write_handle->update_checksum_flexible_zone();
    (void)pImpl->write_handle->commit(pImpl->item_sz);
    (void)pImpl->producer()->release_write_slot(*pImpl->write_handle);
    pImpl->write_handle.reset();
}

void ShmQueue::write_discard() noexcept
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

size_t ShmQueue::capacity() const
{
    if (!pImpl)
        throw std::runtime_error("ShmQueue::capacity(): not connected");
    DataBlockMetrics m{};
    if (const DataBlockConsumer* c = pImpl->consumer())
    {
        if (c->get_metrics(m) == 0)
            return static_cast<size_t>(m.slot_count);
    }
    else if (const DataBlockProducer* p = pImpl->producer())
    {
        if (p->get_metrics(m) == 0)
            return static_cast<size_t>(m.slot_count);
    }
    throw std::runtime_error("ShmQueue::capacity(): failed to query DataBlock metrics");
}

std::string ShmQueue::policy_info() const
{
    if (!pImpl)
        return "shm_unconnected";
    if (pImpl->consumer())
        return "shm_read";
    if (pImpl->producer())
        return "shm_write";
    return "shm_unconnected";
}

// ============================================================================
// Metrics
// ============================================================================

QueueMetrics ShmQueue::metrics() const noexcept
{
    if (!pImpl) return {};

    // ShmQueue wraps exactly one DataBlock handle — either consumer (read mode)
    // or producer (write mode), never both. The metrics reflect this handle's
    // perspective: how this role experiences the shared memory interaction.
    // The mode is fixed at factory time and never changes.

    QueueMetrics m;

    if (const DataBlockConsumer* c = pImpl->consumer())
    {
        const auto &cm = c->metrics();
        m.last_slot_wait_us    = cm.last_slot_wait_us;
        m.last_iteration_us    = cm.last_iteration_us;
        m.max_iteration_us     = cm.max_iteration_us;
        m.context_elapsed_us   = cm.context_elapsed_us;
        m.last_slot_exec_us    = cm.last_slot_exec_us;
        m.configured_period_us = cm.configured_period_us;
        // recv_overflow_count stays 0 for SHM. DataBlock sync policies (Sequential,
        // Latest_only) prevent data loss at the queue level — Sequential blocks the
        // producer, Latest_only skips by design. No receive buffer overflow is possible.
        return m;
    }

    if (const DataBlockProducer* p = pImpl->producer())
    {
        const auto &cm = p->metrics();
        m.last_slot_wait_us    = cm.last_slot_wait_us;
        m.last_iteration_us    = cm.last_iteration_us;
        m.max_iteration_us     = cm.max_iteration_us;
        m.context_elapsed_us   = cm.context_elapsed_us;
        m.last_slot_exec_us    = cm.last_slot_exec_us;
        m.configured_period_us = cm.configured_period_us;
        return m;
    }

    return m; // moved-from or invalid — all zeros
}

void ShmQueue::reset_metrics()
{
    if (!pImpl) return;
    // Reset both sides unconditionally — safe on null (returns immediately).
    if (DataBlockConsumer* c = pImpl->consumer())
        c->clear_metrics();
    if (DataBlockProducer* p = pImpl->producer())
        p->clear_metrics();
}

void ShmQueue::set_configured_period(uint64_t period_us)
{
    if (!pImpl) return;
    auto policy = (period_us > 0) ? LoopPolicy::FixedRate : LoopPolicy::MaxRate;
    auto period = std::chrono::microseconds{period_us};
    // Single-mode: only one handle is non-null.
    if (DataBlockConsumer* c = pImpl->consumer())
        c->set_loop_policy(policy, period);
    else if (DataBlockProducer* p = pImpl->producer())
        p->set_loop_policy(policy, period);
}

} // namespace pylabhub::hub
