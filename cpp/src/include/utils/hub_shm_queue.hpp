#pragma once
/**
 * @file hub_shm_queue.hpp
 * @brief ShmQueue — shared-memory-backed QueueReader/QueueWriter implementation.
 *
 * Wraps a DataBlockConsumer (read mode) or DataBlockProducer (write mode).
 * No ZMQ thread, no broker registration, no protocol.
 *
 * The caller is responsible for SHM setup (broker registration, DataBlock
 * creation/attachment) before constructing the ShmQueue.
 *
 * @par Thread safety
 * ShmQueue is NOT thread-safe; use from exactly one thread at a time.
 *
 * @par Lifecycle
 * start()/stop() are no-ops; the SHM objects are already attached.
 */
#include "utils/hub_queue.hpp"
#include "utils/data_block.hpp"

#include <memory>
#include <string>

namespace pylabhub::hub
{

struct ShmQueueImpl;

/**
 * @class ShmQueue
 * @brief Shared-memory QueueReader (read mode) or QueueWriter (write mode).
 *
 * Inherits both QueueReader and QueueWriter so it can be used as either side
 * of a Processor pipeline. Factories return the appropriate abstract base pointer.
 *
 * @par Read mode (from_consumer / from_consumer_ref)
 * Wraps a DataBlockConsumer. read_acquire() acquires the next committed slot.
 * read_release() releases the read lock on that slot.
 * Supports: last_seq(), set_verify_checksum(), capacity(), policy_info().
 *
 * @par Write mode (from_producer / from_producer_ref)
 * Wraps a DataBlockProducer. write_acquire() acquires a free slot.
 * write_commit() commits it; write_discard() releases without committing.
 * Supports: set_checksum_options(), capacity(), policy_info().
 */
class PYLABHUB_UTILS_EXPORT ShmQueue final : public QueueReader, public QueueWriter
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a read-mode ShmQueue from a DataBlockConsumer.
     *
     * Takes ownership of the consumer. Returns a QueueReader*.
     *
     * @param dbc           DataBlockConsumer to wrap (must not be null).
     * @param item_size     sizeof(DataT) — size of one slot in bytes.
     * @param flexzone_sz   sizeof(FlexZoneT), or 0 if no flexzone.
     * @param channel_name  Optional diagnostic name.
     */
    [[nodiscard]] static std::unique_ptr<QueueReader>
    from_consumer(std::unique_ptr<DataBlockConsumer> dbc,
                  size_t item_size, size_t flexzone_sz = 0,
                  std::string channel_name = {});

    /**
     * @brief Create a write-mode ShmQueue from a DataBlockProducer.
     *
     * Takes ownership of the producer. Returns a QueueWriter*.
     */
    [[nodiscard]] static std::unique_ptr<QueueWriter>
    from_producer(std::unique_ptr<DataBlockProducer> dbp,
                  size_t item_size, size_t flexzone_sz = 0,
                  std::string channel_name = {});

    /**
     * @brief Create a read-mode ShmQueue wrapping an existing DataBlockConsumer.
     *
     * Non-owning: the caller retains ownership of @p dbc, which must remain
     * valid for the lifetime of this ShmQueue. Returns a QueueReader*.
     */
    [[nodiscard]] static std::unique_ptr<QueueReader>
    from_consumer_ref(DataBlockConsumer& dbc, size_t item_size,
                      size_t flexzone_sz = 0, std::string channel_name = {});

    /**
     * @brief Create a write-mode ShmQueue wrapping an existing DataBlockProducer.
     *
     * Non-owning: the caller retains ownership of @p dbp. Returns a QueueWriter*.
     */
    [[nodiscard]] static std::unique_ptr<QueueWriter>
    from_producer_ref(DataBlockProducer& dbp, size_t item_size,
                      size_t flexzone_sz = 0, std::string channel_name = {});

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ShmQueue() override;
    ShmQueue(ShmQueue&&) noexcept;
    ShmQueue& operator=(ShmQueue&&) noexcept;
    ShmQueue(const ShmQueue&) = delete;
    ShmQueue& operator=(const ShmQueue&) = delete;

    // ── QueueReader interface — reading ────────────────────────────────────────

    const void* read_acquire(std::chrono::milliseconds timeout) noexcept override;
    void        read_release() noexcept override;
    const void* read_flexzone() const noexcept override;

    // ── QueueReader extras ────────────────────────────────────────────────────

    /** Monotonic slot id from the last successful read_acquire(); 0 until then. */
    uint64_t last_seq() const noexcept override;

    /**
     * @brief Configure BLAKE2b checksum verification on read_acquire().
     *
     * When @p slot is true: verify_checksum_slot() is checked after acquire;
     * read_acquire() returns nullptr on mismatch and logs an error.
     * When @p fz is true: verify_checksum_flexible_zone() is also checked.
     * Call once before the first read_acquire().
     */
    void set_verify_checksum(bool slot, bool fz) noexcept override;

    // ── QueueWriter interface — writing ────────────────────────────────────────

    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    void  write_commit() noexcept override;
    void  write_discard() noexcept override;
    void* write_flexzone() noexcept override;

    /**
     * @brief Enable BLAKE2b checksum updates on write_commit().
     *
     * When enabled, write_commit() calls update_checksum_slot() and
     * optionally update_checksum_flexible_zone() before releasing the slot.
     * Only meaningful for write-mode ShmQueues.
     */
    void set_checksum_options(bool slot, bool fz) noexcept override;

    /**
     * @brief Initialize flexzone checksum after on_start() writes the initial content.
     *
     * Calls DataBlockProducer::update_checksum_flexible_zone() directly on the segment.
     * No-op if this ShmQueue is in read mode or has no flexzone.
     */
    void sync_flexzone_checksum() noexcept override;

    // ── Shared metadata (both QueueReader and QueueWriter) ────────────────────

    size_t      item_size()     const noexcept override;
    size_t      flexzone_size() const noexcept override;
    std::string name()          const override;

    /**
     * @brief Ring buffer slot count from DataBlock config.
     *
     * Queries DataBlockConsumer or DataBlockProducer (whichever is active)
     * via get_metrics().slot_count.
     */
    size_t      capacity()    const override;

    /**
     * @brief Returns "shm_read" (consumer mode) or "shm_write" (producer mode).
     */
    std::string policy_info() const override;

    // start()/stop()/is_running() — inherited no-op implementations from QueueReader/QueueWriter.
    // Both base classes share the same override. Only QueueReader version is needed
    // since ShmQueue declares it once and both vtables point to the same impl.

    /**
     * @brief Unified metrics snapshot (implements QueueReader::metrics() and QueueWriter::metrics()).
     *
     * Populates fields from DataBlock ContextMetrics where available:
     *   recv_overflow_count — DataBlockConsumer scheduling overruns (read mode).
     *   overrun_count       — DataBlockProducer scheduling overruns (write mode).
     * ZMQ-specific fields are always 0.
     */
    QueueMetrics metrics() const noexcept override;

private:
    explicit ShmQueue(std::unique_ptr<ShmQueueImpl> impl);
    std::unique_ptr<ShmQueueImpl> pImpl;
};

} // namespace pylabhub::hub
