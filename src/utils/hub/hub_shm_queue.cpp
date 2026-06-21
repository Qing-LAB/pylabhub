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
#include "utils/schema_utils.hpp" // align_to_physical_page

#include <nlohmann/json.hpp>

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
    // ── HEP-CORE-0036 §6.7 state ────────────────────────────────────────────
    // Owning pointers populated by start() (Configured → Active transition).
    // Both null in Standby + Configured; one is set in Active depending on
    // mode_is_reader.
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

    // ── HEP-CORE-0036 §6.7 deferred-attach state (Standby / Configured) ─────
    //
    // Stored at factory time so start() can do the actual SHM attach /
    // segment creation using artifacts populated via set_shm_secret().
    // Set on a Standby-mode factory; ignored when neither factory used
    // the deferred path (the legacy direct-attach factories populate
    // dbc/dbp immediately and leave these defaults).
    bool        mode_is_reader{false};       ///< true: reader; false: writer
    bool        has_shm_secret{false};       ///< true once set_shm_secret called
    uint64_t    pending_shm_secret{0};

    // HEP-CORE-0041 capability-transport path (mutually exclusive with
    // shm_secret above).  Borrowed fd from IShmCapabilityProducer (writer
    // side) or received via SCM_RIGHTS (reader side); DataBlock fd-source
    // factories dup internally so ShmQueue does NOT own the fd.
    bool        has_capability_fd{false};
    int         pending_capability_fd{-1};

    // Reader-side deferred params.
    std::string                       pending_shm_name;
    std::vector<SchemaFieldDesc>      pending_expected_slot_schema;
    std::string                       pending_expected_packing;
    std::string                       pending_consumer_uid;
    std::string                       pending_consumer_name;

    // Writer-side deferred params.  Big — but the alternative (separate
    // Impl per mode) would require dispatching in every method.  Kept
    // here so the queue object is mode-agnostic at the C++ type level.
    std::vector<SchemaFieldDesc>      pending_slot_schema;
    std::string                       pending_slot_packing;
    std::vector<SchemaFieldDesc>      pending_fz_schema;
    std::string                       pending_fz_packing;
    uint32_t                          pending_ring_buffer_capacity{0};
    DataBlockPageSize                 pending_page_size{DataBlockPageSize::Size4K};
    DataBlockPolicy                   pending_policy{DataBlockPolicy::RingBuffer};
    ConsumerSyncPolicy                pending_sync_policy{ConsumerSyncPolicy::Latest_only};
    ChecksumPolicy                    pending_checksum_policy{ChecksumPolicy::None};
    std::string                       pending_hub_uid;
    std::string                       pending_hub_name;
    std::string                       pending_producer_uid;
    std::string                       pending_producer_name;
    // SchemaInfo lives in the caller's memory for the role host's lifetime;
    // we hold raw pointers and rely on caller-managed lifetime extension to
    // start().  In practice the role host owns these throughout startup.
    const schema::SchemaInfo *        pending_slot_schema_info{nullptr};
    const schema::SchemaInfo *        pending_fz_schema_info{nullptr};
};

// Old factories removed — use create_writer() / create_reader() instead.

