// src/utils/hub/hub_inbox_queue.cpp
/**
 * @file hub_inbox_queue.cpp
 * @brief InboxQueue (ROUTER receiver) and InboxClient (DEALER sender) implementations.
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N),
 * checksum:bin32] Same wire format as ZmqQueue; shared helpers in zmq_wire_helpers.hpp.
 *
 * ZMQ framing (ROUTER-DEALER):
 *   DEALER → ROUTER (wire): ["", payload]   ZMQ prepends identity on ROUTER side
 *   ROUTER sees:            [identity, "", payload]
 *   ROUTER → DEALER (wire): [identity, "", ack_byte]
 *   DEALER receives ACK:    ["", ack_byte]  (ZMQ strips identity; app drains empty frame)
 */
#include "utils/hub_inbox_queue.hpp"
#include "utils/debug_info.hpp" // PLH_PANIC — unarmed-CURVE invariant
#include "utils/logger.hpp"
#include "utils/scope_guard.hpp" // make_scope_guard — start() failure unwind
#include "utils/zmq_context.hpp"
#include "utils/zmq_socket_policy.hpp"         // send_multipart_atomic (house ZMQ rules)
#include "utils/curve_socket.hpp"              // arm_curve_server / arm_curve_client
#include "utils/security/key_store.hpp"        // kRoleIdentityName, secure().keys()
#include "utils/security/secure_subsystem.hpp" // secure()
#include "utils/security/zap_router.hpp"       // ZapRouter, ZapDomainHandle
#include "utils/replay_guard.hpp"              // shared sliding-window nonce dedup
#include "zmq_wire_helpers.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp" // zmq::multipart_t

#include <array>
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
    size_t item_sz{0};
    size_t max_frame_sz{0};
    int rcvhwm{1000};      ///< ZMQ_RCVHWM applied at start(). 0 = unlimited.
    int last_rcvtimeo{-2}; ///< Cached ZMQ_RCVTIMEO. -2 = not yet set.

    std::array<uint8_t, 8> schema_tag_{}; // first 8 bytes of BLAKE2b-256 of canonical schema string
    std::vector<wire_detail::WireFieldDesc> schema_defs_;

    // Shared ZMQ context via get_zmq_context(). InboxQueue never creates
    // or terminates it; the ZMQContext lifecycle module (persistent) owns
    // the process-wide context.
    zmq::socket_t socket;

    std::atomic<bool> running_{false};

    // Decode buffer (single slot — inbox_thread processes one message at a time)
    std::vector<std::byte> decode_buf_;
    // Frame receive buffer — pre-allocated to max_frame_sz to avoid per-call heap allocation.
    std::vector<char> frame_recv_buf_;
    // The ROUTER address the last message came in on — set by recv_one, read
    // by send_ack to route the receipt back.  Deliberately NOT the sender's
    // identity: that is `current_item_.sender_id`, derived from the proven
    // key.  They agree whenever a client sets its routing id to its own uid,
    // which the stock InboxClient does, and must not be conflated because
    // nothing requires a client to do that (HEP-CORE-0027 §3.7).
    std::string current_route_id_;

    // Current item (returned by recv_one; valid until next recv_one)
    InboxItem current_item_;

    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced};

    // Counters
    std::atomic<uint64_t> recv_frame_error_count_{0};
    std::atomic<uint64_t> ack_send_error_count_{0};
    std::atomic<uint64_t> recv_gap_count_{0};
    std::atomic<uint64_t> checksum_error_count_{0};
    std::atomic<uint64_t> recv_replay_reject_count_{0};
    /// Frames dropped because no name could be derived from the proven key.
    /// One counter, not one per verdict — the verdict is in the log line, and
    /// four counters would be four things to keep for a distinction only a
    /// log reader needs.
    std::atomic<uint64_t> recv_unattributed_count_{0};

    // Replay defense (HEP-CORE-0027 §3.6) — sliding-window nonce dedup
    // keyed by sender identity, the SAME `ReplayGuard` mechanism the hub
    // REG/admin plane uses via `HubState::nonce_seen`.  Role-side
    // instance (the inbox receiver is a separate process from the hub).
    pylabhub::utils::ReplayGuard replay_guard_;

    // Sequence tracking — per-sender, keyed by the uid resolved from the
    // proven key (HEP-CORE-0027 §3.7).  Keyed on the routing id it would let
    // a sender reset another's expected sequence, or its own.
    // A single global counter is meaningless with multiple senders; each sender's
    // sequence is independent and wraps independently.
    std::unordered_map<std::string, uint64_t> sender_expected_seq_;

    // ── CURVE-server auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────
    // Set by set_curve_server_identity() BEFORE start().  There is no
    // unencrypted inbox: `start()` PANICs if this is still empty, so an
    // unarmed InboxQueue cannot exist past construction.
    std::string identity_key_name_; ///< KeyStore key (kRoleIdentityName).
    std::string zap_domain_;        ///< Distinct inbox ZAP domain ("<uid>:inbox").

    // The authority is_peer_allowed asks (HEP-CORE-0035 §4.9.6).  nullptr ==
    // deny-all (secure default between the S1 bind and the role binding
    // itself in).  Written by set_admission_authority (any thread), read on
    // the ZapRouter pump thread — atomic, so the read is lock-free as the
    // reentrance contract prefers, and the shared_ptr keeps the callable
    // alive for the duration of a call that races a rebind.
    std::atomic<std::shared_ptr<const InboxQueue::InboxAuthority>> authority_{nullptr};

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
    size_t item_sz{0};

    std::vector<wire_detail::WireFieldDesc> schema_defs_;

    // Shared ZMQ context via get_zmq_context(). InboxClient never creates
    // or terminates it.
    zmq::socket_t socket;

    std::atomic<bool> running_{false};
    std::vector<std::byte> write_buf_; // zero-initialized

    // reusable send buffer
    msgpack::sbuffer sbuf_;
    std::atomic<uint64_t> send_seq_{0};

    std::array<uint8_t, 8> schema_tag_{}; // first 8 bytes of BLAKE2b-256 of canonical schema string
    ChecksumPolicy checksum_policy_{ChecksumPolicy::Enforced};
    int last_acktimeo{-2}; ///< Cached ZMQ_RCVTIMEO for ACK receives. -2 = not yet set.

    // ── Back-pressure edge latch ──────────────────────────────────────────
    // A DEALER with no writable pipe reports EAGAIN, and that one errno
    // covers both "the receiver's inbox is full" and "there is no peer"
    // (libzmq `lb_t::sendpipe`: `if (_active == 0) { errno = EAGAIN; }`).
    // libzmq exposes no receive-queue depth, so the sender's EAGAIN is the
    // ONLY place either condition is observable in-process.
    //
    // The latch makes the log edge-triggered: one line when the send path
    // becomes blocked, one when it recovers.  A peer that stays blocked
    // stays silent — a per-send line would bury the log at exactly the
    // moment the operator needs to read it.
    bool send_blocked_{false};
    std::atomic<uint64_t> send_blocked_count_{0};

    /// Receipts discarded because their `seq` belonged to an earlier send
    /// whose ACK wait had already expired.  A non-zero value means the ACK
    /// timeout is tuned tighter than the receiver's actual turnaround —
    /// before the ACK carried a seq, each of these was silently returned as
    /// the CURRENT message's result.
    std::atomic<uint64_t> ack_stale_count_{0};

    // ── CURVE-client auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────
    // Set by set_curve_client_identity() BEFORE start().  `start()` PANICs if
    // this is still empty — there is no plaintext sender.
    std::string identity_key_name_; ///< KeyStore key (kRoleIdentityName).
    std::string server_pubkey_z85_; ///< Receiver identity pubkey (curve_serverkey).
};

