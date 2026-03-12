#pragma once
/**
 * @file hub_queue.hpp
 * @brief Abstract QueueReader and QueueWriter interfaces for the hub pipeline.
 *
 * Defines two transport-agnostic contracts:
 *   - QueueReader  — read (input) side: read_acquire / read_release / read_flexzone
 *   - QueueWriter  — write (output) side: write_acquire / write_commit / write_discard / write_flexzone
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pylabhub::hub
{

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
 * @struct QueueMetrics
 * @brief Unified transport-agnostic diagnostic counters for QueueReader/QueueWriter implementations.
 *
 * All counters are read atomically per-field.  Neighbouring fields may reflect
 * slightly different instants if a concurrent write is in progress — this is
 * acceptable for diagnostics.  For a fully consistent snapshot, stop() the queue
 * first so all background threads have quiesced.
 *
 * ZMQ-specific counters (recv_frame_error_count, recv_gap_count, send_retry_count)
 * are always 0 for ShmQueue.  ShmQueue::overrun_count reflects loop scheduling
 * overruns from DataBlock ContextMetrics.
 */
struct QueueMetrics
{
    // ── Receive side (read_acquire path) ─────────────────────────────────────
    /// Items dropped because the receive ring buffer was full (oldest discarded).
    /// ZmqQueue: internal recv ring overflow.
    /// ShmQueue: DataBlockConsumer scheduling overruns.
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

    /// write_acquire() returned nullptr because the send buffer was full.
    /// ZmqQueue (Drop policy): incremented on each overflowing write_acquire().
    /// ZmqQueue (Block policy): incremented on timeout.
    /// ShmQueue: DataBlockProducer scheduling overruns (period_ms exceeded).
    uint64_t overrun_count{0};
};

/**
 * @class QueueReader
 * @brief Transport-agnostic read-side contract for the hub pipeline.
 *
 * Provides blocking slot acquisition with optional checksum verification,
 * last-sequence tracking, and ring buffer status queries.
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

    /**
     * @brief Read-only pointer to the shared flexzone.
     *
     * Returns nullptr if no flexzone is configured or if ZMQ-backed.
     * Valid for the entire lifetime of the QueueReader (not acquire-scoped).
     */
    virtual const void* read_flexzone() const noexcept { return nullptr; }

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

    // ── Checksum verification ─────────────────────────────────────────────────

    /**
     * @brief Configure BLAKE2b verification on read_acquire().
     *
     * ShmQueue: verifies slot and/or flexzone checksum after acquiring; returns nullptr
     * on mismatch (logs LOGGER_ERROR with channel name and slot_id).
     * ZmqQueue: no-op (TCP ensures transport integrity; different threat model).
     * Call once at initialization before the first read_acquire().
     */
    virtual void set_verify_checksum(bool /*slot*/, bool /*fz*/) noexcept {}

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
    /** @brief Size of the flexzone in bytes; 0 if not configured. */
    virtual size_t      flexzone_size() const noexcept { return 0; }
    /** @brief Human-readable channel or endpoint name (for diagnostics). */
    virtual std::string name()          const = 0;
    /** @brief Diagnostic counter snapshot. Thread-safe. */
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Default implementations are no-ops (suitable for ShmQueue).
    // ZmqQueue overrides start()/stop() to manage its recv_thread_.

    /**
     * @brief Start the reader (bind/connect socket, start background threads).
     * @return true on success; false if already running or an error occurred.
     */
    virtual bool start() { return true; }

    /** @brief Stop the reader (join background threads, close sockets). */
    virtual void stop()  {}

    /** @brief Returns true if running (after start(), before stop()). */
    virtual bool is_running() const noexcept { return true; }
};

/**
 * @class QueueWriter
 * @brief Transport-agnostic write-side contract for the hub pipeline.
 *
 * Provides slot acquisition with checksum update, overflow policy, and ring buffer status.
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

    /**
     * @brief Writable pointer to the shared flexzone.
     *
     * Returns nullptr if no flexzone is configured or ZMQ-backed.
     * Valid for the entire lifetime of the QueueWriter (not acquire-scoped).
     */
    virtual void* write_flexzone() noexcept { return nullptr; }

    // ── Checksum ──────────────────────────────────────────────────────────────

    /**
     * @brief Configure BLAKE2b checksum update on write_commit().
     *
     * ShmQueue: updates slot and/or flexzone checksum automatically on each commit.
     * ZmqQueue: no-op (TCP provides transport integrity).
     * Call once at initialization before the first write_acquire().
     */
    virtual void set_checksum_options(bool /*slot*/, bool /*fz*/) noexcept {}

    /**
     * @brief Initialize the flexzone checksum after on_start() populates its contents.
     *
     * ShmQueue: calls DataBlockProducer::update_checksum_flexible_zone() directly on the
     *           shared memory segment (outside the normal write_acquire/write_commit cycle).
     *           Ensures readers can verify the initial flexzone state before the first
     *           write_commit() re-stamps it.
     * ZmqQueue: no-op (TCP integrity; no flexzone concept).
     * Call once after on_init() has written the initial flexzone content.
     */
    virtual void sync_flexzone_checksum() noexcept {}

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
    /** @brief Size of the flexzone in bytes; 0 if not configured. */
    virtual size_t      flexzone_size() const noexcept { return 0; }
    /** @brief Human-readable channel or endpoint name (for diagnostics). */
    virtual std::string name()          const = 0;
    /** @brief Diagnostic counter snapshot. Thread-safe. */
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Default implementations are no-ops (suitable for ShmQueue).
    // ZmqQueue overrides start()/stop() to manage its send_thread_.

    /**
     * @brief Start the writer (bind/connect socket, start background threads).
     * @return true on success; false if already running or an error occurred.
     */
    virtual bool start() { return true; }

    /** @brief Stop the writer (join background threads, close sockets). */
    virtual void stop()  {}

    /** @brief Returns true if running (after start(), before stop()). */
    virtual bool is_running() const noexcept { return true; }
};

} // namespace pylabhub::hub
