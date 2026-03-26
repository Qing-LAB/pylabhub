// src/utils/hub/hub_zmq_queue.cpp
/**
 * @file hub_zmq_queue.cpp
 * @brief ZmqQueue implementation — ZMQ PULL/PUSH-backed Queue with MessagePack schema framing.
 *
 * Wire format: msgpack fixarray[4] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N)]
 *   payload element i: scalar → native msgpack type; array/string/bytes → bin(byte_size)
 */
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"
#include "zmq_wire_helpers.hpp"

#include <zmq.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// Constants
// ============================================================================

/// Rate-limit interval for schema tag mismatch warnings.
static constexpr std::chrono::seconds kMismatchWarnInterval{1};

// ============================================================================
// ZmqQueueImpl — internal state
// ============================================================================
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) — field order kept for clarity; reorder in a dedicated layout pass if desired
struct ZmqQueueImpl
{
    enum class Mode { Read, Write } mode;

    std::string endpoint;
    std::string actual_endpoint; ///< Resolved after zmq_bind() — e.g. port 0 → actual port.
    bool        bind_socket{false};
    size_t      item_sz{0};
    size_t      max_depth{64};
    std::string queue_name;
    int         sndhwm{0}; ///< ZMQ_SNDHWM for PUSH socket (0 = ZMQ default 1000)

    // ── Schema tag (optional — 8 bytes of BLAKE2b-256 of BLDS) ───────────────
    std::array<uint8_t, 8> schema_tag_{};
    bool                   has_schema_tag_{false};

    // ── Schema-based field encoding ───────────────────────────────────────────
    std::vector<wire_detail::WireFieldDesc> schema_defs_;
    size_t                 max_frame_sz_{0}; ///< recv frame buffer size

    void* zmq_ctx{nullptr};
    void* socket{nullptr};

    // ── Read mode — pre-allocated ring buffer [ZQ9] ──────────────────────────
    std::atomic<bool>                recv_stop_{false};
    std::thread                      recv_thread_;

    std::vector<std::vector<std::byte>> recv_ring_;  ///< [max_depth] pre-allocated slots
    size_t                           ring_head_{0};  ///< index of oldest item
    size_t                           ring_tail_{0};  ///< index of next write slot
    size_t                           ring_count_{0}; ///< items currently in ring
    std::mutex                       recv_mu_;
    std::condition_variable          recv_cv_;

    std::vector<std::byte>           decode_tmp_;    ///< recv-thread-only staging buffer
    std::vector<std::byte>           current_read_buf_; ///< returned by read_acquire()

    // ── Write mode — internal send buffer + send_thread_ ─────────────────────
    // The caller writes into write_buf_ (returned by write_acquire()).
    // write_commit() copies write_buf_ into send_ring_[send_tail_] and signals
    // send_thread_.  send_thread_ drains the ring: packs msgpack frames and
    // calls zmq_send, retrying on EAGAIN.  This decouples the caller from the
    // ZMQ send latency and provides configurable back-pressure.
    std::vector<std::byte>              write_buf_;     ///< caller-visible write buffer (item_sz bytes)
    size_t                              send_depth_{64};
    OverflowPolicy                      overflow_policy_{OverflowPolicy::Drop};
    int                                 send_retry_interval_ms_{10};

    std::vector<std::vector<std::byte>> send_ring_;     ///< pre-allocated send ring (send_depth_ × item_sz)
    size_t                              send_head_{0};  ///< oldest filled slot (send_thread_ reads here)
    size_t                              send_tail_{0};  ///< next write slot (write_commit() writes here)
    size_t                              send_count_{0}; ///< filled slots waiting to be sent
    std::mutex                          send_mu_;
    std::condition_variable             send_cv_;
    std::atomic<bool>                   send_stop_{false};
    std::thread                         send_thread_;
    std::vector<std::byte>              send_local_buf_; ///< send_thread_-private staging (item_sz bytes)
    msgpack::sbuffer                    send_sbuf_;      ///< send_thread_-private msgpack buffer (reused)
    std::atomic<uint64_t>               send_seq_{0};

