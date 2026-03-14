// src/utils/hub/hub_inbox_queue.cpp
/**
 * @file hub_inbox_queue.cpp
 * @brief InboxQueue (ROUTER receiver) and InboxClient (DEALER sender) implementations.
 *
 * Wire format: msgpack fixarray[4] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N)]
 * Same wire format as ZmqQueue; shared helpers in zmq_wire_helpers.hpp.
 *
 * ZMQ framing (ROUTER-DEALER):
 *   DEALER → ROUTER (wire): ["", payload]   ZMQ prepends identity on ROUTER side
 *   ROUTER sees:            [identity, "", payload]
 *   ROUTER → DEALER (wire): [identity, "", ack_byte]
 *   DEALER receives ACK:    ["", ack_byte]  (ZMQ strips identity; app drains empty frame)
 */
#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "zmq_wire_helpers.hpp"

#include <zmq.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// InboxQueueImpl
// ============================================================================

struct InboxQueueImpl
{
    std::string endpoint;
    std::string actual_ep;
    size_t      item_sz{0};
    size_t      max_frame_sz{0};
    int         rcvhwm{1000};    ///< ZMQ_RCVHWM applied at start(). 0 = unlimited.
    int         last_rcvtimeo{-2}; ///< Cached ZMQ_RCVTIMEO. -2 = not yet set.

    std::array<uint8_t, 8> schema_tag_{};  // first 8 bytes of BLAKE2b-256 (unused/zero for inbox)
    std::vector<wire_detail::WireFieldDesc> schema_defs_;

    void* zmq_ctx{nullptr};
    void* socket{nullptr};

    std::atomic<bool> running_{false};

    // Decode buffer (single slot — inbox_thread processes one message at a time)
    std::vector<std::byte> decode_buf_;
    // Frame receive buffer — pre-allocated to max_frame_sz to avoid per-call heap allocation.
    std::vector<char>      frame_recv_buf_;
    std::string            current_sender_id_;  // set by recv_one, read by send_ack

    // Current item (returned by recv_one; valid until next recv_one)
    InboxItem current_item_;

    // Counters
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> ack_send_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0};

    // Sequence tracking — per-sender (keyed by sender_id from ZMQ identity frame)
    // A single global counter is meaningless with multiple senders; each sender's
    // sequence is independent and wraps independently.
    std::unordered_map<std::string, uint64_t> sender_expected_seq_;
};

// ============================================================================
// InboxClientImpl
// ============================================================================

struct InboxClientImpl
{
    std::string endpoint;
    std::string sender_uid;
    size_t      item_sz{0};

    std::vector<wire_detail::WireFieldDesc> schema_defs_;

    void* zmq_ctx{nullptr};
    void* socket{nullptr};

    std::atomic<bool>    running_{false};
    std::vector<std::byte> write_buf_;  // zero-initialized

    // reusable send buffer
    msgpack::sbuffer sbuf_;
    std::atomic<uint64_t> send_seq_{0};

    std::array<uint8_t, 8> schema_tag_{};  // unused/zero for inbox
    int last_acktimeo{-2}; ///< Cached ZMQ_RCVTIMEO for ACK receives. -2 = not yet set.
};

// ============================================================================
// Schema validation helper
// ============================================================================

static bool validate_inbox_schema(const std::vector<ZmqSchemaField>& schema,
                                   const std::string& endpoint)
{
    if (schema.empty())
    {
        LOGGER_ERROR("[hub::InboxQueue] '{}': schema must not be empty", endpoint);
        return false;
    }
    for (const auto& f : schema)
    {
        if (!wire_detail::is_valid_type_str(f.type_str))
        {
            LOGGER_ERROR("[hub::InboxQueue] '{}': invalid type_str '{}'", endpoint, f.type_str);
            return false;
        }
        if ((f.type_str == "string" || f.type_str == "bytes") && f.length == 0)
        {
            LOGGER_ERROR("[hub::InboxQueue] '{}': string/bytes field has length=0", endpoint);
            return false;
        }
        if (f.type_str != "string" && f.type_str != "bytes" && f.count == 0)
        {
            LOGGER_ERROR("[hub::InboxQueue] '{}': numeric field count must be >= 1", endpoint);
            return false;
        }
    }
    return true;
}

static bool validate_inbox_packing(const std::string& packing, const std::string& endpoint)
{
    if (packing != "aligned" && packing != "packed")
    {
        LOGGER_ERROR("[hub::InboxQueue] '{}': invalid packing '{}' (must be \"aligned\" or \"packed\")",
                     endpoint, packing);
        return false;
    }
    return true;
}

