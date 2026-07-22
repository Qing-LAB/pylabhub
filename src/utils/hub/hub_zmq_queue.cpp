// src/utils/hub/hub_zmq_queue.cpp
/**
 * @file hub_zmq_queue.cpp
 * @brief ZmqQueue implementation — ZMQ-backed Queue with MessagePack schema framing.
 *        Supports PULL/PUSH (default) and PUB/SUB (fan-out topology,
 *        HEP-CORE-0017 §3.3.0); socket type is selected from (mode ×
 *        socket_pattern) at start().
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N),
 * checksum:bin32] payload element i: scalar → native msgpack type; array/string/bytes →
 * bin(byte_size) checksum: BLAKE2b-256 of the raw (pre-pack) slot data
 */
#include "utils/hub_zmq_queue.hpp"
#include "utils/context_metrics.hpp"
#include "utils/logger.hpp"
#include "utils/loop_timing_policy.hpp" // kBrokerReadinessPollInterval (finalize_connect)

#include <nlohmann/json.hpp>
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
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) — field order kept for clarity; reorder
// in a dedicated layout pass if desired
struct ZmqQueueImpl
{
    enum class Mode
    {
        Read,
        Write
    } mode;

    /// Socket pattern.  `PushPull` is the pre-2026-07-08 default and
    /// remains the shape produced by `pull_from` / `push_to` /
    /// `create_reader(FanIn|OneToOne)` / `create_writer(FanIn|OneToOne)`.
    /// `PubSub` is added for fan-out ZMQ (HEP-CORE-0017 §3.3.0):
    /// producer binds PUB, consumer connects SUB (subscribes to empty
    /// topic).  The pattern is fixed at construction time and drives
    /// libzmq socket-type selection at `start()`.
    enum class SocketPattern
    {
        PushPull,
        PubSub
    } socket_pattern{SocketPattern::PushPull};

    std::string endpoint;
    std::string actual_endpoint; ///< Resolved after zmq_bind() — e.g. port 0 → actual port.
    bool bind_socket{false};
    /// HEP-CORE-0036 §6.6.3 — set to true by `apply_master_approval`
    /// when the queue is a fan-in DIALING PUSH (write side + not
    /// bind_socket).  Signals that `apply_master_approval` deliberately
    /// deferred the `socket.connect()` to `finalize_connect()`, which
    /// the role host invokes uniformly after apply for every role /
    /// topology (HEP-CORE-0036 §I9.1 locality invariant).  Reset to
    /// false once `finalize_connect()` completes.
    bool dial_pending{false};
    size_t item_sz{0};
    size_t max_depth{64};
    std::string queue_name;
    /// ZMQ_SNDHWM for the write-side socket (0 = ZMQ default 1000).
    /// Semantic differs by socket type:
    /// - PUSH: peer-side pushback — send() blocks (or returns EAGAIN)
    ///   when the queue toward any peer is full.
    /// - PUB : per-subscriber drop — libzmq silently discards frames
    ///   for a subscriber whose queue is full.  Drops are NOT visible
    ///   via `send_drop_count_` (rev 2.4 wires ZMQ_EVENT_HWM_DROP).
    int sndhwm{0};

    /// Caller-supplied stable identifier — used as ThreadManager owner_id so the
    /// lifecycle module name is unique across overlapping queue instances (e.g.
    /// port-0 binds where queue_name collides). Empty → start() falls back to
    /// a pointer-address-based id.
    std::string instance_id;

    // ── Schema tag (optional — 8 bytes of BLAKE2b-256 of BLDS) ───────────────
    std::array<uint8_t, 8> schema_tag_{};
    bool has_schema_tag_{false};

    // ── Schema-based field encoding ───────────────────────────────────────────
    std::vector<wire_detail::WireFieldDesc> schema_defs_;
    size_t max_frame_sz_{0}; ///< recv frame buffer size

    // Context is the shared process-wide zmq::context_t owned by the
    // ZMQContext lifecycle module (see utils/zmq_context.hpp). The
    // top-level LifecycleGuard must include GetZMQContextModule(); the
    // persistent flag guarantees the context outlives every ZmqQueue
    // instance. No per-queue context — prevents the racing zmq_ctx_term()
    // vs still-running send/recv threads that caused SIGABRT under
    // parallel test load.
    zmq::socket_t socket; ///< default-constructed empty; assigned in start()

    // ── Read mode — pre-allocated ring buffer [ZQ9] ──────────────────────────
    std::atomic<bool> recv_stop_{false};
    // Threads live inside a ThreadManager owned by the queue. Recreated
    // per start()/stop() cycle so each active window has its own dynamic
    // lifecycle module "ThreadManager:ZmqQueue:<name>".
    std::unique_ptr<pylabhub::utils::ThreadManager> thread_mgr_;

    std::vector<std::vector<std::byte>> recv_ring_; ///< [max_depth] pre-allocated slots
    size_t ring_head_{0};                           ///< index of oldest item
    size_t ring_tail_{0};                           ///< index of next write slot
    size_t ring_count_{0};                          ///< items currently in ring
    std::mutex recv_mu_;
    std::condition_variable recv_cv_;

    std::vector<std::byte> decode_tmp_;       ///< recv-thread-only staging buffer
    std::vector<std::byte> current_read_buf_; ///< returned by read_acquire()

    // ── Write mode — internal send buffer + send_thread_ ─────────────────────
    // The caller writes into write_buf_ (returned by write_acquire()).
    // write_commit() copies write_buf_ into send_ring_[send_tail_] and signals
    // send_thread_.  send_thread_ drains the ring: packs msgpack frames and
    // calls zmq_send, retrying on EAGAIN.  This decouples the caller from the
    // ZMQ send latency and provides configurable back-pressure.
    std::vector<std::byte> write_buf_; ///< caller-visible write buffer (item_sz bytes)
    size_t send_depth_{64};
    OverflowPolicy overflow_policy_{OverflowPolicy::Drop};
    int send_retry_interval_ms_{10};

    std::vector<std::vector<std::byte>>
        send_ring_;        ///< pre-allocated send ring (send_depth_ × item_sz)
    size_t send_head_{0};  ///< oldest filled slot (send_thread_ reads here)
    size_t send_tail_{0};  ///< next write slot (write_commit() writes here)
    size_t send_count_{0}; ///< filled slots waiting to be sent
    std::mutex send_mu_;
    std::condition_variable send_cv_;
    std::atomic<bool> send_stop_{false};
    // send thread lives in thread_mgr_ (declared above in the Read-mode
    // section); spawn() is called from ZmqQueue::start() based on mode.
    std::vector<std::byte> send_local_buf_; ///< send_thread_-private staging (item_sz bytes)
    msgpack::sbuffer send_sbuf_;            ///< send_thread_-private msgpack buffer (reused)
    std::atomic<uint64_t> send_seq_{0};

