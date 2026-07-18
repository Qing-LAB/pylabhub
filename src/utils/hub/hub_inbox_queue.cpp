// src/utils/hub/hub_inbox_queue.cpp
/**
 * @file hub_inbox_queue.cpp
 * @brief InboxQueue (ROUTER receiver) and InboxClient (DEALER sender) implementations.
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N), checksum:bin32]
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
#include "utils/zmq_context.hpp"
#include "utils/security/key_store.hpp"       // kRoleIdentityName, secure().keys()
#include "utils/security/secure_subsystem.hpp" // secure()
#include "utils/security/zap_router.hpp"      // ZapRouter, ZapDomainHandle
#include "zmq_wire_helpers.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"  // zmq::multipart_t

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

    std::array<uint8_t, 8> schema_tag_{};  // first 8 bytes of BLAKE2b-256 of canonical schema string
    std::vector<wire_detail::WireFieldDesc> schema_defs_;

    // Shared ZMQ context via get_zmq_context(). InboxQueue never creates
    // or terminates it; the ZMQContext lifecycle module (persistent) owns
    // the process-wide context.
    zmq::socket_t socket;

    std::atomic<bool> running_{false};

    // Decode buffer (single slot — inbox_thread processes one message at a time)
    std::vector<std::byte> decode_buf_;
    // Frame receive buffer — pre-allocated to max_frame_sz to avoid per-call heap allocation.
    std::vector<char>      frame_recv_buf_;
    std::string            current_sender_id_;  // set by recv_one, read by send_ack

    // Current item (returned by recv_one; valid until next recv_one)
    InboxItem current_item_;

    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced};

    // Counters
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> ack_send_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0};
    std::atomic<uint64_t> checksum_error_count_{0};

    // Sequence tracking — per-sender (keyed by sender_id from ZMQ identity frame)
    // A single global counter is meaningless with multiple senders; each sender's
    // sequence is independent and wraps independently.
    std::unordered_map<std::string, uint64_t> sender_expected_seq_;

    // ── CURVE-server auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────
    // Set by set_curve_server_identity() before start().  Empty
    // identity_key_name_ == legacy plaintext inbox (no CURVE arm).
    std::string identity_key_name_;   ///< KeyStore key (kRoleIdentityName).
    std::string zap_domain_;          ///< Distinct inbox ZAP domain ("<uid>:inbox").

    // Hub-wide known_roles roster (COW snapshot) consulted by the ZapRouter
    // pump thread via is_peer_allowed.  nullptr == deny-all (secure default
    // between the S1 bind and the S3 set_peer_allowlist seed).  Written by
    // set_peer_allowlist (any thread), read on the pump thread — atomic.
    std::atomic<std::shared_ptr<const pylabhub::utils::security::PeerAllowlist>>
        allowlist_{nullptr};

    // RAII registration with the process ZapRouter; destructor unregisters
    // the domain.  Engaged in start() (register_domain before bind), reset
    // in stop().
    std::optional<pylabhub::utils::security::ZapDomainHandle> zap_handle_;
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

    // Shared ZMQ context via get_zmq_context(). InboxClient never creates
    // or terminates it.
    zmq::socket_t socket;

    std::atomic<bool>    running_{false};
    std::vector<std::byte> write_buf_;  // zero-initialized

    // reusable send buffer
    msgpack::sbuffer sbuf_;
    std::atomic<uint64_t> send_seq_{0};

    std::array<uint8_t, 8> schema_tag_{};  // first 8 bytes of BLAKE2b-256 of canonical schema string
    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced};
    int last_acktimeo{-2}; ///< Cached ZMQ_RCVTIMEO for ACK receives. -2 = not yet set.

    // ── CURVE-client auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────
    // Set by set_curve_client_identity() before start().  Empty
    // identity_key_name_ == legacy plaintext (no CURVE arm).
    std::string identity_key_name_;   ///< KeyStore key (kRoleIdentityName).
    std::string server_pubkey_z85_;   ///< Receiver identity pubkey (curve_serverkey).
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

/// Compute 8-byte schema tag from ZmqSchemaField list + packing (BLAKE2b-256 of
/// canonical string).  Mirrors HEP-CORE-0034 §6.3 fingerprint correction: the
/// canonical form ends with `|pack:<packing>` so two schemas with identical
/// fields and different packing produce different tags.  Without this, a
/// sender publishing packed bytes and a receiver decoding as aligned (or vice
/// versa) would silently misinterpret the wire — the exact bug Phase 1 fixes
/// on the SchemaSpec/SchemaInfo paths.
static std::array<uint8_t, 8> compute_inbox_schema_tag(
    const std::vector<ZmqSchemaField>& schema, const std::string& packing)
{
    // Build canonical string: "type:count:length;" per field, then "|pack:<packing>".
    std::string canonical;
    for (const auto& f : schema)
    {
        canonical += f.type_str;
        canonical += ':';
        canonical += std::to_string(f.count);
        canonical += ':';
        canonical += std::to_string(f.length);
        canonical += ';';
    }
    canonical += "|pack:";
    canonical += packing;
    auto full_hash = pylabhub::utils::security::secure().compute_blake2b_array(canonical.data(), canonical.size());
    std::array<uint8_t, 8> tag{};
    std::memcpy(tag.data(), full_hash.data(), 8);
    return tag;
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
    impl->schema_tag_    = compute_inbox_schema_tag(schema, packing);
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
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race

    try
    {
        pImpl->socket =
            zmq::socket_t(pylabhub::hub::get_zmq_context(), zmq::socket_type::router);
        pImpl->socket.set(zmq::sockopt::linger, 0);
        pImpl->socket.set(zmq::sockopt::rcvhwm, pImpl->rcvhwm);

        // ── CURVE-server arm (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ──
        // Mirror of ZmqQueue's bind-side pattern: identity keypair from
        // secure().keys() (secret never leaves the module — flows into
        // libzmq inside the with_seckey callback), curve_server=1, a
        // DISTINCT inbox zap_domain, and register_domain BEFORE bind so
        // the ZapRouter can gate the first handshake.  Until
        // set_peer_allowlist seeds the hub roster, `allowlist_` is
        // nullptr → is_peer_allowed denies all (secure default).
        if (!pImpl->identity_key_name_.empty())
        {
            namespace sec = pylabhub::utils::security;
            auto &ks = sec::secure().keys();
            pImpl->socket.set(zmq::sockopt::curve_publickey,
                              ks.pubkey(pImpl->identity_key_name_));
            ks.with_seckey(pImpl->identity_key_name_,
                [&](std::string_view seckey) {
                    pImpl->socket.set(zmq::sockopt::curve_secretkey, seckey);
                });
            pImpl->socket.set(zmq::sockopt::curve_server, 1);
            pImpl->socket.set(zmq::sockopt::zap_domain, pImpl->zap_domain_);

            // register_domain BEFORE bind (same ordering as ZmqQueue) so
            // no handshake can slip through un-gated.  `*this` is the
            // PeerAdmission the pump thread consults.
            pImpl->zap_handle_.emplace(
                sec::ZapRouter::instance().register_domain(
                    pImpl->zap_domain_, *this));
            LOGGER_INFO(
                "[hub::InboxQueue] CURVE-server armed endpoint='{}' "
                "zap_domain='{}' (deny-all until roster seeded; "
                "HEP-CORE-0027 §3.5)",
                pImpl->endpoint, pImpl->zap_domain_);
        }

        pImpl->socket.bind(pImpl->endpoint);
        pImpl->actual_ep = pImpl->socket.get(zmq::sockopt::last_endpoint);
    }
    catch (const zmq::error_t &e)
    {
        pImpl->zap_handle_.reset();
        pImpl->socket.close();
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxQueue] socket setup failed for '{}': {}",
                     pImpl->endpoint, e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        pImpl->zap_handle_.reset();
        pImpl->socket.close();
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxQueue] CURVE arm failed for '{}': {}",
                     pImpl->endpoint, e.what());
        return false;
    }

    return true;
}

void InboxQueue::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    // Close the socket; shared context is owned by the ZMQContext lifecycle module.
    pImpl->socket.close();

    // Unregister from the ZapRouter (RAII handle destructor); the domain
    // becomes free for a future re-bind.  After this the pump thread no
    // longer holds a reference to this InboxQueue's is_peer_allowed.
    pImpl->zap_handle_.reset();
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

    // HR-03: only apply RCVTIMEO when it changes (saves a syscall per call).
    const int timeout_ms = static_cast<int>(timeout.count());
    if (timeout_ms != pImpl->last_rcvtimeo)
    {
        pImpl->socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
        pImpl->last_rcvtimeo = timeout_ms;
    }

    // ROUTER delivers the three-frame envelope (identity, empty, payload) as a
    // single multipart message. multipart_t::recv reads all frames atomically:
    // either the whole message is available or the call returns false/throws.
    zmq::multipart_t parts;
    try
    {
        if (!parts.recv(pImpl->socket))
            return nullptr;  // RCVTIMEO expired or EAGAIN
    }
    catch (const zmq::error_t &e)
    {
        if (e.num() != ETERM && e.num() != EINTR)
            LOGGER_WARN("[hub::InboxQueue] recv failed: {}", e.what());
        return nullptr;
    }

    if (parts.size() != 3)
    {
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    std::string sender_id = parts[0].to_string();
    // parts[1] is the empty delimiter (size==0, enforced implicitly by ROUTER/DEALER pattern).
    zmq::message_t &payload = parts[2];

    if (payload.size() >= pImpl->max_frame_sz)
    {
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    try
    {
        msgpack::object_handle oh = msgpack::unpack(
            static_cast<const char *>(payload.data()), payload.size());

        auto env = wire_detail::unpack_envelope(oh.get());
        if (!env.valid || env.payload_size != pImpl->schema_defs_.size())
        { ++pImpl->recv_frame_error_count_; return nullptr; }

        // Schema-tag mismatch.
        if (std::memcmp(env.recv_tag, pImpl->schema_tag_.data(), 8) != 0)
        { ++pImpl->recv_frame_error_count_; return nullptr; }

        // Per-sender sequence gap tracking.
        {
            auto it = pImpl->sender_expected_seq_.find(sender_id);
            if (it != pImpl->sender_expected_seq_.end())
            {
                if (env.seq > it->second)
                    pImpl->recv_gap_count_.fetch_add(env.seq - it->second,
                                                      std::memory_order_relaxed);
                it->second = env.seq + 1;
            }
            else
            {
                pImpl->sender_expected_seq_.emplace(sender_id, env.seq + 1);
            }
            pImpl->current_item_.seq = env.seq;
        }

        // Decode payload fields.
        std::fill(pImpl->decode_buf_.begin(), pImpl->decode_buf_.end(), std::byte{0});
        if (!wire_detail::unpack_payload(*env.payload, pImpl->schema_defs_,
                                          pImpl->decode_buf_.data()))
        { ++pImpl->recv_frame_error_count_; return nullptr; }

        // Checksum verification.
        if (pImpl->checksum_policy_ != ChecksumPolicy::None)
        {
            if (!pylabhub::utils::security::secure().verify_blake2b(
                    env.checksum, pImpl->decode_buf_.data(), pImpl->item_sz))
            {
                pImpl->checksum_error_count_.fetch_add(1, std::memory_order_relaxed);
                LOGGER_ERROR("[hub::InboxQueue] checksum error after decode from '{}'",
                             sender_id);
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

    const std::string &id = pImpl->current_sender_id_;

    try
    {
        zmq::multipart_t ack;
        ack.addstr(id);                      // routing identity
        ack.addstr("");                      // empty delimiter
        ack.addmem(&code, sizeof(code));     // 1-byte ack code
        if (!ack.send(pImpl->socket))
            ++pImpl->ack_send_error_count_;
    }
    catch (const zmq::error_t &)
    {
        ++pImpl->ack_send_error_count_;
    }
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

uint64_t InboxQueue::checksum_error_count() const noexcept
{
    return pImpl ? pImpl->checksum_error_count_.load(std::memory_order_relaxed) : 0;
}

void InboxQueue::set_checksum_policy(ChecksumPolicy policy) noexcept
{
    if (pImpl) pImpl->checksum_policy_ = policy;
}

// ── CURVE-server auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────────

void InboxQueue::set_curve_server_identity(std::string identity_key_name,
                                           std::string zap_domain)
{
    if (!pImpl) return;
    pImpl->identity_key_name_ = std::move(identity_key_name);
    pImpl->zap_domain_        = std::move(zap_domain);
}

bool InboxQueue::set_peer_allowlist(
    pylabhub::utils::security::PeerAllowlist allowlist)
{
    if (!pImpl) return false;
    pImpl->allowlist_.store(
        std::make_shared<const pylabhub::utils::security::PeerAllowlist>(
            std::move(allowlist)),
        std::memory_order_release);
    return true;
}

std::optional<pylabhub::utils::security::PeerAllowlist>
InboxQueue::peer_allowlist_snapshot() const
{
    if (!pImpl) return std::nullopt;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap) return std::nullopt;
    return *snap;
}

bool InboxQueue::is_peer_allowed(
    const pylabhub::utils::security::PeerIdentity &peer) const
{
    if (!pImpl) return false;
    // nullptr roster == deny-all (secure default between the S1 bind and
    // the S3 set_peer_allowlist seed).
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap) return false;
    return snap->contains(peer);
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
    impl->schema_tag_    = compute_inbox_schema_tag(schema, packing);
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
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race

    try
    {
        pImpl->socket =
            zmq::socket_t(pylabhub::hub::get_zmq_context(), zmq::socket_type::dealer);
        pImpl->socket.set(zmq::sockopt::linger, 0);
        // ZMQ_ROUTING_ID (modern name for ZMQ_IDENTITY): the peer's ROUTER will
        // prepend this to every message it receives from us.  Note: under
        // CURVE the ROUTER admits by PUBKEY (ZAP), not by this self-asserted
        // routing_id — the id is only the ACK-return address, no longer a
        // trust claim.
        pImpl->socket.set(zmq::sockopt::routing_id, pImpl->sender_uid);

        // ── CURVE-client arm (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ──
        // Present the sender's identity keypair and pin the receiver's
        // identity pubkey (from ROLE_INFO_ACK) as curve_serverkey.  Mirror
        // of ZmqQueue's dialing-side pattern.
        if (!pImpl->identity_key_name_.empty())
        {
            namespace sec = pylabhub::utils::security;
            auto &ks = sec::secure().keys();
            pImpl->socket.set(zmq::sockopt::curve_publickey,
                              ks.pubkey(pImpl->identity_key_name_));
            ks.with_seckey(pImpl->identity_key_name_,
                [&](std::string_view seckey) {
                    pImpl->socket.set(zmq::sockopt::curve_secretkey, seckey);
                });
            pImpl->socket.set(zmq::sockopt::curve_serverkey,
                              pImpl->server_pubkey_z85_);
        }

        pImpl->socket.connect(pImpl->endpoint);
    }
    catch (const zmq::error_t &e)
    {
        pImpl->socket.close();
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxClient] socket setup failed for '{}': {}",
                     pImpl->endpoint, e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        pImpl->socket.close();
        pImpl->running_.store(false);
        LOGGER_ERROR("[hub::InboxClient] CURVE arm failed for '{}': {}",
                     pImpl->endpoint, e.what());
        return false;
    }

    return true;
}

void InboxClient::stop()
{
    if (!pImpl) return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    // Close socket; shared context stays up — owned by ZMQContext module.
    pImpl->socket.close();
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

    // Compute BLAKE2b checksum: Enforced = auto-stamp, Manual/None = zeros.
    uint8_t checksum[32]{};
    if (pImpl->checksum_policy_ == ChecksumPolicy::Enforced)
    {
        pylabhub::utils::security::secure().compute_blake2b(
            checksum, pImpl->write_buf_.data(), pImpl->item_sz);
    }

    wire_detail::pack_frame(pk, pImpl->schema_tag_,
        pImpl->send_seq_.fetch_add(1, std::memory_order_relaxed),
        pImpl->schema_defs_, pImpl->write_buf_.data(), checksum);

    // DEALER sends [empty, payload]; ROUTER sees [identity, empty, payload].
    // multipart_t::send is atomic — every frame goes out or none.
    try
    {
        zmq::multipart_t out;
        out.addstr("");                                                 // empty delimiter
        out.addmem(pImpl->sbuf_.data(), pImpl->sbuf_.size());            // payload
        if (!out.send(pImpl->socket))
        {
            LOGGER_WARN("[hub::InboxClient] send to '{}' returned false (HWM?)",
                        pImpl->endpoint);
            return 255;
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("[hub::InboxClient] send to '{}' failed: {}",
                    pImpl->endpoint, e.what());
        return 255;
    }

    // Fire-and-forget
    if (ack_timeout.count() <= 0) return 0;

    // ROUTER reply: [identity, "", ack_byte]; DEALER strips the identity and
    // sees ["", ack_byte]. Receive as one multipart — atomicity guaranteed.
    const int ack_ms = static_cast<int>(ack_timeout.count());
    if (ack_ms != pImpl->last_acktimeo)
    {
        pImpl->socket.set(zmq::sockopt::rcvtimeo, ack_ms);
        pImpl->last_acktimeo = ack_ms;
    }

    try
    {
        zmq::multipart_t reply;
        if (!reply.recv(pImpl->socket))
        {
            LOGGER_WARN("[hub::InboxClient] ACK timeout from '{}'", pImpl->endpoint);
            return 255;
        }
        if (reply.size() != 2 || reply[1].size() != 1)
        {
            LOGGER_WARN("[hub::InboxClient] malformed ACK from '{}' (frames={})",
                        pImpl->endpoint, reply.size());
            return 255;
        }
        return *static_cast<const uint8_t *>(reply[1].data());
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("[hub::InboxClient] ACK recv error from '{}': {}",
                    pImpl->endpoint, e.what());
        return 255;
    }
}

void InboxClient::abort() noexcept
{
    // Buffer not committed — simply re-zero for next use
    if (pImpl)
        std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
}

void InboxClient::set_checksum_policy(ChecksumPolicy policy) noexcept
{
    if (pImpl) pImpl->checksum_policy_ = policy;
}

void InboxClient::set_curve_client_identity(std::string identity_key_name,
                                            std::string server_pubkey_z85)
{
    if (!pImpl) return;
    pImpl->identity_key_name_ = std::move(identity_key_name);
    pImpl->server_pubkey_z85_ = std::move(server_pubkey_z85);
}

} // namespace pylabhub::hub