// ============================================================================
// InboxQueue — factory
// ============================================================================

std::unique_ptr<InboxQueue>
InboxQueue::bind_at(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
                    std::string packing, int rcvhwm)
{
    if (!validate_inbox_schema(schema, endpoint)) return nullptr;
    if (!validate_inbox_packing(packing, endpoint)) return nullptr;

    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl            = std::make_unique<InboxQueueImpl>();
    impl->endpoint       = endpoint;
    impl->item_sz        = item_sz;
    impl->max_frame_sz   = wire_detail::max_frame_size(layouts);
    impl->rcvhwm         = rcvhwm;
    impl->schema_defs_   = std::move(layouts);
    impl->decode_buf_.resize(item_sz, std::byte{0});
    impl->frame_recv_buf_.resize(impl->max_frame_sz, '\0');

    return std::unique_ptr<InboxQueue>(new InboxQueue(std::move(impl)));
}

// ============================================================================
// InboxQueue — constructor / destructor / move
// ============================================================================

InboxQueue::InboxQueue(std::unique_ptr<InboxQueueImpl> impl) : pImpl(std::move(impl)) {}

InboxQueue::~InboxQueue()
{
    stop();
}

InboxQueue::InboxQueue(InboxQueue&&) noexcept = default;

InboxQueue& InboxQueue::operator=(InboxQueue&& o) noexcept
{
    if (this != &o)
    {
        stop();
        pImpl = std::move(o.pImpl);
    }
    return *this;
}

// ============================================================================
// InboxQueue — lifecycle
// ============================================================================

bool InboxQueue::start()
{
    if (!pImpl) return false;
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return false; // already running

    pImpl->zmq_ctx = zmq_ctx_new();
    if (!pImpl->zmq_ctx)
    {
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxQueue] zmq_ctx_new() failed for '{}'", pImpl->endpoint);
        return false;
    }
    // ZMQ_BLOCKY=0 for clean shutdown
    int blocky = 0;
    zmq_ctx_set(pImpl->zmq_ctx, ZMQ_BLOCKY, blocky);

    pImpl->socket = zmq_socket(pImpl->zmq_ctx, ZMQ_ROUTER);
    if (!pImpl->socket)
    {
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxQueue] zmq_socket(ROUTER) failed for '{}'", pImpl->endpoint);
        return false;
    }

    int linger = 0;
    zmq_setsockopt(pImpl->socket, ZMQ_LINGER, &linger, sizeof(linger));

    // Apply receive high-water mark (queue depth before ZMQ starts dropping messages).
    zmq_setsockopt(pImpl->socket, ZMQ_RCVHWM, &pImpl->rcvhwm, sizeof(pImpl->rcvhwm));

    const int rc = zmq_bind(pImpl->socket, pImpl->endpoint.c_str());
    if (rc != 0)
    {
        zmq_close(pImpl->socket);
        pImpl->socket = nullptr;
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxQueue] zmq_bind('{}') failed: {}", pImpl->endpoint,
                     zmq_strerror(zmq_errno()));
        return false;
    }

    // Resolve actual bound endpoint (port-0 → OS-assigned port)
    {
        char buf[256]{};
        size_t buf_len = sizeof(buf);
        if (zmq_getsockopt(pImpl->socket, ZMQ_LAST_ENDPOINT, buf, &buf_len) == 0)
            pImpl->actual_ep = buf;
        else
        {
            LOGGER_WARN("[hub::InboxQueue] zmq_getsockopt(ZMQ_LAST_ENDPOINT) failed for '{}': {}",
                        pImpl->endpoint, zmq_strerror(zmq_errno()));
            pImpl->actual_ep = pImpl->endpoint;
        }
    }

    return true;
}

void InboxQueue::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    if (pImpl->socket)  { zmq_close(pImpl->socket);     pImpl->socket   = nullptr; }
    if (pImpl->zmq_ctx) { zmq_ctx_term(pImpl->zmq_ctx); pImpl->zmq_ctx  = nullptr; }
}

bool InboxQueue::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

std::string InboxQueue::actual_endpoint() const
{
    if (!pImpl) return {};
    return pImpl->actual_ep.empty() ? pImpl->endpoint : pImpl->actual_ep;
}

size_t InboxQueue::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

// ============================================================================
// InboxQueue — recv_one
// ============================================================================