    // ── Counters ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> recv_overflow_count_{0};
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0};  ///< [ZQ10] sequence gaps
    std::atomic<uint64_t> send_drop_count_{0};
    std::atomic<uint64_t> send_retry_count_{0};
    std::atomic<uint64_t> data_drop_count_{0};

    // ── Domain 2+3 timing (HEP-CORE-0008 §10) ──────────────────────────────
    // Measured in read_acquire/read_release (read mode) or
    // write_acquire/write_commit (write mode). Same semantics as DataBlock ContextMetrics.
    using Clock = std::chrono::steady_clock;
    std::atomic<uint64_t> last_slot_wait_us_{0};
    std::atomic<uint64_t> last_iteration_us_{0};
    std::atomic<uint64_t> max_iteration_us_{0};
    std::atomic<uint64_t> last_slot_exec_us_{0};
    std::atomic<uint64_t> configured_period_us_{0};
    std::atomic<uint64_t> context_elapsed_us_{0};

    // Per-thread timestamps (not atomic — only accessed from caller thread).
    Clock::time_point t_iter_start_{};       ///< Start of previous acquire (for iteration gap).
    Clock::time_point t_acquired_{};         ///< When last acquire returned (for work time).
    Clock::time_point context_start_time_{}; ///< First acquire timestamp (for context_elapsed_us).

    // ── Sequence tracking [ZQ10] ─────────────────────────────────────────────
    uint64_t expected_seq_{0};
    bool     seq_initialized_{false};
    /// Last wire seq decoded by recv_thread_. Atomic: written by recv_thread_,
    /// read by last_seq() from caller thread. Diagnostic use only.
    std::atomic<uint64_t> last_seq_{0};

    // ── Rate-limited mismatch warning [ZQ6] ──────────────────────────────────
    std::chrono::steady_clock::time_point last_mismatch_warn_{};

    std::atomic<bool> running_{false};

    // ────────────────────────────────────────────────────────────────────────
    // recv thread body — all decoding uses pre-allocated buffers (no per-frame heap alloc).
    void run_recv_thread_()
    {
        static constexpr int kRecvTimeoutMs = 100;
        if (socket)
            zmq_setsockopt(socket, ZMQ_RCVTIMEO, &kRecvTimeoutMs, sizeof(kRecvTimeoutMs));

        std::vector<char> frame_buf(max_frame_sz_);

        while (!recv_stop_.load(std::memory_order_relaxed))
        {
            if (!socket) break;

            int rc = zmq_recv(socket, frame_buf.data(),
                              static_cast<int>(max_frame_sz_), 0);

            if (rc < 0)
            {
                const int err = zmq_errno();
                if (err == EAGAIN || err == EINTR)
                    continue;
                if (err != ETERM)
                    LOGGER_WARN("[hub::ZmqQueue] recv error on '{}': {}",
                                queue_name, zmq_strerror(err));
                break;
            }

            // zmq_recv() returns actual message size (bytes in the message, not bytes
            // copied).  max_frame_sz_ has a 4-byte slack above any valid schema frame,
            // so rc >= max_frame_sz_ indicates a malformed or oversized frame. [ZQ4]
            if (rc >= static_cast<int>(max_frame_sz_))
            {
                ++recv_frame_error_count_;
                continue;
            }

            try
            {
                msgpack::object_handle oh = msgpack::unpack(
                    frame_buf.data(), static_cast<size_t>(rc));
                const msgpack::object& obj = oh.get();

                // Outer frame: array of 4
                if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 4)
                    { ++recv_frame_error_count_; continue; }
                const auto* elems = obj.via.array.ptr;

                // [0] magic
                uint32_t magic = 0;
                elems[0].convert(magic);
                if (magic != wire_detail::kFrameMagic)
                    { ++recv_frame_error_count_; continue; }

                // [1] schema_tag (bin, 8 bytes)
                if (elems[1].type != msgpack::type::BIN || elems[1].via.bin.size != 8)
                    { ++recv_frame_error_count_; continue; }
                if (has_schema_tag_ &&
                    std::memcmp(elems[1].via.bin.ptr, schema_tag_.data(), 8) != 0)
                {
                    ++recv_frame_error_count_;
                    // Rate-limited warning: at most once per kMismatchWarnInterval. [ZQ6]
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_mismatch_warn_ >= kMismatchWarnInterval)
                    {
                        LOGGER_WARN("[hub::ZmqQueue] schema tag mismatch on '{}' "
                                    "(suppressing further warnings for {}s)",
                                    queue_name, kMismatchWarnInterval.count());
                        last_mismatch_warn_ = now;
                    }
                    continue;
                }

                // [2] seq — track gaps. [ZQ10]
                {
                    uint64_t seq = 0;
                    elems[2].convert(seq);
                    if (seq_initialized_)
                    {
                        // Guard against unsigned underflow: only count forward gaps.
                        // seq < expected_seq_ can occur on sender restart; silently ignore.
                        if (seq > expected_seq_)
                            recv_gap_count_.fetch_add(seq - expected_seq_,
                                                      std::memory_order_relaxed);
                    }
                    expected_seq_    = seq + 1;
                    seq_initialized_ = true;
                    last_seq_.store(seq, std::memory_order_relaxed);
                }

                // [3] payload: msgpack array of N typed field values
                const auto& arr = elems[3];
                if (arr.type != msgpack::type::ARRAY ||
                    arr.via.array.size != schema_defs_.size())
                {
                    ++recv_frame_error_count_;
                    continue;
                }

                // Decode into decode_tmp_ (pre-allocated; recv-thread-private). [ZQ9]
                std::fill(decode_tmp_.begin(), decode_tmp_.end(), std::byte{0});
                bool ok = true;
                char* dst = reinterpret_cast<char*>(decode_tmp_.data());
                for (size_t i = 0; i < schema_defs_.size() && ok; ++i)
                {
                    if (!wire_detail::unpack_field(arr.via.array.ptr[i], schema_defs_[i], dst))
                    {
                        ++recv_frame_error_count_;
                        ok = false;
                    }
                }
                if (!ok) continue;

                // Push to ring buffer (pre-allocated; memcpy under lock). [ZQ9]
                {
                    std::unique_lock<std::mutex> lk(recv_mu_);
                    if (ring_count_ >= max_depth)
                    {
                        // Overwrite oldest slot (drop it).
                        ring_head_ = (ring_head_ + 1) % max_depth;
                        ++recv_overflow_count_;
                    }
                    else
                    {
                        ++ring_count_;
                    }
                    std::memcpy(recv_ring_[ring_tail_].data(), decode_tmp_.data(), item_sz);
                    ring_tail_ = (ring_tail_ + 1) % max_depth;
                }
                recv_cv_.notify_one();
            }
            catch (const std::exception& e)
            {
                ++recv_frame_error_count_;
                LOGGER_WARN("[hub::ZmqQueue] unpack error on '{}': {}",
                            queue_name, e.what());
            }
        }

        recv_cv_.notify_all();
    }

    // ────────────────────────────────────────────────────────────────────────
    // send thread body — drains send_ring_ FIFO; retries zmq_send on EAGAIN.
    // On stop signal: sends remaining items once (no retry), then exits.
    void run_send_thread_()
    {
        while (true)
        {
            // ── Wait for a slot or stop ───────────────────────────────────
            {
                std::unique_lock<std::mutex> lk(send_mu_);
                send_cv_.wait(lk, [this] {
                    return send_count_ > 0 || send_stop_.load(std::memory_order_relaxed);
                });
                if (send_count_ == 0) break; // stop_ + empty → exit

                // Copy head slot to thread-private buffer under lock (fast memcpy).
                std::memcpy(send_local_buf_.data(), send_ring_[send_head_].data(), item_sz);
            }

            // ── Pack msgpack frame from send_local_buf_ ───────────────────
            {
                send_sbuf_.clear();
                msgpack::packer<msgpack::sbuffer> pk(send_sbuf_);

                pk.pack_array(4);
                pk.pack(wire_detail::kFrameMagic);
                pk.pack_bin(8);
                pk.pack_bin_body(reinterpret_cast<const char*>(schema_tag_.data()), 8);
                pk.pack(send_seq_.fetch_add(1, std::memory_order_relaxed));
                pk.pack_array(static_cast<uint32_t>(schema_defs_.size()));
                const char* src = reinterpret_cast<const char*>(send_local_buf_.data());
                for (const auto& fd : schema_defs_)
                    wire_detail::pack_field(pk, fd, src);
            }

            // ── Send with retry on EAGAIN ─────────────────────────────────
            while (socket)
            {
                const int rc = zmq_send(socket,
                                        send_sbuf_.data(),
                                        send_sbuf_.size(),
                                        ZMQ_DONTWAIT);
                if (rc >= 0) { break; }

                const int err = zmq_errno();
                if (err == EAGAIN)
                {
                    if (send_stop_.load(std::memory_order_relaxed))
                    {
                        // On stop drain: single attempt only — drop without counting a retry.
                        ++send_drop_count_;
                        break;
                    }
                    ++send_retry_count_; // genuine retry (normal operation)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(send_retry_interval_ms_));
                    continue;
                }
                // Non-retriable error
                if (err != ETERM)
                    LOGGER_WARN("[hub::ZmqQueue] send error on '{}': {}",
                                queue_name, zmq_strerror(err));
                ++send_drop_count_;
                break;
            }
            // ── Advance ring head ─────────────────────────────────────────
            {
                std::lock_guard<std::mutex> lk(send_mu_);
                send_head_ = (send_head_ + 1) % send_depth_;
                --send_count_;
            }
            send_cv_.notify_all(); // wake blocked write_acquire (Block policy)
        }
    }
};

