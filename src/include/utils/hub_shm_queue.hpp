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
 * ShmQueue implements the HEP-CORE-0036 §6.7 Standby → Configured →
 * Active state machine.  The Standby-mode factories build a queue
 * object with deferred-attach metadata (no SHM segment touched).
 * Under HEP-CORE-0036 §6.7 Option B, `set_shm_secret(secret)` BUFFERS
 * the secret in Standby; the Standby → Configured → Active transition
 * is driven by `apply_master_approval(CONSUMER_REG_ACK)`, which
 * merges any buffered set_* args with REG_ACK fields and then calls
 * `start()` to perform the actual SHM discovery (reader) or segment
 * creation (writer).  The existing `create_reader(name, secret, ...)`
 * / `create_writer(..., shared_secret, ...)` overloads are
 * convenience wrappers that drive all three transitions in one call
 * — they return `nullptr` on any phase failure, preserving the
 * legacy "factory returns nullptr on bad secret / schema mismatch /
 * missing segment" contract.
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

    // ── Standby-mode factories (HEP-CORE-0041 capability transport) ──────────
    //
    // Build a ShmQueue in Standby WITHOUT driving Configured → Active.  The
    // role host then drives the rest of the state machine via
    // `set_shm_capability_fd(fd)` (Standby → Configured) + `start()`
    // (Configured → Active).
    //
    // Used by HEP-CORE-0041 1i-mig wiring (producer + processor role hosts
    // own an `IShmCapabilityProducer` that creates the anonymous SHM;
    // consumer side receives the SHM fd over SCM_RIGHTS from the producer's
    // L2 attach handshake).  The legacy `create_writer` / `create_reader`
    // factories above will retire in 1i-cleanup.

    /**
     * @brief Build a write-mode ShmQueue in Standby (no secret, no segment).
     *
     * Identical to `create_writer` minus the `shared_secret` parameter.
     * The returned queue is in Standby — call `set_shm_capability_fd(fd)`
     * (where fd is borrowed from `IShmCapabilityProducer::borrow_fd()`) to
     * transition Standby → Configured, then `start()` to transition
     * Configured → Active (wraps the existing memfd via
     * `create_datablock_producer_from_fd_impl`).
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_writer_standby(const std::string &channel_name,
                          const std::vector<SchemaFieldDesc> &slot_schema,
                          const std::string &slot_packing,
                          const std::vector<SchemaFieldDesc> &fz_schema,
                          const std::string &fz_packing,
                          uint32_t ring_buffer_capacity,
                          DataBlockPageSize page_size,
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
     * @brief Build a read-mode ShmQueue in Standby (no secret, no attach).
     *
     * Symmetric with `create_writer_standby`.  Role host transitions
     * Standby → Configured by calling `set_shm_capability_fd(fd)` (where
     * fd is the SHM descriptor received over SCM_RIGHTS during the
     * HEP-CORE-0041 attach handshake), then `start()` for
     * Configured → Active (attaches via `find_datablock_consumer_from_fd_impl`).
     *
     * `shm_name` is purely a diagnostic label here — there is no
     * kernel-namespace name on the capability path (the SHM is anonymous).
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_reader_standby(const std::string &shm_name,
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

    // ── HEP-CORE-0036 §6.7 state machine ──────────────────────────────────
    //
    // ShmQueue follows the same Standby → Configured → Active machine as
    // ZmqQueue.  The Standby-mode factories (create_reader/create_writer
    // when called with no secret) populate the queue object with schema
    // metadata + name + role identity but defer the actual SHM
    // discovery/create.  set_shm_secret(secret) transitions
    // Standby → Configured.  start() does the actual
    // find_datablock_consumer_impl / create_datablock_producer_impl call
    // (Configured → Active).
    //
    // The existing overloads that take a secret are convenience wrappers
    // that drive Standby → Configured → Active in one call — equivalent
    // to constructing in Standby, calling set_shm_secret(secret), then
    // calling start().  Returns nullptr if any phase fails (preserves
    // legacy "create_reader returns nullptr on bad secret" semantics).

    /**
     * @brief Apply broker-supplied SHM secret (HEP-CORE-0036 §6.7
     * Standby → Configured).  Returns true on success.  Refuses
     * (returns false) from Active per §6.7 mutator table (`shm_secret`
     * is per-channel-lifetime; restart needed).  Safe to call in
     * Standby or to replace a previously-set secret in Configured.
     *
     * Refuses if `set_shm_capability_fd` has already been called on this
     * queue: a queue uses EITHER the legacy secret-based path OR the
     * HEP-CORE-0041 capability-transport path, never both (HEP-0041 D7
     * "single unified mechanism").
     */
    bool set_shm_secret(uint64_t secret) noexcept;

    /**
     * @brief Apply HEP-CORE-0041 SHM capability fd (Standby → Configured).
     *
     * The fd is BORROWED — typically obtained from
     * `IShmCapabilityProducer::borrow_fd()` on the writer side or
     * received over `SCM_RIGHTS` during the attach handshake on the
     * reader side.  The DataBlock fd-source factories dup the fd
     * internally (substep 1f), so this ShmQueue does NOT take ownership
     * — the caller (L1 transport) keeps owning the original fd.
     *
     * Refuses (returns false) from Active per §6.7 mutator table.  Refuses
     * if `set_shm_secret` has already been called (mutual exclusion).
     * Safe to call from Standby or to replace a previously-set capability
     * fd in Configured (e.g. attach retry against a different producer).
     */
    bool set_shm_capability_fd(int fd) noexcept;

    /**
     * @brief Configured → Active.  Reader: performs SHM discovery via
     * find_datablock_consumer_impl.  Writer: performs SHM segment
     * creation via create_datablock_producer_impl.  Returns true on
     * success or if already Active (idempotent).  Returns false from
     * Standby (no secret) or on attach failure (with queue state
     * preserved per HEP §6.7 "either fully transitioned or fully
     * refused").
     */
    bool start() override;

    /**
     * @brief HEP-CORE-0036 §6.7 polymorphic Standby → Configured mutator.
     * Extracts `artifacts["shm_secret"]` (uint64) and calls
     * `set_shm_secret(secret)`.  Missing field is a no-op returning true
     * — the role host can call this unconditionally regardless of what
     * the broker emitted; queue state is unchanged when no secret is
     * present (e.g. legacy config-supplied secret already in place).
     */
    bool apply_master_approval(const nlohmann::json& artifacts) noexcept override;

    /** @brief Stop — terminal teardown of any attached SHM resources. */
    void stop() override;

    /**
     * @brief is_running() — returns true iff the queue is Active (an
     * underlying DataBlock is attached).  Standby and Configured both
     * return false; pre-attach metadata methods continue to return safe
     * defaults per HEP §6.7 line 1977-1996 "nullptr is state-honest".
     */
    bool is_running() const noexcept override;

    /** @brief ShmQueue is SHM-backed (both reader and writer sides). */
    bool is_shm_backed() const noexcept override { return true; }

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

    /** @brief Pointer to the shared flexzone (mutable). nullptr if no flexzone.
     *
     *  Overrides both QueueReader::flexzone() and QueueWriter::flexzone() since
     *  ShmQueue inherits from both. The flexzone is a single shared region per
     *  channel, fully read+write on every endpoint (HEP-CORE-0002 §2.2). */
    void* flexzone() noexcept override;
    /** @brief Size of the flexzone in bytes; 0 if not configured. */
    size_t flexzone_size() const noexcept override;

    /** @brief Number of shared spinlocks in the DataBlock header (SHM-specific). */
    uint32_t spinlock_count() const noexcept override;
    /** @brief Get the shared spinlock at the given index (SHM-specific). */
    SharedSpinLock get_spinlock(size_t index) override;

    /** @brief Configure BLAKE2b checksum verification on read_acquire().
     *  Overrides QueueReader::set_verify_checksum. */
    void set_verify_checksum(bool slot, bool fz) const noexcept override;
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