// ============================================================================
// create_writer — HEP-CORE-0036 §6.7 Standby + apply + start convenience
// ============================================================================
//
// Builds a Standby ShmQueue (no segment created), applies the broker /
// config-supplied secret (Standby → Configured), then calls start() to
// actually create the SHM segment (Configured → Active).  Returns
// nullptr if any phase fails — preserves the legacy semantics where
// "create_writer returns nullptr on creation failure".  Production code
// that gets the secret pre-register (config-supplied) can use this
// convenience signature unchanged.  Future AUTH-4 (broker-supplied
// secret post-register) will construct a Standby ShmQueue directly
// (signature without secret — exposed below), then drive set_shm_secret
// + start() through `apply_master_approval`.

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
        fz_size = align_to_physical_page(raw_fz_size);
    }

    // ── Standby: build the queue object with all schema metadata + the
    // deferred-attach parameters.  No SHM segment created yet.
    auto impl                     = std::make_unique<ShmQueueImpl>();
    impl->mode_is_reader          = false;
    impl->item_sz                 = item_size;
    impl->fz_sz                   = fz_size;
    impl->chan_name               = channel_name;
    impl->checksum_slot           = checksum_slot;
    impl->checksum_fz             = checksum_fz;
    impl->always_clear_slot       = always_clear_slot;
    impl->pending_slot_schema     = slot_schema;
    impl->pending_slot_packing    = slot_packing;
    impl->pending_fz_schema       = fz_schema;
    impl->pending_fz_packing      = fz_packing;
    impl->pending_ring_buffer_capacity = ring_buffer_capacity;
    impl->pending_page_size       = page_size;
    impl->pending_policy          = policy;
    impl->pending_sync_policy     = sync_policy;
    impl->pending_checksum_policy = checksum_policy;
    impl->pending_hub_uid         = hub_uid;
    impl->pending_hub_name        = hub_name;
    impl->pending_producer_uid    = producer_uid;
    impl->pending_producer_name   = producer_name;
    impl->pending_slot_schema_info = slot_schema_info;
    impl->pending_fz_schema_info  = fz_schema_info;
    std::unique_ptr<ShmQueue> queue(new ShmQueue(std::move(impl)));

    // ── Configured: apply the broker/config-supplied secret.
    if (!queue->set_shm_secret(shared_secret))
        return nullptr;

    // ── Active: actually create the SHM segment.  HEP §6.7 "either
    // fully transitioned or fully refused": on failure, the queue is
    // destroyed and nullptr returned — preserves the legacy contract
    // where the convenience factory returns nullptr on any error.
    if (!queue->start())
        return nullptr;
    return queue;
}

// ============================================================================
// create_reader — HEP-CORE-0036 §6.7 Standby + apply + start convenience
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
    // Compute expected slot size from schema.  Schema hash validation
    // (broker level) ensures both sides agree on layout; we keep
    // item_sz for the metadata accessor.
    size_t item_size = 0;
    if (!expected_slot_schema.empty())
    {
        auto [layout, sz] = compute_field_layout(expected_slot_schema, expected_packing);
        item_size = sz;
    }

    // ── Standby: build the queue object with deferred-attach params.
    auto impl                          = std::make_unique<ShmQueueImpl>();
    impl->mode_is_reader               = true;
    impl->item_sz                      = item_size;
    impl->chan_name                    = channel_name;
    impl->verify_slot                  = verify_slot;
    impl->verify_fz                    = verify_fz;
    impl->pending_shm_name             = shm_name;
    impl->pending_expected_slot_schema = expected_slot_schema;
    impl->pending_expected_packing     = expected_packing;
    impl->pending_consumer_uid         = consumer_uid;
    impl->pending_consumer_name        = consumer_name;
    std::unique_ptr<ShmQueue> queue(new ShmQueue(std::move(impl)));

    // ── Configured + Active: apply secret + attach.  On failure
    // (segment not found, secret mismatch, schema mismatch) return
    // nullptr — preserves legacy "create_reader returns nullptr on
    // bad secret" semantics.
    if (!queue->set_shm_secret(shared_secret))
        return nullptr;
    if (!queue->start())
        return nullptr;
    return queue;
}

// ============================================================================
// create_writer_standby — HEP-CORE-0041 capability-transport Standby factory
// ============================================================================
//
// Same Standby-build block as create_writer above, minus the
// set_shm_secret + start convenience.  Role host drives Standby →
// Configured by calling set_shm_capability_fd(fd) once the L1
// IShmCapabilityProducer has the memfd ready, then start().

