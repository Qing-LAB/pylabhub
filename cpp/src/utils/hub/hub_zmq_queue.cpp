// src/utils/hub/hub_zmq_queue.cpp
/**
 * @file hub_zmq_queue.cpp
 * @brief ZmqQueue implementation — ZMQ PULL/PUSH-backed Queue.
 *
 * Uses the raw ZMQ C API (zmq.h) for socket management.  The ZMQ context is
 * private to each ZmqQueue instance (not shared with the pylabhub zmq_context).
 *
 * Read mode: a recv_thread_ continuously receives fixed-size messages into a
 * bounded std::queue buffer.  read_acquire() pops from this buffer with a
 * condition-variable timeout.
 *
 * Write mode: write_acquire() returns a pre-allocated send buffer.
 * write_commit() sends it via zmq_send (ZMQ_DONTWAIT, fire-and-forget).
 */
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"

#include <zmq.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// ZmqQueueImpl — internal state
// ============================================================================

struct ZmqQueueImpl
{
    enum class Mode { Read, Write } mode;

    std::string endpoint;
    bool        bind_socket{false};
    size_t      item_sz{0};
    size_t      max_depth{64};
    std::string queue_name;

    void* zmq_ctx{nullptr};    // zmq_ctx_new()
    void* socket{nullptr};     // ZMQ_PULL or ZMQ_PUSH

    // ── Read mode ────────────────────────────────────────────────────────────
    std::atomic<bool>                    recv_stop_{false};
    std::thread                          recv_thread_;
    std::queue<std::vector<std::byte>>   recv_buf_;
    std::mutex                           recv_mu_;
    std::condition_variable              recv_cv_;
    std::vector<std::byte>               current_read_buf_; // item between acquire/release

    // ── Write mode ───────────────────────────────────────────────────────────
    std::vector<std::byte>               write_buf_; // pre-allocated send buffer

    std::atomic<bool> running_{false};

    // ────────────────────────────────────────────────────────────────────────
    // recv_thread_ body: receive fixed-size messages, push to bounded buffer.
    void run_recv_thread_()
    {
        static constexpr int kRecvTimeoutMs = 100;
        if (socket)
        {
            zmq_setsockopt(socket, ZMQ_RCVTIMEO, &kRecvTimeoutMs, sizeof(kRecvTimeoutMs));
        }

        std::vector<std::byte> tmp(item_sz);

        while (!recv_stop_.load(std::memory_order_relaxed))
        {
            if (!socket)
                break;

            int rc = zmq_recv(socket, tmp.data(), static_cast<int>(item_sz), 0);
            if (rc > 0 || (rc == static_cast<int>(item_sz) && item_sz > 0))
            {
                // Successfully received a message.
                std::unique_lock<std::mutex> lk(recv_mu_);
                if (recv_buf_.size() >= max_depth)
                {
                    recv_buf_.pop(); // drop oldest to keep buffer bounded
                }
                recv_buf_.push(tmp);
                lk.unlock();
                recv_cv_.notify_one();
            }
            else if (rc < 0)
            {
                int err = zmq_errno();
                if (err == EAGAIN || err == EINTR)
                    continue; // timeout or interrupted — check recv_stop_
                // Other errors (ETERM, ENOTSUP, etc.) — exit loop
                if (err != ETERM)
                {
                    LOGGER_WARN("[hub::ZmqQueue] recv error on '{}': {}",
                                queue_name, zmq_strerror(err));
                }
                break;
            }
        }

        // Wake any waiting read_acquire() so it can detect the stop.
        recv_cv_.notify_all();
    }
};

// ============================================================================
// Factories
// ============================================================================

std::unique_ptr<ZmqQueue>
ZmqQueue::pull_from(const std::string& endpoint, size_t item_size,
                    bool bind, size_t max_buffer_depth)
{
    auto impl            = std::make_unique<ZmqQueueImpl>();
    impl->mode           = ZmqQueueImpl::Mode::Read;
    impl->endpoint       = endpoint;
    impl->bind_socket    = bind;
    impl->item_sz        = item_size;
    impl->max_depth      = max_buffer_depth;
    impl->queue_name     = endpoint;
    impl->current_read_buf_.resize(item_size, std::byte{0});
    return std::unique_ptr<ZmqQueue>(new ZmqQueue(std::move(impl)));
}

std::unique_ptr<ZmqQueue>
ZmqQueue::push_to(const std::string& endpoint, size_t item_size, bool bind)
{
    auto impl            = std::make_unique<ZmqQueueImpl>();
    impl->mode           = ZmqQueueImpl::Mode::Write;
    impl->endpoint       = endpoint;
    impl->bind_socket    = bind;
    impl->item_sz        = item_size;
    impl->queue_name     = endpoint;
    impl->write_buf_.resize(item_size, std::byte{0});
    return std::unique_ptr<ZmqQueue>(new ZmqQueue(std::move(impl)));
}