const InboxItem* InboxQueue::recv_one(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || !pImpl->socket) return nullptr;

    // HR-03: only call zmq_setsockopt when timeout changes (avoids a syscall per iteration).
    const int timeout_ms = static_cast<int>(timeout.count());
    if (timeout_ms != pImpl->last_rcvtimeo)
    {
        zmq_setsockopt(pImpl->socket, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        pImpl->last_rcvtimeo = timeout_ms;
    }

    // Receive 3 frames: identity, empty delimiter, payload
    // Use zmq_recv in a loop for each frame.

    // Frame 0: identity
    char id_buf[256]{};
    int id_rc = zmq_recv(pImpl->socket, id_buf, sizeof(id_buf) - 1, 0);
    if (id_rc < 0)
    {
        const int err = zmq_errno();
        if (err != EAGAIN && err != EINTR && err != ETERM)
            LOGGER_WARN("[hub::InboxQueue] recv identity failed: {}", zmq_strerror(err));
        return nullptr;
    }
    // Check if more frames available
    int more = 0;
    size_t more_sz = sizeof(more);
    zmq_getsockopt(pImpl->socket, ZMQ_RCVMORE, &more, &more_sz);
    if (!more) { ++pImpl->recv_frame_error_count_; return nullptr; }

    std::string sender_id(id_buf, static_cast<size_t>(id_rc));

    // Frame 1: empty delimiter
    char empty_buf[4]{};
    int empty_rc = zmq_recv(pImpl->socket, empty_buf, sizeof(empty_buf), 0);
    if (empty_rc < 0) { ++pImpl->recv_frame_error_count_; return nullptr; }
    zmq_getsockopt(pImpl->socket, ZMQ_RCVMORE, &more, &more_sz);
    if (!more) { ++pImpl->recv_frame_error_count_; return nullptr; }

    // Frame 2: msgpack payload (reuse persistent buffer to avoid per-call heap allocation)
    int payload_rc = zmq_recv(pImpl->socket, pImpl->frame_recv_buf_.data(),
                              static_cast<int>(pImpl->max_frame_sz), 0);
    if (payload_rc < 0)
    {
        const int err = zmq_errno();
        if (err != EAGAIN && err != EINTR && err != ETERM)
            LOGGER_WARN("[hub::InboxQueue] recv payload failed: {}", zmq_strerror(err));
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    if (payload_rc >= static_cast<int>(pImpl->max_frame_sz))
    {
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    // Decode msgpack
    try
    {
        msgpack::object_handle oh = msgpack::unpack(pImpl->frame_recv_buf_.data(),
                                                     static_cast<size_t>(payload_rc));
        const msgpack::object& obj = oh.get();

        if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 4)
        { ++pImpl->recv_frame_error_count_; return nullptr; }
        const auto* elems = obj.via.array.ptr;

        // [0] magic
        uint32_t magic = 0;
        elems[0].convert(magic);
        if (magic != wire_detail::kFrameMagic) { ++pImpl->recv_frame_error_count_; return nullptr; }

        // [1] schema_tag (bin, 8 bytes) — not validated for inbox (accept any tag)
        if (elems[1].type != msgpack::type::BIN || elems[1].via.bin.size != 8)
        { ++pImpl->recv_frame_error_count_; return nullptr; }

        // [2] seq — track gaps per sender
        {
            uint64_t seq = 0;
            elems[2].convert(seq);
            auto it = pImpl->sender_expected_seq_.find(sender_id);
            if (it != pImpl->sender_expected_seq_.end())
            {
                if (seq > it->second)
                    pImpl->recv_gap_count_.fetch_add(seq - it->second,
                                                      std::memory_order_relaxed);
                it->second = seq + 1;
            }
            else
            {
                pImpl->sender_expected_seq_.emplace(sender_id, seq + 1);
            }
            pImpl->current_item_.seq = seq;
        }

        // [3] payload array
        const auto& arr = elems[3];
        if (arr.type != msgpack::type::ARRAY ||
            arr.via.array.size != pImpl->schema_defs_.size())
        { ++pImpl->recv_frame_error_count_; return nullptr; }

        // Decode into decode_buf_
        std::fill(pImpl->decode_buf_.begin(), pImpl->decode_buf_.end(), std::byte{0});
        char* dst = reinterpret_cast<char*>(pImpl->decode_buf_.data());
        for (size_t i = 0; i < pImpl->schema_defs_.size(); ++i)
        {
            if (!wire_detail::unpack_field(arr.via.array.ptr[i], pImpl->schema_defs_[i], dst))
            {
                ++pImpl->recv_frame_error_count_;
                return nullptr;
            }
        }
    }
    catch (const std::exception& e)
    {
        ++pImpl->recv_frame_error_count_;
        LOGGER_WARN("[hub::InboxQueue] unpack error from '{}': {}", sender_id, e.what());
        return nullptr;
    }

    pImpl->current_sender_id_    = std::move(sender_id);
    pImpl->current_item_.data    = pImpl->decode_buf_.data();
    pImpl->current_item_.sender_id = pImpl->current_sender_id_;
    return &pImpl->current_item_;
}

// ============================================================================
// InboxQueue — send_ack
// ============================================================================

void InboxQueue::send_ack(uint8_t code) noexcept
{
    if (!pImpl || !pImpl->socket) return;

    const std::string& id = pImpl->current_sender_id_;

    // Send [identity, empty, ack_code] using ZMQ_SNDMORE
    int rc1 = zmq_send(pImpl->socket, id.data(), id.size(), ZMQ_SNDMORE);
    if (rc1 < 0) { ++pImpl->ack_send_error_count_; return; }

    int rc2 = zmq_send(pImpl->socket, "", 0, ZMQ_SNDMORE);
    if (rc2 < 0) { ++pImpl->ack_send_error_count_; return; }

    int rc3 = zmq_send(pImpl->socket, &code, 1, 0);
    if (rc3 < 0) { ++pImpl->ack_send_error_count_; }
}

// ============================================================================
// InboxQueue — diagnostics
// ============================================================================

uint64_t InboxQueue::recv_frame_error_count() const noexcept
{
    return pImpl ? pImpl->recv_frame_error_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t InboxQueue::ack_send_error_count() const noexcept
{
    return pImpl ? pImpl->ack_send_error_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t InboxQueue::recv_gap_count() const noexcept
{
    return pImpl ? pImpl->recv_gap_count_.load(std::memory_order_relaxed) : 0;
}

// ============================================================================
// InboxClient — factory
// ============================================================================

std::unique_ptr<InboxClient>
InboxClient::connect_to(const std::string& endpoint, const std::string& sender_uid,
                        std::vector<ZmqSchemaField> schema, std::string packing)
{
    if (!validate_inbox_schema(schema, endpoint)) return nullptr;
    if (!validate_inbox_packing(packing, endpoint)) return nullptr;

    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl            = std::make_unique<InboxClientImpl>();
    impl->endpoint       = endpoint;
    impl->sender_uid     = sender_uid;
    impl->item_sz        = item_sz;
    impl->schema_defs_   = std::move(layouts);
    impl->write_buf_.resize(item_sz, std::byte{0});

    return std::unique_ptr<InboxClient>(new InboxClient(std::move(impl)));
}

// ============================================================================
// InboxClient — constructor / destructor / move
// ============================================================================

InboxClient::InboxClient(std::unique_ptr<InboxClientImpl> impl) : pImpl(std::move(impl)) {}

InboxClient::~InboxClient()
{
    stop();
}

InboxClient::InboxClient(InboxClient&&) noexcept = default;

InboxClient& InboxClient::operator=(InboxClient&& o) noexcept
{
    if (this != &o)
    {
        stop();
        pImpl = std::move(o.pImpl);
    }
    return *this;
}

// ============================================================================
// InboxClient — lifecycle
// ============================================================================

bool InboxClient::start()
{
    if (!pImpl) return false;
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return false;

    pImpl->zmq_ctx = zmq_ctx_new();
    if (!pImpl->zmq_ctx)
    {
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxClient] zmq_ctx_new() failed for '{}'", pImpl->endpoint);
        return false;
    }
    int blocky = 0;
    zmq_ctx_set(pImpl->zmq_ctx, ZMQ_BLOCKY, blocky);

    pImpl->socket = zmq_socket(pImpl->zmq_ctx, ZMQ_DEALER);
    if (!pImpl->socket)
    {
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxClient] zmq_socket(DEALER) failed for '{}'", pImpl->endpoint);
        return false;
    }

    int linger = 0;
    zmq_setsockopt(pImpl->socket, ZMQ_LINGER, &linger, sizeof(linger));

    // Set ZMQ_IDENTITY to sender_uid before connecting
    const std::string& uid = pImpl->sender_uid;
    zmq_setsockopt(pImpl->socket, ZMQ_IDENTITY, uid.c_str(), uid.size());

    const int rc = zmq_connect(pImpl->socket, pImpl->endpoint.c_str());
    if (rc != 0)
    {
        zmq_close(pImpl->socket);
        pImpl->socket = nullptr;
        zmq_ctx_term(pImpl->zmq_ctx);
        pImpl->zmq_ctx = nullptr;
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxClient] zmq_connect('{}') failed: {}", pImpl->endpoint,
                     zmq_strerror(zmq_errno()));
        return false;
    }

    return true;
}