    // ── Counters ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> recv_overflow_count_{0};
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0}; ///< [ZQ10] sequence gaps
    std::atomic<uint64_t> send_drop_count_{0};
    std::atomic<uint64_t> send_retry_count_{0};
    std::atomic<uint64_t> data_drop_count_{0};

    // ── CURVE auth state (HEP-CORE-0036 §7 + HEP-CORE-0040 §8.4) ──────────────
    // Identity key name (KeyStore lookup key, HEP-CORE-0040 §172) +
    // optional zap_domain (PUSH side) + server pubkey (PULL/connect
    // side).  When `identity_key_name_` is set, `ZmqQueue::start()`
    // reads the identity from `secure().keys()` at socket-setup time —
    // secret bytes never materialize in this struct.
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

    // Resolved zap_domain (HEP-CORE-0036 §7): `zap_domain_` if non-empty,
    // else derived from instance_id at start().  Captured here so
    // stop() can release the ZapRouter registration symmetrically.
    std::string resolved_zap_domain_;

    // PUSH/bind side allowlist.  Lock-free reads from ZapRouter's
    // pump thread via PortableAtomicSharedPtr.  Mutations through
    // ZmqQueue::set_peer_allowlist are atomic snapshots (HEP-CORE-0036 §I3
    // contract).  Unused on PULL/connect side.
    pylabhub::utils::detail::PortableAtomicSharedPtr<const pylabhub::utils::security::PeerAllowlist>
        allowlist_;

    // PUSH/bind side ZAP registration handle.  Active iff CURVE was
    // wired AND this is the server side; released in stop().
    std::optional<pylabhub::utils::security::ZapDomainHandle> zap_handle_;

    // PULL/connect side producer-peer membership (HEP-CORE-0017 §3.3,
    // #103 A2 + HEP-CORE-0036 §6.7 Standby state #188).  Two roles:
    //   (a) Multi-endpoint connect target (HEP-CORE-0017 §3.3
    //       Pattern B, closed 2026-07-08).  `start()` iterates
    //       `producer_peers_` and issues per-peer `connect()` calls
    //       with per-peer `curve_serverkey`, so libzmq's PULL
    //       fair-queues data from all N connected PUSH peers.  Under
    //       the canonical HEP-CORE-0036 §6.4 flow, `apply_master_approval
    //       (CONSUMER_REG_ACK)` seeds this vector from `producers[]`
    //       and drives Standby → Configured → Active.  `apply_master_
    //       approval` also promotes peer[0] into `endpoint` +
    //       `server_pubkey_z85_` as the `is_configured()` flag saying
    //       "apply_master_approval has run" — bare `set_producer_peers`
    //       must NOT transition the queue per HEP-0036 §6.7 Option B.
    //   (b) Membership snapshot.  Dispatch layer on
    //       `CHANNEL_PRODUCERS_CHANGED_NOTIFY` →
    //       `GET_CHANNEL_PRODUCERS_ACK` (HEP-CORE-0036 §6.5.1 —
    //       consumer-side equivalent of the producer-side §6.5
    //       allowlist family) refreshes the list via
    //       `set_producer_peers` on the Active PULL queue, so the
    //       queue has a stable record of who's authorized.  Initial
    //       seed: `apply_master_approval(CONSUMER_REG_ACK)` extracts
    //       `ACK.producers[]` (§6.4) at S3 and drives Standby →
    //       Configured → Active in one polymorphic call.
    // Mutex-guarded since both broker thread and role thread may
    // touch.  Locked during `set_producer_peers` snapshot replace
    // and per-peer add/remove; `start()` reads `server_pubkey_z85_`
    // + `endpoint` without taking this mutex because the state
    // machine guarantees `set_*` mutators don't race with `start()`
    // (sequenced by the role host per §I12).
    std::mutex producer_peers_mu_;
    std::vector<ProducerPeer> producer_peers_;

    // ── Domain 2+3 timing (HEP-CORE-0008 §10) ──────────────────────────────
    // Measured in read_acquire/read_release (read mode) or
    // write_acquire/write_commit (write mode). Same semantics as DataBlock ContextMetrics.
    ContextMetrics ctx_metrics_; ///< Unified timing + checksum metrics.
    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced}; ///< Default: auto checksum.

    using Clock = ContextMetrics::Clock;

    // Per-thread timestamps (not atomic — only accessed from caller thread).
    Clock::time_point t_iter_start_{}; ///< Start of previous acquire (for iteration gap).
    Clock::time_point t_acquired_{};   ///< When last acquire returned (for work time).

    // ── Sequence tracking [ZQ10] ─────────────────────────────────────────────
    uint64_t expected_seq_{0};
    bool seq_initialized_{false};
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
        socket.set(zmq::sockopt::rcvtimeo, 100); // 100ms poll tick for stop-flag checks

        zmq::message_t msg;

        while (!recv_stop_.load(std::memory_order_relaxed) && !ctx.shutdown_requested())
        {
            if (!socket)
                break;

            zmq::recv_result_t rr;
            try
            {
                rr = socket.recv(msg, zmq::recv_flags::none);
            }
            catch (const zmq::error_t &e)
            {
                // ETERM on context shutdown → clean exit; EINTR on signal → retry.
                if (e.num() == ETERM)
                    break;
                if (e.num() == EINTR)
                    continue;
                LOGGER_WARN("[hub::ZmqQueue] recv error on '{}': {}", queue_name, e.what());
                break;
            }
            if (!rr.has_value())
                continue; // RCVTIMEO expired; re-check stop flag

            // max_frame_sz_ has a 4-byte slack above any valid schema frame, so
            // size >= max_frame_sz_ indicates a malformed or oversized frame. [ZQ4]
            if (msg.size() >= max_frame_sz_)
            {
                ++recv_frame_error_count_;
                continue;
            }

            try
            {
                msgpack::object_handle oh =
                    msgpack::unpack(static_cast<const char *>(msg.data()), msg.size());

                auto env = wire_detail::unpack_envelope(oh.get());
                if (!env.valid || env.payload_size != schema_defs_.size())
                {
                    ++recv_frame_error_count_;
                    continue;
                }

                // Schema-tag mismatch (rate-limited warning). [ZQ6]
                if (has_schema_tag_ && std::memcmp(env.recv_tag, schema_tag_.data(), 8) != 0)
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
                    expected_seq_ = env.seq + 1;
                    seq_initialized_ = true;
                    last_seq_.store(env.seq, std::memory_order_relaxed);
                }

                // Decode payload fields into decode_tmp_. [ZQ9]
                std::fill(decode_tmp_.begin(), decode_tmp_.end(), std::byte{0});
                if (!wire_detail::unpack_payload(*env.payload, schema_defs_, decode_tmp_.data()))
                {
                    ++recv_frame_error_count_;
                    continue;
                }

                // Checksum verification.
                if (checksum_policy_ != ChecksumPolicy::None)
                {
                    if (!pylabhub::utils::security::secure().verify_blake2b(
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
            catch (const std::exception &e)
            {
                ++recv_frame_error_count_;
                LOGGER_WARN("[hub::ZmqQueue] unpack error on '{}': {}", queue_name, e.what());
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
                send_cv_.wait(lk,
                              [this, &ctx]
                              {
                                  return send_count_ > 0 ||
                                         send_stop_.load(std::memory_order_relaxed) ||
                                         ctx.shutdown_requested();
                              });
                if (send_count_ == 0)
                    break; // stop_ + empty → exit

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
                    pylabhub::utils::security::secure().compute_blake2b(
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
                    auto sr = socket.send(zmq::const_buffer(send_sbuf_.data(), send_sbuf_.size()),
                                          zmq::send_flags::dontwait);
                    if (sr.has_value())
                        break; // sent

                    // EAGAIN — send ring full
                    if (send_stop_.load(std::memory_order_relaxed))
                    {
                        ++send_drop_count_;
                        break;
                    }
                    ++send_retry_count_;
                    std::this_thread::sleep_for(std::chrono::milliseconds(send_retry_interval_ms_));
                    continue;
                }
                catch (const zmq::error_t &e)
                {
                    if (e.num() != ETERM)
                        LOGGER_WARN("[hub::ZmqQueue] send error on '{}': {}", queue_name, e.what());
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

/// Return the libzmq socket-type label ("PULL"/"PUSH"/"SUB"/"PUB")
/// derived from the impl's (mode × pattern) — used in state-transition
/// and start-refusal log lines so log consumers can tell fan-out
/// (PUB/SUB) from fan-in / one-to-one (PUSH/PULL) at a glance.
static constexpr const char *socket_type_label(ZmqQueueImpl::Mode mode,
                                               ZmqQueueImpl::SocketPattern pattern) noexcept
{
    if (pattern == ZmqQueueImpl::SocketPattern::PubSub)
        return mode == ZmqQueueImpl::Mode::Read ? "SUB" : "PUB";
    return mode == ZmqQueueImpl::Mode::Read ? "PULL" : "PUSH";
}

// ============================================================================
// Schema layout computation (file-scope helpers)
// ============================================================================

/// Validate all type_str values in a schema.  Returns the first invalid type_str,
/// or an empty string if all are valid.  [ZQ1]
static std::string find_invalid_type(const std::vector<ZmqSchemaField> &schema)
{
    for (const auto &f : schema)
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
ZmqQueue::build_plaintext_reader_(const std::string &endpoint, std::vector<ZmqSchemaField> schema,
                                  std::string packing, bool bind, size_t max_buffer_depth,
                                  std::optional<std::array<uint8_t, 8>> schema_tag,
                                  std::string instance_id, bool is_pubsub)
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
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': invalid packing '{}' (must be \"aligned\" or "
                     "\"packed\")",
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
    for (const auto &f : schema)
    {
        if ((f.type_str == "string" || f.type_str == "bytes") && f.length == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': string/bytes field has length=0",
                         endpoint);
            return nullptr;
        }
        if (f.type_str != "string" && f.type_str != "bytes" && f.count == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': numeric/array field count must be >= 1",
                         endpoint);
            return nullptr;
        }
    }
    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl = std::make_unique<ZmqQueueImpl>();
    impl->mode = ZmqQueueImpl::Mode::Read;
    impl->socket_pattern =
        is_pubsub ? ZmqQueueImpl::SocketPattern::PubSub : ZmqQueueImpl::SocketPattern::PushPull;
    impl->endpoint = endpoint;
    impl->bind_socket = bind;
    impl->item_sz = item_sz;
    impl->max_depth = max_buffer_depth;
    impl->queue_name = endpoint;
    impl->instance_id = std::move(instance_id);
    impl->max_frame_sz_ = wire_detail::max_frame_size(layouts);
    impl->schema_defs_ = std::move(layouts);
    if (schema_tag)
    {
        impl->schema_tag_ = *schema_tag;
        impl->has_schema_tag_ = true;
    }

    // Pre-allocate ring buffer (max_depth slots) and decode staging buffer. [ZQ9]
    impl->recv_ring_.assign(max_buffer_depth, std::vector<std::byte>(item_sz, std::byte{0}));
    impl->decode_tmp_.resize(item_sz, std::byte{0});
    impl->current_read_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<QueueReader>(new ZmqQueue(std::move(impl)));
}

std::unique_ptr<QueueWriter> ZmqQueue::build_plaintext_writer_(
    const std::string &endpoint, std::vector<ZmqSchemaField> schema, std::string packing, bool bind,
    std::optional<std::array<uint8_t, 8>> schema_tag, int sndhwm, size_t send_buffer_depth,
    OverflowPolicy overflow_policy, int send_retry_interval_ms, std::string instance_id,
    bool is_pubsub)
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
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': invalid packing '{}' (must be \"aligned\" or "
                     "\"packed\")",
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
    for (const auto &f : schema)
    {
        if ((f.type_str == "string" || f.type_str == "bytes") && f.length == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': string/bytes field has length=0", endpoint);
            return nullptr;
        }
        if (f.type_str != "string" && f.type_str != "bytes" && f.count == 0)
        {
            LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': numeric/array field count must be >= 1",
                         endpoint);
            return nullptr;
        }
    }
    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl = std::make_unique<ZmqQueueImpl>();
    impl->mode = ZmqQueueImpl::Mode::Write;
    impl->socket_pattern =
        is_pubsub ? ZmqQueueImpl::SocketPattern::PubSub : ZmqQueueImpl::SocketPattern::PushPull;
    impl->endpoint = endpoint;
    impl->bind_socket = bind;
    impl->item_sz = item_sz;
    impl->queue_name = endpoint;
    impl->instance_id = std::move(instance_id);
    impl->sndhwm = sndhwm; // [ZQ8]
    impl->send_depth_ = send_buffer_depth;
    impl->overflow_policy_ = overflow_policy;
    impl->send_retry_interval_ms_ = send_retry_interval_ms;
    impl->max_frame_sz_ = wire_detail::max_frame_size(layouts);
    impl->schema_defs_ = std::move(layouts);
    if (schema_tag)
    {
        impl->schema_tag_ = *schema_tag;
        impl->has_schema_tag_ = true;
    }

    // Pre-allocate caller write buffer, send ring, and send_thread_-private buffer.
    impl->write_buf_.resize(item_sz, std::byte{0});
    impl->send_ring_.assign(send_buffer_depth, std::vector<std::byte>(item_sz, std::byte{0}));
    impl->send_local_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<QueueWriter>(new ZmqQueue(std::move(impl)));
}

// ============================================================================
// Auth-enabled factories (PeerAdmission (HEP-CORE-0036 §7))
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
/// prior call.  Used by both `pull_from` (PULL/connect)
/// and `push_to` (PUSH/bind).
std::string validate_curve_factory_params(std::string_view identity_key_name,
                                          std::string_view server_pubkey_z85, bool bind_side)
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
        if (!sec::sodium_ready())
            return "keystore_name='" + name_str +
                   "' but KeyStore is not initialized (process must "
                   "construct SecureSubsystem + KeyStore before "
                   "any CURVE-wired queue is built)";
        auto &ks = sec::secure().keys();
        if (!ks.has(name_str))
            return "keystore_name='" + name_str +
                   "' not present in KeyStore (caller must "
                   "`add_identity(name, ...)` BEFORE building the "
                   "queue)";
        const auto pub = ks.pubkey(name_str);
        if (pub.size() != kCurveKeyZ85Chars)
            return "KeyStore entry '" + name_str + "' has pubkey of " + std::to_string(pub.size()) +
                   " chars; expected " + std::to_string(kCurveKeyZ85Chars);
    }

    // PULL/connect-side serverkey is REQUIRED before `start()`, but it
    // MAY be empty at factory time — HEP-CORE-0036 §6.7 Standby state
    // (Stage 1A, #188).  An empty serverkey at construction means
    // "queue held in Standby until role host calls
    // `set_producer_peers(ACK.producers[])`".  The `start()` gate
    // refuses non-Configured queues, so the empty serverkey can never
    // reach a `socket.connect(...)` call.
    //
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
// only).  No `initial_allowlist` parameter — production callers
// seed via `set_peer_allowlist()` after `apply_master_approval`
// runs (HEP-CORE-0036 §6.7 Option B at S3) or via the runtime
// notify-pull path on an Active queue: the role-host BRC handler
// pulls `GET_CHANNEL_AUTH_REQ` in response to
// `CHANNEL_AUTH_CHANGED_NOTIFY` (HEP-CORE-0036 §6.5 amendment
// 2026-06-04 — snapshot-push `CHANNEL_AUTH_UPDATE` retired).
// Task #103 (AUTH-1).  The deny-all default is the safe starting
// point until that first allowlist write lands.
//
// Final-class safety: `ZmqQueue` is `final`, so the `static_cast` from the
// abstract QueueReader/QueueWriter return type of `pull_from` /
// `push_to` to the concrete `ZmqQueue` is sound (the legacy plaintext
// factories ALWAYS construct the most-derived type).  Asserted at
// compile time.
// Ordering invariant: the plaintext factory must NOT call `start()` on
// the queue before we populate the CURVE fields — otherwise the
// socket would bind/connect without auth and our later
// auth-field writes would land on a running socket too late.

std::unique_ptr<ZmqQueue>
ZmqQueue::pull_from(const std::string &endpoint,
                    ::pylabhub::utils::security::Z85PublicKey server_pubkey,
                    std::vector<ZmqSchemaField> schema, std::string packing,
                    std::string_view identity_key_name, bool bind, size_t max_buffer_depth,
                    std::optional<std::array<uint8_t, 8>> schema_tag, std::string instance_id)
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

    if (auto err =
            validate_curve_factory_params(identity_key_name, server_pubkey_str, /*bind_side=*/bind);
        !err.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue::pull_from] invalid auth "
                     "params for '{}': {}",
                     endpoint, err);
        return nullptr;
    }

    static_assert(std::is_final_v<ZmqQueue>,
                  "ZmqQueue must be final for the post-C4 CURVE factories' "
                  "static_cast to be sound — otherwise the cast may "
                  "silently truncate a most-derived subclass instance.");
    auto reader = build_plaintext_reader_(endpoint, std::move(schema), std::move(packing), bind,
                                          max_buffer_depth, schema_tag, std::move(instance_id));
    if (!reader)
        return nullptr;
    std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(reader.release()));
    assert(!z->is_running() && "pull_from: plaintext factory must not start() the "
                               "queue before auth fields are populated; ordering invariant "
                               "broken — silent plaintext-fallback risk");

    z->pImpl->identity_key_name_ = std::string{identity_key_name};
    z->pImpl->server_pubkey_z85_ = server_pubkey_str;
    return z;
}