// ============================================================================
// Schema layout computation (file-scope helpers)
// ============================================================================

/// Validate all type_str values in a schema.  Returns the first invalid type_str,
/// or an empty string if all are valid.  [ZQ1]
static std::string find_invalid_type(const std::vector<ZmqSchemaField>& schema)
{
    for (const auto& f : schema)
    {
        if (!wire_detail::is_valid_type_str(f.type_str))
            return f.type_str;
    }
    return {};
}

// ============================================================================
// Factories — schema mode
// ============================================================================

std::unique_ptr<QueueReader>
ZmqQueue::pull_from(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
                    std::string packing,
                    bool bind, size_t max_buffer_depth,
                    std::optional<std::array<uint8_t, 8>> schema_tag)
{
    if (schema.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': schema must not be empty", endpoint);
        return nullptr;
    }
    if (max_buffer_depth == 0)
    {
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': max_buffer_depth must be > 0", endpoint);
        return nullptr;
    }
    if (packing != "aligned" && packing != "packed")
    {
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': invalid packing '{}' (must be \"aligned\" or \"packed\")",
                     endpoint, packing);
        return nullptr;
    }
    // [ZQ1] Validate all type strings before computing layout.
    if (const std::string bad = find_invalid_type(schema); !bad.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': invalid type_str '{}'", endpoint, bad);
        return nullptr;
    }
    // Validate string/bytes fields have length > 0; numeric fields have count >= 1. [C2]
    for (const auto& f : schema)
    {
        if ((f.type_str == "string" || f.type_str == "bytes") && f.length == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': string/bytes field has length=0", endpoint);
            return nullptr;
        }
        if (f.type_str != "string" && f.type_str != "bytes" && f.count == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': numeric/array field count must be >= 1", endpoint);
            return nullptr;
        }
    }
    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl               = std::make_unique<ZmqQueueImpl>();
    impl->mode              = ZmqQueueImpl::Mode::Read;
    impl->endpoint          = endpoint;
    impl->bind_socket       = bind;
    impl->item_sz           = item_sz;
    impl->max_depth         = max_buffer_depth;
    impl->queue_name        = endpoint;
    impl->max_frame_sz_     = wire_detail::max_frame_size(layouts);
    impl->schema_defs_      = std::move(layouts);
    if (schema_tag) { impl->schema_tag_ = *schema_tag; impl->has_schema_tag_ = true; }

    // Pre-allocate ring buffer (max_depth slots) and decode staging buffer. [ZQ9]
    impl->recv_ring_.assign(max_buffer_depth, std::vector<std::byte>(item_sz, std::byte{0}));
    impl->decode_tmp_.resize(item_sz, std::byte{0});
    impl->current_read_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<QueueReader>(new ZmqQueue(std::move(impl)));
}

