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

#include <msgpack.hpp>
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

/// Frame magic: 'P','L','H','Q'
static constexpr uint32_t kFrameMagic = 0x51484C50u;

/// Rate-limit interval for schema tag mismatch warnings.
static constexpr std::chrono::seconds kMismatchWarnInterval{1};

// ============================================================================
// Type-string validation  [ZQ1]
// ============================================================================

static bool is_valid_type_str(const std::string& t) noexcept
{
    return t == "bool"    || t == "int8"   || t == "uint8"  ||
           t == "int16"   || t == "uint16" || t == "int32"  ||
           t == "uint32"  || t == "int64"  || t == "uint64" ||
           t == "float32" || t == "float64"||
           t == "string"  || t == "bytes";
}

// ============================================================================
// Field layout helpers
// ============================================================================

static size_t field_elem_size(const std::string& t) noexcept
{
    if (t == "bool" || t == "int8"  || t == "uint8")             return 1;
    if (t == "int16" || t == "uint16")                            return 2;
    if (t == "int32" || t == "uint32" || t == "float32")         return 4;
    if (t == "int64" || t == "uint64" || t == "float64")         return 8;
    return 1; // string/bytes: caller uses length directly
}

static size_t field_align(const std::string& t) noexcept
{
    if (t == "string" || t == "bytes") return 1;
    return field_elem_size(t);
}

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
    int         sndhwm{0}; ///< ZMQ_SNDHWM for PUSH socket (0 = ZMQ default 1000)

    // ── Schema tag (optional — 8 bytes of BLAKE2b-256 of BLDS) ───────────────
    std::array<uint8_t, 8> schema_tag_{};
    bool                   has_schema_tag_{false};

    // ── Schema-based field encoding ───────────────────────────────────────────
    struct FieldDesc
    {
        size_t      offset;     ///< byte offset in struct buffer
        size_t      byte_size;  ///< total bytes (count*elem_size, or length for str/bytes)
        std::string type_str;
        bool        is_bin;     ///< encode/decode as msgpack bin (arrays, string, bytes)
    };
    std::vector<FieldDesc> schema_defs_;
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

    // ── Write mode ───────────────────────────────────────────────────────────
    std::vector<std::byte> write_buf_;
    msgpack::sbuffer       write_sbuf_;     ///< reused per write_commit() — avoids heap alloc
    std::atomic<uint64_t>  send_seq_{0};

    // ── Counters ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> recv_overflow_count_{0};
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0};  ///< [ZQ10] sequence gaps
    std::atomic<uint64_t> send_drop_count_{0};

    // ── Sequence tracking [ZQ10] ─────────────────────────────────────────────
    uint64_t expected_seq_{0};
    bool     seq_initialized_{false};

    // ── Rate-limited mismatch warning [ZQ6] ──────────────────────────────────
    std::chrono::steady_clock::time_point last_mismatch_warn_{};

    std::atomic<bool> running_{false};

    // ────────────────────────────────────────────────────────────────────────
    // pack_field — encode one FieldDesc from src buffer into msgpack packer.
    static void pack_field(msgpack::packer<msgpack::sbuffer>& pk,
                           const FieldDesc& fd, const char* src)
    {
        const char* p = src + fd.offset;
        if (fd.is_bin)
        {
            pk.pack_bin(static_cast<int>(fd.byte_size));
            pk.pack_bin_body(p, fd.byte_size);
            return;
        }
        // Scalar — native msgpack type preserves wire type tag for validation.
        if      (fd.type_str == "bool")    pk.pack(*reinterpret_cast<const bool*>(p));
        else if (fd.type_str == "int8")    pk.pack(*reinterpret_cast<const int8_t*>(p));
        else if (fd.type_str == "uint8")   pk.pack(*reinterpret_cast<const uint8_t*>(p));
        else if (fd.type_str == "int16")   pk.pack(*reinterpret_cast<const int16_t*>(p));
        else if (fd.type_str == "uint16")  pk.pack(*reinterpret_cast<const uint16_t*>(p));
        else if (fd.type_str == "int32")   pk.pack(*reinterpret_cast<const int32_t*>(p));
        else if (fd.type_str == "uint32")  pk.pack(*reinterpret_cast<const uint32_t*>(p));
        else if (fd.type_str == "int64")   pk.pack(*reinterpret_cast<const int64_t*>(p));
        else if (fd.type_str == "uint64")  pk.pack(*reinterpret_cast<const uint64_t*>(p));
        else if (fd.type_str == "float32") pk.pack(*reinterpret_cast<const float*>(p));
        else if (fd.type_str == "float64") pk.pack(*reinterpret_cast<const double*>(p));
        // No else: factory validated all type strings at construction time [ZQ1].
    }

    // unpack_field — decode one msgpack object into dst buffer at fd.offset.
    // Returns false on type mismatch or bin size mismatch.
    static bool unpack_field(const msgpack::object& obj,
                             const FieldDesc& fd, char* dst) noexcept
    {
        char* p = dst + fd.offset;
        try
        {
            if (fd.is_bin)
            {
                if (obj.type != msgpack::type::BIN || obj.via.bin.size != fd.byte_size)
                    return false;
                std::memcpy(p, obj.via.bin.ptr, fd.byte_size);
                return true;
            }
            // Scalar: msgpack::convert() checks type compatibility and throws on mismatch.
            if      (fd.type_str == "bool")    { bool     v; obj.convert(v); *reinterpret_cast<bool*>(p)     = v; }
            else if (fd.type_str == "int8")    { int8_t   v; obj.convert(v); *reinterpret_cast<int8_t*>(p)   = v; }
            else if (fd.type_str == "uint8")   { uint8_t  v; obj.convert(v); *reinterpret_cast<uint8_t*>(p)  = v; }
            else if (fd.type_str == "int16")   { int16_t  v; obj.convert(v); *reinterpret_cast<int16_t*>(p)  = v; }
            else if (fd.type_str == "uint16")  { uint16_t v; obj.convert(v); *reinterpret_cast<uint16_t*>(p) = v; }
            else if (fd.type_str == "int32")   { int32_t  v; obj.convert(v); *reinterpret_cast<int32_t*>(p)  = v; }
            else if (fd.type_str == "uint32")  { uint32_t v; obj.convert(v); *reinterpret_cast<uint32_t*>(p) = v; }
            else if (fd.type_str == "int64")   { int64_t  v; obj.convert(v); *reinterpret_cast<int64_t*>(p)  = v; }
            else if (fd.type_str == "uint64")  { uint64_t v; obj.convert(v); *reinterpret_cast<uint64_t*>(p) = v; }
            else if (fd.type_str == "float32") { float    v; obj.convert(v); *reinterpret_cast<float*>(p)    = v; }
            else if (fd.type_str == "float64") { double   v; obj.convert(v); *reinterpret_cast<double*>(p)   = v; }
            else return false;
        }
        catch (...) { return false; }
        return true;
    }

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

            // ZMQ truncates to buffer size — treat >= max_frame_sz_ as truncation. [ZQ4]
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
                if (magic != kFrameMagic)
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
                        if (seq != expected_seq_)
                            recv_gap_count_.fetch_add(seq - expected_seq_,
                                                      std::memory_order_relaxed);
                    }
                    expected_seq_    = seq + 1;
                    seq_initialized_ = true;
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
                    if (!unpack_field(arr.via.array.ptr[i], schema_defs_[i], dst))
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
};