std::unique_ptr<ZmqQueue>
ZmqQueue::push_to(const std::string &endpoint, std::vector<ZmqSchemaField> schema,
                  std::string packing, std::string_view identity_key_name, std::string zap_domain,
                  bool bind, std::optional<std::array<uint8_t, 8>> schema_tag, int sndhwm,
                  size_t send_buffer_depth, OverflowPolicy overflow_policy,
                  int send_retry_interval_ms, std::string instance_id,
                  pylabhub::utils::security::Z85PublicKey server_pubkey)
{
    const std::string server_pubkey_str =
        server_pubkey.empty() ? std::string{} : std::string{server_pubkey.str()};
    if (auto err = validate_curve_factory_params(identity_key_name, server_pubkey_str,
                                                 /*bind_side=*/bind);
        !err.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue::push_to] invalid auth "
                     "params for '{}': {}",
                     endpoint, err);
        return nullptr;
    }

    static_assert(std::is_final_v<ZmqQueue>,
                  "ZmqQueue must be final for the post-C4 CURVE factories' "
                  "static_cast to be sound — otherwise the cast may "
                  "silently truncate a most-derived subclass instance.");
    auto writer = build_plaintext_writer_(endpoint, std::move(schema), std::move(packing), bind,
                                          schema_tag, sndhwm, send_buffer_depth, overflow_policy,
                                          send_retry_interval_ms, std::move(instance_id));
    if (!writer)
        return nullptr;
    std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(writer.release()));
    assert(!z->is_running() && "push_to: plaintext factory must not start() the "
                               "queue before auth fields are populated; ordering invariant "
                               "broken — silent plaintext-fallback risk");

    z->pImpl->identity_key_name_ = std::string{identity_key_name};
    z->pImpl->zap_domain_ = std::move(zap_domain);
    // Dialing-side serverkey — used by start() as curve_serverkey
    // when this queue is a PUSH-connect (fan-in producer).  Empty on
    // BINDING sides; `apply_master_approval(REG_ACK)` may populate
    // from initial_allowlist[0] on the wire path.
    z->pImpl->server_pubkey_z85_ = server_pubkey_str;
    // initial allowlist intentionally NOT seeded — caller invokes
    // `set_peer_allowlist()` AFTER `start()`.  Empty == deny-all
    // secure default.
    return z;
}

// ============================================================================
// Topology-parametric factories — HEP-CORE-0017 §3.3.0 (Phase C)
// ============================================================================
//
// Dispatch table (consumer side / producer side × topology):
//
//   Topology  | Consumer (create_reader)      | Producer (create_writer)
//   ----------+-------------------------------+-------------------------------
//   FanIn     | PULL bind (BINDING)           | PUSH connect (DIALING)
//   OneToOne  | PULL connect (DIALING)        | PUSH bind (BINDING)
//   FanOut    | SUB connect (DIALING)         | PUB bind (BINDING)
//
// PUSH/PULL uses the legacy `pull_from`/`push_to` factories.  PUB/SUB
// uses `build_plaintext_*_(is_pubsub=true)` directly, with the same
// CURVE + ZAP wiring as PUSH/PULL binding sides.

std::unique_ptr<ZmqQueue> ZmqQueue::create_reader(pylabhub::hub::ChannelTopology topology,
                                                  RxCreateOptions opts)
{
    using pylabhub::hub::ChannelTopology;
    switch (topology)
    {
    case ChannelTopology::FanIn:
        // Consumer BINDING side.  server_pubkey is ignored on the
        // binding side (curve_serverkey doesn't apply — the binding
        // side is the CURVE server, gates via ZAP allowlist).  Warn
        // when caller passes a non-empty pubkey — likely a
        // topology/side mixup that a silent drop would mask.
        if (!opts.server_pubkey.empty())
        {
            LOGGER_WARN("[hub::ZmqQueue::create_reader] fan-in consumer "
                        "(BINDING) ignores opts.server_pubkey for '{}'; "
                        "the binding side is the CURVE server. Drop or "
                        "check topology.",
                        opts.endpoint);
        }
        return pull_from(std::move(opts.endpoint),
                         /*server_pubkey=*/{}, std::move(opts.schema), std::move(opts.packing),
                         opts.identity_key_name,
                         /*bind=*/true, opts.max_buffer_depth, std::move(opts.schema_tag),
                         std::move(opts.instance_id));
    case ChannelTopology::OneToOne:
        // Consumer DIALING side.  server_pubkey is the producer's
        // identity pubkey; passed as curve_serverkey.
        return pull_from(std::move(opts.endpoint), std::move(opts.server_pubkey),
                         std::move(opts.schema), std::move(opts.packing), opts.identity_key_name,
                         /*bind=*/false, opts.max_buffer_depth, std::move(opts.schema_tag),
                         std::move(opts.instance_id));
    case ChannelTopology::FanOut:
    {
        // Consumer DIALING side, SUB connect.  Fan-out consumers must
        // provide the producer's identity pubkey as curve_serverkey
        // — same shape as OneToOne consumer.  SUB has no §6.7 Standby-
        // buffer story (the SUB is created only after the ACK carries
        // the peer's data_pubkey), so refuse an empty serverkey at
        // the factory rather than deferring to `start()`'s DEBUG
        // refusal.
        if (opts.server_pubkey.empty())
        {
            LOGGER_ERROR("[hub::ZmqQueue::create_reader] fan-out SUB requires "
                         "opts.server_pubkey (producer's identity pubkey); "
                         "empty rejected for '{}'.",
                         opts.endpoint);
            return nullptr;
        }
        const std::string server_pubkey_str{opts.server_pubkey.str()};
        if (auto err = validate_curve_factory_params(opts.identity_key_name, server_pubkey_str,
                                                     /*bind_side=*/false);
            !err.empty())
        {
            LOGGER_ERROR("[hub::ZmqQueue::create_reader] fan-out SUB: invalid "
                         "auth params for '{}': {}",
                         opts.endpoint, err);
            return nullptr;
        }
        auto reader =
            build_plaintext_reader_(opts.endpoint, std::move(opts.schema), std::move(opts.packing),
                                    /*bind=*/false, opts.max_buffer_depth,
                                    std::move(opts.schema_tag), std::move(opts.instance_id),
                                    /*is_pubsub=*/true);
        if (!reader)
            return nullptr;
        std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(reader.release()));
        assert(!z->is_running() && "create_reader(FanOut): plaintext factory must not "
                                   "start() the queue before auth fields are populated; "
                                   "ordering invariant broken — silent plaintext-fallback risk");
        z->pImpl->identity_key_name_ = std::string{opts.identity_key_name};
        z->pImpl->server_pubkey_z85_ = server_pubkey_str;
        return z;
    }
    }
    return nullptr; // unreachable under well-formed enum
}