std::unique_ptr<QueueWriter>
ZmqQueue::push_to(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
                  std::string packing,
                  bool bind,
                  std::optional<std::array<uint8_t, 8>> schema_tag,
                  int sndhwm,
                  size_t send_buffer_depth,
                  OverflowPolicy overflow_policy,
                  int send_retry_interval_ms)
{
    if (schema.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': schema must not be empty", endpoint);
        return nullptr;
    }
    if (send_buffer_depth == 0)
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': send_buffer_depth must be > 0", endpoint);
        return nullptr;
    }
    if (packing != "aligned" && packing != "packed")
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': invalid packing '{}' (must be \"aligned\" or \"packed\")",
                     endpoint, packing);
        return nullptr;
    }
    // [ZQ1] Validate all type strings before computing layout.
    if (const std::string bad = find_invalid_type(schema); !bad.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': invalid type_str '{}'", endpoint, bad);
        return nullptr;
    }
    // Validate string/bytes fields have length > 0; numeric fields have count >= 1. [C2]
    for (const auto& f : schema)
    {
        if ((f.type_str == "string" || f.type_str == "bytes") && f.length == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': string/bytes field has length=0", endpoint);
            return nullptr;
        }
        if (f.type_str != "string" && f.type_str != "bytes" && f.count == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': numeric/array field count must be >= 1", endpoint);
            return nullptr;
        }
    }
    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl                       = std::make_unique<ZmqQueueImpl>();
    impl->mode                      = ZmqQueueImpl::Mode::Write;
    impl->endpoint                  = endpoint;
    impl->bind_socket               = bind;
    impl->item_sz                   = item_sz;
    impl->queue_name                = endpoint;
    impl->sndhwm                    = sndhwm; // [ZQ8]
    impl->send_depth_               = send_buffer_depth;
    impl->overflow_policy_          = overflow_policy;
    impl->send_retry_interval_ms_   = send_retry_interval_ms;
    impl->max_frame_sz_             = wire_detail::max_frame_size(layouts);
    impl->schema_defs_              = std::move(layouts);
    if (schema_tag) { impl->schema_tag_ = *schema_tag; impl->has_schema_tag_ = true; }

    // Pre-allocate caller write buffer, send ring, and send_thread_-private buffer.
    impl->write_buf_.resize(item_sz, std::byte{0});
    impl->send_ring_.assign(send_buffer_depth, std::vector<std::byte>(item_sz, std::byte{0}));
    impl->send_local_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<QueueWriter>(new ZmqQueue(std::move(impl)));
}