std::unique_ptr<ShmQueue>
ShmQueue::create_writer_standby(const std::string &channel_name,
                                const std::vector<SchemaFieldDesc> &slot_schema,
                                const std::string &slot_packing,
                                const std::vector<SchemaFieldDesc> &fz_schema,
                                const std::string &fz_packing,
                                uint32_t ring_buffer_capacity,
                                DataBlockPageSize page_size,
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
        LOGGER_ERROR("[ShmQueue] create_writer_standby: slot_schema is empty");
        return nullptr;
    }

    auto [slot_layout, item_size] = compute_field_layout(slot_schema, slot_packing);
    if (item_size == 0)
    {
        LOGGER_ERROR("[ShmQueue] create_writer_standby: computed slot size is 0");
        return nullptr;
    }

    size_t fz_size = 0;
    if (!fz_schema.empty())
    {
        auto [fz_layout, raw_fz_size] = compute_field_layout(fz_schema, fz_packing);
        fz_size = align_to_physical_page(raw_fz_size);
    }

    auto impl                     = std::make_unique<ShmQueueImpl>();
    impl->mode_is_reader          = false;
    impl->item_sz                 = item_size;
    impl->fz_sz                   = fz_size;
    impl->chan_name               = channel_name;
    impl->checksum_slot           = checksum_slot;
    impl->checksum_fz             = checksum_fz;
    impl->always_clear_slot       = always_clear_slot;
    impl->pending_slot_schema     = slot_schema;
    impl->pending_slot_packing    = slot_packing;
    impl->pending_fz_schema       = fz_schema;
    impl->pending_fz_packing      = fz_packing;
    impl->pending_ring_buffer_capacity = ring_buffer_capacity;
    impl->pending_page_size       = page_size;
    impl->pending_policy          = policy;
    impl->pending_sync_policy     = sync_policy;
    impl->pending_checksum_policy = checksum_policy;
    impl->pending_hub_uid         = hub_uid;
    impl->pending_hub_name        = hub_name;
    impl->pending_producer_uid    = producer_uid;
    impl->pending_producer_name   = producer_name;
    impl->pending_slot_schema_info = slot_schema_info;
    impl->pending_fz_schema_info  = fz_schema_info;
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// create_reader_standby — HEP-CORE-0041 capability-transport Standby factory
// ============================================================================

std::unique_ptr<ShmQueue>
ShmQueue::create_reader_standby(const std::string &shm_name,
                                const std::vector<SchemaFieldDesc> &expected_slot_schema,
                                const std::string &expected_packing,
                                const std::string &channel_name,
                                bool verify_slot,
                                bool verify_fz,
                                const std::string &consumer_uid,
                                const std::string &consumer_name)
{
    size_t item_size = 0;
    if (!expected_slot_schema.empty())
    {
        auto [layout, sz] = compute_field_layout(expected_slot_schema, expected_packing);
        item_size = sz;
    }

    auto impl                          = std::make_unique<ShmQueueImpl>();
    impl->mode_is_reader               = true;
    impl->item_sz                      = item_size;
    impl->chan_name                    = channel_name;
    impl->verify_slot                  = verify_slot;
    impl->verify_fz                    = verify_fz;
    impl->pending_shm_name             = shm_name;
    impl->pending_expected_slot_schema = expected_slot_schema;
    impl->pending_expected_packing     = expected_packing;
    impl->pending_consumer_uid         = consumer_uid;
    impl->pending_consumer_name        = consumer_name;
    return std::unique_ptr<ShmQueue>(new ShmQueue(std::move(impl)));
}

// ============================================================================
// set_shm_secret — HEP-CORE-0036 §6.7 Standby → Configured
// ============================================================================

bool ShmQueue::set_shm_secret(uint64_t secret) noexcept
{
    if (!pImpl) return false;
    // §6.7 mutator table: refuse from Active (`shm_secret` is per-channel-
    // lifetime; restart needed).
    if (pImpl->dbc.get() != nullptr || pImpl->dbp.get() != nullptr)
    {
        LOGGER_WARN("[ShmQueue] set_shm_secret refused: queue is Active "
                    "(`shm_secret` is per-channel-lifetime; HEP-CORE-0036 §6.7)");
        return false;
    }
    // HEP-CORE-0041 D7 "single unified mechanism": a queue uses EITHER the
    // legacy secret-based path OR the capability-transport path, never
    // both.  The secret-based path retires in 1i-cleanup; this guard
    // catches accidental mixing during the migration window.
    if (pImpl->has_capability_fd)
    {
        LOGGER_WARN("[ShmQueue] set_shm_secret refused: queue is already "
                    "in capability-transport mode (HEP-CORE-0041 D7 — "
                    "modes are mutually exclusive)");
        return false;
    }
    pImpl->pending_shm_secret = secret;
    pImpl->has_shm_secret = true;
    return true;
}

// ============================================================================
// set_shm_capability_fd — HEP-CORE-0041 Standby → Configured (capability)
// ============================================================================