std::unique_ptr<ZmqQueue> ZmqQueue::create_writer(pylabhub::hub::ChannelTopology topology,
                                                  TxCreateOptions opts)
{
    using pylabhub::hub::ChannelTopology;
    switch (topology)
    {
    case ChannelTopology::OneToOne:
        // Producer BINDING side.  PUSH bind.  server_pubkey is ignored
        // on the binding side (curve_serverkey doesn't apply — the
        // binding side is the CURVE server, gates via ZAP allowlist).
        if (!opts.server_pubkey.empty())
        {
            LOGGER_WARN("[hub::ZmqQueue::create_writer] one-to-one "
                        "producer (BINDING) ignores opts.server_pubkey "
                        "for '{}'; the binding side is the CURVE "
                        "server. Drop or check topology.",
                        opts.endpoint);
        }
        return push_to(std::move(opts.endpoint), std::move(opts.schema), std::move(opts.packing),
                       opts.identity_key_name, std::move(opts.zap_domain),
                       /*bind=*/true, std::move(opts.schema_tag), opts.sndhwm,
                       opts.send_buffer_depth, opts.overflow_policy, opts.send_retry_interval_ms,
                       std::move(opts.instance_id),
                       /*server_pubkey=*/{});
    case ChannelTopology::FanIn:
        // Producer DIALING side.  PUSH connect to consumer's bind
        // endpoint.  zap_domain is ignored on the dialing side (ZAP is
        // the binding side's admission gate).  Warn when caller passes
        // a non-empty domain — likely a topology/side mixup.
        // server_pubkey (consumer's CURVE identity pubkey) is
        // FORWARDED — it drives curve_serverkey on the PUSH-connect
        // side.  Empty is tolerated at the factory (Standby buffer
        // per HEP-CORE-0036 §6.7); apply_master_approval(REG_ACK)
        // populates from initial_allowlist[0] on the wire path.
        if (!opts.zap_domain.empty())
        {
            LOGGER_WARN("[hub::ZmqQueue::create_writer] fan-in producer "
                        "(DIALING) ignores opts.zap_domain for '{}'; "
                        "the dialing side has no ZAP gate. Drop or "
                        "check topology.",
                        opts.endpoint);
        }
        return push_to(std::move(opts.endpoint), std::move(opts.schema), std::move(opts.packing),
                       opts.identity_key_name,
                       /*zap_domain=*/{}, // no ZAP on dialing side
                       /*bind=*/false, std::move(opts.schema_tag), opts.sndhwm,
                       opts.send_buffer_depth, opts.overflow_policy, opts.send_retry_interval_ms,
                       std::move(opts.instance_id), std::move(opts.server_pubkey));
    case ChannelTopology::FanOut:
    {
        // Producer BINDING side, PUB bind.  Same CURVE-server wiring
        // as OneToOne producer bind (ZAP gates admission by pubkey).
        // server_pubkey ignored on binding side; WARN on caller mixup.
        if (!opts.server_pubkey.empty())
        {
            LOGGER_WARN("[hub::ZmqQueue::create_writer] fan-out producer "
                        "(BINDING) ignores opts.server_pubkey for '{}'; "
                        "the binding side is the CURVE server. Drop or "
                        "check topology.",
                        opts.endpoint);
        }
        if (auto err =
                validate_curve_factory_params(opts.identity_key_name, /*server_pubkey_z85=*/{},
                                              /*bind_side=*/true);
            !err.empty())
        {
            LOGGER_ERROR("[hub::ZmqQueue::create_writer] fan-out PUB: invalid "
                         "auth params for '{}': {}",
                         opts.endpoint, err);
            return nullptr;
        }
        auto writer = build_plaintext_writer_(
            opts.endpoint, std::move(opts.schema), std::move(opts.packing),
            /*bind=*/true, std::move(opts.schema_tag), opts.sndhwm, opts.send_buffer_depth,
            opts.overflow_policy, opts.send_retry_interval_ms, std::move(opts.instance_id),
            /*is_pubsub=*/true);
        if (!writer)
            return nullptr;
        std::unique_ptr<ZmqQueue> z(static_cast<ZmqQueue *>(writer.release()));
        assert(!z->is_running() && "create_writer(FanOut): plaintext factory must not "
                                   "start() the queue before auth fields are populated; "
                                   "ordering invariant broken — silent plaintext-fallback risk");
        z->pImpl->identity_key_name_ = std::string{opts.identity_key_name};
        z->pImpl->zap_domain_ = std::move(opts.zap_domain);
        // Initial allowlist intentionally NOT seeded — caller invokes
        // `set_peer_allowlist()` after `start()`.  Empty == deny-all
        // secure default (same contract as `push_to`).
        return z;
    }
    }
    return nullptr; // unreachable under well-formed enum
}

// ============================================================================
// PeerAdmission overrides (`PeerAdmission` interface)
// ============================================================================

bool ZmqQueue::set_peer_allowlist(pylabhub::utils::security::PeerAllowlist allowlist)
{
    if (!pImpl)
        return false;
    // BINDING side has the ZAP allowlist — both PUSH bind (OneToOne
    // producer), PUB bind (FanOut producer), AND PULL bind (FanIn
    // consumer, under HEP-CORE-0017 §3.3.0 topology migration).
    // DIALING sides authenticate the peer via curve_serverkey; the
    // inherited PeerAllowlist mutator is inert on them.
    if (!pImpl->bind_socket)
        return false;

    // Diff-based install log — event-sourcing over the ZAP allowlist.
    // Reader replays `added=/removed=` lines against timestamps to
    // reconstruct the full snapshot at any point.  Idempotent replays
    // (broker fires NOTIFY twice per admission) log nothing so the
    // signal is O(delta), not O(size).  Correlates 1:1 with
    // `ZapRouter::pump_one: ALLOW/DENY pubkey=...` on the pump thread
    // — timestamp gap between an `added=[X]` line and a `DENY pubkey=X`
    // line pins the exact race window in which the peer dialed before
    // its pubkey was installed.
    auto prev = pImpl->allowlist_.load(std::memory_order_acquire);
    std::set<std::string> now_keys;
    for (const auto &p : allowlist.peers)
        now_keys.insert(p.data);
    std::set<std::string> prev_keys;
    if (prev)
    {
        for (const auto &p : prev->peers)
            prev_keys.insert(p.data);
    }
    std::vector<std::string> added, removed;
    std::set_difference(now_keys.begin(), now_keys.end(), prev_keys.begin(), prev_keys.end(),
                        std::back_inserter(added));
    std::set_difference(prev_keys.begin(), prev_keys.end(), now_keys.begin(), now_keys.end(),
                        std::back_inserter(removed));
    if (!added.empty() || !removed.empty())
    {
        auto join = [](const std::vector<std::string> &v)
        {
            std::string s;
            for (const auto &e : v)
            {
                if (!s.empty())
                    s += ",";
                s += e;
            }
            return s;
        };
        LOGGER_INFO("[hub::ZmqQueue::set_peer_allowlist] endpoint='{}' zap_domain='{}' "
                    "delta: added=[{}] removed=[{}] new_size={} (HEP-CORE-0036 §6.5)",
                    pImpl->endpoint, pImpl->resolved_zap_domain_, join(added), join(removed),
                    now_keys.size());
    }

    pImpl->allowlist_.store(
        std::make_shared<const pylabhub::utils::security::PeerAllowlist>(std::move(allowlist)),
        std::memory_order_release);
    return true;
}

std::optional<pylabhub::utils::security::PeerAllowlist> ZmqQueue::peer_allowlist_snapshot() const
{
    if (!pImpl)
        return std::nullopt;
    // BINDING side has the ZAP allowlist (PUSH/PUB bind and PULL bind
    // under fan-in topology).  DIALING sides authenticate the peer via
    // curve_serverkey; no allowlist to snapshot.
    if (!pImpl->bind_socket)
        return std::nullopt;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap)
        return std::nullopt;
    return *snap;
}

bool ZmqQueue::is_peer_allowed(const pylabhub::utils::security::PeerIdentity &peer) const
{
    if (!pImpl)
        return false;
    // BINDING side gates admission via allowlist (PUSH/PUB bind, PULL
    // bind under fan-in).  DIALING side has no inbound handshakes.
    if (!pImpl->bind_socket)
        return false;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap)
        return false;
    return snap->contains(peer);
}

// ── Dynamic producer-peer membership (HEP-CORE-0017 §3.3, #103 A2) ──────────