// ============================================================================
// Schema validation helper
// ============================================================================

// ── Replay-metadata frame (HEP-CORE-0027 §3.6) ──────────────────────────────
// A fixed 24-byte frame carried between the ROUTER/DEALER delimiter and the
// msgpack payload: [ client_nonce : 16 ][ client_wall_ts : uint64 big-endian ].
// Kept OUT of the msgpack payload so the payload codec (`zmq_wire_helpers`)
// stays byte-identical to the data-plane ZmqQueue frame it shares.
namespace
{
constexpr std::size_t kInboxNonceLen = 16;
constexpr std::size_t kInboxMetaLen = kInboxNonceLen + 8;
constexpr std::uint64_t kInboxReplaySkewMs = 30'000;
// I-REPLAY-BOUND soundness: window MUST be >= 2 * skew.  A replay stays
// skew-acceptable for up to 2*skew after the original (skew tolerance
// applies to both the original acceptance and the replay), so the nonce
// must be remembered at least that long.  See ReplayGuard header.
constexpr std::uint64_t kInboxReplayWindowMs = 2 * kInboxReplaySkewMs;

std::uint64_t inbox_now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
void inbox_put_be64(unsigned char *p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[7 - i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFu);
}
std::uint64_t inbox_get_be64(const unsigned char *p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}
} // namespace

