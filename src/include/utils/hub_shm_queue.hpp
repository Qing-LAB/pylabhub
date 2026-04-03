#pragma once
/**
 * @file hub_shm_queue.hpp
 * @brief ShmQueue — shared-memory-backed QueueReader/QueueWriter implementation.
 *
 * Wraps a DataBlockConsumer (read mode) or DataBlockProducer (write mode).
 * No ZMQ thread, no broker registration, no protocol.
 *
 * ShmQueue creates and owns its DataBlock internally. The factory receives
 * schema + SHM parameters and computes sizes via compute_field_layout().
 * Symmetric with ZmqQueue which creates and owns its ZMQ sockets.
 *
 * @par Thread safety
 * ShmQueue is NOT thread-safe; use from exactly one thread at a time.
 *
 * @par Lifecycle
 * start()/stop() are no-ops; the SHM objects are already attached.
 */
#include "utils/hub_queue.hpp"
#include "utils/data_block.hpp"
#include "utils/schema_field_layout.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ShmQueueImpl;

/**
 * @class ShmQueue
 * @brief Shared-memory QueueReader (read mode) or QueueWriter (write mode).
 *
 * Inherits both QueueReader and QueueWriter. Factories return the appropriate
 * abstract base pointer.
 *
 * @par Read mode (create_reader)
 * Creates and owns a DataBlockConsumer. read_acquire() acquires the next committed slot.
 * read_release() releases the read lock on that slot.
 * Validates expected schema against the SHM header at creation.
 *
 * @par Write mode (create_writer)
 * Creates and owns a DataBlockProducer. write_acquire() acquires a free slot.
 * write_commit() commits it; write_discard() releases without committing.
 * Computes slot/flexzone sizes from schema via compute_field_layout().
 */
