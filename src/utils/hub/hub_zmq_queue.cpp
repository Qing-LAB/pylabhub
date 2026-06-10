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
#include "portable_atomic_shared_ptr.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/zap_router.hpp"
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
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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

    // ── PeerAdmission Phase C — auth state ─────────────────────────────────────
    // HEP-CORE-0040 §172 + §8.4 (#158): discrete fields replace the
    // legacy `ZmqAuthOptions` struct.  Empty `identity_key_name_` =
    // no CURVE wired (legacy unauth path; reachable only via the
    // plaintext `pull_from`/`push_to` factories that never populate
    // this field).  When set, `ZmqQueue::start()` reads the identity
    // from `key_store()` at socket-setup time — bytes never
    // materialize in this struct.
    std::string identity_key_name_;

    // PUSH/bind side only — zap_domain advertised on the socket
    // (`zmq::sockopt::zap_domain`).  Empty → start() derives from
    // instance_id or queue_name+addr.
    std::string zap_domain_;

    // PULL/connect side only — server's CURVE pubkey passed to
    // `zmq::sockopt::curve_serverkey`.  Empty would mean "no
    // serverkey" which is invalid on the connect side; the
    // `pull_from` factory validates non-empty before populating.
    std::string server_pubkey_z85_;

    // ── CURVE engagement invariant (HEP-CORE-0035 §2 + #161 C5) ──────────
    // Negotiated mechanism observed from libzmq at `start()` time via
    // `zmq_getsockopt(ZMQ_MECHANISM)`.  `start()` enforces the
    // invariant: if the queried mechanism is not `ZMQ_CURVE` the
    // start fails and this field stays `Uninitialized`.  Public
    // `ZmqQueue::mechanism()` reads with acquire ordering.  Written
    // ONLY by `start()` (after CURVE setsockopts) and `stop()`
    // (reset to Uninitialized) — never by callers.
    std::atomic<Mechanism> mechanism_{Mechanism::Uninitialized};

    // Resolved zap_domain (Phase C): `zap_domain_` if non-empty,
    // else derived from instance_id at start().  Captured here so
    // stop() can release the ZapRouter registration symmetrically.
    std::string resolved_zap_domain_;

    // PUSH/bind side allowlist.  Lock-free reads from ZapRouter's
    // pump thread via PortableAtomicSharedPtr.  Mutations through
    // ZmqQueue::set_peer_allowlist are atomic snapshots (Phase A
    // contract).  Unused on PULL/connect side.
    pylabhub::utils::detail::PortableAtomicSharedPtr<
        const pylabhub::utils::security::PeerAllowlist>
        allowlist_;

    // PUSH/bind side ZAP registration handle.  Active iff CURVE was
    // wired AND this is the server side; released in stop().
    std::optional<pylabhub::utils::security::ZapDomainHandle> zap_handle_;

    // PULL/connect side producer-peer membership (HEP-CORE-0017 §3.3,
    // #103 A2).  Metadata-only today — Pattern A/B socket-level
    // fan-in is future work; the single-producer connect endpoint
    // flows through ZmqQueue factories' `endpoint` parameter.  The
    // dispatch layer (A3) appends here on CHANNEL_AUTH_CHANGED_NOTIFY
    // → GET_CHANNEL_AUTH_ACK so the queue has a stable record of
    // who's authorized.  Mutex-guarded since both broker thread and
    // role thread may touch in future fan-in impl.
    std::mutex                   producer_peers_mu_;
    std::vector<ProducerPeer>    producer_peers_;

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
    //
    // The loop runs inside a `ctx.with_active_loop` bracket (set up by
    // the spawn-site wrapper).  HEP-CORE-0031 §4.1 Thread Shutdown
    // Contract: while the bracket is open this thread may freely touch
    // ZmqQueueImpl members; on bracket exit (loop return) it MUST NOT.
    // The bracket also gives `ZmqQueue::stop()`'s drain a Stage 1 signal
    // (active_loop_depth==0) that distinguishes "stuck inside a zmq op"
    // from "stuck in post-loop cleanup" in the ERROR log on timeout.
    void run_recv_thread_(pylabhub::utils::ThreadManager::SlotContext &ctx)
    {
        socket.set(zmq::sockopt::rcvtimeo, 100);  // 100ms poll tick for stop-flag checks

        zmq::message_t msg;

        while (!recv_stop_.load(std::memory_order_relaxed) &&
               !ctx.shutdown_requested())
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

                auto env = wire_detail::unpack_envelope(oh.get());
                if (!env.valid || env.payload_size != schema_defs_.size())
                    { ++recv_frame_error_count_; continue; }

                // Schema-tag mismatch (rate-limited warning). [ZQ6]
                if (has_schema_tag_ &&
                    std::memcmp(env.recv_tag, schema_tag_.data(), 8) != 0)
                {
                    ++recv_frame_error_count_;
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

                // Sequence gap tracking. [ZQ10]
                {
                    if (seq_initialized_ && env.seq > expected_seq_)
                        recv_gap_count_.fetch_add(env.seq - expected_seq_,
                                                  std::memory_order_relaxed);
                    expected_seq_    = env.seq + 1;
                    seq_initialized_ = true;
                    last_seq_.store(env.seq, std::memory_order_relaxed);
                }

                // Decode payload fields into decode_tmp_. [ZQ9]
                std::fill(decode_tmp_.begin(), decode_tmp_.end(), std::byte{0});
                if (!wire_detail::unpack_payload(*env.payload, schema_defs_,
                                                  decode_tmp_.data()))
                    { ++recv_frame_error_count_; continue; }

                // Checksum verification.
                if (checksum_policy_ != ChecksumPolicy::None)
                {
                    if (!pylabhub::crypto::verify_blake2b(
                            env.checksum, decode_tmp_.data(), item_sz))
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
    //
    // Runs inside a `ctx.with_active_loop` bracket (set up by the
    // spawn-site wrapper); same contract as run_recv_thread_ above.
    void run_send_thread_(pylabhub::utils::ThreadManager::SlotContext &ctx)
    {
        while (!ctx.shutdown_requested())
        {
            // ── Wait for a slot or stop ───────────────────────────────────
            {
                std::unique_lock<std::mutex> lk(send_mu_);
                send_cv_.wait(lk, [this, &ctx] {
                    return send_count_ > 0
                        || send_stop_.load(std::memory_order_relaxed)
                        || ctx.shutdown_requested();
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

                wire_detail::pack_frame(pk, schema_tag_,
                    send_seq_.fetch_add(1, std::memory_order_relaxed),
                    schema_defs_, send_local_buf_.data(), checksum);
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
ZmqQueue::build_plaintext_reader_(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
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
ZmqQueue::build_plaintext_writer_(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
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
// Auth-enabled factories (PeerAdmission Phase C)
// ============================================================================

namespace
{

constexpr std::size_t kCurveKeyZ85Chars = 40;

/// Validate the CURVE factory parameters against the side they're
/// being applied to.  Returns empty string on success; returns a
/// human-readable error describing the first violation on failure.
/// Pure function — no logging, no side effects.
///
/// Validate the CURVE auth parameters BEFORE the queue is constructed,
/// so misconfiguration is surfaced at the factory call site (returns
/// nullptr with a precise diagnostic) rather than as a cryptic libzmq
/// failure inside `start()` against a stale errno from an unrelated
/// prior call (H-Q3).  Used by both `pull_from` (PULL/connect)
/// and `push_to` (PUSH/bind).
std::string
validate_curve_factory_params(std::string_view identity_key_name,
                              std::string_view server_pubkey_z85,
                              bool             bind_side)
{
    // C1 (#157, HEP-CORE-0035 §2) + C4 (#160): CURVE is unconditional
    // on every role↔hub data path; the post-C4 public factories have
    // no plaintext entry point.  An empty identity_key_name is a
    // programmer error that the factory must reject at the call site.
    if (identity_key_name.empty())
        return "keystore_name MUST be non-empty (HEP-CORE-0035 §2 — "
               "CURVE is unconditional on every role↔hub data path; "
               "HEP-CORE-0040 §172 — caller must seed the process "
               "KeyStore with the identity name BEFORE building the "
               "queue).";

    // HEP-CORE-0040 §172: validate that the named identity is
    // resolvable + has the right byte budget BEFORE constructing the
    // queue, so misconfiguration surfaces at the factory call site
    // (returns nullptr with a precise error) rather than as a cryptic
    // libzmq failure inside start().
    {
        namespace sec = pylabhub::utils::security;
        const std::string name_str{identity_key_name};
        if (!sec::key_store_ready())
            return "keystore_name='" + name_str +
                   "' but KeyStore is not initialized (process must "
                   "construct SecureMemorySubsystem + KeyStore before "
                   "any CURVE-wired queue is built)";
        auto &ks = sec::key_store();
        if (!ks.has(name_str))
            return "keystore_name='" + name_str +
                   "' not present in KeyStore (caller must "
                   "`add_identity(name, ...)` BEFORE building the "
                   "queue)";
        const auto pub = ks.pubkey(name_str);
        if (pub.size() != kCurveKeyZ85Chars)
            return "KeyStore entry '" + name_str +
                   "' has pubkey of " + std::to_string(pub.size()) +
                   " chars; expected " +
                   std::to_string(kCurveKeyZ85Chars);
    }

    if (!bind_side)
    {
        // Connect side requires serverkey to verify the server's CURVE
        // identity.  Without it, libzmq refuses the connect with a
        // cryptic "Invalid argument" — surface a specific error here
        // instead.  `Z85PublicKey` already enforces the 40-char length
        // invariant at construction time (#158); we only need to
        // catch the sentinel "empty" case here.
        if (server_pubkey_z85.empty())
            return "connect-side CURVE auth requires "
                   "serverkey_z85 (the producer's CURVE pubkey) when "
                   "keystore_name is set";
    }
    // bind side: serverkey is meaningless (we ARE the server).
    return {};
}

} // namespace

// ============================================================================
// HEP-CORE-0040 §8.4 endpoint shape (AUTH_TODO §C2, task #158)
// ============================================================================
//
// `pull_from` / `push_to` are the canonical CURVE-wired
// factory entry points: discrete `identity_key_name` (KeyStore lookup)
// + `Z85PublicKey server_pubkey` (PULL only) + `zap_domain` (PUSH
// only).  No `ZmqAuthOptions` struct, no `initial_allowlist` (callers
// seed via `set_peer_allowlist()` post-`start()`; production callers
// rely on the broker's Phase D `CHANNEL_AUTH_UPDATE` push, the
// deny-all default is the safe starting point).
//
// H-Q1 safety: `ZmqQueue` is `final`, so the `static_cast` from the
// abstract QueueReader/QueueWriter return type of `pull_from` /
// `push_to` to the concrete `ZmqQueue` is sound (the legacy plaintext
// factories ALWAYS construct the most-derived type).  Asserted at
// compile time.
// H-Q2 ordering: the plaintext factory must NOT call `start()` on
// the queue before we populate the CURVE fields — otherwise the
// socket would bind/connect without auth and our later
// auth-field writes would land on a running socket too late.

std::unique_ptr<ZmqQueue>
ZmqQueue::pull_from(const std::string& endpoint,
                          ::pylabhub::utils::security::Z85PublicKey server_pubkey,
                          std::vector<ZmqSchemaField> schema,
                          std::string packing,
                          std::string_view identity_key_name,
                          bool bind,
                          size_t max_buffer_depth,
                          std::optional<std::array<uint8_t, 8>> schema_tag,
                          std::string instance_id)
{
    // Z85PublicKey empty-sentinel handling: a default-constructed
    // `Z85PublicKey` is 40 zero bytes — a sentinel meaning "no
    // pubkey set" that MUST NOT reach libzmq.  Convert to an empty
    // string for the validator so its empty-check fires the
    // documented diagnostic.  Do NOT use `server_pubkey.str()`
    // unconditionally — that would pass 40 zero bytes through and
    // libzmq would silently accept them.
    const std::string server_pubkey_str =
        server_pubkey.empty() ? std::string{} : std::string{server_pubkey.str()};

    if (auto err = validate_curve_factory_params(
            identity_key_name, server_pubkey_str, /*bind_side=*/bind);
        !err.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue::pull_from] invalid auth "
                     "params for '{}': {}", endpoint, err);
        return nullptr;
    }

    static_assert(std::is_final_v<ZmqQueue>,
                  "ZmqQueue must be final for the post-C4 CURVE factories' "
                  "static_cast to be sound — otherwise the cast may "
                  "silently truncate a most-derived subclass instance.");
    auto reader = build_plaintext_reader_(endpoint, std::move(schema), std::move(packing),
                             bind, max_buffer_depth, schema_tag,
                             std::move(instance_id));
    if (!reader) return nullptr;
    std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(reader.release()));
    assert(!z->is_running() &&
           "pull_from: plaintext factory must not start() the "
           "queue before auth fields are populated; ordering invariant "
           "broken — silent plaintext-fallback risk");

    z->pImpl->identity_key_name_ = std::string{identity_key_name};
    z->pImpl->server_pubkey_z85_ = server_pubkey_str;
    return z;
}

std::unique_ptr<ZmqQueue>
ZmqQueue::push_to(const std::string& endpoint,
                        std::vector<ZmqSchemaField> schema,
                        std::string packing,
                        std::string_view identity_key_name,
                        std::string zap_domain,
                        bool bind,
                        std::optional<std::array<uint8_t, 8>> schema_tag,
                        int sndhwm,
                        size_t send_buffer_depth,
                        OverflowPolicy overflow_policy,
                        int send_retry_interval_ms,
                        std::string instance_id)
{
    if (auto err = validate_curve_factory_params(
            identity_key_name, /*server_pubkey_z85=*/{},
            /*bind_side=*/bind);
        !err.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue::push_to] invalid auth "
                     "params for '{}': {}", endpoint, err);
        return nullptr;
    }

    static_assert(std::is_final_v<ZmqQueue>,
                  "ZmqQueue must be final for the post-C4 CURVE factories' "
                  "static_cast to be sound — otherwise the cast may "
                  "silently truncate a most-derived subclass instance.");
    auto writer = build_plaintext_writer_(endpoint, std::move(schema), std::move(packing),
                           bind, schema_tag, sndhwm, send_buffer_depth,
                           overflow_policy, send_retry_interval_ms,
                           std::move(instance_id));
    if (!writer) return nullptr;
    std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(writer.release()));
    assert(!z->is_running() &&
           "push_to: plaintext factory must not start() the "
           "queue before auth fields are populated; ordering invariant "
           "broken — silent plaintext-fallback risk");

    z->pImpl->identity_key_name_ = std::string{identity_key_name};
    z->pImpl->zap_domain_        = std::move(zap_domain);
    // initial allowlist intentionally NOT seeded — caller invokes
    // `set_peer_allowlist()` AFTER `start()`.  Empty == deny-all
    // secure default.
    return z;
}

// ============================================================================
// PeerAdmission overrides (Phase A interface)
// ============================================================================

bool ZmqQueue::set_peer_allowlist(
    pylabhub::utils::security::PeerAllowlist allowlist)
{
    if (!pImpl) return false;
    // Only the PUSH/bind side has an allowlist concept.  Refusing on
    // the PULL side surfaces a caller misuse (broker should not push
    // allowlists to the consumer queue — the consumer trusts the
    // server via curve_serverkey).
    if (pImpl->mode != ZmqQueueImpl::Mode::Write || !pImpl->bind_socket)
        return false;
    pImpl->allowlist_.store(
        std::make_shared<const pylabhub::utils::security::PeerAllowlist>(
            std::move(allowlist)),
        std::memory_order_release);
    return true;
}

std::optional<pylabhub::utils::security::PeerAllowlist>
ZmqQueue::peer_allowlist_snapshot() const
{
    if (!pImpl) return std::nullopt;
    if (pImpl->mode != ZmqQueueImpl::Mode::Write || !pImpl->bind_socket)
        return std::nullopt;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap) return std::nullopt;
    return *snap;
}

bool ZmqQueue::is_peer_allowed(
    const pylabhub::utils::security::PeerIdentity& peer) const
{
    if (!pImpl) return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Write || !pImpl->bind_socket)
        return false;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap) return false;
    return snap->contains(peer);
}

// ── Dynamic producer-peer membership (HEP-CORE-0017 §3.3, #103 A2) ──────────

bool ZmqQueue::add_producer_peer(const ProducerPeer& peer)
{
    if (!pImpl) return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    for (auto& existing : pImpl->producer_peers_)
    {
        if (existing.role_uid == peer.role_uid)
        {
            existing = peer;
            return true;
        }
    }
    pImpl->producer_peers_.push_back(peer);
    return true;
}

bool ZmqQueue::remove_producer_peer(const std::string& role_uid)
{
    if (!pImpl) return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    auto it = std::find_if(
        pImpl->producer_peers_.begin(), pImpl->producer_peers_.end(),
        [&](const ProducerPeer& p) { return p.role_uid == role_uid; });
    if (it == pImpl->producer_peers_.end())
        return false;
    pImpl->producer_peers_.erase(it);
    return true;
}

std::size_t ZmqQueue::producer_peer_count() const noexcept
{
    if (!pImpl) return 0;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read) return 0;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    return pImpl->producer_peers_.size();
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

        // ── PeerAdmission Phase C — CURVE + ZAP wiring (HEP-CORE-0036 §6) ────
        // Empty identity_key_name_ == legacy unauth path (reachable
        // only via plaintext `pull_from`/`push_to` factories which
        // never populate it).  HEP-CORE-0040 §172: keys are sourced
        // from `key_store()` by name — secret bytes flow from
        // LockedKey region directly into libzmq's internal CURVE
        // state inside `with_seckey` callback scope; no std::string
        // copy holds the seckey at queue scope.
        if (!pImpl->identity_key_name_.empty())
        {
            namespace sec = pylabhub::utils::security;
            auto &ks = sec::key_store();

            pImpl->socket.set(zmq::sockopt::curve_publickey,
                              ks.pubkey(pImpl->identity_key_name_));
            ks.with_seckey(pImpl->identity_key_name_,
                [&](std::string_view seckey)
                {
                    pImpl->socket.set(zmq::sockopt::curve_secretkey,
                                      seckey);
                });

            if (pImpl->bind_socket)
            {
                // Server side: CURVE_SERVER + zap_domain + register with ZapRouter.
                pImpl->socket.set(zmq::sockopt::curve_server, 1);

                // Resolve zap_domain.  Use explicit value if provided;
                // otherwise derive from instance_id or queue_name+addr
                // (same key the ThreadManager owner_id uses below).
                pImpl->resolved_zap_domain_ = pImpl->zap_domain_;
                if (pImpl->resolved_zap_domain_.empty())
                {
                    if (!pImpl->instance_id.empty())
                        pImpl->resolved_zap_domain_ = pImpl->instance_id;
                    else
                    {
                        char addr_buf[64];
                        std::snprintf(addr_buf, sizeof(addr_buf),
                                      "%s@%p", pImpl->queue_name.c_str(),
                                      static_cast<void *>(pImpl.get()));
                        pImpl->resolved_zap_domain_ = addr_buf;
                    }
                }
                pImpl->socket.set(zmq::sockopt::zap_domain,
                                  pImpl->resolved_zap_domain_);

                // HEP-CORE-0040 §8.4 (#158): seed an EMPTY allowlist
                // (deny-all secure default).  Storing an empty
                // PeerAllowlist (rather than leaving the atomic
                // nullptr) keeps `peer_allowlist_snapshot()` non-nullopt
                // on a started CURVE-wired queue — the in-memory
                // contract is "CURVE wired → allowlist exists; admit
                // ⊆ peers; empty peers ⇒ deny all".  Callers replace
                // this via `set_peer_allowlist()` (in production,
                // driven by the broker's Phase D
                // `CHANNEL_AUTH_UPDATE` push).
                pImpl->allowlist_.store(
                    std::make_shared<const sec::PeerAllowlist>(),
                    std::memory_order_release);

                // Register with the router BEFORE bind.  Without this
                // ordering, an early peer connect could submit a ZAP
                // request that lands on an unregistered domain and
                // gets denied even though admission is configured.
                pImpl->zap_handle_.emplace(
                    sec::ZapRouter::instance().register_domain(
                        pImpl->resolved_zap_domain_, this));
            }
            else
            {
                // Client side: present serverkey.  Non-empty is
                // guaranteed by `pull_from`'s validator — the
                // factory is the only path to a constructed
                // ZmqQueue (the ctor is private), so a private-API
                // bypass cannot occur.
                pImpl->socket.set(zmq::sockopt::curve_serverkey,
                                  pImpl->server_pubkey_z85_);
            }
        }

        if (pImpl->bind_socket)
            pImpl->socket.bind(pImpl->endpoint);
        else
            pImpl->socket.connect(pImpl->endpoint);

        // ── CURVE engagement guard (HEP-CORE-0035 §2 + #161 C5) ─────────
        // After all CURVE setsockopts and bind/connect have completed,
        // ask libzmq directly what mechanism this socket negotiated.
        // The whole C-chain exists to make CURVE unconditional on
        // every role↔hub data path; if libzmq reports anything other
        // than CURVE we have a wiring regression — fail the start
        // here so no data ever flows on a non-CURVE socket.  The
        // observable `mechanism_` field stays Uninitialized on
        // failure so callers see the bad state via `mechanism()`.
        const int mech = pImpl->socket.get(zmq::sockopt::mechanism);
        if (mech != ZMQ_CURVE)
        {
            // Do NOT publish a transient `Plaintext` value before the
            // throw — the catch handler will store `Uninitialized` and
            // any value visible to a concurrent `mechanism()` reader
            // between the two stores would contradict the enum
            // contract that documents `Plaintext` as "unreachable
            // post-C4".  Skipping the transient keeps the observable
            // a clean two-state machine (Uninitialized → Curve), with
            // the throw + catch transitioning back to Uninitialized
            // on failure.
            throw std::invalid_argument(
                "[hub::ZmqQueue::start] libzmq reported mechanism=" +
                std::to_string(mech) + " (expected ZMQ_CURVE=" +
                std::to_string(ZMQ_CURVE) + ") — CURVE wiring "
                "regression (HEP-CORE-0035 §2 invariant violated; "
                "see ZmqQueue::mechanism() / Mechanism enum)");
        }
        pImpl->mechanism_.store(Mechanism::Curve,
                                std::memory_order_release);
    }
    catch (const std::invalid_argument &e)
    {
        // H-Q3: specific, caller-actionable diagnostic for auth
        // misconfiguration that slipped past factory validation OR
        // (post-#161) the CURVE engagement guard at the bottom of
        // the try block.  The queue did not start; reset the
        // observable mechanism so `mechanism()` reflects the closed
        // state.
        pImpl->socket.close();
        pImpl->mechanism_.store(Mechanism::Uninitialized,
                                std::memory_order_release);
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] auth setup failed for '{}': {}",
                     pImpl->endpoint, e.what());
        return false;
    }
    catch (const zmq::error_t &e)
    {
        pImpl->socket.close();
        pImpl->mechanism_.store(Mechanism::Uninitialized,
                                std::memory_order_release);
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
            [impl_ptr](pylabhub::utils::ThreadManager::SlotContext &ctx) {
                ctx.with_active_loop([impl_ptr, &ctx] {
                    impl_ptr->run_recv_thread_(ctx);
                });
            });
    }
    else // Write
    {
        pImpl->send_stop_.store(false, std::memory_order_release);
        pImpl->thread_mgr_->spawn("send",
            [impl_ptr](pylabhub::utils::ThreadManager::SlotContext &ctx) {
                ctx.with_active_loop([impl_ptr, &ctx] {
                    impl_ptr->run_send_thread_(ctx);
                });
            });
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

    // Explicit drain (NOT via ~ThreadManager) so we can observe the
    // detach count and apply the grace gate BEFORE destroying state
    // the runaway thread still touches.  Mirrors EngineHost::shutdown_()
    // — see HEP-CORE-0031 §4.2 detach-safety gate.
    if (pImpl->thread_mgr_)
    {
        const auto detached = pImpl->thread_mgr_->drain();
        if (detached > 0)
        {
            // Grace-poll for the runaway thread.  Its body still holds
            // `impl_ptr` (raw) and may still touch ZmqQueueImpl members
            // (ring buffers, cv, socket).  We must keep pImpl alive
            // until the thread returns — `~ZmqQueue` calls this stop()
            // before destroying pImpl, so we honor the gate here.
            constexpr auto kGrace = std::chrono::seconds{5};
            const auto deadline   =
                std::chrono::steady_clock::now() + kGrace;
            while (!pImpl->thread_mgr_->all_detached_done() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            if (!pImpl->thread_mgr_->all_detached_done())
            {
                // Grace expired.  Subsequent ~ZmqQueueImpl will UAF the
                // runaway thread; we surface it so the operator sees it
                // in the log.  No safe recovery path here — ZmqQueue's
                // pImpl is a unique_ptr member, we can't leak it.
                LOGGER_ERROR(
                    "[hub::ZmqQueue:{}] {} thread(s) detached on stop() "
                    "AND still running after {}s grace.  Subsequent "
                    "~ZmqQueueImpl will UAF the runaway thread; if this "
                    "fires, the thread body is stuck in a libzmq op "
                    "that ignored the stop flag.",
                    pImpl->queue_name, detached, kGrace.count());
            }
        }
        pImpl->thread_mgr_.reset();
    }

    // Release the ZAP registration BEFORE closing the socket: the
    // RAII handle decrements ZapRouter's ref-count and removes the
    // domain from the routing map.  After this, no more handshakes
    // route to `this`.
    if (pImpl->zap_handle_.has_value())
    {
        pImpl->zap_handle_.reset();
        pImpl->resolved_zap_domain_.clear();
    }

    // Close the socket AFTER threads have exited (threads were the only user).
    // The shared ZMQ context is owned by the ZMQContext lifecycle module —
    // ZmqQueue never terminates it.
    pImpl->socket.close();

    // Reset the observable mechanism — socket is gone, no negotiated
    // mechanism remains.  `mechanism()` will report `Uninitialized`
    // until the next successful `start()`.
    pImpl->mechanism_.store(Mechanism::Uninitialized,
                            std::memory_order_release);
}

bool ZmqQueue::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

Mechanism ZmqQueue::mechanism() const noexcept
{
    if (!pImpl) return Mechanism::Uninitialized;
    return pImpl->mechanism_.load(std::memory_order_acquire);
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
