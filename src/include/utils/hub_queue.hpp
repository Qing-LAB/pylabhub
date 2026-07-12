#pragma once
/**
 * @file hub_queue.hpp
 * @brief Abstract QueueReader and QueueWriter interfaces for the hub pipeline.
 *
 * Defines two transport-agnostic contracts:
 *   - QueueReader  — read (input) side: read_acquire / read_release / flexzone
 *   - QueueWriter  — write (output) side: write_acquire / write_commit / write_discard / flexzone
 *
 * No ZMQ protocol, no HELLO/BYE, no broker registration.
 *
 * Concrete implementations:
 *   - ShmQueue  — inherits both QueueReader and QueueWriter; wraps DataBlockConsumer (read)
 *                 or DataBlockProducer (write).  Factories return the appropriate side.
 *   - ZmqQueue  — inherits both QueueReader and QueueWriter; wraps ZMQ PULL (read) or PUSH (write).
 *
 * See docs/HEP/HEP-CORE-0015-Processor-Binary.md for design rationale.
 */
#include "pylabhub_utils_export.h"
#include "utils/data_block_policy.hpp"
#include "utils/json_fwd.hpp"                 // nlohmann::json fwd-decl
#include "utils/shared_memory_spinlock.hpp"   // SharedSpinLock (spinlock accessors on base)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace pylabhub::hub
{

/**
 * @brief HEP-CORE-0036 §I9.1 + §6.6.3 — dial-side peer-readiness probe.
 *
 * Abstract interface a `QueueWriter` uses to ask "can I connect now?"
 * without knowing about the broker RPC, the CURVE identity of its
 * peer, or the wire shape of `CHECK_PEER_READY_REQ`.  Injected into
 * `QueueWriter::finalize_connect` at role-host startup; the concrete
 * implementation lives on the role side and forwards to
 * `BrokerRequestComm::check_peer_ready` with pre-bound context
 * (channel, own role_uid, own pubkey).
 *
 * Keeping the interface arg-free lets the queue drive the poll loop
 * without leaking topology or transport knowledge upward — the queue
 * merely re-polls until it sees `Ready` or an error, and the oracle
 * hides everything else.  Matches §I9.1 (topology and transport are
 * queue-internal): the oracle is a "small injected interface" the
 * queue is permitted to hold; role-side code populates it once at
 * finalize-connect time.
 */
class PeerReadinessOracle
{
public:
    enum class PollResult
    {
        /// Peer's binding-side ZAP allowlist is confirmed to contain
        /// this queue's pubkey; the queue's next `socket.connect()`
        /// will succeed the CURVE handshake.
        Ready,
        /// Peer's binding side has not yet confirmed the current
        /// snapshot (or transport hiccup).  Queue should keep polling
        /// until deadline.
        NotReady,
        /// Broker returned a permanent error (unresolvable BRC,
        /// `CHANNEL_NOT_FOUND`, `NOT_A_ROLE_OF_CHANNEL`).  Queue
        /// abandons the wait and returns failure from `finalize_connect`.
        PermanentError,
    };

    virtual ~PeerReadinessOracle() = default;

    /// Poll the readiness source once.  Non-blocking / bounded by
    /// the oracle's own internal RPC timeout; the queue paces the
    /// poll rate itself via `kBrokerReadinessPollInterval`.
    virtual PollResult poll() noexcept = 0;
};

/**
 * @brief Overflow policy for the Processor output side.
 *
 * Block — wait up to the output timeout for a free slot/buffer.
 * Drop  — return nullptr immediately if no slot/buffer is available.
 */
enum class OverflowPolicy
{
    Block, ///< Back-pressure: block until output becomes available
    Drop,  ///< Best-effort: drop input slot when output is full
};

/**
 * @brief Parse a JSON string value into an OverflowPolicy enum.
 *
 * Shared by all three role config parsers (producer, processor, consumer) to
 * avoid duplicating the same string → enum logic in each .cpp file.
 *
 * @param s        JSON string value ("drop" or "block").
 * @param context  Caller context for error messages (e.g. "Producer config").
 * @return Parsed OverflowPolicy.
 * @throws std::runtime_error on unknown value.
 */
inline OverflowPolicy parse_overflow_policy(const std::string &s, const char *context = "config")
{
    if (s == "drop")  return OverflowPolicy::Drop;
    if (s == "block") return OverflowPolicy::Block;
    throw std::runtime_error(
        std::string(context) + ": invalid overflow policy = '" + s +
        "' (expected 'drop' or 'block')");
}

/**
 * @struct QueueMetrics
 * @brief Unified transport-agnostic diagnostic counters for QueueReader/QueueWriter implementations.
 *
 * All counters are read atomically per-field.  Neighbouring fields may reflect
 * slightly different instants if a concurrent write is in progress — this is
 * acceptable for diagnostics.  For a fully consistent snapshot, stop() the queue
 * first so all background threads have quiesced.
 *
 * ## Timing fields (Domain 2+3, HEP-CORE-0008 §10)
 *
 * Both ShmQueue and ZmqQueue track the same timing metrics:
 * - ShmQueue: delegates to DataBlock ContextMetrics (measured in acquire/release)
 * - ZmqQueue: measures directly in recv/send thread using steady_clock
 *
 * ## Transport-specific counters
 *
 * ZMQ-specific: recv_frame_error_count, recv_gap_count, send_drop_count, send_retry_count
 * (always 0 for ShmQueue).
 */
struct QueueMetrics
{
    // ── Domain 2: Acquire/release timing (both transports) ───────────────────
    uint64_t last_slot_wait_us{0};    ///< Time blocked inside acquire (µs).
    uint64_t last_iteration_us{0};    ///< Start-to-start time between consecutive acquires (µs).
    uint64_t max_iteration_us{0};     ///< Peak start-to-start time since reset (µs).
    uint64_t context_elapsed_us{0};   ///< Elapsed since first acquire (µs). Updated per acquire.

    // ── Domain 3: Data flow (both transports) ──────────────────────────
    uint64_t last_slot_exec_us{0};    ///< Time from acquire to release (µs).
    uint64_t data_drop_count{0};        ///< Data lost: SHM Latest_only overwrite, ZMQ write buffer full/timeout. 0 for readers.

    // ── Receive side (read_acquire path) ─────────────────────────────────────
    /// Items permanently lost because the receive ring buffer was full (oldest discarded).
    /// ZmqQueue: recv thread overwrote oldest unread item when ring was full.
    /// ShmQueue: always 0 — DataBlock sync policies prevent data loss at the queue level.
    uint64_t recv_overflow_count{0};

    /// Frames rejected due to bad magic, schema tag mismatch, or field type/size error.
    /// ZmqQueue only; always 0 for ShmQueue.
    uint64_t recv_frame_error_count{0};

    /// Sequence number gaps detected (frames lost between PUSH and PULL).
    /// ZmqQueue only; always 0 for ShmQueue.
    uint64_t recv_gap_count{0};

    // ── Send side (write_commit path) ─────────────────────────────────────────
    /// Frames permanently dropped (zmq_send error during stop drain, or non-retriable error).
    /// ZmqQueue only; always 0 for ShmQueue.
    uint64_t send_drop_count{0};

    /// Transient EAGAIN retries by the send_thread_ (ZMQ HWM temporarily exceeded).
    /// ZmqQueue only; always 0 for ShmQueue.
    uint64_t send_retry_count{0};

    // ── Checksum ─────────────────────────────────────────────────────────────
    /// Slot checksum verification failures in read_acquire().
    /// ShmQueue: incremented when verify_checksum_slot returns false.
    /// ZmqQueue: always 0 (TCP provides transport integrity).
    uint64_t checksum_error_count{0};
};

/**
 * @brief Canonical field list for QueueMetrics serialization.
 *
 * Expand with an adapter macro to serialize all fields to JSON, py::dict, Lua table, etc.
 * Adding a field to QueueMetrics: (1) add the member above, (2) add one X() line here.
 * All consumers pick it up at compile time.
 *
 * @code
 * // Example — JSON adapter:
 * #define X(field) j[prefix + #field] = m.field;
 * PYLABHUB_QUEUE_METRICS_FIELDS(X)
 * #undef X
 * @endcode
 */
// NOLINTBEGIN(cppcoreguidelines-macro-usage) — X-macro is the standard pattern for struct↔serialization sync
#define PYLABHUB_QUEUE_METRICS_FIELDS(X) \
    X(last_slot_wait_us)                 \
    X(last_iteration_us)                 \
    X(max_iteration_us)                  \
    X(context_elapsed_us)                \
    X(last_slot_exec_us)                 \
    X(data_drop_count)                   \
    X(recv_overflow_count)               \
    X(recv_frame_error_count)            \
    X(recv_gap_count)                    \
    X(send_drop_count)                   \
    X(send_retry_count)                  \
    X(checksum_error_count)
// NOLINTEND(cppcoreguidelines-macro-usage)

/// Negotiated transport-level authentication mechanism of a started
/// queue.  Transport-agnostic enum: each concrete queue subclass
/// reports the value that describes its data-plane peer auth.
///
/// HEP-CORE-0041 §6.1 + HEP-CORE-0035 §2 — moved here from
/// `hub_zmq_queue.hpp` in 1i-mig-M3.5+ (task #279, 2026-06-22) so
/// `QueueReader::mechanism()` can be a virtual on the base, not a
/// `dynamic_cast<ZmqQueue *>` in `RoleAPIBase::queue_mechanism`.
/// Closes the last script-visible transport-discrimination leak at
/// the queue API surface — pre-#279 the enum had no value for SHM
/// channels (returned `Uninitialized` for fully-functional auth'd
/// SHM queues), which was misleading.
///
/// **Per-subclass post-start invariants:**
/// - `ZmqQueue::mechanism()` → `Curve` (libzmq queried for
///   `ZMQ_CURVE` at `start()` time; HEP-CORE-0035 §2 makes CURVE
///   unconditional, so `Curve` is the only acceptable post-start
///   value).
/// - `ShmQueue::mechanism()` → `ShmCapability` (HEP-CORE-0041 §6.1
///   capability transport: `memfd_create` + `SO_PEERCRED` +
///   `crypto_box` challenge-response + broker `CONSUMER_ATTACH_REQ_SHM`
///   pre-confirm + `SCM_RIGHTS` fd handoff).
/// - `InboxQueue::mechanism()` → `Uninitialized` for now (HEP-0036
///   §9.3 InboxQueue CURVE-wiring is future task #191; once that
///   ships, InboxQueue switches to `Curve`).
/// - Default base impl (any queue type that doesn't override)
///   returns `Uninitialized`.
enum class Mechanism
{
    Uninitialized,  ///< `start()` not called, queue stopped, or no auth wired.
    Curve,          ///< libzmq reported `ZMQ_CURVE` post-start.
    ShmCapability,  ///< SHM capability-transport authed (HEP-CORE-0041 §6.1).
};

/// String name for a `Mechanism` value — stable surface for script
/// bindings + telemetry sinks (HEP-CORE-0035 §2 + HEP-CORE-0041
/// §6.1 + AUTH_TODO §C5 follow-up #186 + task #279).  Used by
/// `RoleAPIBase::queue_mechanism` consumers in Lua / Python / Native
/// paths so the script side never depends on the underlying integer
/// encoding of the enum.
///
/// Strings match the Native ABI `to_string(Mechanism)` in
/// `native_engine_api.h` so a plugin's log output reads identically
/// regardless of engine (Lua / Python / Native).
[[nodiscard]] constexpr const char *mechanism_name(Mechanism m) noexcept
{
    switch (m)
    {
    case Mechanism::Curve:         return "Curve";
    case Mechanism::ShmCapability: return "ShmCapability";
    case Mechanism::Uninitialized: return "Uninitialized";
    }
    return "Uninitialized";
}

/**
 * @class QueueReader
 * @brief Transport-agnostic read-side contract for the hub pipeline.
 *
 * Provides blocking slot acquisition, last-sequence tracking, and ring
 * buffer status queries.
 *
 * @par Usage contract
 * - Only ONE outstanding acquire at a time (no nested acquires).
 * - Always call read_release() after read_acquire() — even on error paths.
 * - Lifecycle: call start() before first acquire; call stop() before destruction.
 *
 * @par Thread safety
 * NOT thread-safe; use from exactly one thread.
 */
class PYLABHUB_UTILS_EXPORT QueueReader
{
public:
    virtual ~QueueReader() = default;

    // ── Reading ───────────────────────────────────────────────────────────────

    /**
     * @brief Block until data is available, then return a read-only pointer to it.
     *
     * Blocks for at most @p timeout.  Returns nullptr on timeout, stop, or checksum error.
     * The returned pointer is valid until the next read_release() call.
     */
    virtual const void* read_acquire(std::chrono::milliseconds timeout) noexcept = 0;

    /**
     * @brief Release the slot acquired by the last read_acquire().
     *
     * No-op if the last read_acquire() returned nullptr.
     */
    virtual void read_release() noexcept = 0;

    // ── Slot sequence number ──────────────────────────────────────────────────

    /**
     * @brief Monotonic sequence number of the last slot returned by read_acquire().
     *
     * IC-04: implementations have slightly different timing guarantees:
     *   ShmQueue  — updated AFTER read_acquire() returns (from the caller thread);
     *               reflects the last slot the caller has consumed.
     *   ZmqQueue  — updated BEFORE read_acquire() returns (by recv_thread_ when the
     *               frame is decoded and enqueued); reflects the most recently decoded
     *               frame, which may be ahead of what the caller has consumed if the
     *               internal ring buffer has pending items.
     * Both return 0 until the first successful decode/read_acquire().
     */
    virtual uint64_t last_seq() const noexcept { return 0; }

    // ── Ring buffer status ────────────────────────────────────────────────────

    /**
     * @brief Ring/recv buffer slot count.
     *
     * ShmQueue: DataBlock ring buffer capacity (ring_buffer_capacity from SharedMemoryHeader).
     * ZmqQueue: max_buffer_depth configured at construction.
     */
    virtual size_t capacity() const = 0;

    /**
     * @brief Overflow policy description for diagnostics.
     *
     * ShmQueue: "shm_read".
     * ZmqQueue: "zmq_pull_ring_N" where N = max_buffer_depth.
     */
    virtual std::string policy_info() const = 0;

    // ── Metadata ──────────────────────────────────────────────────────────────

    /** @brief Size of one data item in bytes (set at construction). */
    virtual size_t      item_size()     const noexcept = 0;
    /** @brief Human-readable channel or endpoint name (for diagnostics). */
    virtual std::string name()          const = 0;
    /** @brief Diagnostic counter snapshot (timing + transport counters). Thread-safe. */
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    /** @brief Reset all metric counters. Called at role startup before the data loop. */
    virtual void reset_metrics() {}

    /** @brief Full initialization at session start. Resets counters AND transport state
     *  (e.g., sequence tracking). Call once before the data loop. Default: calls reset_metrics(). */
    virtual void init_metrics() { reset_metrics(); }

    /** @brief Set the target loop period (informational — stored in ContextMetrics). */
    virtual void set_configured_period(uint64_t /*period_us*/) {}

    // ── Checksum (unified interface — per-role policy) ───────────────────────

    /** @brief Set the checksum policy. Applied uniformly to this queue. */
    virtual void set_checksum_policy(ChecksumPolicy /*policy*/) {}
    /** @brief Enable/disable flexzone checksum. SHM-specific; ZMQ no-op. */
    virtual void set_flexzone_checksum(bool /*enabled*/) {}
    /** @brief Configure verification of slot + flexzone checksums on read_acquire().
     *  SHM-specific; ZMQ no-op. */
    virtual void set_verify_checksum(bool /*verify_slot*/, bool /*verify_fz*/) const noexcept {}
    /** @brief Verify slot checksum on current read buffer. Returns true if valid or no checksum. */
    virtual bool verify_checksum() { return true; }
    /** @brief Verify flexzone checksum. SHM-specific; ZMQ returns true. */
    virtual bool verify_flexzone_checksum() { return true; }

    // ── Flexzone access (SHM provides data; ZMQ returns nullptr/0) ───────────
    //
    // Single shared region per channel, fully read+write on every endpoint
    // (HEP-CORE-0002 §2.2: user-managed bidirectional coordination).
    // The accessor returns a mutable pointer; there is no permission
    // distinction between the writer-side and reader-side handles.

    /** @brief Flexzone pointer (mutable). nullptr if no flexzone or not SHM. */
    virtual void *flexzone() noexcept { return nullptr; }
    /** @brief Flexzone size in bytes. 0 if no flexzone or not SHM. */
    virtual size_t flexzone_size() const noexcept { return 0; }

    // ── Spinlock access (SHM-specific; ZMQ returns empty/0) ──────────────────
    //
    // HEP-CORE-0002 §2.2 TABLE 1 coordination primitives. Shared spinlocks
    // live in the DataBlock header and are reachable through every queue
    // handle attached to that DataBlock.

    /** @brief Number of shared spinlocks. 0 when no SHM backing. */
    virtual uint32_t spinlock_count() const noexcept { return 0; }

    /** @brief Get the shared spinlock at index. Throws if no SHM backing. */
    virtual SharedSpinLock get_spinlock(size_t /*index*/)
    {
        throw std::runtime_error("get_spinlock: no SHM backing on this queue");
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Default implementations are no-ops (suitable for ShmQueue).
    // ZmqQueue overrides start()/stop() to manage its recv_thread_.
    //
    // Contract: start() → use → stop(). Both are idempotent:
    //   start() on already-running queue returns true (not false).
    //   stop()  on already-stopped queue is a safe no-op.

    /**
     * @brief Start the reader (bind/connect socket, start background threads).
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    virtual bool start() { return true; }

    /** @brief Stop the reader (join background threads, close sockets). Idempotent. */
    virtual void stop()  {}

    /** @brief Returns true if running (after start(), before stop()). */
    virtual bool is_running() const noexcept { return true; }

    /**
     * @brief Apply master-approval artifacts (HEP-CORE-0036 §6.7 Standby → Configured).
     *
     * Polymorphic Standby → Configured mutator that lets the role host drive
     * both transports through the same call site.  Concrete implementations
     * extract their transport-specific fields from `artifacts` and dispatch
     * to the appropriate mutator:
     *   - `ZmqQueue` (PULL side): reads `artifacts["producers"]` (array of
     *     `{role_uid, endpoint, pubkey_z85}` objects per HEP-0036 §6.4)
     *     and calls `set_producer_peers(...)`.
     *   - `ZmqQueue` (PUSH side): reads `artifacts["allowlist"]` (per HEP-0036
     *     §I11) and calls `set_peer_allowlist(...)`.
     *   - `ShmQueue`: no artifact field — SHM consumer/producer wiring
     *     runs through the capability-fd handshake at L2
     *     (HEP-CORE-0041 §5.5), not the broker-artifact channel.
     *     `ShmQueue::apply_master_approval` is a no-op returning true.
     *
     * Default implementation returns `true` and no-ops.  HEP §6.7's
     * "either fully transitioned or fully refused" rule applies:
     * on malformed `artifacts`, return `false` and leave queue state
     * unchanged.
     */
    virtual bool apply_master_approval(const nlohmann::json& /*artifacts*/) noexcept
    {
        return true;
    }

    /**
     * @brief HEP-CORE-0036 §I9.1 + §6.5 step 6 — queue's own binding
     *        role_type identity for `CHANNEL_AUTH_APPLIED_REQ`.
     *
     * Returns `"consumer"` for a binding-side reader (fan-in
     * consumer), `"producer"` for a binding-side writer (fan-out /
     * one-to-one producer), empty string for a non-binding side
     * (this queue doesn't own admission; no APPLIED_REQ to publish).
     *
     * Retires the role-side `rx_queue->is_binding_side()` branch in
     * `handle_channel_auth_notifies`: the queue tells the role
     * which `role_type` to declare on the wire; the role never
     * pattern-matches queue shape.  Layer-clean per §I9.1.
     */
    virtual std::string_view binding_role_type() const noexcept { return {}; }

    /**
     * @brief HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036 §I9.1
     *        — does this reader have any admitted peer to receive from?
     *
     * Queue-owned admission fact.  Consumers and processors read
     * this on their rx side via the role-API forwarder to gate
     * their data loop; the answer is authoritative because the
     * queue owns the ZAP allowlist (binding side) or the known
     * peer set (dialing side).  Layer-clean per §I9.1: no caller
     * asks "is this fan-in?" — the queue's own state answers.
     *
     * Default `true` for transports without CURVE admission
     * (plaintext, SHM — attach handshake gates admission externally).
     * `ZmqQueue` overrides to check its allowlist / peer set.
     */
    virtual bool is_admission_populated() const noexcept { return true; }

    /**
     * @brief True iff this reader is backed by shared memory (vs ZMQ).
     *
     * Single transport-discriminator exposed on the abstract interface, so
     * callers holding only a QueueReader* can still emit a transport-aware
     * diagnostic / take the SHM-only code path (e.g., sync flexzone checksum)
     * without needing to know the concrete implementation class.
     *
     * Default: false (safe for any non-SHM transport). Override in ShmQueue.
     */
    virtual bool is_shm_backed() const noexcept { return false; }

    /**
     * @brief HEP-CORE-0041 1i-mig-4 (#272) — apply the SHM capability
     * fd received via SCM_RIGHTS during the §5.5 attach handshake.
     *
     * Polymorphic Standby → Configured mutator for the capability-
     * transport path (analogous to `apply_master_approval` for ZMQ
     * PULL).  The role host's `apply_consumer_reg_ack` SHM branch
     * dispatches through this so it doesn't need to downcast to
     * `ShmQueue*`.
     *
     * Default implementation returns `false` (non-SHM queues refuse
     * the call — callers should never reach this path for ZMQ).
     * `ShmQueue` overrides to drive Standby → Configured via
     * `set_shm_capability_fd(fd)`.  Returns `false` on refusal
     * (already-Active queue, non-SHM transport) so callers can
     * log + fail.
     */
    virtual bool set_shm_capability_fd(int /*fd*/) noexcept { return false; }

    /// HEP-CORE-0017 §3.3.0 — true iff this queue is the binding
    /// side for its (topology, transport) cell.  Role code consults
    /// this to decide whether to publish an endpoint via
    /// `BrokerRequestComm::send_endpoint_update` after
    /// `apply_master_approval` succeeds.  Default false (dialing
    /// side or transport where "binding" doesn't apply); concrete
    /// queues override to reflect their configured direction.
    [[nodiscard]] virtual bool is_binding_side() const noexcept { return false; }

    /// HEP-CORE-0021 §16 — resolved bind endpoint on the BINDING
    /// side, after `start()` has completed the bind.  Non-empty when
    /// this queue is the binding side (fan-in reader, fan-out /
    /// one-to-one writer per HEP-CORE-0017 §3.3.0) and the queue is
    /// Active; empty otherwise (dialing side, not started, or
    /// non-ZMQ transport).  Role code passes the return value to
    /// `BrokerRequestComm::send_endpoint_update` so the broker can
    /// advertise it to dialing peers via REG_ACK.  Default empty;
    /// `ZmqQueue` overrides with `ZMQ_LAST_ENDPOINT`.
    [[nodiscard]] virtual std::string actual_endpoint() const { return {}; }

    /// HEP-CORE-0041 §6.1 + task #279 — negotiated transport-level
    /// authentication mechanism observed at `start()` time.  See the
    /// `hub::Mechanism` enum docstring above for the per-subclass
    /// invariants.  Default returns `Uninitialized` for queue types
    /// that don't override (e.g. `InboxQueue` until #191 CURVE-wires
    /// it).  `ZmqQueue` overrides to return `Curve` after libzmq
    /// reports `ZMQ_CURVE`; `ShmQueue` overrides to return
    /// `ShmCapability` once `start()` has attached the DataBlock.
    [[nodiscard]] virtual Mechanism mechanism() const noexcept
    {
        return Mechanism::Uninitialized;
    }
};

/**
 * @class QueueWriter
 * @brief Transport-agnostic write-side contract for the hub pipeline.
 *
 * Provides slot acquisition with overflow policy and ring buffer status.
 *
 * @par Usage contract
 * - Only ONE outstanding acquire at a time (no nested acquires).
 * - Always call write_commit() OR write_discard() after write_acquire().
 * - Lifecycle: call start() before first acquire; call stop() before destruction.
 *
 * @par Thread safety
 * NOT thread-safe; use from exactly one thread.
 */
class PYLABHUB_UTILS_EXPORT QueueWriter
{
public:
    virtual ~QueueWriter() = default;

    // ── Writing ───────────────────────────────────────────────────────────────

    /**
     * @brief Acquire a writable output buffer.
     *
     * Returns a writable pointer that the caller may fill with output data.
     * The pointer is valid until write_commit() or write_discard() is called.
     *
     * @param timeout   For Block policy: max wait for a free slot.
     *                  For Drop policy: ignored (immediate).
     * @return          Writable pointer, or nullptr if no slot is available.
     */
    virtual void* write_acquire(std::chrono::milliseconds timeout) noexcept = 0;

    /**
     * @brief Commit (publish) the output buffer acquired by write_acquire().
     *
     * Makes the written data visible to downstream readers.
     * No-op if write_acquire() returned nullptr.
     */
    virtual void  write_commit() noexcept = 0;

    /**
     * @brief Discard the output buffer acquired by write_acquire().
     *
     * The slot is returned without being published.
     * No-op if write_acquire() returned nullptr.
     */
    virtual void  write_discard() noexcept = 0;

    // ── Ring buffer status ────────────────────────────────────────────────────

    /**
     * @brief Ring/send buffer slot count.
     *
     * ShmQueue: DataBlock ring buffer capacity.
     * ZmqQueue: send_buffer_depth configured at construction.
     */
    virtual size_t capacity() const = 0;

    /**
     * @brief Overflow policy description for diagnostics.
     *
     * ShmQueue: "shm_write".
     * ZmqQueue: "zmq_push_drop" or "zmq_push_block".
     */
    virtual std::string policy_info() const = 0;

    // ── Metadata ──────────────────────────────────────────────────────────────

    /** @brief Size of one data item in bytes (set at construction). */
    virtual size_t      item_size()     const noexcept = 0;
    /** @brief Human-readable channel or endpoint name (for diagnostics). */
    virtual std::string name()          const = 0;
    /** @brief Diagnostic counter snapshot (timing + transport counters). Thread-safe. */
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    /** @brief Reset all metric counters. Called at role startup before the data loop. */
    virtual void reset_metrics() {}

    /** @brief Full initialization at session start. Resets counters AND transport state
     *  (e.g., sequence tracking). Call once before the data loop. Default: calls reset_metrics(). */
    virtual void init_metrics() { reset_metrics(); }

    /** @brief Set the target loop period (informational — stored in ContextMetrics). */
    virtual void set_configured_period(uint64_t /*period_us*/) {}

    // ── Checksum (unified interface — per-role policy) ───────────────────────

    /** @brief Set the checksum policy. Applied uniformly to this queue. */
    virtual void set_checksum_policy(ChecksumPolicy /*policy*/) {}
    /** @brief Enable/disable flexzone checksum. SHM-specific; ZMQ no-op. */
    virtual void set_flexzone_checksum(bool /*enabled*/) {}
    /** @brief Compute slot checksum on current write buffer. */
    virtual void update_checksum() {}
    /** @brief Compute flexzone checksum. SHM-specific; ZMQ no-op. */
    virtual void update_flexzone_checksum() {}

    // ── Flexzone access (SHM provides data; ZMQ returns nullptr/0) ───────────
    //
    // Single shared region per channel, fully read+write on every endpoint
    // (HEP-CORE-0002 §2.2: user-managed bidirectional coordination).

    /** @brief Flexzone pointer (mutable). nullptr if no flexzone or not SHM. */
    virtual void *flexzone() noexcept { return nullptr; }
    /** @brief Flexzone size in bytes. 0 if no flexzone or not SHM. */
    virtual size_t flexzone_size() const noexcept { return 0; }
    /** @brief Stamp flexzone checksum. Call after initial flexzone setup (e.g., on_init). */
    virtual void sync_flexzone_checksum() noexcept {}

    // ── Spinlock access (SHM-specific; ZMQ returns empty/0) ──────────────────
    //
    // HEP-CORE-0002 §2.2 TABLE 1 coordination primitives.

    virtual uint32_t spinlock_count() const noexcept { return 0; }
    virtual SharedSpinLock get_spinlock(size_t /*index*/)
    {
        throw std::runtime_error("get_spinlock: no SHM backing on this queue");
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Default implementations are no-ops (suitable for ShmQueue).
    // ZmqQueue overrides start()/stop() to manage its send_thread_.
    //
    // Contract: start() → use → stop(). Both are idempotent:
    //   start() on already-running queue returns true (not false).
    //   stop()  on already-stopped queue is a safe no-op.

    /**
     * @brief Start the writer (bind/connect socket, start background threads).
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    virtual bool start() { return true; }

    /** @brief Stop the writer (join background threads, close sockets). Idempotent. */
    virtual void stop()  {}

    /** @brief Returns true if running (after start(), before stop()). */
    virtual bool is_running() const noexcept { return true; }

    /**
     * @brief Apply master-approval artifacts (HEP-CORE-0036 §6.7 Standby → Configured).
     *
     * See `QueueReader::apply_master_approval` for the polymorphic contract
     * and per-transport dispatch.  The write-side variant uses `allowlist`
     * for ZMQ PUSH; `ShmQueue` writers wire via the capability-fd
     * handshake at L2 (HEP-CORE-0041 §5.5), so `apply_master_approval`
     * is a no-op returning true on the SHM tx side.
     */
    virtual bool apply_master_approval(const nlohmann::json& /*artifacts*/) noexcept
    {
        return true;
    }

    /**
     * @brief HEP-CORE-0036 §I9.1 + §6.5 step 6 — queue's own binding
     *        role_type identity for `CHANNEL_AUTH_APPLIED_REQ`.
     *
     * Returns `"producer"` for a binding-side writer (fan-out /
     * one-to-one producer), `"consumer"` for a binding-side reader
     * (fan-in consumer), empty string for a non-binding side.
     * Retires the role-side `is_binding_side()` branch.  See the
     * QueueReader mirror for the full rationale.
     */
    virtual std::string_view binding_role_type() const noexcept { return {}; }

    /**
     * @brief HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036 §I9.1
     *        — does this writer have any admitted peer to write to?
     *
     * Queue-owned admission fact.  Consumers and processors read
     * this on their rx side via the role-API forwarder to gate
     * their data loop; the answer is authoritative because the
     * queue owns the ZAP allowlist (binding side) or the known
     * peer set (dialing side).  Layer-clean per §I9.1: no caller
     * asks "is this fan-in?" — the queue's own state answers.
     *
     * Default `true` for transports without CURVE admission
     * (plaintext, SHM — attach handshake gates admission externally).
     * `ZmqQueue` overrides to check its allowlist / peer set.
     */
    virtual bool is_admission_populated() const noexcept { return true; }

    /**
     * @brief HEP-CORE-0036 §6.6.3 + §I9.1 — complete any deferred
     *        connect that `apply_master_approval` left pending.
     *
     * Called by the role host UNIFORMLY for every role type and every
     * topology, once, right after `apply_master_approval`.  The queue
     * decides internally whether it has a deferred `socket.connect()`
     * to complete:
     * - Queues that did NOT defer (fan-out producer, one-to-one
     *   binding-side producer, dialing consumer, SHM) return `true`
     *   immediately.  The oracle is never touched.
     * - Queues that deferred (fan-in DIALING PUSH, ZMQ transport)
     *   poll `oracle` at `kBrokerReadinessPollInterval` until the
     *   result is `Ready` (then run the deferred `start()` and
     *   return `true`), `PermanentError` (return `false`),
     *   `is_cancelled()` returns true (return `false`), or
     *   `timeout_ms` elapses (return `false`).
     *
     * `is_cancelled` (optional; empty function disables cancellation)
     * is polled between broker RPCs and before every sleep so a
     * shutdown during startup exits within one poll interval.
     *
     * Default no-op returning `true`.  Only `ZmqQueue` overrides this.
     *
     * @param oracle       Readiness source; polled in a loop while
     *                     the queue is dial-pending.  Ignored for
     *                     non-deferred queues.
     * @param timeout_ms   Maximum time to wait; 0 = wait forever.
     * @param is_cancelled Optional shutdown-observer; return true to
     *                     abort the wait early.
     * @param log_tag      Log prefix for diagnostic messages.
     *
     * @return `true` when the queue is ready to send / the wait
     *         completed successfully; `false` on timeout /
     *         cancellation / permanent error.
     */
    virtual bool finalize_connect(PeerReadinessOracle & /*oracle*/,
                                    std::uint64_t         /*timeout_ms*/,
                                    const std::function<bool()> & /*is_cancelled*/,
                                    const char *          /*log_tag*/) noexcept
    {
        return true;
    }

    /**
     * @brief Z85-encoded CURVE identity pubkey this writer uses when
     *        it is the CURVE client.
     *
     * Returned as an owning string because the underlying keystore
     * lookup uses a name key that this queue owns.  Empty on writers
     * with no CURVE identity (plaintext transports; SHM writers use
     * capability-fd handshake, not CURVE).  Used by the role-side
     * finalize_connect glue to seed the `PeerReadinessOracle` with
     * the pubkey the broker checks against `binding_side_confirmed_allowlist`.
     */
    virtual std::string own_pubkey_z85() const noexcept { return {}; }

    /**
     * @brief True iff this writer is backed by shared memory (vs ZMQ).
     *
     * Default: false. Override in ShmQueue. See QueueReader::is_shm_backed.
     */
    virtual bool is_shm_backed() const noexcept { return false; }

    /**
     * @brief HEP-CORE-0041 §5.5 — apply the SHM capability fd
     * (writer-side symmetric mirror of QueueReader::set_shm_capability_fd).
     *
     * The producer-side role host (build_tx_queue) calls this after
     * hub::Queue::create_writer returns a SHM writer in Standby, to
     * attach the memfd its capability layer pre-allocated and drive
     * Standby → Configured.  ZMQ writers reject the call.  Default
     * returns false so a caller that reaches this on the wrong
     * transport surfaces a hard error rather than a silent no-op.
     */
    virtual bool set_shm_capability_fd(int /*fd*/) noexcept { return false; }
};

} // namespace pylabhub::hub