bool ShmQueue::set_shm_capability_fd(int fd) noexcept
{
    if (!pImpl) return false;
    if (fd < 0)
    {
        LOGGER_WARN("[ShmQueue] set_shm_capability_fd refused: fd<0 "
                    "(HEP-CORE-0041 §5.5 capability transport)");
        return false;
    }
    // §6.7 mutator table semantics (carried over to the capability path):
    // refuse from Active.  An Active queue's underlying DataBlock already
    // wraps the SHM mapping; swapping the fd would require a teardown
    // first.
    if (pImpl->dbc.get() != nullptr || pImpl->dbp.get() != nullptr)
    {
        LOGGER_WARN("[ShmQueue] set_shm_capability_fd refused: queue is "
                    "Active (capability fd is per-channel-lifetime; "
                    "HEP-CORE-0036 §6.7 / HEP-CORE-0041 §5.5)");
        return false;
    }
    // Mutual exclusion with the legacy secret-based path (HEP-CORE-0041 D7).
    if (pImpl->has_shm_secret)
    {
        LOGGER_WARN("[ShmQueue] set_shm_capability_fd refused: queue is "
                    "already in secret-based mode (HEP-CORE-0041 D7 — "
                    "modes are mutually exclusive)");
        return false;
    }
    pImpl->pending_capability_fd = fd;
    pImpl->has_capability_fd     = true;
    return true;
}

// ============================================================================
// start — HEP-CORE-0036 §6.7 Configured → Active
// ============================================================================