static bool validate_inbox_schema(const std::vector<ZmqSchemaField> &schema,
                                  const std::string &endpoint)
{
    if (schema.empty())
    {
        LOGGER_ERROR("[hub::InboxQueue] '{}': schema must not be empty", endpoint);
        return false;
    }
    for (const auto &f : schema)
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
static std::array<uint8_t, 8> compute_inbox_schema_tag(const std::vector<ZmqSchemaField> &schema,
                                                       const std::string &packing)
{
    // Build canonical string: "type:count:length;" per field, then "|pack:<packing>".
    std::string canonical;
    for (const auto &f : schema)
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
    // Unchecked deliberately — same reasoning as `wire_envelope.cpp`: an
    // all-zero return would make EVERY schema tag identical, so mismatch
    // detection would stop discriminating.  Unreachable because `canonical`
    // is a std::string (data() never null) and the fixed-outlen hash does not
    // fail.  Revisit if either premise changes.
    auto full_hash = pylabhub::utils::security::secure().compute_blake2b_array(canonical.data(),
                                                                               canonical.size());
    std::array<uint8_t, 8> tag{};
    std::memcpy(tag.data(), full_hash.data(), 8);
    return tag;
}

// ── ACK frame (HEP-CORE-0027 §3.7, HEP-CORE-0047 §3.0) ──────────────────────
// The ACK rides the SAME typed-data codec as the message it acknowledges —
// `wire_detail` is the only msgpack coder in the tree and nothing bypasses it.
// Correlation costs nothing extra: `seq` is already element [2] of the 5-tuple
// envelope, so the receiver echoes the seq it processed and the sender can tell
// its own receipt from a stale one left over from an earlier, timed-out send.
// The payload is a single field: the ack code.
namespace
{
const std::vector<ZmqSchemaField> &inbox_ack_fields()
{
    static const std::vector<ZmqSchemaField> f{{"uint8", 1, 0}};
    return f;
}
const std::vector<wire_detail::WireFieldDesc> &inbox_ack_defs()
{
    static const std::vector<wire_detail::WireFieldDesc> d =
        wire_detail::compute_field_layout(inbox_ack_fields(), "aligned").first;
    return d;
}

/// Schema tag identifying an ACK frame.  Derived from the ack field list the
/// same way every other tag is, so an ACK can never be mistaken for a data
/// frame (or vice versa) — the receiver of either checks the tag before
/// trusting the payload.  Computed once, on first use, after SecureSubsystem
/// is up (both call sites run only after `start()`).
const std::array<uint8_t, 8> &inbox_ack_tag()
{
    static const std::array<uint8_t, 8> t = compute_inbox_schema_tag(inbox_ack_fields(), "aligned");
    return t;
}
} // namespace

static bool validate_inbox_packing(const std::string &packing, const std::string &endpoint)
{
    if (packing != "aligned" && packing != "packed")
    {
        LOGGER_ERROR(
            "[hub::InboxQueue] '{}': invalid packing '{}' (must be \"aligned\" or \"packed\")",
            endpoint, packing);
        return false;
    }
    return true;
}

// ============================================================================
// InboxQueue — factory
// ============================================================================

std::unique_ptr<InboxQueue> InboxQueue::bind_at(const std::string &endpoint,
                                                std::vector<ZmqSchemaField> schema,
                                                std::string packing, int rcvhwm)
{
    if (!validate_inbox_schema(schema, endpoint))
        return nullptr;
    if (!validate_inbox_packing(packing, endpoint))
        return nullptr;

    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl = std::make_unique<InboxQueueImpl>();
    impl->endpoint = endpoint;
    impl->item_sz = item_sz;
    impl->max_frame_sz = wire_detail::max_frame_size(layouts);
    impl->rcvhwm = rcvhwm;
    impl->schema_tag_ = compute_inbox_schema_tag(schema, packing);
    impl->schema_defs_ = std::move(layouts);
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

InboxQueue::InboxQueue(InboxQueue &&) noexcept = default;

InboxQueue &InboxQueue::operator=(InboxQueue &&o) noexcept
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
    if (!pImpl)
        return false;
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race

    // `running_` is true from here, so every exit that is not a completed
    // start must put it back.  One guard owns that, rather than each
    // `catch` handler repeating it — see ZmqQueue::start for the failure
    // this shape prevents (a throw of a type nobody enumerated skipping
    // the cleanup, leaving the queue Active-looking with nothing bound,
    // after which the idempotence check reports success on every retry).
    auto start_guard = pylabhub::basics::make_scope_guard(
        [impl = pImpl.get()]() noexcept
        {
            impl->zap_handle_.reset();
            impl->socket.close();
            impl->running_.store(false, std::memory_order_release);
        });

    try
    {
        pImpl->socket = zmq::socket_t(pylabhub::hub::get_zmq_context(), zmq::socket_type::router);
        // House ZMQ policy (linger, bounded sndtimeo, ZMTP heartbeat).  This
        // header names "Inbox receiver ROUTER accepting DEALER senders" as a
        // TcpBind user in its own docs, and the inbox never called it — so it
        // ran on libzmq's raw defaults: no heartbeat, unbounded send block.
        // That unbounded send is the 60s hang this arc started from.
        pylabhub::utils::apply_socket_policy(pImpl->socket,
                                             pylabhub::utils::ZmqSocketRole::TcpBind);
        pImpl->socket.set(zmq::sockopt::rcvhwm, pImpl->rcvhwm);

        // ── CURVE-server arm (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ──
        // Mirror of ZmqQueue's bind-side pattern: identity keypair from
        // secure().keys() (secret never leaves the module — flows into
        // libzmq inside the with_seckey callback), curve_server=1, a
        // DISTINCT inbox zap_domain, and register_domain BEFORE bind so
        // the ZapRouter can gate the first handshake.  Until a role binds
        // its roster in, `admits_` is nullptr → is_peer_allowed denies all
        // (secure default).
        // CURVE is not optional and there is no unarmed shape to fall back
        // to.  This used to be `if (!identity_key_name_.empty())`, which
        // silently built a PLAINTEXT ROUTER when the caller forgot to arm —
        // an inbox listening with no authentication, reading as sanctioned
        // because the branch looked deliberate.  Unreachable in production
        // (`role_host_helpers.hpp` arms unconditionally) is not a guarantee;
        // it is a backdoor nobody is watching.
        if (pImpl->identity_key_name_.empty())
        {
            PLH_PANIC("InboxQueue::start: no CURVE identity armed for "
                      "endpoint='{}'.  set_curve_server_identity() MUST be "
                      "called before start() — an inbox ROUTER without CURVE "
                      "would accept unauthenticated peers, and there is no "
                      "plaintext inbox in this system (HEP-CORE-0027 §3.5, "
                      "HEP-CORE-0035 §2).",
                      pImpl->endpoint);
        }
        {
            namespace sec = pylabhub::utils::security;
            // Shared CURVE-server arm (use-not-export) — same helper the broker
            // ROUTER and admin console use; keyed with the role identity.
            pylabhub::utils::arm_curve_server(pImpl->socket, pImpl->identity_key_name_);
            pImpl->socket.set(zmq::sockopt::zap_domain, pImpl->zap_domain_);

            // register_domain BEFORE bind (same ordering as ZmqQueue) so
            // no handshake can slip through un-gated.  `*this` is the
            // PeerAdmission the pump thread consults.
            pImpl->zap_handle_.emplace(
                sec::ZapRouter::instance().register_domain(pImpl->zap_domain_, *this));
            LOGGER_INFO("[hub::InboxQueue] CURVE-server armed endpoint='{}' "
                        "zap_domain='{}' (deny-all until roster seeded; "
                        "HEP-CORE-0027 §3.5)",
                        pImpl->endpoint, pImpl->zap_domain_);
        }

        pImpl->socket.bind(pImpl->endpoint);
        pImpl->actual_ep = pImpl->socket.get(zmq::sockopt::last_endpoint);
    }
    // Each handler below returns false and lets `start_guard` unwind on the
    // way out; they differ only in the diagnostic they can offer.
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("[hub::InboxQueue] socket setup failed for '{}': {}", pImpl->endpoint,
                     e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[hub::InboxQueue] CURVE arm failed for '{}': {}", pImpl->endpoint, e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("[hub::InboxQueue] start failed for '{}': unknown exception", pImpl->endpoint);
        return false;
    }

    // Bound, armed and registered — the one path that keeps `running_`.
    start_guard.dismiss();
    return true;
}

void InboxQueue::stop()
{
    if (!pImpl)
        return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    // Enter/exit pairs around each blocking step: this teardown runs while
    // the ZAP pump thread is still live, so a stall here is only diagnosable
    // if the log says which step was entered and never left.
    LOGGER_INFO("[hub::InboxQueue] stop:enter endpoint='{}' zap_domain='{}'", pImpl->endpoint,
                pImpl->zap_domain_);

    // Close the socket; shared context is owned by the ZMQContext lifecycle module.
    pImpl->socket.close();
    LOGGER_INFO("[hub::InboxQueue] stop:socket-closed endpoint='{}'", pImpl->endpoint);

    // Unregister from the ZapRouter (RAII handle destructor); the domain
    // becomes free for a future re-bind.  After this the pump thread no
    // longer holds a reference to this InboxQueue's is_peer_allowed.
    pImpl->zap_handle_.reset();
    LOGGER_INFO("[hub::InboxQueue] stop:exit endpoint='{}' zap_domain='{}'", pImpl->endpoint,
                pImpl->zap_domain_);
}

bool InboxQueue::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

std::optional<::pylabhub::BoundAddress> InboxQueue::bound_address() const
{
    if (!pImpl || pImpl->actual_ep.empty())
        return std::nullopt;
    // No fallback to the configured endpoint: the inbox default IS
    // port 0, so the fallback would have published an unconnectable
    // address as the role's inbox (HEP-CORE-0036 §6.7.2).
    return ::pylabhub::BoundAddress::try_validate(pImpl->actual_ep);
}

size_t InboxQueue::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

// ============================================================================
// InboxQueue — recv_one
// ============================================================================

const InboxItem *InboxQueue::recv_one(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || !pImpl->socket)
        return nullptr;

    // HR-03: only apply RCVTIMEO when it changes (saves a syscall per call).
    const int timeout_ms = static_cast<int>(timeout.count());
    if (timeout_ms != pImpl->last_rcvtimeo)
    {
        pImpl->socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
        pImpl->last_rcvtimeo = timeout_ms;
    }

    // ROUTER delivers the four-frame envelope (identity, empty, replay_meta,
    // payload; HEP-CORE-0027 §3.6) as a single multipart message.
    // multipart_t::recv reads all frames atomically:
    // either the whole message is available or the call returns false/throws.
    zmq::multipart_t parts;
    try
    {
        if (!parts.recv(pImpl->socket))
            return nullptr; // RCVTIMEO expired or EAGAIN
    }
    catch (const zmq::error_t &e)
    {
        if (e.num() != ETERM && e.num() != EINTR)
            LOGGER_WARN("[hub::InboxQueue] recv failed: {}", e.what());
        return nullptr;
    }

    if (parts.size() != 4)
    {
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    // Frame 0 is the ROUTER routing id.  It is an ACK return address and
    // NOTHING else: a DEALER picks its own, so it is a label the sender wrote
    // (HEP-CORE-0027 §3.7).
    std::string route_id = parts[0].to_string();
    // parts[1] is the empty delimiter (size==0, ROUTER/DEALER pattern).
    // parts[2] is the replay-metadata frame; parts[3] is the msgpack payload.

    // ── Who sent this (HEP-CORE-0027 §3.7) ───────────────────────────────
    // The name comes from the key the CURVE handshake PROVED, never from the
    // frame above.  libzmq stamps the ZAP metadata on every part, so frame 0
    // carries the attestation as well as the address — the same frame the
    // control plane reads for this (`WireEnvelope::parse_router_recv`).
    //
    // Keying the three identity uses below on the routing id instead would
    // let one sender be attributed as another AND let a sender earn a fresh
    // replay window by presenting a new id, which is why this is derived
    // once, here, before any of them.
    //
    // The optional goes in as-is: `attribute_sender` answers `no_attestation`
    // for an absent one, so an unarmed socket needs no separate branch.
    std::string sender_id;
    {
        namespace sec = pylabhub::utils::security;
        auto authority = pImpl->authority_.load(std::memory_order_acquire);
        const auto attested = sec::AttestedKey::from_message(pImpl->socket, parts[0]);
        const auto who = (authority && authority->name_of) ? authority->name_of(attested)
                                                           : sec::AttributedSender{};
        if (who.verdict != sec::ClaimVerdict::accepted)
        {
            // Unnameable is undeliverable.  A message whose sender cannot be
            // named has no replay key, no sequence state and nothing to
            // report to the application — and admitting it would mean
            // inventing one of those.  Reachable for a federation peer
            // dialling a role mailbox (`kind_not_permitted`), and for any
            // frame arriving before the role binds its authority.
            pImpl->recv_unattributed_count_.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[hub::InboxQueue] dropping frame with no attributable sender: "
                        "verdict={} route_id='{}' (HEP-CORE-0027 §3.7)",
                        sec::to_string(who.verdict), route_id);
            return nullptr;
        }
        sender_id = who.uid;
    }

    // ── Replay defense (HEP-CORE-0027 §3.6) ──────────────────────────────
    // Skew-check the wall_ts, then dedup the nonce via the shared
    // ReplayGuard (keyed by sender identity).  A rejected frame is dropped
    // BEFORE any handler runs, so a replayed side effect never fires.
    {
        zmq::message_t &meta = parts[2];
        if (meta.size() != kInboxMetaLen)
        {
            ++pImpl->recv_frame_error_count_;
            return nullptr;
        }
        const auto *m = static_cast<const unsigned char *>(meta.data());
        const std::uint64_t wall_ts = inbox_get_be64(m + kInboxNonceLen);
        const std::uint64_t now = inbox_now_ms();
        const std::uint64_t skew = now > wall_ts ? now - wall_ts : wall_ts - now;
        if (skew > kInboxReplaySkewMs)
        {
            pImpl->recv_replay_reject_count_.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[hub::InboxQueue] dropping frame from '{}': "
                        "wall-clock skew {}ms exceeds tolerance",
                        sender_id, skew);
            return nullptr;
        }
        const std::string_view nonce(reinterpret_cast<const char *>(m), kInboxNonceLen);
        // Dedup: the ReplayGuard prunes against its OWN trusted monotonic
        // clock — no timestamp is passed, so the client `wall_ts` (confined
        // to the skew gate above) cannot reach the dedup window (see
        // ReplayGuard header).
        if (!pImpl->replay_guard_.check_and_record(sender_id, nonce, kInboxReplayWindowMs))
        {
            pImpl->recv_replay_reject_count_.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[hub::InboxQueue] dropping replayed frame from '{}' "
                        "(nonce reused within window)",
                        sender_id);
            return nullptr;
        }
    }

    zmq::message_t &payload = parts[3];

    if (payload.size() >= pImpl->max_frame_sz)
    {
        ++pImpl->recv_frame_error_count_;
        return nullptr;
    }

    try
    {
        auto frame =
            wire_detail::decode_frame(payload.data(), payload.size(), pImpl->schema_defs_.size());

        const auto &env = frame.env;
        if (!env.valid || env.payload_size != pImpl->schema_defs_.size())
        {
            ++pImpl->recv_frame_error_count_;
            return nullptr;
        }

        // Schema-tag mismatch.
        if (std::memcmp(env.recv_tag, pImpl->schema_tag_.data(), 8) != 0)
        {
            ++pImpl->recv_frame_error_count_;
            return nullptr;
        }

        // Per-sender sequence gap tracking.  The same subtraction feeds two
        // consumers: the process-wide counter (an operator metric) and the
        // per-message `gap` the receiving handler sees.  The handler needs it
        // per message because "23 lost overall" cannot tell it WHICH state
        // it is now missing; "3 lost right before this one" can.
        {
            uint64_t gap = 0;
            auto it = pImpl->sender_expected_seq_.find(sender_id);
            if (it != pImpl->sender_expected_seq_.end())
            {
                if (env.seq > it->second)
                {
                    gap = env.seq - it->second;
                    pImpl->recv_gap_count_.fetch_add(gap, std::memory_order_relaxed);
                }
                it->second = env.seq + 1;
            }
            else
            {
                // First message from this sender — there is no previous
                // sequence to measure against, so nothing is known to be
                // missing.  Reporting `env.seq` here would brand every
                // late-joining sender as lossy.
                pImpl->sender_expected_seq_.emplace(sender_id, env.seq + 1);
            }
            pImpl->current_item_.seq = env.seq;
            pImpl->current_item_.gap = gap;
        }

        // Decode payload fields.
        std::fill(pImpl->decode_buf_.begin(), pImpl->decode_buf_.end(), std::byte{0});
        if (!wire_detail::unpack_payload(*env.payload, pImpl->schema_defs_,
                                         pImpl->decode_buf_.data()))
        {
            ++pImpl->recv_frame_error_count_;
            return nullptr;
        }

        // Checksum verification.
        if (pImpl->checksum_policy_ != ChecksumPolicy::None)
        {
            if (!pylabhub::utils::security::secure().verify_blake2b(
                    env.checksum, pImpl->decode_buf_.data(), pImpl->item_sz))
            {
                pImpl->checksum_error_count_.fetch_add(1, std::memory_order_relaxed);
                LOGGER_ERROR("[hub::InboxQueue] checksum error after decode from '{}'", sender_id);
                return nullptr;
            }
        }
    }
    catch (const std::exception &e)
    {
        ++pImpl->recv_frame_error_count_;
        LOGGER_WARN("[hub::InboxQueue] unpack error from '{}': {}", sender_id, e.what());
        return nullptr;
    }

    pImpl->current_route_id_ = std::move(route_id);
    pImpl->current_item_.data = pImpl->decode_buf_.data();
    pImpl->current_item_.sender_id = std::move(sender_id);
    return &pImpl->current_item_;
}

