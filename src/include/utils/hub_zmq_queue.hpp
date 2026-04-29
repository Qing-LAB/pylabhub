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

/**
 * @class ZmqQueue
 * @brief ZMQ PULL (read) or PUSH (write) QueueReader/QueueWriter implementation.
 *
 * Factories return the appropriate abstract base pointer:
 *   pull_from() → unique_ptr<QueueReader>
 *   push_to()   → unique_ptr<QueueWriter>
 */
class PYLABHUB_UTILS_EXPORT ZmqQueue final : public QueueReader, public QueueWriter
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a read-mode ZmqQueue (ZMQ PULL socket) with schema encoding.
     *
     * Returns a QueueReader*. item_size is computed from @p schema using C ctypes
     * alignment rules (aligned or packed). Each received frame is decoded field-by-field:
     * scalar fields are type-checked via the msgpack type tag; array/string/bytes
     * fields are size-validated against count * sizeof(element) or length.
     * Mismatches increment recv_frame_error_count() and discard the frame.
     *
     * @param endpoint          ZMQ endpoint (e.g. "tcp://127.0.0.1:5555").
     * @param schema            Field list — must be non-empty; returns nullptr on error.
     * @param packing           "aligned" (C struct alignment) or "packed" (no padding).
     *                          Must match the sender's packing.
     * @param bind              If true, bind; otherwise connect.
     * @param max_buffer_depth  Drop oldest item when internal buffer exceeds this depth.
     * @param schema_tag        Optional 8-byte identity guard (first 8 B of BLAKE2b-256
     *                          over the HEP-CORE-0034 §6.3 canonical wire form,
     *                          via `compute_schema_hash(slot_spec, fz_spec)`).
     *                          Mismatched tags → recv_frame_error_count++.
     * @param instance_id       Caller-provided stable identifier (e.g. role_tag+uid+":rx").
     *                          Used as the ThreadManager owner_id for the recv thread;
     *                          MUST be unique across every ZmqQueue live at the same
     *                          time in this process. Empty → a per-pointer-address
     *                          id is used as a safe fallback (unique but opaque).
     */
    [[nodiscard]] static std::unique_ptr<QueueReader>
    pull_from(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
              std::string packing,
              bool bind = false, size_t max_buffer_depth = kZmqDefaultBufferDepth,
              std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
              std::string instance_id = {});

    /**
     * @brief Create a write-mode ZmqQueue (ZMQ PUSH socket) with schema encoding.
     *
     * Returns a QueueWriter*. write_commit() enqueues the written item to an internal
     * send buffer (depth @p send_buffer_depth). A dedicated send_thread_ drains the
     * buffer, encodes each item as a msgpack frame, and sends it via zmq_send(ZMQ_DONTWAIT).
     * On EAGAIN (ZMQ HWM exceeded), send_thread_ retries after @p send_retry_interval_ms.
     * On stop(), pending items are sent if possible; remaining items are dropped.
     *
     * write_acquire() is non-blocking when the buffer has space.  When full:
     *   OverflowPolicy::Drop  — returns nullptr immediately; data_drop_count()++.
     *   OverflowPolicy::Block — blocks until space is available or @p timeout elapses.
     *
     * @param endpoint              ZMQ endpoint.
     * @param schema                Field list — non-empty, all type_str valid; nullptr on error.
     * @param packing               "aligned" or "packed".  Must match the receiver.
     * @param bind                  If true, bind; otherwise connect (default: bind).
     * @param schema_tag            Optional 8-byte identity guard embedded in every frame.
     * @param sndhwm                ZMQ_SNDHWM for the PUSH socket.  0 = ZMQ default (1000).
     *                              Set to 1–4 for latency-sensitive pipelines.
     * @param send_buffer_depth     Internal send ring buffer depth (default 64).
     *                              write_acquire() respects OverflowPolicy when full.
     * @param overflow_policy       Drop (default) or Block when send buffer is full.
     * @param send_retry_interval_ms Milliseconds between EAGAIN retries in send_thread_
     *                              (default 10).  Set to match the role's target_period_ms.
     * @param instance_id           Caller-provided stable identifier (e.g. role_tag+uid+":tx").
     *                              Used as the ThreadManager owner_id for the send thread;
     *                              MUST be unique across every ZmqQueue live at the same
     *                              time in this process. Empty → a per-pointer-address id
     *                              is used as a safe fallback.
     */
    [[nodiscard]] static std::unique_ptr<QueueWriter>
    push_to(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
            std::string packing,
            bool bind = true,
            std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
            int sndhwm = 0,
            size_t send_buffer_depth = kZmqDefaultBufferDepth,
            OverflowPolicy overflow_policy = OverflowPolicy::Drop,
            int send_retry_interval_ms = 10,
            std::string instance_id = {});

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

private:
    explicit ZmqQueue(std::unique_ptr<ZmqQueueImpl> impl);
    std::unique_ptr<ZmqQueueImpl> pImpl;
};

} // namespace pylabhub::hub