bool ZmqQueue::set_producer_peers(std::vector<ProducerPeer> list)
{
    if (!pImpl)
        return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
    {
        LOGGER_INFO("[hub::ZmqQueue::set_producer_peers] inert on "
                    "PUSH side (queue='{}'); call set_peer_allowlist "
                    "instead",
                    pImpl->endpoint);
        return false;
    }

    // HEP-CORE-0017 §3.3.0 fan-out consumer is DIALING with singular
    // peer (one PUB binding side per channel); N>1 is a topology
    // contract violation.  Refuse instead of silently accepting a
    // multi-peer list that Phase E retirement will strip anyway.
    if (pImpl->socket_pattern == ZmqQueueImpl::SocketPattern::PubSub && list.size() > 1)
    {
        LOGGER_WARN("[hub::ZmqQueue::set_producer_peers] SUB side "
                    "(fan-out consumer) accepts at most one peer per "
                    "HEP-CORE-0017 §3.3.0; got {} peers for '{}' — "
                    "refusing",
                    list.size(), pImpl->endpoint);
        return false;
    }

    // HEP-CORE-0036 §6.7 Option B (locked 2026-06-12).  Bare
    // set_producer_peers BUFFERS args only — it writes producer_peers_
    // and returns.  It does NOT promote peer[0] into the queue's
    // transport-artifact fields (server_pubkey_z85_, endpoint), so
    // is_configured() stays false and start() stays refused.  The
    // single Standby → Configured → Active driver is
    // apply_master_approval(CONSUMER_REG_ACK) (or set_producer_peers
    // called on an already-Active queue, where it acts as a runtime
    // refresh).
    //
    // For an already-running queue, runtime refresh is just the
    // snapshot replace; the running socket is not touched (peer-swap
    // would require teardown+rebuild per §6.7 "stop() is terminal" +
    // §I12 "no cached-authority replay").
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    pImpl->producer_peers_ = std::move(list);
    return true;
}

bool ZmqQueue::add_producer_peer(const ProducerPeer &peer)
{
    if (!pImpl)
        return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    for (auto &existing : pImpl->producer_peers_)
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

bool ZmqQueue::remove_producer_peer(const std::string &role_uid)
{
    if (!pImpl)
        return false;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    auto it = std::find_if(pImpl->producer_peers_.begin(), pImpl->producer_peers_.end(),
                           [&](const ProducerPeer &p) { return p.role_uid == role_uid; });
    if (it == pImpl->producer_peers_.end())
        return false;
    pImpl->producer_peers_.erase(it);
    return true;
}

std::size_t ZmqQueue::producer_peer_count() const noexcept
{
    if (!pImpl)
        return 0;
    if (pImpl->mode != ZmqQueueImpl::Mode::Read)
        return 0;
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    return pImpl->producer_peers_.size();
}

bool ZmqQueue::apply_master_approval(const nlohmann::json &artifacts) noexcept
{
    if (!pImpl)
        return false;
    try
    {
        // Already-running queues: apply runtime updates per §6.7 Active
        // column.  Either way no socket bind/connect — that already
        // happened in the prior apply_master_approval call that drove
        // Standby → Active.
        const bool already_running = pImpl->running_.load(std::memory_order_acquire);

        // ── Unified peer-list wire field (HEP-CORE-0036 §6.2 + §6.4) ──
        //
        // Both REG_ACK and CONSUMER_REG_ACK carry a peer-list under
        // sender-specific historical field names.  The payload schema
        // is unified: array of `{role_uid?, endpoint?, pubkey_z85}`
        // objects.  Interpretation is topology-role driven:
        //
        //   BINDING side (bind_socket == true):
        //     - peers.size() == 0..N — allowlist snapshot
        //     - each entry: pubkey_z85 REQUIRED; endpoint may be empty
        //       (BINDING doesn't dial); role_uid optional metadata
        //
        //   DIALING side (bind_socket == false):
        //     - peers.size() == 0..N — dial targets (legacy multi-
        //       producer fan-in supported; post-Phase-G migration
        //       converges to size == 1 for OneToOne/FanOut consumers
        //       and fan-in producers, but that's a wire-shape
        //       constraint enforced by the broker builder, NOT here)
        //     - each entry: pubkey_z85 REQUIRED (curve_serverkey);
        //       endpoint REQUIRED (dial target); role_uid optional
        //
        // No-op tolerance: if the ACK lacks the peer field the queue
        // is unchanged.  Used by SHM ACKs (SHM branch doesn't touch
        // ZmqQueue) and by runtime refresh ACKs.
        const bool is_read = (pImpl->mode == ZmqQueueImpl::Mode::Read);
        const char *field = is_read ? "producers" : "initial_allowlist";
        const bool is_dialing = !pImpl->bind_socket;

        std::vector<ProducerPeer> peers;
        bool have_peers = false;

        if (artifacts.contains(field))
        {
            const auto &arr = artifacts.at(field);
            if (!arr.is_array())
            {
                LOGGER_WARN("[hub::ZmqQueue::apply_master_approval] "
                            "'{}' field is not an array — refusing",
                            field);
                return false;
            }
            // Fan-out (PubSub) enforces singular DIALING per
            // HEP-CORE-0017 §3.3.0 — SUB has exactly one PUB peer.
            // PushPull DIALING may still be multi (legacy fan-in
            // consumer / one-to-one after Phase G).
            if (is_dialing && pImpl->socket_pattern == ZmqQueueImpl::SocketPattern::PubSub &&
                arr.size() > 1)
            {
                LOGGER_WARN("[hub::ZmqQueue::apply_master_approval] "
                            "'{}' on fan-out DIALING (SUB) must have at "
                            "most one entry per HEP-CORE-0017 §3.3.0; "
                            "got {}",
                            field, arr.size());
                return false;
            }
            peers.reserve(arr.size());
            for (const auto &entry : arr)
            {
                if (!entry.is_object())
                {
                    LOGGER_WARN("[hub::ZmqQueue::apply_master_approval] "
                                "'{}' entry not an object — refusing",
                                field);
                    return false;
                }
                ProducerPeer p;
                p.role_uid = entry.value("role_uid", std::string{});
                p.endpoint = entry.value("endpoint", std::string{});
                p.pubkey_z85 = entry.value("pubkey_z85", std::string{});
                if (p.pubkey_z85.size() != 40)
                {
                    LOGGER_WARN("[hub::ZmqQueue::apply_master_approval] "
                                "'{}' entry pubkey_z85 wrong length: {} "
                                "(expected 40 Z85 chars)",
                                field, p.pubkey_z85.size());
                    return false;
                }
                if (is_dialing && p.endpoint.empty())
                {
                    LOGGER_WARN("[hub::ZmqQueue::apply_master_approval] "
                                "'{}' entry on DIALING side missing "
                                "required endpoint",
                                field);
                    return false;
                }
                peers.push_back(std::move(p));
            }
            have_peers = true;
        }

        // Apply peers per (mode × bind_socket) role.
        if (have_peers)
        {
            if (is_read)
            {
                // Read side — buffer full peer list into producer_peers_
                // regardless of bind/connect direction.  start() reads
                // producer_peers_ directly for the connect loop on the
                // DIALING side; BINDING side ignores it.
                if (!set_producer_peers(std::move(peers)))
                    return false;
            }
            if (pImpl->bind_socket)
            {
                // BINDING side (both PULL bind and PUSH/PUB bind) —
                // peers are ZAP allowlist entries.  Snapshot from
                // pubkey_z85 (PeerIdentity kind="curve" matches
                // handle_channel_auth_notifies + ZAP router lookups).
                pylabhub::utils::security::PeerAllowlist allowlist;
                const auto &src = is_read ? pImpl->producer_peers_ : peers;
                {
                    std::unique_lock<std::mutex> lock(pImpl->producer_peers_mu_, std::defer_lock);
                    if (is_read)
                        lock.lock();
                    for (const auto &p : src)
                    {
                        allowlist.peers.insert(
                            pylabhub::utils::security::PeerIdentity{"curve", p.pubkey_z85});
                    }
                }
                if (!set_peer_allowlist(std::move(allowlist)))
                    return false;
            }
            else if (!already_running)
            {
                // DIALING side — promote peer[0] into is_configured()
                // flag artifacts.  start() reads server_pubkey_z85_ +
                // endpoint for the CURVE-authenticated connect (Read
                // side also reads producer_peers_ for the per-peer
                // connect loop; these fields are the "Standby ->
                // Configured" flag per HEP-CORE-0036 §6.7 Option B).
                // Only mutate on Standby; on Active, held stable for
                // the socket's lifetime.
                std::unique_lock<std::mutex> lock(pImpl->producer_peers_mu_, std::defer_lock);
                if (is_read)
                    lock.lock();
                const auto &src = is_read ? pImpl->producer_peers_ : peers;
                if (!src.empty())
                {
                    const auto &p0 = src.front();
                    if (pImpl->server_pubkey_z85_.empty() && !p0.pubkey_z85.empty())
                        pImpl->server_pubkey_z85_ = p0.pubkey_z85;
                    if (pImpl->endpoint.empty() && !p0.endpoint.empty())
                        pImpl->endpoint = p0.endpoint;
                }
            }
        }

        // Drive Standby → Configured → Active.  start() is the private
        // implementation detail invoked here per HEP-CORE-0036 §6.7
        // Option B; production code does not call start() directly.
        if (already_running)
            return true;
        LOGGER_INFO("[hub::ZmqQueue] event=QueueStateTransition side={} "
                    "from=Standby to=Configured queue='{}' endpoint='{}'",
                    socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->queue_name,
                    pImpl->endpoint);

        // HEP-CORE-0036 §6.6.3 — defer connect for fan-in DIALING PUSH.
        // Rationale: `socket.connect()` inside `start()` triggers the
        // CURVE handshake at consumer's PULL socket immediately.  Under
        // fan-in the consumer's ZAP allowlist is populated AFTER the
        // consumer receives `CHANNEL_AUTH_CHANGED_NOTIFY(admitted, P)`
        // + pulls the allowlist + applies it (~100 ms).  A connect at
        // REG_ACK apply time (~ms) races and gets denied by ZAP; libzmq
        // treats ZAP DENY as terminal (no client-side retry).  The
        // role host invokes `finalize_connect()` UNIFORMLY after apply
        // for every role type; this queue's override polls the injected
        // `PeerReadinessOracle` at `kBrokerReadinessPollInterval` until
        // `Ready` and then completes the deferred `start()`.
        //
        // Detection: write-side + not bind_socket is exclusively fan-in
        // DIALING PUSH.  Fan-out producer binds PUB; one-to-one
        // producer binds PUSH.  Read-side (any transport) also uses
        // the DIALING code path — but for the reader, the peer set is
        // known from REG_ACK.producers[] and the *reader's* client-
        // side CURVE talks to a producer that's already up, so no
        // race exists.
        const bool is_fanin_dialing_push =
            (pImpl->mode == ZmqQueueImpl::Mode::Write) && !pImpl->bind_socket;
        if (is_fanin_dialing_push)
        {
            pImpl->dial_pending = true;
            LOGGER_INFO("[hub::ZmqQueue] event=DialDeferred side={} endpoint='{}' "
                        "queue='{}' (HEP-CORE-0036 §6.6.3 — awaiting role-host "
                        "finalize_connect() with PeerReadinessOracle)",
                        socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->endpoint,
                        pImpl->queue_name);
            return true;
        }

        return start();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[hub::ZmqQueue::apply_master_approval] exception "
                     "parsing artifacts: {}",
                     e.what());
        return false;
    }
}

