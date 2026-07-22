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
 *
 * Reader side (post-HEP-CORE-0041 §5):
 * Reader side: `create_reader_standby(...)` followed by
 * `set_shm_capability_fd(fd)` (fd received via `SCM_RIGHTS` in
 * `RoleAPIBase::apply_consumer_reg_ack_shm_`, or pre-populated by
 * L2/L3 tests via the test fast-path on
 * `RxQueueOptions::shm_capability_fd`) and then `start()`.  No
 * segment name, no secret.
 *
 * Writer side: `create_writer_standby(...)` builds in Standby; the
 * tx capability factory `create_capability_writer(...)` mints the
 * memfd inside `RoleHostFrame::prepare_tx_capability_` and drives
 * the queue Configured → Active.  Both factories return `nullptr`
 * on any phase failure.
 *
 * HEP-CORE-0041 1i-cleanup S3c (#275) deleted the legacy secret-based
 * factories (`create_writer(name, ..., shared_secret, ...)` +
 * `create_reader(name, shared_secret, ...)`) + `set_shm_secret` API.
 */
#include "utils/hub_queue.hpp"
#include "utils/hub_state.hpp" // ChannelTopology (for topology-parametric factories)
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
 * @par Read mode (create_reader_standby + capability fd)
 * Creates and owns a DataBlockConsumer. read_acquire() acquires the next committed slot.
 * read_release() releases the read lock on that slot.
 * Validates expected schema against the SHM header at creation.
 *
 * @par Write mode (create_writer_standby + capability fd)
 * Creates and owns a DataBlockProducer. write_acquire() acquires a free slot.
 * write_commit() commits it; write_discard() releases without committing.
 * Computes slot/flexzone sizes from schema via compute_field_layout().
 */
class PYLABHUB_UTILS_EXPORT ShmQueue final : public QueueReader, public QueueWriter
{
  public:
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
    // L2 attach handshake).
    //
    // HEP-CORE-0041 1i-cleanup S3c (#275) deleted the legacy
    // `create_writer(name, ..., shared_secret, ...)` +
    // `create_reader(name, shared_secret, ...)` factories.

    /**
     * @brief Build a write-mode ShmQueue in Standby (no segment created).
     *
     * The returned queue is in Standby — call `set_shm_capability_fd(fd)`
     * (where fd is borrowed from `IShmCapabilityProducer::borrow_fd()`) to
     * transition Standby → Configured, then `start()` to transition
     * Configured → Active (wraps the existing memfd via
     * `create_datablock_producer_from_fd_impl`).
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue> create_writer_standby(
        const std::string &channel_name, const std::vector<SchemaFieldDesc> &slot_schema,
        const std::string &slot_packing, const std::vector<SchemaFieldDesc> &fz_schema,
        const std::string &fz_packing, uint32_t ring_buffer_capacity, DataBlockPageSize page_size,
        DataBlockPolicy policy, ConsumerSyncPolicy sync_policy, ChecksumPolicy checksum_policy,
        bool checksum_slot = false, bool checksum_fz = false, bool always_clear_slot = true,
        const std::string &hub_uid = {}, const std::string &hub_name = {},
        const schema::SchemaInfo *slot_schema_info = nullptr,
        const schema::SchemaInfo *fz_schema_info = nullptr, const std::string &producer_uid = {},
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
    [[nodiscard]] static std::unique_ptr<ShmQueue> create_reader_standby(
        const std::string &shm_name, const std::vector<SchemaFieldDesc> &expected_slot_schema,
        const std::string &expected_packing, const std::string &channel_name,
        bool verify_slot = false, bool verify_fz = false, const std::string &consumer_uid = {},
        const std::string &consumer_name = {});

    // ─── Topology-parametric factories (HEP-CORE-0017 §3.3.0, Phase C step 3) ──
    //
    // Symmetric with `ZmqQueue::create_reader` / `create_writer` (Phase C
    // step 1-2).  SHM supports fan-out and one-to-one topologies; fan-in
    // is illegal per HEP-CORE-0017 §3.3.0 gate 1 (SHM is host-local +
    // single-producer by physical constraint of a shared DataBlock).
    //
    // These factories are the SHM half of the unified `hub::Queue::create_*`
    // factory (added in Phase C step 4 per HEP-CORE-0017 §3.3.0.1);
    // internally each dispatches to the existing `create_*_standby`
    // machinery — no new SHM state, no new memfd/AttachProtocol code.
    //
    // Legacy `create_*_standby` factories remain live during transition;
    // role code migrates to these topology-parametric factories in Phase C
    // step 6.

    /// Options passed to `create_reader` — the consumer-side SHM queue.
    struct RxCreateOptions
    {
        std::string channel_name; ///< Also used as diagnostic shm_name label.
        std::vector<SchemaFieldDesc> slot_schema;
        std::string slot_packing;
        std::string consumer_uid;
        std::string consumer_name;
        bool verify_slot = false;
        bool verify_fz = false;
    };

    /// Options passed to `create_writer` — the producer-side SHM queue.
    struct TxCreateOptions
    {
        std::string channel_name;
        std::vector<SchemaFieldDesc> slot_schema;
        std::string slot_packing;
        std::vector<SchemaFieldDesc> fz_schema; ///< Empty → no flexzone.
        std::string fz_packing;
        uint32_t ring_buffer_capacity{0};
        DataBlockPageSize page_size{DataBlockPageSize::Unset};
        DataBlockPolicy policy{DataBlockPolicy::RingBuffer};
        ConsumerSyncPolicy sync_policy{ConsumerSyncPolicy::Sequential};
        ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
        bool checksum_slot = false;
        bool checksum_fz = false;
        bool always_clear_slot = true;
        std::string hub_uid;
        std::string hub_name;
        const schema::SchemaInfo *slot_schema_info = nullptr;
        const schema::SchemaInfo *fz_schema_info = nullptr;
        std::string producer_uid;
        std::string producer_name;
    };

    /// Construct the consumer-side SHM queue for `topology`.
    /// Dispatch per HEP-CORE-0017 §3.3.0:
    /// - `FanIn`: returns `nullptr` + `LOGGER_ERROR` — fan-in requires
    ///   ZMQ (SHM is host-local + single-producer by physical
    ///   constraint of the shared DataBlock).  Belt-and-braces: the
    ///   outer `hub::Queue::create_reader` gate 1 also refuses.
    /// - `OneToOne` / `FanOut`: wraps `create_reader_standby`.  Both
    ///   topologies produce the same SHM-side construction (consumer
    ///   attaches to the DataBlock via AttachProtocol at S3); the
    ///   topology parameter is currently gate-only, kept for symmetry
    ///   with `ZmqQueue::create_reader` and for future divergence if
    ///   fan-out SHM needs distinct handshake semantics.
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_reader(pylabhub::hub::ChannelTopology topology, RxCreateOptions opts);

    /// Construct the producer-side SHM queue for `topology`.
    /// - `FanIn`: returns `nullptr` + `LOGGER_ERROR` per §3.3.0 gate 1.
    /// - `OneToOne` / `FanOut`: wraps `create_writer_standby`.  Both
    ///   produce a single DataBlock owned by the producer; the accept
    ///   loop at L2 (`RoleHostFrame::spawn_shm_auth_listener_`) admits
    ///   N consumers under fan-out via successive SCM_RIGHTS handoffs
    ///   of the same anon memfd — the queue construction is unchanged.
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    create_writer(pylabhub::hub::ChannelTopology topology, TxCreateOptions opts);

    // ── Raw DataBlock accessor (for template RAII path only) ─────────────────

    /** @brief Internal accessor for C++ RAII template path. NOT for role hosts. */
    [[nodiscard]] DataBlockProducer *raw_producer() noexcept;
    [[nodiscard]] DataBlockConsumer *raw_consumer() noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ShmQueue() override;
    ShmQueue(ShmQueue &&) noexcept;
    ShmQueue &operator=(ShmQueue &&) noexcept;
    ShmQueue(const ShmQueue &) = delete;
    ShmQueue &operator=(const ShmQueue &) = delete;

    // ── QueueReader interface — reading ────────────────────────────────────────

    const void *read_acquire(std::chrono::milliseconds timeout) noexcept override;
    void read_release() noexcept override;

    /** Monotonic slot id from the last successful read_acquire(); 0 until then. */
    uint64_t last_seq() const noexcept override;

    // ── QueueWriter interface — writing ────────────────────────────────────────

    void *write_acquire(std::chrono::milliseconds timeout) noexcept override;
    void write_commit() noexcept override;
    void write_discard() noexcept override;

    // ── Shared metadata (both QueueReader and QueueWriter) ────────────────────

    size_t item_size() const noexcept override;
    std::string name() const override;

    /**
     * @brief Ring buffer slot count from DataBlock config.
     *
     * Queries DataBlockConsumer or DataBlockProducer (whichever is active)
     * via get_metrics().slot_count.
     */
    size_t capacity() const override;

    /**
     * @brief Returns "shm_read" (consumer mode) or "shm_write" (producer mode).
     */
    std::string policy_info() const override;

    // ── HEP-CORE-0036 §6.7 state machine ──────────────────────────────────
    //
    // ShmQueue follows the same Standby → Configured → Active machine as
    // ZmqQueue.  The Standby-mode factories populate the queue object
    // with schema metadata + role identity but defer the actual SHM
    // wrap.  `set_shm_capability_fd(fd)` transitions Standby → Configured.
    // `start()` does the actual `create_datablock_*_from_fd_impl` call
    // (Configured → Active).
    //
    // HEP-CORE-0041 1i-cleanup S3c (#275) deleted `set_shm_secret` +
    // the legacy convenience-factory overloads that drove the three
    // transitions in one call.

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
     * Refuses (returns false) from Active per §6.7 mutator table.
     * Safe to call from Standby or to replace a previously-set capability
     * fd in Configured (e.g. attach retry against a different producer).
     *
     * Overrides the abstract `QueueReader::set_shm_capability_fd` (the
     * non-SHM default returns `false`).  Same body used for both the
     * reader- and writer-side ShmQueue instances since the field is
     * single-stored on Impl regardless of role.
     */
    bool set_shm_capability_fd(int fd) noexcept override;

    /**
     * @brief Configured → Active.  Reader: `find_datablock_consumer_from_fd_impl`.
     * Writer: `create_datablock_producer_from_fd_impl`.  Returns true on
     * success or if already Active (idempotent).  Returns false from
     * Standby (no capability fd applied) or on attach failure (with queue
     * state preserved per HEP §6.7 "either fully transitioned or fully
     * refused").
     */
    bool start() override;

    /**
     * @brief HEP-CORE-0036 §6.7 polymorphic Standby → Configured mutator.
     *
     * No-op-returning-true for `ShmQueue` — the SHM transport wires via
     * the capability-fd handshake at L2 (HEP-CORE-0041 §5.5), not the
     * broker-artifact channel.  Role hosts call `apply_master_approval`
     * unconditionally; this override keeps that uniform-dispatch shape
     * working without driving a state transition.
     */
    bool apply_master_approval(const nlohmann::json &artifacts) noexcept override;

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

    /// HEP-CORE-0041 §6.1 + task #279 (2026-06-22): returns
    /// `Mechanism::ShmCapability` once `start()` has attached the
    /// DataBlock (i.e. `is_running() == true`); `Mechanism::Uninitialized`
    /// otherwise.  Overrides the `QueueReader::mechanism()` virtual
    /// added in #279 so `RoleAPIBase::queue_mechanism(side)` can
    /// dispatch polymorphically and return the SHM-shaped value
    /// instead of a misleading `Uninitialized` for fully-functional
    /// auth'd SHM channels.
    [[nodiscard]] Mechanism mechanism() const noexcept override;

    /**
     * @brief Unified metrics snapshot (implements QueueReader::metrics() and
     * QueueWriter::metrics()).
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
    void *flexzone() noexcept override;
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