bool ShmQueue::start()
{
    if (!pImpl) return false;
    // Already Active: idempotent no-op (HEP §6.7 mutator table).
    if (pImpl->dbc.get() != nullptr || pImpl->dbp.get() != nullptr)
        return true;
    // Standby (neither mode applied): refuse per §6.7 mutator table.
    if (!pImpl->has_shm_secret && !pImpl->has_capability_fd)
    {
        LOGGER_ERROR("[ShmQueue] start refused: queue in Standby (neither "
                     "shm_secret nor capability fd applied; "
                     "HEP-CORE-0036 §6.7 / HEP-CORE-0041 §5.5)");
        return false;
    }

    // HEP-CORE-0041 capability-transport path takes precedence (the
    // legacy secret-based branch below retires in 1i-cleanup; mutual
    // exclusion in set_shm_secret + set_shm_capability_fd ensures at
    // most one is set at start() time).
    if (pImpl->has_capability_fd)
    {
        if (pImpl->mode_is_reader)
        {
            const char *uid  = pImpl->pending_consumer_uid.empty()
                                   ? nullptr : pImpl->pending_consumer_uid.c_str();
            const char *cnam = pImpl->pending_consumer_name.empty()
                                   ? nullptr : pImpl->pending_consumer_name.c_str();
            std::unique_ptr<DataBlockConsumer> dbc;
            try
            {
                // No expected_config — the producer-side ftruncate fixed
                // the layout; consumer validates against the SHM header
                // it reads from the received fd.
                dbc = find_datablock_consumer_from_fd_impl(
                    pImpl->chan_name, pImpl->pending_capability_fd,
                    /*expected_config=*/nullptr,
                    pImpl->pending_fz_schema_info,
                    pImpl->pending_slot_schema_info,
                    uid, cnam);
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[ShmQueue] start (capability): consumer "
                             "attach failed for channel='{}' fd={}: {}",
                             pImpl->chan_name,
                             pImpl->pending_capability_fd, e.what());
                return false;
            }
            if (!dbc) return false;
            pImpl->fz_sz = dbc->flexible_zone_span().size();
            pImpl->dbc   = std::move(dbc);
            return true;
        }

        // Writer mode (capability): wrap the externally-allocated fd
        // (typically from IShmCapabilityProducer::borrow_fd()).  The
        // L1 transport pre-allocated via memfd_create + ftruncate to
        // datablock_layout_total_size(config); the fd-source ctor
        // validates fstat size matches.
        DataBlockConfig config;
        config.logical_unit_size    = pImpl->item_sz;
        config.flex_zone_size       = pImpl->fz_sz;
        config.ring_buffer_capacity = pImpl->pending_ring_buffer_capacity;
        config.physical_page_size   = pImpl->pending_page_size;
        // shared_secret is irrelevant on the capability path; left at
        // default (0).  Auth is via L2 attach handshake (HEP-0041 §5.5).
        config.policy               = pImpl->pending_policy;
        config.consumer_sync_policy = pImpl->pending_sync_policy;
        config.checksum_policy      = pImpl->pending_checksum_policy;
        config.hub_uid              = pImpl->pending_hub_uid;
        config.hub_name             = pImpl->pending_hub_name;
        config.producer_uid         = pImpl->pending_producer_uid;
        config.producer_name        = pImpl->pending_producer_name;
        std::unique_ptr<DataBlockProducer> dbp;
        try
        {
            dbp = create_datablock_producer_from_fd_impl(
                pImpl->chan_name, pImpl->pending_capability_fd,
                pImpl->pending_policy, config,
                pImpl->pending_fz_schema_info,
                pImpl->pending_slot_schema_info);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[ShmQueue] start (capability): producer create "
                         "failed for channel='{}' fd={}: {}",
                         pImpl->chan_name,
                         pImpl->pending_capability_fd, e.what());
            return false;
        }
        if (!dbp) return false;
        pImpl->dbp = std::move(dbp);
        return true;
    }

    // Legacy secret-based path (1i-cleanup will delete this entire branch).
    if (pImpl->mode_is_reader)
    {
        const char *uid  = pImpl->pending_consumer_uid.empty()
                               ? nullptr : pImpl->pending_consumer_uid.c_str();
        const char *cnam = pImpl->pending_consumer_name.empty()
                               ? nullptr : pImpl->pending_consumer_name.c_str();
        std::unique_ptr<DataBlockConsumer> dbc;
        try
        {
            dbc = find_datablock_consumer_impl(
                pImpl->pending_shm_name, pImpl->pending_shm_secret,
                nullptr, nullptr, nullptr, uid, cnam);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[ShmQueue] start: attachment failed for '{}': {}",
                         pImpl->pending_shm_name, e.what());
            return false;
        }
        if (!dbc) return false;  // segment missing or secret mismatch
        // Recompute fz_sz from the actual attached segment.
        pImpl->fz_sz = dbc->flexible_zone_span().size();
        pImpl->dbc = std::move(dbc);
        return true;
    }

    // Writer mode: create the SHM segment.
    DataBlockConfig config;
    config.logical_unit_size    = pImpl->item_sz;
    config.flex_zone_size       = pImpl->fz_sz;
    config.ring_buffer_capacity = pImpl->pending_ring_buffer_capacity;
    config.physical_page_size   = pImpl->pending_page_size;
    config.shared_secret        = pImpl->pending_shm_secret;
    config.policy               = pImpl->pending_policy;
    config.consumer_sync_policy = pImpl->pending_sync_policy;
    config.checksum_policy      = pImpl->pending_checksum_policy;
    config.hub_uid              = pImpl->pending_hub_uid;
    config.hub_name             = pImpl->pending_hub_name;
    config.producer_uid         = pImpl->pending_producer_uid;
    config.producer_name        = pImpl->pending_producer_name;
    auto dbp = create_datablock_producer_impl(
        pImpl->chan_name, pImpl->pending_policy, config,
        pImpl->pending_fz_schema_info,
        pImpl->pending_slot_schema_info);
    if (!dbp)
    {
        LOGGER_ERROR("[ShmQueue] start: DataBlock creation failed for '{}'",
                     pImpl->chan_name);
        return false;
    }
    pImpl->dbp = std::move(dbp);
    return true;
}

// ============================================================================
// stop — HEP-CORE-0036 §6.7 Active → terminal teardown
// ============================================================================

void ShmQueue::stop()
{
    if (!pImpl) return;
    // Release outstanding handles before tearing down the DataBlock.
    if (pImpl->read_handle && pImpl->dbc.get())
    {
        (void)pImpl->dbc.get()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
    }
    if (pImpl->write_handle && pImpl->dbp.get())
    {
        (void)pImpl->dbp.get()->release_write_slot(*pImpl->write_handle);
        pImpl->write_handle.reset();
    }
    pImpl->dbc.reset();
    pImpl->dbp.reset();
    // Per §6.7 "stop() is terminal": clear pending secret + capability
    // fd so we don't silently restart with stale artifacts.  The fd
    // itself is non-owning (L1 transport owns it), so we only forget
    // our reference; we do not close().
    pImpl->has_shm_secret        = false;
    pImpl->pending_shm_secret    = 0;
    pImpl->has_capability_fd     = false;
    pImpl->pending_capability_fd = -1;
}