// ============================================================================
// Constructor / destructor / move
// ============================================================================

ZmqQueue::ZmqQueue(std::unique_ptr<ZmqQueueImpl> impl) : pImpl(std::move(impl)) {}

ZmqQueue::~ZmqQueue()
{
    stop();
}

ZmqQueue::ZmqQueue(ZmqQueue&&) noexcept            = default;
ZmqQueue& ZmqQueue::operator=(ZmqQueue&&) noexcept = default;

// ============================================================================
// Lifecycle
// ============================================================================

bool ZmqQueue::start()
{
    if (!pImpl)
        return false;
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return false; // already running

    // Create a private ZMQ context (not shared with the main pylabhub context).
    pImpl->zmq_ctx = zmq_ctx_new();
    if (!pImpl->zmq_ctx)
    {
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] zmq_ctx_new() failed for '{}'", pImpl->queue_name);
        return false;
    }
    // ZMQ_BLOCKY=0: all sockets created in this context default to LINGER=0.
    zmq_ctx_set(pImpl->zmq_ctx, ZMQ_BLOCKY, 0);

    int socket_type = (pImpl->mode == ZmqQueueImpl::Mode::Read) ? ZMQ_PULL : ZMQ_PUSH;
    pImpl->socket = zmq_socket(pImpl->zmq_ctx, socket_type);
    if (!pImpl->socket)
    {
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] zmq_socket() failed for '{}'", pImpl->queue_name);
        return false;
    }

    int linger = 0;
    zmq_setsockopt(pImpl->socket, ZMQ_LINGER, &linger, sizeof(linger));

    int rc = pImpl->bind_socket
                 ? zmq_bind(pImpl->socket, pImpl->endpoint.c_str())
                 : zmq_connect(pImpl->socket, pImpl->endpoint.c_str());
    if (rc != 0)
    {
        zmq_close(pImpl->socket);
        pImpl->socket = nullptr;
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] {} failed for '{}': {}",
                     pImpl->bind_socket ? "zmq_bind" : "zmq_connect",
                     pImpl->endpoint, zmq_strerror(zmq_errno()));
        return false;
    }

    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        pImpl->recv_stop_.store(false, std::memory_order_release);
        pImpl->recv_thread_ = std::thread([this] { pImpl->run_recv_thread_(); });
    }

    return true;
}

void ZmqQueue::stop()
{
    if (!pImpl)
        return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return; // was not running

    // Signal recv_thread_ to stop and wake any blocked read_acquire().
    pImpl->recv_stop_.store(true, std::memory_order_release);
    pImpl->recv_cv_.notify_all();

    if (pImpl->recv_thread_.joinable())
        pImpl->recv_thread_.join();

    if (pImpl->socket)
    {
        zmq_close(pImpl->socket);
        pImpl->socket = nullptr;
    }
    if (pImpl->zmq_ctx)
    {
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
    }
}

bool ZmqQueue::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

// ============================================================================
// Reading
// ============================================================================

const void* ZmqQueue::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Read)
        return nullptr;

    std::unique_lock<std::mutex> lk(pImpl->recv_mu_);
    bool got_item = pImpl->recv_cv_.wait_for(lk, timeout, [this] {
        return !pImpl->recv_buf_.empty() || pImpl->recv_stop_.load(std::memory_order_relaxed);
    });

    if (!got_item || pImpl->recv_buf_.empty())
        return nullptr;

    pImpl->current_read_buf_ = std::move(pImpl->recv_buf_.front());
    pImpl->recv_buf_.pop();
    return pImpl->current_read_buf_.data();
}

void ZmqQueue::read_release() noexcept
{
    // No-op: item was already moved to current_read_buf_ in read_acquire().
}

// ============================================================================
// Writing
// ============================================================================

void* ZmqQueue::write_acquire(std::chrono::milliseconds /*timeout*/) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write)
        return nullptr;
    return pImpl->write_buf_.data();
}

void ZmqQueue::write_commit() noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write || !pImpl->socket)
        return;
    // Fire-and-forget: ZMQ_DONTWAIT so we don't block the process_thread_.
    zmq_send(pImpl->socket, pImpl->write_buf_.data(),
             static_cast<int>(pImpl->item_sz), ZMQ_DONTWAIT);
}

void ZmqQueue::write_abort() noexcept
{
    // No-op: just don't send.
}

// ============================================================================
// Metadata
// ============================================================================

size_t ZmqQueue::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

std::string ZmqQueue::name() const
{
    return pImpl ? pImpl->queue_name : "(null)";
}

} // namespace pylabhub::hub