bool ZmqQueue::finalize_connect(::pylabhub::hub::PeerReadinessOracle &oracle,
                                std::uint64_t timeout_ms, const std::function<bool()> &is_cancelled,
                                const char *log_tag) noexcept
{
    if (!pImpl)
        return false;

    // Non-deferred queues (fan-out producer, one-to-one binding-side
    // producer, dialing consumer, any already-running queue): no-op
    // success.  Oracle is untouched.
    if (!pImpl->dial_pending)
    {
        return true;
    }

    const auto start_ts = std::chrono::steady_clock::now();
    const auto deadline = timeout_ms == 0 ? std::chrono::steady_clock::time_point::max()
                                          : start_ts + std::chrono::milliseconds(timeout_ms);
    const bool cancellable = static_cast<bool>(is_cancelled);
    const char *tag = log_tag ? log_tag : "hub::ZmqQueue::finalize_connect";
    LOGGER_INFO("[{}] event=FinalizeConnectPolling side={} endpoint='{}' "
                "queue='{}' timeout_ms={} cancellable={} "
                "(HEP-CORE-0036 §6.6.3)",
                tag, socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->endpoint,
                pImpl->queue_name, timeout_ms, cancellable);

    for (;;)
    {
        if (cancellable && is_cancelled())
        {
            LOGGER_INFO("[{}] event=FinalizeConnectCancelled queue='{}' "
                        "elapsed_ms={} (shutdown / critical-error observed)",
                        tag, pImpl->queue_name,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_ts)
                            .count());
            return false;
        }

        auto result = oracle.poll();
        if (result == ::pylabhub::hub::PeerReadinessOracle::PollResult::Ready)
        {
            break;
        }
        if (result == ::pylabhub::hub::PeerReadinessOracle::PollResult::PermanentError)
        {
            LOGGER_ERROR("[{}] event=FinalizeConnectPermanentError queue='{}' "
                         "elapsed_ms={} (oracle returned PermanentError)",
                         tag, pImpl->queue_name,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_ts)
                             .count());
            return false;
        }
        // NotReady — continue polling until deadline.
        if (deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline)
        {
            LOGGER_ERROR("[{}] event=FinalizeConnectTimeout queue='{}' "
                         "budget_ms={} — fatal (HEP-CORE-0036 §6.6.3)",
                         tag, pImpl->queue_name, timeout_ms);
            return false;
        }
        std::this_thread::sleep_for(::pylabhub::kBrokerReadinessPollInterval);
    }

    pImpl->dial_pending = false;
    LOGGER_INFO("[hub::ZmqQueue] event=FinalizeConnect side={} endpoint='{}' "
                "queue='{}' elapsed_ms={} (peer confirmed ready, running "
                "deferred start())",
                socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->endpoint,
                pImpl->queue_name,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_ts)
                    .count());
    return start();
}

std::string ZmqQueue::own_pubkey_z85() const noexcept
{
    if (!pImpl || pImpl->identity_key_name_.empty())
        return {};
    try
    {
        namespace sec = pylabhub::utils::security;
        auto &ks = sec::secure().keys();
        return std::string(ks.pubkey(pImpl->identity_key_name_));
    }
    catch (...)
    {
        // Fail-quiet on lookup: caller treats empty as "no CURVE
        // identity" and the queue's finalize_connect default no-ops,
        // so the queue-owned readiness path degrades to a safe wait-
        // free success rather than a crash.  This matches how
        // apply_master_approval already handles missing keystore
        // material (LOGGER_WARN + skip).
        return {};
    }
}

std::string_view ZmqQueue::binding_role_type() const noexcept
{
    // HEP-CORE-0036 §I9.1 + §6.5 step 6 — queue-owned side identity.
    if (!pImpl || !pImpl->bind_socket)
        return {};
    return (pImpl->mode == ZmqQueueImpl::Mode::Read) ? std::string_view{"consumer"}
                                                     : std::string_view{"producer"};
}

bool ZmqQueue::is_admission_populated() const noexcept
{
    // HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036 §I9.1 —
    // queue-owned admission fact.  Called by role-side loop-ready
    // gate through `RoleAPIBase::channel_admission_populated`.
    if (!pImpl)
        return false;
    if (pImpl->bind_socket)
    {
        // Binding side: ZAP allowlist snapshot.  Empty = deny-all,
        // no peer can dial in yet — gate NotReady.
        auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
        if (!snap)
            return false;
        return !snap->peers.empty();
    }
    // Dialing side.
    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        // Dialing reader (fan-out consumer): peer set known?
        // producer_peers_ populated by apply_master_approval from
        // REG_ACK.producers[].
        std::lock_guard<std::mutex> lk(pImpl->producer_peers_mu_);
        return !pImpl->producer_peers_.empty();
    }
    // Dialing writer (fan-in producer): server_pubkey_z85_ non-empty
    // means the peer to connect to is known.  Rarely queried
    // (producer's gate default is true) but symmetric.
    return !pImpl->server_pubkey_z85_.empty();
}

