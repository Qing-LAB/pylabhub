#pragma once
/**
 * @file hub_zmq_queue.hpp
 * @brief ZmqQueue — ZMQ PULL/PUSH-backed Queue implementation.
 *
 * Wraps a raw ZMQ PULL socket (read mode) or PUSH socket (write mode).
 * No Messenger, no broker registration, no protocol — direct point-to-point ZMQ.
 *
 * @par Read mode
 * A recv_thread_ continuously receives fixed-size messages into an internal
 * bounded buffer (std::queue).  read_acquire() pops from this buffer with
 * a timeout.  flexzone is always nullptr (ZMQ transport has no flexzone).
 *
 * @par Write mode
 * write_acquire() returns a pre-allocated send buffer.  write_commit() sends
 * it via zmq_send (fire-and-forget).  write_abort() discards without sending.
 *
 * @par Thread safety
 * ZmqQueue is NOT thread-safe for its public API.  Internally, the recv_thread_
 * uses a mutex to protect the receive buffer.  Use from one caller thread only.
 *
 * @par Lifecycle
 * Call start() before first acquire; call stop() before destruction.
 * stop() joins the recv_thread_ (read mode) and closes the ZMQ context.
 */
#include "utils/hub_queue.hpp"

#include <memory>
#include <string>

namespace pylabhub::hub
{

struct ZmqQueueImpl;

/**
 * @class ZmqQueue
 * @brief ZMQ PULL (read) or PUSH (write) Queue implementation.
 */
class PYLABHUB_UTILS_EXPORT ZmqQueue final : public Queue
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a read-mode ZmqQueue using a ZMQ PULL socket.
     *
     * A background recv_thread_ receives fixed-size items into a bounded buffer.
     * read_acquire() pops items from this buffer (blocking up to @p timeout).
     *
     * @param endpoint        ZMQ endpoint (e.g. "tcp://127.0.0.1:5555").
     * @param item_size       Expected message size in bytes.
     * @param bind            If true, bind to the endpoint; otherwise connect.
     * @param max_buffer_depth  Drop oldest item when buffer exceeds this depth.
     */
    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    pull_from(const std::string& endpoint, size_t item_size,
              bool bind = false, size_t max_buffer_depth = 64);

    /**
     * @brief Create a write-mode ZmqQueue using a ZMQ PUSH socket.
     *
     * write_acquire() returns a pre-allocated send buffer.
     * write_commit() sends it fire-and-forget via zmq_send.
     *
     * @param endpoint    ZMQ endpoint (e.g. "tcp://127.0.0.1:5555").
     * @param item_size   Size of each message in bytes.
     * @param bind        If true, bind to the endpoint; otherwise connect.
     */
    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    push_to(const std::string& endpoint, size_t item_size,
            bool bind = true);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ZmqQueue() override;
    ZmqQueue(ZmqQueue&&) noexcept;
    ZmqQueue& operator=(ZmqQueue&&) noexcept;
    ZmqQueue(const ZmqQueue&) = delete;
    ZmqQueue& operator=(const ZmqQueue&) = delete;

    // ── Queue interface — reading ─────────────────────────────────────────────

    /** Pops one item from the internal buffer; blocks up to @p timeout. */
    const void* read_acquire(std::chrono::milliseconds timeout) noexcept override;
    /** No-op — item is already consumed from the internal buffer. */
    void        read_release() noexcept override;
    /** Always nullptr for ZmqQueue (no flexzone in ZMQ transport). */
    // read_flexzone() — inherited nullptr default from Queue.

    // ── Queue interface — writing ─────────────────────────────────────────────

    /** Returns pointer to the internal send buffer (always succeeds). */
    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    /** Sends the write buffer via zmq_send (fire-and-forget). */
    void  write_commit() noexcept override;
    /** Discards the write buffer without sending. */
    void  write_abort() noexcept override;
    /** Always nullptr for ZmqQueue. */
    // write_flexzone() — inherited nullptr default from Queue.

    // ── Queue interface — metadata ────────────────────────────────────────────

    size_t      item_size()     const noexcept override;
    // flexzone_size() — inherited 0 default from Queue.
    std::string name()          const override;

    // ── Lifecycle (overrides Queue no-ops) ────────────────────────────────────

    /**
     * @brief Bind/connect socket and start recv_thread_ (read mode).
     * @return true on success; false if already running or socket setup failed.
     */
    bool start() override;

    /**
     * @brief Stop recv_thread_ (read mode), close socket and ZMQ context.
     *
     * Wakes any blocking read_acquire() immediately.  Safe to call multiple times.
     */
    void stop() override;

    bool is_running() const noexcept override;

private:
    explicit ZmqQueue(std::unique_ptr<ZmqQueueImpl> impl);
    std::unique_ptr<ZmqQueueImpl> pImpl;
};

} // namespace pylabhub::hub