// ============================================================================
// Constructor / destructor / move
// ============================================================================

ZmqQueue::ZmqQueue(std::unique_ptr<ZmqQueueImpl> impl) : pImpl(std::move(impl)) {}

ZmqQueue::~ZmqQueue()
{
    stop();
}

// NOTE: ZmqQueue is NOT restartable.  After stop(), ring/send indices (ring_head_,
// ring_tail_, ring_count_, send_head_, send_tail_, send_count_) are NOT reset.
// Calling start() again would deliver stale ring items and re-send stale send-ring
// slots.  Destroy and reconstruct the queue if a restart is needed.

ZmqQueue::ZmqQueue(ZmqQueue&&) noexcept = default;

ZmqQueue& ZmqQueue::operator=(ZmqQueue&& o) noexcept
{
    // The defaulted move-assignment would destroy pImpl without stopping threads first.
    // A running ZmqQueueImpl holds joinable std::threads; destroying them without
    // join() calls std::terminate().  Explicitly stop before releasing pImpl.
    if (this != &o)
    {
        stop();
        pImpl = std::move(o.pImpl);
    }
    return *this;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool ZmqQueue::start()
{
    if (!pImpl) return false;
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race — another thread started it

    pImpl->zmq_ctx = zmq_ctx_new();
    if (!pImpl->zmq_ctx)
    {
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] zmq_ctx_new() failed for '{}'", pImpl->queue_name);
        return false;
    }
    zmq_ctx_set(pImpl->zmq_ctx, ZMQ_BLOCKY, 0);

    const int socket_type = (pImpl->mode == ZmqQueueImpl::Mode::Read) ? ZMQ_PULL : ZMQ_PUSH;
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

    // [ZQ8] Apply SNDHWM on PUSH sockets when caller requested a specific value.
    if (pImpl->mode == ZmqQueueImpl::Mode::Write && pImpl->sndhwm > 0)
        zmq_setsockopt(pImpl->socket, ZMQ_SNDHWM, &pImpl->sndhwm, sizeof(pImpl->sndhwm));

    const int rc = pImpl->bind_socket
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

    // Resolve actual bound endpoint (captures OS-assigned port when endpoint uses ":0").
    if (pImpl->bind_socket)
    {
        char buf[256]{};
        size_t buf_len = sizeof(buf);
        if (zmq_getsockopt(pImpl->socket, ZMQ_LAST_ENDPOINT, buf, &buf_len) == 0)
            pImpl->actual_endpoint = buf;
        else
        {
            LOGGER_WARN("[hub::ZmqQueue] zmq_getsockopt(ZMQ_LAST_ENDPOINT) failed for '{}': {}; "
                        "actual_endpoint() may be inaccurate",
                        pImpl->queue_name, zmq_strerror(zmq_errno()));
            pImpl->actual_endpoint = pImpl->endpoint;
        }
    }
    else
    {
        pImpl->actual_endpoint = pImpl->endpoint;
    }

    // [ZQ5] Capture pImpl raw pointer, not `this`, so move of ZmqQueue is safe.
    ZmqQueueImpl* impl_ptr = pImpl.get();

    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        pImpl->recv_stop_.store(false, std::memory_order_release);
        pImpl->recv_thread_ = std::thread([impl_ptr] { impl_ptr->run_recv_thread_(); });
    }
    else // Write
    {
        pImpl->send_stop_.store(false, std::memory_order_release);
        pImpl->send_thread_ = std::thread([impl_ptr] { impl_ptr->run_send_thread_(); });
    }

    return true;
}

