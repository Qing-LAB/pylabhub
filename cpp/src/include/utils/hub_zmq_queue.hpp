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
 *   - schema_tag : first 8 bytes of BLAKE2b-256(BLDS) — schema identity guard
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
 * This is the same 13-type set as FieldDef::type_str (script_host_schema.hpp).
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
 * flexzone is always nullptr (ZMQ transport has no flexzone).
 * Sequence gaps (network drops) are counted by recv_gap_count().
 * last_seq() returns the wire frame seq of the most recently decoded frame (atomic).
 *
 * @par Write mode
 * write_acquire() returns a pointer to an internal send buffer.
 * OverflowPolicy::Drop (default): returns nullptr immediately when the send buffer
 * is full; overrun_count() increments.
 * OverflowPolicy::Block: blocks up to @p timeout waiting for space; overrun_count()
 * increments on timeout.
 * write_commit() enqueues the buffer to an internal send ring; a dedicated
 * send_thread_ drains the ring, encodes msgpack frames, and calls zmq_send with
 * EAGAIN retry (send_retry_count()) until success or stop().
 * On stop() drain, pending items are sent once; EAGAIN causes send_drop_count()++.
 * write_discard() discards without enqueuing.
 * set_checksum_options() and set_verify_checksum() are no-ops (TCP provides integrity).
 *
 * @par Thread safety
 * ZmqQueue is NOT thread-safe for its public API.  Internally, the recv_thread_
 * uses a mutex to protect the ring buffer.  Use from one caller thread only.
 *
 * @par Lifecycle
 * Call start() before first acquire; call stop() before destruction.
 * stop() joins the recv_thread_ (read mode) and closes the ZMQ context.
 */
#include "utils/hub_queue.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ZmqQueueImpl;

/**
 * @struct ZmqSchemaField
 * @brief Describes one typed field for schema-mode ZmqQueue encode/decode.
 *
 * Scalars (count == 1, not string/bytes) → native msgpack type on the wire.
 * Arrays (count > 1) and string/bytes    → msgpack bin (raw bytes, size-validated).
 */
struct ZmqSchemaField
{
    /// "bool","int8","uint8","int16","uint16","int32","uint32",
    /// "int64","uint64","float32","float64","string","bytes"
    std::string type_str;
    /// Elements: 1 = scalar, >1 = array encoded as bin.
    uint32_t    count{1};
    /// For "string"/"bytes": total byte length.  Ignored for numeric types.
    uint32_t    length{0};
};

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
     *                          of BLDS).  Mismatched tags → recv_frame_error_count++.
     */
    [[nodiscard]] static std::unique_ptr<QueueReader>
    pull_from(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
              std::string packing,
              bool bind = false, size_t max_buffer_depth = 64,
              std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt);

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
     *   OverflowPolicy::Drop  — returns nullptr immediately; overrun_count()++.
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
     */
    [[nodiscard]] static std::unique_ptr<QueueWriter>
    push_to(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
            std::string packing,
            bool bind = true,
            std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
            int sndhwm = 0,
            size_t send_buffer_depth = 64,
            OverflowPolicy overflow_policy = OverflowPolicy::Drop,
            int send_retry_interval_ms = 10);

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
    /** Always nullptr for ZmqQueue (no flexzone in ZMQ transport). */
    // read_flexzone() — inherited nullptr default from QueueReader.

    /**
     * @brief Returns the wire frame seq of the most recently decoded frame.
     *
     * Updated by recv_thread_ (atomic, relaxed store). Returns 0 until the
     * first frame is received. Note: this reflects the recv_thread_ perspective
     * (most recently decoded), not the read_acquire() perspective.
     */
    uint64_t last_seq() const noexcept override;

    /**
     * @brief No-op for ZmqQueue (TCP provides transport integrity).
     *
     * ZMQ has its own frame-level validation (magic, schema_tag, field types).
     * BLAKE2b slot verification is not applicable to ZMQ transport.
     */
    void set_verify_checksum(bool, bool) noexcept override {}

    /** ZMQ recv buffer depth (max_buffer_depth configured at construction). */
    size_t      capacity()    const override;
    /** Returns "zmq_pull_ring_N" where N = max_buffer_depth. */
    std::string policy_info() const override;

    // ── QueueWriter interface — writing ───────────────────────────────────────

    /**
     * @brief Acquire the internal send buffer.
     *
     * Drop policy: returns nullptr immediately if send buffer is full (overrun_count()++).
     * Block policy: waits up to @p timeout for space; returns nullptr on timeout (overrun_count()++).
     * @p timeout is ignored for Drop policy.
     */
    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    /** Enqueues buffer to the internal send ring; send_thread_ delivers asynchronously. */
    void  write_commit() noexcept override;
    /** Discards the write buffer without sending. */
    void  write_discard() noexcept override;
    /** Always nullptr for ZmqQueue. */
    // write_flexzone() — inherited nullptr default from QueueWriter.

    /**
     * @brief No-op for ZmqQueue (TCP provides transport integrity).
     */
    void set_checksum_options(bool, bool) noexcept override {}

    /** ZMQ send buffer depth (send_buffer_depth configured at construction). */
    // capacity() — shared implementation; returns mode-appropriate depth.
    /** Returns "zmq_push_drop" or "zmq_push_block" depending on overflow_policy. */
    // policy_info() — shared implementation.

    // ── Shared metadata (both QueueReader and QueueWriter) ────────────────────

    size_t      item_size()     const noexcept override;
    // flexzone_size() — inherited 0 default from both bases.
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
    [[nodiscard]] uint64_t overrun_count()          const noexcept;

    /** Unified metrics snapshot (implements QueueReader::metrics() + QueueWriter::metrics()). */
    QueueMetrics metrics() const noexcept override;

    // ── Lifecycle (overrides both QueueReader and QueueWriter no-ops) ──────────

    /**
     * @brief Bind/connect socket and start recv_thread_ (read mode) or send_thread_ (write mode).
     * @return true on success; false if already running, socket setup failed,
     *         or bind/connect failed.  Callers MUST check the return value.
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