// ============================================================================
// apply_master_approval — HEP-CORE-0036 §6.7 polymorphic dispatch
// ============================================================================

bool ShmQueue::apply_master_approval(const nlohmann::json& artifacts) noexcept
{
    if (!pImpl) return false;
    // Missing `shm_secret` is a no-op-returning-true: the role host may
    // call this unconditionally and the legacy config-supplied secret
    // path stays valid (queue is already Configured or Active).
    if (!artifacts.contains("shm_secret"))
        return true;
    try
    {
        if (!artifacts.at("shm_secret").is_number_unsigned())
        {
            LOGGER_WARN("[ShmQueue::apply_master_approval] `shm_secret` "
                        "is not an unsigned number — refusing per HEP-CORE-0036 §6.7");
            return false;
        }
        const uint64_t secret = artifacts.at("shm_secret").get<uint64_t>();
        return set_shm_secret(secret);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[ShmQueue::apply_master_approval] exception "
                     "parsing artifacts: {}", e.what());
        return false;
    }
}

// ============================================================================
// raw_producer / raw_consumer — internal accessors for template RAII path
// ============================================================================

DataBlockProducer *ShmQueue::raw_producer() noexcept
{
    return pImpl ? pImpl->dbp.get() : nullptr;
}

DataBlockConsumer *ShmQueue::raw_consumer() noexcept
{
    return pImpl ? pImpl->dbc.get() : nullptr;
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
    if (pImpl && pImpl->dbp.get() && pImpl->fz_sz > 0)
        (void)pImpl->dbp.get()->update_checksum_flexible_zone();
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
    // HEP-CORE-0036 §6.7: returns true iff the queue is Active (a
    // DataBlock is attached).  Standby and Configured both return
    // false (dbc/dbp still null until start() runs).  Moved-from
    // instances (null pImpl) likewise return false.
    return pImpl && (pImpl->dbc.get() != nullptr || pImpl->dbp.get() != nullptr);
}

ShmQueue::ShmQueue(std::unique_ptr<ShmQueueImpl> impl) : pImpl(std::move(impl)) {}