void ZmqQueue::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    // Signal all background threads to exit.
    pImpl->recv_stop_.store(true, std::memory_order_release);
    pImpl->send_stop_.store(true, std::memory_order_release);
    // HR-06: recv_cv_.notify_all() wakes any thread blocked in read_acquire() so it
    // can observe the stopping condition and return nullptr.  It does NOT wake the
    // recv_thread_ itself (which polls on its own ZMQ_RCVTIMEO interval).
    pImpl->recv_cv_.notify_all();
    pImpl->send_cv_.notify_all(); // wake send_thread_ to drain remaining items

    if (pImpl->recv_thread_.joinable())
        pImpl->recv_thread_.join();

    // send_thread_ will drain remaining items (one attempt each, no retry after stop_)
    // then exit.
    if (pImpl->send_thread_.joinable())
        pImpl->send_thread_.join();

    if (pImpl->socket)  { zmq_close(pImpl->socket);     pImpl->socket   = nullptr; }
    if (pImpl->zmq_ctx) { zmq_ctx_term(pImpl->zmq_ctx); pImpl->zmq_ctx  = nullptr; }
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
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Read) return nullptr;

    const auto t_entry = ZmqQueueImpl::Clock::now();

    // Block until data is available.
    std::unique_lock<std::mutex> lk(pImpl->recv_mu_);
    pImpl->recv_cv_.wait_for(lk, timeout, [this] {
        return pImpl->ring_count_ > 0 ||
               pImpl->recv_stop_.load(std::memory_order_relaxed);
    });

    if (pImpl->ring_count_ == 0) return nullptr;

    const auto t_acquired = ZmqQueueImpl::Clock::now();

    // Copy from ring slot to current_read_buf_ (pre-allocated; no heap alloc). [ZQ9]
    std::memcpy(pImpl->current_read_buf_.data(),
                pImpl->recv_ring_[pImpl->ring_head_].data(),
                pImpl->item_sz);
    pImpl->ring_head_ = (pImpl->ring_head_ + 1) % pImpl->max_depth;
    --pImpl->ring_count_;

    // Timing metrics — computed after wait, matching DataBlock's measurement points.
    // last_iteration_us = t_acquired(N) - t_acquired(N-1) = full cycle including wait.
    // last_slot_wait_us = t_acquired - t_entry = time blocked in this acquire.
    const ZmqQueueImpl::Clock::time_point t_zero{};
    if (pImpl->t_iter_start_ != t_zero)
    {
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->t_iter_start_).count());
        pImpl->last_iteration_us_.store(elapsed_us, std::memory_order_relaxed);
        if (elapsed_us > pImpl->max_iteration_us_.load(std::memory_order_relaxed))
            pImpl->max_iteration_us_.store(elapsed_us, std::memory_order_relaxed);
        pImpl->context_elapsed_us_.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->context_start_time_).count()),
            std::memory_order_relaxed);
        // No timing overrun detection — the main loop owns the deadline.
        // Reader never drops data (data_drop_count stays 0).
    }
    else
    {
        pImpl->context_start_time_ = t_acquired;
    }

    pImpl->last_slot_wait_us_.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            t_acquired - t_entry).count()),
        std::memory_order_relaxed);
    pImpl->t_iter_start_ = t_acquired;
    pImpl->t_acquired_   = t_acquired;

    return pImpl->current_read_buf_.data();
}

