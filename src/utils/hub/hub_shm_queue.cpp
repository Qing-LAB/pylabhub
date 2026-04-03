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
    // Owning pointers (created by create_writer / create_reader):
    std::unique_ptr<DataBlockConsumer> dbc;
    std::unique_ptr<DataBlockProducer> dbp;

    // Current acquired handles (valid between acquire and release/commit/discard):
    std::unique_ptr<SlotConsumeHandle> read_handle;
    std::unique_ptr<SlotWriteHandle>   write_handle;

    size_t      item_sz{0};
    size_t      fz_sz{0};
    std::string chan_name;

    // Write-side checksum flags.
    bool checksum_slot{false};
    bool checksum_fz{false};
    bool always_clear_slot{true}; ///< Zero buffer on write_acquire (default: true for safety).

    // Read-side checksum verification flags.
    // mutable: set_verify_checksum() is const on ShmQueue
    // (behavior flags, not queue state).
    mutable bool verify_slot{false};
    mutable bool verify_fz{false};

    // Last slot id from read_acquire() (monotonic slot id / commit_index).
    uint64_t last_seq{0};

    DataBlockConsumer* consumer() { return dbc.get(); }
    DataBlockProducer* producer() { return dbp.get(); }
    const DataBlockConsumer* consumer() const { return dbc.get(); }
    const DataBlockProducer* producer() const { return dbp.get(); }
};

// Old factories removed — use create_writer() / create_reader() instead.

// ============================================================================
// create_writer — creates DataBlock internally from schema
// ============================================================================

