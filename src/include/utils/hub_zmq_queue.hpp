#pragma once
/**
 * @file hub_zmq_queue.hpp
 * @brief ZmqQueue — ZMQ PULL/PUSH-backed QueueReader/QueueWriter implementation.
 *
 * Wraps a raw ZMQ PULL socket (read mode) or PUSH socket (write mode).
 * No Messenger, no broker registration, no protocol — direct point-to-point ZMQ.
 *
 * @par Wire format (MessagePack array, 4 elements)
 * Each ZMQ message is encoded as a msgpack fixarray of 4 elements:
 *   [magic:uint32, schema_tag:bin8, seq:uint64, payload]
 *   - magic      : 0x51484C50 ('PLHQ') — frame identity guard
 *   - schema_tag : first 8 bytes of BLAKE2b-256 over the HEP-CORE-0034 §6.3
 *                  canonical wire form (`compute_schema_hash(slot_spec, fz_spec)`);
 *                  NOT the HEP-0002 BLDS short form — schema identity guard
 *   - seq        : monotonic send counter (uint64, wraps around); gaps are counted
 *   - payload    : msgpack array of N typed field values:
 *       scalar field  → native msgpack type (int32, float64, bool, …)
 *       array field   → bin(count * elem_size) — raw element bytes, size-validated
 *       string/bytes  → bin(length)
 *
 * @par Schema mode — type safety
 * Each field is encoded with its declared type.  The receiver validates:
 *   - payload element [3] is an array of exactly N elements (field count)
 *   - each scalar element has the correct msgpack type (integer vs float)
 *   - each bin element has exactly the expected byte size
 * Type or size mismatches increment recv_frame_error_count() and discard the frame.
 *
 * @par Schema requirement
 * Every ZmqQueue MUST be created with a non-empty ZmqSchemaField list.
 * Factories return nullptr (with an error log) if the schema is empty or any
 * type_str is invalid.  Valid type_str values: "bool","int8","uint8","int16",
 * "uint16","int32","uint32","int64","uint64","float32","float64","string","bytes".
 * This is the same 13-type set as FieldDef::type_str (schema_types.hpp).
 * Note: the BLDS named schema registry uses "char" instead of "string"/"bytes" —
 * see SchemaFieldDef (schema_def.hpp) for the BLDS→FieldDef conversion rules.
 *
 * @par item_size
 * Computed from the ZmqSchemaField list using the same alignment rules as Python
 * ctypes.LittleEndianStructure (aligned or packed).  Buffer pointers from
 * read_acquire() / write_acquire() are vector-allocated (>= max_align_t),
 * so typed pointer casts to C++ structs are always safe.
 *
 * @par Read mode
 * A recv_thread_ receives frames, validates and decodes them into a pre-allocated
 * ring buffer (max_depth slots of item_size bytes; zero heap allocation per frame).
 * read_acquire() pops from this ring buffer with a timeout.
 * Sequence gaps (network drops) are counted by recv_gap_count().
 * last_seq() returns the wire frame seq of the most recently decoded frame (atomic).
 *
 * @par Write mode
 * write_acquire() returns a pointer to an internal send buffer.
 * OverflowPolicy::Drop (default): returns nullptr immediately when the send buffer
 * is full; data_drop_count() increments.
 * OverflowPolicy::Block: blocks up to @p timeout waiting for space; data_drop_count()
 * increments on timeout.
 * write_commit() enqueues the buffer to an internal send ring; a dedicated
 * send_thread_ drains the ring, encodes msgpack frames, and sends via cppzmq
 * (zmq::send_flags::dontwait) with EAGAIN retry (send_retry_count()) until
 * success or stop(). On stop() drain, pending items are sent once; EAGAIN
 * causes send_drop_count()++. write_discard() discards without enqueuing.
 *
 * @par Thread safety
 * ZmqQueue is NOT thread-safe for its public API.  Internally, the recv_thread_
 * uses a mutex to protect the ring buffer.  Use from one caller thread only.
 *
 * @par ZMQ context
 * Sockets are created from the shared process-wide zmq::context_t owned by the
 * `ZMQContext` lifecycle module (utils/zmq_context.hpp). ZmqQueue never creates
 * or terminates the context — the top-level LifecycleGuard must include
 * `pylabhub::hub::GetZMQContextModule()`, which is persistent and outlives
 * every ZmqQueue instance.
 *
 * @par Lifecycle
 * Call start() before first acquire; call stop() before destruction. stop()
 * signals the background threads, joins them with per-thread bounded timeout
 * via ThreadManager (detaches on timeout with ERROR log), and then closes the
 * socket. The shared ZMQ context is NOT closed here.
 */