// ============================================================================
// InboxQueue — send_ack
// ============================================================================

void InboxQueue::send_ack(uint8_t code) noexcept
{
    if (!pImpl || !pImpl->socket)
        return;

    const std::string &id = pImpl->current_route_id_;

    try
    {
        // Echo the seq of the message being acknowledged.  That is the whole
        // point of framing the ACK: without it the sender cannot tell this
        // receipt from one left over by an earlier send whose ACK wait timed
        // out, and would report a stale code — including a stale SUCCESS —
        // for a message the receiver never processed (§3.7).
        msgpack::sbuffer sbuf;
        msgpack::packer<msgpack::sbuffer> pk(sbuf);
        const uint8_t ack_checksum[32]{}; // ACK carries no payload integrity of its own
        wire_detail::pack_frame(pk, inbox_ack_tag(), pImpl->current_item_.seq, inbox_ack_defs(),
                                &code, ack_checksum);

        zmq::multipart_t ack;
        ack.addstr(id);                       // routing identity
        ack.addstr("");                       // empty delimiter
        ack.addmem(sbuf.data(), sbuf.size()); // framed ACK (5-tuple, seq-correlated)
        if (!ack.send(pImpl->socket))
            ++pImpl->ack_send_error_count_;
    }
    catch (const std::exception &)
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

uint64_t InboxQueue::recv_replay_reject_count() const noexcept
{
    return pImpl ? pImpl->recv_replay_reject_count_.load(std::memory_order_relaxed) : 0;
}

void InboxQueue::set_checksum_policy(ChecksumPolicy policy) noexcept
{
    if (pImpl)
        pImpl->checksum_policy_ = policy;
}

// ── CURVE-server auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ────────────

void InboxQueue::set_curve_server_identity(std::string identity_key_name, std::string zap_domain)
{
    if (!pImpl)
        return;
    pImpl->identity_key_name_ = std::move(identity_key_name);
    pImpl->zap_domain_ = std::move(zap_domain);
}

void InboxQueue::set_admission_authority(InboxAuthority authority)
{
    if (!pImpl)
        return;
    pImpl->authority_.store(std::make_shared<const InboxAuthority>(std::move(authority)),
                            std::memory_order_release);
}

bool InboxQueue::set_peer_allowlist(pylabhub::utils::security::PeerAllowlist /*allowlist*/)
{
    LOGGER_WARN("[hub::InboxQueue::set_peer_allowlist] inert (queue='{}') — the inbox gate "
                "holds no list; bind an authority with set_admission_authority() instead "
                "(HEP-CORE-0035 §4.9.6)",
                pImpl ? pImpl->endpoint : std::string{});
    return false;
}

std::optional<pylabhub::utils::security::PeerAllowlist> InboxQueue::peer_allowlist_snapshot() const
{
    // No stored list, and the authority answers one key at a time — there is
    // nothing to enumerate.  `nullopt` is the interface's own spelling of
    // "this instance does not expose its state".
    return std::nullopt;
}

bool InboxQueue::is_peer_allowed(const pylabhub::utils::security::PeerIdentity &peer) const
{
    if (!pImpl)
        return false;
    // The ROUTER is a CURVE server and nothing else, so a peer arriving under
    // any other mechanism is not one this gate has an opinion about.
    if (peer.kind != pylabhub::utils::security::kCurveMechanism)
        return false;
    // No authority bound == deny-all (secure default between the S1 bind and
    // the role binding itself in).
    auto authority = pImpl->authority_.load(std::memory_order_acquire);
    if (!authority || !authority->admits)
        return false;
    return authority->admits(peer.data);
}

// ============================================================================
// InboxClient — factory
// ============================================================================

std::unique_ptr<InboxClient> InboxClient::connect_to(const std::string &endpoint,
                                                     const std::string &sender_uid,
                                                     std::vector<ZmqSchemaField> schema,
                                                     std::string packing)
{
    if (!validate_inbox_schema(schema, endpoint))
        return nullptr;
    if (!validate_inbox_packing(packing, endpoint))
        return nullptr;

    auto [layouts, item_sz] = wire_detail::compute_field_layout(schema, packing);

    auto impl = std::make_unique<InboxClientImpl>();
    impl->endpoint = endpoint;
    impl->sender_uid = sender_uid;
    impl->item_sz = item_sz;
    impl->schema_tag_ = compute_inbox_schema_tag(schema, packing);
    impl->schema_defs_ = std::move(layouts);
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

InboxClient::InboxClient(InboxClient &&) noexcept = default;

InboxClient &InboxClient::operator=(InboxClient &&o) noexcept
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
    if (!pImpl)
        return false;
    if (pImpl->running_.load(std::memory_order_acquire))
        return true; // already running — idempotent
    if (pImpl->running_.exchange(true, std::memory_order_acq_rel))
        return true; // lost race

    // Same contract as InboxQueue::start and ZmqQueue::start: one guard
    // owns the unwind so no exit path can leave `running_` set on a queue
    // that never connected.
    auto start_guard = pylabhub::basics::make_scope_guard(
        [impl = pImpl.get()]() noexcept
        {
            impl->socket.close();
            impl->running_.store(false, std::memory_order_release);
        });

    try
    {
        pImpl->socket = zmq::socket_t(pylabhub::hub::get_zmq_context(), zmq::socket_type::dealer);
        // House ZMQ policy — this header names "Inbox sender DEALER connecting
        // to a peer ROUTER" as a TcpConnect user.  MUST precede connect():
        // reconnect options only take effect when set pre-connect.
        pylabhub::utils::apply_socket_policy(pImpl->socket,
                                             pylabhub::utils::ZmqSocketRole::TcpConnect);
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
        // Same invariant as the ROUTER side: an unarmed DEALER would send the
        // payload in the clear to a peer it never authenticated.
        if (pImpl->identity_key_name_.empty())
        {
            PLH_PANIC("InboxClient::start: no CURVE identity armed for "
                      "endpoint='{}'.  set_curve_client_identity() MUST be "
                      "called before start() (HEP-CORE-0027 §3.5).",
                      pImpl->endpoint);
        }
        pylabhub::utils::arm_curve_client(pImpl->socket, pImpl->identity_key_name_,
                                          pImpl->server_pubkey_z85_);

        pImpl->socket.connect(pImpl->endpoint);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("[hub::InboxClient] socket setup failed for '{}': {}", pImpl->endpoint,
                     e.what());
        return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[hub::InboxClient] CURVE arm failed for '{}': {}", pImpl->endpoint, e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("[hub::InboxClient] start failed for '{}': unknown exception",
                     pImpl->endpoint);
        return false;
    }

    // Connected and armed — the one path that keeps `running_`.
    start_guard.dismiss();
    return true;
}