bool ZmqQueue::is_configured() const noexcept
{
    if (!pImpl)
        return false;
    // Server side (any bind): only needs endpoint — CURVE_SERVER
    // artifacts (identity keypair + ZAP allowlist) are resolved at
    // `start()` time from the KeyStore-seeded identity_key_name_ and
    // the broker-pushed allowlist.  Applies to PUSH+bind (canonical
    // producer) AND PULL+bind (test-only inverse pattern: see
    // make_pull_test in test_hub_zmq_queue.cpp — production PULL
    // always connects).
    // Connect side: PULL+connect needs both endpoint AND serverkey
    // (the connect-side artifact the master delivers via
    // CONSUMER_REG_ACK / §6.5.1 pull).
    //
    // Thread-safety (post-#188 review fix M3): `server_pubkey_z85_`
    // and `endpoint` are mutated by `set_producer_peers()` under
    // `producer_peers_mu_`.  is_configured() takes the same mutex to
    // see a consistent snapshot — without it a concurrent reader
    // (e.g. `start()` called from the role-host main thread while
    // `set_producer_peers` runs on the worker thread per §I12) could
    // observe a torn read.  Contention is negligible: the lock is
    // held for a couple of `std::string::empty()` calls.
    std::lock_guard<std::mutex> lock(pImpl->producer_peers_mu_);
    if (pImpl->bind_socket)
        return !pImpl->endpoint.empty();

    // PULL/connect side: Configured iff `apply_master_approval` has
    // run (or the legacy `pull_from(endpoint, key)` factory
    // populated `endpoint` + `server_pubkey_z85_` at construction).
    // The peer[0] promote block in `apply_master_approval` writes
    // these fields as the "I have run" flag — HEP-CORE-0036 §6.7
    // Option B requires that bare `set_producer_peers` NOT
    // transition the queue; only `apply_master_approval` does.
    // `start()` reads `producer_peers_` directly for the per-peer
    // connect (HEP-CORE-0017 §3.3); these fields are used ONLY as
    // the Standby→Configured flag here.
    return !pImpl->server_pubkey_z85_.empty() && !pImpl->endpoint.empty();
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

ZmqQueue::ZmqQueue(ZmqQueue &&) noexcept = default;

ZmqQueue &ZmqQueue::operator=(ZmqQueue &&o) noexcept
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
    if (!pImpl)
        return false;
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent

    // HEP-CORE-0036 §6.7 Standby gate: refuse to start a queue that
    // is not Configured.  "Configured" means transport artifacts are
    // populated: PULL needs serverkey + connect endpoint; PUSH needs
    // bind endpoint.  Refusing here keeps `start()` honest as the
    // §I12 "door opens" — partial / placeholder state cannot reach
    // libzmq.  Refuse is silent w.r.t. running_ (do NOT exchange) so
    // callers can re-call after `set_producer_peers(...)` populates
    // the artifacts.
    if (!is_configured())
    {
        LOGGER_DEBUG("[hub::ZmqQueue::start] refused — queue in Standby "
                     "(mode={}, endpoint='{}', server_pubkey_set={}); HEP-"
                     "CORE-0036 §6.7 requires Configured state.  Call "
                     "set_producer_peers() to populate transport artifacts "
                     "before start().",
                     socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->endpoint,
                     !pImpl->server_pubkey_z85_.empty());
        return false;
    }

    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race — another thread started it

    try
    {
        // Socket-type selection: mode × pattern (HEP-CORE-0017 §3.3.0).
        //   Read  + PushPull → PULL   (fan-in consumer bind; 1-to-1 consumer connect)
        //   Write + PushPull → PUSH   (fan-in producer connect; 1-to-1 producer bind)
        //   Read  + PubSub   → SUB    (fan-out consumer connect; subscribes to empty topic)
        //   Write + PubSub   → PUB    (fan-out producer bind)
        zmq::socket_type stype;
        if (pImpl->socket_pattern == ZmqQueueImpl::SocketPattern::PushPull)
        {
            stype = (pImpl->mode == ZmqQueueImpl::Mode::Read) ? zmq::socket_type::pull
                                                              : zmq::socket_type::push;
        }
        else // PubSub
        {
            stype = (pImpl->mode == ZmqQueueImpl::Mode::Read) ? zmq::socket_type::sub
                                                              : zmq::socket_type::pub;
        }
        pImpl->socket = zmq::socket_t(pylabhub::hub::get_zmq_context(), stype);
        pImpl->socket.set(zmq::sockopt::linger, 0);

        // SUB sockets require an explicit subscription; empty topic
        // filter matches every message (HEP-CORE-0017 §3.3.1 — script
        // owns any per-message filtering above the queue layer).
        if (stype == zmq::socket_type::sub)
        {
            pImpl->socket.set(zmq::sockopt::subscribe, "");
        }

        // [ZQ8] Apply SNDHWM on the write-side socket when caller
        // requested a specific value.  Semantic differs by socket type
        // (see ZmqQueueImpl::sndhwm comment): PUSH triggers upstream
        // block/EAGAIN; PUB triggers per-subscriber silent drop.  The
        // knob is the same; the observed behavior is not.
        if (pImpl->mode == ZmqQueueImpl::Mode::Write && pImpl->sndhwm > 0)
            pImpl->socket.set(zmq::sockopt::sndhwm, pImpl->sndhwm);

        // ── PeerAdmission (HEP-CORE-0036 §7) — CURVE + ZAP wiring (HEP-CORE-0036 §6) ────
        // Empty identity_key_name_ == legacy unauth path (reachable
        // only via plaintext `pull_from`/`push_to` factories which
        // never populate it).  HEP-CORE-0040 §172: keys are sourced
        // from `secure().keys()` by name — secret bytes flow from
        // LockedKey region directly into libzmq's internal CURVE
        // state inside `with_seckey` callback scope; no std::string
        // copy holds the seckey at queue scope.
        if (!pImpl->identity_key_name_.empty())
        {
            namespace sec = pylabhub::utils::security;
            auto &ks = sec::secure().keys();

            pImpl->socket.set(zmq::sockopt::curve_publickey, ks.pubkey(pImpl->identity_key_name_));
            ks.with_seckey(pImpl->identity_key_name_, [&](std::string_view seckey)
                           { pImpl->socket.set(zmq::sockopt::curve_secretkey, seckey); });

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
                        std::snprintf(addr_buf, sizeof(addr_buf), "%s@%p",
                                      pImpl->queue_name.c_str(), static_cast<void *>(pImpl.get()));
                        pImpl->resolved_zap_domain_ = addr_buf;
                    }
                }
                pImpl->socket.set(zmq::sockopt::zap_domain, pImpl->resolved_zap_domain_);

                // HEP-CORE-0040 §8.4 (#158): seed an EMPTY allowlist
                // (deny-all secure default) ONLY when no caller has
                // populated one yet.  Two ordering patterns both reach
                // this branch in a CURVE-wired queue's lifetime:
                //
                //   (a) factory → start() → set_peer_allowlist()
                //       (L2 tests; `push_to` leaves `allowlist_`
                //       nullptr — see line 801-803).  On start(), the
                //       atomic is nullptr → seed empty here.  The
                //       follow-up `set_peer_allowlist()` overwrites.
                //   (b) factory → set_peer_allowlist(initial_allowlist)
                //       → start()
                //       (apply_master_approval(REG_ACK) at S3 per
                //       HEP-CORE-0036 §6.7).  On start(), the atomic
                //       already holds the broker-supplied snapshot.
                //       Clobbering it here would drop REG_ACK's
                //       `initial_allowlist` on the floor and leave
                //       the PUSH socket deny-all under CURVE — every
                //       authorized consumer would fail handshake.
                //
                // Architectural symmetry with PULL side: `start()`
                // READS `server_pubkey_z85_` + `endpoint` that
                // `set_producer_peers()` populated; it does NOT
                // overwrite them.  The PUSH-side allowlist follows the
                // same rule: caller-populated transport artifacts are
                // load-bearing inputs to start(), not state to seed
                // unconditionally.
                //
                // In-memory contract preserved either way: "CURVE
                // wired → allowlist exists (admit ⊆ peers; empty
                // peers ⇒ deny all)" — post-start, the atomic always
                // holds a non-null shared_ptr.  Runtime refresh via
                // `CHANNEL_AUTH_CHANGED_NOTIFY` (HEP-CORE-0036 §6.5)
                // calls `set_peer_allowlist()` on the Active queue;
                // start() is idempotent so this branch never re-runs.
                // Task #103 (AUTH-1).
                if (!pImpl->allowlist_.load(std::memory_order_acquire))
                {
                    pImpl->allowlist_.store(std::make_shared<const sec::PeerAllowlist>(),
                                            std::memory_order_release);
                }

                // Register with the router BEFORE bind.  Without this
                // ordering, an early peer connect could submit a ZAP
                // request that lands on an unregistered domain and
                // gets denied even though admission is configured.
                pImpl->zap_handle_.emplace(
                    sec::ZapRouter::instance().register_domain(pImpl->resolved_zap_domain_, *this));
            }
            // Client side: CURVE `serverkey` is set per-connect below
            // (HEP-CORE-0017 §3.3 multi-endpoint PULL).  The legacy
            // `pull_from(endpoint, serverkey)` factory populates
            // `endpoint` + `server_pubkey_z85_` for single-peer L2 tests
            // and is threaded through the same per-connect loop as
            // producer_peers_-driven multi-peer configurations.
        }

        if (pImpl->bind_socket)
        {
            pImpl->socket.bind(pImpl->endpoint);
        }
        else
        {
            // ── Dialing-side connect: PULL (Read/PushPull),
            //    SUB (Read/PubSub), PUSH (Write/PushPull + FanIn)
            //    (HEP-CORE-0017 §3.3) ──────────────────────────────
            //
            // Per-peer connect with per-peer `curve_serverkey`.  The
            // libzmq option `ZMQ_CURVE_SERVERKEY` is set on the socket
            // and captured into the session state at the following
            // `connect()` call, so alternating (set, connect) pairs
            // produce N independent CURVE-authenticated connections,
            // one per producer.  Reference: ZMTP/CURVE handshake is
            // per-connection; the option is a client-side input that
            // libzmq snapshots at connect time.
            //
            // Two configuration sources feed the loop:
            //   (a) `producer_peers_` (populated by
            //       `set_producer_peers` / `apply_master_approval`)
            //       — the HEP-CORE-0036 §6.4 canonical path.  Handles
            //       multi-producer fan-in.
            //   (b) `endpoint` + `server_pubkey_z85_` populated by
            //       the legacy `pull_from(endpoint, key)` factory
            //       — single-peer L2 test path.  This branch fires
            //       ONLY when `producer_peers_` is empty; the two
            //       sources are never used simultaneously.
            //
            // Both branches take the same producer_peers_mu_ so a
            // concurrent `set_producer_peers` on the role-host thread
            // sees a consistent view.  CURVE key mutation between
            // connects is a client-side no-op with respect to already-
            // established connections — only the NEXT connect is
            // affected.
            std::lock_guard<std::mutex> peers_lock(pImpl->producer_peers_mu_);

            const bool curve_wired = !pImpl->identity_key_name_.empty();

            auto connect_one = [&](const std::string &pubkey_z85, const std::string &ep)
            {
                if (curve_wired && !pubkey_z85.empty())
                {
                    pImpl->socket.set(zmq::sockopt::curve_serverkey, pubkey_z85);
                }
                pImpl->socket.connect(ep);
            };

            if (!pImpl->producer_peers_.empty())
            {
                // (a) HEP-0036 canonical multi-peer path.
                for (const auto &peer : pImpl->producer_peers_)
                {
                    if (peer.endpoint.empty())
                        continue;
                    connect_one(peer.pubkey_z85, peer.endpoint);
                    LOGGER_INFO("[hub::ZmqQueue] event=PullPeerConnected "
                                "role_uid='{}' endpoint='{}'",
                                peer.role_uid, peer.endpoint);
                }
            }
            else
            {
                // (b) Legacy pull_from single-peer path.
                connect_one(pImpl->server_pubkey_z85_, pImpl->endpoint);
            }
        }

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
                "[hub::ZmqQueue::start] libzmq reported mechanism=" + std::to_string(mech) +
                " (expected ZMQ_CURVE=" + std::to_string(ZMQ_CURVE) +
                ") — CURVE wiring "
                "regression (HEP-CORE-0035 §2 invariant violated; "
                "see ZmqQueue::mechanism() / Mechanism enum)");
        }
        pImpl->mechanism_.store(Mechanism::Curve, std::memory_order_release);
    }
    catch (const std::invalid_argument &e)
    {
        // specific, caller-actionable diagnostic for auth
        // misconfiguration that slipped past factory validation OR
        // (post-#161) the CURVE engagement guard at the bottom of
        // the try block.  The queue did not start; reset the
        // observable mechanism so `mechanism()` reflects the closed
        // state.
        pImpl->socket.close();
        pImpl->mechanism_.store(Mechanism::Uninitialized, std::memory_order_release);
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] auth setup failed for '{}': {}", pImpl->endpoint, e.what());
        return false;
    }
    catch (const zmq::error_t &e)
    {
        pImpl->socket.close();
        pImpl->mechanism_.store(Mechanism::Uninitialized, std::memory_order_release);
        pImpl->running_.store(false, std::memory_order_release);
        LOGGER_ERROR("[hub::ZmqQueue] socket setup ({}) failed for '{}': {}",
                     pImpl->bind_socket ? "bind" : "connect", pImpl->endpoint, e.what());
        return false;
    }

    // Resolve actual bound endpoint (captures OS-assigned port when endpoint uses ":0").
    if (pImpl->bind_socket)
    {
        try
        {
            pImpl->actual_endpoint = pImpl->socket.get(zmq::sockopt::last_endpoint);
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
    ZmqQueueImpl *impl_ptr = pImpl.get();

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
        std::snprintf(addr_buf, sizeof(addr_buf), "@%p", static_cast<void *>(impl_ptr));
        owner_id = pImpl->queue_name;
        owner_id += addr_buf;
    }
    pImpl->thread_mgr_ = std::make_unique<pylabhub::utils::ThreadManager>("ZmqQueue", owner_id);

    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        pImpl->recv_stop_.store(false, std::memory_order_release);
        pImpl->thread_mgr_->spawn(
            "recv", [impl_ptr](pylabhub::utils::ThreadManager::SlotContext &ctx)
            { ctx.with_active_loop([impl_ptr, &ctx] { impl_ptr->run_recv_thread_(ctx); }); });
    }
    else // Write
    {
        pImpl->send_stop_.store(false, std::memory_order_release);
        pImpl->thread_mgr_->spawn(
            "send", [impl_ptr](pylabhub::utils::ThreadManager::SlotContext &ctx)
            { ctx.with_active_loop([impl_ptr, &ctx] { impl_ptr->run_send_thread_(ctx); }); });
    }

    // HEP-CORE-0036 §6.7 — queue has bound/connected its socket and
    // spawned its worker thread.  Pair-marker to Standby->Configured
    // emitted by apply_master_approval just above the start() call.
    LOGGER_INFO("[hub::ZmqQueue] event=QueueStateTransition side={} "
                "from=Configured to=Active queue='{}' endpoint='{}'",
                socket_type_label(pImpl->mode, pImpl->socket_pattern), pImpl->queue_name,
                pImpl->bind_socket ? pImpl->actual_endpoint : pImpl->endpoint);
    return true;
}