class PYLABHUB_UTILS_EXPORT ShmQueue final : public QueueReader, public QueueWriter
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a write-mode ShmQueue (producer — creates SHM).
     *
     * Creates DataBlock internally from schema + SHM parameters. Computes
     * slot size and flexzone size via compute_field_layout(). Symmetric with
     * ZmqQueue::push_to().
     *
     * Returns unique_ptr<ShmQueue> (concrete type, not QueueWriter base) so
     * the caller can store as ShmQueue for direct access to SHM-specific
     * methods (raw_producer(), spinlock), or move into unique_ptr<QueueWriter>
     * via implicit upcast when only the unified queue interface is needed.
     * Use dynamic_cast for safe downcasting from QueueWriter* when needed.
     *
     * @param channel_name       SHM segment name (from broker channel).
     * @param slot_schema        Slot field definitions.
     * @param slot_packing       "aligned" or "packed".
     * @param fz_schema          Flexzone field definitions (empty = no flexzone).
     * @param fz_packing         Flexzone packing.
     * @param ring_buffer_capacity Number of slots in the ring buffer.
     * @param page_size          OS SHM page size.
     * @param shared_secret      Access token for SHM discovery.
     * @param policy             Buffer management strategy.
     * @param sync_policy        Consumer synchronization contract.
     * @param checksum_policy    Checksum enforcement level.
     * @param checksum_slot      Enable slot checksum on write_commit().
     * @param checksum_fz        Enable flexzone checksum on write_commit().
     * @param always_clear_slot  Zero-fill slot buffer on write_acquire().
     * @param hub_uid            Hub identity (stored in SHM header).
     * @param hub_name           Hub name (stored in SHM header).
     * @param slot_schema_info   Schema hash for consumer validation (stored in header).
     * @param fz_schema_info     Flexzone schema hash (stored in header).
     * @return QueueWriter, or nullptr on failure.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_writer(const std::string &channel_name,
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
                  bool checksum_slot = false,
                  bool checksum_fz = false,
                  bool always_clear_slot = true,
                  const std::string &hub_uid = {},
                  const std::string &hub_name = {},
                  const schema::SchemaInfo *slot_schema_info = nullptr,
                  const schema::SchemaInfo *fz_schema_info = nullptr,
                  const std::string &producer_uid = {},
                  const std::string &producer_name = {});

    /**
     * @brief Create a read-mode ShmQueue (consumer — attaches to existing SHM).
     *
     * Attaches to an existing DataBlock, validates schema against the SHM
     * header. Returns nullptr on attachment failure or schema mismatch.
     *
     * @param shm_name           SHM segment name (from broker discovery).
     * @param shared_secret      Access token for SHM attachment.
     * @param expected_slot_schema Expected slot field definitions (for validation).
     * @param expected_packing   Expected packing (for size validation).
     * @param channel_name       Diagnostic name.
     * @param verify_slot        Enable slot checksum verification on read_acquire().
     * @param verify_fz          Enable flexzone checksum verification on read_acquire().
     * @param consumer_uid       Consumer identity (stored in SHM header).
     * @param consumer_name      Consumer name.
     * @return QueueReader, or nullptr on failure or validation error.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_reader(const std::string &shm_name,
                  uint64_t shared_secret,
                  const std::vector<SchemaFieldDesc> &expected_slot_schema,
                  const std::string &expected_packing,
                  const std::string &channel_name,
                  bool verify_slot = false,
                  bool verify_fz = false,
                  const std::string &consumer_uid = {},
                  const std::string &consumer_name = {});

    // ── Raw DataBlock accessor (for template RAII path only) ─────────────────

    /** @brief Internal accessor for C++ RAII template path. NOT for role hosts. */
    [[nodiscard]] DataBlockProducer *raw_producer() noexcept;
    [[nodiscard]] DataBlockConsumer *raw_consumer() noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ShmQueue() override;
    ShmQueue(ShmQueue&&) noexcept;
    ShmQueue& operator=(ShmQueue&&) noexcept;
    ShmQueue(const ShmQueue&) = delete;
    ShmQueue& operator=(const ShmQueue&) = delete;

    // ── QueueReader interface — reading ────────────────────────────────────────

    const void* read_acquire(std::chrono::milliseconds timeout) noexcept override;
    void        read_release() noexcept override;

    /** Monotonic slot id from the last successful read_acquire(); 0 until then. */
    uint64_t last_seq() const noexcept override;

    // ── QueueWriter interface — writing ────────────────────────────────────────

    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    void  write_commit() noexcept override;
    void  write_discard() noexcept override;

    // ── Shared metadata (both QueueReader and QueueWriter) ────────────────────

    size_t      item_size()     const noexcept override;
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

    // start()/stop() — no-op (no background thread; queue is always operational once
    // constructed from a valid DataBlock ref).
    //
    // is_running() — overrides base to return false on a moved-from (null-pImpl) instance.
    // A freshly constructed ShmQueue is always "running" (the underlying DataBlock is live);
    // after a move, pImpl is null and is_running() correctly returns false.
    bool is_running() const noexcept override;

    /**
     * @brief Unified metrics snapshot (implements QueueReader::metrics() and QueueWriter::metrics()).
     *
     * Bridges Domain 2+3 timing fields from DataBlock ContextMetrics.
     * ZMQ-specific counters (recv_frame_error_count, recv_gap_count, etc.) are always 0.
     */
    QueueMetrics metrics() const noexcept override;

    /** @brief Reset all counters. Delegates to DataBlock clear_metrics(). */
    void reset_metrics() override;

    // ── SHM-specific operations (not on base QueueReader/QueueWriter) ─────────

    /** @brief Set target period. Writes to DataBlock ContextMetrics directly. */
    void set_configured_period(uint64_t period_us) override;

    /** @brief Read-only pointer to the shared flexzone. nullptr if no flexzone. */
    const void* read_flexzone() const noexcept override;
    /** @brief Writable pointer to the shared flexzone. nullptr if no flexzone. */
    void* write_flexzone() noexcept override;
    /** @brief Size of the flexzone in bytes; 0 if not configured. */
    size_t flexzone_size() const noexcept override;

    /** @brief Configure BLAKE2b checksum verification on read_acquire(). */
    void set_verify_checksum(bool slot, bool fz) const noexcept;
    /** @brief Enable BLAKE2b checksum updates on write_commit(). */
    void set_checksum_options(bool slot, bool fz) noexcept;

    // ── Unified checksum interface (overrides base QueueReader/QueueWriter) ──
    void set_checksum_policy(ChecksumPolicy policy) override;
    void set_flexzone_checksum(bool enabled) override;
    void update_checksum() override;
    void update_flexzone_checksum() override;
    bool verify_checksum() override;
    bool verify_flexzone_checksum() override;
    /** @brief Enable/disable zero-fill of slot buffer on write_acquire(). */
    void set_always_clear_slot(bool enable) noexcept;
    /** @brief Stamp flexzone checksum after on_init() writes initial content. */
    void sync_flexzone_checksum() noexcept override;

private:
    explicit ShmQueue(std::unique_ptr<ShmQueueImpl> impl);
    std::unique_ptr<ShmQueueImpl> pImpl;
};

} // namespace pylabhub::hub