void ZmqQueue::read_release() noexcept
{
    // Item already copied to current_read_buf_ in read_acquire().
    // Measure work time: acquire return → release call.
    if (pImpl)
    {
        const ZmqQueueImpl::Clock::time_point t_zero{};
        if (pImpl->t_acquired_ != t_zero)
        {
            pImpl->last_slot_exec_us_.store(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    ZmqQueueImpl::Clock::now() - pImpl->t_acquired_).count()),
                std::memory_order_relaxed);
        }
    }
}

// ============================================================================
// Writing
// ============================================================================

void* ZmqQueue::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write) return nullptr;
    if (pImpl->send_stop_.load(std::memory_order_relaxed))
        return nullptr;

    const auto t_entry = ZmqQueueImpl::Clock::now();

    if (pImpl->overflow_policy_ == OverflowPolicy::Drop)
    {
        std::lock_guard<std::mutex> lk(pImpl->send_mu_);
        if (pImpl->send_count_ >= pImpl->send_depth_)
        {
            pImpl->data_drop_count_.fetch_add(1, std::memory_order_relaxed); // buffer-full: cycle failed
            return nullptr;
        }
    }
    else // Block
    {
        ZmqQueueImpl* impl_ptr = pImpl.get();
        std::unique_lock<std::mutex> lk(impl_ptr->send_mu_);
        const bool ok = impl_ptr->send_cv_.wait_for(lk, timeout, [impl_ptr] {
            return impl_ptr->send_count_ < impl_ptr->send_depth_ ||
                   impl_ptr->send_stop_.load(std::memory_order_relaxed);
        });
        if (!ok || impl_ptr->send_stop_.load(std::memory_order_relaxed))
        {
            impl_ptr->data_drop_count_.fetch_add(1, std::memory_order_relaxed); // timeout or shutdown: cycle failed
            return nullptr;
        }
    }

    // Successful acquire — timing computed after wait, matching DataBlock.
    // last_iteration_us = t_acquired(N) - t_acquired(N-1) = full cycle including wait.
    const auto t_acquired = ZmqQueueImpl::Clock::now();

    const ZmqQueueImpl::Clock::time_point t_zero{};
    if (pImpl->t_iter_start_ != t_zero)
    {
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->t_iter_start_).count());
        pImpl->last_iteration_us_.store(elapsed_us, std::memory_order_relaxed);
        if (elapsed_us > pImpl->max_iteration_us_.load(std::memory_order_relaxed))
            pImpl->max_iteration_us_.store(elapsed_us, std::memory_order_relaxed);
        pImpl->context_elapsed_us_.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->context_start_time_).count()),
            std::memory_order_relaxed);

        // No timing overrun detection — the main loop owns the deadline.
        // Write-side data_drop_count is incremented above on buffer-full/timeout only.
    }
    else
    {
        pImpl->context_start_time_ = t_acquired;
    }

    pImpl->last_slot_wait_us_.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            t_acquired - t_entry).count()),
        std::memory_order_relaxed);
    pImpl->t_iter_start_ = t_acquired;
    pImpl->t_acquired_   = t_acquired;

    return pImpl->write_buf_.data();
}

void ZmqQueue::write_commit() noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write) return;

    // Measure work time: acquire return → commit call.
    {
        const ZmqQueueImpl::Clock::time_point t_zero{};
        if (pImpl->t_acquired_ != t_zero)
        {
            pImpl->last_slot_exec_us_.store(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    ZmqQueueImpl::Clock::now() - pImpl->t_acquired_).count()),
                std::memory_order_relaxed);
        }
    }

    // Copy write_buf_ into the next send ring slot under lock.
    {
        std::lock_guard<std::mutex> lk(pImpl->send_mu_);
        if (pImpl->send_count_ >= pImpl->send_depth_)
        {
            LOGGER_ERROR("[hub::ZmqQueue] write_commit on '{}': ring full — "
                         "violated single-acquire contract; item dropped",
                         pImpl->queue_name);
            return;
        }
        std::memcpy(pImpl->send_ring_[pImpl->send_tail_].data(),
                    pImpl->write_buf_.data(),
                    pImpl->item_sz);
        pImpl->send_tail_ = (pImpl->send_tail_ + 1) % pImpl->send_depth_;
        ++pImpl->send_count_;
    }
    pImpl->send_cv_.notify_one(); // wake send_thread_
}

