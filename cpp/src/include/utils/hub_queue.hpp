#pragma once
/**
 * @file hub_queue.hpp
 * @brief Abstract Queue interface for the hub::Processor pipeline.
 *
 * Defines a single transport-agnostic data-flow contract for both reading
 * and writing. No ZMQ protocol, no HELLO/BYE, no broker registration.
 *
 * Concrete implementations:
 *   - ShmQueue  — wraps DataBlockConsumer (read) or DataBlockProducer (write)
 *   - ZmqQueue  — wraps a raw ZMQ PULL (read) or PUSH (write) socket
 *
 * See docs/HEP/HEP-CORE-0015-Processor-Binary.md for design rationale.
 */
#include "pylabhub_utils_export.h"

#include <chrono>
#include <cstddef>
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
 * @class Queue
 * @brief Transport-agnostic data-flow contract for hub::Processor.
 *
 * A Queue provides two orthogonal APIs:
 *   - Reading  (input side):  read_acquire / read_release / read_flexzone
 *   - Writing  (output side): write_acquire / write_commit / write_abort / write_flexzone
 *
 * @par Usage contract
 * - Only ONE outstanding acquire per side at a time (no nested acquires).
 * - Always call read_release() after read_acquire() — even on error paths.
 * - Always call write_commit() OR write_abort() after write_acquire().
 * - Lifecycle: call start() before first acquire; call stop() before destruction.
 *
 * @par Thread safety
 * Queue implementations are NOT thread-safe; each Queue instance should be
 * used from exactly one thread (Processor's process_thread_).
 */
class PYLABHUB_UTILS_EXPORT Queue
{
public:
    virtual ~Queue() = default;

    // ── Reading (input side) ──────────────────────────────────────────────────

    /**
     * @brief Block until data is available, then return a read-only pointer to it.
     *
     * Blocks for at most @p timeout.  Returns nullptr on timeout or when stopped.
     * The returned pointer is valid until the next read_release() call.
     *
     * @param timeout   Maximum time to wait for data.
     * @return          Pointer to slot data, or nullptr on timeout/stop.
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
     * Returns nullptr if no flexzone is configured or if this is a ZMQ-backed Queue.
     * Valid for the entire lifetime of the Queue (not acquire-scoped).
     */
    virtual const void* read_flexzone() const noexcept { return nullptr; }

    // ── Writing (output side) ─────────────────────────────────────────────────

    /**
     * @brief Acquire a writable output buffer.
     *
     * Returns a writable pointer that the caller may fill with output data.
     * The pointer is valid until write_commit() or write_abort() is called.
     *
     * @param timeout   For Block policy: max wait for a free slot.
     *                  For Drop policy: pass 0ms to get immediate result.
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
    virtual void  write_abort() noexcept = 0;

    /**
     * @brief Writable pointer to the shared flexzone.
     *
     * Returns nullptr if no flexzone is configured or ZMQ-backed.
     * Valid for the entire lifetime of the Queue (not acquire-scoped).
     */
    virtual void* write_flexzone() noexcept { return nullptr; }

    // ── Schema metadata ───────────────────────────────────────────────────────

    /** @brief Size of one data item in bytes (set at construction). */
    virtual size_t      item_size()     const noexcept = 0;

    /** @brief Size of the flexzone in bytes; 0 if not configured. */
    virtual size_t      flexzone_size() const noexcept { return 0; }

    /** @brief Human-readable channel or endpoint name (for diagnostics). */
    virtual std::string name()          const = 0;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Default implementations are no-ops (suitable for ShmQueue).
    // ZmqQueue overrides start()/stop() to manage its recv_thread_.

    /**
     * @brief Start the queue (bind/connect socket, start background threads).
     * @return true on success; false if already running or an error occurred.
     */
    virtual bool start() { return true; }

    /** @brief Stop the queue (join background threads, close sockets). */
    virtual void stop()  {}

    /** @brief Returns true if the queue is running (after start(), before stop()). */
    virtual bool is_running() const noexcept { return true; }
};

} // namespace pylabhub::hub