ShmQueue::~ShmQueue()
{
    // Release any outstanding handles before the DataBlock objects are destroyed.
    if (pImpl)
    {
        if (pImpl->read_handle && pImpl->dbc.get())
        {
            (void)pImpl->dbc.get()->release_consume_slot(*pImpl->read_handle);
        }
        if (pImpl->write_handle && pImpl->dbp.get())
        {
            // Abort without committing — do not expose partial data.
            (void)pImpl->dbp.get()->release_write_slot(*pImpl->write_handle);
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
    if (!pImpl || !pImpl->dbc.get())
        return nullptr;

    pImpl->read_handle = pImpl->dbc.get()->acquire_consume_slot(
        static_cast<int>(timeout.count()));

    if (!pImpl->read_handle)
        return nullptr;

    // Store monotonic slot id for last_seq().
    pImpl->last_seq = pImpl->read_handle->slot_id();

    // Optional checksum verification (pre-read gate, honors slot_cks_is_valid).
    if (pImpl->verify_slot && !pImpl->read_handle->verify_checksum_slot())
    {
        pImpl->dbc.get()->mutable_metrics().inc_checksum_error();
        LOGGER_ERROR("[ShmQueue] slot checksum error on slot {} channel '{}'",
                     pImpl->last_seq, pImpl->chan_name);
        (void)pImpl->dbc.get()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
        return nullptr;
    }
    if (pImpl->verify_fz && !pImpl->read_handle->verify_checksum_flexible_zone())
    {
        pImpl->dbc.get()->mutable_metrics().inc_checksum_error();
        LOGGER_ERROR("[ShmQueue] flexzone checksum error on slot {} channel '{}'",
                     pImpl->last_seq, pImpl->chan_name);
        (void)pImpl->dbc.get()->release_consume_slot(*pImpl->read_handle);
        pImpl->read_handle.reset();
        return nullptr;
    }

    std::span<const std::byte> buf = pImpl->read_handle->buffer_span();
    return buf.empty() ? nullptr : buf.data();
}

void ShmQueue::read_release() noexcept
{
    if (!pImpl || !pImpl->dbc.get() || !pImpl->read_handle)
        return;
    (void)pImpl->dbc.get()->release_consume_slot(*pImpl->read_handle);
    pImpl->read_handle.reset();
}

uint32_t ShmQueue::spinlock_count() const noexcept
{
    // Spinlocks live in the DataBlock header; reachable via either side handle.
    if (pImpl && pImpl->dbp) return pImpl->dbp->spinlock_count();
    if (pImpl && pImpl->dbc) return pImpl->dbc->spinlock_count();
    return 0;
}

SharedSpinLock ShmQueue::get_spinlock(size_t index)
{
    if (pImpl && pImpl->dbp) return pImpl->dbp->get_spinlock(index);
    if (pImpl && pImpl->dbc) return pImpl->dbc->get_spinlock(index);
    throw std::runtime_error("ShmQueue::get_spinlock: no DataBlock backing");
}

void* ShmQueue::flexzone() noexcept
{
    // Single authoritative guard: fz_sz is the flexzone size stored at
    // construction. Zero → no flexzone configured on this channel (this is
    // also the state for ZmqQueue, which inherits the default nullptr/0
    // implementation from the base class).
    //
    // Per HEP-CORE-0002 §2.2 the flexzone is a single shared region per
    // channel, fully read+write on every endpoint. The underlying DataBlock
    // RAII handle (dbp for writer-mode, dbc for reader-mode) yields the same
    // physical bytes; we cast away the reader-side const because the
    // HEP-0002 design guarantees writability on both ends.
    if (!pImpl || pImpl->fz_sz == 0)
        return nullptr;
    if (pImpl->dbp)
    {
        std::span<std::byte> s = pImpl->dbp->flexible_zone_span();
        return s.empty() ? nullptr : s.data();
    }
    if (pImpl->dbc)
    {
        std::span<const std::byte> s = pImpl->dbc->flexible_zone_span();
        return s.empty() ? nullptr : const_cast<std::byte *>(s.data());
    }
    return nullptr;
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
    if (!pImpl || !pImpl->dbp.get())
        return nullptr;

    pImpl->write_handle = pImpl->dbp.get()->acquire_write_slot(
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
    if (!pImpl || !pImpl->dbp.get() || !pImpl->write_handle)
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
    (void)pImpl->dbp.get()->release_write_slot(*pImpl->write_handle);
    pImpl->write_handle.reset();
}

void ShmQueue::write_discard() noexcept
{
    if (!pImpl || !pImpl->dbp.get() || !pImpl->write_handle)
        return;
    // Release without commit — slot is discarded and returned to the ring.
    (void)pImpl->dbp.get()->release_write_slot(*pImpl->write_handle);
    pImpl->write_handle.reset();
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
    if (pImpl->dbc.get())
        return pImpl->dbc.get()->name();
    if (pImpl->dbp.get())
        return pImpl->dbp.get()->name();
    return "(unnamed)";
}

size_t ShmQueue::capacity() const
{
    if (!pImpl)
        throw std::runtime_error("ShmQueue::capacity(): not connected");
    DataBlockMetrics m{};
    if (const DataBlockConsumer* c = pImpl->dbc.get())
    {
        if (c->get_metrics(m) == 0)
            return static_cast<size_t>(m.slot_count);
    }
    else if (const DataBlockProducer* p = pImpl->dbp.get())
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
    if (pImpl->dbc.get())
        return "shm_read";
    if (pImpl->dbp.get())
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

    if (const DataBlockConsumer* c = pImpl->dbc.get())
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

    if (const DataBlockProducer* p = pImpl->dbp.get())
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
    if (DataBlockConsumer* c = pImpl->dbc.get())
        c->clear_metrics();
    if (DataBlockProducer* p = pImpl->dbp.get())
        p->clear_metrics();
}

void ShmQueue::set_configured_period(uint64_t period_us)
{
    if (!pImpl) return;
    // Write directly to ContextMetrics via DataBlock mutable_metrics() accessor.
    if (DataBlockConsumer* c = pImpl->dbc.get())
        c->mutable_metrics().set_configured_period(period_us);
    else if (DataBlockProducer* p = pImpl->dbp.get())
        p->mutable_metrics().set_configured_period(period_us);
}

} // namespace pylabhub::hub