#include "utils/hub_queue.hpp"
#include "utils/schema_field_layout.hpp"
#include "utils/security/curve_keypair.hpp"   // Z85PublicKey strong type
#include "utils/security/key_store.hpp"       // kRoleIdentityName canonical default
#include "utils/security/peer_admission.hpp"  // PeerAdmission base + PeerAllowlist

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ZmqQueueImpl;

/// Default depth for ZmqQueue internal ring buffers (PULL recv ring and PUSH send ring).
/// Used as the default argument in `pull_from()` / `push_to()` and as the default for
/// the role-config `zmq_buffer_depth` field so all callers share the same default
/// without embedding a magic 64.
inline constexpr size_t kZmqDefaultBufferDepth = 64;

/**
 * @typedef ZmqSchemaField
 * @brief Alias for SchemaFieldDesc — backward-compatible name used by ZmqQueue/InboxQueue.
 *
 * The canonical type definition is in schema_field_layout.hpp.
 */
using ZmqSchemaField = SchemaFieldDesc;

/// HEP-CORE-0017 §3.3 + HEP-CORE-0036 §4.1 / §6.4 — descriptor for a
/// single producer the consumer's RX queue may receive data from.
/// One entry per producer in `CONSUMER_REG_ACK.producers[]`.  For
/// ZMQ transport `producer_peers` admits N entries (fan-in); for
/// SHM transport `producer_peers.size() ≤ 1` (HEP-CORE-0007 §12.4a
/// `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`).  Scripts never see this
/// surface; ZmqQueue uses these internally for connect direction +
/// per-peer ZAP cache.
struct ProducerPeer
{
    /// Producer's role uid (HEP-CORE-0033 §G2.2.0b).
    std::string role_uid;

    /// `tcp://host:port` per HEP-CORE-0021 §16.
    std::string endpoint;

    /// Producer's identity pubkey, Z85-encoded 40 chars
    /// (HEP-CORE-0036 §I6).  Used as the consumer-side
    /// `curve_serverkey` for the data-plane CURVE handshake.  The
    /// broker-side ZAP installed on the producer's PUSH socket
    /// (HEP-CORE-0036 §7) is what gates whether this consumer's
    /// pubkey is admitted; this field is the dual half the consumer
    /// needs to authenticate the producer's identity in turn.
    std::string pubkey_z85;
};

