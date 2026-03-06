#pragma once
/**
 * @file hub_zmq_queue.hpp
 * @brief ZmqQueue — ZMQ PULL/PUSH-backed Queue implementation.
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
 * ctypes.LittleEndianStructure (natural or packed).  Buffer pointers from
 * read_acquire() / write_acquire() are vector-allocated (>= max_align_t),
 * so typed pointer casts to C++ structs are always safe.
 *
 * @par Read mode
 * A recv_thread_ receives frames, validates and decodes them into a pre-allocated
 * ring buffer (max_depth slots of item_size bytes; zero heap allocation per frame).
 * read_acquire() pops from this ring buffer with a timeout.
 * flexzone is always nullptr (ZMQ transport has no flexzone).
 * Sequence gaps (network drops) are counted by recv_gap_count().
 *
 * @par Write mode
 * write_acquire() returns a pre-allocated struct-layout send buffer. Always returns
 * immediately — there is no write-side back-pressure. @p timeout is ignored.
 * write_commit() encodes and sends the frame (ZMQ_DONTWAIT); drops are counted
 * by send_drop_count(). write_abort() discards without sending.
 * Use send_drop_count() for flow-control feedback when the peer is slow/absent.
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
 * @brief ZMQ PULL (read) or PUSH (write) Queue implementation.
 */
class PYLABHUB_UTILS_EXPORT ZmqQueue final : public Queue
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a read-mode ZmqQueue (ZMQ PULL socket) with schema encoding.
     *
     * item_size is computed from @p schema using C ctypes alignment rules
     * (natural or packed).  Each received frame is decoded field-by-field:
     * scalar fields are type-checked via the msgpack type tag; array/string/bytes
     * fields are size-validated against count * sizeof(element) or length.
     * Mismatches increment recv_frame_error_count() and discard the frame.
     *
     * @param endpoint          ZMQ endpoint (e.g. "tcp://127.0.0.1:5555").
     * @param schema            Field list — must be non-empty; returns nullptr on error.
     * @param packing           "natural" (C struct alignment) or "packed" (no padding).
     *                          Must match the sender's packing.
     * @param bind              If true, bind; otherwise connect.
     * @param max_buffer_depth  Drop oldest item when internal buffer exceeds this depth.
     * @param schema_tag        Optional 8-byte identity guard (first 8 B of BLAKE2b-256
     *                          of BLDS).  Mismatched tags → recv_frame_error_count++.
     */
    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    pull_from(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
              std::string packing,
              bool bind = false, size_t max_buffer_depth = 64,
              std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt);

    /**
     * @brief Create a write-mode ZmqQueue (ZMQ PUSH socket) with schema encoding.
     *
     * item_size is computed from @p schema.  write_commit() encodes each field
     * individually: scalars as their native msgpack type (preserves integer/float
     * distinction on the wire), arrays as bin(count * elem_size), string/bytes as bin.
     *
     * @param endpoint    ZMQ endpoint.
     * @param schema      Field list — must be non-empty and all type_str valid; returns nullptr on error.
     * @param packing     "natural" or "packed".  Must match the receiver's packing.
     * @param bind        If true, bind; otherwise connect.
     * @param schema_tag  Optional 8-byte identity guard embedded in every frame.
     * @param sndhwm      ZMQ_SNDHWM for the PUSH socket.  0 = use ZMQ default (1000).
     *                    Set to a small value (e.g. 1–4) for latency-sensitive pipelines
     *                    to prevent silent buffering when the peer is slow or absent.
     *                    Frames beyond HWM are dropped and counted by send_drop_count().
     */
    [[nodiscard]] static std::unique_ptr<ZmqQueue>
    push_to(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
            std::string packing,
            bool bind = true,
            std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
            int sndhwm = 0);

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

    /** Returns pointer to the internal send buffer; always succeeds immediately.
     *  @p timeout is ignored — use send_drop_count() for flow-control feedback. */
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

    // ── Diagnostics counters ──────────────────────────────────────────────────

    /** Number of incoming payloads dropped because the ring buffer was full. */
    [[nodiscard]] uint64_t recv_overflow_count()    const noexcept;
    /** Number of received frames rejected (bad magic, schema mismatch, wrong size). */
    [[nodiscard]] uint64_t recv_frame_error_count() const noexcept;
    /** Number of sequence number gaps detected (network drops between PUSH and PULL). */
    [[nodiscard]] uint64_t recv_gap_count()         const noexcept;
    /** Number of outgoing frames dropped because zmq_send returned EAGAIN or HWM hit. */
    [[nodiscard]] uint64_t send_drop_count()        const noexcept;

    // ── Lifecycle (overrides Queue no-ops) ────────────────────────────────────

    /**
     * @brief Bind/connect socket and start recv_thread_ (read mode).
     * @return true on success; false if already running, socket setup failed,
     *         or bind/connect failed.  Callers MUST check the return value.
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