void InboxClient::stop()
{
    if (!pImpl)
        return;
    if (!pImpl->running_.exchange(false, std::memory_order_acq_rel))
        return;

    LOGGER_INFO("[hub::InboxClient] stop:enter endpoint='{}'", pImpl->endpoint);
    // Close socket; shared context stays up — owned by ZMQContext module.
    pImpl->socket.close();
    LOGGER_INFO("[hub::InboxClient] stop:exit endpoint='{}'", pImpl->endpoint);
}

bool InboxClient::is_running() const noexcept
{
    return pImpl && pImpl->running_.load(std::memory_order_relaxed);
}

size_t InboxClient::item_size() const noexcept
{
    return pImpl ? pImpl->item_sz : 0;
}

uint64_t InboxClient::send_blocked_count() const noexcept
{
    return pImpl ? pImpl->send_blocked_count_.load(std::memory_order_relaxed) : 0;
}

uint64_t InboxClient::ack_stale_count() const noexcept
{
    return pImpl ? pImpl->ack_stale_count_.load(std::memory_order_relaxed) : 0;
}

// ============================================================================
// InboxClient — acquire / send / abort
// ============================================================================

void *InboxClient::acquire() noexcept
{
    if (!pImpl || !pImpl->socket)
        return nullptr;
    // Zero-initialize before returning to caller
    std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
    return pImpl->write_buf_.data();
}