void ZmqQueue::stop()
{
    if (!pImpl)
        return;
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
            const auto deadline = std::chrono::steady_clock::now() + kGrace;
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
                LOGGER_ERROR("[hub::ZmqQueue:{}] {} thread(s) detached on stop() "
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
    pImpl->mechanism_.store(Mechanism::Uninitialized, std::memory_order_release);
}

bool ZmqQueue::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

Mechanism ZmqQueue::mechanism() const noexcept
{
    if (!pImpl)
        return Mechanism::Uninitialized;
    return pImpl->mechanism_.load(std::memory_order_acquire);
}

// ============================================================================
// Reading
// ============================================================================

const void *ZmqQueue::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Read)
        return nullptr;

    const auto t_entry = ZmqQueueImpl::Clock::now();

    // Block until data is available.
    std::unique_lock<std::mutex> lk(pImpl->recv_mu_);
    pImpl->recv_cv_.wait_for(
        lk, timeout, [this]
        { return pImpl->ring_count_ > 0 || pImpl->recv_stop_.load(std::memory_order_relaxed); });

    if (pImpl->ring_count_ == 0)
        return nullptr;

    const auto t_acquired = ZmqQueueImpl::Clock::now();

    // Copy from ring slot to current_read_buf_ (pre-allocated; no heap alloc). [ZQ9]
    std::memcpy(pImpl->current_read_buf_.data(), pImpl->recv_ring_[pImpl->ring_head_].data(),
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
            std::chrono::duration_cast<std::chrono::microseconds>(t_acquired - pImpl->t_iter_start_)
                .count());
        pImpl->ctx_metrics_.set_last_iteration(elapsed_us);
        pImpl->ctx_metrics_.update_max_iteration(elapsed_us);
        pImpl->ctx_metrics_.set_context_elapsed(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      t_acquired - pImpl->ctx_metrics_.context_start_time_val())
                                      .count()));
        // No timing overrun detection — the main loop owns the deadline.
        // Reader never drops data (data_drop_count stays 0).
    }
    else
    {
        pImpl->ctx_metrics_.set_context_start(t_acquired);
    }

    pImpl->ctx_metrics_.set_last_slot_wait(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t_acquired - t_entry).count()));
    pImpl->t_iter_start_ = t_acquired;
    pImpl->t_acquired_ = t_acquired;

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
                                          ZmqQueueImpl::Clock::now() - pImpl->t_acquired_)
                                          .count()));
        }
    }
}

// ============================================================================
// Writing
// ============================================================================

void *ZmqQueue::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write)
        return nullptr;
    if (pImpl->send_stop_.load(std::memory_order_relaxed))
        return nullptr;

    const auto t_entry = ZmqQueueImpl::Clock::now();

    if (pImpl->overflow_policy_ == OverflowPolicy::Drop)
    {
        std::lock_guard<std::mutex> lk(pImpl->send_mu_);
        if (pImpl->send_count_ >= pImpl->send_depth_)
        {
            pImpl->data_drop_count_.fetch_add(
                1, std::memory_order_relaxed); // buffer-full: cycle failed
            return nullptr;
        }
    }
    else // Block
    {
        ZmqQueueImpl *impl_ptr = pImpl.get();
        std::unique_lock<std::mutex> lk(impl_ptr->send_mu_);
        const bool ok = impl_ptr->send_cv_.wait_for(
            lk, timeout,
            [impl_ptr]
            {
                return impl_ptr->send_count_ < impl_ptr->send_depth_ ||
                       impl_ptr->send_stop_.load(std::memory_order_relaxed);
            });
        if (!ok || impl_ptr->send_stop_.load(std::memory_order_relaxed))
        {
            impl_ptr->data_drop_count_.fetch_add(
                1, std::memory_order_relaxed); // timeout or shutdown: cycle failed
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
            std::chrono::duration_cast<std::chrono::microseconds>(t_acquired - pImpl->t_iter_start_)
                .count());
        pImpl->ctx_metrics_.set_last_iteration(elapsed_us);
        pImpl->ctx_metrics_.update_max_iteration(elapsed_us);
        pImpl->ctx_metrics_.set_context_elapsed(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      t_acquired - pImpl->ctx_metrics_.context_start_time_val())
                                      .count()));

        // No timing overrun detection — the main loop owns the deadline.
        // Write-side data_drop_count is incremented above on buffer-full/timeout only.
    }
    else
    {
        pImpl->ctx_metrics_.set_context_start(t_acquired);
    }

    pImpl->ctx_metrics_.set_last_slot_wait(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t_acquired - t_entry).count()));
    pImpl->t_iter_start_ = t_acquired;
    pImpl->t_acquired_ = t_acquired;

    // Zero buffer so padding bytes are deterministic (checksum consistency).
    std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
    return pImpl->write_buf_.data();
}

void ZmqQueue::write_commit() noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write)
        return;

    // Measure work time: acquire return → commit call.
    {
        const ZmqQueueImpl::Clock::time_point t_zero{};
        if (pImpl->t_acquired_ != t_zero)
        {
            pImpl->ctx_metrics_.set_last_slot_exec(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          ZmqQueueImpl::Clock::now() - pImpl->t_acquired_)
                                          .count()));
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
        std::memcpy(pImpl->send_ring_[pImpl->send_tail_].data(), pImpl->write_buf_.data(),
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
    if (!pImpl)
        return 0;
    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
        return pImpl->max_depth;
    return pImpl->send_depth_;
}

std::string ZmqQueue::policy_info() const
{
    if (!pImpl)
        return "zmq_unconnected";
    const bool is_pubsub = pImpl->socket_pattern == ZmqQueueImpl::SocketPattern::PubSub;
    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        const char *stype = is_pubsub ? "sub" : "pull";
        return std::string{"zmq_"} + stype + "_ring_" + std::to_string(pImpl->max_depth);
    }
    const char *stype = is_pubsub ? "pub" : "push";
    return (pImpl->overflow_policy_ == OverflowPolicy::Drop)
               ? (std::string{"zmq_"} + stype + "_drop")
               : (std::string{"zmq_"} + stype + "_block");
}

std::string ZmqQueue::actual_endpoint() const
{
    if (!pImpl)
        return "";
    // After start(): resolved (e.g. port-0 bind resolves to actual port).
    // Before start() or on connect-mode: returns configured endpoint.
    return pImpl->actual_endpoint.empty() ? pImpl->endpoint : pImpl->actual_endpoint;
}

bool ZmqQueue::is_binding_side() const noexcept
{
    return pImpl && pImpl->bind_socket;
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
    if (!pImpl)
        return {};
    QueueMetrics m;
    // Domain 2+3 timing fields (from unified ContextMetrics).
    m.last_slot_wait_us = pImpl->ctx_metrics_.last_slot_wait_us_val();
    m.last_iteration_us = pImpl->ctx_metrics_.last_iteration_us_val();
    m.max_iteration_us = pImpl->ctx_metrics_.max_iteration_us_val();
    m.context_elapsed_us = pImpl->ctx_metrics_.context_elapsed_us_val();
    m.last_slot_exec_us = pImpl->ctx_metrics_.last_slot_exec_us_val();
    m.data_drop_count = pImpl->data_drop_count_.load(std::memory_order_relaxed);
    // configured_period_us reported at loop level (LoopMetricsSnapshot), not queue level.
    // Transport-specific counters.
    m.recv_overflow_count = pImpl->recv_overflow_count_.load(std::memory_order_relaxed);
    m.recv_frame_error_count = pImpl->recv_frame_error_count_.load(std::memory_order_relaxed);
    m.recv_gap_count = pImpl->recv_gap_count_.load(std::memory_order_relaxed);
    m.send_drop_count = pImpl->send_drop_count_.load(std::memory_order_relaxed);
    m.send_retry_count = pImpl->send_retry_count_.load(std::memory_order_relaxed);
    m.checksum_error_count = pImpl->ctx_metrics_.checksum_error_count_val();
    return m;
}

void ZmqQueue::reset_metrics()
{
    if (!pImpl)
        return;
    pImpl->ctx_metrics_.clear();
    pImpl->data_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_overflow_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_frame_error_count_.store(0, std::memory_order_relaxed);
    pImpl->recv_gap_count_.store(0, std::memory_order_relaxed);
    pImpl->send_drop_count_.store(0, std::memory_order_relaxed);
    pImpl->send_retry_count_.store(0, std::memory_order_relaxed);
    pImpl->t_iter_start_ = {};
    pImpl->t_acquired_ = {};
}

void ZmqQueue::init_metrics()
{
    reset_metrics();
    if (!pImpl)
        return;
    // Reset sequence tracking — only safe at session start (not mid-session,
    // where it would cause false gap detection on the receiver).
    pImpl->send_seq_.store(0, std::memory_order_relaxed);
    pImpl->last_seq_.store(0, std::memory_order_relaxed);
    pImpl->expected_seq_ = 0;
    pImpl->seq_initialized_ = false;
}

void ZmqQueue::set_configured_period(uint64_t period_us)
{
    if (!pImpl)
        return;
    pImpl->ctx_metrics_.set_configured_period(period_us);
}

void ZmqQueue::set_checksum_policy(ChecksumPolicy policy)
{
    if (!pImpl)
        return;
    pImpl->checksum_policy_ = policy;
}

} // namespace pylabhub::hub