// ============================================================================
// Schema layout computation (file-scope helpers)
// ============================================================================

/// Computed layout for one field (includes offset derived from alignment rules).
struct FieldLayout
{
    size_t      offset;
    size_t      byte_size;
    std::string type_str;
    bool        is_bin;
};

/// Compute per-field offsets using ctypes.LittleEndianStructure alignment rules.
/// Returns {field_layouts, total_struct_size}.
static std::pair<std::vector<FieldLayout>, size_t>
compute_field_layout(const std::vector<ZmqSchemaField>& fields,
                     const std::string& packing)
{
    const bool packed = (packing == "packed");
    std::vector<FieldLayout> result;
    size_t offset    = 0;
    size_t max_align = 1;

    for (const auto& f : fields)
    {
        const bool is_blob = (f.type_str == "string" || f.type_str == "bytes");
        const bool is_bin  = is_blob || (f.count > 1);

        size_t esz   = is_blob ? static_cast<size_t>(f.length)
                               : field_elem_size(f.type_str);
        size_t align = (packed || is_blob) ? size_t{1} : field_align(f.type_str);
        size_t total = is_blob ? static_cast<size_t>(f.length)
                               : esz * static_cast<size_t>(f.count);

        if (align > 1)
            offset = (offset + align - 1) & ~(align - 1);
        max_align = std::max(max_align, align);

        result.push_back({offset, total, f.type_str, is_bin});
        offset += total;
    }

    // Pad struct total to max field alignment (natural packing only).
    if (!packed && max_align > 1)
        offset = (offset + max_align - 1) & ~(max_align - 1);

    return {result, offset};
}