std::unique_ptr<ShmQueue>
ShmQueue::create_writer(const std::string &channel_name,
                        const std::vector<SchemaFieldDesc> &slot_schema,
                        const std::string &slot_packing,
                        const std::vector<SchemaFieldDesc> &fz_schema,
                        const std::string &fz_packing,
                        uint32_t ring_buffer_capacity,
                        DataBlockPageSize page_size,
                        uint64_t shared_secret,
                        DataBlockPolicy policy,
                        ConsumerSyncPolicy sync_policy,
                        ChecksumPolicy checksum_policy,
                        bool checksum_slot,
                        bool checksum_fz,
                        bool always_clear_slot,
                        const std::string &hub_uid,
                        const std::string &hub_name,
                        const schema::SchemaInfo *slot_schema_info,
                        const schema::SchemaInfo *fz_schema_info,
                        const std::string &producer_uid,
                        const std::string &producer_name)
{
    if (slot_schema.empty())
    {
        LOGGER_ERROR("[ShmQueue] create_writer: slot_schema is empty");
        return nullptr;
    }

    // Compute slot size from schema (authoritative).
    auto [slot_layout, item_size] = compute_field_layout(slot_schema, slot_packing);
    if (item_size == 0)
    {
        LOGGER_ERROR("[ShmQueue] create_writer: computed slot size is 0");
        return nullptr;
    }

    // Compute flexzone size from schema (0 if no flexzone).
    size_t fz_size = 0;
    if (!fz_schema.empty())
    {
        auto [fz_layout, raw_fz_size] = compute_field_layout(fz_schema, fz_packing);
        // Round to 4KB page boundary.
        fz_size = (raw_fz_size + 4095U) & ~size_t{4095U};
    }

    // Build DataBlockConfig.
    DataBlockConfig config;
    config.logical_unit_size    = item_size;
    config.flex_zone_size       = fz_size;
    config.ring_buffer_capacity = ring_buffer_capacity;
    config.physical_page_size   = page_size;
    config.shared_secret        = shared_secret;
    config.policy               = policy;
    config.consumer_sync_policy = sync_policy;
    config.checksum_policy      = checksum_policy;
    config.hub_uid              = hub_uid;
    config.hub_name             = hub_name;
    config.producer_uid         = producer_uid;
    config.producer_name        = producer_name;

    // Create DataBlock.
    auto dbp = create_datablock_producer_impl(
        channel_name, policy, config, fz_schema_info, slot_schema_info);
    if (!dbp)
    {
        LOGGER_ERROR("[ShmQueue] create_writer: DataBlock creation failed for '{}'",
                     channel_name);
        return nullptr;
    }

    // Build ShmQueue wrapping the owned DataBlock.
    auto impl               = std::make_unique<ShmQueueImpl>();
    impl->dbp               = std::move(dbp);
    impl->item_sz            = item_size;
    impl->fz_sz              = fz_size;
    impl->chan_name           = channel_name;
    impl->checksum_slot      = checksum_slot;
    impl->checksum_fz        = checksum_fz;
    impl->always_clear_slot  = always_clear_slot;
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// create_reader — attaches to existing DataBlock, validates schema
// ============================================================================

std::unique_ptr<ShmQueue>
ShmQueue::create_reader(const std::string &shm_name,
                        uint64_t shared_secret,
                        const std::vector<SchemaFieldDesc> &expected_slot_schema,
                        const std::string &expected_packing,
                        const std::string &channel_name,
                        bool verify_slot,
                        bool verify_fz,
                        const std::string &consumer_uid,
                        const std::string &consumer_name)
{
    // Attach to existing DataBlock.
    const char *uid  = consumer_uid.empty()  ? nullptr : consumer_uid.c_str();
    const char *cnam = consumer_name.empty() ? nullptr : consumer_name.c_str();
    std::unique_ptr<DataBlockConsumer> dbc;
    try
    {
        dbc = find_datablock_consumer_impl(shm_name, shared_secret,
                                           nullptr, nullptr, nullptr, uid, cnam);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[ShmQueue] create_reader: attachment failed for '{}': {}",
                     shm_name, e.what());
        return nullptr;
    }
    if (!dbc)
    {
        // Attachment failed — SHM not found or secret mismatch.
        // This is not necessarily an error (ZMQ fallback may be available).
        return nullptr;
    }

    // Compute expected slot size from schema.
    // Schema hash validation (done at broker level) ensures both sides agree on layout.
    size_t item_size = 0;
    if (!expected_slot_schema.empty())
    {
        auto [layout, sz] = compute_field_layout(expected_slot_schema, expected_packing);
        item_size = sz;
    }

    // Flexzone size from the DataBlock's actual flexible zone span.
    const size_t fz_size = dbc->flexible_zone_span().size();

    // Build ShmQueue wrapping the owned DataBlock.
    auto impl          = std::make_unique<ShmQueueImpl>();
    impl->dbc          = std::move(dbc);
    impl->item_sz      = item_size;
    impl->fz_sz        = fz_size;
    impl->chan_name     = channel_name;
    impl->verify_slot  = verify_slot;
    impl->verify_fz    = verify_fz;
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// raw_producer / raw_consumer — internal accessors for template RAII path
// ============================================================================

DataBlockProducer *ShmQueue::raw_producer() noexcept
{
    return pImpl ? pImpl->producer() : nullptr;
}

DataBlockConsumer *ShmQueue::raw_consumer() noexcept
{
    return pImpl ? pImpl->consumer() : nullptr;
}

// ============================================================================
// set_checksum_options  (ShmQueue-specific)
// ============================================================================

void ShmQueue::set_always_clear_slot(bool enable) noexcept
{
    if (pImpl)
        pImpl->always_clear_slot = enable;
}

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
// set_verify_checksum  (ShmQueue-specific)
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
// Unified checksum interface (overrides QueueReader/QueueWriter base)
// ============================================================================

void ShmQueue::set_checksum_policy(ChecksumPolicy policy)
{
    if (!pImpl) return;
    // Write side: auto-stamp only for Enforced. Manual = caller stamps explicitly.
    pImpl->checksum_slot = (policy == ChecksumPolicy::Enforced);
    // Read side: always verify for Manual and Enforced (catches missing stamps).
    pImpl->verify_slot = (policy != ChecksumPolicy::None);
}

void ShmQueue::set_flexzone_checksum(bool enabled)
{
    if (!pImpl) return;
    pImpl->checksum_fz = enabled;
    pImpl->verify_fz   = enabled;
}

void ShmQueue::update_checksum()
{
    if (!pImpl || !pImpl->write_handle) return;
    (void)pImpl->write_handle->update_checksum_slot();
}

void ShmQueue::update_flexzone_checksum()
{
    if (!pImpl || !pImpl->write_handle) return;
    (void)pImpl->write_handle->update_checksum_flexible_zone();
}

bool ShmQueue::verify_checksum()
{
    if (!pImpl || !pImpl->read_handle) return true;
    return pImpl->read_handle->verify_checksum_slot();
}

bool ShmQueue::verify_flexzone_checksum()
{
    if (!pImpl || !pImpl->read_handle) return true;
    return pImpl->read_handle->verify_checksum_flexible_zone();
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

    // Optional checksum verification (pre-read gate, honors slot_cks_is_valid).
    if (pImpl->verify_slot && !pImpl->read_handle->verify_checksum_slot())
    {
        pImpl->consumer()->mutable_metrics().inc_checksum_error();
        LOGGER_ERROR("[ShmQueue] slot checksum error on slot {} channel '{}'",
                     pImpl->last_seq, pImpl->chan_name);
        (void)pImpl->consumer()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
        return nullptr;
    }
    if (pImpl->verify_fz && !pImpl->read_handle->verify_checksum_flexible_zone())
    {
        pImpl->consumer()->mutable_metrics().inc_checksum_error();
        LOGGER_ERROR("[ShmQueue] flexzone checksum error on slot {} channel '{}'",
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
    if (buf.empty()) return nullptr;
    if (pImpl->always_clear_slot)
        std::fill(buf.begin(), buf.end(), std::byte{0});
    return buf.data();
}

void ShmQueue::write_commit() noexcept
{
    if (!pImpl || !pImpl->producer() || !pImpl->write_handle)
        return;
    // Compute or invalidate checksum BEFORE commit() marks the slot as written.
    // commit() sets the slot size header; a reader racing past release_write_slot()
    // must see a consistent checksum.  CR-03: always checksum before publishing.
    if (pImpl->checksum_slot)
        (void)pImpl->write_handle->update_checksum_slot();
    else
        pImpl->write_handle->invalidate_checksum_slot();
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
        m.last_slot_wait_us    = cm.last_slot_wait_us_val();
        m.last_iteration_us    = cm.last_iteration_us_val();
        m.max_iteration_us     = cm.max_iteration_us_val();
        m.context_elapsed_us   = cm.context_elapsed_us_val();
        m.last_slot_exec_us    = cm.last_slot_exec_us_val();
        // configured_period_us reported at loop level (LoopMetricsSnapshot), not queue level.
        // recv_overflow_count stays 0 for SHM (no receive buffer overflow possible).
        m.checksum_error_count = cm.checksum_error_count_val();
        return m;
    }

    if (const DataBlockProducer* p = pImpl->producer())
    {
        const auto &cm = p->metrics();
        m.last_slot_wait_us    = cm.last_slot_wait_us_val();
        m.last_iteration_us    = cm.last_iteration_us_val();
        m.max_iteration_us     = cm.max_iteration_us_val();
        m.context_elapsed_us   = cm.context_elapsed_us_val();
        m.last_slot_exec_us    = cm.last_slot_exec_us_val();
        // configured_period_us reported at loop level (LoopMetricsSnapshot), not queue level.
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
    // Write directly to ContextMetrics via DataBlock mutable_metrics() accessor.
    if (DataBlockConsumer* c = pImpl->consumer())
        c->mutable_metrics().set_configured_period(period_us);
    else if (DataBlockProducer* p = pImpl->producer())
        p->mutable_metrics().set_configured_period(period_us);
}

} // namespace pylabhub::hub