void InboxClient::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    if (pImpl->socket)  { zmq_close(pImpl->socket);     pImpl->socket   = nullptr; }
    if (pImpl->zmq_ctx) { zmq_ctx_term(pImpl->zmq_ctx); pImpl->zmq_ctx  = nullptr; }
}

bool InboxClient::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

size_t InboxClient::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

// ============================================================================
// InboxClient — acquire / send / abort
// ============================================================================

void* InboxClient::acquire() noexcept
{
    if (!pImpl || !pImpl->socket) return nullptr;
    // Zero-initialize before returning to caller
    std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
    return pImpl->write_buf_.data();
}

uint8_t InboxClient::send(std::chrono::milliseconds ack_timeout) noexcept
{
    if (!pImpl || !pImpl->socket) return 255;

    // Pack msgpack frame
    pImpl->sbuf_.clear();
    msgpack::packer<msgpack::sbuffer> pk(pImpl->sbuf_);
    pk.pack_array(4);
    pk.pack(wire_detail::kFrameMagic);
    // schema_tag: 8 zero bytes (inbox doesn't use schema tag guard)
    pk.pack_bin(8);
    pk.pack_bin_body(reinterpret_cast<const char*>(pImpl->schema_tag_.data()), 8);
    // seq
    pk.pack(pImpl->send_seq_.fetch_add(1, std::memory_order_relaxed));
    // payload array
    pk.pack_array(static_cast<uint32_t>(pImpl->schema_defs_.size()));
    const char* src = reinterpret_cast<const char*>(pImpl->write_buf_.data());
    for (const auto& fd : pImpl->schema_defs_)
        wire_detail::pack_field(pk, fd, src);

    // DEALER sends: [empty_delimiter, payload]
    // ROUTER receives: [identity, empty_delimiter, payload]
    // The explicit empty frame ensures consistent 3-frame envelope on the ROUTER side.
    const int delim_rc = zmq_send(pImpl->socket, "", 0, ZMQ_SNDMORE);
    if (delim_rc < 0)
    {
        LOGGER_WARN("[hub::InboxClient] zmq_send(delim) to '{}' failed: {}",
                    pImpl->endpoint, zmq_strerror(zmq_errno()));
        return 255;
    }

    const int send_rc = zmq_send(pImpl->socket,
                                  pImpl->sbuf_.data(),
                                  pImpl->sbuf_.size(),
                                  0);
    if (send_rc < 0)
    {
        LOGGER_WARN("[hub::InboxClient] zmq_send to '{}' failed: {}",
                    pImpl->endpoint, zmq_strerror(zmq_errno()));
        return 255;
    }

    // Fire-and-forget
    if (ack_timeout.count() <= 0) return 0;

    // Wait for ACK: ROUTER reply is [identity, "", ack_byte]; DEALER receives ["", ack_byte].
    // Drain the empty delimiter frame first, then read the ack byte.
    const int ack_ms = static_cast<int>(ack_timeout.count());
    // HR-03: only set ZMQ_RCVTIMEO when the value changes.
    if (ack_ms != pImpl->last_acktimeo)
    {
        zmq_setsockopt(pImpl->socket, ZMQ_RCVTIMEO, &ack_ms, sizeof(ack_ms));
        pImpl->last_acktimeo = ack_ms;
    }

    // Frame 0: empty delimiter
    char delim_recv[4]{};
    const int delim_recv_rc = zmq_recv(pImpl->socket, delim_recv, sizeof(delim_recv), 0);
    if (delim_recv_rc < 0)
    {
        const int err = zmq_errno();
        if (err == EAGAIN)
            LOGGER_WARN("[hub::InboxClient] ACK delimiter timeout from '{}'", pImpl->endpoint);
        else
            LOGGER_WARN("[hub::InboxClient] ACK delimiter recv error from '{}': {}",
                        pImpl->endpoint, zmq_strerror(err));
        return 255;
    }

    // Frame 1: ack byte — keep the same ack_ms timeout active (set at line above).
    // ZMQ delivers multi-part messages atomically, so frame 1 is always immediately
    // available after frame 0 is received; but keeping the timeout is safer.
    uint8_t ack_code = 255;
    const int ack_rc = zmq_recv(pImpl->socket, &ack_code, sizeof(ack_code), 0);
    if (ack_rc < 0)
    {
        LOGGER_WARN("[hub::InboxClient] ACK byte recv error from '{}': {}",
                    pImpl->endpoint, zmq_strerror(zmq_errno()));
        return 255;
    }
    return ack_code;
}

void InboxClient::abort() noexcept
{
    // Buffer not committed — simply re-zero for next use
    if (pImpl)
        std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
}

} // namespace pylabhub::hub