/// Negotiated transport-level authentication mechanism of a started
/// `ZmqQueue` socket — observed directly from libzmq via
/// `zmq_getsockopt(ZMQ_MECHANISM)` at `start()` time and cached in an
/// atomic for thread-safe read by callers.
///
/// **Post-C4 invariant (HEP-CORE-0035 §2 + AUTH_TODO §C5, #161).** A
/// successfully-started ZmqQueue MUST report `Curve`.  The `start()`
/// guard enforces this — it fails if libzmq reports anything else,
/// so a regression that loses CURVE wiring surfaces immediately
/// instead of silently shipping plaintext data.  Callers may use
/// `ZmqQueue::mechanism()` as the single observation point:
///
/// ```cpp
/// EXPECT_EQ(queue.mechanism(), pylabhub::hub::Mechanism::Curve);
/// ```
///
/// Three values rather than two: a not-yet-started queue is
/// genuinely different from a queue that started with auth turned
/// OFF — the latter is the regression we want to surface, the
/// former is fine.  `Plaintext` collapses libzmq `ZMQ_NULL` /
/// `ZMQ_PLAIN` because neither is a valid post-C4 state (HEP-0035
/// §2 — CURVE is unconditional on every role↔hub data path).
///
/// **Concurrency contract — `is_running() ⇒ mechanism() == Curve`
/// once start() returns true.**  The observable is written via
/// release-ordered atomic stores; readers use acquire ordering.
/// `stop()` clears `running_` *before* resetting `mechanism_`, so
/// the implication holds at stop time.  During `start()` setup
/// there is a brief window (between the `running_=true` reservation
/// at the top of `start()` and the `mechanism_=Curve` store after
/// the libzmq mechanism query) where a concurrent reader can see
/// `is_running()==true && mechanism()==Uninitialized`.  This is
/// the INTENDED polling pattern for off-thread observers: poll
/// `mechanism()` and treat `Curve` as the signal that CURVE
/// engagement is confirmed.  The L2 tests at `test_hub_zmq_queue.cpp`
/// "Mechanism_AfterPushBind_IsCurve" assert the post-start value
/// from the same thread that called start() — no race.
enum class Mechanism
{
    Uninitialized,  ///< `start()` not called, or `stop()` reset the field.
    Plaintext,      ///< Reserved.  No ZmqQueue ever publishes this value:
                    ///  the `start()` guard throws BEFORE writing the
                    ///  mechanism field on any non-CURVE observation,
                    ///  and the catch handler restores `Uninitialized`.
                    ///  Kept as a named enum value so callers comparing
                    ///  arbitrary libzmq mechanisms have a stable name
                    ///  for the non-CURVE case.
    Curve,          ///< libzmq reported `ZMQ_CURVE`.  The only acceptable post-start value.
};

/// String name for a `Mechanism` value — stable surface for script
/// bindings and telemetry sinks (HEP-CORE-0035 §2 + AUTH_TODO §C5
/// follow-up #186).  Used by `RoleAPIBase::queue_mechanism` consumers
/// in Lua / Python / JSON paths so the script side never depends on
/// the underlying integer encoding of the enum.
[[nodiscard]] constexpr const char *mechanism_name(Mechanism m) noexcept
{
    switch (m)
    {
        case Mechanism::Uninitialized: return "Uninitialized";
        case Mechanism::Plaintext:     return "Plaintext";
        case Mechanism::Curve:         return "Curve";
    }
    return "Uninitialized";  // unreachable; silences -Wreturn-type
}

/**
 * @class ZmqQueue
 * @brief ZMQ PULL (read) or PUSH (write) QueueReader/QueueWriter implementation.
 *
 * Inherits `PeerAdmission` so the PUSH side can be the broker-glue
 * target for `set_peer_allowlist` calls.  On the PULL side the
 * inherited methods are inert (no allowlist concept — the consumer
 * trusts the server via `curve_serverkey`).
 *
 * Factories (HEP-CORE-0035 §2 — CURVE unconditional):
 *   pull_from() / push_to() → unique_ptr<ZmqQueue>
 *
 * Both factories return the concrete type so callers can access the
 * `PeerAdmission` interface to push allowlist updates.  Phase D's
 * broker glue uses this to drive `set_peer_allowlist` on the
 * producer-side queue from the broker thread.
 */
