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
#include <stdexcept>
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
    uint64_t configured_period_us{0}; ///< Target period (µs). 0 = MaxRate. Config input, not measured.

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
};

} // namespace pylabhub::hub