/// Compute recv frame buffer size for schema mode.
/// Outer envelope: fixarray(1)+uint32(5)+bin8(10)+uint64(9) = 25 bytes.
/// Payload array header: 3 bytes.  Per scalar: 9 bytes max.  Per bin: 5+byte_size.
static size_t schema_max_frame_size(const std::vector<FieldLayout>& defs)
{
    size_t sz = 25 + 3 + 4; // outer + inner array header + slack
    for (const auto& d : defs)
        sz += d.is_bin ? (5 + d.byte_size) : 9;
    return sz;
}

/// Validate all type_str values in a schema.  Returns the first invalid type_str,
/// or an empty string if all are valid.  [ZQ1]
static std::string find_invalid_type(const std::vector<ZmqSchemaField>& schema)
{
    for (const auto& f : schema)
    {
        if (!is_valid_type_str(f.type_str))
            return f.type_str;
    }
    return {};
}

// ============================================================================
// Factories — schema mode
// ============================================================================

std::unique_ptr<ZmqQueue>
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
    // [ZQ1] Validate all type strings before computing layout.
    if (const std::string bad = find_invalid_type(schema); !bad.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] pull_from '{}': invalid type_str '{}'", endpoint, bad);
        return nullptr;
    }
    auto [layouts, item_sz] = compute_field_layout(schema, packing);

    auto impl               = std::make_unique<ZmqQueueImpl>();
    impl->mode              = ZmqQueueImpl::Mode::Read;
    impl->endpoint          = endpoint;
    impl->bind_socket       = bind;
    impl->item_sz           = item_sz;
    impl->max_depth         = max_buffer_depth;
    impl->queue_name        = endpoint;
    impl->max_frame_sz_     = schema_max_frame_size(layouts);
    for (auto& l : layouts)
        impl->schema_defs_.push_back({l.offset, l.byte_size, l.type_str, l.is_bin});
    if (schema_tag) { impl->schema_tag_ = *schema_tag; impl->has_schema_tag_ = true; }

    // Pre-allocate ring buffer (max_depth slots) and decode staging buffer. [ZQ9]
    impl->recv_ring_.assign(max_buffer_depth, std::vector<std::byte>(item_sz, std::byte{0}));
    impl->decode_tmp_.resize(item_sz, std::byte{0});
    impl->current_read_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<ZmqQueue>(new ZmqQueue(std::move(impl)));
}

std::unique_ptr<ZmqQueue>
ZmqQueue::push_to(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
                  std::string packing,
                  bool bind,
                  std::optional<std::array<uint8_t, 8>> schema_tag,
                  int sndhwm)
{
    if (schema.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': schema must not be empty", endpoint);
        return nullptr;
    }
    // [ZQ1] Validate all type strings before computing layout.
    if (const std::string bad = find_invalid_type(schema); !bad.empty())
    {
        LOGGER_ERROR("[hub::ZmqQueue] push_to '{}': invalid type_str '{}'", endpoint, bad);
        return nullptr;
    }
    auto [layouts, item_sz] = compute_field_layout(schema, packing);

    auto impl               = std::make_unique<ZmqQueueImpl>();
    impl->mode              = ZmqQueueImpl::Mode::Write;
    impl->endpoint          = endpoint;
    impl->bind_socket       = bind;
    impl->item_sz           = item_sz;
    impl->queue_name        = endpoint;
    impl->sndhwm            = sndhwm; // [ZQ8]
    impl->max_frame_sz_     = schema_max_frame_size(layouts);
    for (auto& l : layouts)
        impl->schema_defs_.push_back({l.offset, l.byte_size, l.type_str, l.is_bin});
    if (schema_tag) { impl->schema_tag_ = *schema_tag; impl->has_schema_tag_ = true; }
    impl->write_buf_.resize(item_sz, std::byte{0});
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
    if (!pImpl) return false;
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return false; // already running

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

    if (pImpl->mode == ZmqQueueImpl::Mode::Read)
    {
        pImpl->recv_stop_.store(false, std::memory_order_release);
        // [ZQ5] Capture pImpl raw pointer, not `this`, so move of ZmqQueue is safe.
        ZmqQueueImpl* impl_ptr = pImpl.get();
        pImpl->recv_thread_ = std::thread([impl_ptr] { impl_ptr->run_recv_thread_(); });
    }

    return true;
}