class PYLABHUB_UTILS_EXPORT ZmqQueue final
    : public QueueReader,
      public QueueWriter,
      public pylabhub::utils::security::PeerAdmission
{
public:
    // ── Factories (HEP-CORE-0040 §8.4 — CURVE unconditional) ────────────────
    //
    // Canonical CURVE-wired entry points per HEP-CORE-0035 §2 + §4.6.5
    // (CURVE on every role↔hub data path) + HEP-CORE-0040 §8.4 +
    // AUTH_TODO §C2/§C4 (#158, #160): discrete `identity_key_name`
    // (KeyStore lookup) + `Z85PublicKey server_pubkey` (PULL only) +
    // `zap_domain` (PUSH only).  No `ZmqAuthOptions` struct; no
    // `initial_allowlist` parameter — callers seed via
    // `set_peer_allowlist()` AFTER `start()`.  Production callers
    // rely on the broker's Phase D `CHANNEL_AUTH_UPDATE` push; the
    // deny-all default is the safe starting point.
    //
    // Return the concrete `ZmqQueue` so callers can drive the
    // `PeerAdmission` interface directly (Phase D broker glue calls
    // `set_peer_allowlist` on the PUSH-side queue when
    // `CHANNEL_AUTH_UPDATE` arrives).
    //
    // Legacy plaintext public `pull_from()` / `push_to()` were
    // deleted in #160 (C4); the bare names now refer exclusively to
    // the CURVE-only factories.  The plaintext queue-building logic
    // moved to the private `build_plaintext_reader_` /
    // `build_plaintext_writer_` helpers below — they're not callable
    // from outside this class.

    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    pull_from(const std::string& endpoint,
            ::pylabhub::utils::security::Z85PublicKey server_pubkey,
            std::vector<ZmqSchemaField> schema,
            std::string packing,
            std::string_view identity_key_name =
                ::pylabhub::utils::security::kRoleIdentityName,
            bool bind = false,
            size_t max_buffer_depth = kZmqDefaultBufferDepth,
            std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
            std::string instance_id = {});

    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    push_to(const std::string& endpoint,
            std::vector<ZmqSchemaField> schema,
            std::string packing,
            std::string_view identity_key_name =
                ::pylabhub::utils::security::kRoleIdentityName,
            std::string zap_domain = {},
            bool bind = true,
            std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
            int sndhwm = 0,
            size_t send_buffer_depth = kZmqDefaultBufferDepth,
            OverflowPolicy overflow_policy = OverflowPolicy::Drop,
            int send_retry_interval_ms = 10,
            std::string instance_id = {});

private:
    // ─── Internal plaintext queue builders ─────────────────────────────────
    //
    // The public `pull_from` / `push_to` factories above wire CURVE on
    // top of a bare (plaintext) queue.  The bare-queue construction
    // logic lives in these internal helpers — they're NOT a public
    // API because exposing a plaintext path would re-introduce the
    // bypass risk HEP-CORE-0035 §2 + HEP-CORE-0040 §8.4 (#160 C4)
    // close.  Only callable from this class's own implementations.
    [[nodiscard]] static std::unique_ptr<QueueReader>
    build_plaintext_reader_(const std::string& endpoint,
                            std::vector<ZmqSchemaField> schema,
                            std::string packing,
                            bool bind, size_t max_buffer_depth,
                            std::optional<std::array<uint8_t, 8>> schema_tag,
                            std::string instance_id);

    [[nodiscard]] static std::unique_ptr<QueueWriter>
    build_plaintext_writer_(const std::string& endpoint,
                            std::vector<ZmqSchemaField> schema,
                            std::string packing,
                            bool bind,
                            std::optional<std::array<uint8_t, 8>> schema_tag,
                            int sndhwm,
                            size_t send_buffer_depth,
                            OverflowPolicy overflow_policy,
                            int send_retry_interval_ms,
                            std::string instance_id);

public:

    // ── PeerAdmission overrides (Phase A interface) ────────────────────────────
    //
    // On the PUSH/bind side: storage backed by PortableAtomicSharedPtr
    // for lock-free reads from the ZapRouter pump thread.
    //
    // On the PULL/connect side: inert.  set_peer_allowlist returns
    // false (no allowlist on a client socket — admission is the
    // server's concern, gated via curve_serverkey).
    // peer_allowlist_snapshot returns nullopt.  is_peer_allowed
    // returns false unconditionally (no inbound handshakes to gate).
    // admission_is_enforced returns true iff CURVE keys were
    // supplied (the consumer side is "enforced" in the sense that
    // it presents identity to the server's ZAP).

    bool set_peer_allowlist(
        pylabhub::utils::security::PeerAllowlist allowlist) override;
    [[nodiscard]] std::optional<pylabhub::utils::security::PeerAllowlist>
    peer_allowlist_snapshot() const override;
    [[nodiscard]] bool is_peer_allowed(
        const pylabhub::utils::security::PeerIdentity& peer) const override;
    [[nodiscard]] bool admission_is_enforced() const noexcept override;

    // ── Dynamic producer-peer membership (HEP-CORE-0017 §3.3, #103 A2) ──────────
    //
    // PULL/connect side: tracks the set of producers the consumer is
    // currently authorized to receive from.  Driven by the role-host
    // framework in response to HEP-CORE-0033 §12 channel-event
    // broadcasts (producer joined / left) and by the post-§6.5 notify-
    // then-pull cycle.  Today the impl is metadata-only — Pattern A vs
    // Pattern B socket-level fan-in is HEP-CORE-0017 §3.3 future work;
    // the single-producer connect endpoint still flows through
    // `pull_from()`'s `endpoint` parameter.
    // The methods exist now so the A3 dispatch layer has a stable
    // surface to call.
    //
    // PUSH/bind side: inert.  The producer doesn't "add peers"; consumer
    // admission is handled via the broker-pushed allowlist
    // (`set_peer_allowlist`) + the producer's ZAP handler installed at
    // bind time.  Calling these on a PUSH-side queue returns false +
    // logs once at INFO.

    /// Append a producer to this queue's peer set.  Idempotent on
    /// `role_uid` collision (existing entry overwritten in place).
    /// Returns true on success; false on PUSH side or null impl.
    bool add_producer_peer(const ProducerPeer& peer);

    /// Remove a producer from this queue's peer set by `role_uid`.
    /// Returns true if a peer was removed; false otherwise (not found
    /// or PUSH side).  Per HEP-CORE-0036 I5 "revocation is forward-
    /// looking": frames already received are not discarded.
    bool remove_producer_peer(const std::string& role_uid);

    /// Number of producer peers currently tracked.  Returns 0 on PUSH
    /// side or null impl.  Diagnostic + test observability.
    [[nodiscard]] std::size_t producer_peer_count() const noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ZmqQueue() override;
    ZmqQueue(ZmqQueue&&) noexcept;
    ZmqQueue& operator=(ZmqQueue&&) noexcept;
    ZmqQueue(const ZmqQueue&) = delete;
    ZmqQueue& operator=(const ZmqQueue&) = delete;

    // ── QueueReader interface — reading ────────────────────────────────────────

    /** Pops one item from the internal buffer; blocks up to @p timeout. */
    const void* read_acquire(std::chrono::milliseconds timeout) noexcept override;
    /** No-op — item is already consumed from the internal buffer. */
    void        read_release() noexcept override;
    /**
     * @brief Returns the wire frame seq of the most recently decoded frame.
     *
     * Updated by recv_thread_ (atomic, relaxed store). Returns 0 until the
     * first frame is received. Note: this reflects the recv_thread_ perspective
     * (most recently decoded), not the read_acquire() perspective.
     */
    uint64_t last_seq() const noexcept override;

    /** ZMQ recv buffer depth (max_buffer_depth configured at construction). */
    size_t      capacity()    const override;
    /** Returns "zmq_pull_ring_N" where N = max_buffer_depth. */
    std::string policy_info() const override;

    // ── QueueWriter interface — writing ───────────────────────────────────────

    /**
     * @brief Acquire the internal send buffer.
     *
     * Drop policy: returns nullptr immediately if send buffer is full (data_drop_count()++).
     * Block policy: waits up to @p timeout for space; returns nullptr on timeout (data_drop_count()++).
     * @p timeout is ignored for Drop policy.
     */
    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    /** Enqueues buffer to the internal send ring; send_thread_ delivers asynchronously. */
    void  write_commit() noexcept override;
    /** Discards the write buffer without sending. */
    void  write_discard() noexcept override;
    /** ZMQ send buffer depth (send_buffer_depth configured at construction). */
    // capacity() — shared implementation; returns mode-appropriate depth.
    /** Returns "zmq_push_drop" or "zmq_push_block" depending on overflow_policy. */
    // policy_info() — shared implementation.

    // ── Shared metadata (both QueueReader and QueueWriter) ────────────────────

    size_t      item_size()     const noexcept override;
    std::string name()          const override;

    /**
     * @brief Returns the actual bound endpoint after start().
     *
     * When the configured endpoint uses port 0 (OS-assigned), returns the
     * resolved endpoint (e.g. "tcp://127.0.0.1:54321") after start().
     * For connect-mode sockets, returns the configured endpoint.
     * Before start(), returns the configured endpoint string.
     * Useful for tests and callers that bind to port 0 and need the peer address.
     */
    [[nodiscard]] std::string actual_endpoint() const;

    // ── Diagnostics counters ──────────────────────────────────────────────────
    // Individual accessors retained for backward compatibility.
    // Prefer metrics() for a unified snapshot.

    /** PULL: items dropped because the internal recv ring was full (oldest discarded). */
    [[nodiscard]] uint64_t recv_overflow_count()    const noexcept;
    /** PULL: frames rejected (bad magic, schema tag mismatch, field type/size error). */
    [[nodiscard]] uint64_t recv_frame_error_count() const noexcept;
    /** PULL: sequence number gaps (frames lost between PUSH and PULL). */
    [[nodiscard]] uint64_t recv_gap_count()         const noexcept;
    /** PUSH: frames permanently dropped (zmq_send error during stop drain). */
    [[nodiscard]] uint64_t send_drop_count()        const noexcept;
    /** PUSH: EAGAIN retries by send_thread_ (ZMQ HWM temporarily exceeded). */
    [[nodiscard]] uint64_t send_retry_count()       const noexcept;
    /** PUSH: write_acquire() returned nullptr (send buffer full, Drop/Block timeout). */
    [[nodiscard]] uint64_t data_drop_count()          const noexcept;

    /** Unified metrics snapshot — timing (D2+D3) + transport counters. */
    QueueMetrics metrics() const noexcept override;
    /** Reset counters and timing (preserves sequence state for mid-session use). */
    void reset_metrics() override;
    /** Full init: reset_metrics() + sequence state. Call at session start only. */
    void init_metrics() override;
    /** Set target loop period (informational, reported in metrics). 0 = MaxRate. */
    void set_configured_period(uint64_t period_us) override;

    // ── Unified checksum interface ───────────────────────────────────────────
    void set_checksum_policy(ChecksumPolicy policy) override;
    // set_flexzone_checksum: base class no-op (ZMQ has no flexzone)
    // update_flexzone_checksum: base class no-op
    // verify_flexzone_checksum: base class returns true

    // ── Lifecycle (overrides both QueueReader and QueueWriter no-ops) ──────────

    /**
     * @brief Bind/connect socket and start recv_thread_ (read mode) or send_thread_ (write mode).
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure (socket/bind/connect error).
     */
    bool start() override;

    /**
     * @brief Stop recv_thread_ (read mode) or send_thread_ (write mode).
     *
     * Wakes any blocking read_acquire() immediately.  Safe to call multiple times.
     */
    void stop() override;

    bool is_running() const noexcept override;

    /// Negotiated CURVE mechanism observed at `start()` time.  See
    /// the `Mechanism` enum docstring above for the invariant —
    /// post-C4 this MUST be `Curve` whenever the queue is running.
    /// Thread-safe: read from any thread; written only by
    /// `start()`/`stop()` internally via an atomic.
    [[nodiscard]] Mechanism mechanism() const noexcept;

private:
    explicit ZmqQueue(std::unique_ptr<ZmqQueueImpl> impl);
    std::unique_ptr<ZmqQueueImpl> pImpl;
};

} // namespace pylabhub::hub
