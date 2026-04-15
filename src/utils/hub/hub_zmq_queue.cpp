// src/utils/hub/hub_zmq_queue.cpp
/**
 * @file hub_zmq_queue.cpp
 * @brief ZmqQueue implementation — ZMQ PULL/PUSH-backed Queue with MessagePack schema framing.
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N), checksum:bin32]
 *   payload element i: scalar → native msgpack type; array/string/bytes → bin(byte_size)
 *   checksum: BLAKE2b-256 of the raw (pre-pack) slot data
 */
#include "utils/hub_zmq_queue.hpp"
#include "utils/context_metrics.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/logger.hpp"
#include "utils/thread_manager.hpp"
#include "utils/zmq_context.hpp"
#include "zmq_wire_helpers.hpp"

#include "cppzmq/zmq.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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

    /// Caller-supplied stable identifier — used as ThreadManager owner_id so the
    /// lifecycle module name is unique across overlapping queue instances (e.g.
    /// port-0 binds where queue_name collides). Empty → start() falls back to
    /// a pointer-address-based id.
    std::string instance_id;

    // ── Schema tag (optional — 8 bytes of BLAKE2b-256 of BLDS) ───────────────
    std::array<uint8_t, 8> schema_tag_{};
    bool                   has_schema_tag_{false};

    // ── Schema-based field encoding ───────────────────────────────────────────
    std::vector<wire_detail::WireFieldDesc> schema_defs_;
    size_t                 max_frame_sz_{0}; ///< recv frame buffer size

    // Context is the shared process-wide zmq::context_t owned by the
    // ZMQContext lifecycle module (see utils/zmq_context.hpp). The
    // top-level LifecycleGuard must include GetZMQContextModule(); the
    // persistent flag guarantees the context outlives every ZmqQueue
    // instance. No per-queue context — prevents the racing zmq_ctx_term()
    // vs still-running send/recv threads that caused SIGABRT under
    // parallel test load.
    zmq::socket_t socket;  ///< default-constructed empty; assigned in start()

    // ── Read mode — pre-allocated ring buffer [ZQ9] ──────────────────────────
    std::atomic<bool>                recv_stop_{false};
    // Threads live inside a ThreadManager owned by the queue. Recreated
    // per start()/stop() cycle so each active window has its own dynamic
    // lifecycle module "ThreadManager:ZmqQueue:<name>".
    std::unique_ptr<pylabhub::utils::ThreadManager> thread_mgr_;

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
    // send thread lives in thread_mgr_ (declared above in the Read-mode
    // section); spawn() is called from ZmqQueue::start() based on mode.
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
    ContextMetrics ctx_metrics_;  ///< Unified timing + checksum metrics.
    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced}; ///< Default: auto checksum.

    using Clock = ContextMetrics::Clock;

    // Per-thread timestamps (not atomic — only accessed from caller thread).
    Clock::time_point t_iter_start_{};       ///< Start of previous acquire (for iteration gap).
    Clock::time_point t_acquired_{};         ///< When last acquire returned (for work time).

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
    // recv thread body — cppzmq message_t reused across iterations (no per-frame
    // heap alloc beyond what libzmq internally does).
    void run_recv_thread_()
    {
        socket.set(zmq::sockopt::rcvtimeo, 100);  // 100ms poll tick for stop-flag checks

        zmq::message_t msg;

        while (!recv_stop_.load(std::memory_order_relaxed))
        {
            if (!socket) break;

            zmq::recv_result_t rr;
            try
            {
                rr = socket.recv(msg, zmq::recv_flags::none);
            }
            catch (const zmq::error_t &e)
            {
                // ETERM on context shutdown → clean exit; EINTR on signal → retry.
                if (e.num() == ETERM) break;
                if (e.num() == EINTR) continue;
                LOGGER_WARN("[hub::ZmqQueue] recv error on '{}': {}",
                            queue_name, e.what());
                break;
            }
            if (!rr.has_value()) continue;  // RCVTIMEO expired; re-check stop flag

            // max_frame_sz_ has a 4-byte slack above any valid schema frame, so
            // size >= max_frame_sz_ indicates a malformed or oversized frame. [ZQ4]
            if (msg.size() >= max_frame_sz_)
            {
                ++recv_frame_error_count_;
                continue;
            }

            try
            {
                msgpack::object_handle oh = msgpack::unpack(
                    static_cast<const char *>(msg.data()), msg.size());
                const msgpack::object& obj = oh.get();

                // Outer frame: array of 5 [magic, tag, seq, payload, checksum]
                if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 5)
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

                // [4] checksum: bin, 32 bytes.
                if (elems[4].type != msgpack::type::BIN || elems[4].via.bin.size != 32)
                    { ++recv_frame_error_count_; continue; }
                // Verify checksum: Manual and Enforced both verify (catches missing stamps).
                if (checksum_policy_ != ChecksumPolicy::None)
                {
                    if (!pylabhub::crypto::verify_blake2b(
                            reinterpret_cast<const uint8_t*>(elems[4].via.bin.ptr),
                            decode_tmp_.data(), item_sz))
                    {
                        ctx_metrics_.inc_checksum_error();
                        LOGGER_ERROR("[hub::ZmqQueue] checksum error after decode on '{}'",
                                     queue_name);
                        continue; // drop frame
                    }
                }

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

                // Compute BLAKE2b checksum: Enforced = auto-stamp, Manual = caller stamps
                // (zeros here; caller must have written checksum into the slot payload),
                // None = zeros (no checksum).
                uint8_t checksum[32]{};
                if (checksum_policy_ == ChecksumPolicy::Enforced)
                {
                    pylabhub::crypto::compute_blake2b(
                        checksum, send_local_buf_.data(), item_sz);
                }

                pk.pack_array(5);
                pk.pack(wire_detail::kFrameMagic);
                pk.pack_bin(8);
                pk.pack_bin_body(reinterpret_cast<const char*>(schema_tag_.data()), 8);
                pk.pack(send_seq_.fetch_add(1, std::memory_order_relaxed));
                pk.pack_array(static_cast<uint32_t>(schema_defs_.size()));
                const char* src = reinterpret_cast<const char*>(send_local_buf_.data());
                for (const auto& fd : schema_defs_)
                    wire_detail::pack_field(pk, fd, src);
                pk.pack_bin(32);
                pk.pack_bin_body(reinterpret_cast<const char*>(checksum), 32);
            }

            // ── Send with retry on EAGAIN ─────────────────────────────────
            while (socket)
            {
                try
                {
                    auto sr = socket.send(
                        zmq::const_buffer(send_sbuf_.data(), send_sbuf_.size()),
                        zmq::send_flags::dontwait);
                    if (sr.has_value()) break;  // sent

                    // EAGAIN — send ring full
                    if (send_stop_.load(std::memory_order_relaxed))
                    {
                        ++send_drop_count_;
                        break;
                    }
                    ++send_retry_count_;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(send_retry_interval_ms_));
                    continue;
                }
                catch (const zmq::error_t &e)
                {
                    if (e.num() != ETERM)
                        LOGGER_WARN("[hub::ZmqQueue] send error on '{}': {}",
                                    queue_name, e.what());
                    ++send_drop_count_;
                    break;
                }
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
                    std::optional<std::array<uint8_t, 8>> schema_tag,
                    std::string instance_id)
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
    impl->instance_id       = std::move(instance_id);
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
                  int send_retry_interval_ms,
                  std::string instance_id)
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
    impl->instance_id               = std::move(instance_id);
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

    try
    {
        const zmq::socket_type stype = (pImpl->mode == ZmqQueueImpl::Mode::Read)
                                           ? zmq::socket_type::pull
                                           : zmq::socket_type::push;
        pImpl->socket = zmq::socket_t(pylabhub::hub::get_zmq_context(), stype);
        pImpl->socket.set(zmq::sockopt::linger, 0);

        // [ZQ8] Apply SNDHWM on PUSH sockets when caller requested a specific value.
        if (pImpl->mode == ZmqQueueImpl::Mode::Write && pImpl->sndhwm > 0)
            pImpl->socket.set(zmq::sockopt::sndhwm, pImpl->sndhwm);

        if (pImpl->bind_socket)
            pImpl->socket.bind(pImpl->endpoint);
        else
            pImpl->socket.connect(pImpl->endpoint);
    }
    catch (const zmq::error_t &e)
    {
        pImpl->socket.close();
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] socket setup ({}) failed for '{}': {}",
                     pImpl->bind_socket ? "bind" : "connect",
                     pImpl->endpoint, e.what());
        return false;
    }

    // Resolve actual bound endpoint (captures OS-assigned port when endpoint uses ":0").
    if (pImpl->bind_socket)
    {
        try
        {
            pImpl->actual_endpoint =
                pImpl->socket.get(zmq::sockopt::last_endpoint);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("[hub::ZmqQueue] last_endpoint query failed for '{}': {}; "
                        "actual_endpoint() may be inaccurate",
                        pImpl->queue_name, e.what());
            pImpl->actual_endpoint = pImpl->endpoint;
        }
    }
    else
    {
        pImpl->actual_endpoint = pImpl->endpoint;
    }

    // [ZQ5] Capture pImpl raw pointer, not `this`, so move of ZmqQueue is safe.
    ZmqQueueImpl* impl_ptr = pImpl.get();

    // Create a fresh ThreadManager for this start/stop cycle.
    //
    // owner_id MUST be unique across every ZmqQueue live at the same time in
    // this process (lifecycle module names must not collide — async-unload of
    // a prior instance may overlap with re-register of a new one). We use:
    //   1. the caller-supplied instance_id (role passes e.g. "prod:UID-...:tx")
    //      — this is the canonical path for role hosts
    //   2. or, when empty (unit tests constructing queues directly), fall back
    //      to "{queue_name}@{pImpl-address}" — the address is unique per live
    //      instance, so it prevents collisions even with identical endpoints
    //      (port-0 binds) and overlapping teardown windows.
    std::string owner_id;
    if (!pImpl->instance_id.empty())
    {
        owner_id = pImpl->instance_id;
    }
    else
    {
        char addr_buf[32];
        std::snprintf(addr_buf, sizeof(addr_buf), "@%p",
                      static_cast<void *>(impl_ptr));
        owner_id = pImpl->queue_name;
        owner_id += addr_buf;
    }
    pImpl->thread_mgr_ = std::make_unique<pylabhub::utils::ThreadManager>(
        "ZmqQueue", owner_id);

    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        pImpl->recv_stop_.store(false, std::memory_order_release);
        pImpl->thread_mgr_->spawn("recv",
            [impl_ptr] { impl_ptr->run_recv_thread_(); });
    }
    else // Write
    {
        pImpl->send_stop_.store(false, std::memory_order_release);
        pImpl->thread_mgr_->spawn("send",
            [impl_ptr] { impl_ptr->run_send_thread_(); });
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

    // ThreadManager::~destroy() (via unique_ptr reset) bounded-joins both
    // recv_thread_ and send_thread_ (whichever was spawned for this mode).
    // The stop atomics above signal shutdown; the threads observe and exit;
    // the manager joins them with per-thread kMidTimeoutMs deadline. On
    // timeout: ERROR log + detach + increment process_detached_count.
    pImpl->thread_mgr_.reset();

    // Close the socket AFTER threads have exited (threads were the only user).
    // The shared ZMQ context is owned by the ZMQContext lifecycle module —
    // ZmqQueue never terminates it.
    pImpl->socket.close();
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
        pImpl->ctx_metrics_.set_last_iteration(elapsed_us);
        pImpl->ctx_metrics_.update_max_iteration(elapsed_us);
        pImpl->ctx_metrics_.set_context_elapsed(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->ctx_metrics_.context_start_time_val()).count()));
        // No timing overrun detection — the main loop owns the deadline.
        // Reader never drops data (data_drop_count stays 0).
    }
    else
    {
        pImpl->ctx_metrics_.set_context_start(t_acquired);
    }

    pImpl->ctx_metrics_.set_last_slot_wait(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            t_acquired - t_entry).count()));
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
            pImpl->ctx_metrics_.set_last_slot_exec(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    ZmqQueueImpl::Clock::now() - pImpl->t_acquired_).count()));
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
        pImpl->ctx_metrics_.set_last_iteration(elapsed_us);
        pImpl->ctx_metrics_.update_max_iteration(elapsed_us);
        pImpl->ctx_metrics_.set_context_elapsed(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - pImpl->ctx_metrics_.context_start_time_val()).count()));

        // No timing overrun detection — the main loop owns the deadline.
        // Write-side data_drop_count is incremented above on buffer-full/timeout only.
    }
    else
    {
        pImpl->ctx_metrics_.set_context_start(t_acquired);
    }

    pImpl->ctx_metrics_.set_last_slot_wait(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            t_acquired - t_entry).count()));
    pImpl->t_iter_start_ = t_acquired;
    pImpl->t_acquired_   = t_acquired;

    // Zero buffer so padding bytes are deterministic (checksum consistency).
    std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
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
            pImpl->ctx_metrics_.set_last_slot_exec(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    ZmqQueueImpl::Clock::now() - pImpl->t_acquired_).count()));
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
    // Domain 2+3 timing fields (from unified ContextMetrics).
    m.last_slot_wait_us    = pImpl->ctx_metrics_.last_slot_wait_us_val();
    m.last_iteration_us    = pImpl->ctx_metrics_.last_iteration_us_val();
    m.max_iteration_us     = pImpl->ctx_metrics_.max_iteration_us_val();
    m.context_elapsed_us   = pImpl->ctx_metrics_.context_elapsed_us_val();
    m.last_slot_exec_us    = pImpl->ctx_metrics_.last_slot_exec_us_val();
    m.data_drop_count        = pImpl->data_drop_count_.load(std::memory_order_relaxed);
    // configured_period_us reported at loop level (LoopMetricsSnapshot), not queue level.
    // Transport-specific counters.
    m.recv_overflow_count    = pImpl->recv_overflow_count_.load(std::memory_order_relaxed);
    m.recv_frame_error_count = pImpl->recv_frame_error_count_.load(std::memory_order_relaxed);
    m.recv_gap_count         = pImpl->recv_gap_count_.load(std::memory_order_relaxed);
    m.send_drop_count        = pImpl->send_drop_count_.load(std::memory_order_relaxed);
    m.send_retry_count       = pImpl->send_retry_count_.load(std::memory_order_relaxed);
    m.checksum_error_count   = pImpl->ctx_metrics_.checksum_error_count_val();
    return m;
}

void ZmqQueue::reset_metrics()
{
    if (!pImpl) return;
    pImpl->ctx_metrics_.clear();
    pImpl->data_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_overflow_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_frame_error_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_gap_count_.store(0, std::memory_order_relaxed);
    pImpl->send_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->send_retry_count_.store(0, std::memory_order_relaxed);
    pImpl->t_iter_start_ = {};
    pImpl->t_acquired_   = {};
}

void ZmqQueue::init_metrics()
{
    reset_metrics();
    if (!pImpl) return;
    // Reset sequence tracking — only safe at session start (not mid-session,
    // where it would cause false gap detection on the receiver).
    pImpl->send_seq_.store(0, std::memory_order_relaxed);
    pImpl->last_seq_.store(0, std::memory_order_relaxed);
    pImpl->expected_seq_ = 0;
    pImpl->seq_initialized_ = false;
}

void ZmqQueue::set_configured_period(uint64_t period_us)
{
    if (!pImpl) return;
    pImpl->ctx_metrics_.set_configured_period(period_us);
}

void ZmqQueue::set_checksum_policy(ChecksumPolicy policy)
{
    if (!pImpl) return;
    pImpl->checksum_policy_ = policy;
}

} // namespace pylabhub::hub