void ZmqQueue::write_discard() noexcept
{
    // No-op: write_buf_ was not added to the send ring.
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

uint64_t ZmqQueue::last_seq() const noexcept
{
    return pImpl ? pImpl->last_seq_.load(std::memory_order_relaxed) : 0;
}

size_t ZmqQueue::capacity() const
{
    if (!pImpl) return 0;
    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
        return pImpl->max_depth;
    return pImpl->send_depth_;
}

std::string ZmqQueue::policy_info() const
{
    if (!pImpl)
        return "zmq_unconnected";
    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
        return "zmq_pull_ring_" + std::to_string(pImpl->max_depth);
    return (pImpl->overflow_policy_ == OverflowPolicy::Drop)
               ? "zmq_push_drop"
               : "zmq_push_block";
}

std::string ZmqQueue::actual_endpoint() const
{
    if (!pImpl) return "";
    // After start(): resolved (e.g. port-0 bind resolves to actual port).
    // Before start() or on connect-mode: returns configured endpoint.
    return pImpl->actual_endpoint.empty() ? pImpl->endpoint : pImpl->actual_endpoint;
}

// ============================================================================
// Diagnostics counters
// ============================================================================

uint64_t ZmqQueue::recv_overflow_count() const noexcept
{
    return pImpl ? pImpl->recv_overflow_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t ZmqQueue::recv_frame_error_count() const noexcept
{
    return pImpl ? pImpl->recv_frame_error_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t ZmqQueue::recv_gap_count() const noexcept
{
    return pImpl ? pImpl->recv_gap_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t ZmqQueue::send_drop_count() const noexcept
{
    return pImpl ? pImpl->send_drop_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t ZmqQueue::send_retry_count() const noexcept
{
    return pImpl ? pImpl->send_retry_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t ZmqQueue::data_drop_count() const noexcept
{
    return pImpl ? pImpl->data_drop_count_.load(std::memory_order_relaxed) : 0;
}

QueueMetrics ZmqQueue::metrics() const noexcept
{
    if (!pImpl) return {};
    QueueMetrics m;
    // Domain 2+3 timing fields.
    m.last_slot_wait_us    = pImpl->last_slot_wait_us_.load(std::memory_order_relaxed);
    m.last_iteration_us    = pImpl->last_iteration_us_.load(std::memory_order_relaxed);
    m.max_iteration_us     = pImpl->max_iteration_us_.load(std::memory_order_relaxed);
    m.context_elapsed_us   = pImpl->context_elapsed_us_.load(std::memory_order_relaxed);
    m.last_slot_exec_us    = pImpl->last_slot_exec_us_.load(std::memory_order_relaxed);
    m.data_drop_count        = pImpl->data_drop_count_.load(std::memory_order_relaxed);
    m.configured_period_us = pImpl->configured_period_us_.load(std::memory_order_relaxed);
    // Transport-specific counters.
    m.recv_overflow_count    = pImpl->recv_overflow_count_.load(std::memory_order_relaxed);
    m.recv_frame_error_count = pImpl->recv_frame_error_count_.load(std::memory_order_relaxed);
    m.recv_gap_count         = pImpl->recv_gap_count_.load(std::memory_order_relaxed);
    m.send_drop_count        = pImpl->send_drop_count_.load(std::memory_order_relaxed);
    m.send_retry_count       = pImpl->send_retry_count_.load(std::memory_order_relaxed);
    return m;
}

void ZmqQueue::reset_metrics()
{
    if (!pImpl) return;
    pImpl->last_slot_wait_us_.store(0, std::memory_order_relaxed);
    pImpl->last_iteration_us_.store(0, std::memory_order_relaxed);
    pImpl->max_iteration_us_.store(0, std::memory_order_relaxed);
    pImpl->last_slot_exec_us_.store(0, std::memory_order_relaxed);
    pImpl->data_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->context_elapsed_us_.store(0, std::memory_order_relaxed);
    pImpl->recv_overflow_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_frame_error_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_gap_count_.store(0, std::memory_order_relaxed);
    pImpl->send_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->send_retry_count_.store(0, std::memory_order_relaxed);
    pImpl->t_iter_start_       = {};
    pImpl->t_acquired_         = {};
    pImpl->context_start_time_ = {};
}

void ZmqQueue::set_configured_period(uint64_t period_us)
{
    if (!pImpl) return;
    pImpl->configured_period_us_.store(period_us, std::memory_order_relaxed);
}

} // namespace pylabhub::hub