void ZmqQueue::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    pImpl->recv_stop_.store(true, std::memory_order_release);
    pImpl->recv_cv_.notify_all();

    if (pImpl->recv_thread_.joinable())
        pImpl->recv_thread_.join();

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

    std::unique_lock<std::mutex> lk(pImpl->recv_mu_);
    pImpl->recv_cv_.wait_for(lk, timeout, [this] {
        return pImpl->ring_count_ > 0 ||
               pImpl->recv_stop_.load(std::memory_order_relaxed);
    });

    if (pImpl->ring_count_ == 0) return nullptr;

    // Copy from ring slot to current_read_buf_ (pre-allocated; no heap alloc). [ZQ9]
    std::memcpy(pImpl->current_read_buf_.data(),
                pImpl->recv_ring_[pImpl->ring_head_].data(),
                pImpl->item_sz);
    pImpl->ring_head_ = (pImpl->ring_head_ + 1) % pImpl->max_depth;
    --pImpl->ring_count_;
    return pImpl->current_read_buf_.data();
}

void ZmqQueue::read_release() noexcept
{
    // No-op: item already copied to current_read_buf_ in read_acquire().
}

// ============================================================================
// Writing
// ============================================================================

void* ZmqQueue::write_acquire(std::chrono::milliseconds /*timeout*/) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write) return nullptr;
    return pImpl->write_buf_.data();
}

void ZmqQueue::write_commit() noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write || !pImpl->socket)
        return;

    try
    {
        pImpl->write_sbuf_.clear();
        msgpack::packer<msgpack::sbuffer> pk(pImpl->write_sbuf_);

        pk.pack_array(4);

        // [0] magic
        pk.pack(kFrameMagic);

        // [1] schema_tag (always 8 bytes; zero-filled if not configured)
        pk.pack_bin(8);
        pk.pack_bin_body(reinterpret_cast<const char*>(pImpl->schema_tag_.data()), 8);

        // [2] sequence number
        pk.pack(pImpl->send_seq_.fetch_add(1, std::memory_order_relaxed));

        // [3] payload: array of N typed field values
        pk.pack_array(static_cast<uint32_t>(pImpl->schema_defs_.size()));
        const char* src = reinterpret_cast<const char*>(pImpl->write_buf_.data());
        for (const auto& fd : pImpl->schema_defs_)
            ZmqQueueImpl::pack_field(pk, fd, src);

        const int rc = zmq_send(pImpl->socket,
                                pImpl->write_sbuf_.data(),
                                pImpl->write_sbuf_.size(),
                                ZMQ_DONTWAIT);
        if (rc < 0)
        {
            const int err = zmq_errno();
            ++pImpl->send_drop_count_;
            if (err == EAGAIN)
                LOGGER_WARN("[hub::ZmqQueue] send dropped (EAGAIN/HWM) on '{}'",
                            pImpl->queue_name);
            else
                LOGGER_WARN("[hub::ZmqQueue] send error on '{}': {}",
                            pImpl->queue_name, zmq_strerror(err));
        }
    }
    catch (const std::exception& e)
    {
        LOGGER_WARN("[hub::ZmqQueue] write_commit error on '{}': {}",
                    pImpl->queue_name, e.what());
    }
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

} // namespace pylabhub::hub