uint8_t InboxClient::send(std::chrono::milliseconds ack_timeout) noexcept
{
    if (!pImpl || !pImpl->socket)
        return 255;

    // Pack msgpack frame
    pImpl->sbuf_.clear();
    msgpack::packer<msgpack::sbuffer> pk(pImpl->sbuf_);

    // Compute BLAKE2b checksum: Enforced = auto-stamp, Manual/None = zeros.
    uint8_t checksum[32]{};
    if (pImpl->checksum_policy_ == ChecksumPolicy::Enforced)
    {
        pylabhub::utils::security::secure().compute_blake2b(checksum, pImpl->write_buf_.data(),
                                                            pImpl->item_sz);
    }

    // The sequence number is consumed here, BEFORE the send, and is NOT
    // rolled back if the send is refused below.  That is deliberate: a
    // dropped message leaves a hole, and the receiver's per-sender gap
    // tracking (`recv_gap_count`) is how that loss becomes visible on the
    // far side.  Renumbering densely on failure would make a lossy link
    // look pristine — the sender would know it dropped traffic and the
    // receiver never would.
    const uint64_t sent_seq = pImpl->send_seq_.fetch_add(1, std::memory_order_relaxed);
    wire_detail::pack_frame(pk, pImpl->schema_tag_, sent_seq, pImpl->schema_defs_,
                            pImpl->write_buf_.data(), checksum);

    // DEALER sends [empty, payload]; ROUTER sees [identity, empty, payload].
    // multipart_t::send is atomic — every frame goes out or none.
    try
    {
        // Replay metadata (HEP-CORE-0027 §3.6): fresh random nonce +
        // current wall-clock, stamped per send so the receiver can skew-
        // and dedup-check before running the handler.
        unsigned char meta[kInboxMetaLen];
        pylabhub::utils::security::secure().random_bytes(meta, kInboxNonceLen);
        inbox_put_be64(meta + kInboxNonceLen, inbox_now_ms());

        // `dontwait` (inside `send_multipart_atomic`) is what makes this call
        // bounded.  libzmq's default ZMQ_SNDTIMEO of -1 retries forever when
        // the DEALER has no writable peer, so a denied CURVE handshake or a
        // departed peer parked the calling thread permanently — no error, no
        // timeout, no log.  It also made the documented `ack_timeout == 0`
        // fire-and-forget contract false, because the block happened before
        // this function ever read `ack_timeout`.
        //
        // Multipart atomicity — which part failed, and whether libzmq's
        // discard latch must be cleared — is NOT this function's knowledge.
        // It is a house rule about using a ZMQ socket, so it lives with the
        // rest of them in `zmq_socket_policy.hpp` and applies to any
        // multipart sender rather than to this one message format.
        if (!pylabhub::utils::send_multipart_atomic(
                pImpl->socket, {zmq::buffer("", 0),               // empty delimiter
                                zmq::buffer(meta, kInboxMetaLen), // replay metadata
                                zmq::buffer(pImpl->sbuf_.data(), pImpl->sbuf_.size())}))
        {
            pImpl->send_blocked_count_.fetch_add(1, std::memory_order_relaxed);
            if (!pImpl->send_blocked_)
            {
                pImpl->send_blocked_ = true;
                LOGGER_WARN("[hub::InboxClient] send:blocked endpoint='{}' — no writable "
                            "peer (receiver's inbox is full, or the peer is not connected / was "
                            "denied).  Message dropped; further blocked sends are counted, not "
                            "logged, until the path recovers",
                            pImpl->endpoint);
            }
            return 255;
        }
        if (pImpl->send_blocked_)
        {
            pImpl->send_blocked_ = false;
            LOGGER_INFO("[hub::InboxClient] send:recovered endpoint='{}' blocked_total={}",
                        pImpl->endpoint,
                        pImpl->send_blocked_count_.load(std::memory_order_relaxed));
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("[hub::InboxClient] send to '{}' failed: {}", pImpl->endpoint, e.what());
        return 255;
    }

    // Fire-and-forget
    if (ack_timeout.count() <= 0)
        return 0;

    // ROUTER reply: [identity, "", ack_frame]; DEALER strips the identity and
    // sees ["", ack_frame].  The ACK is a full 5-tuple frame whose envelope
    // `seq` echoes the message it acknowledges.
    //
    // Why the loop: a previous send whose ACK wait expired may still have its
    // receipt in flight.  Without correlation that receipt would be read here
    // and returned as THIS message's result — reporting a stale code, and
    // since the only code production ever sends is 0, reporting stale SUCCESS
    // for a message the receiver never processed.  So a receipt whose seq is
    // not ours is discarded and we keep waiting, bounded by the caller's
    // deadline rather than by a per-receive timeout.
    const auto deadline = std::chrono::steady_clock::now() + ack_timeout;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            LOGGER_WARN("[hub::InboxClient] ACK timeout from '{}' (seq={})", pImpl->endpoint,
                        sent_seq);
            return 255;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        // Never 0: that would turn the receive non-blocking and spin the loop.
        const int wait_ms = static_cast<int>(remaining > 0 ? remaining : 1);
        if (wait_ms != pImpl->last_acktimeo)
        {
            pImpl->socket.set(zmq::sockopt::rcvtimeo, wait_ms);
            pImpl->last_acktimeo = wait_ms;
        }

        try
        {
            zmq::multipart_t reply;
            if (!reply.recv(pImpl->socket))
            {
                LOGGER_WARN("[hub::InboxClient] ACK timeout from '{}' (seq={})", pImpl->endpoint,
                            sent_seq);
                return 255;
            }
            if (reply.size() != 2)
            {
                LOGGER_WARN("[hub::InboxClient] malformed ACK from '{}' (frames={})",
                            pImpl->endpoint, reply.size());
                return 255;
            }

            const auto frame = wire_detail::decode_frame(reply[1].data(), reply[1].size(),
                                                         inbox_ack_defs().size());
            const auto &env = frame.env;
            if (!env.valid || env.payload_size != inbox_ack_defs().size() ||
                std::memcmp(env.recv_tag, inbox_ack_tag().data(), 8) != 0)
            {
                LOGGER_WARN("[hub::InboxClient] malformed ACK frame from '{}'", pImpl->endpoint);
                return 255;
            }

            if (env.seq != sent_seq)
            {
                // A receipt for an earlier message, arriving after its wait
                // expired.  Drop it and keep waiting for ours.
                pImpl->ack_stale_count_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            uint8_t code = 255;
            if (!wire_detail::unpack_payload(*env.payload, inbox_ack_defs(), &code))
            {
                LOGGER_WARN("[hub::InboxClient] undecodable ACK payload from '{}'",
                            pImpl->endpoint);
                return 255;
            }
            return code;
        }
        catch (const std::exception &e)
        {
            LOGGER_WARN("[hub::InboxClient] ACK recv error from '{}': {}", pImpl->endpoint,
                        e.what());
            return 255;
        }
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
    if (pImpl)
        pImpl->checksum_policy_ = policy;
}

void InboxClient::set_curve_client_identity(std::string identity_key_name,
                                            std::string server_pubkey_z85)
{
    if (!pImpl)
        return;
    pImpl->identity_key_name_ = std::move(identity_key_name);
    pImpl->server_pubkey_z85_ = std::move(server_pubkey_z85);
}

} // namespace pylabhub::hub
