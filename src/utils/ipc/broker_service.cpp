#include "utils/broker_service.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_queue_factory.hpp" // Queue::reader_is_binding_side
#include "utils/hub_state_queries.hpp" // HEP-CORE-0039 for_each_presence_matching
#include "utils/hub_state_json.hpp"
#include "utils/naming.hpp" // is_valid_identifier (audit R3.5)
#include "utils/net_address.hpp"

#include "utils/recovery_api.hpp"
#include "utils/schema_loader.hpp"
#include "utils/schema_utils.hpp" // canonical_fields_str, compute_fingerprint_from_wire, make_schema_record
#include "utils/security/pubkey_origin.hpp" // PeerAuthority (HEP-CORE-0035 §4.2)

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/curve_keypair.hpp"  // HEP-CORE-0035 §2 — shared keygen
#include "utils/curve_socket.hpp"            // arm_curve_server (shared CURVE arm)
#include "utils/zmq_socket_policy.hpp"       // apply_socket_policy (house ZMQ rules)
#include "utils/security/key_store.hpp"      // HEP-CORE-0040 §172 — hub identity
#include "utils/security/peer_admission.hpp" // HEP-CORE-0035 Phase D
#include "utils/security/zap_router.hpp"     // HEP-CORE-0035 Phase D
#include "portable_atomic_shared_ptr.hpp"    // sibling header in src/utils/
#include "plh_version_registry.hpp"          // HEP-CORE-0032 §8 ABI fingerprint
#include "utils/timeout_constants.hpp"
#include "utils/wire_dispatch.hpp" // HEP-CORE-0046 unified receive+validate
#include "utils/wire_envelope.hpp" // HEP-CORE-0046 §14 typed envelope
#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "utils/json_fwd.hpp"
#include <zmq.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace pylabhub::broker
{

namespace
{
// Z85 keypair buffer: 40 printable chars + null terminator
constexpr size_t kZ85KeyBufSize = 41;
// Z85 key length (no null terminator)
constexpr size_t kZ85KeyLen = 40;
// Broker poll timeout in milliseconds (kept short so heartbeat timeouts are checked promptly)
constexpr std::chrono::milliseconds kPollTimeout{100};
// Universal framing: Frame 0 type byte for all ZMQ messages.
constexpr char kFrameTypeControl = 'C';

/// Decode a stored/claimed 128-hex fingerprint for COMPARISON purposes,
/// mapping an unparseable value to the all-zero fingerprint.
///
/// Conversion itself is `hub::fingerprint_from_hex` (HEP-CORE-0034
/// §2.4 I10 — the one sanctioned pair); this wrapper only chooses what a
/// comparison site does with a malformed input.  All-zero is the
/// "no format" sentinel, so a malformed value compares unequal to every
/// real fingerprint — the citation is rejected on the fingerprint axis
/// with the normal wire code, which is the desired outcome at these
/// sites.  Registration paths that must DISTINGUISH "malformed" from
/// "absent" call `fingerprint_from_hex` directly and branch on nullopt.
std::array<uint8_t, 64> fingerprint_for_compare(const std::string &hex) noexcept
{
    return hub::fingerprint_from_hex(hex).value_or(std::array<uint8_t, 64>{});
}

// ─── HEP-CORE-0033 §9.2 reply-shape classification ──────────────────────────
//
// Single source of truth for which msg_types are request-reply (ACK/ERROR
// reply expected) vs fire-and-forget (no reply, ever).  Used by the
// dispatcher to:
//   - validate that an inbound msg_type is known (R1: don't pollute
//     msg_type_counts with attacker-supplied unknowns); and
//   - decide whether the catch-block ERROR reply path runs (R3: never
//     send an ERROR reply for a fire-and-forget type).
//
// When adding a new msg_type to process_message(), it MUST also be listed
// in the appropriate array below.

constexpr std::array<std::string_view, 21> kRequestReplyTypes = {
    "REG_REQ",
    "DISC_REQ",
    "DEREG_REQ",
    "CONSUMER_REG_REQ",
    "CONSUMER_DEREG_REQ",
    "ENDPOINT_UPDATE_REQ",
    "SCHEMA_REQ",
    "CHANNEL_LIST_REQ",
    "METRICS_REQ",
    "SHM_BLOCK_QUERY_REQ",
    "ROLE_PRESENCE_REQ",
    "ROLE_INFO_REQ",
    "BAND_JOIN_REQ",
    "BAND_LEAVE_REQ",
    "BAND_MEMBERS_REQ",
    "HUB_PEER_HELLO",
    // Producer queries the current channel-scope allowlist on receipt
    // of CHANNEL_AUTH_CHANGED_NOTIFY (or proactively at setup) per
    // HEP-CORE-0036 §6.5.  Standard request-reply via existing
    // do_request infrastructure.
    "GET_CHANNEL_AUTH_REQ",
    // Producer asks the broker to confirm one specific consumer is
    // authorized for a channel before sending the SHM capability fd
    // (HEP-CORE-0041 §9 D4 pre-attach confirmation; HEP-CORE-0042
    // §6.1 Bindings.SHM).  Reply is CONSUMER_ATTACH_ACK_SHM with
    // status=success|denied for the auth decision, or ERROR for
    // protocol-level failures.  Read-only against HubState — pure
    // query, no mutation.  Renamed from CONSUMER_ATTACH_REQ →
    // CONSUMER_ATTACH_REQ_SHM (broker_proto 6 → 7, 2026-07-01) for
    // symmetry with the sibling ZMQ envelope below.
    "CONSUMER_ATTACH_REQ_SHM",
    // Consumer asks the broker to gate a ZMQ data-plane attach against
    // producer P for channel K (HEP-CORE-0042 §6.2 Bindings.ZMQ).  Reply
    // is CONSUMER_ATTACH_ACK_ZMQ with status=success (fast-path or drained
    // after producer confirmed cache) / denied / timeout.  Wait-path
    // enqueues the REQ and holds the reply until producer's
    // CHANNEL_AUTH_APPLIED_REQ advances confirmed_version[K][P].
    // Handler stub in Phase 2.1 — full impl in Phase 2.2 (fast-path)
    // and Phase 2.3 (wait-path + denial cases).
    "CONSUMER_ATTACH_REQ_ZMQ",
    // Producer confirms it has applied a specific allowlist snapshot
    // version (HEP-CORE-0042 §5.5.2 + §6.2).  Broker replies "ok",
    // advances confirmed_version[K][P] iff the producer's echoed
    // instance_id matches the current instance[P] (stale-instance
    // guard), and drains any pending CONSUMER_ATTACH_REQ_ZMQ entries
    // whose target_version ≤ confirmed_version.  Handler stub in
    // Phase 2.1 — full impl in Phase 2.2 (fast-path + APPLIED drain) +
    // Phase 2.4 (stale-instance drop).
    "CHANNEL_AUTH_APPLIED_REQ",
    // Dialing-side role asks the broker "is the binding-side ZAP
    // allowlist populated with my pubkey, i.e., will my next CURVE
    // handshake succeed?".  Read-only against HubState — resolved via
    // `HubState::is_pubkey_visible_to(chan, binding_role_uid, pk)`
    // which delegates to the channel's `VersionedAdmissionLedger`
    // (HEP-CORE-0042 §5.5.2 unified 2026-07-13).  Used by the fan-in
    // producer between REG_ACK apply and `socket.connect()`.
    // See HEP-CORE-0036 §6.6.3.
    "CHECK_PEER_READY_REQ",
};

constexpr std::array<std::string_view, 8> kFireAndForgetTypes = {
    // M1.4 (2026-05-11) — `METRICS_REPORT_REQ` retired.  Metrics now
    // piggyback on `HEARTBEAT_NOTIFY` per HEP-CORE-0019 §2.3 Phase 6.
    "HEARTBEAT_NOTIFY",
    "CHECKSUM_ERROR_REPORT",
    // Audit R3.6 (2026-05-17): CHANNEL_NOTIFY_REQ removed.  Role-side
    // BRC::send_notify was deleted in O1; federation peer-relay uses
    // HUB_RELAY_MSG (broker↔broker), not CHANNEL_NOTIFY_REQ.  No
    // caller exists anywhere in src/.  Old clients sending
    // CHANNEL_NOTIFY_REQ now receive UNKNOWN_MSG_TYPE.
    "CHANNEL_BROADCAST_SEND_NOTIFY",
    "BAND_BROADCAST_SEND_NOTIFY",
    "HUB_PEER_BYE",
    // Inbound on outbound DEALER (peer→us); not request-reply but valid:
    "HUB_RELAY_MSG",
    "HUB_TARGETED_MSG",
};

[[nodiscard]] bool is_request_reply(std::string_view t) noexcept
{
    for (auto x : kRequestReplyTypes)
        if (x == t)
            return true;
    return false;
}

[[nodiscard]] bool is_fire_and_forget(std::string_view t) noexcept
{
    for (auto x : kFireAndForgetTypes)
        if (x == t)
            return true;
    return false;
}

[[nodiscard]] bool is_known_msg_type(std::string_view t) noexcept
{
    return is_request_reply(t) || is_fire_and_forget(t);
}

// HEP-CORE-0035 §4.8 — CTRL ROUTER ZAP admission.
//
// `BrokerCtrlAdmission` adapts the broker's `known_roles` operator-
// allowlist to the `PeerAdmission` interface that `ZapRouter` calls
// from the ZAP handler thread.  Allowlist storage is a
// `PortableAtomicSharedPtr<PeerAllowlist>` so:
//   - `is_peer_allowed` reads the current snapshot lock-free
//     (called from the ZAP thread per pump_one)
//   - `set_peer_allowlist` swaps the snapshot atomically
//     (future hot-reload entry point — HEP-CORE-0035 §4.8.5)
//
// Admission is unconditional per HEP-CORE-0035 §2 + §4.6.5 (no-bypass
// discipline): every CURVE-authenticated peer is matched against the
// current snapshot; absent allowlist or empty union is deny-all per
// §4.8.4.
class BrokerCtrlAdmission final : public pylabhub::utils::security::PeerAdmission
{
  public:
    explicit BrokerCtrlAdmission(pylabhub::utils::security::PeerAllowlist initial)
    {
        current_.store(
            std::make_shared<pylabhub::utils::security::PeerAllowlist>(std::move(initial)));
    }

    bool set_peer_allowlist(pylabhub::utils::security::PeerAllowlist allowlist) override
    {
        current_.store(
            std::make_shared<pylabhub::utils::security::PeerAllowlist>(std::move(allowlist)));
        return true;
    }

    [[nodiscard]] std::optional<pylabhub::utils::security::PeerAllowlist>
    peer_allowlist_snapshot() const override
    {
        auto p = current_.load();
        if (!p)
            return std::nullopt;
        return *p;
    }

    [[nodiscard]] bool
    is_peer_allowed(const pylabhub::utils::security::PeerIdentity &peer) const override
    {
        auto p = current_.load();
        if (!p)
            return false;
        return p->contains(peer);
    }

  private:
    pylabhub::utils::detail::PortableAtomicSharedPtr<pylabhub::utils::security::PeerAllowlist>
        current_;
};

// HEP-CORE-0032 §8 — ABI fingerprint verification on REG_REQ /
// CONSUMER_REG_REQ / CONSUMER_ATTACH_REQ ingest.
//
// Slice C shipped log-only.  Slice D (2026-07-03) adds the strict-mode
// reject path: caller passes `strict_mode` from `BrokerService::Config::
// strict_abi_mismatch`; a MAJOR-axis mismatch in strict mode logs
// verdict=`MAJOR_MISMATCH_REJECTED` and populates
// `AbiFingerprintOutcome::reject=true` + `mismatched_axes`.  Caller
// short-circuits with a `status=abi_major_mismatch` response.
//
// Absent envelope during roll-out window (§8.5 default lenient
// policy): verdict = 'ABSENT', accept regardless of strict mode.  A
// future MAJOR bump on broker_proto promotes the field to REQUIRED
// (which will make ABSENT a reject at the wire-shape level, not the
// ABI-fingerprint level).
//
// Malformed envelope: verdict = 'INVALID_ENVELOPE', accept regardless
// of strict mode (wire-shape errors handled elsewhere; §8 fingerprint
// only rejects ABI-major mismatches, not JSON parse failures).
struct AbiFingerprintOutcome
{
    bool reject = false;         ///< strict + MAJOR mismatch → true
    std::string mismatched_axes; ///< comma-separated axes (for error message)
};

static AbiFingerprintOutcome log_peer_abi_fingerprint(const nlohmann::json &req,
                                                      const std::string &role_uid,
                                                      const char *event_prefix,
                                                      const char *event_detail, bool strict_mode,
                                                      const char *transport = nullptr)
{
    // `transport` is non-null for CONSUMER_ATTACH_REQ ingest (values
    // "shm" / "zmq") per HEP-CORE-0032 §8.6.  null → REG_REQ /
    // CONSUMER_REG_REQ; the `transport='...'` slot is omitted from the
    // format string.  (2026-07-03 code review Finding #5.)
    auto trans_slot = [&]() -> std::string
    { return transport != nullptr ? fmt::format(" transport='{}'", transport) : std::string{}; };
    AbiFingerprintOutcome outcome;
    // Absence path — no envelope on the wire (older role, or role
    // built before slice C shipped).  Log verdict='ABSENT' and
    // return; no verify call.  Strict mode does NOT reject on absence
    // — the wire field is optional until a broker_proto MAJOR bump
    // promotes it to required.
    if (!req.contains("abi_fingerprint") || !req["abi_fingerprint"].is_object())
    {
        LOGGER_INFO("[broker] event={} role='{}'{} verdict='ABSENT' mismatched_axes=''",
                    event_prefix, role_uid, trans_slot());
        return outcome;
    }

    // Parse envelope.  Malformed shape (missing axis fields, wrong
    // types) throws std::invalid_argument per §8.7 — surface as
    // verdict='INVALID_ENVELOPE'.  Not a strict-mode reject — that
    // path is reserved for ABI-major mismatches only (§8 fingerprint
    // is about ABI compat, not JSON parse errors).  Wire-shape
    // rejection lives in the surrounding wire-shape validators.
    pylabhub::version::ComponentVersions peer{};
    try
    {
        peer = pylabhub::version::from_json_object(req["abi_fingerprint"]);
    }
    catch (const std::exception &e)
    {
        LOGGER_INFO("[broker] event={} role='{}'{} verdict='INVALID_ENVELOPE' "
                    "mismatched_axes='' error='{}'",
                    event_prefix, role_uid, trans_slot(), e.what());
        return outcome;
    }

    // Optional sibling: build_id.  nullptr → skip build_id comparison.
    const std::string build_id_str = req.value("build_id", std::string{});
    const char *peer_bid = build_id_str.empty() ? nullptr : build_id_str.c_str();

    const auto verdict = pylabhub::version::verify_peer_versions(peer, peer_bid);
    const auto classified = pylabhub::version::classify_peer_verdict(verdict);

    // Map classifier Kind → §8.6 log verdict string, applying
    // strict_mode to the MAJOR case only.  Shared helper used by
    // both broker and role side (2026-07-03 review Finding #14).
    const char *verdict_str = "OK";
    switch (classified.kind)
    {
    case pylabhub::version::AbiPeerVerdict::Kind::Ok:
        verdict_str = "OK";
        break;
    case pylabhub::version::AbiPeerVerdict::Kind::MinorMismatch:
        verdict_str = "MINOR_MISMATCH";
        break;
    case pylabhub::version::AbiPeerVerdict::Kind::BuildOnly:
        verdict_str = "BUILD_ONLY";
        break;
    case pylabhub::version::AbiPeerVerdict::Kind::MajorMismatch:
        verdict_str = strict_mode ? "MAJOR_MISMATCH_REJECTED" : "MAJOR_MISMATCH_ACCEPTED";
        if (strict_mode)
        {
            outcome.reject = true;
            outcome.mismatched_axes = classified.major_axes;
        }
        break;
    }

    // For MINOR_MISMATCH the axis list carried on the log line is the
    // minor-axes list (major-axes are empty by definition).  For OK
    // both are empty.  For MAJOR_MISMATCH_* / BUILD_ONLY use the
    // major-axes list.
    const std::string &axes_for_log =
        (classified.kind == pylabhub::version::AbiPeerVerdict::Kind::MinorMismatch)
            ? classified.minor_axes
            : classified.major_axes;

    LOGGER_INFO("[broker] event={} role='{}'{} verdict='{}' mismatched_axes='{}'", event_prefix,
                role_uid, trans_slot(), verdict_str, axes_for_log);

    // Detail line — §8.6 says emit only when verdict != OK.
    if (std::string_view(verdict_str) != "OK")
    {
        LOGGER_INFO("[broker] event={} role='{}' role_versions='{}' broker_versions='{}'",
                    event_detail, role_uid,
                    fmt::format("lib={}.{}.{},shm={}.{},broker_proto={}.{},"
                                "zmq_frame={}.{},script_api={}.{},"
                                "script_engine={}.{},config={}.{}",
                                peer.library_major, peer.library_minor, peer.library_rolling,
                                peer.shm_major, peer.shm_minor, peer.broker_proto_major,
                                peer.broker_proto_minor, peer.zmq_frame_major, peer.zmq_frame_minor,
                                peer.script_api_major, peer.script_api_minor,
                                peer.script_engine_major, peer.script_engine_minor,
                                peer.config_major, peer.config_minor),
                    pylabhub::version::version_info_string());
    }
    return outcome;
}

} // namespace

// ============================================================================
// BrokerServiceImpl — all private state and logic
// ============================================================================

class BrokerServiceImpl
{
  public:
    ~BrokerServiceImpl()
    {
        // Wave M3 step 5f defensive cleanup: if run() exited abnormally
        // (exception, signal) without reaching its unsubscribe block, the
        // band_left handler still holds `this` by capture.  Remove it
        // here so HubState (which outlives BrokerServiceImpl) cannot
        // fire a lambda into a destroyed object.  Idempotent: no-op if
        // already unsubscribed or never subscribed.
        if (hub_state_ != nullptr && band_left_handler_id_ != pylabhub::hub::kInvalidHandlerId)
        {
            hub_state_->unsubscribe(band_left_handler_id_);
            band_left_handler_id_ = pylabhub::hub::kInvalidHandlerId;
        }
        active_router_ = nullptr;

        // HEP-CORE-0040 §172: hub identity bytes live in the process
        // KeyStore (locked memory).  No per-Impl seckey copy to wipe;
        // KeyStore's dtor handles the zero-on-destruct.
    }

    BrokerService::Config cfg;

    /// HEP-CORE-0035 §4.2 — the single structure that answers "what does
    /// this CURVE key mean to this hub."  Built once from `cfg` when the
    /// broker is configured, then read wherever the operator's roster
    /// has to be projected or a key has to be resolved to a subject.
    /// Before this existed the same roster was re-derived independently
    /// at three call sites; three copies of one identity mapping is a
    /// security defect waiting for the copies to disagree.
    /// The one key->subject index, held as a REPLACEABLE IMMUTABLE SNAPSHOT.
    ///
    /// Readers take `peer_authority()` and hold a `shared_ptr<const>` that
    /// cannot change under them; a roster reload builds a fresh index and
    /// swaps the pointer, and in-flight readers drain on their old snapshot.
    /// Same shape as `BrokerCtrlAdmission`'s allowlist, deliberately: the
    /// allowlist is a PROJECTION of this index, so the two must be able to
    /// move together or a revoked role could still resolve to a valid
    /// principal while ZAP had already begun denying it.
    pylabhub::utils::detail::PortableAtomicSharedPtr<const pylabhub::utils::security::PeerAuthority>
        peer_authority_snapshot;

    /// Current snapshot.  NEVER null — an empty index is published by this
    /// class's constructor before anything can read one, so callers may
    /// dereference without checking.  An empty index resolves nothing and
    /// projects a deny-all allowlist, which is the correct bootstrap state
    /// for a hub with no configured roles (HEP-CORE-0035 §4.8.4) — so the
    /// non-null guarantee costs no safety, it just removes a null window
    /// that previously existed only until config ingestion happened to run.
    [[nodiscard]] std::shared_ptr<const pylabhub::utils::security::PeerAuthority>
    peer_authority() const
    {
        return peer_authority_snapshot.load();
    }

    /// Install a rebuilt index.  The caller builds a complete new index;
    /// there is deliberately no way to edit the published one.
    BrokerServiceImpl()
    {
        // Establish the non-null invariant before any reader exists.
        publish_peer_authority(pylabhub::utils::security::PeerAuthority::Builder{}.build());
    }

    void publish_peer_authority(pylabhub::utils::security::PeerAuthority built)
    {
        peer_authority_snapshot.store(
            std::make_shared<const pylabhub::utils::security::PeerAuthority>(std::move(built)));
    }

    /// Versions the hub-wide roster the roles replicate (HEP-CORE-0035 §4.9).
    ///
    /// One hub-scoped instance beside the per-channel ones on
    /// `ChannelAccessEntry`.  The roster asks less of it than channel
    /// admission does — it never asks the per-role filtered-visibility
    /// question, because every role is entitled to the same list — but the
    /// versioning, the idempotent admit, and revoke are exactly what this
    /// needs, and reimplementing that half untested to avoid carrying the
    /// other half is how one idea becomes two implementations.
    ///
    /// **Keyed on role uids, not keys** (§4.9.4).  What this ledger tracks
    /// is which roles are PRESENT — the registry's half of I-ROSTER-PRESENT.
    /// The key each identity maps to comes from the vault, so a departure
    /// needs nothing looked up: the hub is told which role left, and that
    /// is exactly what this is keyed on.  It starts EMPTY and is moved only
    /// by registration and disconnection; seeding it from the configured
    /// roster would make every configured role present from startup, which
    /// is the state the invariant exists to stop replicating.
    ///
    /// The confirmation map it also maintains has no consumer yet: nothing
    /// in the broker's own decisions waits on a role converging.  It earns
    /// its place when an operator can ask "has this revocation reached
    /// everywhere," which arrives with runtime reload (§4.8.5).
    pylabhub::hub::VersionedAdmissionLedger<std::string, std::string> roster_ledger_;

    /// Serializes `roster_ledger_`.  The ledger is non-thread-safe by
    /// design — its caller owns the lock — and this one has two callers on
    /// possibly different threads: the presence handlers, which fire on
    /// whatever thread ran the HubState mutation (the broker IO thread in
    /// production, a test thread in L2), and `roster_ack_block`, which runs
    /// on the IO thread while building a reply.
    mutable std::mutex roster_mu_;

    /// HubState presence subscriptions that move `roster_ledger_`.
    pylabhub::hub::HandlerId role_registered_handler_id_{pylabhub::hub::kInvalidHandlerId};
    pylabhub::hub::HandlerId role_disconnected_handler_id_{pylabhub::hub::kInvalidHandlerId};

    /// HEP-CORE-0033 §8 state aggregate.  Sole owner of channel / role /
    /// band / peer / shm / counter state; updated only via the broker's
    /// `_on_*` capability ops (friend access).  Per HEP-CORE-0033 §4,
    /// ownership lives in `HubHost`; the broker holds a non-owning
    /// pointer.  In tests, the L3 fixture (`LocalBrokerHandle`) plays
    /// the HubHost role and owns the HubState alongside the broker.
    pylabhub::hub::HubState *hub_state_{nullptr};
    std::atomic<bool> stop_requested{false};

    // #74 — last CHANNEL_COUNT_NOTIFY value broadcast per channel, so the
    // periodic reconciler in check_heartbeat_timeouts re-fans only when the
    // objective live count actually changed.  Router-thread-only (no lock).
    std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> last_channel_counts_;

    /// HEP-CORE-0046 §14 unified receive+validate binder.  Owns the
    /// admission callbacks (known_roles lookup, key-rotation check,
    /// nonce dedup, wall clock) bound against `cfg.known_roles` +
    /// `hub_state_`.  Populated once at BrokerService construction
    /// (see `bind_admission_context`); used by the ROUTER poll loop's
    /// `wire::dispatch::receive_and_validate` call.
    ///
    /// This binder is the SINGLE place identity / replay / known-role /
    /// rotation checks are enforced for incoming REG-family + control
    /// REQs.  No handler re-runs these checks.
    ::pylabhub::wire::dispatch::AdmissionBinder admission_binder_;

    /// HEP-CORE-0041 §D1(d) broker observer pubkey (task #317 C.2.a).
    /// Broker generates a fresh CURVE keypair at construction via
    /// `keystore.generate_and_add_identity("broker.observer")`; the
    /// pubkey is cached here for the REG_ACK emitter to include in
    /// `broker_observer_pubkey_z85`.  Seckey stays in KeyStore
    /// (mlocked, use-not-export) for future observer-dial slices.
    /// Rotated on every broker startup — producers relearn via the
    /// REG_ACK stashing path shipped in #317 D2 (f7d3a51e).
    std::string broker_observer_pubkey_z85;

    /// HEP-CORE-0042 §5.4 wait-path pending queue entry.  One entry per
    /// deferred CONSUMER_ATTACH_REQ_ZMQ that the broker enqueued instead
    /// of replying to synchronously.  All fields are captured under the
    /// broker's single-threaded ROUTER dispatch, so no synchronization
    /// is needed — reads (drain, sweep) happen on the same thread.
    ///
    /// - `router_identity` — ZMQ ROUTER identity of the requesting
    ///   consumer.  Used verbatim as the destination when the deferred
    ///   reply is later sent from `handle_channel_auth_applied_req`
    ///   (drain) or `sweep_pending_attach_timeouts_` (timeout).
    /// - `correlation_id` — echoed back on the deferred reply so the
    ///   consumer-side script API can match reply to the originating
    ///   request.  May be empty (consumer didn't send one).
    /// - `consumer_pubkey`, `consumer_role_uid` — carried for log
    ///   observability of the drain path; not used as decision inputs.
    /// - `target_version` — value of `ChannelAccessEntry.channel_version`
    ///   snapshotted at enqueue time.  The drain condition is
    ///   `confirmed_version[K][P] >= target_version`.  Multiple entries
    ///   with the same (K, P) can have different target_versions because
    ///   channel_version can bump between enqueues; the drain per §5.4
    ///   step d walks the deque in order and drains every entry whose
    ///   target_version has been surpassed by an APPLIED_REQ.
    /// - `enqueued_at` — steady-clock timestamp for the Phase 2.3c
    ///   timeout sweep (`producer_apply_wait_ms` budget, default 3000
    ///   ms per HEP-CORE-0042 §5.5 taxonomy).
    struct PendingAttachEntry
    {
        std::string router_identity;
        std::string correlation_id;
        std::string consumer_pubkey;
        std::string consumer_role_uid;
        std::uint64_t target_version = 0;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    /// HEP-CORE-0042 §5.4 step 5 — pending attach queue.  Owned by the
    /// broker (not HubState) because the queue entry carries the ROUTER
    /// identity + `zmq::socket_t` reply target, both of which are
    /// broker-orchestration concerns rather than pure state.  Keyed by
    /// `channel_name → producer_role_uid → deque<PendingAttachEntry>`
    /// so the two normative drain paths run in constant time:
    ///
    /// - APPLIED_REQ drain (§5.4 step d, Phase 2.3b) — direct lookup by
    ///   (channel, producer_uid), walk the deque, drain entries whose
    ///   `target_version` has been surpassed.
    /// - Producer-disconnect drain (§5.4 producer-not-live, Phase 2.3b)
    ///   — direct lookup by (channel, disconnected_uid), drain everything
    ///   as {status="denied", reason="producer_not_live"}.
    /// - Channel-close drain (§5.4 channel_closing, Phase 2.3b) —
    ///   iterate the outer map's entry for `channel`, drain every
    ///   producer's deque as {status="denied", reason="channel_closing"}.
    /// - Timeout sweep (§5.6 producer_apply_wait_ms, Phase 2.3c) —
    ///   full iteration; drain entries older than the budget.
    ///
    /// Concurrency: mutations happen exclusively on the ROUTER dispatch
    /// thread (single-pumper, HEP-CORE-0036 §7.4).  No external mutex —
    /// the ROUTER pump serialises all writes and reads.
    std::unordered_map<std::string, std::unordered_map<std::string, std::deque<PendingAttachEntry>>>
        pending_attach_queue_;

    /// HEP-CORE-0042 §5.4 step d — pending queue drain on APPLIED_REQ.
    /// Pops from `pending_attach_queue_[channel_name][producer_role_uid]`
    /// every entry whose `target_version <= new_confirmed`, replies
    /// `{status="success", ...}` to each drained entry using its
    /// captured `router_identity` + `correlation_id`, and cleans up
    /// empty inner / outer map entries.  Returns the number drained.
    ///
    /// Called from `handle_channel_auth_applied_req` (Phase 2.3b).
    std::size_t drain_pending_attach_queue_for_producer_confirmed_(
        zmq::socket_t &socket, const std::string &channel_name,
        const std::string &producer_role_uid, std::uint64_t new_confirmed);

    /// HEP-CORE-0042 §5.4 producer-disconnect + channel-close drains.
    /// Pops EVERY entry from `pending_attach_queue_[channel_name][producer_role_uid]`
    /// (producer-not-live) or from `pending_attach_queue_[channel_name][*]`
    /// (channel_closing) and replies `{status="denied", reason=<reason>}`
    /// to each.  Cleans up empty inner / outer map entries.
    ///
    /// Called from:
    /// - `handle_dereg_req` non-last-producer (producer_not_live).
    /// - `check_heartbeat_timeouts` non-last-producer (producer_not_live).
    /// - The three broker teardown paths that call
    ///   `_on_channel_access_closed` (channel_closing).
    std::size_t drain_pending_attach_queue_for_producer_denied_(
        zmq::socket_t &socket, const std::string &channel_name,
        const std::string &producer_role_uid, const std::string &reason);
    std::size_t drain_pending_attach_queue_for_channel_denied_(zmq::socket_t &socket,
                                                               const std::string &channel_name,
                                                               const std::string &reason);

    /// HEP-CORE-0042 §5.4 pending-entry timeout + §5.6
    /// producer_apply_wait_ms sweep.  Walks every (K, P) deque in
    /// `pending_attach_queue_`; pops entries whose age exceeds
    /// `cfg.effective_producer_apply_wait()`; sends
    /// `{status="timeout", reason="producer_did_not_confirm_within_budget"}`.
    ///
    /// Called from `check_heartbeat_timeouts` (Phase 2.3c), piggybacked
    /// on the existing sweep cadence.  Returns total number drained.
    std::size_t sweep_pending_attach_timeouts_(zmq::socket_t &socket,
                                               std::chrono::steady_clock::time_point now,
                                               std::chrono::milliseconds budget);

    /// Serializes the run() thread's post-poll work against external
    /// readers (e.g. `list_channels_json_str()`).  HubState has its own
    /// internal mutex; this one only protects broker-private structures
    /// (request queues, federation session flags) and orders post-poll
    /// drainage with respect to external query callers.
    mutable std::mutex m_query_mu;

    /// Guards close_request_queue_ for thread-safe request_close_channel().
    mutable std::mutex m_close_req_mu;
    /// Script/admin-requested channel closes; drained in run() post-poll phase
    /// under m_query_mu.  Carries the issuing session's `origin_uid` (§11.0.5
    /// provenance stamp) and the command's `request_id` (to tag the §11.0.4
    /// console completion line).  Script/hub-internal closes leave both empty.
    struct CloseRequest
    {
        std::string channel, origin_uid, request_id;
    };
    std::deque<CloseRequest> close_request_queue_;

    /// Wave M3 step 5f (2026-05-11) — handler-driven BAND_LEAVE_NOTIFY
    /// fan-out.  HubState's `_set_role_disconnected` /
    /// `_dispatch_role_disconnected_if_dead` cascade band cleanup and
    /// fire `band_left` handlers; this broker subscribes to those and
    /// emits the wire notification.  Replaces the previous imperative
    /// `band_on_role_closed` calls from channel-close/consumer-close
    /// fanouts (which fired too eagerly for multi-presence roles —
    /// evicted a band member when ONE of its channels closed even if
    /// the role itself was alive elsewhere).
    ///
    /// `active_router_` is set when run() starts (router socket lives
    /// on the run() stack) and cleared on exit.  Handler reads it on
    /// the broker IO thread (same thread that fires HubState
    /// callbacks in production); off-thread firings (L2 tests with
    /// no broker running) see nullptr and no-op.
    zmq::socket_t *active_router_{nullptr};
    pylabhub::hub::HandlerId band_left_handler_id_{pylabhub::hub::kInvalidHandlerId};

    // The CTRL ZAP admission (HEP-CORE-0035 §4.8 + HEP-CORE-0036 §4.2)
    // is wired inside `run()` as locals so its `ZapDomainHandle`
    // unregisters at run-exit (same scope as the ROUTER socket).
    // Storing it as a member would let the registration outlive a
    // hypothetical run() restart and throw on second `register_domain`
    // (per `zap_router.hpp:99`).

    /// Guards broadcast_request_queue_ for thread-safe request_broadcast_channel().
    mutable std::mutex m_broadcast_req_mu;
    /// Admin-shell-requested broadcasts; drained in run() post-poll phase under m_query_mu.
    struct BroadcastRequest
    {
        std::string channel, message, data, origin_uid, request_id;
    };
    std::deque<BroadcastRequest> broadcast_request_queue_;

    // ── Hub federation (HEP-CORE-0022) ─────────────────────────────────────
    // Peer state (uid, state, last_seen, zmq_identity, relay_channels) lives
    // in HubState (HEP-CORE-0033 §8).  The inbound-peer map and the
    // channel→identities reverse index (former [BR5] optimization) are now
    // computed on-the-fly from the snapshot in `relay_notify_to_peers`.

    /// [BR1] Track which hub_uids have already triggered on_hub_connected, to avoid
    /// double-firing in bidirectional federation (each side sends HELLO + receives ACK).
    /// Session flag, not state — kept broker-private.
    std::unordered_set<std::string> hub_connected_notified_;

    /// [BR7] Relay dedup: ordered deque for O(expired) pruning + set for O(1) lookup.
    static constexpr std::chrono::seconds kRelayDedupeWindow{5};
    struct RelayDedupEntry
    {
        std::string msg_id;
        std::chrono::steady_clock::time_point expiry;
    };
    std::deque<RelayDedupEntry> relay_dedup_queue_;   ///< Ordered by expiry (monotone insert)
    std::unordered_set<std::string> relay_dedup_set_; ///< O(1) existence check

    /// Monotone sequence counter for outgoing HUB_RELAY_MSG msg_id.
    uint64_t relay_seq_{0};

    /// Thread-safe queue for send_hub_targeted_msg().
    struct HubTargetedRequest
    {
        std::string target_hub_uid, channel, payload;
    };
    mutable std::mutex m_hub_targeted_mu;
    std::deque<HubTargetedRequest> hub_targeted_queue_;

    // ── Metrics store (HEP-CORE-0019) ──────────────────────────────────────
    // M1.4 (2026-05-11): `metrics_store_`, `ChannelMetrics`,
    // `ParticipantMetrics`, `update_producer_metrics`,
    // `update_consumer_metrics`, `query_metrics(channel)`, and
    // `handle_metrics_report_req` all DELETED.  Metrics now live
    // exclusively on `HubState.roles[uid].presences[(ch, role_type)].latest_metrics`
    // (HEP-CORE-0019 §2.3 Phase 6 "metrics piggyback on heartbeat").
    // Admin queries route through `HubState::channel_metrics_snapshot(channel)`.
    // Closes the H34 leak class (per-uid entries leaking on multi-
    // producer DEREG) structurally — presence rows are erased on
    // disconnect under Wave M3 H18.
    nlohmann::json handle_metrics_req(const nlohmann::json &req);
    nlohmann::json handle_shm_block_query(const nlohmann::json &req) const;
    nlohmann::json collect_shm_info(const std::string &channel) const;

    void run();

    /// HEP-CORE-0046 §14 dispatch entry — visits the ReceivedMessage
    /// variant produced by `wire::dispatch::receive_and_validate`.
    /// Successful variants (Validated<Body>) extract the body JSON,
    /// inject correlation_id for legacy-handler compat, and call the
    /// existing `handle_*` methods.  RejectedMessage variants build
    /// an ERROR envelope via `wire::dispatch::build_error_reply`
    /// and send it to the (already known) sender identity.
    void dispatch_received(zmq::socket_t &socket,
                           ::pylabhub::wire::dispatch::ReceivedMessage &&received);

    void process_message(zmq::socket_t &socket, const zmq::message_t &identity,
                         const std::string &msg_type, const nlohmann::json &payload,
                         std::size_t bytes_in);

    /// HEP-CORE-0033 §9.6: invoke `cfg.on_processing_error` if configured,
    /// catching any user-supplied callback exception (R2).  `identity` may
    /// be nullptr for failures that happen before the routing identity is
    /// known (peer-DEALER inbound, S1/S2 errors).
    void emit_processing_error(const std::string &msg_type, const std::string &error_kind,
                               const std::string &detail, const zmq::message_t *identity);

    nlohmann::json handle_reg_req(const ::pylabhub::wire::WireEnvelope &env,
                                  const ::pylabhub::wire::ProducerRegReqBody &body,
                                  const zmq::message_t &identity, zmq::socket_t &socket);
    /// Always returns a response. The returned JSON's status field indicates
    /// the DISC response variant: "success" (DISC_ACK), "pending" (DISC_PENDING),
    /// or "error" (CHANNEL_NOT_FOUND). See HEP-CORE-0023 §2.2.
    // HEP-CORE-0046 §12 step 5: uniform typed handler signature
    // `handle_XXX(const WireEnvelope&, const XxxBody&)`.  `corr_id` is read from
    // the envelope; `channel_name` from the typed `DiscReqBody`.
    nlohmann::json handle_disc_req(const ::pylabhub::wire::WireEnvelope &env,
                                   const ::pylabhub::wire::DiscReqBody &body);
    nlohmann::json handle_dereg_req(const ::pylabhub::wire::WireEnvelope &env,
                                    const ::pylabhub::wire::DeregReqBody &body,
                                    zmq::socket_t &socket);
    nlohmann::json handle_consumer_reg_req(const ::pylabhub::wire::WireEnvelope &env,
                                           const ::pylabhub::wire::ConsumerRegReqBody &body,
                                           const zmq::message_t &identity, zmq::socket_t &socket);
    nlohmann::json handle_consumer_dereg_req(const ::pylabhub::wire::WireEnvelope &env,
                                             const ::pylabhub::wire::DeregReqBody &body,
                                             zmq::socket_t &socket);
    void handle_heartbeat_req(const ::pylabhub::wire::WireEnvelope &env,
                              const ::pylabhub::wire::HeartbeatNotifyBody &body,
                              zmq::socket_t &socket);

    /// `GET_CHANNEL_AUTH_REQ` handler (HEP-CORE-0036 §6.5).  Returns
    /// the channel's currently-admitted pubkey set (sourced from
    /// `ChannelAccessEntry::ledger` per HEP-CORE-0042 §5.5.2 unified
    /// 2026-07-13) to a producer that asks.  Errors:
    /// `CHANNEL_NOT_FOUND` (channel does not exist),
    /// `PRODUCER_NOT_AUTHORIZED` (caller's role_uid is not a
    /// registered producer of the named channel).
    /// Defence-in-depth: never return another channel's allowlist to a
    /// non-producer caller.
    // HEP-CORE-0046 §12 step 5: uniform typed handler signature.
    // `corr_id` from the envelope; `channel_name` / `role_uid` from the body.
    nlohmann::json handle_get_channel_auth_req(const ::pylabhub::wire::WireEnvelope &env,
                                               const ::pylabhub::wire::GetChannelAuthReqBody &body);

    /// `CONSUMER_ATTACH_REQ_SHM` handler (SHM binding, HEP-CORE-0041
    /// §9 D4 step 4-5 = HEP-CORE-0042 §6.1 Bindings.SHM).  Pre-attach
    /// confirmation: the producer asks the broker whether one specific
    /// consumer is currently authorized for a channel, before sending
    /// the SHM capability fd.  Reply carries either `status="success"`
    /// or `status="denied"` (both are normal auth decisions wrapped in
    /// a CONSUMER_ATTACH_ACK_SHM frame so the producer-side cache
    /// divergence WARN logic can distinguish a clean broker "no" from
    /// a wire error).  Protocol-level failures (shape error, channel not found,
    /// caller not a producer, HubState invariant broken) return the
    /// standard error envelope and are surfaced as ERROR replies by the
    /// dispatcher.  Read-only against HubState — pure query.
    ///
    /// Named `_shm` for symmetry with the sibling
    /// `handle_consumer_attach_req_zmq` under HEP-CORE-0042 §6.2 (both
    /// instantiate the transport-agnostic coordination protocol; SHM
    /// is producer-initiated and stateless, ZMQ is consumer-initiated
    /// and stateful with the `confirmed_version` cache-tracking layer).
    nlohmann::json handle_consumer_attach_req_shm(const nlohmann::json &req);

    /// `CONSUMER_ATTACH_REQ_ZMQ` handler (ZMQ binding, HEP-CORE-0042
    /// §6.2 Bindings.ZMQ + §5.4 handler flow).  Consumer-initiated
    /// pre-attach gate: consumer asks the broker whether producer P's
    /// per-connection ZAP cache is caught up enough to admit consumer
    /// C on channel K.  Fast-path: `confirmed_version[K][P] >=
    /// channel_version[K]` → reply `success` immediately.  Wait-path:
    /// enqueue in `pending_attach_queue_[K][P]`, fire
    /// `CHANNEL_AUTH_CHANGED_NOTIFY` to P, RETURN A SENTINEL
    /// `{status="pending"}` to the dispatcher (which then sends
    /// NOTHING — the reply will be sent later, once
    /// `CHANNEL_AUTH_APPLIED_REQ` from P advances confirmed_version
    /// past the enqueued target_version, or the
    /// `producer_apply_wait_ms` budget elapses).
    ///
    /// **Deferred-reply contract (Phase 2.3a, 2026-07-01).**  Handler
    /// return codes and dispatcher behavior:
    /// - `status="success"|"denied"` → dispatcher sends CONSUMER_ATTACH_ACK_ZMQ.
    /// - `status="timeout"` (future — Phase 2.3c) → dispatcher sends CONSUMER_ATTACH_ACK_ZMQ.
    /// - `status="pending"` (Phase 2.3a) → dispatcher SENDS NOTHING.
    ///   The handler has already enqueued the (identity, correlation_id,
    ///   consumer_pubkey, consumer_role_uid, target_version, enqueued_at)
    ///   tuple into `pending_attach_queue_[channel][producer_uid]`, and
    ///   fired CHANNEL_AUTH_CHANGED_NOTIFY to the producer.  The reply
    ///   will be sent from `handle_channel_auth_applied_req` (Phase 2.3b
    ///   drain path) or `sweep_pending_attach_timeouts_` (Phase 2.3c timeout
    ///   sweep, once wired) using the enqueued router identity.
    /// - `status="internal_error"` → dispatcher sends ERROR.
    ///
    /// Because the handler mutates broker-side state
    /// (`pending_attach_queue_`) and needs the socket to fire NOTIFY +
    /// the ROUTER identity to enqueue the deferred-reply target, the
    /// signature differs from the SHM sibling: it takes both the
    /// socket handle and the requester's ROUTER identity.
    ///
    /// **Impl phasing:**
    /// - Phase 2.1 stub: returns INTERNAL_ERROR.  ✅ SHIPPED (2.1a).
    /// - Phase 2.2 fast-path (§5.4 steps 1-4).  ✅ SHIPPED (2.2).
    /// - Phase 2.3a wait-path enqueue + NOTIFY + deferred-reply
    ///   contract.  ✅ SHIPPED (this commit).
    /// - Phase 2.3b APPLIED_REQ drain (§5.4 step d) + disconnect drain
    ///   (§5.4 producer-disconnect) + channel-close drain.  ⏳ pending.
    /// - Phase 2.3c timeout sweep (§5.6 producer_apply_wait_ms).
    ///   ⏳ pending.
    nlohmann::json handle_consumer_attach_req_zmq(const nlohmann::json &req, zmq::socket_t &socket,
                                                  const std::string &router_identity);

    /// `CHANNEL_AUTH_APPLIED_REQ` handler (HEP-CORE-0042 §5.5.2 +
    /// §5.4).  Producer P confirms it has applied allowlist snapshot
    /// version W and echoes its instance_id.  Broker: if
    /// `instance_id != instance[P]` → silently drop (stale-instance
    /// guard, P4).  Otherwise: reply `{status="ok"}`, advance
    /// `confirmed_version[K][P] = max(current, W)`, drain any pending
    /// CONSUMER_ATTACH_REQ_ZMQ entries whose target_version ≤
    /// confirmed_version.
    ///
    /// Phase 2.1 stub: returns INTERNAL_ERROR with reason
    /// "not_implemented_hep_0042_phase_2_1".  Full impl in Phase 2.2
    /// (advance + drain) + Phase 2.5 (stale-instance guard).
    /// Takes `socket` so the §5.4 step d queue drain (Phase 2.3b) can
    /// send deferred CONSUMER_ATTACH_ACK_ZMQ replies to the ROUTER
    /// identities captured at wait-path enqueue time.
    /// HEP-CORE-0036 §6.6.3 — CHECK_PEER_READY_REQ handler.
    /// Authorization: caller must be a registered role on the
    /// channel (either dialing side or binding side), rejected with
    /// NOT_A_ROLE_OF_CHANNEL otherwise.  Under the operational
    /// semantics the binding-side role never legitimately calls this
    /// wire — the message names the topology mismatch when it does.
    /// Visibility check delegates to
    /// `HubState::is_pubkey_visible_to(chan, binding_role_uid, pk)`
    /// which queries the channel's `VersionedAdmissionLedger`
    /// (HEP-CORE-0042 §5.5.2 unified 2026-07-13
    /// INVARIANT-BIND-CONFIRM-2).
    nlohmann::json handle_check_peer_ready_req(const nlohmann::json &req,
                                               const zmq::message_t &identity);

    nlohmann::json
    handle_channel_auth_applied_req(const ::pylabhub::wire::WireEnvelope &env,
                                    const ::pylabhub::wire::ChannelAuthAppliedReqBody &body,
                                    zmq::socket_t &socket);

    /// Fire-and-forget `CHANNEL_AUTH_CHANGED_NOTIFY` to the BINDING
    /// side of the named channel (HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_
    /// NOTIFY, lines 1803-1864; HEP-CORE-0036 §6.5).  Fan-out target
    /// dispatches by topology: fan-in → consumers; fan-out /
    /// one-to-one → producers.  Payload emits
    /// `{channel_name, channel_version, role_uid, role_type, phase}`
    /// per HEP-0007 lines 1840-1849.  No ACK awaited; the binding
    /// side's response depends on phase:
    ///   - `admitted`/`left` — pull `get_channel_auth` (allowlist changed).
    ///   - `live` — local map update only; no pull.
    /// Peers without a captured ZMQ identity are skipped (no
    /// transport to reach them).  Caller has already mutated the
    /// channel's allowlist via the matching
    /// `_on_consumer_authorized` / `_on_consumer_revoked` HubState op
    /// (for admitted/left; live fires from first-heartbeat detection).
    void fire_channel_auth_changed_notify(zmq::socket_t &socket, const std::string &channel_name,
                                          const std::string &phase, const std::string &role_uid,
                                          const std::string &role_type);

    /// #74 — objective peer counts.  The channel's LIVE
    /// {producer_count, consumer_count} (RoleState kLive = Connected +
    /// first_heartbeat_seen), distinct from the registered ChannelEntry sizes.
    std::pair<std::uint32_t, std::uint32_t>
    compute_channel_live_counts(const std::string &channel) const;

    /// #74 — fan CHANNEL_COUNT_NOTIFY {producer_count, consumer_count} to ALL
    /// members of the channel (both sides) with a captured zmq_identity.  This
    /// is the single count source behind producer_count()/consumer_count() on
    /// every role; the per-peer identity stream stays binding-side only.
    void fire_channel_count_notify(zmq::socket_t &socket, const std::string &channel);

    /// HEP-CORE-0023 §2.5 — heartbeat negotiation block carried in
    /// REG_ACK / CONSUMER_REG_ACK.  Communicates the hub's tolerated
    /// timeout contract so the registering role can validate its own
    /// configured cadence (`role.heartbeat_interval_ms ≤ hub_max`) and
    /// align its periodic-task schedule.  Always populated from
    /// `cfg.heartbeat_*` — no per-channel override.
    nlohmann::json heartbeat_ack_block() const;
    /// The replicated roster block both registration ACKs carry
    /// (HEP-CORE-0035 §4.9): `known_roles` as {uid, pubkey} pairs plus
    /// `known_roles_version`.  Shared for the same reason
    /// `heartbeat_ack_block` is — two ACKs owe the receiver the same block,
    /// and two copies of the builder is how they come to disagree.
    nlohmann::json roster_ack_block() const;

    /// Record that @p uid is now / no longer registered on this hub
    /// (HEP-CORE-0035 §4.9.2).  Both take `roster_mu_`.
    ///
    /// Admission is idempotent and does not advance the version, so a role
    /// registering a second presence leaves every other role's copy current
    /// instead of inviting a re-fetch of a list identical to what it holds.
    void roster_admit_present(const std::string &uid);
    void roster_revoke_absent(const std::string &uid);

    /// Can @p asker_uid reach @p target_uid's inbox right now
    /// (HEP-CORE-0035 §4.9.7, I-INBOX-REACHABLE)?
    enum class Reachability
    {
        Reachable,   ///< The target has confirmed a roster naming the asker.
        NotYet,      ///< The target is behind; send it the roster and retry.
        AskerAbsent, ///< The asker is not registered here.  Permanent.
    };
    [[nodiscard]] Reachability roster_reachability(const std::string &target_uid,
                                                   const std::string &asker_uid) const;

    /// A role reported the roster version it holds (HEP-CORE-0035 §4.9.7).
    /// Answers with `ROSTER_UPDATE_NOTIFY` only when that version is not
    /// ours; silence means "you are current."
    void handle_roster_check_notify(const ::pylabhub::wire::WireEnvelope &env,
                                    const ::pylabhub::wire::RosterCheckNotifyBody &body,
                                    zmq::socket_t &socket);

    nlohmann::json handle_endpoint_update_req(const ::pylabhub::wire::WireEnvelope &env,
                                              const ::pylabhub::wire::EndpointUpdateReqBody &body);
    nlohmann::json handle_schema_req(const nlohmann::json &req);

    /// HEP-CORE-0034 Phase 4b — register hub-global schemas at broker startup.
    /// Walks `cfg.schema_search_dirs` via the stateless free function
    /// `schema::load_all_from_dirs`, translates each parsed entry into a
    /// `(hub, schema_id)` SchemaRecord via `to_hub_schema_record`, and
    /// inserts it into `HubState.schemas` via `_on_schema_registered`.
    /// Path C citations (producer adopts hub-global) become valid once
    /// this completes.  Idempotent on repeated calls; safe to invoke
    /// from `run()` startup.
    /// @see HEP-CORE-0034 §2.4 I1+I2 (single mutator, single load pipeline)
    void load_hub_globals_();

    void check_heartbeat_timeouts(zmq::socket_t &socket);

    // ── Role-close cleanup API (HEP-CORE-0023 §2.5) ───────────────────────
    // Central hooks called at every dereg site so federation/band/future
    // modules can clean up per-role state consistently. Called under m_query_mu
    // from the broker run() thread; `socket` is the broker's ROUTER for
    // emitting implicit notifications (e.g. BAND_LEAVE_NOTIFY to remaining
    // band members).
    void on_channel_closed(zmq::socket_t &socket, const std::string &channel_name,
                           const pylabhub::hub::ChannelEntry &entry, const std::string &reason);
    void on_consumer_closed(zmq::socket_t &socket, const std::string &channel_name,
                            const pylabhub::hub::ConsumerEntry &consumer,
                            const std::string &reason);

    // Per-module cleanup helpers (invoked by the hooks above).
    void federation_on_channel_closed(const std::string &channel_name,
                                      const pylabhub::hub::ChannelEntry &entry,
                                      const std::string &reason);
    /// Wave M3 step 5f (2026-05-11): handler-driven BAND_LEAVE_NOTIFY
    /// fanout helper.  Reads the current member list (HubState already
    /// removed the leaving uid via its band cascade) and sends the
    /// notification to remaining members.  Replaces the prior
    /// `band_on_role_closed` that also mutated HubState — now HubState
    /// owns the state mutation and this helper only fans out the wire
    /// notification.
    void send_band_leave_notify(zmq::socket_t &socket, const std::string &band_name,
                                const std::string &role_uid, const std::string &role_name,
                                const std::string &reason);

    void send_closing_notify(zmq::socket_t &socket, const std::string &channel_name,
                             const pylabhub::hub::ChannelEntry &entry, const std::string &reason);

    void handle_checksum_error_report(zmq::socket_t &socket, const nlohmann::json &req);

    // CHANNEL_NOTIFY_REQ handler removed — audit R3.6 (2026-05-17).

    /// A channel broadcast, once the broker has established who sent it.
    ///
    /// `sender_uid` is not a field of the request and never was one to
    /// trust: recipients read it as fact, so establishing it is the
    /// caller's job — from the key the connection proved, on the wire
    /// path, or from the hub's own identity for a broadcast raised inside
    /// the hub.  Both origins fill this in before the fan-out, which
    /// therefore parses nothing.
    struct ChannelBroadcast
    {
        std::string target_channel;
        std::string message;
        std::string data; ///< optional application payload, forwarded as-is
        std::string sender_uid;
    };

    /// Fan a broadcast out to a channel's producers and consumers, then to
    /// any federation peer subscribed to that channel.
    void handle_channel_broadcast_req(zmq::socket_t &socket, const ChannelBroadcast &bc);

    nlohmann::json handle_channel_list_req(const nlohmann::json &req);

    // ── Band pub/sub (HEP-CORE-0030) ───────────────────────────────────
    nlohmann::json handle_band_join_req(const nlohmann::json &req, const zmq::message_t &identity,
                                        zmq::socket_t &socket);
    nlohmann::json handle_band_leave_req(const nlohmann::json &req, zmq::socket_t &socket);
    void handle_band_broadcast_req(zmq::socket_t &socket, const nlohmann::json &req,
                                   const zmq::message_t &identity);
    nlohmann::json handle_band_members_req(const nlohmann::json &req);

    // Phase 4: role presence + info queries.
    nlohmann::json handle_role_presence_req(const nlohmann::json &req);
    /// Answer "where is this role's inbox".  Also the reachability gate
    /// (HEP-CORE-0035 §4.9.7, I-INBOX-REACHABLE): the coordinates are
    /// withheld until the target has confirmed a roster naming @p asker_uid,
    /// so `socket` is needed to prompt a target that has not.
    ///
    /// @p asker_uid is the caller's ROUTER identity, which on this message
    /// tier is claimed rather than proven — it may decide reachability and
    /// never access.
    nlohmann::json handle_role_info_req(zmq::socket_t &socket, const std::string &asker_uid,
                                        const nlohmann::json &req);

    /// Push an unsolicited message to a specific ZMQ ROUTER identity (raw bytes).
    static void send_to_identity(zmq::socket_t &socket, const std::string &identity,
                                 const std::string &msg_type, const nlohmann::json &body);

    static void send_reply(zmq::socket_t &socket, const zmq::message_t &identity,
                           const std::string &msg_type_ack, const nlohmann::json &body);

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static nlohmann::json make_error(const std::string &correlation_id,
                                     const std::string &error_code, const std::string &message);

    // ── Role state-machine metrics (HEP-CORE-0023 §2.5) ─────────────────
    // Owned by HubState (`BrokerCounters`) per HEP-CORE-0033 §8;
    // accessed via `hub_state_->counters()`.

    // ── Hub federation handlers (HEP-CORE-0022) ───────────────────────────
    void handle_hub_peer_hello(zmq::socket_t &socket, const zmq::message_t &identity,
                               const nlohmann::json &payload);
    void handle_hub_peer_bye(const nlohmann::json &payload);
    void handle_hub_peer_hello_ack(const std::string &peer_hub_uid, const nlohmann::json &payload);
    void handle_hub_relay_msg(zmq::socket_t &socket, const nlohmann::json &payload);
    void handle_hub_targeted_msg(const nlohmann::json &payload);

    /// Relay a CHANNEL_NOTIFY_REQ event to all inbound peers subscribed to channel.
    void relay_notify_to_peers(zmq::socket_t &socket, const std::string &channel,
                               const std::string &event, const std::string &sender_uid,
                               const std::string &data);

    /// Prune expired entries from relay_dedup_.
    void prune_relay_dedup();

    // Legacy `verify_known_role_binding`, `validate_identity_fields`,
    // `validate_role_uid_only` retired (2026-07-14 task #46).  Grammar,
    // known-role binding, and per-msg-type role-tag policy now live in
    // the wire_dispatch pipeline (HEP-CORE-0046 §14.5 gates).
};

// ============================================================================
// BrokerServiceImpl::run() — main event loop
// ============================================================================

void BrokerServiceImpl::run()
{
    // Use the shared process-wide zmq::context_t owned by the ZMQContext
    // lifecycle module.  The hub binary (plh_hub) and every role binary
    // include GetZMQContextModule() in their LifecycleGuard; so do all
    // BrokerService tests.  No per-instance context — matches the pattern
    // used by ZmqQueue, InboxQueue, BrokerRequestComm, and Messenger.
    zmq::context_t &ctx = pylabhub::hub::get_zmq_context();
    zmq::socket_t router(ctx, zmq::socket_type::router);
    // House ZMQ policy — linger, bounded sndtimeo, ZMTP heartbeat.  This line
    // previously set linger alone while citing "§ZMQ socket policy", so the
    // intent was there and the call was not: the broker's ROUTER, which every
    // role in the system connects to, had NO transport-level liveness.
    //
    // The broker's application-level `HEARTBEAT_NOTIFY` + `peer_dead_timeout_ms`
    // answers a DIFFERENT question — "is this role still reporting" — and does
    // not tear down a dead TCP connection or reclaim its per-peer pipe state.
    // ZMTP heartbeat (5s ping / 30s timeout) does, and also produces the
    // ZMQ_EVENT_DISCONNECTED that task #93 needs to enforce disconnect-is-terminal.
    pylabhub::utils::apply_socket_policy(router, pylabhub::utils::ZmqSocketRole::TcpBind);

    // Locals live for the entire run() call — same scope as the
    // ROUTER socket.  Storing the ZAP handle as a member would let
    // the registration outlive run() and throw on a hypothetical
    // restart (`zap_router.hpp:99` — per-domain uniqueness).
    std::unique_ptr<BrokerCtrlAdmission> ctrl_admission;
    std::optional<pylabhub::utils::security::ZapDomainHandle> ctrl_zap_handle;

    // HEP-CORE-0035 §2 + §4.6.5 — CURVE-server and ZAP admission are
    // unconditional.  KeyStore presence of "hub_identity" is enforced
    // at BrokerService ctor (HEP-CORE-0040 §172), so by the time we
    // reach run() the lookup is guaranteed to succeed.  The secret
    // half flows from LockedKey → libzmq inside `with_seckey` scope;
    // no std::string copy of the seckey lives at broker scope.
    namespace sec = pylabhub::utils::security;
    auto &ks = sec::secure().keys();
    // Shared CURVE-server arm (use-not-export) — the same helper the admin
    // console and inbox use; keyed with the hub broker identity.
    pylabhub::utils::arm_curve_server(router, sec::kHubIdentityName);

    // The initial CTRL allowlist is a PROJECTION of the pubkey origin
    // index, not a second derivation of the same roster (HEP-CORE-0035
    // §4.2 — one structure answers "what does this key mean to this
    // hub", and the ZAP layer's view of it comes from that structure).
    // The index already holds the union the allowlist needs: roles that
    // may register, and federation peer hubs that may dial this
    // broker's ROUTER (HEP-CORE-0022).  An empty allowlist is the legal
    // deny-all bootstrap state per HEP-CORE-0035 §4.8.4.
    pylabhub::utils::security::PeerAllowlist initial = peer_authority()->zap_allowlist();
    const auto allowlist_size = initial.peers.size();

    // The ZAP domain MUST be unique per BrokerService instance.  Two
    // brokers in the same process (federation L3 tests, dual-hub
    // processor tests, in-process bring-up smoke tests) registering
    // the same domain would throw on the second `register_domain`
    // (per-domain uniqueness in `zap_router.hpp:99`) → `std::terminate`
    // inside the broker thread.  Use `cfg.self_hub_uid` as a
    // discriminator when present; fall back to a process-counter
    // otherwise so the unnamed-hub case still scales to N>=2.
    std::string zap_domain;
    if (!cfg.self_hub_uid.empty())
    {
        zap_domain = "broker.ctrl." + cfg.self_hub_uid;
    }
    else
    {
        static std::atomic<std::uint64_t> ctrl_zap_seq{0};
        const auto seq = ctrl_zap_seq.fetch_add(1, std::memory_order_relaxed);
        zap_domain = "broker.ctrl." + std::to_string(seq);
    }
    ctrl_admission = std::make_unique<BrokerCtrlAdmission>(std::move(initial));
    router.set(zmq::sockopt::zap_domain, zap_domain);
    ctrl_zap_handle.emplace(pylabhub::utils::security::ZapRouter::instance().register_domain(
        zap_domain, *ctrl_admission));

    LOGGER_INFO("Broker: CTRL ZAP installed enforced on domain '{}' "
                "({} known_roles + {} federation peers = {} allowed)",
                zap_domain, cfg.known_roles.size(), cfg.peers.size(), allowlist_size);

    router.bind(cfg.endpoint);
    const std::string bound = router.get(zmq::sockopt::last_endpoint);
    // Pubkey is non-secret — materialize from KeyStore once for
    // diagnostics + the on_ready callback that publishes it to test
    // harnesses / federation peer config.
    const std::string hub_pubkey_z85(ks.pubkey(sec::kHubIdentityName));
    // Emit the "listening on" log line BEFORE firing the on_ready
    // callback.  Rationale: on_ready is the sync signal that unblocks
    // HubHost's main-thread startup path — which then logs
    // "[HubHost:...] startup complete".  If we log after on_ready, the
    // broker-thread log message races the main-thread one for a spot
    // in the LOGGER queue.  Test PlhHubCliTest.RunMode_LogShowsCorrect
    // StartupAndShutdownOrdering asserts "Broker: listening on"
    // appears BEFORE "startup complete" — an invariant that is only
    // structurally guaranteed when the log line is emitted from THIS
    // thread before the main thread is woken.  Source incident:
    // 2026-07-04 intermittent failure of that test, root-caused via
    // persistent log (test_artifacts/plh_hub_l4/.../logs/) showing
    // main-thread "startup complete" at 932086µs BEFORE broker-thread
    // "listening on" at 932218µs — 132µs race.
    LOGGER_INFO("Broker: listening on {}", bound);
    LOGGER_INFO("Broker: hub identity pubkey = {}", hub_pubkey_z85);
    if (cfg.on_ready)
    {
        cfg.on_ready(bound, hub_pubkey_z85);
    }

    // ── Wave M3 step 5f (2026-05-11): subscribe to band_left ──
    // HubState's terminal cleanup cascades band membership removal
    // and fires this handler.  The broker translates each removal
    // into a BAND_LEAVE_NOTIFY wire message to remaining members.
    //
    // Handlers fire from whatever thread invoked the HubState op.  In
    // production that's this broker IO thread (all wire dispatch +
    // sweep timers run here), so `active_router_` is always valid at
    // handler-fire time.  L2 tests with no broker running see
    // `active_router_ == nullptr` and the handler no-ops.
    active_router_ = &router;
    band_left_handler_id_ = hub_state_->subscribe_band_left(
        [this](const std::string &band, const std::string &uid, const std::string &role_name,
               const std::string &reason)
        {
            if (active_router_ == nullptr)
                return;
            send_band_leave_notify(*active_router_, band, uid, role_name, reason);
        });

    // ── Roster presence tracking (HEP-CORE-0035 §4.9.2, I-ROSTER-PRESENT) ──
    // A replicated roster entry names a role the operator configured AND
    // whose registration this hub currently holds.  These two handlers are
    // the second half: the registry tells the ledger who is here.
    //
    // `role_disc` fires on terminal cleanup only — a role with several
    // presences is not revoked while any of them is still alive — so the
    // pair means exactly "arrived" and "gone", not "one channel opened"
    // and "one channel closed".
    //
    // Nothing is emitted from here.  A version change is picked up by
    // whichever role asks next (§4.9.8): the hub keeps no delivery state
    // and no per-role timers, so a role that died between the change and
    // its next check costs nothing to have missed.
    role_registered_handler_id_ = hub_state_->subscribe_role_registered(
        [this](const pylabhub::hub::RoleEntry &entry) { roster_admit_present(entry.uid); });
    role_disconnected_handler_id_ = hub_state_->subscribe_role_disconnected(
        [this](const std::string &uid) { roster_revoke_absent(uid); });

    // ── Federation: outbound DEALER sockets per peer (HEP-CORE-0022) ────────
    // Stored as unique_ptr so socket handles remain stable in the pollitem_t vector.
    struct OutboundPeer
    {
        const FederationPeer *cfg_entry;
        zmq::socket_t socket;
    };
    std::vector<std::unique_ptr<OutboundPeer>> peer_sockets;

    if (!cfg.self_hub_uid.empty())
    {
        for (const auto &peer_cfg : cfg.peers)
        {
            if (peer_cfg.broker_endpoint.empty())
                continue;
            auto ps = std::make_unique<OutboundPeer>();
            ps->cfg_entry = &peer_cfg;
            ps->socket = zmq::socket_t(ctx, zmq::socket_type::dealer);
            ps->socket.set(zmq::sockopt::linger,
                           0); // policy: always LINGER=0; see §ZMQ socket policy

            // ⚠ NOTE FOR THE FEDERATION DESIGN (task #69) — not a live hole.
            //
            // Federation is UNFINISHED SCAFFOLDING: there is no federation
            // app, no federation config surface, and the hub↔hub contract was
            // never settled.  Nothing an operator can configure today reaches
            // this code, so this is a design note, not an exposure to go fix.
            //
            // What is here: a peer configured without a pubkey is wired PLAIN
            // and connected anyway.  Setting none of the CURVE options leaves libzmq on the
            // NULL mechanism (`options.cpp` selects the mechanism from
            // whichever curve keys were set), so this socket does not fail
            // closed — it connects and sends in the clear.
            //
            // The original rationale: the remote broker's ROUTER requires
            // CURVE, will reject the unauthenticated handshake, and the
            // operator sees `HANDSHAKE_FAILED_*` on the monitor instead of a
            // silent no-op.  Diagnostics over silence is a fair instinct.
            //
            // Why it is still a gap: that argument delegates a security
            // property to a machine we do not control.  It holds only while
            // the far end enforces.  Two ways it does not:
            //   - the peer is older or permissive → both ends talk plaintext
            //     indefinitely, nothing fails, nothing warns;
            //   - the endpoint is wrong (typo, recycled address, hostile
            //     listener) → we connect and begin sending, and having no key
            //     to check against, we cannot tell it is the wrong party.
            //
            // Note also that `FederationPeer::pubkey_z85` and
            // `FederationPeerEntry::pubkey` both DOCUMENT the empty value as
            // "no CURVE", and nothing validates it at config load — so this
            // reads as a supported mode rather than a hole.  That is the same
            // shape as the two backdoors closed in #90/#91: unreachable in
            // practice, documented as legitimate, therefore unwatched.
            //
            // NOT fixed here, and deliberately not "hardened" either: adding
            // a hard failure to scaffolding whose contract does not exist yet
            // just bakes in an assumption the design has not made.  The point
            // of this comment is to stop the shape being carried forward when
            // federation IS designed — a peer with an endpoint and no key
            // cannot federate under any circumstances, so refusing it at
            // config load beats connecting and hoping a monitor is watched.
            // Whatever the design settles on, "trusted peer hub" has to
            // resolve to real entries in `peers` from an authority: the
            // blanket-admit primitive is gone (#91) and is not coming back.
            // See task #69 / HEP-CORE-0037.
            if (!peer_cfg.pubkey_z85.empty())
            {
                // HEP-CORE-0040 §172: same use-not-export pattern as
                // the bind ROUTER above.  Seckey bytes live only in
                // LockedKey region; copied into libzmq's internal CURVE
                // state inside `with_seckey` scope.
                ps->socket.set(zmq::sockopt::curve_serverkey, peer_cfg.pubkey_z85);
                ps->socket.set(zmq::sockopt::curve_publickey, ks.pubkey(sec::kHubIdentityName));
                ks.with_seckey(sec::kHubIdentityName, [&](std::string_view seckey)
                               { ps->socket.set(zmq::sockopt::curve_secretkey, seckey); });
            }

            // Give the DEALER a stable routing-id so the peer can identify us.
            const std::string dealer_id = cfg.self_hub_uid;
            ps->socket.set(zmq::sockopt::routing_id, dealer_id);

            ps->socket.connect(peer_cfg.broker_endpoint);
            LOGGER_INFO("Broker: federation DEALER connected to peer '{}' at {}", peer_cfg.hub_uid,
                        peer_cfg.broker_endpoint);

            // Send HUB_PEER_HELLO immediately after connect.
            // ZMQ queues the message until the connection is established.
            nlohmann::json hello;
            hello["hub_uid"] = cfg.self_hub_uid;
            hello["subscribed_channels"] =
                nlohmann::json::array(); // let peer decide from its config
            hello["protocol_version"] = 1;
            const std::string hello_body = hello.dump();
            ps->socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
            ps->socket.send(zmq::message_t("HUB_PEER_HELLO", 14), zmq::send_flags::sndmore);
            ps->socket.send(zmq::message_t(hello_body.data(), hello_body.size()),
                            zmq::send_flags::none);

            peer_sockets.push_back(std::move(ps));
        }
    }

    // HEP-CORE-0034 Phase 4b — register hub-globals into HubState.schemas.
    // Runs once after federation peers are wired but before the poll loop
    // accepts inbound REG_REQ / CONSUMER_REG_REQ.  This makes path-C
    // citations (producer adopts a hub-global by sending
    // schema_owner="hub") valid immediately when REG_REQ arrives.
    load_hub_globals_();

    while (!stop_requested.load(std::memory_order_acquire))
    {
        // --- Poll phase (no mutex: zmq_poll blocks up to kPollTimeout, no registry access) ---
        std::vector<zmq::pollitem_t> items;
        items.reserve(1 + peer_sockets.size());
        items.push_back({router.handle(), 0, ZMQ_POLLIN, 0});
        for (const auto &ps : peer_sockets)
            items.push_back({ps->socket.handle(), 0, ZMQ_POLLIN, 0});

        zmq::poll(items, kPollTimeout);

        // HEP-CORE-0035 Phase D step D2 — drain any pending ZAP
        // requests on the inproc REP socket.  Non-blocking; returns
        // false when no request is pending or the ZapRouter module
        // is not loaded (test fixtures without CURVE).  When CURVE
        // is on, every connecting peer's CURVE handshake generates
        // exactly one ZAP request that MUST be answered for the
        // handshake to complete — pumping per poll cycle keeps the
        // CTRL acceptance latency at ~kPollTimeout (acceptable for
        // registration; data path latency is unaffected because data
        // sockets pump from their own poll loops).
        (void)pylabhub::utils::security::ZapRouter::instance().pump_one(
            std::chrono::milliseconds{0});

        // --- Post-poll phase (mutex held: all registry reads/writes are protected) ---
        {
            std::lock_guard<std::mutex> lock(m_query_mu);

            // Drain script/admin-requested channel closes.
            std::vector<CloseRequest> pending_closes;
            {
                std::lock_guard<std::mutex> close_lk(m_close_req_mu);
                pending_closes.assign(std::make_move_iterator(close_request_queue_.begin()),
                                      std::make_move_iterator(close_request_queue_.end()));
                close_request_queue_.clear();
            }
            for (const auto &cr : pending_closes)
            {
                const std::string &ch = cr.channel;
                auto hub_entry = hub_state_->channel(ch);
                const bool closed = hub_entry.has_value();
                if (closed)
                {
                    // HEP-CORE-0023 §2.1 atomic teardown: emit a best-effort
                    // CHANNEL_CLOSING_NOTIFY (informational; consumers may
                    // race with the close), then call _on_channel_closed
                    // which marks the producer-presence Disconnected, fires
                    // CHANNEL_CLOSED handlers, and erases the channel entry.
                    // The closing `reason` is an ADVISORY hint delivered to the
                    // consumer's on_channel_closing(channel, reason) callback +
                    // logs; no framework code branches on it (contrast the
                    // attach-retry reasons at role_api_base, which ARE branched
                    // on).  Report it accurately: an operator close carries an
                    // origin_uid, a script close does not.
                    const char *close_reason =
                        cr.origin_uid.empty() ? "script_requested" : "admin_requested";
                    LOGGER_INFO("Broker: requested close for channel '{}' ({})", ch, close_reason);
                    send_closing_notify(router, ch, *hub_entry, close_reason);
                    on_channel_closed(router, ch, *hub_entry, close_reason);
                    hub_state_->_on_channel_closed(ch,
                                                   pylabhub::hub::ChannelCloseReason::AdminClose);
                    // HEP-CORE-0036 §6.5 + HEP-CORE-0042 §5.4: drop the
                    // channel-access record.  Idempotent — safe even if
                    // _on_channel_access_opened was never called.
                    // Symmetric with the VoluntaryDereg last-producer
                    // path in handle_dereg_req and the HeartbeatTimeout
                    // last-producer path in check_heartbeat_timeouts.
                    hub_state_->_on_channel_access_closed(ch);
                    // HEP-CORE-0042 §5.4 channel-close drain (script-
                    // requested close variant).  Reply denied to every
                    // pending ATTACH_REQ_ZMQ so consumers get a bounded
                    // outcome even when the close was operator-initiated.
                    drain_pending_attach_queue_for_channel_denied_(router, ch, "channel_closing");
                }
                // HEP-CORE-0033 §11.0.4: report the completion to the operator
                // console when this close came from an admin command
                // (request_id non-empty).  `channel_absent` = the channel was
                // already gone by actuation time (accepted, then a no-op).
                if (!cr.request_id.empty())
                    hub_state_->append_console_line(
                        cr.request_id,
                        nlohmann::json{{"event", closed ? "channel_closed" : "channel_absent"},
                                       {"channel", ch},
                                       {"origin_uid", cr.origin_uid}});
            }

            // Drain hub-side broadcast requests (originating from
            // BrokerService::request_broadcast_channel — called by
            // HubAPI::broadcast_channel control delegate / AdminService
            // broadcast RPC / future hub-internal callers).
            std::vector<BroadcastRequest> pending_broadcasts;
            {
                std::lock_guard<std::mutex> bcast_lk(m_broadcast_req_mu);
                pending_broadcasts.assign(std::make_move_iterator(broadcast_request_queue_.begin()),
                                          std::make_move_iterator(broadcast_request_queue_.end()));
                broadcast_request_queue_.clear();
            }
            for (const auto &br : pending_broadcasts)
            {
                // §11.0.5 provenance: an operator-triggered broadcast carries
                // the issuing session's `origin_uid` as `sender_uid`; script /
                // hub-internal broadcasts fall back to `self_hub_uid` (or
                // "hub" before identity is wired).  These originate inside the
                // hub, so the hub is the authority on who sent them — the wire
                // path instead derives its sender from the proven key.
                ChannelBroadcast bc;
                bc.target_channel = br.channel;
                bc.message = br.message;
                bc.data = br.data;
                bc.sender_uid =
                    !br.origin_uid.empty()
                        ? br.origin_uid
                        : (cfg.self_hub_uid.empty() ? std::string("hub") : cfg.self_hub_uid);
                handle_channel_broadcast_req(router, bc);
                // HEP-CORE-0033 §11.0.4: report the completion to the operator
                // console when this broadcast came from an admin command.
                if (!br.request_id.empty())
                    hub_state_->append_console_line(
                        br.request_id, nlohmann::json{{"event", "channel_broadcast_sent"},
                                                      {"channel", br.channel},
                                                      {"message", br.message},
                                                      {"origin_uid", br.origin_uid}});
            }

            // Drain hub_targeted_msg queue (thread-safe send from external callers).
            {
                std::vector<HubTargetedRequest> pending_targeted;
                {
                    std::lock_guard<std::mutex> ht_lk(m_hub_targeted_mu);
                    pending_targeted.assign(std::make_move_iterator(hub_targeted_queue_.begin()),
                                            std::make_move_iterator(hub_targeted_queue_.end()));
                    hub_targeted_queue_.clear();
                }
                for (const auto &ht : pending_targeted)
                {
                    auto peer = hub_state_->peer(ht.target_hub_uid);
                    if (!peer.has_value() || peer->state != pylabhub::hub::PeerState::Connected ||
                        peer->zmq_identity.empty())
                    {
                        LOGGER_WARN("Broker: send_hub_targeted_msg: peer '{}' not connected",
                                    ht.target_hub_uid);
                        continue;
                    }
                    nlohmann::json msg;
                    msg["target_hub_uid"] = ht.target_hub_uid;
                    msg["channel_name"] = ht.channel;
                    msg["sender_uid"] = cfg.self_hub_uid;
                    msg["payload"] = ht.payload;
                    send_to_identity(router, peer->zmq_identity, "HUB_TARGETED_MSG", msg);
                    LOGGER_DEBUG("Broker: sent HUB_TARGETED_MSG to peer '{}'", ht.target_hub_uid);
                }
            }

            // Check heartbeat timeouts every poll cycle (~100ms resolution).
            check_heartbeat_timeouts(router);
            prune_relay_dedup();

            // --- Handle ROUTER socket (local clients + inbound peer DEALERs) ---
            //
            // HEP-CORE-0046 §14 unified receive+validate.  One call parses
            // the 5-frame envelope, validates envelope_hash, constructs the
            // typed body for the msg_type, and runs the tier-appropriate
            // admission gates (identity match, replay dedup, known_roles
            // binding, key rotation, grammar) BEFORE any handler runs.
            //
            // The variant `received` encodes success (a `Validated<Body>`
            // per msg_type — guaranteed to have passed every gate its tier
            // requires) or `RejectedMessage` carrying the reject code +
            // correlation_id + identity needed to build the ERROR reply.
            //
            // The visit below dispatches each success case to the
            // corresponding legacy handler (still takes JSON) so this
            // commit only rewires the entry.  Handler-signature migration
            // to typed bodies is per-msg_type follow-on work.
            if ((items[0].revents & ZMQ_POLLIN) != 0)
            {
                zmq::multipart_t raw;
                if (!raw.recv(router))
                {
                    LOGGER_WARN("Broker: multipart recv failed, possible ZMQ "
                                "error or context termination.");
                }
                else
                {
                    // LIVE admission entry point for EVERY inbound control
                    // message.  `receive_and_validate` parses the WireEnvelope
                    // (I-ENVELOPE-BODY-BINDING) and, for REG-family msg_types,
                    // runs the full gate chain via `run_reg_family_gates`
                    // (admission_gates.cpp): identity-match, grammar, role-tag,
                    // known-role/I-PUBKEY-BINDING, key-rotation, and
                    // I-REPLAY-BOUND (nonce dedup + wall_ts skew).  `context`
                    // is wired at broker init (see admission_binder_ finalize
                    // below: record_and_check_nonce → HubState::nonce_seen).
                    // A gate rejection becomes a RejectedMessage that
                    // `dispatch_received` turns into an ERROR reply — the
                    // security invariants are enforced HERE, not in the
                    // per-msg handlers.  See HEP-CORE-0046 "What is LIVE today".
                    auto received = ::pylabhub::wire::dispatch::receive_and_validate(
                        std::move(raw), router, admission_binder_.context);
                    dispatch_received(router, std::move(received));
                }
            }

            // --- Handle outbound peer DEALER sockets (ACKs and relays from peers) ---
            // DEALER receives: ['C', msg_type, json_body]  (3 frames, no identity)
            for (size_t i = 0; i < peer_sockets.size(); ++i)
            {
                if ((items[1 + i].revents & ZMQ_POLLIN) == 0)
                    continue;
                std::vector<zmq::message_t> peer_frames;
                auto np =
                    zmq::recv_multipart(peer_sockets[i]->socket, std::back_inserter(peer_frames));
                if (!np.has_value() || *np < 3)
                    continue;
                try
                {
                    const std::string peer_msg_type = peer_frames[1].to_string();
                    const std::string peer_body_raw = peer_frames[2].to_string();
                    nlohmann::json peer_payload = nlohmann::json::parse(peer_body_raw);
                    if (peer_msg_type == "HUB_PEER_HELLO_ACK")
                    {
                        handle_hub_peer_hello_ack(peer_sockets[i]->cfg_entry->hub_uid,
                                                  peer_payload);
                    }
                    else if (peer_msg_type == "HUB_RELAY_MSG")
                    {
                        handle_hub_relay_msg(router, peer_payload);
                    }
                    else if (peer_msg_type == "HUB_TARGETED_MSG")
                    {
                        handle_hub_targeted_msg(peer_payload);
                    }
                    else
                    {
                        LOGGER_WARN("Broker: unexpected msg_type '{}' from peer DEALER",
                                    peer_msg_type);
                    }
                }
                catch (const nlohmann::json::exception &e)
                {
                    LOGGER_WARN("Broker: malformed JSON from peer DEALER: {}", e.what());
                }
            }
        } // mutex released before next poll
    }

    // Send HUB_PEER_BYE on all outbound sockets before closing.
    if (!cfg.self_hub_uid.empty())
    {
        nlohmann::json bye;
        bye["hub_uid"] = cfg.self_hub_uid;
        const std::string bye_body = bye.dump();
        for (auto &ps : peer_sockets)
        {
            try
            {
                ps->socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
                ps->socket.send(zmq::message_t("HUB_PEER_BYE", 12), zmq::send_flags::sndmore);
                ps->socket.send(zmq::message_t(bye_body.data(), bye_body.size()),
                                zmq::send_flags::none);
            }
            catch (const zmq::error_t &)
            {
            } // best-effort on shutdown
            ps->socket.close();
        }
    }

    // Wave M3 step 5f: unsubscribe before router goes out of scope so
    // no handler fires against a dead socket reference.
    if (band_left_handler_id_ != pylabhub::hub::kInvalidHandlerId)
    {
        hub_state_->unsubscribe(band_left_handler_id_);
        band_left_handler_id_ = pylabhub::hub::kInvalidHandlerId;
    }
    if (role_registered_handler_id_ != pylabhub::hub::kInvalidHandlerId)
    {
        hub_state_->unsubscribe(role_registered_handler_id_);
        role_registered_handler_id_ = pylabhub::hub::kInvalidHandlerId;
    }
    if (role_disconnected_handler_id_ != pylabhub::hub::kInvalidHandlerId)
    {
        hub_state_->unsubscribe(role_disconnected_handler_id_);
        role_disconnected_handler_id_ = pylabhub::hub::kInvalidHandlerId;
    }
    active_router_ = nullptr;

    router.close();
    LOGGER_INFO("Broker: stopped.");
}

// ============================================================================
// Message dispatch
// ============================================================================

void BrokerServiceImpl::dispatch_received(zmq::socket_t &socket,
                                          ::pylabhub::wire::dispatch::ReceivedMessage &&received)
{
    namespace wd = ::pylabhub::wire::dispatch;

    std::visit(
        [&](auto &&v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, wd::RejectedMessage>)
            {
                // Log — the enforcement wins here belong in operator logs.
                const auto code_str = ::pylabhub::admission::to_wire_string(v.code);
                LOGGER_WARN("Broker: admission rejected msg_type='{}' "
                            "identity='{}' corr_id='{}' error_code='{}' "
                            "field='{}' message='{}'",
                            v.msg_type, v.identity, v.correlation_id, code_str, v.field, v.message);
                // C12 resolution — per-code + total counters instead of
                // a single opaque `sys.admission_rejected`.  The total
                // is the aggregate operator monitors track; the
                // per-code map surfaces which gate is firing.
                hub_state_->_bump_counter("sys.admission_rejected_total");
                hub_state_->_bump_counter("sys.admission_rejected_by_code." +
                                          std::string{code_str});
                // Only emit an ERROR envelope when the sender identity is
                // recoverable (envelope parse got past Frame 0).  If the
                // parse failed before that, we cannot address a reply
                // safely and drop silently.
                if (!v.identity.empty())
                {
                    try
                    {
                        auto reply = wd::build_error_reply(v);
                        reply.send(socket);
                    }
                    catch (const std::exception &e)
                    {
                        LOGGER_WARN("Broker: failed to send admission-"
                                    "reject ERROR reply: {}",
                                    e.what());
                    }
                }
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedChannelBroadcastSend>)
            {
                // The sender rides the validated message: admission named it
                // from the key this connection proved, and refused the
                // broadcast outright if it could not.  Nothing here re-reads
                // the body for an identity, because the body no longer
                // carries one.
                ChannelBroadcast bc;
                bc.target_channel = v.body.target_channel();
                bc.message = v.body.message();
                bc.data = v.body.data();
                bc.sender_uid = std::move(v.attributed_sender);
                handle_channel_broadcast_req(socket, bc);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedRawControl>)
            {
                // EnvelopeOnly tier — msg_type known, no typed body yet.
                // Body was validated for envelope_hash only.  Handler still
                // takes JSON.  Injects corr_id for legacy compat.
                const std::string msg_type = v.msg_type();
                nlohmann::json body = std::move(v.body);
                const std::string corr = v.correlation_id();
                if (!corr.empty())
                    body["correlation_id"] = corr;
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                process_message(socket, id_frame, msg_type, body, body.dump().size());
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedRegReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — gates already ran; no to_legacy round-trip.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_reg_req(v.env, v.body, id_frame, socket);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "REG_ACK" : "ERROR";
                if (ack == "REG_ACK")
                {
                    // "REG_ACK sending" marker (Pattern 4 rung 2): channel +
                    // heartbeat cadence + allowlist distinguish an accepted
                    // registration from rejections (which log via ERROR/WARN).
                    int hb_interval_ms = 0;
                    if (resp.contains("heartbeat") && resp["heartbeat"].is_object())
                        hb_interval_ms = resp["heartbeat"].value("heartbeat_interval_ms", 0);
                    LOGGER_INFO("[broker] event=RegAckSending channel='{}' "
                                "heartbeat_interval_ms={} initial_allowlist={}",
                                resp.value("channel_name", "?"), hb_interval_ms,
                                resp.value("initial_allowlist", nlohmann::json::array()).dump());
                }
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedConsumerRegReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — gates already ran; no to_legacy round-trip.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp =
                    handle_consumer_reg_req(v.env, v.body, id_frame, socket);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "CONSUMER_REG_ACK" : "ERROR";
                if (ack == "CONSUMER_REG_ACK")
                {
                    // Pins the HEP-CORE-0036 §6.4 producers[] payload the consumer
                    // role-host needs for its rx-queue CURVE allowlist (empty `[]`
                    // is legal; SHM channels omit the key — HEP-0036 §5.6).
                    const std::string producers_dump =
                        resp.contains("producers") ? resp["producers"].dump() : "[]";
                    LOGGER_INFO("[broker] event=ConsumerRegAckSending channel='{}' "
                                "producers={}",
                                resp.value("channel_name", "?"), producers_dump);
                }
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedDeregReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — the grammar / tag / identity gates already ran
                // in receive_and_validate, so there is no to_legacy round-trip.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_dereg_req(v.env, v.body, socket);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "DEREG_ACK" : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedConsumerDeregReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body (ValidatedConsumerDeregReq carries a DeregReqBody).
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_consumer_dereg_req(v.env, v.body, socket);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "CONSUMER_DEREG_ACK" : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedEndpointUpdateReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — gates already ran; no to_legacy round-trip.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_endpoint_update_req(v.env, v.body);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "ENDPOINT_UPDATE_ACK" : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedChannelAuthAppliedReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — gates already ran; no to_legacy round-trip.
                // NOTE: success status here is "ok" (not "success").
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_channel_auth_applied_req(v.env, v.body, socket);
                const std::string ack =
                    (resp.value("status", "") == "ok") ? "CHANNEL_AUTH_APPLIED_ACK" : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedHeartbeatNotify>)
            {
                // HEP-CORE-0046 §12 step 5: HEARTBEAT_NOTIFY runs the typed
                // handler directly on the validated envelope + body — the
                // grammar / role_uid↔tag / identity gates already ran in
                // `receive_and_validate` (run_control_gates), so there is no
                // `to_legacy` round-trip.  Fire-and-forget (no reply); `socket`
                // is forwarded so the handler can fan out
                // CHANNEL_AUTH_CHANGED_NOTIFY(phase=live) to the binding side on
                // first-heartbeat detection (HEP-CORE-0007 lines 1819-1822).
                handle_heartbeat_req(v.env, v.body, socket);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedRosterCheckNotify>)
            {
                // HEP-CORE-0035 §4.9.7 — fire-and-forget in, and usually
                // nothing out: the handler replies only when the reported
                // version is stale.  `socket` is forwarded so it can send
                // ROSTER_UPDATE_NOTIFY back to this identity when it is.
                handle_roster_check_notify(v.env, v.body, socket);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedGetChannelAuthReq>)
            {
                // HEP-CORE-0046 §12 step 5: typed handler on the validated
                // envelope + body — gates already ran in `receive_and_validate`,
                // so no `to_legacy` round-trip.  Read-only; the reply already
                // ships as a typed `WireEnvelope` via `send_reply`.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_get_channel_auth_req(v.env, v.body);
                const std::string ack =
                    (resp.value("status", "") == "success") ? "GET_CHANNEL_AUTH_ACK" : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
            else if constexpr (std::is_same_v<T, wd::ValidatedDiscReq>)
            {
                // HEP-CORE-0046 §12 step 5: DISC_REQ runs the typed
                // handler directly on the validated envelope + body — the
                // admission gates already ran in `receive_and_validate`, so
                // there is no `to_legacy` round-trip.  Read-only handler
                // (HEP-CORE-0023 §2.2 three-response dispatch); the reply already
                // ships as a typed `WireEnvelope` via `send_reply`.
                const std::string identity = v.identity();
                zmq::message_t id_frame(identity.data(), identity.size());
                const nlohmann::json resp = handle_disc_req(v.env, v.body);
                const std::string status = resp.value("status", "");
                const std::string ack = (status == "success")   ? "DISC_ACK"
                                        : (status == "pending") ? "DISC_PENDING"
                                                                : "ERROR";
                send_reply(socket, id_frame, ack, resp);
            }
        },
        std::move(received));
}

void BrokerServiceImpl::process_message(zmq::socket_t &socket, const zmq::message_t &identity,
                                        const std::string &msg_type, const nlohmann::json &payload,
                                        std::size_t bytes_in)
{
    LOGGER_TRACE("Broker: dispatch {} channel='{}'", msg_type,
                 payload.is_object() ? payload.value("channel_name", std::string{}) // HEP-0036 §5b
                                     : std::string{});

    // HEP-CORE-0033 §9.3 S3: unknown msg_type — bump only sys.unknown_msg_type
    // (R1: do NOT pollute msg_type_counts with attacker-supplied strings),
    // ERROR reply, fire hook, return.
    if (!is_known_msg_type(msg_type))
    {
        LOGGER_WARN("Broker: unknown msg_type '{}'", msg_type);
        const std::string corr_id = payload.value("correlation_id", "");
        try
        {
            send_reply(
                socket, identity, "ERROR",
                make_error(corr_id, "UNKNOWN_MSG_TYPE", "Unknown message type: " + msg_type));
        }
        catch (const std::exception &re)
        {
            LOGGER_WARN("Broker: failed to send UNKNOWN_MSG_TYPE ERROR reply: {}", re.what());
        }
        hub_state_->_bump_counter("sys.unknown_msg_type");
        emit_processing_error(msg_type, "unknown_msg_type", msg_type, &identity);
        return;
    }

    // HEP-CORE-0033 §9.5 exception-safety contract: process_message MUST NOT
    // propagate exceptions to its caller (the recv loop).  Wrap the dispatch
    // chain so any std::exception is logged + counted + (request-reply only)
    // an ERROR reply attempted, and the broker continues to the next message.
    bool errored = false;

    try
    {

        // REG_REQ retired from process_message — it now dispatches typed from
        // `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives as
        // `ValidatedRegReq`, so this branch was unreachable.
        // DISC_REQ retired from process_message — it now dispatches typed from
        // `dispatch_received` (HEP-CORE-0046 §12 step 5).  It always arrives
        // as `ValidatedDiscReq`, so this branch was unreachable.
        // DEREG_REQ retired from process_message — it now dispatches typed from
        // `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives as
        // `ValidatedDeregReq`, so this branch was unreachable.
        // CONSUMER_REG_REQ retired from process_message — it now dispatches typed
        // from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives
        // as `ValidatedConsumerRegReq`, so this branch was unreachable.
        // GET_CHANNEL_AUTH_REQ retired from process_message — it now dispatches
        // typed from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always
        // arrives as `ValidatedGetChannelAuthReq`, so this branch was unreachable.
        if (msg_type == "CONSUMER_ATTACH_REQ_SHM")
        {
            // HEP-CORE-0041 §9 D4 = HEP-CORE-0042 §6.1 Bindings.SHM.  Producer's
            // pre-attach confirmation for SHM transport.  Special-cased dispatch:
            // "denied" is a normal auth decision, NOT a wire error.  Both
            // "success" and "denied" map to CONSUMER_ATTACH_ACK_SHM so the
            // producer's cache-divergence observer (HEP-0041 substep 1e) can
            // distinguish a clean broker "no" from a transport failure.
            nlohmann::json resp = handle_consumer_attach_req_shm(payload);
            const std::string status = resp.value("status", "");
            const std::string ack =
                (status == "success" || status == "denied") ? "CONSUMER_ATTACH_ACK_SHM" : "ERROR";
            send_reply(socket, identity, ack, resp);
        }
        else if (msg_type == "CONSUMER_ATTACH_REQ_ZMQ")
        {
            // HEP-CORE-0042 §6.2 Bindings.ZMQ + §5.4 handler flow.  Consumer-
            // initiated pre-attach gate.  Fast-path replies are sent here;
            // wait-path replies are DEFERRED per the §5.4 wait-path contract.
            //
            // Deferred-reply contract (Phase 2.3a shipped):
            // - status="success" / "denied" / "timeout" → send CONSUMER_ATTACH_ACK_ZMQ.
            // - status="pending" → SEND NOTHING.  The handler has already
            //   enqueued the reply target (router identity + correlation_id)
            //   into pending_attach_queue_.  The reply will be sent later by
            //   handle_channel_auth_applied_req (§5.4 step d drain, Phase 2.3b),
            //   the producer-disconnect / channel-close drain paths (Phase 2.3b),
            //   or sweep_pending_attach_timeouts_ (§5.6 producer_apply_wait_ms, Phase 2.3c).
            // - Anything else → send ERROR.
            // `identity` is the ROUTER identity frame (zmq::message_t); the
            // handler stores it as a string so the deferred-reply path can
            // send back to this specific consumer.
            const std::string identity_str(static_cast<const char *>(identity.data()),
                                           identity.size());
            nlohmann::json resp = handle_consumer_attach_req_zmq(payload, socket, identity_str);
            const std::string status = resp.value("status", "");
            if (status == "pending")
            {
                // §5.4 wait-path: reply is deferred.  Do not send anything here.
                // Log at TRACE level for debug; the enqueue site already
                // LOGGER_INFO'd the full context.
                LOGGER_TRACE("[broker] event=AttachReqZmqDeferred "
                             "channel='{}' producer_uid='{}' (wait-path reply "
                             "will be sent from drain/timeout paths)",
                             resp.value("channel_name", ""), resp.value("producer_role_uid", ""));
            }
            else
            {
                const std::string ack =
                    (status == "success" || status == "denied" || status == "timeout")
                        ? "CONSUMER_ATTACH_ACK_ZMQ"
                        : "ERROR";
                send_reply(socket, identity, ack, resp);
            }
        }
        // CHANNEL_AUTH_APPLIED_REQ retired from process_message — it now dispatches
        // typed from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always
        // arrives as `ValidatedChannelAuthAppliedReq`, so this branch was unreachable.
        else if (msg_type == "CHECK_PEER_READY_REQ")
        {
            // HEP-CORE-0036 §6.6.3.  Dialing-side role pulls its
            // readiness state before initiating the CURVE handshake.
            // Read-only query against
            // `ChannelAccessEntry.ledger` via `is_pubkey_visible_to`
            // (HEP-CORE-0042 §5.5.2 unified 2026-07-13).
            nlohmann::json resp = handle_check_peer_ready_req(payload, identity);
            const std::string status = resp.value("status", "");
            const std::string ack =
                (status == "ready" || status == "not_ready") ? "CHECK_PEER_READY_ACK" : "ERROR";
            send_reply(socket, identity, ack, resp);
        }
        // CONSUMER_DEREG_REQ retired from process_message — it now dispatches typed
        // from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives
        // as `ValidatedConsumerDeregReq`, so this branch was unreachable.
        // HEARTBEAT_NOTIFY retired from process_message — it now dispatches typed
        // from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives
        // as `ValidatedHeartbeatNotify`, so this branch was unreachable.
        else if (msg_type == "CHECKSUM_ERROR_REPORT")
        {
            // Cat 2: producer/consumer reports a slot checksum error.
            // Fire-and-forget: no reply expected.
            handle_checksum_error_report(socket, payload);
        }
        else if (msg_type == "SCHEMA_REQ")
        {
            // HEP-CORE-0016 Phase 3: consumer queries broker for channel schema info.
            nlohmann::json resp = handle_schema_req(payload);
            const std::string ack =
                (resp.value("status", "") == "success") ? "SCHEMA_ACK" : "ERROR";
            send_reply(socket, identity, ack, resp);
        }
        // CHANNEL_NOTIFY_REQ dispatch removed (audit R3.6, 2026-05-17) —
        // the wire path is dead end-to-end: O1 deleted role-side
        // BRC::send_notify (no caller), and federation peer-relay uses
        // HUB_RELAY_MSG (broker↔broker), not CHANNEL_NOTIFY_REQ.  Old
        // clients receive UNKNOWN_MSG_TYPE via the dispatch fall-through.
        // CHANNEL_BROADCAST_SEND_NOTIFY retired from process_message — it now
        // dispatches typed from `dispatch_received`; it always arrives as
        // `ValidatedChannelBroadcastSend`, carrying the sender admission
        // derived from the connection, so this branch was unreachable.  It
        // could not be kept as a fallback either: `payload` here has no
        // sender for the handler to attribute the fan-out to.
        else if (msg_type == "CHANNEL_LIST_REQ")
        {
            // Synchronous: return list of registered channels.
            nlohmann::json resp = handle_channel_list_req(payload);
            LOGGER_TRACE("Broker: CHANNEL_LIST_ACK channels={}",
                         resp.value("channels", nlohmann::json::array()).size());
            send_reply(socket, identity, "CHANNEL_LIST_ACK", resp);
        }
        else if (msg_type == "METRICS_REQ")
        {
            // HEP-CORE-0019: query aggregated metrics.
            nlohmann::json resp = handle_metrics_req(payload);
            send_reply(socket, identity, "METRICS_ACK", resp);
        }
        else if (msg_type == "SHM_BLOCK_QUERY_REQ")
        {
            send_reply(socket, identity, "SHM_BLOCK_QUERY_ACK", handle_shm_block_query(payload));
        }
        else if (msg_type == "ROLE_PRESENCE_REQ")
        {
            // Phase 4: check if a role UID is alive in any channel.
            nlohmann::json resp = handle_role_presence_req(payload);
            send_reply(socket, identity, "ROLE_PRESENCE_ACK", resp);
        }
        else if (msg_type == "ROLE_INFO_REQ")
        {
            // Phase 4: return inbox connection info for a role UID — and,
            // per HEP-CORE-0035 §4.9.7, decide whether this caller may have
            // it yet.  The caller's ROUTER identity is its uid under
            // I-DEALER-IDENTITY; on this tier it is claimed rather than
            // proven, which the handler's contract accounts for.
            const std::string asker_uid(static_cast<const char *>(identity.data()),
                                        identity.size());
            nlohmann::json resp = handle_role_info_req(socket, asker_uid, payload);
            send_reply(socket, identity, "ROLE_INFO_ACK", resp);
        }
        else if (msg_type == "HUB_PEER_HELLO")
        {
            // HEP-CORE-0022: inbound peer DEALER connected and is announcing itself.
            handle_hub_peer_hello(socket, identity, payload);
        }
        else if (msg_type == "HUB_PEER_BYE")
        {
            // HEP-CORE-0022: peer is disconnecting gracefully.
            handle_hub_peer_bye(payload);
        }
        else if (msg_type == "HUB_TARGETED_MSG")
        {
            // HEP-CORE-0022: hub-targeted message from a peer (via peer's DEALER ->our ROUTER).
            handle_hub_targeted_msg(payload);
        }
        // ENDPOINT_UPDATE_REQ retired from process_message — it now dispatches typed
        // from `dispatch_received` (HEP-CORE-0046 §12 step 5); it always arrives
        // as `ValidatedEndpointUpdateReq`, so this branch was unreachable.
        // ── Band pub/sub (HEP-CORE-0030) ───────────────────────────────────
        else if (msg_type == "BAND_JOIN_REQ")
        {
            auto resp = handle_band_join_req(payload, identity, socket);
            send_reply(socket, identity,
                       resp.value("status", "") == "success" ? "BAND_JOIN_ACK" : "ERROR", resp);
        }
        else if (msg_type == "BAND_LEAVE_REQ")
        {
            auto resp = handle_band_leave_req(payload, socket);
            send_reply(socket, identity,
                       resp.value("status", "") == "success" ? "BAND_LEAVE_ACK" : "ERROR", resp);
        }
        else if (msg_type == "BAND_BROADCAST_SEND_NOTIFY")
        {
            handle_band_broadcast_req(socket, payload, identity);
            // fire-and-forget — no reply
        }
        else if (msg_type == "BAND_MEMBERS_REQ")
        {
            auto resp = handle_band_members_req(payload);
            // Audit R3.5: BAND_MEMBERS_REQ now can return INVALID_BAND_NAME.
            // Match the BAND_JOIN/LEAVE dispatch shape — send ERROR when the
            // handler emits a non-success status (validation rejection).
            const std::string ack =
                (resp.value("status", "") == "error") ? "ERROR" : "BAND_MEMBERS_ACK";
            send_reply(socket, identity, ack, resp);
        }
        // No final `else` branch: unknown msg_types are short-circuited at the
        // top of process_message (R1).  If we reach here, msg_type was on the
        // known list but the dispatcher chain did not match — that is a code
        // bug (added to is_known_msg_type but not to dispatch); log loudly.
        else
        {
            LOGGER_ERROR("Broker: msg_type '{}' is classified as known but "
                         "process_message has no dispatch branch — this is a "
                         "broker bug",
                         msg_type);
        }

    } // end try
    catch (const std::exception &e)
    {
        errored = true;
        LOGGER_ERROR("Broker: handler exception for msg_type='{}': {}", msg_type, e.what());
        // R3: ERROR reply only for request-reply types — the protocol
        // shape is fixed per msg_type, not per outcome.  Fire-and-forget
        // clients never expect a reply, even when the server errored.
        if (is_request_reply(msg_type))
        {
            try
            {
                send_reply(
                    socket, identity, "ERROR",
                    make_error(payload.value("correlation_id", ""), "INTERNAL_ERROR", e.what()));
            }
            catch (const std::exception &re)
            {
                // R11: best-effort; swallow.
                LOGGER_WARN("Broker: failed to send INTERNAL_ERROR reply for "
                            "msg_type='{}': {}",
                            msg_type, re.what());
            }
            catch (...)
            {
                // Non-std exception type from a ZMQ send path —
                // unexpected but we still don't want to abort the
                // handler.  Log a generic line so the failure surfaces.
                LOGGER_WARN("Broker: failed to send INTERNAL_ERROR reply "
                            "for msg_type='{}' (non-std exception type)",
                            msg_type);
            }
        }
        hub_state_->_bump_counter("sys.handler_exception");
        emit_processing_error(msg_type, "exception", e.what(), &identity);
    }

    // HEP-CORE-0033 §9.4 wire metric: always bump for known msg_types,
    // regardless of error outcome (counts dispatch-completed messages).
    // bytes_out=0 because multi-target fan-out (broadcast/relay) makes a
    // single per-message accounting ambiguous; per-target byte tracking
    // deferred.
    hub_state_->_on_message_processed(msg_type, bytes_in, /*bytes_out=*/0);
    if (errored)
        hub_state_->_bump_msg_type_error(msg_type);
}

// HEP-CORE-0033 §9.6: invoke the on_processing_error hook (if configured),
// catching any user-supplied callback exception (R2) so it never escapes
// past process_message.
void BrokerServiceImpl::emit_processing_error(const std::string &msg_type,
                                              const std::string &error_kind,
                                              const std::string &detail,
                                              const zmq::message_t *identity)
{
    if (!cfg.on_processing_error)
        return;
    pylabhub::broker::ProcessingError err;
    err.msg_type = msg_type;
    err.error_kind = error_kind;
    err.detail = detail;
    if (identity != nullptr && identity->size() > 0)
        err.peer_identity =
            std::string(static_cast<const char *>(identity->data()), identity->size());
    try
    {
        cfg.on_processing_error(err);
    }
    catch (const std::exception &he)
    {
        LOGGER_ERROR("Broker: on_processing_error callback threw "
                     "(msg_type='{}', kind='{}'): {}",
                     msg_type, error_kind, he.what());
    }
    catch (...)
    {
        LOGGER_ERROR("Broker: on_processing_error callback threw "
                     "(unknown exception type, msg_type='{}', kind='{}')",
                     msg_type, error_kind);
    }
}

// ============================================================================
// Handlers
// ============================================================================

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  Gates (grammar / role_tag / identity / known-role / replay) already
// ran in receive_and_validate; this handler reads via typed accessors and owns
// the admission logic.  In-handler checks that merely repeat a gate are dropped
// (§14.7): grammar/tag, the (role_uid, zmq_pubkey) known-role binding, and the
// zmq_pubkey presence+length (gate_grammar rejects size!=40) all ran upstream.
nlohmann::json BrokerServiceImpl::handle_reg_req(const ::pylabhub::wire::WireEnvelope &env,
                                                 const ::pylabhub::wire::ProducerRegReqBody &body,
                                                 const zmq::message_t &identity,
                                                 zmq::socket_t &socket)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    const std::string role_name = body.role_name();
    const std::string role_uid = body.role_uid();

    // HEP-CORE-0032 §8 — ABI fingerprint verification.  Runs BEFORE any state
    // mutation, so a strict-mode reject short-circuits without leaving broker
    // state dirty.  Strict mode gated on `cfg.strict_abi_mismatch`.  The shared
    // helper reads a JSON probe; feed it the typed abi_fingerprint + build_id.
    {
        nlohmann::json abi_probe;
        abi_probe["abi_fingerprint"] = body.abi_fingerprint();
        if (const std::string bid = body.build_id(); !bid.empty())
            abi_probe["build_id"] = bid;
        if (auto abi = log_peer_abi_fingerprint(abi_probe, role_uid, "AbiFingerprintReceived",
                                                "AbiFingerprintDetail", cfg.strict_abi_mismatch);
            abi.reject)
        {
            return make_error(corr_id, "abi_major_mismatch",
                              "ABI major-axis mismatch on: " + abi.mismatched_axes);
        }
    }

    // Topology wire parse (INVALID_REQUEST for garbage input).  Parsed
    // FIRST — before the schema Path A/B filing and the admission op —
    // because it drives the owner-first gate directly below, and that
    // gate must run BEFORE any state mutation (same parse-first shape
    // as `handle_consumer_reg_req`).  Empty means "no wire
    // declaration" — HubState's atomic admission op treats this as
    // "inherit stored" for an existing channel, or "default to
    // OneToOne" for a fresh channel per HEP-CORE-0018 §5 +
    // HEP-CORE-0017 §4.7.
    std::optional<pylabhub::hub::ChannelTopology> declared_topology;
    {
        const std::string wire = body.channel_topology();
        if (!wire.empty())
        {
            declared_topology = pylabhub::hub::topology::parse(wire);
            if (!declared_topology)
            {
                LOGGER_WARN("[broker] event=RegReqRejected reason='INVALID_REQUEST' "
                            "role='{}' channel='{}' wire_topology='{}' "
                            "detail='not one of fan-in|fan-out|one-to-one'",
                            role_uid, channel_name, wire);
                return make_error(corr_id, "INVALID_REQUEST",
                                  "REG_REQ channel_topology='" + wire +
                                      "' must be 'fan-in', 'fan-out', or 'one-to-one'");
            }
        }
    }

    // §4.7.0.3 arrival classification — owner-first establishment gate
    // (contract C2/C3).  A dialing-side producer arriving before its
    // owner has opened the book is NOT an error — reply the retryable
    // AWAITING_OWNER (immediate; the broker never pends) and the role
    // host's REG retry loop re-attempts within its init budget.  This
    // gate MUST sit here, before the schema Path A/B filing below: a
    // later reject would leave an orphan schema record for a channel
    // that was never opened AND pre-file a schema the owner never
    // chose.  The atomic admission op re-consults the SAME
    // classification under the writer lock (rule 2), closing the
    // snapshot-to-admission race; THIS check keeps the common-case
    // reject side-effect-free.  An absent wire declaration defaults to
    // OneToOne for classification, matching the admission op's
    // fresh-channel default (HEP-CORE-0018 §5).
    if (pylabhub::hub::topology::classify_arrival(
            declared_topology.value_or(pylabhub::hub::ChannelTopology::OneToOne),
            pylabhub::hub::topology::AdmissionSide::Producer,
            hub_state_->channel(channel_name).has_value()) ==
        pylabhub::hub::topology::ArrivalClass::AwaitOwner)
    {
        LOGGER_INFO("[broker] event=RegReqAwaitingOwner role='{}' channel='{}' "
                    "(dialing producer arrived before its owner; retryable)",
                    role_uid, channel_name);
        return make_error(corr_id, "AWAITING_OWNER",
                          "channel '" + channel_name +
                              "' owner (fan-in consumer) not registered yet; retry");
    }

    const std::string attempted_schema = body.schema_hash();
    const uint64_t attempted_pid = body.producer_pid();

    // Producer's CURVE identity pubkey (HEP-CORE-0036 §4.1) — presence + Z85
    // length (==40) already enforced by gate_grammar (§14.5); stored on the
    // producer entry + echoed to consumers via CONSUMER_REG_ACK.producers[].
    const std::string producer_pubkey = body.zmq_pubkey();

    // HEP-CORE-0036 §6.1 Layer-2 identity verification — the wire-
    // claimed (role_uid, zmq_pubkey) pair MUST match a single
    // known_roles.json record.  Layer-1 ZAP already proved the
    // connecting socket holds a known_roles pubkey; this binds the
    // REG_REQ to the specific uid claimed in the body.
    // Known-role (uid, pubkey) binding check already ran at the
    // wire_dispatch pipeline via gate_attested_binding per
    // HEP-CORE-0046 §14.5 gate 5.  Legacy verify_known_role_binding
    // retired (2026-07-14 task #46).

    // Wave M2.5 step 3 — build the new producer entry from the wire
    // payload.  All per-producer attributes (inbox_*, zmq_node_endpoint,
    // zmq_pubkey, metadata) live on `ProducerEntry`; the deprecated
    // channel-scope versions on `ChannelEntry` are no longer written
    // from REG_REQ.  See `docs/tech_draft/controlled_access_api_design.md`
    // §7.5.3 + `REVIEW_WaveM2.5_2026-05-10.md` F6/F7.
    pylabhub::hub::ProducerEntry primary_producer;
    primary_producer.producer_pid = attempted_pid;
    primary_producer.producer_hostname = body.producer_hostname();
    primary_producer.role_name = role_name;
    primary_producer.role_uid = role_uid;
    primary_producer.inbox_endpoint = body.inbox_endpoint();
    primary_producer.inbox_schema_json = body.inbox_schema_json();
    // Packing is carried ONCE, inside the schema object (HEP-0046 B.2 /
    // HEP-0034 §6.2) — the stored `inbox_packing` is derived from the
    // once-parsed spec so ROLE_INFO re-emit keeps its shape.  The
    // separate `inbox_packing` wire field is retired.
    primary_producer.inbox_packing =
        body.has_inbox_schema() ? body.inbox_schema().packing : std::string{};
    primary_producer.inbox_checksum = body.inbox_checksum();
    primary_producer.zmq_node_endpoint = body.zmq_node_endpoint();
    primary_producer.zmq_pubkey = producer_pubkey;
    // HEP-CORE-0041 §5.1 (substep 1g #254) — SHM channels carry the
    // producer's L2 capability-transport endpoint on REG_REQ; broker
    // stores it on the per-producer entry so the CONSUMER_REG_ACK
    // builder can echo it back to authorized consumers (§5.3).
    // Empty for ZMQ channels.
    //
    // HEP-CORE-0041 1i-prod-hardening H3c — for SHM channels, REJECT
    // an empty `shm_capability_endpoint` at the wire.  Without the
    // endpoint string, broker can't echo it to consumers in
    // CONSUMER_REG_ACK, and consumers will fail with a confusing
    // "connect to empty path" error after registration.  Symmetric
    // with the `zmq_pubkey` length enforcement at `gate_grammar`
    // (HEP-CORE-0046 §14.5).
    primary_producer.shm_capability_endpoint = body.shm_capability_endpoint();

    // HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1 — `data_transport` is a
    // REQUIRED string field on REG_REQ, one of {"shm", "zmq"}.  No
    // default — a missing or empty field is a wire-shape contract
    // violation and is rejected at the boundary.
    //
    // Pre-#281 (2026-06-23) the broker silently defaulted absent
    // `data_transport` to `"shm"`, which then tripped the §5.1 endpoint
    // check downstream and produced a confusing diagnostic that pointed
    // at the missing endpoint rather than the actually-missing transport
    // declaration.  Surfacing the malformed REG_REQ explicitly here
    // routes wire bugs to the right diagnostic.
    // `data_transport` is a REQUIRED string on the wire — the ProducerRegReqBody
    // ctor enforces presence + type (→ BODY_SCHEMA_VIOLATION at parse), so the
    // handler validates only the VALUE is one of {"shm","zmq"}.
    const std::string data_transport_req = body.data_transport();
    if (data_transport_req != "shm" && data_transport_req != "zmq")
    {
        LOGGER_WARN("Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
                    "`data_transport`='{}' is not one of {{\"shm\",\"zmq\"}} "
                    "(HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1).",
                    channel_name, role_uid, data_transport_req);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ `data_transport`='" + data_transport_req +
                              "' is invalid; expected 'shm' or 'zmq'");
    }

    // HEP-CORE-0041 §5.1 — SHM channels MUST publish their L2
    // capability transport endpoint so the broker can echo it back to
    // authorized consumers in CONSUMER_REG_ACK (§5.3).  Reject SHM
    // REG_REQ with empty endpoint at the wire — without it, consumers
    // would fail with a confusing "connect to empty path" error after
    // registration.  (`zmq_pubkey` gets the equivalent enforcement at
    // `gate_grammar`, §14.5.)
    if (data_transport_req == "shm" && primary_producer.shm_capability_endpoint.empty())
    {
        LOGGER_WARN("Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
                    "data_transport='shm' but `shm_capability_endpoint` is empty "
                    "(HEP-CORE-0041 §5.1 — SHM channels MUST publish their L2 "
                    "capability endpoint so the broker can echo it to authorized "
                    "consumers in CONSUMER_REG_ACK).",
                    channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ data_transport='shm' requires non-empty "
                          "`shm_capability_endpoint` (HEP-CORE-0041 §5.1)");
    }
    if (body.has_metadata())
    {
        primary_producer.metadata = body.metadata();
    }
    // Producer ZMQ identity: captured here for future unsolicited pushes
    // (CHANNEL_CLOSING_NOTIFY / CHANNEL_ERROR_NOTIFY etc.).
    primary_producer.zmq_identity.assign(static_cast<const char *>(identity.data()),
                                         identity.size());

    // HEP-0021 §16: reject registration if inbox_endpoint has unresolved port 0.
    if (!primary_producer.inbox_endpoint.empty())
    {
        auto inbox_ep = pylabhub::validate_tcp_endpoint(primary_producer.inbox_endpoint);
        if (inbox_ep.ok() && inbox_ep.port == 0)
        {
            LOGGER_WARN("Broker: REG_REQ for '{}' rejected — inbox_endpoint '{}' has port 0",
                        channel_name, primary_producer.inbox_endpoint);
            return make_error(corr_id, "INVALID_INBOX_ENDPOINT",
                              "inbox_endpoint '" + primary_producer.inbox_endpoint +
                                  "' has unresolved port 0");
        }
    }

    // ── Wire schema fields (HEP-CORE-0034 §10.1) ────────────────────────────
    //
    // The producer's wire `schema_blds` is the slot's HEP-0034 canonical
    // form; held here so the schema-mismatch gate below can compare
    // prior vs new BLDS for re-registration.  Schema record creation
    // (path B) and adoption (path C) happen in the dedicated
    // HEP-CORE-0034 block further below.
    const std::string schema_blds_in = body.schema_blds();

    // ── Channel-mismatch early gate (audit fix — must precede
    //    schema-record creation so a failed REG_REQ leaves no orphan
    //    records in HubState.schemas) ─────────────────────────────────
    //
    // HEP-0007 Cat-1 invariant: re-registration of an existing channel
    // with a different schema_hash is rejected.  `_on_producer_added`
    // would also catch this as RejectedMismatch on `schema_hash`, but
    // running the check here lets us short-circuit BEFORE creating
    // schema records (path B/C / inbox), preventing orphans.  The
    // other invariant mismatches (schema_blds / schema_owner /
    // schema_id / transport_*) are caught by
    // `_on_producer_added` after record creation; an orphan record
    // there is a rare anomaly the broker logs as an ERROR.
    //
    // CHANNEL_ERROR_NOTIFY fan-out per HEP-CORE-0007: notify every
    // existing producer so all co-producers on this channel see the
    // schema-mismatch event from the rejected newcomer.
    if (auto existing_opt = hub_state_->channel(channel_name); existing_opt.has_value())
    {
        // Full front-door channel-match through the single validator
        // (HEP-CORE-0034 §9 / §2.4 I4), BEFORE any schema record is created, so
        // a mismatched newcomer leaves no orphan.  This matches the producer's
        // claimed schema_hash + schema_id + schema_owner against the channel's;
        // the producer's structure is self-consistency-checked in the §10.1
        // block below (so a claimed hash that matches the channel but a lying
        // structure is still caught).  `_on_producer_added` re-checks the same
        // invariants at state-write time as a tripwire.  Any mismatch is a
        // SCHEMA_MISMATCH — the same wire code the backstop returns today.
        pylabhub::hub::SchemaCitationInput sin;
        sin.channel_owner = existing_opt->schema_owner;
        sin.channel_id = existing_opt->schema_id;
        sin.channel_hash = fingerprint_for_compare(existing_opt->schema_hash);
        sin.channel_producer_uids.reserve(existing_opt->producers.size());
        for (const auto &p : existing_opt->producers)
            sin.channel_producer_uids.push_back(p.role_uid);
        sin.cited_id = body.schema_id();
        // Effective owner: an empty schema_owner means self-registration.
        const std::string claimed_owner = body.schema_owner();
        sin.cited_owner = claimed_owner.empty() ? role_uid : claimed_owner;
        sin.expected_hash = fingerprint_for_compare(attempted_schema);

        if (const auto vc = hub_state_->_validate_schema_citation(sin); !vc.ok())
        {
            const std::string &existing_schema = existing_opt->schema_hash;
            LOGGER_ERROR("Broker: schema match rejected [Cat1 schema mismatch] "
                         "channel='{}' producer_pid={}: {} (existing_hash={} attempted_hash={})",
                         channel_name, attempted_pid, vc.detail, existing_schema, attempted_schema);
            nlohmann::json err;
            err["channel_name"] = channel_name;
            err["event"] = "schema_mismatch_attempt";
            err["existing_schema_hash"] = existing_schema;
            err["attempted_schema_hash"] = attempted_schema;
            err["attempted_pid"] = attempted_pid;
            for (const auto &prod : existing_opt->producers)
            {
                if (prod.zmq_identity.empty())
                    continue;
                send_to_identity(socket, prod.zmq_identity, "CHANNEL_ERROR_NOTIFY", err);
                LOGGER_ERROR("Broker: CHANNEL_ERROR_NOTIFY to producer of '{}': "
                             "event={}, existing_hash={}, attempted_hash={}, "
                             "attempted_pid={}, target={}",
                             channel_name, "schema_mismatch_attempt", existing_schema,
                             attempted_schema, attempted_pid, prod.role_uid);
            }
            return make_error(corr_id, "SCHEMA_MISMATCH",
                              "Schema does not match existing registration for "
                              "channel '" +
                                  channel_name + "': " + vc.detail);
        }
        // Matches — admission will append to existing producers[] via
        // _on_producer_added; consumers are preserved automatically.
    }

    // ── HEP-CORE-0034 Phase 3+4b — named schema record (paths B + C) ───
    //
    // When the producer claims a named schema (schema_id non-empty), the
    // wire payload MUST carry the full structure: schema_blds (canonical
    // fields per HEP-CORE-0034 §10.1), schema_packing, and schema_hash.
    // The broker recomputes BLAKE2b-256 over the wire form and rejects
    // if the producer's claimed hash doesn't match — Stage-2
    // self-verification.
    //
    // The wire `schema_owner` field selects between two paths:
    //
    //   Path B (default): schema_owner empty or equal to role_uid.
    //     Producer self-registers a new record under (role_uid, schema_id).
    //     Other roles cite (role_uid, schema_id) via path A.  This is the
    //     common case for producer-private schemas.
    //
    //   Path C (Phase 4b): schema_owner == "hub".  Producer adopts a
    //     pre-loaded hub-global record under (hub, schema_id).  The
    //     broker verifies the producer's fingerprint matches the
    //     existing global; no new record is created.  Producer's
    //     channel.schema_owner is set to "hub" so consumer citations
    //     resolve through the global record.
    //
    //   Cross-owner: schema_owner equals some third role's uid.  Rejected
    //     with SCHEMA_FORBIDDEN_OWNER — producers cannot register or
    //     adopt records owned by another producer.
    //
    // Backward compat: REG_REQs without schema_id (truly anonymous /
    // pre-Phase-3 legacy) skip this block entirely.  HEP-0016 library
    // annotation above may have populated `entry.schema_id`; that
    // annotation is informational and does not by itself trigger
    // record creation.
    const std::string req_schema_packing = body.schema_packing();
    const std::string req_schema_id_raw = body.schema_id();
    const std::string req_schema_owner = body.schema_owner();

    // Resolved schema_id / schema_owner for the channel invariants; set
    // by the path B/C blocks below, or left empty for anonymous channels.
    std::string final_schema_id;
    std::string final_schema_owner;

    // An owner claim without a named schema is meaningless — naming an
    // owner says "this schema belongs to X", which needs a schema to
    // point at.  Rejected loudly rather than ignored silently; the
    // consumer twin lives in handle_consumer_reg_req.
    if (req_schema_id_raw.empty() && !req_schema_owner.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ names a schema owner but no schema_id — an "
                          "owner claim needs a schema to point at");
    }

    if (!req_schema_id_raw.empty())
    {
        // These are CONDITIONAL rules: they apply only because this
        // registration cites a schema by name, and only the schema
        // protocol knows they belong together.  That is why they live
        // here in the handler.
        //
        // Contrast `role_uid`, which is an UNCONDITIONAL requirement of
        // every message of this type — and is therefore enforced once,
        // upstream, for all handlers (HEP-CORE-0046 §14.7 rule 3: a
        // handler trusts the shared gates and never re-implements one).
        // By the time this code runs, `role_uid` is guaranteed present
        // and equal to the authenticated sender.  A caller that omits
        // it is rejected at body parse with BODY_SCHEMA_VIOLATION
        // ("wire body: field 'role_uid' missing"); one that sends it
        // empty or wrong is rejected at the identity gate with
        // IDENTITY_MISMATCH.  Both name the field, so the operator
        // diagnostic is at least as good as the handler-level check
        // that used to sit here (`MISSING_ROLE_UID`, removed
        // 2026-07-27 — it was unreachable, and an unreachable copy of
        // a gate drifts from the real one).
        if (req_schema_packing.empty())
            return make_error(corr_id, "MISSING_PACKING",
                              "REG_REQ with schema_id requires schema_packing "
                              "(HEP-CORE-0034 §10.1)");
        if (schema_blds_in.empty())
            return make_error(corr_id, "MISSING_BLDS",
                              "REG_REQ with schema_id requires schema_blds "
                              "(HEP-CORE-0034 §10.1)");
        if (attempted_schema.empty())
            return make_error(corr_id, "MISSING_HASH",
                              "REG_REQ with schema_id requires schema_hash "
                              "(HEP-CORE-0034 §10.1)");

        // Stage-2 fingerprint check (slot + flexzone) — common to both
        // path B and path C.  HEP-CORE-0034 §6.3 / §10.1 — the canonical
        // form covers BOTH the slot and the flexzone when present.
        const std::string req_flexzone_blds = body.flexzone_blds();
        const std::string req_flexzone_packing = body.flexzone_packing();
        // Job A — self-consistency: the producer's structure must hash to its
        // claimed schema_hash.  The shared pre-check is the ONE place a handler
        // recomputes the wire fingerprint (HEP-CORE-0034 §2.4 I4); its computed
        // hash is reused below for the record and any Path-C adoption compare.
        const auto fp_check = pylabhub::hub::verify_request_fingerprint(
            schema_blds_in, req_schema_packing, req_flexzone_blds, req_flexzone_packing,
            attempted_schema);
        if (!fp_check.consistent)
        {
            LOGGER_WARN("Broker: REG_REQ for '{}' rejected — schema_blds + "
                        "schema_packing does not hash to schema_hash "
                        "(HEP-CORE-0034 §6.3 fingerprint inconsistent)",
                        channel_name);
            return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                              "schema_hash does not match BLAKE2b-256 of "
                              "canonical(schema_blds || \"|pack:\" + schema_packing); "
                              "see HEP-CORE-0034 §6.3");
        }
        const auto h_recomputed = fp_check.hash;

        const bool is_path_c = (req_schema_owner == "hub");
        const bool is_path_b = req_schema_owner.empty() || req_schema_owner == role_uid;
        if (!is_path_b && !is_path_c)
        {
            // Cross-owner attempt — third role's uid claimed.
            LOGGER_WARN("Broker: REG_REQ rejected — producer '{}' attempted "
                        "to register under foreign owner '{}'",
                        role_uid, req_schema_owner);
            return make_error(corr_id, "SCHEMA_FORBIDDEN_OWNER",
                              "Producer cannot register schema under owner '" + req_schema_owner +
                                  "' — only \"hub\" "
                                  "(adopt global) or self are permitted");
        }

        if (is_path_c)
        {
            // Path C: adopt an existing hub-global — a named citation of
            // (hub, schema_id).  Validate it through the single validator's
            // named-registry branch (HEP-CORE-0034 §9 step 3 / §2.4 I4): the
            // record must exist and its fingerprint match the producer's.
            pylabhub::hub::SchemaCitationInput sin;
            sin.channel_owner = "hub";
            sin.channel_id = req_schema_id_raw;
            sin.channel_hash = h_recomputed; // computed-from-string
            sin.cited_id = req_schema_id_raw;
            sin.expected_hash = h_recomputed; // channel-match trivial
            sin.check_registry_record = true; // Path C IS a registry citation
            const auto vc = hub_state_->_validate_schema_citation(sin);
            if (!vc.ok())
            {
                // Path C channel owner is "hub" and step-2 is trivially
                // satisfied (cited == channel), so the only reachable step-3
                // outcomes are kUnknownSchema and kFingerprintMismatch; the
                // default is a defensive backstop (e.g. a future cross-citation).
                const char *code = "SCHEMA_MISMATCH";
                switch (vc.reason)
                {
                    using R = ::pylabhub::schema::CitationOutcome::Reason;
                case R::kUnknownSchema:
                    code = "SCHEMA_UNKNOWN"; // no such hub-global to adopt
                    break;
                case R::kFingerprintMismatch:
                    code = "FINGERPRINT_INCONSISTENT"; // exists, fingerprint differs
                    break;
                default:
                    code = "SCHEMA_MISMATCH"; // defensive
                    break;
                }
                LOGGER_WARN("Broker: schema match rejected — channel='{}' "
                            "path=adopt-hub-global code={}: {}",
                            channel_name, code, vc.detail);
                return make_error(corr_id, code,
                                  "Cannot adopt hub-global (hub, " + req_schema_id_raw +
                                      "): " + vc.detail);
            }
            // Adoption succeeds.  Channel is owned by the hub-global,
            // not by the producer.
            final_schema_id = req_schema_id_raw;
            final_schema_owner = "hub";
        }
        else
        {
            // Path B: self-registration.  THE single builder computes the
            // 64-byte `db‖fz` fingerprint from both zones (== h_recomputed).
            auto rec = pylabhub::hub::make_schema_record(role_uid, req_schema_id_raw,
                                                         schema_blds_in, req_schema_packing,
                                                         req_flexzone_blds, req_flexzone_packing);

            using O = pylabhub::schema::SchemaRegOutcome;
            const auto outcome = hub_state_->_on_schema_registered(rec);
            if (outcome == O::kHashMismatchSelf)
            {
                LOGGER_WARN("Broker: REG_REQ schema record mismatch for ({}, {}): "
                            "existing record under producer's namespace has different "
                            "hash or packing",
                            role_uid, req_schema_id_raw);
                return make_error(corr_id, "SCHEMA_HASH_MISMATCH_SELF",
                                  "Schema record (" + role_uid + ", " + req_schema_id_raw +
                                      ") already exists with a different hash or packing");
            }
            if (outcome == O::kForbiddenOwner)
            {
                // Defensive — should not fire here since we set owner ourselves.
                return make_error(corr_id, "SCHEMA_FORBIDDEN_OWNER",
                                  "Schema record (" + role_uid + ", " + req_schema_id_raw +
                                      ") rejected as forbidden_owner — "
                                      "missing required fields");
            }
            // Created or Idempotent → success.
            final_schema_id = req_schema_id_raw;
            final_schema_owner = role_uid;
        }
    }
    else if (!schema_blds_in.empty() || !body.flexzone_blds().empty())
    {
        // Open-row rule (2026-07-26) — ANONYMOUS
        // producer (no schema_id) carrying schema STRUCTURE.  The
        // named path above validates completeness + self-consistency;
        // this path used to skip straight to the invariant fill, so a
        // fresh channel could open with an inconsistent hash/structure
        // pair, or with structure and no hash at all (an unciteable
        // channel — every honest citer recomputes a real fingerprint
        // and is rejected against the empty one).  The open row
        // validates the contract it installs: structure present ⇒
        // packing + hash present and self-consistent.  (Hash WITHOUT
        // structure stays legal — the legacy fingerprint-only
        // registration; the join gate still compares it exactly.)
        if (!schema_blds_in.empty() && req_schema_packing.empty())
            return make_error(corr_id, "MISSING_PACKING",
                              "REG_REQ with schema_blds requires schema_packing "
                              "(HEP-CORE-0034 §6.3)");
        if (attempted_schema.empty())
            return make_error(corr_id, "MISSING_HASH",
                              "REG_REQ with schema structure requires schema_hash — "
                              "the installed contract must carry its fingerprint "
                              "(HEP-CORE-0034 §10.1)");
        const auto fp_check = pylabhub::hub::verify_request_fingerprint(
            schema_blds_in, req_schema_packing, body.flexzone_blds(), body.flexzone_packing(),
            attempted_schema);
        if (!fp_check.consistent)
        {
            LOGGER_WARN("Broker: REG_REQ for '{}' rejected — anonymous schema structure "
                        "does not hash to schema_hash (HEP-CORE-0034 §6.3)",
                        channel_name);
            return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                              "schema_hash does not match BLAKE2b-256 of "
                              "canonical(schema_blds + packing) — anonymous "
                              "registration structure must be self-consistent");
        }
    }

    // ── HEP-CORE-0027 inbox advertisement ──────────────────────────────
    //
    // The inbox schema is a role's *mailbox message layout*, NOT a channel
    // datablock/flexzone schema.  It is stored on the ProducerEntry
    // (`inbox_schema_json`) and advertised to senders as JSON via
    // ROLE_INFO_ACK (HEP-CORE-0027 §4) — it is NOT filed in
    // `HubState.schemas` and is NOT reachable via SCHEMA_REQ.  Shape +
    // packing validation happens ONCE, in the ProducerRegReqBody ctor
    // (HEP-0046 B.2: canonical `hub::parse_schema_json`, malformed →
    // BODY_SCHEMA_VIOLATION at parse) — this handler never re-parses the
    // string.  The receiver still validates its own config when it builds
    // its InboxQueue (`hub_inbox_queue.cpp::validate_inbox_schema`).

    // ── Wave M2.5 step 3: controlled-access admission ───────────────
    //
    // Build channel-wide invariants from the wire payload and call
    // _on_producer_added.  The op appends `primary_producer` to the
    // channel's `producers[]` (or opens a fresh channel if this is the
    // first admission) and returns a typed ProducerAdmissionResult that
    // tells us which wire error code, if any, to surface.
    //
    // The early Cat-1 schema_hash gate above caught the most common
    // re-registration failure mode (with CHANNEL_ERROR_NOTIFY fan-out).
    // Remaining reject modes are: UID_CONFLICT (uid spoof or hub-side
    // residue); the topology gate (TOPOLOGY_MISMATCH,
    // TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT, FAN_OUT_IS_SINGLE_PRODUCER,
    // ONE_TO_ONE_CARDINALITY_VIOLATED) — the latter subsumes the
    // retired MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM check; and the
    // broader invariant-mismatch class (schema_blds, schema_owner,
    // schema_id, data_transport).
    // (HEP-CORE-0036 §5b.4 retired the legacy duplicates
    // has_shared_memory / shm_name / channel_pattern.)
    // A reject in those remaining cases CAN leave an orphan schema record in
    // HubState.schemas (rare anomaly); broker logs ERROR.
    pylabhub::hub::ChannelSchemaInvariants schema_inv;
    schema_inv.schema_hash = attempted_schema;
    // schema_version retired per C2 — version rides inside schema_id.
    schema_inv.schema_id = final_schema_id;
    schema_inv.schema_blds = schema_blds_in;
    schema_inv.schema_owner = final_schema_owner;
    // HEP-CORE-0034 §6.3 two-zone — carry the flexzone content so the channel
    // can return it (SCHEMA_ACK / DISC_ACK).  The flexzone half is already
    // folded into `attempted_schema` (the 128-hex fingerprint).
    schema_inv.flexzone_blds = body.flexzone_blds();

    pylabhub::hub::ChannelTransportInvariants transport_inv;
    // HEP-CORE-0036 §5b.4: `data_transport` is the only canonical
    // transport-classification field on REG_REQ.  Validated above
    // (§6.1 + HEP-CORE-0041 §5.1); reuse the same local so the
    // persisted ChannelEntry.data_transport cannot drift from the
    // value the §5.1 endpoint check accepted.
    transport_inv.data_transport = data_transport_req;

    // `declared_topology` was parsed + validated at the top of this
    // handler (before the schema filing), where it also fed the
    // owner-first AWAITING_OWNER gate.

    // Atomic producer-side admission.  All checks (schema, transport,
    // topology, cardinality, uid conflict) happen under the same
    // writer lock as the mutation.  No TOCTOU window.
    const auto admission = hub_state_->_on_producer_added(
        channel_name, std::move(schema_inv), std::move(transport_inv), declared_topology,
        std::move(primary_producer));

    // Outcome dispatch (order matches HubState's evaluation order).
    if (admission.invalid_identifier)
    {
        // Defensive branch — production wires were already gated by
        // `gate_grammar` in `receive_and_validate` (HEP-CORE-0046 §14.5).
        // Fires only when a bypass path (test-access) violates the
        // precondition.
        LOGGER_WARN("[broker] event=RegReqRejected reason='INVALID_REQUEST' "
                    "role='{}' channel='{}' detail='identifier grammar failure'",
                    role_uid, channel_name);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ rejected — identifier grammar failure "
                          "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (admission.topology_error_code != nullptr)
    {
        LOGGER_WARN("[broker] event=RegReqRejected reason='{}' role='{}' channel='{}'",
                    admission.topology_error_code, role_uid, channel_name);
        return make_error(corr_id, admission.topology_error_code,
                          std::string("REG_REQ rejected — topology gate '") +
                              admission.topology_error_code + "'");
    }
    if (admission.invariant_result == pylabhub::hub::InvariantSetResult::RejectedMismatch)
    {
        const auto &field = admission.mismatched_invariant;
        if (field == "schema_hash" || field == "schema_id" || field == "schema_blds" ||
            field == "schema_owner")
        {
            LOGGER_ERROR("Broker: REG_REQ for '{}' rejected — invariant mismatch on '{}' "
                         "(schema-class).  Anomaly: orphan schema record possible.",
                         channel_name, field);
            return make_error(corr_id, "SCHEMA_MISMATCH",
                              "REG_REQ rejected — schema invariant '" + field +
                                  "' differs from existing channel registration");
        }
        LOGGER_ERROR("Broker: REG_REQ for '{}' rejected — invariant mismatch on '{}' "
                     "(transport-class)",
                     channel_name, field);
        return make_error(corr_id, "TRANSPORT_MISMATCH",
                          "REG_REQ rejected — transport invariant '" + field +
                              "' differs from existing channel registration");
    }
    if (admission.producer_result == pylabhub::hub::AddProducerResult::RejectedUidConflict)
    {
        LOGGER_ERROR("Broker: REG_REQ for '{}' uid='{}' rejected — UID_CONFLICT "
                     "(uid already exists in HubState; possible residue or spoof)",
                     channel_name, role_uid);
        return make_error(corr_id, "UID_CONFLICT",
                          "uid conflict, not trusting this connection, "
                          "try again with clean state");
    }

    // HEP-CORE-0036 §6.5: a freshly-opened channel needs a
    // `ChannelAccessEntry` so subsequent CONSUMER_REG_REQ accepts can
    // populate the allowlist via `_on_consumer_authorized` (no-op
    // without an existing access record per its safe-default
    // invariant).  SHM secret stays zero — SHM auth wiring is tracked
    // separately (HEP-CORE-0036 §12 Phase 5 + task #106).
    if (admission.channel_opened)
    {
        hub_state_->_on_channel_access_opened(channel_name);
    }

    // "REG_REQ accepted" marker (rung 2 of Pattern 4 ladder).
    // Pin: role_uid + channel + producer_pubkey identify the registration
    // uniquely; channel_opened distinguishes first-producer vs subsequent.
    LOGGER_INFO("[broker] event=RegReqAccepted role='{}' channel='{}' producer_pubkey='{}' "
                "schema_hash='{}' (pending first heartbeat){}",
                role_uid, channel_name, producer_pubkey, attempted_schema,
                admission.channel_opened ? " - channel opened" : " - appended to existing");
    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel_name"] = channel_name; // HEP-CORE-0036 §5b.5 canonical
    resp["message"] = "Producer registered successfully";
    resp["heartbeat"] = heartbeat_ack_block(); // HEP-CORE-0023 §2.5
    // HEP-CORE-0032 §8.2 — broker echoes its own ABI envelope on
    // REG_ACK so role-side verification per §8.7 sequence can compare
    // broker versions against role's own local ComponentVersions.
    // Field name `broker_abi_fingerprint` distinguishes from the
    // role-emitted `abi_fingerprint` on REG_REQ.
    resp["broker_abi_fingerprint"] =
        pylabhub::version::to_json_object(pylabhub::version::current());
    if (const char *bid = pylabhub::version::build_id())
    {
        resp["broker_build_id"] = bid;
    }
    // HEP-CORE-0041 §D1(d) — broker observer pubkey (task #317
    // C.2.a).  Producer stashes this via #317 D2 extraction path;
    // consumer of the pubkey is the producer's
    // AttachProtocolAcceptor when C.2.b lights up the observer
    // handshake verify.  Emit even when the observer role_type
    // hasn't shipped end-to-end yet — the producer's stash is
    // idempotent and pre-#317 producers ignore unknown fields.
    // Empty string means keygen failed at broker startup (logged
    // at ERROR); producer treats as "no observer available."
    if (!broker_observer_pubkey_z85.empty())
    {
        resp["broker_observer_pubkey_z85"] = broker_observer_pubkey_z85;
    }

    // HEP-CORE-0036 §5.1 + §6.5: REG_ACK carries the channel's current
    // authorized-consumer allowlist so a producer reconnecting to an
    // already-populated channel observes the live set without waiting
    // for the next `CHANNEL_AUTH_CHANGED_NOTIFY`.  Empty array for a
    // freshly-opened channel.  This is the producer-offline recovery
    // path called out in §6.5 (replaces the retired snapshot-push-with-
    // ACK design).
    // HEP-CORE-0042 §5.5.4 review fix (2026-07-02) — capture the
    // channel snapshot ONCE and reuse for both `initial_allowlist`
    // and `snapshot_version`.  Prior version used two separate
    // channel_access() calls, each acquiring its own shared_lock;
    // the paired snapshot the earlier comment claimed was NOT
    // actually atomic against a concurrent writer.  Today's broker
    // uses a single ROUTER dispatch thread so no concurrent writer
    // exists in practice, but a future federation-relay thread or
    // background sweep would silently corrupt this pairing.  One
    // access grab = one lock scope = truly atomic.
    // HEP-CORE-0036 §6.2 (rev 2.3 2026-07-09) — payload shape
    // unified with CONSUMER_REG_ACK.producers[]: array of
    // `{role_uid?, endpoint?, pubkey_z85}` objects.  Topology-driven
    // size + endpoint semantics per HEP-CORE-0017 §3.3.0:
    //   - Producer BINDING (OneToOne/FanOut): 0..N allowlist entries;
    //     pubkey_z85 required, endpoint may be empty (binding side
    //     doesn't dial), role_uid optional metadata.
    //   - Producer DIALING (FanIn): 1 entry — the consumer's bind
    //     endpoint + CURVE identity pubkey.  (Emission path lands
    //     with the fan-in producer wire glue in Phase G; the
    //     BINDING-side path is what this call site owns today.)
    // Under fan-in the PRODUCER is the DIALING side and the
    // "initial_allowlist" carries a single entry — the consumer's
    // {endpoint, pubkey_z85} — that the producer will use as its
    // dial target and curve_serverkey.  Under fan-out / one-to-one
    // the producer is BINDING and the field carries the list of
    // authorized consumer pubkeys (endpoint empty on each entry).
    nlohmann::json allowlist = nlohmann::json::array();
    std::uint64_t snapshot_version = 0;
    auto ch_snapshot = hub_state_->channel(channel_name);
    const bool producer_is_dialing =
        ch_snapshot.has_value() &&
        !pylabhub::hub::topology::is_owner(ch_snapshot->topology,
                                           pylabhub::hub::topology::AdmissionSide::Producer);
    if (producer_is_dialing)
    {
        if (!ch_snapshot->consumers.empty() && !ch_snapshot->consumers.front().zmq_pubkey.empty() &&
            ch_snapshot->data_endpoint.has_value() && !ch_snapshot->data_endpoint->empty())
        {
            nlohmann::json entry;
            entry["role_uid"] = ch_snapshot->consumers.front().role_uid;
            entry["endpoint"] = *ch_snapshot->data_endpoint;
            entry["pubkey_z85"] = ch_snapshot->consumers.front().zmq_pubkey;
            allowlist.push_back(std::move(entry));
            // Source snapshot_version from the ledger, not the retired
            // `ChannelEntry.channel_version` field (dead pre-2026-07-13,
            // always 0).  Under fan-in the producer is DIALING so it
            // does not own a ZAP allowlist to install; the returned
            // version is still meaningful — it lets the producer
            // correlate the received endpoint/pubkey pair with the
            // channel's admission-state generation.
            if (auto access = hub_state_->channel_access(channel_name); access.has_value())
            {
                snapshot_version = access->ledger.current_version();
            }
        }
        // else: consumer hasn't fully published yet.  Emit empty
        // allowlist; producer's queue stays in Standby until a
        // CHANNEL_AUTH_CHANGED_NOTIFY delivers the update.  The R6
        // pending path (which would delay this REG_ACK entirely
        // until conditions clear) is a separate work item.
    }
    else if (auto access = hub_state_->channel_access(channel_name); access.has_value())
    {
        // for_each_admitted avoids the per-request vector allocation
        // that `admitted_snapshot()` would incur — this is a hot wire
        // path (every producer REG_ACK).
        access->ledger.for_each_admitted(
            [&](const std::string &pk)
            {
                nlohmann::json entry;
                entry["pubkey_z85"] = pk;
                allowlist.push_back(std::move(entry));
            });
        snapshot_version = access->ledger.current_version();
    }
    resp["initial_allowlist"] = std::move(allowlist);

    // HEP-CORE-0027 §3.5 — the roster this role's inbox gate answers from.
    // The inbox is hub-wide role-to-role messaging, so the question it
    // decides is "is the sender a role this hub knows and currently has
    // registered", not "is the sender on my data channel" — which is why
    // the roster travels separately from `initial_allowlist` above.
    //
    // Built here rather than earlier so this role is in its OWN roster:
    // registration is already committed by the time the ACK is assembled,
    // so the ledger has admitted it and the block names it.
    resp.update(roster_ack_block());

    // HEP-CORE-0042 §5.5.3: REG_ACK echoes the broker-assigned
    // `instance_id` for this producer identity so the role can quote
    // it on subsequent CHANNEL_AUTH_APPLIED_REQ.  The broker's
    // stale-instance guard (§5.4 step a) uses the echoed value to
    // reject APPLIED_REQ from a prior crashed instance that a fresh
    // REG has since superseded.  `producer_instance()` returns the
    // counter that `_on_producer_added` (invoked during the REG_REQ
    // admission above) has already bumped.
    resp["instance_id"] = hub_state_->producer_instance(role_uid);

    // HEP-CORE-0042 §5.5.4: REG_ACK echoes `snapshot_version` —
    // the `channel_version[K]` at the same instant as the allowlist
    // snapshot above (single `access` grab; see comment above).
    // Zero for a channel that has never had a consumer REG (fresh-
    // opened channel; §5.2 channel_version[K] undefined == 0).
    // Emit unconditionally so the producer's capture code doesn't
    // have to distinguish absent from zero.
    resp["snapshot_version"] = snapshot_version;

    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }

    // HEP-CORE-0017 §3.3.0 + HEP-CORE-0036 §6.5: under fan-in the
    // producer is the DIALING side; the consumer owns the ZAP
    // allowlist and needs to admit this producer's CURVE pubkey
    // before the producer's PUSH-connect + CURVE handshake can
    // succeed.  Fire the change notify to the (binding-side)
    // consumer so its role host pulls GET_CHANNEL_AUTH_REQ and
    // applies the updated allowlist via set_peer_allowlist.  The
    // helper dispatches by topology (fan-in → consumers) per its
    // own doc-comment at line 774+.
    //
    // Under fan-out / one-to-one this NOTIFY is not fired from
    // the producer-REG path — the producer is the binding side
    // there and receives its allowlist seed via REG_ACK
    // initial_allowlist.  The consumer-side NOTIFY (line 3485)
    // handles admission on those topologies.
    if (auto ch = hub_state_->channel(channel_name);
        ch.has_value() && !pylabhub::hub::topology::is_owner(
                              ch->topology, pylabhub::hub::topology::AdmissionSide::Producer))
    {
        // Admit producer's pubkey into the channel's unified ledger
        // (HEP-CORE-0042 §5.5.2 unified 2026-07-13).  The
        // `_on_consumer_authorized` name is a pre-topology misnomer
        // — the underlying primitive (`ledger.admit`) is topology-
        // agnostic: it holds "pubkeys admitted to the binding side's
        // ZAP allowlist."  Under fan-out / one-to-one those are
        // consumers; under fan-in they are producers.  Advances the
        // ledger's `current_version_` so the consumer's follow-up
        // GET_CHANNEL_AUTH_REQ pulls the new pubkey with an updated
        // `snapshot_version`.
        hub_state_->_on_consumer_authorized(channel_name, producer_pubkey);
        fire_channel_auth_changed_notify(socket, channel_name,
                                         /*phase=*/"admitted",
                                         /*role_uid=*/role_uid,
                                         /*role_type=*/"producer");
    }
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_disc_req(const ::pylabhub::wire::WireEnvelope &env,
                                                  const ::pylabhub::wire::DiscReqBody &body)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    // Take a single snapshot so the channel + producer-presence lookup
    // observe consistent state — channel teardown is atomic with the
    // producer-presence Disconnected transition (HEP-CORE-0023 §2.1).
    const auto snap = hub_state_->snapshot();
    const auto cit = snap.channels.find(channel_name);

    // ── HEP-CORE-0023 §2.2: Three-response state-machine dispatch ──────
    // Broker replies immediately based on the producer-presence state.
    // No queuing.
    if (cit == snap.channels.end())
    {
        // No channel registered.
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> CHANNEL_NOT_FOUND", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }
    const auto &entry_ref = cit->second;

    // Resolve a producer-presence row to base the response on.  Per
    // HEP-CORE-0023 §2.2 + §2.1.1 (multi-producer): the channel is
    // discoverable iff ANY producer-presence is alive — prefer one
    // that is fully Live (Connected + first_heartbeat_seen); fall
    // back to a not-yet-Live presence so we can return DISC_PENDING
    // with a useful reason; only if no presence at all → NOT_FOUND.
    const pylabhub::hub::RolePresence *presence = nullptr;
    const pylabhub::hub::RolePresence *fallback = nullptr;
    for (const auto &prod : entry_ref.producers)
    {
        auto rit = snap.roles.find(prod.role_uid);
        if (rit == snap.roles.end())
            continue;
        const auto *p = rit->second.find_presence(channel_name, "producer");
        if (p == nullptr)
            continue;
        if (p->state == pylabhub::hub::RoleState::Disconnected)
            continue;
        if (p->state == pylabhub::hub::RoleState::Connected && p->first_heartbeat_seen)
        {
            presence = p;
            break;
        }
        if (fallback == nullptr)
            fallback = p;
    }
    if (presence == nullptr)
        presence = fallback;

    if (presence == nullptr)
    {
        // Channel exists but all producer-presences absent / Disconnected
        // (presence rows reaped while atomic teardown is in flight).
        // From the consumer's perspective this channel is not discoverable.
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> CHANNEL_NOT_FOUND "
                     "(no live producer-presence)",
                     channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    if (!presence->first_heartbeat_seen || presence->state == pylabhub::hub::RoleState::Pending)
    {
        // Producer registered but not yet Live — either we're still
        // awaiting the first heartbeat (presence Connected without
        // first_heartbeat_seen), or the heartbeat has stalled (presence
        // Pending).  Client is responsible for retry; reason field
        // distinguishes the two for diagnostics.
        const char *reason =
            !presence->first_heartbeat_seen ? "awaiting_first_heartbeat" : "heartbeat_stalled";
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> DISC_PENDING ({})", channel_name, reason);
        nlohmann::json resp;
        resp["status"] = "pending";
        resp["channel_name"] = channel_name;
        resp["reason"] = reason;
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        return resp;
    }

    // presence->state == Connected with first_heartbeat_seen — channel
    // is Live, fall through to DISC_ACK.

    // Resolve the first producer's per-producer fields for the
    // transitional wire shape (Wave M2.5 step 3 — see
    // controlled_access_api_design.md §7.5.3).  REG_REQ now writes
    // zmq_node_endpoint / zmq_pubkey / metadata to ProducerEntry,
    // not ChannelEntry.  DISC_REQ_ACK returns the FIRST admitted
    // producer's endpoint + pubkey (legacy single-producer wire
    // shape) and the aggregated tree of all producers' metadata
    // blobs (per §6.1).
    // Per-producer arrays surface via CONSUMER_REG_ACK.producers[] per HEP-CORE-0036
    // §3.5 + §6.4 (symmetric Option-α; ENDPOINT_UPDATE_REQ retired 2026-06-12).
    // Producer endpoint comes from REG_REQ.zmq_node_endpoint directly.
    const auto *first_prod = entry_ref.first_producer();

    // HEP-0021 §16: reject if NO producer has a resolved endpoint.
    // Multi-producer channels are reachable iff at least one producer
    // is ready; here we approximate via the first-producer transitional
    // shape (step 5 expands to the full any-ready scan).
    if (entry_ref.data_transport == "zmq" && first_prod != nullptr &&
        !first_prod->zmq_node_endpoint.empty())
    {
        auto ep_check = pylabhub::validate_tcp_endpoint(first_prod->zmq_node_endpoint);
        if (ep_check.ok() && ep_check.port == 0)
        {
            LOGGER_INFO("Broker: DISC_REQ channel '{}' ZMQ endpoint has port 0 (awaiting_endpoint)",
                        channel_name);
            return make_error(corr_id, "CHANNEL_NOT_READY",
                              "ZMQ endpoint for channel '" + channel_name +
                                  "' has unresolved port 0 (awaiting_endpoint)");
        }
    }

    LOGGER_INFO("Broker: discovered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["schema_hash"] = entry_ref.schema_hash;     // 128-hex `db‖fz` fingerprint
    resp["flexzone_blds"] = entry_ref.flexzone_blds; // two-zone (HEP-CORE-0034 §6.3)
    // schema_version retired per C2 — consumers can parse the version
    // from `schema_id` via `pylabhub::hub::parse_schema_id()` if needed.
    // metadata wire shape decided in §6.1: per-producer tree keyed by
    // role_uid (HEP-CORE-0007 §12.4 commit 25dc376).
    resp["metadata"] = entry_ref.aggregate_metadata_tree();
    resp["consumer_count"] = static_cast<uint32_t>(entry_ref.consumers.size());
    // HEP-CORE-0036 §5b.4: `data_transport` is the canonical transport
    // classification; pre-§5b duplicates `shm_name`, `has_shared_memory`,
    // `channel_pattern` retired.
    // zmq_ctrl_endpoint / zmq_data_endpoint retired in Wave M2.5 step 2c.
    // zmq_pubkey + zmq_node_endpoint use first-producer transitional shape
    // until step 5 lifts them to per-producer arrays.
    resp["zmq_pubkey"] = (first_prod != nullptr ? first_prod->zmq_pubkey : std::string{});
    resp["data_transport"] = entry_ref.data_transport;
    resp["zmq_node_endpoint"] =
        (first_prod != nullptr ? first_prod->zmq_node_endpoint : std::string{});
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  Gates (grammar / tag / identity) already ran in receive_and_validate.
nlohmann::json BrokerServiceImpl::handle_dereg_req(const ::pylabhub::wire::WireEnvelope &env,
                                                   const ::pylabhub::wire::DeregReqBody &body,
                                                   zmq::socket_t &socket)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const std::string wire_role_uid = body.role_uid();

    // Target resolution is by `role_uid` ALONE — the authoritative unique
    // producer key (HEP-CORE-0023 §2.1.1 + same-uid-restart-replace: a channel
    // never holds two producer-presences under one role_uid).  A PID is NOT read
    // here — it is debug/record only, machine-local, and never a validation
    // input (HEP-CORE-0023 "A PID is debug/record only").
    //
    // Resolving by uid alone is safe because `receive_and_validate` ran
    // `gate_attested_role_ownership`: the connection PROVED a key the hub
    // resolves to this role_uid.  Note what does NOT establish that — the
    // routing-id-equals-role_uid check is a consistency requirement over
    // two client-chosen values, so on its own it would let any admitted
    // peer name a victim in both places and drop that role's producer.

    // HEP-CORE-0023 §2.1.1 atomic-teardown contract, owner-bound per
    // HEP-CORE-0017 §4.7.0.2 T2: channel teardown fires only when the LAST
    // producer leaves AND the producer side is the binding owner (fan-out /
    // one-to-one).  Fan-in producers are dialers — their leave never tears
    // down.  `_on_producer_dropped` encapsulates this.
    auto entry = hub_state_->channel(channel_name);
    std::string target_role_uid;
    if (entry.has_value())
    {
        for (const auto &prod : entry->producers)
        {
            if (prod.role_uid == wire_role_uid)
            {
                target_role_uid = prod.role_uid;
                break;
            }
        }
    }
    if (!entry.has_value() || target_role_uid.empty())
    {
        LOGGER_WARN("Broker: DEREG_REQ failed for channel '{}' (role_uid='{}')", channel_name,
                    wire_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name +
                              "' not registered or no producer matches role_uid");
    }

    // Capture the channel state (with the to-be-dropped producer
    // still admitted) so we can fan-out CHANNEL_CLOSING_NOTIFY to
    // every party iff this is the LAST producer's leave.  The
    // snapshot is from before the drop; `_on_producer_dropped`
    // tells us whether the channel was torn down or not.
    const pylabhub::hub::ChannelEntry pre_drop = *entry;

    auto drop = hub_state_->_on_producer_dropped(channel_name, target_role_uid,
                                                 pylabhub::hub::ChannelCloseReason::VoluntaryDereg);

    if (!drop.removed)
    {
        // Should not happen — we just resolved a matching producer
        // before the call.  Race condition (presence already reaped
        // between resolve and drop) → treat as NOT_REGISTERED.
        LOGGER_WARN("Broker: DEREG_REQ for '{}' uid='{}' lost the race "
                    "(producer no longer admitted)",
                    channel_name, target_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name + "' producer no longer admitted (race)");
    }

    if (drop.channel_now_empty)
    {
        // Last producer's leave — atomic channel teardown per
        // HEP-CORE-0023 §2.1.1.  Notify consumers + federation peers
        // BEFORE the channel record is gone (we use the captured
        // pre_drop snapshot for the fan-out target list — channels
        // map already has the channel erased by _on_producer_dropped).
        send_closing_notify(socket, channel_name, pre_drop, "producer_deregistered");
        on_channel_closed(socket, channel_name, pre_drop, "producer_deregistered");
        // HEP-CORE-0036 §6.5: drop the channel-access record now that
        // the channel is gone.  Idempotent — safe even if
        // `_on_channel_access_opened` was never called.
        hub_state_->_on_channel_access_closed(channel_name);
        // HEP-CORE-0042 §5.4 channel-close drain — pop every pending
        // ATTACH_REQ_ZMQ across all producers of this channel and reply
        // {status="denied", reason="channel_closing"} to each.
        drain_pending_attach_queue_for_channel_denied_(socket, channel_name, "channel_closing");
        // M1.4 (2026-05-11): no metrics_store_.erase needed — metrics
        // live on per-presence rows which are erased atomically by
        // `_on_channel_closed`'s cascade.
        LOGGER_INFO("Broker: deregistered channel '{}' (last producer left — channel torn down)",
                    channel_name);
    }
    else
    {
        // HEP-CORE-0042 §5.4 producer-disconnect drain — pop every
        // pending ATTACH_REQ_ZMQ for THIS (K, P) and reply
        // {status="denied", reason="producer_not_live"}.  Pairs with the
        // confirmed_version[K][P] erase in
        // HubState::_on_producer_dropped for the non-last-producer path.
        drain_pending_attach_queue_for_producer_denied_(socket, channel_name, target_role_uid,
                                                        "producer_not_live");
        // Multi-producer channel survives: producer X left, the rest
        // continue.  No CHANNEL_CLOSING_NOTIFY (channel is still
        // alive).  Metrics for the channel stay (other producers'
        // metrics still accumulate).
        LOGGER_INFO("Broker: deregistered producer uid='{}' on channel '{}' "
                    "({} producer(s) remain — channel survives)",
                    target_role_uid, channel_name,
                    static_cast<uint32_t>(pre_drop.producer_count() - 1));
    }

    nlohmann::json resp;
    resp["status"] = "success";
    resp["message"] = "Producer deregistered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  Gates (grammar / role_tag {cons,proc} / identity / known-role / replay)
// already ran in receive_and_validate; reads via typed accessors.
nlohmann::json
BrokerServiceImpl::handle_consumer_reg_req(const ::pylabhub::wire::WireEnvelope &env,
                                           const ::pylabhub::wire::ConsumerRegReqBody &body,
                                           const zmq::message_t &identity, zmq::socket_t &socket)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    const std::string role_name = body.role_name();
    const std::string role_uid = body.role_uid();

    // HEP-CORE-0032 §8 — ABI fingerprint verification (same shape as REG_REQ);
    // the shared helper reads a JSON probe built from the typed accessors.
    {
        nlohmann::json abi_probe;
        abi_probe["abi_fingerprint"] = body.abi_fingerprint();
        if (const std::string bid = body.build_id(); !bid.empty())
            abi_probe["build_id"] = bid;
        if (auto abi = log_peer_abi_fingerprint(abi_probe, role_uid, "AbiFingerprintReceived",
                                                "AbiFingerprintDetail", cfg.strict_abi_mismatch);
            abi.reject)
        {
            return make_error(corr_id, "abi_major_mismatch",
                              "ABI major-axis mismatch on: " + abi.mismatched_axes);
        }
    }

    // Topology wire parse (INVALID_REQUEST for garbage input).
    // Parsed FIRST because it drives the channel-existence + producer-
    // Live gates below: under fan-in the consumer is the binding side
    // per HEP-CORE-0017 §3.3.0, so an as-yet-unregistered channel
    // becomes the "consumer opens the channel" path rather than a
    // hard CHANNEL_NOT_FOUND.  Topology mismatch + cardinality checks
    // still happen atomically inside `_on_consumer_joined` below under
    // the writer lock — no TOCTOU window between snapshot and mutation.
    std::optional<pylabhub::hub::ChannelTopology> declared_topology;
    {
        const std::string wire = body.channel_topology();
        if (!wire.empty())
        {
            declared_topology = pylabhub::hub::topology::parse(wire);
            if (!declared_topology)
            {
                LOGGER_WARN("[broker] event=ConsumerRegReqRejected reason='INVALID_REQUEST' "
                            "role='{}' channel='{}' wire_topology='{}' "
                            "detail='not one of fan-in|fan-out|one-to-one'",
                            role_uid, channel_name, wire);
                return make_error(corr_id, "INVALID_REQUEST",
                                  "CONSUMER_REG_REQ channel_topology='" + wire +
                                      "' must be 'fan-in', 'fan-out', or 'one-to-one'");
            }
        }
    }

    // Single HubState snapshot covers channel-existence, endpoint
    // freshness, R6 producer-readiness gate, and downstream schema
    // checks — matches DISC_REQ's pattern at line ~1909 and avoids
    // any drift between separate snapshot calls.
    const auto snap = hub_state_->snapshot();
    const auto cit = snap.channels.find(channel_name);

    // §4.7.0.3 arrival classification for the consumer side (same
    // table as `handle_reg_req` — the two handlers encode NOTHING
    // locally).  Owner-opens routes to the open-path in
    // `_on_consumer_joined` (which atomically creates the
    // ChannelEntry with the consumer as the binding owner + first
    // admitted role); await-owner replies the retryable
    // AWAITING_OWNER.  An absent wire declaration cannot classify the
    // consumer as an opener — a fresh channel needs the owner's
    // declared topology — so empty + missing-book lands on
    // await-owner, the safe default for a dialer.
    const auto consumer_arrival = pylabhub::hub::topology::classify_arrival(
        declared_topology.value_or(pylabhub::hub::ChannelTopology::OneToOne),
        pylabhub::hub::topology::AdmissionSide::Consumer,
        /*book_exists=*/cit != snap.channels.end());
    const bool consumer_will_open_channel =
        declared_topology.has_value() &&
        consumer_arrival == pylabhub::hub::topology::ArrivalClass::OwnerOpens;

    if (cit == snap.channels.end() && !consumer_will_open_channel)
    {
        // §4.7.0.3 await-owner row (contract C2/C3): the DIALING
        // consumer arrived before the producer-owner opened the book —
        // NOT a caller error.  Reply the retryable AWAITING_OWNER
        // (immediate; the broker never pends); the role host's REG
        // retry loop re-attempts within its init budget.  Accepted C3
        // trade-off: a genuinely wrong channel name also lands here
        // and fails only after the dialer's retry budget — at REG
        // time "not yet" and "never" are indistinguishable.
        LOGGER_INFO("[broker] event=ConsumerRegReqAwaitingOwner role='{}' channel='{}' "
                    "(dialing consumer arrived before owner; retryable)",
                    role_uid, channel_name);
        return make_error(corr_id, "AWAITING_OWNER",
                          "channel '" + channel_name + "' owner not registered yet; retry");
    }

    // channel_entry references the existing entry when present; on the
    // consumer-opens-channel path (fan-in, first arrival) the entry
    // gets created inside `_on_consumer_joined`, so downstream checks
    // that need a `channel_entry` are skipped in that branch.
    static const pylabhub::hub::ChannelEntry k_empty_channel_entry;
    const auto &channel_entry = consumer_will_open_channel ? k_empty_channel_entry : cit->second;

    // HEP-0021 §16: reject if first producer's ZMQ endpoint has unresolved
    // port 0 (single-producer shape — the "any ready producer" scan per
    // HEP-CORE-0021 §16.4 is future work).
    const auto *cons_first_prod = channel_entry.first_producer();
    if (channel_entry.data_transport == "zmq" && cons_first_prod != nullptr &&
        !cons_first_prod->zmq_node_endpoint.empty())
    {
        auto ep_check = pylabhub::validate_tcp_endpoint(cons_first_prod->zmq_node_endpoint);
        if (ep_check.ok() && ep_check.port == 0)
        {
            LOGGER_INFO(
                "Broker: CONSUMER_REG_REQ channel '{}' ZMQ endpoint has port 0 (awaiting_endpoint)",
                channel_name);
            auto err = make_error(corr_id, "CHANNEL_NOT_READY",
                                  "ZMQ endpoint for channel '" + channel_name +
                                      "' has unresolved port 0 (awaiting_endpoint)");
            err["reason"] = "awaiting_endpoint";
            return err;
        }
    }

    // HEP-CORE-0036 §5.2 R6 gate + §6.6 rejection vocabulary:
    // CONSUMER_REG_REQ is admitted only if at least one producer-
    // presence has reached kLive (Connected + first_heartbeat_seen).
    // Without this gate a consumer would be authorized against a
    // kRegistering producer whose data plane is not yet running —
    // the consumer's `Authorized` transition would race the
    // producer's first-heartbeat fire, producing handshake failures
    // during the gap.  Mirrors DISC_REQ's presence scan.
    //
    // Fan-in EXEMPT: under fan-in the consumer is the binding side
    // per HEP-CORE-0017 §3.3.0 — it opens the channel; producers dial
    // in.  NOTE: the symmetric R6' gate (dialing producers PEND until
    // the binding consumer is Live + endpoint resolved) is DESIGNED
    // (HEP-0017 §4.7.1; draft §5.4) but NOT YET IMPLEMENTED — the
    // 2026-07-09 attempt was reverted.  Today a fan-in producer gets
    // an immediate REG_ACK (empty initial_allowlist when the consumer
    // hasn't published) and catches up via
    // CHANNEL_AUTH_CHANGED_NOTIFY (Standby → Configured).  Tracked:
    // TOPOLOGY_TODO Phase D R6.
    //
    // Four outcomes, matching the §6.6 reason catalog:
    //   - at least one kLive producer    → admit
    //   - some kRegistering (Connected,  → CHANNEL_NOT_READY /
    //     !first_heartbeat_seen)           "awaiting_first_heartbeat"
    //                                      (transient — client retries)
    //   - some kStalled (Pending; per    → CHANNEL_NOT_READY /
    //     HEP-CORE-0023 §2.6)              "heartbeat_stalled"
    //                                      (transient — client retries)
    //   - only kAbsent (Disconnected /   → CHANNEL_NOT_FOUND
    //     no presences)                    (terminal — matches
    //                                      DISC_REQ; client must not
    //                                      retry indefinitely)
    if (!consumer_will_open_channel &&
        !(declared_topology.has_value() &&
          pylabhub::hub::topology::is_owner(*declared_topology,
                                            pylabhub::hub::topology::AdmissionSide::Consumer)))
    {
        bool any_live = false;
        bool any_kRegistering = false; // Connected, !first_heartbeat
        bool any_kStalled = false;     // Pending
        for (const auto &prod : channel_entry.producers)
        {
            auto rit = snap.roles.find(prod.role_uid);
            if (rit == snap.roles.end())
                continue;
            const auto *p = rit->second.find_presence(channel_name, "producer");
            if (p == nullptr)
                continue;
            if (p->state == pylabhub::hub::RoleState::Connected)
            {
                if (p->first_heartbeat_seen)
                {
                    any_live = true;
                    break;
                }
                any_kRegistering = true;
            }
            else if (p->state == pylabhub::hub::RoleState::Pending)
            {
                any_kStalled = true;
            }
        }
        if (!any_live)
        {
            if (any_kRegistering)
            {
                LOGGER_INFO("Broker: CONSUMER_REG_REQ channel '{}' deferred — "
                            "awaiting_first_heartbeat",
                            channel_name);
                auto err = make_error(corr_id, "CHANNEL_NOT_READY",
                                      "Channel '" + channel_name +
                                          "' is not ready (awaiting_first_heartbeat — "
                                          "producer registered but has not yet sent its "
                                          "first heartbeat)");
                err["reason"] = "awaiting_first_heartbeat";
                return err;
            }
            if (any_kStalled)
            {
                LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' deferred — "
                            "heartbeat_stalled",
                            channel_name);
                auto err = make_error(corr_id, "CHANNEL_NOT_READY",
                                      "Channel '" + channel_name +
                                          "' is not ready (heartbeat_stalled — producer "
                                          "missed its heartbeat window; HEP-CORE-0023 §2.6)");
                err["reason"] = "heartbeat_stalled";
                return err;
            }
            LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' rejected — all "
                        "producer-presences absent or Disconnected",
                        channel_name);
            return make_error(corr_id, "CHANNEL_NOT_FOUND",
                              "Channel '" + channel_name + "' has no producer-presence alive");
        }
    }

    // ── Transport arbitration (HEP-CORE-0036 §5b.6) ─────────────────────────
    // `data_transport` is the REQUIRED transport declaration on
    // CONSUMER_REG_REQ, symmetric with REG_REQ (C9 resolution; the ctor
    // enforces presence + type, the handler validates the VALUE per
    // §14.7).  The retired `consumer_queue_type` wire field ("Forbidden /
    // removed" per §5b.6) is no longer read.  Against an existing channel
    // the declaration must equal the channel's stored transport or the
    // REG is rejected; on the consumer-opens-channel path (fan-in first
    // arrival) it becomes the channel's transport invariant (set inside
    // `_on_consumer_joined`) — no silent default.
    const std::string consumer_transport = body.data_transport();
    if (consumer_transport != "shm" && consumer_transport != "zmq")
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ rejected — channel '{}' role_uid='{}' "
                    "`data_transport`='{}' is not one of {{\"shm\",\"zmq\"}} "
                    "(HEP-CORE-0036 §5b.6).",
                    channel_name, role_uid, consumer_transport);
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_REG_REQ `data_transport`='" + consumer_transport +
                              "' is invalid; expected 'shm' or 'zmq'");
    }
    if (!consumer_will_open_channel && consumer_transport != channel_entry.data_transport)
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ transport mismatch on '{}': "
                    "consumer declares '{}' but channel uses '{}'",
                    channel_name, consumer_transport, channel_entry.data_transport);
        return make_error(corr_id, "TRANSPORT_MISMATCH",
                          "Consumer data_transport '" + consumer_transport +
                              "' does not match channel transport '" +
                              channel_entry.data_transport + "'");
    }

    // ── HEP-CORE-0034 §9 — consumer schema validation ───────────────────────
    //
    // Two modes per the citation rule (HEP-CORE-0034 §10.3):
    //
    //   Named: expected_schema_id present.  Consumer is asserting it
    //     knows the schema by name (presumably has the structure cached
    //     locally).  Hash check is sufficient.  If the consumer ALSO
    //     supplies expected_schema_blds + expected_schema_packing, the
    //     broker opportunistically verifies the consumer's local
    //     structure against the channel's hash (defense-in-depth —
    //     catches consumer-side hash/blds drift).
    //
    //   Anonymous: expected_schema_id empty AND any other expected_*
    //     field set.  Consumer must supply the full structure
    //     (expected_schema_blds + expected_schema_packing);
    //     expected_schema_hash is
    //     optional but, if present, must match the recomputed hash.
    //     Broker recomputes h_c from the structure and compares to the
    //     channel's schema_hash.
    //
    //   Empty: all expected_* empty → no validation (consumer signals
    //     "I don't care about schema").
    const std::string expected_schema_id = body.expected_schema_id();
    const std::string expected_hash_hex = body.expected_schema_hash();
    const std::string expected_blds = body.expected_schema_blds();
    const std::string expected_packing = body.expected_schema_packing();
    // HEP-0034 §10.3 — flexzone mirrors the producer-side wire fields
    // (Phase 5a).  When the consumer's structure includes flexzone, the
    // recomputed fingerprint must include it too — otherwise the
    // consumer-recomputed hash (slot+fz) won't match the channel's
    // stored hash (slot+fz from REG_REQ).  Same correctness gap that
    // Phase 4a fixed on the REG_REQ side; mirrored here for symmetry.
    const std::string expected_fz_blds = body.expected_flexzone_blds();
    const std::string expected_fz_packing = body.expected_flexzone_packing();
    // The owner axis is a first-class citation axis (ruled 2026-07-26).
    // Openers have it validated below (∈ {"", "hub"} + registry
    // resolution); joiners have a non-empty claim matched exactly.
    // Previously it was installed verbatim on the open path and silently
    // ignored on the join path — both stale-silent-fallbacks.
    const std::string expected_owner = body.expected_schema_owner();
    if (expected_schema_id.empty() && !expected_owner.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_REG_REQ expected_schema_owner requires "
                          "expected_schema_id — an owner claim without a named "
                          "schema is meaningless");
    }
    // Structure the OPEN path installs — may be materialized from the
    // registry record on a named open (G10); joiners never read these.
    std::string open_blds = expected_blds;
    std::string open_fz_blds = expected_fz_blds;

    // Validate the consumer's cited schema against the channel through the
    // single validator (HEP-CORE-0034 §9 / §2.4 I4).  When the consumer is
    // opening the channel (fan-in first arrival) there are no stored invariants
    // yet — its expected_* BECOME the invariants inside `_on_consumer_joined` —
    // so this block is skipped; likewise when the consumer opts out (all
    // expected_* empty).
    const bool has_any_expected = !expected_schema_id.empty() || !expected_hash_hex.empty() ||
                                  !expected_blds.empty() || !expected_packing.empty() ||
                                  !expected_fz_blds.empty() || !expected_fz_packing.empty();
    // Owner-declares rule (ratified
    // 2026-07-26): the fan-in consumer-OWNER always declares its
    // schema — the book it opens (HEP-CORE-0017 §4.7.0.1 C1/C2)
    // includes the channel's format, and dialers receive their view
    // of it from the hub.  A material-free open would create a blank
    // contract that the exact-equality matcher (empty matches only
    // empty) then holds against every schema-carrying producer.
    if (consumer_will_open_channel && !has_any_expected)
    {
        LOGGER_WARN("[broker] event=ConsumerRegReqRejected reason='SCHEMA_REQUIRED' "
                    "role='{}' channel='{}' detail='fan-in owner must declare the "
                    "channel schema at open'",
                    role_uid, channel_name);
        return make_error(corr_id, "SCHEMA_REQUIRED",
                          "fan-in channel open requires a schema declaration — the "
                          "owner establishes the channel's format (HEP-CORE-0017 "
                          "§4.7.0.1 C1/C2)");
    }

    if (has_any_expected)
    {
        // Step 1 (Job A) — resolve the citer's fingerprint.  Runs for
        // JOINERS and OPENERS alike (the open row validates the
        // contract it installs at least as strictly as the join row
        // checks those who match it — closes the G8 hole where an
        // owner's stale/typo'd config seeded an internally
        // inconsistent contract).  Required-field presence is
        // mode-specific and stays here; the recompute +
        // self-consistency is the shared pre-check (the ONE place a
        // handler recomputes the wire hash — HEP-CORE-0034 §2.4 I4).
        const bool named = !expected_schema_id.empty();
        const bool has_structure = !expected_blds.empty() || !expected_packing.empty();
        if (named && expected_hash_hex.empty())
            return make_error(corr_id, "MISSING_HASH_FOR_NAMED_CITATION",
                              "CONSUMER_REG_REQ with expected_schema_id requires "
                              "expected_schema_hash (HEP-CORE-0034 §10.3)");

        std::array<uint8_t, 64> joiner_hash{};
        if (has_structure || !named)
        {
            // Named-with-structure (defense-in-depth) or anonymous: the full
            // structure is required and the fingerprint is computed from it.
            const char *miss_blds = named ? "MISSING_BLDS" : "MISSING_BLDS_FOR_ANONYMOUS_CITATION";
            const char *miss_pack =
                named ? "MISSING_PACKING" : "MISSING_PACKING_FOR_ANONYMOUS_CITATION";
            if (expected_blds.empty())
                return make_error(corr_id, miss_blds,
                                  "citation requires expected_schema_blds "
                                  "(HEP-CORE-0034 §10.3)");
            if (expected_packing.empty())
                return make_error(corr_id, miss_pack,
                                  "citation requires expected_schema_packing "
                                  "(HEP-CORE-0034 §10.3)");
            const auto fp = pylabhub::hub::verify_request_fingerprint(
                expected_blds, expected_packing, expected_fz_blds, expected_fz_packing,
                expected_hash_hex);
            if (!fp.consistent)
                return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                                  "expected_schema_hash does not match BLAKE2b-256 "
                                  "of canonical(expected_schema_blds + packing)");
            joiner_hash = fp.hash; // computed-from-string
        }
        else
        {
            // Named without structure: the claim IS the cited fingerprint.
            joiner_hash = fingerprint_for_compare(expected_hash_hex);
        }

        // Open-row rule: the installed contract must carry its
        // fingerprint.  A joiner's anonymous citation may omit the hash
        // (the broker recomputes it); an OPENER's citation becomes the
        // channel record verbatim, and a hash-less record would reject
        // every honest citer against an empty fingerprint.
        if (consumer_will_open_channel && expected_hash_hex.empty())
        {
            return make_error(corr_id, "MISSING_HASH",
                              "channel-open citation requires expected_schema_hash — "
                              "the opened channel's contract must carry its "
                              "fingerprint (HEP-CORE-0034 §10.3)");
        }

        if (consumer_will_open_channel)
        {
            // Opener: no stored invariants exist to match against — the
            // now-validated citation BECOMES the invariants inside
            // `_on_consumer_joined`.  Steps 2/3 are joiner-only.
            //
            // Ruled 2026-07-26: the open row validates the
            // OWNER AXIS it installs.  The producer front-door defaults
            // an ownerless named citation to cited_owner=self, so on
            // fan-in — where EVERY producer is a joiner — a named book
            // is joinable only under owner="hub" (all producers adopt
            // the same hub-global).  An empty or third-party owner
            // would open a channel no producer can ever join: the name
            // axis blocks anonymous joiners, the owner axis named ones.
            if (!expected_owner.empty() && expected_owner != "hub")
            {
                LOGGER_WARN("[broker] event=ConsumerRegReqRejected "
                            "reason='SCHEMA_FORBIDDEN_OWNER' role='{}' channel='{}' "
                            "claimed_owner='{}'",
                            role_uid, channel_name, expected_owner);
                return make_error(corr_id, "SCHEMA_FORBIDDEN_OWNER",
                                  "channel-open citation may claim owner \"hub\" only — "
                                  "a consumer cannot open under a producer's namespace "
                                  "");
            }
            if (named && expected_owner.empty())
            {
                LOGGER_WARN("[broker] event=ConsumerRegReqRejected "
                            "reason='SCHEMA_OWNER_REQUIRED' role='{}' channel='{}' "
                            "schema_id='{}'",
                            role_uid, channel_name, expected_schema_id);
                return make_error(corr_id, "SCHEMA_OWNER_REQUIRED",
                                  "named fan-in open requires expected_schema_owner="
                                  "\"hub\" — an unowned named book is unjoinable by "
                                  "every producer");
            }
            if (named)
            {
                // owner == "hub": the named open IS a registry citation —
                // resolve it through the single validator exactly like
                // the producer's Path C (HEP-CORE-0034 §9 step 3).
                pylabhub::hub::SchemaCitationInput sin;
                sin.channel_owner = "hub";
                sin.channel_id = expected_schema_id;
                sin.channel_hash = joiner_hash;
                sin.cited_id = expected_schema_id;
                sin.expected_hash = joiner_hash;
                sin.check_registry_record = true;
                const auto vc = hub_state_->_validate_schema_citation(sin);
                if (!vc.ok())
                {
                    const char *code = "SCHEMA_MISMATCH"; // defensive default
                    switch (vc.reason)
                    {
                        using R = ::pylabhub::schema::CitationOutcome::Reason;
                    case R::kUnknownSchema:
                        code = "SCHEMA_UNKNOWN"; // no such hub-global
                        break;
                    case R::kFingerprintMismatch:
                        code = "FINGERPRINT_INCONSISTENT"; // exists, differs
                        break;
                    default:
                        break;
                    }
                    LOGGER_WARN("[broker] event=ConsumerRegReqRejected reason='{}' "
                                "role='{}' channel='{}' detail='{}'",
                                code, role_uid, channel_name, vc.detail);
                    return make_error(corr_id, code,
                                      "Cannot open on hub-global (hub, " + expected_schema_id +
                                          "): " + vc.detail);
                }
                // G10 — a named-open citation may omit the structure; the
                // registry record always carries it (`make_schema_record`
                // asserts >= 1 zone).  Materialize it into the channel
                // invariants so the channel record serves structure
                // directly (channel-form SCHEMA_REQ; the establishment
                // ACK when schema-at-establishment lands).
                if (expected_blds.empty())
                {
                    if (const auto rec = hub_state_->schema("hub", expected_schema_id);
                        rec.has_value())
                    {
                        open_blds = rec->blds;
                        open_fz_blds = rec->flexzone_blds;
                    }
                }
            }
        }
        else
        {
            // Step 2/3 — channel match + named-registry via the single validator.
            pylabhub::hub::SchemaCitationInput sin;
            sin.channel_owner = channel_entry.schema_owner;
            sin.channel_id = channel_entry.schema_id;
            sin.channel_hash = fingerprint_for_compare(channel_entry.schema_hash);
            sin.channel_producer_uids.reserve(channel_entry.producers.size());
            for (const auto &p : channel_entry.producers)
                sin.channel_producer_uids.push_back(p.role_uid);
            sin.cited_id = expected_schema_id;
            sin.cited_owner = expected_owner; // exact-match when claimed
            sin.expected_hash = joiner_hash;

            const auto vc = hub_state_->_validate_schema_citation(sin);
            if (!vc.ok())
            {
                // Consumer-side (data-out) direction: id mismatch → SCHEMA_ID_MISMATCH,
                // else SCHEMA_CITATION_REJECTED (HEP-CORE-0034 §10.4).
                const char *code =
                    vc.reason == ::pylabhub::schema::CitationOutcome::Reason::kSchemaIdMismatch
                        ? "SCHEMA_ID_MISMATCH"
                        : "SCHEMA_CITATION_REJECTED";
                LOGGER_WARN("Broker: schema match rejected — channel='{}' consumer_uid='{}' "
                            "code={}: {}",
                            channel_name, role_uid, code, vc.detail);
                return make_error(corr_id, code, vc.detail);
            }
        }
    }
    // else: all expected_* empty on a JOIN — the opt-out mode (
    // readers may opt out; the channel's integrity machinery holds
    // regardless).  The material-free OPEN was rejected above (the
    // Option B), and a material-carrying OPEN was self-validated
    // (open-row rule) without a channel match (nothing stored to match yet).

    // Role identity is enforced by the CTRL ROUTER's ZAP handler at the
    // CURVE handshake (HEP-CORE-0035 §4.1); grammar already ran at handler
    // entry (audit R3.5b).  The legacy self-asserted string gate was
    // deleted per §4.5 / §8 Phase 6.

    pylabhub::hub::ConsumerEntry entry;
    entry.consumer_pid = body.consumer_pid();
    entry.consumer_hostname = body.consumer_hostname();
    entry.role_name = role_name;
    entry.role_uid = role_uid;
    entry.inbox_endpoint = body.inbox_endpoint();
    entry.inbox_schema_json = body.inbox_schema_json();
    // Derived from the once-parsed spec (HEP-0046 B.2) — the separate
    // `inbox_packing` wire field is retired; packing lives in-object.
    entry.inbox_packing = body.has_inbox_schema() ? body.inbox_schema().packing : std::string{};
    entry.inbox_checksum = body.inbox_checksum();
    // HEP-CORE-0036 §6.5: the consumer's CURVE pubkey is REQUIRED on
    // the wire so the broker can populate the channel-scope
    // authorized-consumer allowlist via `_on_consumer_authorized` and
    // revoke it on DEREG / heartbeat timeout.  HEP-CORE-0035 §2 makes
    // CURVE unconditional.  Empty or wrong-length values are
    // programmer errors and rejected at wire admission, matching the
    // producer-side REG_REQ check.
    // Consumer's CURVE pubkey (HEP-CORE-0036 §6.5) — presence + Z85 length
    // (==40) already enforced by gate_grammar (§14.5); stored on the consumer
    // entry for the channel authorized-consumer allowlist.
    const std::string consumer_pubkey = body.zmq_pubkey();

    // HEP-CORE-0036 §6.3 Layer-2 identity verification — symmetric
    // with the producer-side REG_REQ check (§6.1).  Binds the
    // CONSUMER_REG_REQ to a specific (role_uid, pubkey) pair within
    // the operator-authorized known_roles allowlist.
    // Known-role (uid, pubkey) binding check already ran at the
    // wire_dispatch pipeline via gate_attested_binding.  Legacy
    // verify_known_role_binding retired (2026-07-14 task #46).
    entry.zmq_pubkey = consumer_pubkey;
    // Capture ZMQ identity for future CHANNEL_CLOSING_NOTIFY.
    entry.zmq_identity.assign(static_cast<const char *>(identity.data()), identity.size());

    // Atomic consumer-side admission — topology + cardinality checks
    // run under HubState's writer lock alongside the mutation.
    //
    // Fan-in consumer-opens-channel path: supply schema + transport
    // invariants gleaned from the wire so the atomic op can create
    // the ChannelEntry with the consumer as first + only admitted role
    // (HEP-CORE-0017 §3.3.0 binding-side rule).  The consumer's
    // expected_* fields become the channel's stored invariants;
    // subsequent producer REG_REQs must match them.
    std::optional<pylabhub::hub::ChannelSchemaInvariants> open_schema;
    std::optional<pylabhub::hub::ChannelTransportInvariants> open_transport;
    if (consumer_will_open_channel)
    {
        pylabhub::hub::ChannelSchemaInvariants s;
        s.schema_hash = expected_hash_hex;
        // expected_schema_version retired per C2 — version rides inside
        // expected_schema_id (`$name.v<N>`) per HEP-CORE-0034 §5.1.
        s.schema_id = expected_schema_id;
        // open_* may carry structure materialized from the hub-global
        // record on a named open (G10) — otherwise they equal the
        // consumer's expected_* verbatim.
        s.schema_blds = open_blds;
        s.flexzone_blds = open_fz_blds;  // two-zone content (fingerprint folds packing)
        s.schema_owner = expected_owner; // validated ∈ {"", "hub"}
        open_schema = std::move(s);

        pylabhub::hub::ChannelTransportInvariants t;
        // §5b.6: the consumer's REQUIRED declaration (value-validated
        // above) is the channel's transport invariant — no silent
        // default.  A fan-in × "shm" declaration is rejected by the
        // topology×transport admissibility check inside the atomic
        // `_on_consumer_joined` (TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT).
        t.data_transport = consumer_transport;
        open_transport = std::move(t);
    }
    const auto cons_admission =
        hub_state_->_on_consumer_joined(channel_name, std::move(entry), declared_topology,
                                        std::move(open_schema), std::move(open_transport));
    if (cons_admission.invalid_identifier)
    {
        LOGGER_WARN("[broker] event=ConsumerRegReqRejected reason='INVALID_REQUEST' "
                    "role='{}' channel='{}' detail='identifier grammar failure'",
                    role_uid, channel_name);
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_REG_REQ rejected — identifier grammar failure "
                          "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (cons_admission.topology_error_code != nullptr)
    {
        LOGGER_WARN("[broker] event=ConsumerRegReqRejected reason='{}' role='{}' channel='{}'",
                    cons_admission.topology_error_code, role_uid, channel_name);
        return make_error(corr_id, cons_admission.topology_error_code,
                          std::string("CONSUMER_REG_REQ rejected — topology gate '") +
                              cons_admission.topology_error_code + "'");
    }
    // `admitted=false` with no `topology_error_code` = the atomic op's
    // silent-skip contract: the channel vanished between the pre-check
    // above and the writer-lock acquisition inside `_add_consumer`
    // (race with a concurrent `_on_channel_closed`; the identifier-
    // validation branch surfaces separately as `invalid_identifier`).
    // Surface as AWAITING_OWNER — the observable state is identical to
    // the handler-entry gate's (dialing consumer, no channel), and the
    // wire reply must be a pure function of state, not of which side
    // of the writer lock the teardown landed on (HEP-CORE-0017
    // §4.7.0.1 C3).  The role's retry loop re-attempts within its
    // budget; a fan-in consumer-owner that raced its own channel's
    // teardown re-enters on retry via the consumer-opens path.
    if (!cons_admission.admitted)
    {
        LOGGER_WARN("[broker] event=ConsumerRegReqAwaitingOwner role='{}' channel='{}' "
                    "detail='channel vanished during admission; retryable'",
                    role_uid, channel_name);
        return make_error(corr_id, "AWAITING_OWNER",
                          "channel '" + channel_name +
                              "' vanished during admission (owner gone); retry");
    }

    // HEP-CORE-0036 §6.5 + §I11.1 + §6.6.2 — every channel admission
    // wires a `ChannelAccessEntry` so `GET_CHANNEL_AUTH_REQ` finds a
    // populated record to serve.  The wire response falls back to
    // "empty allowlist + loud log" if the record is missing (§6.6.2
    // response semantics), so a missed call here does not deadlock
    // the caller — but the invariant "channel is open ⟹ access
    // record exists" is what makes the fail-closed empty-list branch
    // a genuine "no peers yet" observation rather than a chronic
    // broker-state anomaly.  Wire the call symmetrically:
    //   - Producer opens (fan-out / one-to-one): wired at
    //     `handle_reg_req` when `admission.channel_opened == true`.
    //   - Consumer opens (fan-in): wired here.  Under HEP-CORE-0017
    //     §3.3.0 the fan-in consumer is the binding side and creates
    //     the channel record on first arrival; the access record is
    //     its symmetric partner.
    // Missing this call was the pre-topology-migration bug that
    // caused `GET_CHANNEL_AUTH_REQ` to hit the "no ChannelAccessEntry"
    // branch on every fan-in consumer pull.  Under the amended §6.6.2
    // response semantics that no longer produces `INTERNAL_ERROR`,
    // but it would still fire an ERROR log on every pull — the
    // invariant here is the primary fix; the amended response is
    // defence-in-depth.
    if (cons_admission.channel_opened)
    {
        hub_state_->_on_channel_access_opened(channel_name);
    }

    // HEP-CORE-0036 §6.5 — the "add pubkey to the channel-scope
    // allowlist and fire the change notify" step is topology-scoped:
    //   - Fan-out / one-to-one: the consumer is the dialing side.
    //     Producer(s) bind and run ZAP as server; their ZAP allowlist
    //     needs this new consumer's pubkey.  Add + notify producers.
    //   - Fan-in: the consumer is the binding side.  The
    //     `ChannelAccessEntry.ledger` (HEP-CORE-0042 §5.5.2 unified
    //     2026-07-13) holds producer pubkeys admitted for the binding
    //     consumer's ZAP; see the mirror comment in the fan-in branch
    //     of handle_reg_req.  Adding
    //     the consumer's own pubkey here would insert self into its
    //     own ZAP allowlist and — worse — inflate every subsequent
    //     `admitted_peers_count(channel)` reading on the loop-ready
    //     gate (HEP-CORE-0011 §"Loop-ready gate") by 1, causing the
    //     consumer's gate to flip Ready before any producer has been
    //     admitted.  A self-targeted CHANNEL_AUTH_CHANGED_NOTIFY is
    //     semantically meaningless — under fan-in the consumer IS
    //     the only binding-side role and does not authenticate
    //     against itself.  Skip both under fan-in.
    // Effective topology: the channel-open path (fan-in first
    // arrival) uses the declared topology since `channel_entry`
    // hasn't been populated yet; the join-existing-channel path uses
    // the stored topology from `channel_entry`.
    const auto effective_topology = consumer_will_open_channel
                                        ? *declared_topology // must be FanIn per the
                                                             // `consumer_will_open_channel` guard
                                        : channel_entry.topology;
    const bool consumer_is_binding =
        pylabhub::hub::Queue::reader_is_binding_side(effective_topology);
    if (!consumer_is_binding)
    {
        hub_state_->_on_consumer_authorized(channel_name, consumer_pubkey);
        fire_channel_auth_changed_notify(socket, channel_name,
                                         /*phase=*/"admitted",
                                         /*role_uid=*/role_uid,
                                         /*role_type=*/"consumer");
    }

    // HEP-CORE-0036 §6.4 — one-shot per accepted CONSUMER_REG_REQ; format
    // parallels REG_REQ accepted (line ~1148) so test harnesses can grep
    // by uid/channel/pubkey.  Pubkey is Z85 (40 chars).
    LOGGER_INFO("[broker] event=ConsumerRegReqAccepted role='{}' channel='{}' "
                "consumer_pubkey='{}'",
                role_uid, channel_name, consumer_pubkey);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel_name"] = channel_name;
    resp["message"] = "Consumer registered successfully";
    resp["heartbeat"] = heartbeat_ack_block(); // HEP-CORE-0023 §2.5
    // HEP-CORE-0032 §8.2 — broker ABI envelope echo, symmetric with
    // REG_ACK path so consumer-side verification per §8.7 can use the
    // same broker_abi_fingerprint field name across both ACK shapes.
    resp["broker_abi_fingerprint"] =
        pylabhub::version::to_json_object(pylabhub::version::current());
    if (const char *bid = pylabhub::version::build_id())
    {
        resp["broker_build_id"] = bid;
    }
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }

    // HEP-CORE-0036 §5b / §6.4 — unified `producers[]` shape across
    // transports (B-4, #289, 2026-06-25).  Pre-B-4 the ZMQ branch
    // emitted `producers[]` while the SHM branch emitted flat
    // `shm_capability_endpoint` + `producer_pubkey_z85` fields, and
    // the consumer discriminated by "which field name is present"
    // (a fragile pattern — the same silent-gate class as the
    // pre-B-1 `channel_id` vs `channel_name` bug).
    //
    // Unified shape:
    //   resp["data_transport"] ∈ {"shm", "zmq"}  — self-describing
    //                                              discriminator
    //   resp["producers"] = [{role_uid, pubkey_z85, endpoint}, ...]
    //
    // `endpoint` is transport-specific by value, uniform by key:
    //   - ZMQ → `ProducerEntry::zmq_node_endpoint` (tcp://host:port)
    //   - SHM → `ProducerEntry::shm_capability_endpoint`
    //           (ipc:///run/.../<channel>.sock for the §5.5 ZAP-CURVE
    //           dial; HEP-CORE-0041 §5.3)
    //
    // `pubkey_z85` is the SINGLE canonical key (no `pubkey` dual
    // name).  HEP-CORE-0036 §I10 invariant: one CURVE pubkey per
    // role uid, irrespective of transport.
    //
    // SHM is single-producer per HEP-CORE-0023 §2.1.1 cardinality;
    // the broker has already rejected a second SHM producer at
    // REG_REQ time via the topology cardinality gate
    // (ONE_TO_ONE_CARDINALITY_VIOLATED under the default topology;
    // FAN_OUT_IS_SINGLE_PRODUCER under fan-out; the pre-migration
    // MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM is retired), so for
    // SHM channels `producers[]` has length 1 by construction.
    //
    // The legacy AUTH-4 / task #164 design (a single `shm_secret`
    // field) is SUPERSEDED by HEP-CORE-0041 and was never wired on
    // the wire — do not add it.
    if (auto ch_opt = hub_state_->channel(channel_name); ch_opt.has_value())
    {
        resp["data_transport"] = ch_opt->data_transport;
        nlohmann::json producers_array = nlohmann::json::array();
        for (const auto &p : ch_opt->producers)
        {
            nlohmann::json entry;
            entry["role_uid"] = p.role_uid;
            entry["pubkey_z85"] = p.zmq_pubkey;
            if (ch_opt->data_transport == "shm")
            {
                entry["endpoint"] = p.shm_capability_endpoint;
            }
            else
            {
                entry["endpoint"] = p.zmq_node_endpoint;
            }
            producers_array.push_back(std::move(entry));
        }
        resp["producers"] = std::move(producers_array);

        // Schema-at-establishment (HEP-CORE-0034 §10.3a): the channel's
        // established schema rides the success ACK — the consumer's view
        // of the book includes its format.  Empty fields are ELIDED (an
        // absent axis is not established on the record); a hash-only
        // anonymous channel delivers schema_hash and no blds by design
        // by design.  NO packing fields — the fingerprint binds packing;
        // the receiver recovers it via the §6.4 candidate recompute.
        // Success-only by construction: this block runs after every
        // admission gate has passed (trust sequence).
        if (!ch_opt->schema_id.empty())
            resp["schema_id"] = ch_opt->schema_id;
        if (!ch_opt->schema_owner.empty())
            resp["schema_owner"] = ch_opt->schema_owner;
        if (!ch_opt->schema_blds.empty())
            resp["blds"] = ch_opt->schema_blds;
        if (!ch_opt->flexzone_blds.empty())
            resp["flexzone_blds"] = ch_opt->flexzone_blds;
        if (!ch_opt->schema_hash.empty())
            resp["schema_hash"] = ch_opt->schema_hash;
    }

    // HEP-CORE-0027 §3.5 — the roster this consumer's inbox gate answers
    // from.  Consumers have inboxes too, and every role on a hub is
    // entitled to the same list, so this is the identical block the
    // producer REG_ACK carries.
    //
    // Built here rather than earlier so this role is in its OWN roster:
    // registration is already committed by the time the ACK is assembled,
    // so the ledger has admitted it and the block names it.
    resp.update(roster_ack_block());

    return resp;
}

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body (ValidatedConsumerDeregReq carries a DeregReqBody).  Gates already ran.
nlohmann::json
BrokerServiceImpl::handle_consumer_dereg_req(const ::pylabhub::wire::WireEnvelope &env,
                                             const ::pylabhub::wire::DeregReqBody &body,
                                             zmq::socket_t &socket)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const std::string wire_role_uid = body.role_uid();

    // Target resolution is by `role_uid` ALONE — the authoritative unique
    // consumer key (HEP-CORE-0023 §2.1.1).  A PID is NOT read here — it is
    // debug/record only, machine-local, and never a validation input
    // (HEP-CORE-0023 "A PID is debug/record only").
    //
    // Resolving by uid alone is safe because `receive_and_validate` ran
    // `gate_attested_role_ownership`: the connection PROVED a key the hub
    // resolves to this role_uid.  The routing-id-equals-role_uid check is
    // NOT what establishes that — both of those values are client-chosen.

    // Fetch consumer entry BEFORE removal so the cleanup hook can read
    // role_uid — and the whole pre-drop ChannelEntry, because under
    // fan-in this consumer is the binding OWNER and its leave tears
    // the channel down (HEP-CORE-0017 §4.7.0.2 T2): the close-out
    // fan-out below needs the party list of a record that will be
    // gone from HubState by then.
    pylabhub::hub::ConsumerEntry closing_entry{};
    pylabhub::hub::ChannelEntry pre_drop_channel{};
    bool have_entry = false;
    {
        auto ch = hub_state_->channel(channel_name);
        if (ch.has_value())
        {
            for (const auto &c : ch->consumers)
            {
                if (c.role_uid == wire_role_uid)
                {
                    closing_entry = c;
                    pre_drop_channel = *ch;
                    have_entry = true;
                    break;
                }
            }
        }
    }

    if (!have_entry)
    {
        LOGGER_WARN("Broker: CONSUMER_DEREG_REQ failed for channel '{}' (role_uid='{}')",
                    channel_name, wire_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Consumer (role_uid='" + wire_role_uid +
                              "') not registered for channel '" + channel_name + "'");
    }

    // Consumer voluntarily left.  `role_uid` was validated non-empty
    // by `gate_grammar` in `receive_and_validate` (HEP-CORE-0046
    // §14.5), so the role-side cleanup branch in `_on_consumer_left`
    // always runs.
    const auto drop = hub_state_->_on_consumer_left(channel_name, closing_entry.role_uid);
    on_consumer_closed(socket, channel_name, closing_entry, "voluntary_close");

    if (drop.channel_now_empty)
    {
        // HEP-CORE-0017 §4.7.0.2 T2 — the leaving consumer was the
        // fan-in binding OWNER: owner leave is channel death.  Same
        // close-out sequence as the last-owning-producer DEREG path
        // in `handle_dereg_req`: CHANNEL_CLOSING_NOTIFY to every
        // party from the pre-drop snapshot, federation relay,
        // channel-access record close (whole ChannelAccessEntry —
        // ledger included — goes; a per-key revoke would be redundant
        // and is skipped), and the pending-ATTACH drain.
        send_closing_notify(socket, channel_name, pre_drop_channel, "consumer_deregistered");
        on_channel_closed(socket, channel_name, pre_drop_channel, "consumer_deregistered");
        hub_state_->_on_channel_access_closed(channel_name);
        drain_pending_attach_queue_for_channel_denied_(socket, channel_name, "channel_closing");
        LOGGER_INFO("Broker: channel '{}' torn down (fan-in consumer-owner deregistered)",
                    channel_name);
    }
    // HEP-CORE-0036 §6.5: revoke this consumer's pubkey from the
    // channel-scope allowlist + notify producers.  CONSUMER_REG_REQ
    // hard-rejects empty / non-40-char `zmq_pubkey` at the wire
    // (HEP-CORE-0035 §2 unconditional CURVE), so every stored
    // ConsumerEntry MUST carry a 40-char Z85 key.  A value that
    // fails that invariant indicates HubState corruption — log
    // loudly and skip the revoke (passing a malformed pubkey to
    // `_on_consumer_revoked` would be a no-op anyway).
    else if (closing_entry.zmq_pubkey.size() != 40)
    {
        LOGGER_ERROR("Broker: ConsumerEntry on channel='{}' role_uid='{}' has "
                     "invalid zmq_pubkey (length {}, expected 40 Z85 chars).  "
                     "This SHOULD NOT happen — CONSUMER_REG_REQ hard-rejects "
                     "empty / wrong-length pubkey at the wire (HEP-CORE-0035 §2 "
                     "unconditional CURVE).  System may be compromised; restart "
                     "the hub ASAP.  Skipping channel-access revocation.",
                     channel_name, closing_entry.role_uid, closing_entry.zmq_pubkey.size());
    }
    else
    {
        hub_state_->_on_consumer_revoked(channel_name, closing_entry.zmq_pubkey);
        fire_channel_auth_changed_notify(socket, channel_name,
                                         /*phase=*/"left",
                                         /*role_uid=*/closing_entry.role_uid,
                                         /*role_type=*/"consumer");
    }

    LOGGER_INFO("Broker: consumer deregistered from channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["message"] = "Consumer deregistered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ─── Channel-auth pull + notify helpers (HEP-CORE-0036 §6.5) ───────────────

nlohmann::json
BrokerServiceImpl::handle_get_channel_auth_req(const ::pylabhub::wire::WireEnvelope &env,
                                               const ::pylabhub::wire::GetChannelAuthReqBody &body)
{
    // HEP-CORE-0036 §6.5 — producer pulls the channel-scope
    // authorized-consumer allowlist.
    // Reply (success): { status="success", allowlist=[z85, ...], corr_id }.
    // Reply (error):   { status="error", error_code, message, corr_id }.
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    const std::string caller_uid = body.role_uid();

    if (channel_name.empty() || caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "GET_CHANNEL_AUTH_REQ requires non-empty "
                          "channel_name and role_uid");
    }

    // Authorization: the caller MUST be a registered binding-side
    // role of the requested channel.  Defence-in-depth — even though
    // Layer-1 ZAP already gated who can speak to the broker, we
    // never reveal one channel's allowlist to a role that doesn't
    // legitimately need it.  The binding-side role runs ZAP as
    // server for the channel and is the only role that seeds the
    // allowlist into its own transport (HEP-CORE-0036 §6.5 +
    // §6.6):
    //   - Fan-out / one-to-one → producer binds → producer authorised
    //   - Fan-in              → consumer binds → consumer authorised
    // A dialing-side role has no ZAP allowlist to seed; sending it
    // the peer set would leak information without operational need.
    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' does not exist");
    }
    const bool consumer_binds = pylabhub::hub::Queue::reader_is_binding_side(ch->topology);
    bool caller_is_binding_side = false;
    if (consumer_binds)
    {
        for (const auto &cons : ch->consumers)
        {
            if (cons.role_uid == caller_uid)
            {
                caller_is_binding_side = true;
                break;
            }
        }
    }
    else
    {
        for (const auto &prod : ch->producers)
        {
            if (prod.role_uid == caller_uid)
            {
                caller_is_binding_side = true;
                break;
            }
        }
    }
    if (!caller_is_binding_side)
    {
        // Defence-in-depth: a misbehaving (or compromised) role
        // querying another channel's allowlist is observable here,
        // separately from the wire-level CURVE+ZAP layer that gates
        // who can speak to the broker at all.  The wire code
        // preserves `PRODUCER_NOT_AUTHORIZED` for back-compat with
        // pre-topology-migration parsers; the message names the
        // binding-side rule that actually applies.
        LOGGER_WARN("Broker: GET_CHANNEL_AUTH_REQ rejected — role_uid='{}' is "
                    "not the binding-side role of channel '{}' (topology={}, "
                    "expected {} to bind) (HEP-CORE-0036 §6.6)",
                    caller_uid, channel_name, static_cast<int>(ch->topology),
                    consumer_binds ? "consumer" : "producer");
        return make_error(corr_id, "PRODUCER_NOT_AUTHORIZED",
                          "Caller role_uid='" + caller_uid +
                              "' is not the binding-side role of channel '" + channel_name +
                              "' (topology requires " +
                              std::string(consumer_binds ? "consumer" : "producer") + " to bind)");
    }

    // Read the current authoritative allowlist.  Two distinct
    // situations are handled by the two-layer lookup above + here:
    //   1. `ChannelEntry` missing → `CHANNEL_NOT_FOUND` (returned
    //      already at line ~3742).  The channel isn't a thing.
    //   2. `ChannelEntry` present but `ChannelAccessEntry` missing →
    //      the channel exists but has no admitted peers on record.
    //      This is either (a) a legitimate observation (no
    //      `_on_consumer_authorized` call has landed since channel
    //      open — normal at startup) or (b) a bug on some code path
    //      that opened a channel without wiring
    //      `_on_channel_access_opened`.  In both cases the
    //      externally-observable truth is the same: zero admitted
    //      pubkeys.  Wire response is `status=success`,
    //      `allowlist=[]`; the caller's ZAP applies empty (fail-
    //      closed, correct) and its default loop-ready gate stays
    //      NotReady until the next NOTIFY drives a re-pull.  A loud
    //      ERROR log fires so operators diagnosing case (b) via
    //      grep / alerts see the anomaly, but the wire does not
    //      fail — refusing to serve a query whose answer is
    //      genuinely "zero" makes the wire semantics less truthful,
    //      not more.
    auto access = hub_state_->channel_access(channel_name);
    if (!access.has_value())
    {
        LOGGER_ERROR("Broker: GET_CHANNEL_AUTH_REQ for channel '{}' from role_uid='{}' "
                     "found a ChannelEntry but no ChannelAccessEntry — HubState "
                     "invariant may be broken (REG path should wire "
                     "`_on_channel_access_opened` on every channel admission).  "
                     "Returning empty allowlist (fail-closed on wire; operator "
                     "should investigate the log to distinguish 'no peers admitted "
                     "yet' from 'callback not wired on some open path').",
                     channel_name, caller_uid);
        access = pylabhub::hub::ChannelAccessEntry{};
    }
    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel_name"] = channel_name;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;

    // HEP-CORE-0036 §6.5 (locked 2026-06-12): allowlist entries are
    // bare Z85 pubkey strings — symmetric with §6.2
    // `REG_ACK.initial_allowlist` and the role-side cache
    // (`PeerAllowlist::peers` is `std::set<PeerIdentity>` keyed on
    // pubkey).  The pubkey is the authoritative enforcement key at
    // the producer's ZAP layer; `role_uid` is operator-side metadata
    // and is not needed on the wire (a producer that wants the
    // role_uid for the matching pubkey resolves it locally via its
    // `known_roles` view, not via this ACK).  Earlier 2026-06-10
    // `{role_uid, pubkey}` shape retired; see AUTH_TODO sub-6.1.
    nlohmann::json allowlist_arr = nlohmann::json::array();
    access->ledger.for_each_admitted([&](const std::string &pk) { allowlist_arr.push_back(pk); });
    resp["allowlist"] = std::move(allowlist_arr);

    // HEP-CORE-0042 §5.5.4: GET_CHANNEL_AUTH_ACK echoes
    // `snapshot_version` — the `channel_version[K]` value at the
    // moment `allowlist` above was extracted.  The producer applies
    // the allowlist to its ZAP cache, then emits
    // CHANNEL_AUTH_APPLIED_REQ with `applied_version =
    // snapshot_version` so the broker can advance
    // confirmed_version[K][P] and drain any pending attach requests
    // waiting for this producer to catch up (§5.4 wait-path).
    // Reading `access->ledger.current_version()` here reflects the same
    // snapshot as the allowlist copy above (single-threaded ROUTER
    // dispatch context, no interleaved writer).
    resp["snapshot_version"] = access->ledger.current_version();
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_consumer_attach_req_shm(const nlohmann::json &req)
{
    // HEP-CORE-0041 §9 D4 step 4-5 — pre-attach broker confirmation.
    // Producer asks whether one specific consumer is currently
    // authorized for a channel before sending the SHM capability fd.
    //
    // Request shape:
    //   { channel_name, consumer_pubkey, consumer_role_uid,
    //     role_uid (= caller's = producer's), [correlation_id] }
    //
    // Reply (auth decision — both wrapped in CONSUMER_ATTACH_ACK_SHM by
    // the dispatcher special case at the call site):
    //   success: { status="success", channel_name, consumer_pubkey,
    //              corr_id }
    //   denied:  { status="denied",  channel_name, consumer_pubkey,
    //              denial_reason, corr_id }
    //
    // Reply (protocol-level error — sent as ERROR frame):
    //   INVALID_REQUEST / CHANNEL_NOT_FOUND / PRODUCER_NOT_AUTHORIZED /
    //   INTERNAL_ERROR — caller treats these distinct from "denied".
    //
    // Read-only against HubState — pure query, no mutation.

    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    const std::string consumer_pubkey = req.value("consumer_pubkey", "");
    const std::string consumer_role_uid = req.value("consumer_role_uid", "");
    const std::string caller_uid = req.value("role_uid", "");

    if (channel_name.empty() || consumer_pubkey.empty() || consumer_role_uid.empty() ||
        caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_ATTACH_REQ_SHM requires non-empty "
                          "channel_name, consumer_pubkey, "
                          "consumer_role_uid, and role_uid");
    }

    // HEP-CORE-0032 §8 — log producer's ABI fingerprint on ATTACH ingest.
    // Log-only: this is a per-consumer authorization query from an
    // already-registered producer (strict-mode reject already exercised
    // at REG_REQ if configured); redundant rejection here would be
    // heavy-handed.
    (void)log_peer_abi_fingerprint(req, caller_uid, "ConsumerAttachAbiReceived",
                                   "ConsumerAttachAbiDetail",
                                   /*strict_mode=*/false,
                                   /*transport=*/"shm");

    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' does not exist");
    }

    // Authorization: caller MUST be a registered producer of the
    // channel.  Mirrors GET_CHANNEL_AUTH_REQ's defence-in-depth — we
    // never disclose another channel's auth state to a non-producer
    // caller, even though Layer-1 ZAP already gated who can speak to
    // the broker.
    bool caller_is_producer = false;
    for (const auto &prod : ch->producers)
    {
        if (prod.role_uid == caller_uid)
        {
            caller_is_producer = true;
            break;
        }
    }
    if (!caller_is_producer)
    {
        LOGGER_WARN("Broker: CONSUMER_ATTACH_REQ_SHM rejected — role_uid='{}' is "
                    "not a registered producer of channel '{}' "
                    "(HEP-CORE-0041 §9 D4 PRODUCER_NOT_AUTHORIZED)",
                    caller_uid, channel_name);
        return make_error(corr_id, "PRODUCER_NOT_AUTHORIZED",
                          "Caller role_uid='" + caller_uid +
                              "' is not a registered producer of channel '" + channel_name + "'");
    }

    auto access = hub_state_->channel_access(channel_name);
    if (!access.has_value())
    {
        // Same broker-invariant-broken short-circuit as
        // handle_get_channel_auth_req: degrading to deny-all here
        // would silently lock every consumer out of a working
        // channel; INTERNAL_ERROR forces a hub restart instead.
        LOGGER_ERROR("Broker: CONSUMER_ATTACH_REQ_SHM for channel '{}' from role_uid='{}' "
                     "found a ChannelEntry but no ChannelAccessEntry — HubState "
                     "invariant broken; returning INTERNAL_ERROR.",
                     channel_name, caller_uid);
        return make_error(corr_id, "INTERNAL_ERROR",
                          "ChannelAccessEntry missing for channel '" + channel_name + "'");
    }

    const bool authorized = access->ledger.admission_version_of(consumer_pubkey).has_value();

    nlohmann::json resp;
    resp["channel_name"] = channel_name;
    resp["consumer_pubkey"] = consumer_pubkey;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    // HEP-CORE-0032 §8.2 — broker ABI envelope echo on
    // CONSUMER_ATTACH_ACK_SHM.  Symmetric with REG_ACK path.
    resp["broker_abi_fingerprint"] =
        pylabhub::version::to_json_object(pylabhub::version::current());
    if (const char *bid = pylabhub::version::build_id())
    {
        resp["broker_build_id"] = bid;
    }

    if (authorized)
    {
        resp["status"] = "success";
        LOGGER_INFO("[broker] event=ConsumerAttachAuthorized channel='{}' "
                    "consumer_pubkey='{}' consumer_uid='{}' producer_uid='{}'",
                    channel_name, consumer_pubkey, consumer_role_uid, caller_uid);
    }
    else
    {
        resp["status"] = "denied";
        resp["denial_reason"] = "consumer_pubkey not in channel allowlist";
        LOGGER_INFO("[broker] event=ConsumerAttachDenied channel='{}' "
                    "consumer_pubkey='{}' consumer_uid='{}' producer_uid='{}' "
                    "reason='not_in_allowlist'",
                    channel_name, consumer_pubkey, consumer_role_uid, caller_uid);
    }
    return resp;
}

// ===========================================================================
// HEP-CORE-0042 §6.2 Bindings.ZMQ + §5.4/§5.5 handlers (Phase 2.x impl chain)
// ===========================================================================
//
// Authoritative status table lives in HEP-CORE-0042 §5.4 "Implementation
// status".  Summary as of 2026-07-01:
//   Phase 2.1 — envelope registration + dispatch + method skeleton.  ✅
//   Phase 2.2 — fast-path (steps 1-4) + stale-instance guard + §5.4
//               confirmed_version reset on re-registration / VoluntaryDereg /
//               kDead heartbeat + broker teardown-symmetry.  ✅
//   Phase 2.3a — wait-path enqueue + targeted §5.4 step 5b NOTIFY to P +
//                deferred-reply sentinel.  ✅
//   Phase 2.3b — APPLIED_REQ drain (step d) + producer-disconnect drain +
//                channel-close drain.  ✅ shipped + tested
//                (WaitPathEnqueueAndDrainOnAppliedReq / WaitPathDrainOn*).
//   Phase 2.3c — timeout sweep (§5.6 producer_apply_wait_ms) + L2/L3
//                queue-state tests.  ✅ shipped + tested
//                (WaitPathTimeoutOnMissingAppliedReq).

nlohmann::json BrokerServiceImpl::handle_consumer_attach_req_zmq(const nlohmann::json &req,
                                                                 zmq::socket_t &socket,
                                                                 const std::string &router_identity)
{
    // HEP-CORE-0042 §5.4 handler flow — Phase 2.3a fast-path + wait-path
    // enqueue.  Fast-path (steps 1-4) synchronously replies success/denied.
    // Wait-path (step 5) enqueues into pending_attach_queue_ + fires NOTIFY
    // + returns {status="pending"} as the sentinel the dispatcher special-
    // cases to skip send_reply.
    //
    // Wait-path drain branches (Phase 2.3b/c — shipped + tested):
    // - APPLIED_REQ drain (§5.4 step d): pops matching entries + sends the
    //   deferred success replies from handle_channel_auth_applied_req.
    // - Producer-disconnect drain (§5.4 producer-not-live): pops entries +
    //   sends denied replies from _on_producer_dropped / _on_pending_timeout.
    // - Channel-close drain (§5.4 channel_closing): pops entries + sends
    //   denied replies from the three teardown paths.
    // - Timeout sweep (§5.6 producer_apply_wait_ms, default 3000 ms):
    //   pops expired entries + sends timeout replies from sweep_pending_attach_timeouts_.
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    const std::string consumer_role_uid = req.value("consumer_role_uid", "");
    const std::string consumer_pubkey = req.value("consumer_pubkey", "");
    const std::string producer_role_uid = req.value("producer_role_uid", "");

    // Step 1: validate payload shape.
    if (channel_name.empty() || consumer_pubkey.empty() || consumer_role_uid.empty() ||
        producer_role_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_ATTACH_REQ_ZMQ requires non-empty "
                          "channel_name, consumer_pubkey, "
                          "consumer_role_uid, and producer_role_uid");
    }

    // HEP-CORE-0032 §8 — log consumer's ABI fingerprint on ATTACH ingest.
    // Log-only (same rationale as SHM handler: consumer already passed
    // strict-mode REG_REQ if configured).
    (void)log_peer_abi_fingerprint(req, consumer_role_uid, "ConsumerAttachAbiReceived",
                                   "ConsumerAttachAbiDetail",
                                   /*strict_mode=*/false,
                                   /*transport=*/"zmq");

    auto make_denied = [&](const std::string &reason)
    {
        nlohmann::json resp;
        resp["status"] = "denied";
        resp["reason"] = reason;
        resp["channel_name"] = channel_name;
        resp["producer_role_uid"] = producer_role_uid;
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        return resp;
    };

    // Step 2: consumer's pubkey must be in the channel allowlist —
    // BUT ONLY under fan-out / one-to-one, where the consumer is the
    // DIALING side and therefore needs an admission entry the binding
    // producer's ZAP will honour.  Under FAN-IN the consumer IS the
    // binding side (it owns the ZAP allowlist itself); its own pubkey
    // is not — and MUST NOT be — in the channel's admitted-set (which
    // under fan-in holds the dialing-side producer pubkeys, per the
    // topology-agnostic ledger contract at hub_state.hpp).  Applying
    // the fan-out admission check under fan-in would always deny
    // `consumer_not_in_channel_allowlist` — pinned by
    // `Pattern4AttachCoordinationTest.WaitPathDrainOnProducerDisconnect`
    // which exercises the wait-path drain of ATTACH_REQ_ZMQ enqueue-
    // then-DEREG under fan-in.
    auto access = hub_state_->channel_access(channel_name);
    if (!access.has_value())
    {
        // No access record for the channel is functionally the same as
        // "consumer not in allowlist" for the caller — deny per §5.4.
        return make_denied("consumer_not_in_channel_allowlist");
    }
    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
        return make_denied("producer_not_live");
    // Under a consumer-owned topology the attaching consumer IS the
    // binding owner — its own pubkey is not in its admission ledger
    // (the ledger holds the DIALING side's keys), so the allowlist
    // check applies only where the consumer dials.
    const bool consumer_is_owner = pylabhub::hub::topology::is_owner(
        ch->topology, pylabhub::hub::topology::AdmissionSide::Consumer);
    if (!consumer_is_owner && !access->ledger.admission_version_of(consumer_pubkey).has_value())
    {
        return make_denied("consumer_not_in_channel_allowlist");
    }

    // Step 3: producer P must be kLive on channel K.  ProducerEntry does
    // not carry an inline state field — liveness is inferred from the
    // producer's RolePresence.  On disconnect + reap, the producer entry
    // is removed from ch->producers[] atomically per HEP-CORE-0023 §2.1;
    // therefore "producer is still in ch->producers" is the coarse-grained
    // signal used across the broker (mirrors SHM's
    // handle_consumer_attach_req_shm producer-authorization check).  For
    // presence-state-aware "Connected+first_heartbeat_seen" gating (finer
    // than reap-based), a future phase may wire in a snapshot-lookup pattern
    // for finer producer-liveness gating.
    bool producer_registered = false;
    std::string producer_zmq_identity;
    for (const auto &prod : ch->producers)
    {
        if (prod.role_uid == producer_role_uid)
        {
            producer_registered = true;
            // Captured for the §5.4 step 5b targeted NOTIFY below.  May be
            // empty for pre-REG_ACK / partial-state producers; the wait-path
            // handles the empty case explicitly (see the send site).
            producer_zmq_identity = prod.zmq_identity;
            break;
        }
    }
    if (!producer_registered)
        return make_denied("producer_not_live");

    // Step 4: fast-path check — is producer P caught up to the current
    // allowlist version?
    const std::uint64_t confirmed =
        access->ledger.confirmed_version_of(producer_role_uid).value_or(0);
    const std::uint64_t channel_version = access->ledger.current_version();

    if (confirmed >= channel_version)
    {
        LOGGER_DEBUG("[broker] event=AttachReqZmqFastPath channel='{}' "
                     "producer_uid='{}' consumer_uid='{}' confirmed_version={} "
                     "channel_version={} (HEP-CORE-0042 §5.4 fast-path admit)",
                     channel_name, producer_role_uid, consumer_role_uid, confirmed,
                     channel_version);
        nlohmann::json resp;
        resp["status"] = "success";
        resp["channel_name"] = channel_name;
        resp["producer_role_uid"] = producer_role_uid;
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        // HEP-CORE-0032 §8.2 — broker ABI envelope echo on
        // CONSUMER_ATTACH_ACK_ZMQ (fast-path success).
        resp["broker_abi_fingerprint"] =
            pylabhub::version::to_json_object(pylabhub::version::current());
        if (const char *bid = pylabhub::version::build_id())
        {
            resp["broker_build_id"] = bid;
        }
        return resp;
    }

    // Step 5 wait-path (HEP-CORE-0042 §5.4 step 5, Phase 2.3a shipped).
    //
    // Enqueue this REQ into pending_attach_queue_[K][P] with
    // target_version = channel_version[K] snapshotted at this moment.
    // The drain condition on APPLIED_REQ arrival (Phase 2.3b) is
    // confirmed_version[K][P] >= target_version.  Fire
    // CHANNEL_AUTH_CHANGED_NOTIFY as a TARGETED doorbell to producer P
    // only (§5.4 step 5b — "Fire CHANNEL_AUTH_CHANGED_NOTIFY to P.
    // Always fire — no debounce.").  Do NOT reuse the fan-out helper
    // fire_channel_auth_changed_notify() here — that helper is used by
    // REG / DEREG allowlist mutations which legitimately affect every
    // producer's cache; the wait-path only needs P's cache poked
    // because only P is behind.  Fanning to all producers on wait-path
    // miss would cause spurious CHANNEL_AUTH_APPLIED_REQ traffic from
    // every other P' that is already caught up (correctness-safe, but
    // waste under channels with many producers).
    //
    // Return the {status="pending"} sentinel so the dispatcher skips
    // sending an immediate reply — the reply will be sent later by
    // whichever drain path (APPLIED_REQ / producer-disconnect /
    // channel-close / timeout) claims this entry.
    //
    // Enqueue captures router_identity + corr_id so the deferred
    // reply can be addressed back to the originating consumer.  The
    // producer-uid keyed inner map ensures FIFO ordering per (K, P)
    // — the drain per §5.4 step d walks the deque in order.
    PendingAttachEntry entry;
    entry.router_identity = router_identity;
    entry.correlation_id = corr_id;
    entry.consumer_pubkey = consumer_pubkey;
    entry.consumer_role_uid = consumer_role_uid;
    entry.target_version = channel_version;
    entry.enqueued_at = std::chrono::steady_clock::now();
    pending_attach_queue_[channel_name][producer_role_uid].push_back(std::move(entry));

    LOGGER_INFO("[broker] event=AttachReqZmqEnqueued channel='{}' producer_uid='{}' "
                "consumer_uid='{}' consumer_pubkey='{}' target_version={} "
                "confirmed_version={} queue_depth={} (HEP-CORE-0042 §5.4 wait-path; "
                "firing CHANNEL_AUTH_CHANGED_NOTIFY doorbell)",
                channel_name, producer_role_uid, consumer_role_uid, consumer_pubkey,
                channel_version, confirmed,
                pending_attach_queue_[channel_name][producer_role_uid].size());

    // Targeted doorbell to producer P only — HEP-CORE-0042 §5.4 step 5b.
    // If P's zmq_identity was empty at step 3 capture time (pre-REG_ACK /
    // partial state), skip the send but keep the enqueued entry: P will
    // still eventually issue APPLIED_REQ when its REG_ACK completes and
    // it pulls the initial_allowlist via the REG_ACK path.  Wire shape
    // matches HEP-CORE-0036 §6.5 CHANNEL_AUTH_CHANGED_NOTIFY.  This targeted
    // wait-path fire carries phase="admitted" (same fields as the fan-out
    // helper) and does NOT add a distinguishing `reason` field.
    if (!producer_zmq_identity.empty())
    {
        // Targeted phase=admitted doorbell to producer P — HEP-CORE-0042
        // §5.4 step 5b.  Payload shape matches the fan-out helper per
        // HEP-CORE-0007 lines 1840-1849.  role_uid/role_type describe
        // the dialing-side role whose admission drove the allowlist
        // bump (the consumer that just registered).
        nlohmann::json notify;
        notify["channel_name"] = channel_name;
        notify["channel_version"] = channel_version;
        notify["role_uid"] = consumer_role_uid;
        notify["role_type"] = "consumer";
        notify["phase"] = "admitted";
        send_to_identity(socket, producer_zmq_identity, "CHANNEL_AUTH_CHANGED_NOTIFY", notify);
        LOGGER_DEBUG("[broker] event=AttachWaitPathNotifyTargeted channel='{}' "
                     "producer_uid='{}' consumer_uid='{}' (HEP-CORE-0042 §5.4 "
                     "step 5b singular-P)",
                     channel_name, producer_role_uid, consumer_role_uid);
    }
    else
    {
        LOGGER_WARN("[broker] event=AttachWaitPathNotifySkipped channel='{}' "
                    "producer_uid='{}' (empty zmq_identity — producer in pre-REG_ACK "
                    "or partial state; producer will pick up allowlist via REG_ACK "
                    "initial_allowlist path and issue APPLIED_REQ when caught up)",
                    channel_name, producer_role_uid);
    }

    // Sentinel — dispatcher sees status="pending" and does NOT
    // send_reply().  The producer-side APPLIED_REQ (or the timeout
    // sweep) will drain this entry and send the reply with the
    // captured router_identity + corr_id.
    nlohmann::json resp;
    resp["status"] = "pending";
    resp["channel_name"] = channel_name;
    resp["producer_role_uid"] = producer_role_uid;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  Gates already ran in receive_and_validate.
nlohmann::json BrokerServiceImpl::handle_channel_auth_applied_req(
    const ::pylabhub::wire::WireEnvelope &env,
    const ::pylabhub::wire::ChannelAuthAppliedReqBody &body, zmq::socket_t &socket)
{
    // HEP-CORE-0042 §5.4 APPLIED_REQ handler.  Implements step (a)
    // stale-instance guard, step (b) reply, step (c) confirmed_version
    // advance, and step (d) queue drain — walks
    // pending_attach_queue_[K][P] after advancing confirmed_version and
    // sends deferred CONSUMER_ATTACH_ACK_ZMQ{status="success"} to each
    // entry whose target_version has been surpassed.  See
    // docs/HEP/HEP-CORE-0042 §5.4 "Implementation status" for the
    // authoritative phase mapping.
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    // HEP-CORE-0042 §5.5.2: the broker discriminates its producer vs consumer
    // branch on `role_type` — REQUIRED "producer" | "consumer" (the ctor
    // enforces presence; the amendment-era absent→"producer" default and the
    // `producer_role_uid` wire alias are retired).  `role_uid` is the
    // applier's identity.
    const std::string role_type = body.role_type();
    const std::string role_uid = body.role_uid();
    const std::uint64_t incoming_instance = body.instance_id();
    const std::uint64_t applied_version = body.applied_version();

    if (channel_name.empty() || role_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CHANNEL_AUTH_APPLIED_REQ requires non-empty "
                          "channel_name and role_uid");
    }

    // HEP-CORE-0042 §5.5.2 registration guard (Agent-2 review finding
    // 2026-07-13).  The wire's `role_uid` is client-supplied — any
    // authenticated peer could otherwise poison ANOTHER role's
    // confirmed_version and, combined with the ledger clamp, still
    // achieve a "confirm at max issued version" for a role that has
    // done no work.  Verify via HubState's shared registration primitive
    // that `role_uid` is currently registered on `channel_name` in the
    // appropriate role_type before advancing the ledger.  A wire that
    // identifies a channel/role pair the broker has no admission
    // record for is a wire-protocol violation, not a transient race —
    // hard-error the request.
    if (!hub_state_->channel(channel_name).has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "CHANNEL_AUTH_APPLIED_REQ for channel '" + channel_name +
                              "' — no such channel");
    }
    if (!hub_state_->is_role_registered_on_channel(channel_name, role_uid, role_type))
    {
        LOGGER_WARN("[broker] event=ChannelAuthAppliedNotARoleOfChannel "
                    "channel='{}' role_uid='{}' role_type='{}' "
                    "(HEP-CORE-0042 §5.5.2 registration guard — rejecting to "
                    "prevent cross-role confirmed_version poisoning)",
                    channel_name, role_uid, role_type);
        return make_error(corr_id, "NOT_A_ROLE_OF_CHANNEL",
                          "CHANNEL_AUTH_APPLIED_REQ role_uid='" + role_uid +
                              "' is not a registered " + role_type + " on channel '" +
                              channel_name + "'");
    }

    if (role_type == "consumer")
    {
        // HEP-CORE-0042 §5.5.2 consumer (binding-side) branch (unified
        // 2026-07-13 per INVARIANT-BIND-CONFIRM-1).  Advance this role's
        // confirmed_version in the ledger — same primitive the fan-out
        // producer branch below uses.  The wire's `applied_version` is
        // the sole source of truth for what the consumer installed;
        // broker MUST NOT snapshot its own current state (the retired
        // `_on_binding_confirmed` did exactly that, causing the test
        // #2480 race: broker over-confirmed pubkeys admitted after the
        // consumer's APPLIED_REQ was sent but before broker processed
        // it, letting producer-B's premature dial DENY at the
        // consumer's ZAP).  The ledger's `confirm()` also CLAMPS
        // applied_version at `current_version_` — see
        // versioned_admission_ledger.hpp INVARIANT-BIND-CONFIRM-1
        // (Agent-1 bound guard).
        const std::uint64_t new_confirmed =
            hub_state_->_on_role_confirmed(channel_name, role_uid, applied_version);

        LOGGER_INFO("[broker] event=ChannelAuthAppliedConsumer channel='{}' "
                    "consumer_uid='{}' applied_version={} confirmed_version={} "
                    "(HEP-CORE-0042 §5.5.2 unified — ledger.confirm advanced)",
                    channel_name, role_uid, applied_version, new_confirmed);

        nlohmann::json resp;
        resp["status"] = "ok";
        resp["channel_name"] = channel_name;
        resp["applied_version"] = new_confirmed;
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        return resp;
    }

    if (role_type != "producer")
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CHANNEL_AUTH_APPLIED_REQ role_type must be 'producer' "
                          "or 'consumer'");
    }

    // §5.4 step (a): stale-instance guard.  If the echoed instance_id
    // does not match the current instance[P], the message was minted by
    // a previous registration epoch — reply ERROR STALE_INSTANCE and
    // touch NO state (ratified 2026-07-24, HEP-0042 §5.4).  The reply
    // is deliberate: a LIVE producer whose own re-REG raced its
    // in-flight APPLIED_REQ needs the named error instead of an ack-
    // timeout stall, while a dead sender's reply is harmless (unroutable
    // → ROUTER discards; delivered to the successor → its correlation_id
    // matches nothing and the BRC drops it).
    const std::uint64_t current_instance = hub_state_->producer_instance(role_uid);
    if (incoming_instance != current_instance)
    {
        LOGGER_WARN("[broker] event=ChannelAuthAppliedStaleInstance channel='{}' "
                    "producer_uid='{}' incoming_instance={} current_instance={} "
                    "applied_version={} (HEP-CORE-0042 §5.2 stale-instance guard)",
                    channel_name, role_uid, incoming_instance, current_instance, applied_version);
        return make_error(corr_id, "STALE_INSTANCE",
                          "CHANNEL_AUTH_APPLIED_REQ from stale producer "
                          "instance dropped per HEP-CORE-0042 §5.2");
    }

    // §5.4 step (c): confirmed_version[K][P] = max(current, applied_version).
    // Unified 2026-07-13 — same `_on_role_confirmed` primitive the
    // consumer branch uses.  Producer-side vs consumer-side is just
    // "which role_uid" — the ledger is role-agnostic.
    const std::uint64_t new_confirmed =
        hub_state_->_on_role_confirmed(channel_name, role_uid, applied_version);

    LOGGER_INFO("[broker] event=ChannelAuthApplied channel='{}' producer_uid='{}' "
                "instance={} applied_version={} confirmed_version={}",
                channel_name, role_uid, incoming_instance, applied_version, new_confirmed);

    // §5.4 step (d): drain pending_attach_queue_[K][P] of every entry
    // whose target_version has been surpassed by new_confirmed, and send
    // the deferred CONSUMER_ATTACH_ACK_ZMQ{status="success"} reply to
    // each drained entry.  Single-pumper ROUTER serialises drain vs.
    // reply; the drain-then-reply order below is intent-preserving per
    // §5.4 step d wording ("walk … and remove").
    drain_pending_attach_queue_for_producer_confirmed_(socket, channel_name, role_uid,
                                                       new_confirmed);

    // §5.4 step (b): reply {status="ok", ...}.
    nlohmann::json resp;
    resp["status"] = "ok";
    resp["channel_name"] = channel_name;
    resp["applied_version"] = new_confirmed;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_check_peer_ready_req(const nlohmann::json &req,
                                                              const zmq::message_t &)
{
    // HEP-CORE-0036 §6.6.3 — dialing-side role's readiness pull.
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    const std::string role_uid = req.value("role_uid", "");
    const std::string pubkey_z85 = req.value("pubkey_z85", "");

    if (channel_name.empty() || role_uid.empty() || pubkey_z85.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CHECK_PEER_READY_REQ requires non-empty "
                          "channel_name, role_uid, and pubkey_z85");
    }

    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' does not exist");
    }

    // Authorization — caller must be a registered *dialing-side*
    // role of the channel per §6.6.3.  The dialing side is the mirror
    // of §6.6.1's binding-side rule:
    //   FanOut / OneToOne → dialing side is the consumer
    //   FanIn            → dialing side is the producer
    // A binding-side caller asking this question makes no operational
    // sense (they ARE the guard) — also rejected with the same code
    // so callers get one clear error rather than two.
    const bool consumer_binds = pylabhub::hub::Queue::reader_is_binding_side(ch->topology);
    auto match_uid = [&](const auto &entry) { return entry.role_uid == role_uid; };
    // Dialing side is the mirror of the binding side: consumer under
    // fan-out / one-to-one, producer under fan-in.  Branch on
    // topology to avoid ternary over different container element
    // types (ProducerEntry vs ConsumerEntry).
    const bool caller_is_dialing_side =
        consumer_binds ? std::any_of(ch->producers.begin(), ch->producers.end(), match_uid)
                       : std::any_of(ch->consumers.begin(), ch->consumers.end(), match_uid);
    if (!caller_is_dialing_side)
    {
        LOGGER_WARN("Broker: CHECK_PEER_READY_REQ rejected — role_uid='{}' is not "
                    "the dialing-side role of channel '{}' (topology={}, expected "
                    "{} to dial) (HEP-CORE-0036 §6.6.3)",
                    role_uid, channel_name, static_cast<int>(ch->topology),
                    consumer_binds ? "producer" : "consumer");
        return make_error(corr_id, "NOT_A_ROLE_OF_CHANNEL",
                          "Caller role_uid='" + role_uid +
                              "' is not the dialing-side role of channel '" + channel_name +
                              "' (topology requires " +
                              std::string(consumer_binds ? "producer" : "consumer") + " to dial)");
    }

    // HEP-CORE-0042 §5.5.2 INVARIANT-BIND-CONFIRM-2 — visibility query
    // against the unified ledger.  The binding-side role is singular
    // per channel (one consumer under fan-in / one-to-one; one producer
    // under fan-out); resolve its uid from the topology to key the
    // per-role confirmation lookup.
    nlohmann::json resp;
    resp["channel_name"] = channel_name;
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    std::string binding_role_uid;
    if (consumer_binds)
    {
        if (!ch->consumers.empty())
        {
            binding_role_uid = ch->consumers.front().role_uid;
        }
    }
    else
    {
        if (!ch->producers.empty())
        {
            binding_role_uid = ch->producers.front().role_uid;
        }
    }
    if (binding_role_uid.empty())
    {
        LOGGER_WARN("Broker: CHECK_PEER_READY_REQ for channel '{}' — no "
                    "binding-side role present yet (consumer_binds={}); "
                    "responding not_ready (HEP-CORE-0042 §5.5.2)",
                    channel_name, consumer_binds);
        resp["status"] = "not_ready";
        resp["reason"] = "not_confirmed";
        return resp;
    }

    const auto ready_opt =
        hub_state_->is_pubkey_visible_to(channel_name, binding_role_uid, pubkey_z85);

    if (!ready_opt.has_value())
    {
        // Either ChannelAccessEntry is missing (HubState invariant
        // broken) OR the binding role has never confirmed anything yet
        // (INVARIANT-BIND-CONFIRM-2: nullopt distinguishes "role has
        // not applied anything" from the concrete false answer).
        auto access = hub_state_->channel_access(channel_name);
        if (!access.has_value())
        {
            LOGGER_ERROR("Broker: CHECK_PEER_READY_REQ for channel '{}' from "
                         "role_uid='{}' found a ChannelEntry but no "
                         "ChannelAccessEntry — HubState invariant may be broken "
                         "(REG path should wire `_on_channel_access_opened` on "
                         "every channel admission).  Responding not_ready "
                         "(fail-closed on wire).",
                         channel_name, role_uid);
            resp["status"] = "not_ready";
            resp["reason"] = "not_admitted";
            return resp;
        }
        // Access record exists; binding role just hasn't confirmed yet.
        resp["status"] = "not_ready";
        resp["reason"] = "not_confirmed";
        return resp;
    }

    if (*ready_opt)
    {
        resp["status"] = "ready";
        return resp;
    }

    // Distinguish "admitted but binding-side has not caught up in
    // confirmed_version" from "not admitted at all," so operators
    // diagnosing startup can tell which end is behind.  Both surface
    // as not_ready on the wire, differ only in `reason`.
    auto access = hub_state_->channel_access(channel_name);
    const bool admitted =
        access.has_value() && access->ledger.admission_version_of(pubkey_z85).has_value();
    resp["status"] = "not_ready";
    resp["reason"] = admitted ? "not_confirmed" : "not_admitted";
    return resp;
}

std::size_t BrokerServiceImpl::drain_pending_attach_queue_for_producer_confirmed_(
    zmq::socket_t &socket, const std::string &channel_name, const std::string &producer_role_uid,
    std::uint64_t new_confirmed)
{
    // HEP-CORE-0042 §5.4 step d — drain success replies.  Walks the
    // (K, P) deque IN ORDER; per §5.4 step d "walk pending_attach_queue:
    // for entries with target_version ≤ confirmed_version[K][P], reply
    // {status="success"} and remove from queue."  Because
    // channel_version is monotonic and target_version is snapshotted at
    // enqueue time, all drainable entries sit at the front of the deque
    // — we stop at the first entry whose target_version is still
    // strictly greater than new_confirmed.
    auto ch_it = pending_attach_queue_.find(channel_name);
    if (ch_it == pending_attach_queue_.end())
        return 0;
    auto p_it = ch_it->second.find(producer_role_uid);
    if (p_it == ch_it->second.end())
        return 0;

    auto &queue = p_it->second;
    std::size_t drained = 0;
    while (!queue.empty() && queue.front().target_version <= new_confirmed)
    {
        const auto &entry = queue.front();
        nlohmann::json reply;
        reply["status"] = "success";
        reply["channel_name"] = channel_name;
        reply["producer_role_uid"] = producer_role_uid;
        if (!entry.correlation_id.empty())
            reply["correlation_id"] = entry.correlation_id;
        send_to_identity(socket, entry.router_identity, "CONSUMER_ATTACH_ACK_ZMQ", reply);
        queue.pop_front();
        ++drained;
    }

    if (queue.empty())
    {
        ch_it->second.erase(p_it);
        if (ch_it->second.empty())
            pending_attach_queue_.erase(ch_it);
    }

    if (drained > 0)
    {
        LOGGER_INFO("[broker] event=AttachQueueDrainedSuccess channel='{}' "
                    "producer_uid='{}' drained_count={} new_confirmed_version={} "
                    "(HEP-CORE-0042 §5.4 step d)",
                    channel_name, producer_role_uid, drained, new_confirmed);
    }
    return drained;
}

std::size_t BrokerServiceImpl::drain_pending_attach_queue_for_producer_denied_(
    zmq::socket_t &socket, const std::string &channel_name, const std::string &producer_role_uid,
    const std::string &reason)
{
    // HEP-CORE-0042 §5.4 producer-disconnect drain.  Empties the entire
    // (K, P) deque and sends a denied reply for each entry.  Used by
    // handle_dereg_req non-last-producer + check_heartbeat_timeouts
    // non-last-producer (reason="producer_not_live") as the pair to the
    // §5.4 confirmed_version reset on producer disconnect.
    auto ch_it = pending_attach_queue_.find(channel_name);
    if (ch_it == pending_attach_queue_.end())
        return 0;
    auto p_it = ch_it->second.find(producer_role_uid);
    if (p_it == ch_it->second.end())
        return 0;

    auto &queue = p_it->second;
    std::size_t drained = 0;
    while (!queue.empty())
    {
        const auto &entry = queue.front();
        nlohmann::json reply;
        reply["status"] = "denied";
        reply["reason"] = reason;
        reply["channel_name"] = channel_name;
        reply["producer_role_uid"] = producer_role_uid;
        if (!entry.correlation_id.empty())
            reply["correlation_id"] = entry.correlation_id;
        send_to_identity(socket, entry.router_identity, "CONSUMER_ATTACH_ACK_ZMQ", reply);
        queue.pop_front();
        ++drained;
    }

    ch_it->second.erase(p_it);
    if (ch_it->second.empty())
        pending_attach_queue_.erase(ch_it);

    if (drained > 0)
    {
        LOGGER_INFO("[broker] event=AttachQueueDrainedDenied channel='{}' "
                    "producer_uid='{}' reason='{}' drained_count={} "
                    "(HEP-CORE-0042 §5.4 producer-disconnect drain)",
                    channel_name, producer_role_uid, reason, drained);
    }
    return drained;
}

std::size_t BrokerServiceImpl::drain_pending_attach_queue_for_channel_denied_(
    zmq::socket_t &socket, const std::string &channel_name, const std::string &reason)
{
    // HEP-CORE-0042 §5.4 channel-close drain.  Empties every producer's
    // deque for the named channel and sends a denied reply for each
    // entry.  Used by the three broker teardown paths that call
    // _on_channel_access_closed (reason="channel_closing"): last-producer
    // DEREG_REQ, last-producer heartbeat-timeout, script-requested close.
    auto ch_it = pending_attach_queue_.find(channel_name);
    if (ch_it == pending_attach_queue_.end())
        return 0;

    std::size_t drained = 0;
    for (auto &kv : ch_it->second)
    {
        const std::string &producer_uid = kv.first;
        auto &queue = kv.second;
        while (!queue.empty())
        {
            const auto &entry = queue.front();
            nlohmann::json reply;
            reply["status"] = "denied";
            reply["reason"] = reason;
            reply["channel_name"] = channel_name;
            reply["producer_role_uid"] = producer_uid;
            if (!entry.correlation_id.empty())
                reply["correlation_id"] = entry.correlation_id;
            send_to_identity(socket, entry.router_identity, "CONSUMER_ATTACH_ACK_ZMQ", reply);
            queue.pop_front();
            ++drained;
        }
    }
    pending_attach_queue_.erase(ch_it);

    if (drained > 0)
    {
        LOGGER_INFO("[broker] event=AttachQueueDrainedDeniedChannel channel='{}' "
                    "reason='{}' drained_count={} "
                    "(HEP-CORE-0042 §5.4 channel-close drain)",
                    channel_name, reason, drained);
    }
    return drained;
}

std::size_t
BrokerServiceImpl::sweep_pending_attach_timeouts_(zmq::socket_t &socket,
                                                  std::chrono::steady_clock::time_point now,
                                                  std::chrono::milliseconds budget)
{
    // HEP-CORE-0042 §5.4 pending-entry timeout + §5.6
    // producer_apply_wait_ms sweep.  Walks every (K, P) deque; pops
    // front entries while their age exceeds the budget.  Because
    // entries are enqueued in monotonic wall order and `enqueued_at`
    // is captured under the single-pumper, the deque's front is
    // always the oldest — front-only stop-early check is correct
    // (parallel to the drain_pending_attach_queue_for_producer_confirmed_
    // monotonic-target_version invariant).
    //
    // Total O(K * P + drained).  For thousands of channels this is a
    // linear scan on the sweep cadence; acceptable per §4 P1 (queue
    // growth accepted-degradation), and Phase 2.3c's coarse-grained
    // sweep is intentionally piggybacked on check_heartbeat_timeouts
    // instead of introducing a new timer thread (HEP-CORE-0031
    // spawn_bounded #283 lands the finer-grained primitive later).
    std::size_t total_drained = 0;

    for (auto ch_it = pending_attach_queue_.begin(); ch_it != pending_attach_queue_.end();)
    {
        for (auto p_it = ch_it->second.begin(); p_it != ch_it->second.end();)
        {
            auto &queue = p_it->second;
            std::size_t drained_this_pair = 0;
            while (!queue.empty() && (now - queue.front().enqueued_at) > budget)
            {
                const auto &entry = queue.front();
                nlohmann::json reply;
                reply["status"] = "timeout";
                reply["reason"] = "producer_did_not_confirm_within_budget";
                reply["channel_name"] = ch_it->first;
                reply["producer_role_uid"] = p_it->first;
                if (!entry.correlation_id.empty())
                    reply["correlation_id"] = entry.correlation_id;
                send_to_identity(socket, entry.router_identity, "CONSUMER_ATTACH_ACK_ZMQ", reply);
                queue.pop_front();
                ++drained_this_pair;
                ++total_drained;
            }

            if (drained_this_pair > 0)
            {
                LOGGER_WARN("[broker] event=AttachQueueDrainedTimeout channel='{}' "
                            "producer_uid='{}' drained_count={} budget_ms={} "
                            "(HEP-CORE-0042 §5.4 pending-entry timeout — producer "
                            "did not APPLIED_REQ within budget)",
                            ch_it->first, p_it->first, drained_this_pair,
                            static_cast<long long>(budget.count()));
            }

            if (queue.empty())
                p_it = ch_it->second.erase(p_it);
            else
                ++p_it;
        }
        if (ch_it->second.empty())
            ch_it = pending_attach_queue_.erase(ch_it);
        else
            ++ch_it;
    }
    return total_drained;
}

void BrokerServiceImpl::fire_channel_auth_changed_notify(zmq::socket_t &socket,
                                                         const std::string &channel_name,
                                                         const std::string &phase,
                                                         const std::string &role_uid,
                                                         const std::string &role_type)
{
    // HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_NOTIFY (lines 1803-1864;
    // topology migration 2026-07-08) + HEP-CORE-0036 §6.5.  Fan a
    // fire-and-forget doorbell to the BINDING side of the channel:
    // fan-in → consumers, fan-out / one-to-one → producers (line
    // 1806).  Same threading as CHANNEL_CLOSING_NOTIFY /
    // CONSUMER_DIED_NOTIFY.  Peers without a captured ZMQ identity
    // (legacy or partial-state entries) are skipped: no transport
    // reaches them; recovery is via REG_ACK on reconnect.
    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        // Caller (REG / DEREG / heartbeat / heartbeat-timeout
        // handler) just mutated the channel state; the channel
        // SHOULD exist.  A missing channel here means the lookup
        // races with a concurrent teardown — observable but harmless
        // (binding-side peers who were live during the mutation will
        // pick up the change via REG_ACK.initial_allowlist on the
        // next reconnect).
        LOGGER_WARN("Broker: CHANNEL_AUTH_CHANGED_NOTIFY fan-out skipped — "
                    "channel '{}' no longer exists in HubState (probable race "
                    "with concurrent channel teardown; phase='{}' role_uid='{}' "
                    "role_type='{}').",
                    channel_name, phase, role_uid, role_type);
        return;
    }
    // Payload per HEP-CORE-0007 lines 1840-1849.  channel_version:
    // bumped on phase=admitted / phase=left; unchanged on phase=live.
    // Sourced from the ledger on ChannelAccessEntry (HEP-CORE-0042
    // §5.5.2 unified 2026-07-13) — the single authority for per-channel
    // admission version.  The pre-2026-07-13 code read the dead
    // ChannelEntry.channel_version field which was never written and
    // shipped 0 to every subscriber.
    nlohmann::json notify;
    notify["channel_name"] = channel_name;
    if (auto access = hub_state_->channel_access(channel_name); access.has_value())
    {
        notify["channel_version"] = access->ledger.current_version();
    }
    else
    {
        notify["channel_version"] = std::uint64_t{0};
    }
    notify["role_uid"] = role_uid;
    notify["role_type"] = role_type;
    notify["phase"] = phase;

    // Fan-out target dispatch (HEP-CORE-0007 line 1806).  Fan-in's
    // binding side is the consumer; fan-out / one-to-one's binding
    // side is the producer.  Under the singular-side ownership
    // model, the binding-side collection has size 1 in steady state,
    // but iteration handles the transient overlap window.
    std::size_t fanned = 0;
    std::size_t skipped_no_identity = 0;
    auto fan_to = [&](const std::string &identity)
    {
        if (identity.empty())
        {
            ++skipped_no_identity;
            return;
        }
        // ZMQ identities are binary — hex-encode so the log is
        // grep-friendly and byte-exact against the BRC-side capture.
        std::string identity_hex;
        identity_hex.reserve(identity.size() * 2);
        constexpr const char kHex[] = "0123456789abcdef";
        for (unsigned char b : identity)
        {
            identity_hex.push_back(kHex[(b >> 4) & 0xF]);
            identity_hex.push_back(kHex[b & 0xF]);
        }
        LOGGER_INFO("[broker] event=ChannelAuthChangedNotifyFanning "
                    "channel='{}' phase='{}' role_uid='{}' role_type='{}' "
                    "target_identity_hex='{}' target_identity_bytes={}",
                    channel_name, phase, role_uid, role_type, identity_hex, identity.size());
        send_to_identity(socket, identity, "CHANNEL_AUTH_CHANGED_NOTIFY", notify);
        ++fanned;
    };
    std::size_t binding_side_total = 0;
    if (pylabhub::hub::topology::binding_side(ch->topology) ==
        pylabhub::hub::topology::AdmissionSide::Consumer)
    {
        binding_side_total = ch->consumers.size();
        for (const auto &cons : ch->consumers)
            fan_to(cons.zmq_identity);
    }
    else
    {
        binding_side_total = ch->producers.size();
        for (const auto &prod : ch->producers)
            fan_to(prod.zmq_identity);
    }
    if (skipped_no_identity > 0)
    {
        // Identity-less peers are a transient state (pre-REG_ACK
        // capture, or post-teardown partial entries).  Reveal them so
        // operators can investigate whether a peer is stuck not
        // receiving notifies — which would degrade to "stale allowlist
        // until peer's next reconnect" per the §6.5 drift window.
        LOGGER_WARN("Broker: CHANNEL_AUTH_CHANGED_NOTIFY for channel '{}' "
                    "skipped {} binding-side peer(s) with empty zmq_identity "
                    "(transient or partial state); fanned to {} peer(s).",
                    channel_name, skipped_no_identity, fanned);
    }
    LOGGER_DEBUG("Broker: CHANNEL_AUTH_CHANGED_NOTIFY fan-out for "
                 "channel '{}' phase='{}' role_uid='{}' role_type='{}' "
                 "to {} of {} binding-side peer(s)",
                 channel_name, phase, role_uid, role_type, fanned, binding_side_total);
}

// #74 — objective peer counts (DRAFT_objective_peer_counts_2026-07-26).
// LIVE means Connected + first_heartbeat_seen (HEP-CORE-0036 §3.5.2) — the wire
// is ready to deliver.  This is NOT ChannelEntry's registered .size().
std::pair<std::uint32_t, std::uint32_t>
BrokerServiceImpl::compute_channel_live_counts(const std::string &channel) const
{
    const auto snap = hub_state_->snapshot();
    const auto cit = snap.channels.find(channel);
    if (cit == snap.channels.end())
        return {0u, 0u};
    std::uint32_t live_p = 0, live_c = 0;
    for (const auto &p : cit->second.producers)
    {
        auto rit = snap.roles.find(p.role_uid);
        if (rit == snap.roles.end())
            continue;
        const auto *pp = rit->second.find_presence(channel, "producer");
        if (pp != nullptr && pp->state == pylabhub::hub::RoleState::Connected &&
            pp->first_heartbeat_seen)
            ++live_p;
    }
    for (const auto &c : cit->second.consumers)
    {
        auto rit = snap.roles.find(c.role_uid);
        if (rit == snap.roles.end())
            continue;
        const auto *pp = rit->second.find_presence(channel, "consumer");
        if (pp != nullptr && pp->state == pylabhub::hub::RoleState::Connected &&
            pp->first_heartbeat_seen)
            ++live_c;
    }
    return {live_p, live_c};
}

// #74 — fan the objective count to EVERY member of the channel (both sides).
// Channel-level status (a number, no per-peer identity): the dialing side
// receives only this, never the CHANNEL_AUTH_CHANGED_NOTIFY identity stream.
void BrokerServiceImpl::fire_channel_count_notify(zmq::socket_t &socket, const std::string &channel)
{
    auto ch = hub_state_->channel(channel);
    if (!ch.has_value())
        return;
    const auto [pc, cc] = compute_channel_live_counts(channel);
    nlohmann::json body;
    body["channel_name"] = channel;
    body["producer_count"] = pc;
    body["consumer_count"] = cc;
    std::size_t fanned = 0;
    for (const auto &p : ch->producers)
        if (!p.zmq_identity.empty())
        {
            send_to_identity(socket, p.zmq_identity, "CHANNEL_COUNT_NOTIFY", body);
            ++fanned;
        }
    for (const auto &c : ch->consumers)
        if (!c.zmq_identity.empty())
        {
            send_to_identity(socket, c.zmq_identity, "CHANNEL_COUNT_NOTIFY", body);
            ++fanned;
        }
    last_channel_counts_[channel] = {pc, cc};
    LOGGER_DEBUG("Broker: CHANNEL_COUNT_NOTIFY channel='{}' producers={} consumers={} "
                 "fanned to {} member(s) (#74)",
                 channel, pc, cc, fanned);
}

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  `env` is unused — HEARTBEAT_NOTIFY is fire-and-forget (no reply, no
// corr_id) — but carried for the uniform §14.4 signature.
//
// Design contract — where heartbeat validation lives (§14.7 rule 3: a handler
// trusts the shared guards and does NOT re-implement them):
//
//   1. `run_control_gates` (admission_gates.cpp) runs in `receive_and_validate`
//      BEFORE this handler and rejects a non-empty-but-invalid `role_uid` or
//      `channel_name` — including a `role_uid` this connection does not own,
//      which is what stops a forged heartbeat holding a dead role's presence
//      alive.  The gate list itself is documented at that function and is
//      deliberately NOT repeated here; a second copy would be one more thing
//      to update when the tier changes, and would say something false the
//      day it was missed.  Note it intentionally SKIPS empty fields
//      (`if (!field.empty())`).
//
//   2. `HubState::_on_heartbeat` (hub_state.cpp) is the AUTHORITATIVE state
//      guard: it no-ops on invalid-grammar channel/role_uid (bumping the
//      invalid-identifier counter), on a blank role_uid/role_type, and on an
//      unknown role or presence.  NOTHING blank or invalid mutates state,
//      regardless of what this handler does.
//
// So this handler performs NO validation of its own.  The only field checks it
// keeps are OBSERVABILITY, not gates: `_on_heartbeat` drops a blank mandatory
// field SILENTLY (no counter, no log), which would make a misconfigured client
// undiagnosable — so we log+return early on the two mandatory fields.  These are
// not load-bearing (the outcome is identical without them); they exist only so
// the drop is visible.  A blank channel_name needs no separate check — it can
// never match a channel key, so it falls through to the CHANNEL_NOT_FOUND path
// below.
void BrokerServiceImpl::handle_heartbeat_req(
    [[maybe_unused]] const ::pylabhub::wire::WireEnvelope &env,
    const ::pylabhub::wire::HeartbeatNotifyBody &body, zmq::socket_t &socket)
{
    const std::string channel_name = body.channel_name();
    // Peek existence + producer-presence state before applying the
    // heartbeat so we can log the Pending->Live channel-observable
    // transition (the actual mutation + counter bump happens inside
    // hub_state_->_on_heartbeat below).  Per HEP-CORE-0023 §2.2 the
    // channel observable is derived from the producer-presence FSM,
    // so a transition is only "channel-level" when this heartbeat is
    // the producer's; consumer heartbeats refresh their own presence
    // and never flip the channel observable.  A blank or unknown channel
    // both land here (a blank name is never a channel key).
    const auto snap = hub_state_->snapshot();
    const auto cit = snap.channels.find(channel_name);
    if (cit == snap.channels.end())
    {
        LOGGER_WARN("Broker: HEARTBEAT_NOTIFY for unknown/blank channel '{}'", channel_name);
        return;
    }
    // Channel observable is the BEST-of all producer-presences
    // (HEP-CORE-0023 §2.1.1).  `was_pending` is true iff NO producer
    // is currently Live — i.e., every registered producer-presence is
    // sub-Live (Pending or Connected without first_heartbeat_seen).
    bool was_pending = !cit->second.producers.empty();
    for (const auto &prod : cit->second.producers)
    {
        auto rit = snap.roles.find(prod.role_uid);
        if (rit == snap.roles.end())
            continue;
        const auto *pp = rit->second.find_presence(channel_name, "producer");
        if (pp == nullptr)
            continue;
        if (pp->state == pylabhub::hub::RoleState::Connected && pp->first_heartbeat_seen)
        {
            was_pending = false;
            break;
        }
    }
    // Blank mandatory-field visibility (NOT validation — see the function-head
    // contract).  `_on_heartbeat` below already no-ops a blank role_uid/role_type
    // (hub_state.cpp), so removing this changes no state or wire outcome; it is
    // kept ONLY so a misconfigured client that omits a mandatory field per
    // HEP-CORE-0019 §4.1 is diagnosable instead of vanishing into a silent no-op.
    const std::string wire_uid = body.role_uid();
    const std::string wire_role_type = body.role_type();
    LOGGER_DEBUG("Broker: HEARTBEAT_NOTIFY channel='{}' role_uid='{}' role_type='{}'", channel_name,
                 wire_uid, wire_role_type);

    if (wire_uid.empty() || wire_role_type.empty())
    {
        LOGGER_WARN("Broker: HEARTBEAT_NOTIFY for '{}' dropped — blank "
                    "'role_uid' or 'role_type' (HEP-CORE-0019 §4.1)",
                    channel_name);
        return;
    }

    // Producer-presence-sub-Live diagnostic: gate on `role_type ==
    // "producer"` so consumer heartbeats don't inflate log volume
    // (audit L3 closure).  The actual FSM transition + counter bump
    // happens inside `_on_heartbeat`; this is pre-emptive observability.
    if (was_pending && wire_role_type == "producer")
    {
        LOGGER_INFO("Broker: channel '{}' producer-presence sub-Live "
                    "(may transition to Live on this heartbeat)",
                    channel_name);
    }

    // `producer_pid` is NOT read here — it is a debug/record-only wire field
    // (machine-local; never an input to any broker decision).  The heartbeat's
    // authoritative key is `(role_uid, channel, role_type)`; a missing/zero pid
    // is not an error and never affects presence.  It is echoed in the metrics
    // log below purely for per-presence attribution.

    // Refresh the presence row keyed on `(role_uid, channel, role_type)`
    // per HEP-CORE-0023 §2.5.2 + HEP-CORE-0019 §2.3.  Each heartbeat
    // refreshes ONLY its own presence row.
    std::optional<nlohmann::json> metrics_opt;
    if (body.has_metrics())
        metrics_opt = body.metrics();

    const auto eff = hub_state_->_on_heartbeat(channel_name, wire_uid, wire_role_type,
                                               std::chrono::steady_clock::now(), metrics_opt);

    // Metrics observability (HEP-CORE-0019 §2.3): when a heartbeat carries
    // a metrics object, log the stored per-presence metrics so an operator
    // (and L3 tests that verify wire→storage) can observe them without
    // reaching into HubState.  Metrics heartbeats are periodic, not
    // per-tick, so this is bounded.  `producer_pid` is echoed (debug/record)
    // for per-presence attribution.
    if (metrics_opt.has_value())
    {
        LOGGER_INFO("[broker] event=HeartbeatMetricsStored channel='{}' "
                    "role_uid='{}' role_type='{}' producer_pid={} metrics={}",
                    channel_name, wire_uid, wire_role_type, body.producer_pid(),
                    metrics_opt->dump());
    }

    // HEP-CORE-0023 §2.5 telemetry — first-heartbeat observability.
    // One-shot per presence per session: gate
    // `!was_first_heartbeat_seen` is true on the SAME tick that
    // flipped `first_heartbeat_seen` to true inside `_on_heartbeat`.
    // Bounded by role count × channel count; never per-tick.  Logged
    // at the wire layer (here) rather than inside HubState because
    // HubState is a pure state machine; wire-protocol observability
    // belongs at the wire layer.
    //
    // HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_NOTIFY line 1819-1822: the
    // same first-heartbeat gate also drives `phase=live` — "broker
    // receives first heartbeat from a DIALING-side role" so binding
    // side's `live_peers` accessor reflects the new peer.  The gate
    // fires only for the DIALING side per topology; binding-side
    // roles are the notification target, not the trigger.
    if (eff.presence_found && !eff.was_first_heartbeat_seen)
    {
        LOGGER_INFO("Broker: first heartbeat received from role='{}' "
                    "channel='{}' role_type='{}'",
                    wire_uid, channel_name, wire_role_type);

        // Topology-role → is this the dialing side?  §4.7.0.3 truth
        // table: the dialing side is whoever is NOT the binding owner.
        // (Symmetric with the binding-side target dispatch in
        // fire_channel_auth_changed_notify.)
        auto ch = hub_state_->channel(channel_name);
        if (ch.has_value())
        {
            const auto side = wire_role_type == "producer"
                                  ? pylabhub::hub::topology::AdmissionSide::Producer
                                  : pylabhub::hub::topology::AdmissionSide::Consumer;
            const bool is_dialing_side = !pylabhub::hub::topology::is_owner(ch->topology, side);
            if (is_dialing_side)
            {
                fire_channel_auth_changed_notify(socket, channel_name,
                                                 /*phase=*/"live",
                                                 /*role_uid=*/wire_uid,
                                                 /*role_type=*/wire_role_type);
            }
            // #74 — a member just became live: refresh the objective count on
            // ALL members (both sides), any role_type.  Immediate here for low
            // join latency; the reconciler in check_heartbeat_timeouts catches
            // leaves.
            fire_channel_count_notify(socket, channel_name);
        }
    }
}

// ============================================================================
// ENDPOINT_UPDATE_REQ handler (HEP-0021 §16)
// ============================================================================

// HEP-CORE-0046 §12 step 5: typed handler on the validated envelope +
// body.  Gates already ran in receive_and_validate.
nlohmann::json
BrokerServiceImpl::handle_endpoint_update_req(const ::pylabhub::wire::WireEnvelope &env,
                                              const ::pylabhub::wire::EndpointUpdateReqBody &body)
{
    const std::string corr_id = std::string(env.correlation_id());
    const std::string channel_name = body.channel_name();
    const std::string endpoint_type = body.endpoint_type();
    const std::string endpoint = body.endpoint();

    if (channel_name.empty() || endpoint_type.empty() || endpoint.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "Missing channel_name, endpoint_type, or endpoint");
    }

    // Validate the new endpoint.
    auto ep_check = pylabhub::validate_tcp_endpoint(endpoint);
    if (!ep_check.ok() || ep_check.port == 0)
    {
        return make_error(corr_id, "INVALID_ENDPOINT",
                          "Endpoint '" + endpoint + "' is invalid or has port 0");
    }

    auto entry = hub_state_->channel(channel_name);
    if (!entry.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    // Resolve the sender's identity against the channel's roster.
    // Per HEP-CORE-0017 §3.3.0 the "binding side" that owns the
    // channel's data-plane endpoint depends on topology:
    //   fan-out / one-to-one → producer binds
    //   fan-in                → consumer binds
    // Identity-based resolution (matching sender's ZMTP identity to
    // stored `zmq_identity`) is more secure than trusting a wire
    // `role_uid`.  Both producer + consumer paths use the same
    // identity mechanism.  `env.identity()` is the authoritative ROUTER
    // frame-0 identity (HEP-CORE-0046 I-DEALER-IDENTITY) — same bytes the
    // legacy `identity` frame carried.
    const std::string sender_id(env.identity());
    std::string sender_role_uid;
    bool sender_is_consumer_binding = false;
    for (const auto &prod : entry->producers)
    {
        if (sender_id == prod.zmq_identity)
        {
            sender_role_uid = prod.role_uid;
            break;
        }
    }
    if (sender_role_uid.empty() &&
        pylabhub::hub::topology::is_owner(entry->topology,
                                          pylabhub::hub::topology::AdmissionSide::Consumer))
    {
        for (const auto &cons : entry->consumers)
        {
            if (sender_id == cons.zmq_identity)
            {
                sender_role_uid = cons.role_uid;
                sender_is_consumer_binding = true;
                break;
            }
        }
    }
    if (sender_role_uid.empty())
    {
        LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' rejected — "
                    "sender is not a registered producer or fan-in "
                    "consumer of the channel",
                    channel_name);
        return make_error(corr_id, "NOT_CHANNEL_OWNER",
                          "Sender is not the binding side of channel '" + channel_name + "'");
    }

    // Only `zmq_node` is mutable post-registration; reject everything else.
    if (endpoint_type == "inbox")
    {
        // Inbox endpoints must be resolved before REG_REQ — runtime update is not
        // supported.  If any producer's inbox port is 0, that's a registration
        // bug, not an update scenario.  HEP-CORE-0023 §2.1.1 + HEP-CORE-0027:
        // inbox lives per-ProducerEntry; scan each.
        for (const auto &prod : entry->producers)
        {
            if (prod.inbox_endpoint.empty())
                continue;
            auto inbox_check = pylabhub::validate_tcp_endpoint(prod.inbox_endpoint);
            if (inbox_check.ok() && inbox_check.port == 0)
            {
                LOGGER_ERROR("Broker: ENDPOINT_UPDATE_REQ for '{}' inbox (producer '{}') — "
                             "current port is 0; inbox endpoint should be resolved before "
                             "registration",
                             channel_name, prod.role_uid);
            }
        }
        LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' inbox rejected — "
                    "inbox endpoint update is not supported; "
                    "inbox must be resolved before channel registration",
                    channel_name);
        return make_error(corr_id, "INBOX_UPDATE_NOT_SUPPORTED",
                          "Inbox endpoint must be resolved before REG_REQ, "
                          "not updated afterwards");
    }
    if (endpoint_type != "zmq_node")
    {
        return make_error(corr_id, "UNKNOWN_ENDPOINT_TYPE",
                          "endpoint_type must be 'zmq_node', got: '" + endpoint_type + "'");
    }

    // Idempotency + apply.  Two backing stores depending on which side
    // is publishing:
    //   consumer binding side (fan-in) → ChannelEntry.data_endpoint
    //   producer                        → ProducerEntry.zmq_node_endpoint
    //     (legacy per-producer scope, HEP-CORE-0021 §16.3 — retires as
    //      producers migrate to the topology-model data_endpoint).
    const std::string current_value =
        sender_is_consumer_binding
            ? entry->data_endpoint.value_or(std::string{})
            : entry->producer_zmq_node_endpoint(sender_role_uid).value_or(std::string{});
    auto current = pylabhub::validate_tcp_endpoint(current_value);
    const bool already_resolved = current.ok() && current.port != 0;

    if (already_resolved && current_value == endpoint)
    {
        // HEP-CORE-0021 §16.4 row 3 — idempotent resolved(X) → resolved(X):
        // ACK ok, no mutation.  Fixed-port producers hit this on every
        // startup (config endpoint == resolved endpoint).
        LOGGER_DEBUG("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} — "
                     "already set to '{}' (idempotent)",
                     channel_name, sender_role_uid, endpoint_type, endpoint);
    }
    else
    {
        // HEP-CORE-0021 §16.4 rows 2/4/5 + §16.8 mid-life rules.
        //
        // The endpoint under update is always the BINDING side's data-plane
        // address (producer for fan-out/one-to-one; fan-in binding consumer's
        // data_endpoint).  A change AWAY from an already-resolved value
        // (resolved(X) → resolved(Y), Y≠X) is a mid-life re-update, allowed
        // only while NO dialing-side peer has been admitted to `X` — otherwise
        // those peers hold live connections to X (HEP-CORE-0036 §I5 makes them
        // trusted for the session) and a silent switch to Y would strand them.
        //
        // The channel's admission ledger holds the admitted DIALING-side peers
        // (consumers under fan-out; producers under fan-in), so a non-empty
        // ledger is exactly "peers are dialing the endpoint being changed."
        if (already_resolved)
        {
            std::size_t admitted = 0;
            if (auto access = hub_state_->channel_access(channel_name); access.has_value())
                admitted = access->ledger.admitted_count();

            if (admitted > 0)
            {
                // §16.4 row 5 / §16.8 — consumers attached: change FORBIDDEN.
                // Producer keeps its existing bound port; restart-with-fresh-
                // instance is the supported migration.
                LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} rejected — "
                            "endpoint change forbidden: {} dialing peer(s) already "
                            "admitted to '{}' (HEP-0021 §16.8; restart with a fresh "
                            "instance to change)",
                            channel_name, sender_role_uid, endpoint_type, admitted, current_value);
                return make_error(corr_id, "ENDPOINT_CHANGE_FORBIDDEN",
                                  endpoint_type +
                                      " endpoint change forbidden: " + std::to_string(admitted) +
                                      " dialing peer(s) already admitted to '" + current_value +
                                      "'; restart with a fresh instance to change");
            }
            // §16.4 row 4 / §16.8 special case — zero consumers admitted:
            // change ACCEPTED (pre-first-consumer administrative migration).
            LOGGER_INFO("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} changed "
                        "'{}' → '{}' (no dialing peer admitted; resolved→resolved, "
                        "HEP-0021 §16.8)",
                        channel_name, sender_role_uid, endpoint_type, current_value, endpoint);
        }
        else
        {
            // §16.4 row 2 — unset → resolved.
            LOGGER_INFO("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' role='{}' {} "
                        "updated to '{}'",
                        channel_name, sender_role_uid,
                        sender_is_consumer_binding ? "consumer(binding)" : "producer",
                        endpoint_type, endpoint);
        }

        const bool applied = sender_is_consumer_binding
                                 ? hub_state_->_set_channel_data_endpoint(channel_name, endpoint)
                                 : hub_state_->_set_producer_zmq_node_endpoint(
                                       channel_name, sender_role_uid, endpoint);
        if (!applied)
        {
            LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' lost the race "
                        "(role no longer admitted)",
                        channel_name, sender_role_uid);
            return make_error(corr_id, "NOT_CHANNEL_OWNER",
                              "Sender no longer registered (race condition)");
        }
    }

    nlohmann::json resp;
    resp["status"] = "success";
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// ============================================================================
// HEP-CORE-0034 Phase 4b — hub-globals startup loader
// ----------------------------------------------------------------------------
// Routes hub-global schema files into HubState.schemas via the §2.4 I2
// pipeline:
//
//   filesystem(<hub_dir>/schemas/*.json)
//     → schema::load_all_from_dirs   (stateless parser; §2.4 I5)
//     → to_hub_schema_record         (HEP-0002 → HEP-0034 wire form; §2.4 I6)
//     → HubState::_on_schema_registered  (sole mutator; §2.4 I1)
//
// No state held inside BrokerServiceImpl; the broker is not a registry
// (§2.4 I3 — only HubState is).  This method may be invoked once at
// `run()` startup; idempotency is provided by HubState's `kIdempotent`
// outcome on equivalent re-registration.
// ============================================================================

void BrokerServiceImpl::load_hub_globals_()
{
    const auto dirs = cfg.schema_search_dirs.empty()
                          ? pylabhub::schema::SchemaLibrary::default_search_dirs()
                          : cfg.schema_search_dirs;

    const auto entries = pylabhub::schema::load_all_from_dirs(dirs);

    using O = pylabhub::schema::SchemaRegOutcome;
    std::size_t created = 0;
    std::size_t idempotent = 0;
    std::size_t conflicted = 0;

    for (const auto &[path, entry] : entries)
    {
        // Translate file-form SchemaEntry → wire-form SchemaRecord.  This
        // recomputes the hash under HEP-CORE-0034 §6.3 canonical form;
        // the SHM-header form (`SchemaInfo::hash`) is NOT used here
        // (§2.4 I6 — the two forms are different by design).
        auto rec = pylabhub::hub::to_hub_schema_record(entry);
        const std::string id_for_log = rec.schema_id; // captured before move

        const auto outcome = hub_state_->_on_schema_registered(std::move(rec));
        switch (outcome)
        {
        case O::kCreated:
            ++created;
            break;
        case O::kIdempotent:
            ++idempotent;
            break;
        case O::kHashMismatchSelf:
            LOGGER_WARN("Broker: hub-global schema '{}' from '{}' rejected as "
                        "hash_mismatch_self — another (hub, {}) entry already "
                        "registered with a different fingerprint",
                        id_for_log, path, id_for_log);
            ++conflicted;
            break;
        case O::kForbiddenOwner:
            // Defensive — owner is set to "hub" by to_hub_schema_record;
            // this branch indicates an invariant violation in the
            // translator.
            LOGGER_ERROR("Broker: hub-global schema '{}' from '{}' rejected as "
                         "forbidden_owner — internal invariant violation",
                         id_for_log, path);
            ++conflicted;
            break;
        }
    }
    LOGGER_INFO("Broker: registered {} hub-global schema record(s) "
                "({} idempotent, {} conflicted) from {} dir(s)",
                created, idempotent, conflicted, dirs.size());
}

// ============================================================================
// SCHEMA_REQ handler (HEP-CORE-0034 §10.3)
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_schema_req(const nlohmann::json &req)
{
    // Defensive: null or non-object payloads would throw inside `value()`
    // below (`json.exception.type_error.306`), which would escape past
    // the dispatcher's outer catch and crash the broker.  Wire payloads
    // are always JSON objects per protocol; reject anything else.
    if (req.is_null() || !req.is_object())
    {
        return make_error(/*correlation_id=*/{}, "INVALID_REQUEST",
                          "SCHEMA_REQ payload must be a JSON object");
    }

    const std::string corr_id = req.value("correlation_id", "");

    // Identity-checked pull (2026-07-26): `role_uid` is the
    // CALLER's uid, authenticated by the admission tier
    // (Control_EnvelopeWithRoleUid: envelope identity == body.role_uid,
    // grammar + tag).  Required on every SCHEMA_REQ; the channel form
    // is additionally member-gated below.  The (owner, schema_id)
    // registry form stays open to all known roles — the registry is
    // shared infrastructure and hub-globals have no channel to be a
    // member of.
    const std::string caller_uid = req.value("role_uid", "");
    if (caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "SCHEMA_REQ requires role_uid (the caller's own uid — "
                          "the caller's own uid)");
    }

    // HEP-CORE-0034 §10.3 — owner+id keying.  When both `owner` and
    // `schema_id` are present, look up the SchemaRecord directly in
    // HubState.schemas and return it.  This is the preferred form going
    // forward; the legacy `channel_name` form below is retained for
    // backward compatibility (Phase 3a clients that still ask for
    // schemas by channel).
    const std::string req_owner = req.value("owner", "");
    const std::string req_schema_id = req.value("schema_id", "");
    if (!req_owner.empty() && !req_schema_id.empty())
    {
        const auto rec = hub_state_->schema(req_owner, req_schema_id);
        if (!rec.has_value())
        {
            LOGGER_WARN("Broker: SCHEMA_REQ no record under ({}, {})", req_owner, req_schema_id);
            return make_error(corr_id, "SCHEMA_UNKNOWN",
                              "No schema record under (" + req_owner + ", " + req_schema_id + ")");
        }
        nlohmann::json resp;
        resp["status"] = "success";
        resp["owner"] = rec->owner_uid;
        resp["schema_id"] = rec->schema_id;
        // Both zones, un-merged (HEP-CORE-0034 §10.3).  `packing`/`blds` are the
        // datablock; `flexzone_*` the flexzone; empty strings ⇒ that zone absent.
        resp["packing"] = rec->packing;
        resp["blds"] = rec->blds;
        resp["flexzone_packing"] = rec->flexzone_packing;
        resp["flexzone_blds"] = rec->flexzone_blds;
        resp["schema_hash"] = format_tools::bytes_to_hex(
            {reinterpret_cast<const char *>(rec->hash.data()), rec->hash.size()});
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        return resp;
    }

    // Legacy form: channel_name → returns the channel's schema fields
    // (HEP-CORE-0016 era).  Phase 3a clients with `schema_owner` set on
    // the channel will see that field too in the response.
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "SCHEMA_REQ requires either ('owner' + 'schema_id') "
                          "or 'channel_name'");
    }
    const auto entry = hub_state_->channel(channel_name);
    if (!entry.has_value())
    {
        // Queries answer from machine state and never wait —
        // Absent → terminal CHANNEL_NOT_FOUND (never AWAITING_OWNER).
        LOGGER_WARN("Broker: SCHEMA_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }
    // Member gate — the channel form answers only roles holding a
    // presence on the queried channel (least-privilege precedent:
    // GET_CHANNEL_AUTH is binding-side-gated, CHECK_PEER_READY is
    // dialing-side-gated).  Either side qualifies for a read.
    if (!hub_state_->is_role_registered_on_channel(channel_name, caller_uid, "producer") &&
        !hub_state_->is_role_registered_on_channel(channel_name, caller_uid, "consumer"))
    {
        LOGGER_WARN("Broker: SCHEMA_REQ rejected — role_uid='{}' is not a member of "
                    "channel '{}'",
                    caller_uid, channel_name);
        return make_error(corr_id, "NOT_A_ROLE_OF_CHANNEL",
                          "Caller role_uid='" + caller_uid +
                              "' is not a registered role of "
                              "channel '" +
                              channel_name + "'");
    }
    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel_name"] = channel_name;
    resp["schema_id"] = entry->schema_id;
    resp["schema_owner"] = entry->schema_owner; // HEP-0034 — empty for legacy channels
    resp["blds"] = entry->schema_blds;
    resp["flexzone_blds"] = entry->flexzone_blds; // two-zone (HEP-CORE-0034 §10.3)
    resp["schema_hash"] = entry->schema_hash;     // 128-hex `db‖fz` fingerprint
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ============================================================================
// Heartbeat negotiation block (HEP-CORE-0023 §2.5)
// ============================================================================

nlohmann::json BrokerServiceImpl::heartbeat_ack_block() const
{
    nlohmann::json hb;
    hb["heartbeat_interval_ms"] = static_cast<int64_t>(cfg.heartbeat_interval.count());
    hb["ready_miss_heartbeats"] = cfg.ready_miss_heartbeats;
    hb["pending_miss_heartbeats"] = cfg.pending_miss_heartbeats;
    return hb;
}

nlohmann::json BrokerServiceImpl::roster_ack_block() const
{
    // The replicated roster is the intersection of two facts the hub holds
    // separately (HEP-CORE-0035 §4.9.2, I-ROSTER-PRESENT): the vault says
    // which roles may ever exist here, the ledger says which of them are
    // registered right now.
    //
    // The KEYS come from the vault, never from a registration record.  The
    // ledger contributes exactly one thing — the set of uids that are
    // present — so the vault stays the only place key material is
    // authoritative.
    //
    // A role cannot attribute a sender from a bare key, which is why the
    // name travels with it; the version is what lets a role ask whether
    // what it holds is current instead of the hub resending an identical
    // list.
    //
    // The version and the membership are read under ONE lock so they cannot
    // describe different revisions.  The vault index needs no such care —
    // it is immutable behind a shared_ptr, so the entries it yields belong
    // to whichever revision this load saw.
    const auto authority = peer_authority();
    std::uint64_t version = 0;
    std::vector<std::string> present;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        version = roster_ledger_.current_version();
        present = roster_ledger_.admitted_snapshot();
    }
    const std::unordered_set<std::string> present_set(present.begin(), present.end());

    nlohmann::json roster = nlohmann::json::array();
    for (const auto &entry : authority->local_role_roster())
    {
        if (present_set.find(entry.uid) == present_set.end())
            continue;
        roster.push_back(nlohmann::json{{"uid", entry.uid}, {"pubkey", entry.pubkey_z85}});
    }
    return nlohmann::json{{"known_roles", std::move(roster)}, {"known_roles_version", version}};
}

void BrokerServiceImpl::roster_admit_present(const std::string &uid)
{
    if (uid.empty())
        return;
    std::uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        roster_ledger_.admit(uid);
        version = roster_ledger_.current_version();
    }
    LOGGER_DEBUG("Broker: event=RosterPresent uid='{}' known_roles_version={}", uid, version);
}

void BrokerServiceImpl::roster_revoke_absent(const std::string &uid)
{
    if (uid.empty())
        return;
    std::uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        roster_ledger_.revoke(uid);
        // A departed role stops being a HOLDER as well as a subject.
        // Without this the confirmation outlives the presence, and a role
        // that stops and restarts is credited with a version it applied in
        // a previous life — so the gate in `handle_role_info_req` would
        // call it reachable before it had adopted any roster, and its
        // inbox would refuse the sender the gate just waved through.
        //
        // It also bounds the map.  Channel ledgers die with their channel;
        // this one lives as long as the hub, so it is the first instance
        // where a confirmation that is never erased accumulates.
        roster_ledger_.reset_role_confirmation(uid);
        version = roster_ledger_.current_version();
    }
    LOGGER_DEBUG("Broker: event=RosterAbsent uid='{}' known_roles_version={}", uid, version);
}

BrokerServiceImpl::Reachability
BrokerServiceImpl::roster_reachability(const std::string &target_uid,
                                       const std::string &asker_uid) const
{
    // Named wrapper rather than a bare `is_visible_to` at the call site.
    // Both of its parameters are `std::string`, and holder/subject
    // inverted compiles cleanly and answers a plausible-looking wrong
    // question.  Here the roles are in the names.
    std::lock_guard<std::mutex> lk(roster_mu_);
    if (!roster_ledger_.admission_version_of(asker_uid).has_value())
    {
        // The asker is not registered on this hub, so no roster this hub
        // can ever issue will name it.  Permanent, not pending — telling
        // the caller to try again would be a livelock, and pushing a
        // roster to the target would not change the answer.
        return Reachability::AskerAbsent;
    }
    const auto visible = roster_ledger_.is_visible_to(target_uid, asker_uid);
    return (visible.has_value() && *visible) ? Reachability::Reachable : Reachability::NotYet;
}

void BrokerServiceImpl::handle_roster_check_notify(
    const ::pylabhub::wire::WireEnvelope &env, const ::pylabhub::wire::RosterCheckNotifyBody &body,
    zmq::socket_t &socket)
{
    const std::uint64_t held = body.known_roles_version();
    const std::string reporter = body.role_uid();
    std::uint64_t current = 0;
    {
        std::lock_guard<std::mutex> lk(roster_mu_);
        // This report IS the confirmation (HEP-CORE-0035 §4.9.4): a role
        // saying which version it holds is a role saying which version it
        // has applied, so no second message exists or is needed.
        //
        // This is the ONLY place the confirmation map moves.  The hub knows
        // which version it handed out on every REG_ACK and could record the
        // confirmation itself, saving a message — that is the
        // over-confirmation bug the ledger was built to eliminate (a holder
        // credited with a version it had not applied), and `confirm()`'s
        // contract forbids inferring confirmation from anything but wire
        // evidence.  It stays forbidden here.
        roster_ledger_.confirm(reporter, held);
        current = roster_ledger_.current_version();
    }
    if (held == current)
    {
        // Current — say nothing.  This is the overwhelmingly common
        // outcome and the reason the check can afford to run every tick
        // (HEP-CORE-0035 §4.9.7).
        LOGGER_TRACE("Broker: ROSTER_CHECK_NOTIFY role_uid='{}' current at version {}",
                     body.role_uid(), current);
        return;
    }

    // Not current — send the whole list.  A newer-than-ours version is
    // answered the same way as an older one: the hub is the authority on
    // what the roster is, and a role reporting a version this hub never
    // issued has a list from somewhere else.
    const nlohmann::json block = roster_ack_block();
    send_to_identity(socket, std::string(env.identity()), "ROSTER_UPDATE_NOTIFY", block);
    LOGGER_DEBUG("Broker: event=RosterUpdate role_uid='{}' held={} sent={} entries={}",
                 body.role_uid(), held, block.value("known_roles_version", std::uint64_t{0}),
                 block.at("known_roles").size());
}

// ============================================================================
// Heartbeat timeout detection
// ============================================================================

void BrokerServiceImpl::check_heartbeat_timeouts(zmq::socket_t &socket)
{
    // Two-pass role liveness state machine (HEP-CORE-0023 §2.5).
    // Pass 1: Ready -> Pending on heartbeat absence (producer presumed unresponsive).
    // Pass 2: Pending -> deregistered immediately on extended absence
    //         (no Closing/grace — producer is presumed dead, no one to wait for).
    // Timeouts are ALWAYS enforced; effective_*_timeout() is floored at 1 heartbeat.
    const auto ready_timeout = cfg.effective_ready_timeout();
    const auto pending_timeout = cfg.effective_pending_timeout();
    const auto attach_budget = cfg.effective_producer_apply_wait();

    const auto snap = hub_state_->snapshot();
    const auto now = std::chrono::steady_clock::now();

    // HEP-CORE-0042 §5.4 pending-entry timeout sweep — piggybacked on
    // the heartbeat-timeout cadence.  Runs BEFORE the Pass 1/2
    // presence FSM so a producer that goes silent for both
    // producer_apply_wait_ms and pending_timeout on the same tick has
    // its attach-queue drained with reason="timeout" before the
    // producer-disconnect drain would have re-classified those same
    // consumers as reason="producer_not_live" — timeout is the more
    // specific reason (broker observed the missed deadline; producer
    // may still be alive but slow).
    sweep_pending_attach_timeouts_(socket, now, attach_budget);

    // ── Pass 1: Connected (live) -> Pending demotion (HEP-CORE-0023 §2.1) ─
    // Migrated 2026-06-02 to `for_each_presence_matching` per HEP-0039
    // §6 P8 Step A.  Two-phase: visit collects decisions over `snap`
    // (no lock held), apply drains via the canonical mutator
    // `_on_heartbeat_timeout` (each call takes its own writer lock).
    // Pass-2 below captures `snap2` AFTER this apply phase so that the
    // fresh `state_since` Pass-1 stamps via `_on_heartbeat_timeout`
    // appears in Pass-2's view — see HEP-0039 §6
    // "Two-passes-with-cross-pass-dependency note" for why this
    // ordering is load-bearing.
    //
    // Multi-producer aware (HEP-CORE-0023 §2.1.1): each producer-
    // presence demotes independently when its own heartbeat ages out;
    // co-producers stay alive.  Consumer FSM is independent of producer
    // FSM (HEP-CORE-0023 §2.1, Wave-B M2 3/3) — same timeout, different
    // presence row, same mutator (the mutator dispatches by role_type).
    // The channel does NOT close on consumer demotion — that's a
    // producer-only signal (§2.1.1) handled inside the mutator.
    //
    // `last_heartbeat` is the timeout anchor regardless of
    // `first_heartbeat_seen`.  At REG_REQ time the presence is
    // created with `last_heartbeat = now`, so a producer/consumer
    // that registers and never heartbeats DOES demote here once
    // ready_timeout elapses (then Pending → Disconnected via Pass-2).
    struct Pass1Decision
    {
        std::string channel;
        std::string role_uid;
        std::string role_type; ///< "producer" | "consumer"
    };
    std::vector<Pass1Decision> p1;
    for (const auto &[channel_name, entry] : snap.channels)
    {
        pylabhub::hub::for_each_presence_matching(
            entry, snap.roles,
            [&](const pylabhub::hub::RolePresence &p)
            {
                return p.state == pylabhub::hub::RoleState::Connected &&
                       (now - p.last_heartbeat) >= ready_timeout;
            },
            [&](const pylabhub::hub::PresenceSweepTarget &t)
            {
                Pass1Decision d;
                d.channel = t.channel;
                d.role_uid = (t.party == pylabhub::hub::PartyKind::Producer ? t.producer->role_uid
                                                                            : t.consumer->role_uid);
                d.role_type =
                    (t.party == pylabhub::hub::PartyKind::Producer ? "producer" : "consumer");
                p1.push_back(std::move(d));
            });
    }
    for (const auto &d : p1)
    {
        if (d.role_type == "producer")
        {
            LOGGER_WARN("Broker: role '{}' on channel '{}' demoted Ready -> Pending "
                        "(no heartbeat within {} ms)",
                        d.role_uid, d.channel, ready_timeout.count());
        }
        else
        {
            LOGGER_WARN("Broker: consumer '{}' on channel '{}' demoted Ready -> "
                        "Pending (no heartbeat within {} ms)",
                        d.role_uid, d.channel, ready_timeout.count());
        }
        hub_state_->_on_heartbeat_timeout(d.channel, d.role_uid, d.role_type);
    }

    // ── Pass 2: Pending -> Disconnected (HEP-CORE-0023 §2.1.1) ──
    //
    // Wave M2.5 step 6: per-producer sweep.  Re-snapshot to observe
    // pass 1's transitions, then iterate (channel × producer) and
    // call `_on_pending_timeout(channel, role_uid)` for each
    // producer-presence in Pending state past the deadline.  Atomic
    // channel teardown fires ONLY when the LAST producer transitions
    // Disconnected; non-last drops just remove that producer from
    // `producers[]` and let the channel survive (HEP-CORE-0023 §2.1.1).
    //
    // Notification fan-out (CHANNEL_CLOSING_NOTIFY +
    // on_channel_closed federation relay) only fires when
    // channel_now_empty == true; the `pre_drop` snapshot preserves
    // the full party list for the fan-out target.
    // Migrated 2026-06-02 to `for_each_presence_matching` per HEP-0039
    // §6 P8 Step B.  Per-channel two-phase: visit collects all
    // Pass-2 decisions for the channel (producers + consumers in
    // declaration order) over `snap2`; apply phase drains producer
    // decisions first (with the `channel_torn_down` short-circuit
    // setting on last-producer atomic teardown), then if the channel
    // survives, consumer decisions.  The per-channel structure
    // preserves the original code's `break`-on-teardown + skip-
    // consumers ordering exactly.
    //
    // `pre_drop_channel` is captured into the decision struct at
    // visit time (a value copy of the snapshot's `ChannelEntry` via
    // `t.channel_entry`), so the fan-out target list survives the
    // mutator that erases the live channel.  Same for
    // `pre_drop_consumer` on the consumer path.
    const auto snap2 = hub_state_->snapshot();
    struct Pass2Decision
    {
        pylabhub::hub::PartyKind party;
        std::string channel;
        std::string role_uid;
        pylabhub::hub::ChannelEntry pre_drop_channel;
        std::optional<pylabhub::hub::ConsumerEntry> pre_drop_consumer;
    };
    for (const auto &[channel_name, entry] : snap2.channels)
    {
        std::vector<Pass2Decision> p2;
        pylabhub::hub::for_each_presence_matching(
            entry, snap2.roles,
            [&](const pylabhub::hub::RolePresence &p)
            {
                return p.state == pylabhub::hub::RoleState::Pending &&
                       (now - p.state_since) >= pending_timeout;
            },
            [&](const pylabhub::hub::PresenceSweepTarget &t)
            {
                Pass2Decision d;
                d.party = t.party;
                d.channel = t.channel;
                d.pre_drop_channel = *t.channel_entry;
                if (t.party == pylabhub::hub::PartyKind::Producer)
                {
                    d.role_uid = t.producer->role_uid;
                }
                else
                {
                    d.role_uid = t.consumer->role_uid;
                    d.pre_drop_consumer = *t.consumer;
                }
                p2.push_back(std::move(d));
            });

        // Apply producer decisions for this channel first.  On
        // last-producer teardown, set `channel_torn_down` and stop —
        // remaining producer decisions for this channel were against
        // a now-gone channel and their `_on_pending_timeout` calls
        // would no-op anyway; their snapshot data is stale and the
        // notify fan-outs would be wrong.  Per HEP-0039 §6 P8 the
        // `break` is the per-channel atomicity boundary.
        bool channel_torn_down = false;
        for (const auto &d : p2)
        {
            if (d.party != pylabhub::hub::PartyKind::Producer)
                continue;

            LOGGER_WARN("Broker: producer '{}' on '{}' reclaimed from Pending "
                        "(no heartbeat within {} ms)",
                        d.role_uid, d.channel, pending_timeout.count());

            auto drop = hub_state_->_on_pending_timeout(d.channel, d.role_uid, "producer");
            if (drop.removed && drop.channel_now_empty)
            {
                // Last-producer drop → atomic channel teardown.
                // Notify consumers + federation peers using the
                // pre_drop snapshot (the channel record is now gone).
                send_closing_notify(socket, d.channel, d.pre_drop_channel, "pending_timeout");
                on_channel_closed(socket, d.channel, d.pre_drop_channel, "pending_timeout");
                // HEP-CORE-0036 §6.5 + HEP-CORE-0042 §5.4: drop the
                // channel-access record now that the channel is gone.
                // Idempotent — safe even if _on_channel_access_opened
                // was never called.  Symmetric with the VoluntaryDereg
                // last-producer path in handle_dereg_req (line ~2430);
                // omitting this call would leak the full
                // ChannelAccessEntry — including the unified `ledger`
                // (admission_version_ + confirmed_version_ per role) —
                // into channel_access_index[K] even though the channel
                // itself is gone.  Prior to the 2026-07-01 Phase 2.2
                // close-out this asymmetry was masked by
                // channel_access_index being drained on broker restart
                // (only path); with HEP-CORE-0042's fast-path reading
                // ledger.current_version() + confirmed_version_of, a
                // subsequent channel re-open on the same name would
                // inherit stale state.
                hub_state_->_on_channel_access_closed(d.channel);
                // HEP-CORE-0042 §5.4 channel-close drain — reply
                // {status="denied", reason="channel_closing"} to every
                // pending ATTACH_REQ_ZMQ across all producers of this
                // channel.  Symmetric with the VoluntaryDereg last-
                // producer path in handle_dereg_req.
                drain_pending_attach_queue_for_channel_denied_(socket, d.channel,
                                                               "channel_closing");
                // M1.4 (2026-05-11): no metrics_store_.erase — see
                // comment at handle_dereg_req last-producer path.
                LOGGER_INFO("Broker: channel '{}' torn down (last "
                            "producer presence-timeout)",
                            d.channel);
                channel_torn_down = true;
                break;
            }
            else if (drop.removed)
            {
                // HEP-CORE-0042 §5.4 producer-disconnect drain (kDead) —
                // reply {status="denied", reason="producer_not_live"} to
                // every pending ATTACH_REQ_ZMQ for THIS (K, P).  Pairs
                // with HubState::_on_pending_timeout's
                // confirmed_version[K][P] erase (2.2 close-out) on the
                // non-last-producer branch.
                drain_pending_attach_queue_for_producer_denied_(socket, d.channel, d.role_uid,
                                                                "producer_not_live");
                LOGGER_INFO("Broker: producer '{}' dropped on '{}' "
                            "(presence-timeout; {} producer(s) remain — "
                            "channel survives)",
                            d.role_uid, d.channel,
                            static_cast<uint32_t>(d.pre_drop_channel.producer_count() - 1));
            }
        }

        // Wave-B M2 (3/3): consumer-presence Pending → Disconnected.
        // Skip on torn-down channels — `_on_channel_closed` already
        // demoted every consumer-presence on this channel to
        // Disconnected and the surviving consumer list is empty.
        if (channel_torn_down)
            continue;

        for (const auto &d : p2)
        {
            if (d.party != pylabhub::hub::PartyKind::Consumer)
                continue;

            LOGGER_WARN("Broker: consumer '{}' on '{}' reclaimed from Pending "
                        "(no heartbeat within {} ms)",
                        d.role_uid, d.channel, pending_timeout.count());

            auto drop = hub_state_->_on_pending_timeout(d.channel, d.role_uid, "consumer");
            if (!drop.removed)
                continue;

            if (drop.channel_now_empty)
            {
                // HEP-CORE-0017 §4.7.0.2 T2 — the reaped consumer was
                // the fan-in binding OWNER: owner death is channel
                // death.  Same close-out sequence as the last-owning-
                // producer branch above (pre-drop snapshot for the
                // fan-out; access record + attach drain).  The
                // per-consumer DIED notify / key revoke below are
                // superseded by the channel-wide close: producers get
                // CHANNEL_CLOSING_NOTIFY, and the whole
                // ChannelAccessEntry (ledger included) is erased.
                send_closing_notify(socket, d.channel, d.pre_drop_channel, "pending_timeout");
                on_channel_closed(socket, d.channel, d.pre_drop_channel, "pending_timeout");
                hub_state_->_on_channel_access_closed(d.channel);
                drain_pending_attach_queue_for_channel_denied_(socket, d.channel,
                                                               "channel_closing");
                LOGGER_INFO("Broker: channel '{}' torn down (fan-in "
                            "consumer-owner presence-timeout)",
                            d.channel);
                continue;
            }

            // Fan out CONSUMER_DIED_NOTIFY with reason="heartbeat_timeout"
            // to every producer on the channel.  Producers consume the
            // notification per HEP-CORE-0023 §2.1.1 to drop their
            // per-consumer bookkeeping.
            // broker_proto 4→5 (audit R3.5b, 2026-05-19): notify body
            // uses `role_uid` (the consumer's role.uid; tag `cons.` or
            // `proc.` is embedded in the value) — replaces the legacy
            // `consumer_uid` field for cross-message uniformity.
            const auto &pre_drop_consumer = *d.pre_drop_consumer;
            const auto &pre_drop_channel = d.pre_drop_channel;
            nlohmann::json notify;
            notify["channel_name"] = d.channel;
            notify["role_uid"] = pre_drop_consumer.role_uid;
            notify["consumer_pid"] = pre_drop_consumer.consumer_pid;
            notify["consumer_hostname"] = pre_drop_consumer.consumer_hostname;
            notify["reason"] = "heartbeat_timeout";
            pylabhub::hub::for_each_party_identity(
                pre_drop_channel, pylabhub::hub::PartyKind::Producer,
                [&](std::string_view zmq_identity, std::string_view producer_uid)
                {
                    send_to_identity(socket, std::string(zmq_identity), "CONSUMER_DIED_NOTIFY",
                                     notify);
                    LOGGER_INFO("Broker: CONSUMER_DIED_NOTIFY to producer of '{}': "
                                "role_uid={} reason=heartbeat_timeout target_role={}",
                                d.channel, d.role_uid, producer_uid);
                });
            // Federation / observer fan-out.
            on_consumer_closed(socket, d.channel, pre_drop_consumer, "heartbeat_timeout");

            // Revoke admission on reclaim.  The consumer presence has just
            // gone Pending→Disconnected on the heartbeat FSM (HEP-CORE-0023
            // §2.1) — now the SOLE consumer-liveness mechanism — so this is
            // the only place a reaped consumer's key leaves the channel's
            // admission ledger.  Revoke + refresh the producers' caches via
            // CHANNEL_AUTH_CHANGED_NOTIFY (HEP-CORE-0036 §6.5).
            // CONSUMER_REG_REQ hard-rejects a non-40-char pubkey at the wire
            // (HEP-CORE-0035 §2 unconditional CURVE), so a stored
            // ConsumerEntry always carries a valid key.
            if (pre_drop_consumer.zmq_pubkey.size() == 40)
            {
                hub_state_->_on_consumer_revoked(d.channel, pre_drop_consumer.zmq_pubkey);
                fire_channel_auth_changed_notify(socket, d.channel,
                                                 /*phase=*/"left",
                                                 /*role_uid=*/pre_drop_consumer.role_uid,
                                                 /*role_type=*/"consumer");
            }
        }
    }
}
void BrokerServiceImpl::send_closing_notify(zmq::socket_t &socket, const std::string &channel_name,
                                            const pylabhub::hub::ChannelEntry &entry,
                                            const std::string &reason)
{
    nlohmann::json body;
    body["channel_name"] = channel_name;
    body["reason"] = reason;

    // Notify all registered consumers.
    for (const auto &consumer : entry.consumers)
    {
        if (consumer.zmq_identity.empty())
        {
            continue;
        }
        try
        {
            send_to_identity(socket, consumer.zmq_identity, "CHANNEL_CLOSING_NOTIFY", body);
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' ->consumer pid={}", channel_name,
                        consumer.consumer_pid);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("Broker: failed to notify consumer pid={} for '{}': {}",
                        consumer.consumer_pid, channel_name, e.what());
        }
    }

    // Also notify every registered producer (HEP-CORE-0023 §2.1.1).
    for (const auto &prod : entry.producers)
    {
        if (prod.zmq_identity.empty())
            continue;
        try
        {
            send_to_identity(socket, prod.zmq_identity, "CHANNEL_CLOSING_NOTIFY", body);
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' ->producer pid={} role={}",
                        channel_name, prod.producer_pid, prod.role_uid);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("Broker: failed to notify producer {} for '{}': {}", prod.role_uid,
                        channel_name, e.what());
        }
    }

    // #74 — periodic objective-count reconciliation.  Any leave (DISC, dereg,
    // death) or missed join is caught here: recompute each live channel's
    // {producer_count, consumer_count} and re-fan CHANNEL_COUNT_NOTIFY only
    // when it changed since the last broadcast.  Router-thread-only; bounded by
    // channel x member count, and silent when nothing moved.
    {
        const auto count_snap = hub_state_->snapshot();
        for (const auto &kv : count_snap.channels)
        {
            const auto &chan = kv.first;
            const auto counts = compute_channel_live_counts(chan);
            auto lit = last_channel_counts_.find(chan);
            if (lit != last_channel_counts_.end() && lit->second == counts)
                continue;
            fire_channel_count_notify(socket, chan);
        }
        for (auto cit2 = last_channel_counts_.begin(); cit2 != last_channel_counts_.end();)
            cit2 = (count_snap.channels.count(cit2->first) == 0) ? last_channel_counts_.erase(cit2)
                                                                 : std::next(cit2);
    }
}

// ============================================================================
// Role-close cleanup API (HEP-CORE-0023 §2.5)
// ============================================================================

void BrokerServiceImpl::on_channel_closed(zmq::socket_t & /*socket*/,
                                          const std::string &channel_name,
                                          const pylabhub::hub::ChannelEntry &entry,
                                          const std::string &reason)
{
    // Wave M3 step 5f (2026-05-11) — band cleanup MOVED from this
    // per-producer imperative loop into HubState's terminal-cleanup
    // cascade (`cascade_role_terminal_cleanup_locked`), which fires
    // `band_left` for every band the disconnecting role was in.  The
    // broker subscribes to `band_left` in `run()` and emits
    // BAND_LEAVE_NOTIFY from there.  This fix tracks band membership
    // by role-lifetime rather than channel-lifetime — a multi-presence
    // role that loses ONE channel but remains alive elsewhere stays in
    // its bands (the prior imperative code evicted it too eagerly).
    federation_on_channel_closed(channel_name, entry, reason);
}

void BrokerServiceImpl::on_consumer_closed(zmq::socket_t & /*socket*/,
                                           const std::string & /*channel_name*/,
                                           const pylabhub::hub::ConsumerEntry & /*consumer*/,
                                           const std::string & /*reason*/)
{
    // Wave M3 step 5f (2026-05-11) — see `on_channel_closed`.  Consumer
    // role-disconnect band cleanup is also handler-driven now.  This
    // hook is kept for future broker-side reactions to consumer-close
    // that don't fit the role-disconnect path (e.g., per-consumer
    // observability).
}

void BrokerServiceImpl::federation_on_channel_closed(const std::string & /*channel_name*/,
                                                     const pylabhub::hub::ChannelEntry & /*entry*/,
                                                     const std::string & /*reason*/)
{
    // No broker-internal index to maintain (relay targets are computed
    // on-the-fly from `hub_state_->snapshot().peers`).  Stale channel-name
    // entries in a peer's `relay_channels` are benign: relay_notify_to_peers
    // only fires when a NOTIFY arrives for a live channel name; if the
    // channel is gone, no NOTIFY arrives.
}

void BrokerServiceImpl::send_band_leave_notify(zmq::socket_t &socket, const std::string &band_name,
                                               const std::string &role_uid,
                                               const std::string &role_name,
                                               const std::string &reason)
{
    if (role_uid.empty() || band_name.empty())
        return;
    // HubState has already removed the leaving uid from band members
    // (under the writer lock, before firing this handler).  Query the
    // current member list and notify each remaining member.  If the
    // band was auto-deleted (uid was its last member), `band(name)` is
    // nullopt and there's nothing to notify.
    // Log the leave regardless of whether the band survived (matches
    // prior imperative behaviour so diagnostic output is consistent).
    LOGGER_INFO("Broker: role '{}' removed from band '{}' (reason={})", role_uid, band_name,
                reason);
    auto remaining = hub_state_->band(band_name);
    if (!remaining.has_value())
        return; // auto-deleted; no NOTIFY targets
    // BandLeaveNotifyBody REQUIRED fields per wire_bodies.cpp:323 =
    // {band, role_uid, role_name}.  role_name is captured pre-erase by
    // HubState::_set_band_left and threaded through the handler; empty
    // here means the leaver was never a member (defensive) — still emit
    // for wire consistency; downstream body-class construction would
    // reject on empty string anyway.
    nlohmann::json notify;
    notify["band"] = band_name; // HEP-CORE-0030 §5.1 wire key
    notify["role_uid"] = role_uid;
    notify["role_name"] = role_name;
    notify["reason"] = reason;
    for (const auto &m : remaining->members)
    {
        if (!m.zmq_identity.empty())
            send_to_identity(socket, m.zmq_identity, "BAND_LEAVE_NOTIFY", notify);
    }
}

void BrokerServiceImpl::handle_checksum_error_report(zmq::socket_t &socket,
                                                     const nlohmann::json &req)
{
    const auto channel = req.value("channel_name", std::string{});
    const auto slot = req.value("slot_index", -1);
    const auto pid = req.value("reporter_pid", uint64_t{0});
    const auto error_str = req.value("error", std::string{});
    LOGGER_WARN("Broker: Cat2 checksum error on '{}' slot={} pid={} err='{}'", channel, slot, pid,
                error_str);

    if (cfg.checksum_repair_policy == ChecksumRepairPolicy::NotifyOnly)
    {
        auto entry = hub_state_->channel(channel);
        if (entry)
        {
            nlohmann::json fwd = req;
            fwd["broker_action"] = "notify_only";
            for (const auto &consumer : entry->consumers)
            {
                if (!consumer.zmq_identity.empty())
                {
                    send_to_identity(socket, consumer.zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
                }
            }
            // Fan-out to every producer (HEP-CORE-0023 §2.1.1).
            for (const auto &prod : entry->producers)
            {
                if (prod.zmq_identity.empty())
                    continue;
                send_to_identity(socket, prod.zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
            }
            LOGGER_INFO("Broker: CHANNEL_EVENT_NOTIFY ->all members of '{}': "
                        "checksum_error slot={}, action=notify_only",
                        channel, slot);
        }
    }
    // ChecksumRepairPolicy::Repair — deferred; requires WriteAttach slot repair path.
}

// ============================================================================
// CHANNEL_NOTIFY_REQ handler removed — audit R3.6 (2026-05-17)
// ============================================================================
//
// `handle_channel_notify_req` deleted along with the dispatch entry,
// the `is_known_msg_type` list entry, and the declaration above.
// Pre-2026-05-17 this handler stayed because we believed
// HEP-CORE-0022 federation peers emitted CHANNEL_NOTIFY_REQ on
// peer-relay paths.  Investigation found federation actually uses
// `HUB_RELAY_MSG` (broker↔broker, `handle_hub_relay_msg` —
// broker_service.cpp:4073), NOT CHANNEL_NOTIFY_REQ.  The role-side
// `BRC::send_notify` (the only emitter of CHANNEL_NOTIFY_REQ) was
// already deleted in O1.  Net: handler was 100% dead.  Old clients
// sending CHANNEL_NOTIFY_REQ now receive UNKNOWN_MSG_TYPE.

// ============================================================================
// CHANNEL_BROADCAST_SEND_NOTIFY — fan out message to ALL members of a channel
// (broker emits CHANNEL_BROADCAST_DELIVER_NOTIFY to each recipient).
// ============================================================================

void BrokerServiceImpl::handle_channel_broadcast_req(zmq::socket_t &socket,
                                                     const ChannelBroadcast &bc)
{
    const std::string &target_channel = bc.target_channel;

    if (target_channel.empty())
    {
        LOGGER_WARN("Broker: CHANNEL_BROADCAST_SEND_NOTIFY with empty target_channel");
        return;
    }

    auto entry = hub_state_->channel(target_channel);
    if (!entry)
    {
        LOGGER_DEBUG("Broker: CHANNEL_BROADCAST_SEND_NOTIFY for '{}' — channel not found",
                     target_channel);
        return;
    }

    // Build the broadcast notification body.
    nlohmann::json fwd;
    fwd["channel_name"] = target_channel;
    fwd["event"] = "broadcast";
    fwd["sender_uid"] = bc.sender_uid;
    fwd["message"] = bc.message;
    if (!bc.data.empty())
        fwd["data"] = bc.data;

    // Fan out to ALL consumers.
    for (const auto &consumer : entry->consumers)
    {
        if (consumer.zmq_identity.empty())
            continue;
        try
        {
            send_to_identity(socket, consumer.zmq_identity, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                             fwd);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("Broker: broadcast to consumer pid={} for '{}' failed: {}",
                        consumer.consumer_pid, target_channel, e.what());
        }
    }

    // Also send to every registered producer (HEP-CORE-0023 §2.1.1).
    for (const auto &prod : entry->producers)
    {
        if (prod.zmq_identity.empty())
            continue;
        try
        {
            send_to_identity(socket, prod.zmq_identity, "CHANNEL_BROADCAST_DELIVER_NOTIFY", fwd);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("Broker: broadcast to producer {} for '{}' failed: {}", prod.role_uid,
                        target_channel, e.what());
        }
    }

    LOGGER_DEBUG(
        "Broker: CHANNEL_BROADCAST_SEND_NOTIFY '{}' msg='{}' ->{} consumers + {} producer(s)",
        target_channel, bc.message, entry->consumers.size(), entry->producers.size());

    // HEP-CORE-0022: relay to federation peers subscribed to this channel.
    // [BR6] Use fixed event name "broadcast" and put the message in the payload field,
    // consistent with how CHANNEL_EVENT_NOTIFY delivers local broadcast events.
    const std::string relay_payload = bc.data.empty() ? bc.message : bc.message + "|" + bc.data;
    relay_notify_to_peers(socket, target_channel, "broadcast", bc.sender_uid, relay_payload);
}

// ============================================================================
// CHANNEL_LIST_REQ — return list of registered channels
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_channel_list_req(const nlohmann::json &req)
{
    const std::string corr_id = req.value("correlation_id", "");
    nlohmann::json resp;
    resp["status"] = "success";

    nlohmann::json channels = nlohmann::json::array();
    const auto snap = hub_state_->snapshot();
    for (const auto &[name, entry] : snap.channels)
    {
        nlohmann::json ch;
        ch["name"] = name;
        // Multi-producer channels (HEP-CORE-0023 §2.1.1): expose the
        // full list as `producer_uids`; `producer_uid` is the first
        // for back-compat with single-producer admin clients.
        nlohmann::json producer_uids = nlohmann::json::array();
        for (const auto &p : entry.producers)
            producer_uids.push_back(p.role_uid);
        ch["producer_uids"] = std::move(producer_uids);
        ch["producer_uid"] =
            entry.producers.empty() ? std::string{} : entry.producers.front().role_uid;
        // producer_pid of the first producer (0 if none) — admin/list
        // introspection field consumed by broker-admin list tests.
        ch["producer_pid"] =
            entry.producers.empty() ? std::uint64_t{0} : entry.producers.front().producer_pid;
        ch["schema_id"] = entry.schema_id;
        ch["consumer_count"] = entry.consumers.size();
        // HEP-CORE-0023 §2.2 — channel state is the protocol-defined
        // `observable`, derived from the producer-presence row.
        ch["observable"] = pylabhub::hub::to_string(pylabhub::hub::observe_channel(entry, snap));
        channels.push_back(std::move(ch));
    }
    resp["channels"] = std::move(channels);
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// ============================================================================
// ROLE_PRESENCE_REQ / ROLE_INFO_REQ (Phase 4)
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_role_presence_req(const nlohmann::json &req)
{
    // HEP-CORE-0007 §"ROLE_PRESENCE_REQ" — wire field `role_uid`
    // (unified with REG_REQ / CONSUMER_REG_REQ; old `uid` form retired
    // 2026-05-09 as part of the protocol-doc-vs-code unification).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string uid = req.value("role_uid", "");
    if (uid.empty())
    {
        // Standard error envelope per HEP-CORE-0007 §12.3 + §12.4a
        // (`MISSING_ROLE_UID`).  Pre-2026-05-10 this handler emitted
        // an ad-hoc `{"present": false, "error": "..."}` shape that
        // diverged from the broker-wide error envelope.  Now goes
        // through `make_error` for uniform `{status, error_code,
        // message, correlation_id}` shape.
        return make_error(corr_id, "MISSING_ROLE_UID", "missing role_uid");
    }
    // role_uid grammar + role-tag policy ran at the wire dispatch
    // pipeline: ROLE_PRESENCE_REQ is in Tier::
    // Control_EnvelopeWithQueryRoleUid — the body role_uid here is
    // the QUERIED subject, not the caller's own uid, so
    // identity_match is intentionally NOT run.  Universal
    // {prod,cons,proc} tag policy still applies.

    // Scan all channels: check every producer + each consumer
    // (HEP-CORE-0023 §2.1.1 multi-producer aware).
    const auto snap = hub_state_->snapshot();
    for (const auto &[name, entry] : snap.channels)
    {
        if (entry.find_producer(uid) != nullptr)
        {
            nlohmann::json resp;
            resp["present"] = true;
            resp["channel"] = name;
            resp["role"] = "producer";
            if (!corr_id.empty())
                resp["correlation_id"] = corr_id;
            LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' found as producer on '{}'", uid, name);
            return resp;
        }
        for (const auto &c : entry.consumers)
        {
            if (!c.role_uid.empty() && c.role_uid == uid)
            {
                nlohmann::json resp;
                resp["present"] = true;
                resp["channel"] = name;
                resp["role"] = "consumer";
                if (!corr_id.empty())
                    resp["correlation_id"] = corr_id;
                LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' found as consumer on '{}'", uid,
                             name);
                return resp;
            }
        }
    }

    LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' not found", uid);
    nlohmann::json resp;
    resp["present"] = false;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_role_info_req(zmq::socket_t &socket,
                                                       const std::string &asker_uid,
                                                       const nlohmann::json &req)
{
    // HEP-CORE-0007 §"ROLE_INFO_REQ" — wire field `role_uid` (unified with
    // REG_REQ / CONSUMER_REG_REQ / ROLE_PRESENCE_REQ; old `uid` form retired
    // 2026-05-09).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string uid = req.value("role_uid", "");
    if (uid.empty())
    {
        // Standard error envelope per HEP-CORE-0007 §12.3 + §12.4a.
        return make_error(corr_id, "MISSING_ROLE_UID", "missing role_uid");
    }
    // role_uid grammar + role-tag policy ran in the wire dispatch pipeline:
    // ROLE_INFO_REQ is Tier::Control_EnvelopeWithQueryRoleUid — the body
    // role_uid is the QUERIED subject, not the caller's own uid, so
    // identity_match is intentionally NOT run.  `asker_uid` is therefore the
    // routing identity the caller CHOSE.  That is why it may decide
    // reachability and must never decide access (HEP-CORE-0035 §4.9.7).

    /// One role's inbox coordinates, wherever they were found.  Producers and
    /// consumers keep them on different records, but every consumer of them
    /// wants the same five fields — so the search yields this and the reply is
    /// built once.  Two search loops and two reply builders is how the gate
    /// below would have had to exist twice.
    struct InboxRecord
    {
        std::string channel;
        std::string endpoint;
        std::string packing;
        std::string checksum;
        std::string schema_json;
    };

    const auto snap = hub_state_->snapshot();
    std::optional<InboxRecord> rec;
    const char *found_as = "";
    for (const auto &[name, entry] : snap.channels)
    {
        if (const auto *prod = entry.find_producer(uid); prod != nullptr)
        {
            rec = InboxRecord{name, prod->inbox_endpoint, prod->inbox_packing, prod->inbox_checksum,
                              prod->inbox_schema_json};
            found_as = "producer";
            break;
        }
    }
    for (const auto &[name, entry] : snap.channels)
    {
        if (rec)
            break;
        for (const auto &cons : entry.consumers)
        {
            if (cons.role_uid.empty() || cons.role_uid != uid)
                continue;
            rec = InboxRecord{name, cons.inbox_endpoint, cons.inbox_packing, cons.inbox_checksum,
                              cons.inbox_schema_json};
            found_as = "consumer";
            break;
        }
    }

    // ONE shape for every answer.  The key set is constant; what varies is
    // whether the coordinates are filled in and what `reason` says.  Three
    // outcomes emitting three different key sets would make ROLE_INFO_ACK
    // three wire messages wearing one name, and a caller would have to
    // probe for keys to find out which it got.
    const auto answer = [&corr_id](bool found, const char *reason, const std::string &channel)
    {
        nlohmann::json r;
        r["found"] = found;
        r["reason"] = reason;
        r["channel"] = channel;
        r["inbox_endpoint"] = "";
        r["inbox_packing"] = "";
        r["inbox_checksum"] = "";
        r["inbox_receiver_pubkey_z85"] = "";
        r["inbox_schema"] = nlohmann::json::array();
        if (!corr_id.empty())
            r["correlation_id"] = corr_id;
        return r;
    };

    if (!rec)
    {
        LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' not found", uid);
        return answer(false, "no_such_role", "");
    }

    // A role with no inbox has no coordinates to withhold and no roster to
    // wait on, so the gate must not run for it — doing so would push a roster
    // on behalf of a sender that has nothing to reach.
    if (rec->endpoint.empty())
    {
        LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found as {} on '{}', no inbox", uid, found_as,
                     rec->channel);
        return answer(false, "no_inbox", rec->channel);
    }

    // ── I-INBOX-REACHABLE (HEP-CORE-0035 §4.9.7) ─────────────────────────
    // The one place this hub says where a mailbox is.  A denied handshake is
    // terminal — it costs the sender the connection and the message on it —
    // so a sender is not sent to a door that will not open.  The coordinates
    // travel only once the target has confirmed a roster naming this asker.
    //
    // `roster_reachability` takes `roster_mu_` and releases it before
    // returning: `roster_ack_block()` below takes the same non-recursive
    // mutex, so the verdict must not be held across it.
    switch (roster_reachability(uid, asker_uid))
    {
    case Reachability::Reachable:
        break;

    case Reachability::NotYet:
        // Prompt the target now rather than waiting for its next tick, then
        // tell the asker to come back.  Addressed by uid: I-DEALER-IDENTITY
        // (HEP-CORE-0046) makes a role's control-plane routing id its uid and
        // the broker verifies that at REG admission, so a role-scoped send
        // needs no identity lookup and does not consult the per-presence
        // `zmq_identity` copies.
        send_to_identity(socket, uid, "ROSTER_UPDATE_NOTIFY", roster_ack_block());
        LOGGER_INFO("Broker: event=InboxNotReachableYet target='{}' asker='{}' — target has not "
                    "confirmed a roster naming the asker; roster sent to target, asker told to "
                    "retry (HEP-CORE-0035 §4.9.7)",
                    uid, asker_uid);
        return answer(false, "not_reachable_yet", rec->channel);

    case Reachability::AskerAbsent:
        // Permanent, not pending.  No roster this hub issues can ever name a
        // role that holds no registration here, so telling the caller to
        // retry would be a livelock and pushing a roster would change
        // nothing.
        LOGGER_WARN("Broker: event=InboxAskerNotRegistered target='{}' asker='{}' — the asker "
                    "holds no registration on this hub, so no roster this hub issues can name "
                    "it; answering permanently rather than pending (HEP-CORE-0035 §4.9.7)",
                    uid, asker_uid);
        return answer(false, "sender_not_registered", rec->channel);
    }

    // ── Disclosure ───────────────────────────────────────────────────────
    // Everything the sender needs to dial, and nothing before it is allowed
    // to.  HEP-CORE-0027 §3.5: the sender's InboxClient DEALER pins the
    // receiver's identity pubkey as `curve_serverkey`, so that key travels
    // here.  It is the receiver's `known_roles` entry (single-key model I6 —
    // the same key it presents on data sockets).
    nlohmann::json resp = answer(true, "reachable", rec->channel);
    resp["inbox_endpoint"] = rec->endpoint;
    resp["inbox_packing"] = rec->packing;
    resp["inbox_checksum"] = rec->checksum;
    resp["inbox_receiver_pubkey_z85"] = [this, &uid]() -> std::string
    {
        for (const auto &kr : cfg.known_roles)
            if (kr.uid == uid)
                return kr.pubkey_z85;
        return {};
    }();
    if (!rec->schema_json.empty())
    {
        try
        {
            resp["inbox_schema"] = nlohmann::json::parse(rec->schema_json);
        }
        catch (const nlohmann::json::exception &je)
        {
            // REG_REQ validation should have rejected this, so reaching here
            // means stored state is corrupt.  Warn rather than silently
            // returning an empty schema, which the caller would happily use
            // and thereby mask the corruption.
            LOGGER_WARN("Broker: stored inbox_schema_json for channel '{}' {} '{}' is malformed: "
                        "{}; returning empty array",
                        rec->channel, found_as, uid, je.what());
        }
    }
    LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found as {} on '{}', inbox='{}'", uid, found_as,
                 rec->channel, rec->endpoint);
    return resp;
}

// ============================================================================
// send_to_identity — push unsolicited message to a connected DEALER by raw identity
// ============================================================================

void BrokerServiceImpl::send_to_identity(zmq::socket_t &socket, const std::string &identity,
                                         const std::string &msg_type, const nlohmann::json &body)
{
    // HEP-CORE-0046 §14 wire envelope.  Extract correlation_id from the
    // body (handlers echo it, or NOTIFY sites leave it empty — is_notify
    // check in build_router_send allows empty for _NOTIFY suffix per
    // I-CORRELATION-STABLE / I-MSG-TYPE-TAXONOMY).
    const std::string corr_id = body.value("correlation_id", std::string{});
    try
    {
        zmq::multipart_t wire =
            ::pylabhub::wire::WireEnvelope::build_router_send(identity, msg_type, corr_id, body);
        wire.send(socket);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("Broker: send_to_identity '{}' failed: {}", msg_type, e.what());
    }
}

// ============================================================================
// Helpers
// ============================================================================

void BrokerServiceImpl::send_reply(zmq::socket_t &socket, const zmq::message_t &identity,
                                   const std::string &msg_type_ack, const nlohmann::json &body)
{
    LOGGER_TRACE("Broker: send {} status='{}'", msg_type_ack, body.value("status", ""));
    // HEP-CORE-0046 §14 wire envelope.  correlation_id echoed from the
    // request body per adapter Tier 2 contract; ERROR / ACK bodies both
    // carry it (make_error stamps it, handlers set resp[correlation_id]
    // = corr_id).  Empty is allowed only for _NOTIFY suffix — send_reply
    // is only used for REQ replies so a missing echo is a bug we want
    // loud (envelope build will throw).
    const std::string identity_str(static_cast<const char *>(identity.data()), identity.size());
    const std::string corr_id = body.value("correlation_id", std::string{});
    try
    {
        zmq::multipart_t wire = ::pylabhub::wire::WireEnvelope::build_router_send(
            identity_str, msg_type_ack, corr_id, body);
        wire.send(socket);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("Broker: send_reply '{}' failed: {}", msg_type_ack, e.what());
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
nlohmann::json BrokerServiceImpl::make_error(const std::string &correlation_id,
                                             const std::string &error_code,
                                             const std::string &message)
{
    nlohmann::json err;
    err["status"] = "error";
    err["error_code"] = error_code;
    err["message"] = message;
    if (!correlation_id.empty())
    {
        err["correlation_id"] = correlation_id;
    }
    return err;
}

// ============================================================================
// BrokerService — Pimpl delegation
// ============================================================================

BrokerService::BrokerService(Config cfg, pylabhub::hub::HubState &state)
    : pImpl(std::make_unique<BrokerServiceImpl>())
{
    // HEP-CORE-0035 §2 — CURVE is unconditional.
    // HEP-CORE-0040 §172 — hub identity bytes live in the process
    // KeyStore under `"hub_identity"`; production seeds it via
    // `HubConfig::load_keypair(password)` before `HubHost::startup`
    // constructs the broker, tests via `seed_curve_identities()`
    // (curve_test_setup.h).  An absent KeyStore entry is a
    // programmer error (no-bypass discipline, §4.6.5).
    namespace sec = pylabhub::utils::security;
    if (!sec::sodium_ready() || !sec::secure().keys().has(sec::kHubIdentityName))
        throw std::logic_error("BrokerService: KeyStore entry 'hub_identity' is REQUIRED "
                               "(HEP-CORE-0035 §2; HEP-CORE-0040 §172).  Production: "
                               "route through HubHost::startup (HubConfig::load_keypair "
                               "seeds the KeyStore from HubVault).  Tests: call "
                               "`pylabhub::tests::seed_curve_identities(setup)` before "
                               "building the broker.");
    pImpl->cfg = std::move(cfg);
    pImpl->hub_state_ = &state; // non-owning; HubHost (or test fixture) owns it

    // HEP-CORE-0035 §4.2 — build the pubkey origin index from the
    // operator's configured roster.  This is the config-ingestion
    // boundary: the one place operator text becomes a recognised
    // principal, and therefore the one place that can refuse.
    //
    // A malformed entry aborts startup, named: the Builder throws and
    // nothing here catches.  Skipping one instead would leave the hub
    // green while a role it was configured to recognise has lost its
    // identity — surfacing later as that device failing to connect.
    //
    // Built as a local, then PUBLISHED as an immutable snapshot.  The same
    // routine is what a roster reload re-runs: build a complete new index
    // and swap it, never edit a published one (which `const` forbids).
    {
        pylabhub::utils::security::PeerAuthority::Builder authority;
        for (const auto &kr : pImpl->cfg.known_roles)
        {
            authority.add_local_role(kr);
        }

        for (const auto &peer : pImpl->cfg.peers)
        {
            // An EMPTY peer pubkey is legitimate configuration, not an
            // error: `FederationPeer::pubkey_z85` documents empty as
            // "no CURVE" for that peer.  Such a peer has no key, so it
            // has no entry in a key→subject index and nothing is owed.
            // A non-empty but malformed key is an operator typo that
            // would cost the peer its identity — that one is fatal.
            if (peer.pubkey_z85.empty())
                continue;
            authority.add_federation_peer(peer.hub_uid, peer.pubkey_z85);
        }
        // Built WITHOUT a version.  This index is the vault's answer to
        // "who may ever exist here, and whose key is this" — it gates the
        // broker's own door and names senders, and neither question is
        // replicated to anyone.  The version that travels belongs to the
        // roster, which is this index narrowed to the roles currently
        // registered, and it is carried by `roster_ledger_`
        // (HEP-CORE-0035 §4.9.2).  Stamping this one with a roster version
        // would put a number on an object that is not the thing the number
        // describes.
        pImpl->publish_peer_authority(std::move(authority).build());
    }

    // HEP-CORE-0046 §14.5 admission binder — bind the callbacks the
    // wire::dispatch::receive_and_validate call needs.  Constructed
    // once here so per-request handling never re-binds std::function
    // objects.  Every callback captures the pImpl pointer by value;
    // BrokerServiceImpl is non-movable / non-copyable so the captures
    // remain valid for the impl's lifetime.
    {
        auto *impl = pImpl.get();

        // I-PUBKEY-BINDING: decide the claim against the published
        // authority snapshot (HEP-CORE-0035 §4.2).  The gate receives a
        // verdict and never the roster — it cannot enumerate who this hub
        // recognises, only learn the outcome for one claim.
        //
        // Each call loads the current snapshot, so a roster swap is
        // observed by the next registration without re-binding this
        // callback.
        impl->admission_binder_.callbacks.check_registration =
            [impl](const std::optional<::pylabhub::utils::security::AttestedKey> &attested,
                   std::string_view uid,
                   std::string_view pubkey) -> ::pylabhub::utils::security::ClaimVerdict
        { return impl->peer_authority()->check_registration_claim(attested, uid, pubkey); };

        // The same question for messages acting on an already-registered
        // role (DEREG / ENDPOINT_UPDATE / CHANNEL_AUTH_APPLIED), whose
        // bodies carry no announced key.  Same snapshot, same verdicts.
        impl->admission_binder_.callbacks.check_role_ownership =
            [impl](const std::optional<::pylabhub::utils::security::AttestedKey> &attested,
                   std::string_view uid) -> ::pylabhub::utils::security::ClaimVerdict
        { return impl->peer_authority()->check_role_ownership(attested, uid); };

        // And the attribution form, for the broadcast whose sender the
        // broker stamps instead of the client declaring it.  Same snapshot
        // again: a connection cannot be refused as a claimant here and
        // accepted as an author there.
        impl->admission_binder_.callbacks.attribute_sender =
            [impl](const std::optional<::pylabhub::utils::security::AttestedKey> &attested)
            -> ::pylabhub::utils::security::AttributedSender
        { return impl->peer_authority()->attribute_sender(attested); };

        // I-KEY-ROTATION-VIA-DEREG (HEP-0046): a role's CURVE pubkey is
        // immutable for the broker's lifetime.  Rotation is edit-config
        // + hard-reload + re-REG; an on-the-fly re-REG with a different
        // pubkey is rejected as PUBKEY_MISMATCH by
        // `check_known_role_binding` (KnownRoleLookup::pubkey_mismatch).
        // There is no separate key-rotation gate.

        // I-REPLAY-BOUND: nonce dedup delegated to HubState's per-role
        // sliding-window map (`nonce_seen` returns true = fresh).
        impl->admission_binder_.callbacks.record_and_check_nonce =
            [impl](std::string_view uid, std::string_view nonce)
        {
            // No timestamp crosses here — the ReplayGuard owns its
            // trusted monotonic clock (see ReplayGuard header).
            return impl->hub_state_->nonce_seen(uid, nonce,
                                                impl->admission_binder_.context.nonce_window_ms);
        };

        impl->admission_binder_.callbacks.wall_now_ms = []()
        {
            using namespace std::chrono;
            return static_cast<std::uint64_t>(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
        };

        // Ambient context knobs.  No `broker_proto` field on
        // AdmissionContext (retired per C3 — wire-version + ABI is
        // verified through `abi_fingerprint` at the REG handler per
        // HEP-CORE-0032 §8, not by this admission pipeline).
        impl->admission_binder_.context.skew_tolerance_ms = 30'000ULL;
        // I-REPLAY-BOUND soundness invariant: the nonce window MUST be >=
        // 2 * skew_tolerance_ms.  Dedup is pruned against the TRUSTED broker
        // clock (record_and_check_nonce receives wall_now_ms(), not the
        // client stamp), so a peer cannot force early eviction.  But a
        // replay is skew-acceptable for up to 2*skew after the original
        // (the tolerance bounds both the original acceptance and the
        // replay), so the nonce must be remembered that long or a late-but-
        // skew-valid replay finds its nonce pruned and is wrongly admitted.
        //
        // DERIVED, not restated.  A second literal here would let the two
        // drift apart on any future change to the tolerance -- silently,
        // since nothing would fail to compile and no test would notice.
        // The inbox and admin planes express the same invariant the same
        // way (`2 * kInboxReplaySkewMs`, `2 * kReplaySkewMs`); this is the
        // third plane sharing one rule, so it states it identically.
        impl->admission_binder_.context.nonce_window_ms =
            2 * impl->admission_binder_.context.skew_tolerance_ms;
        impl->admission_binder_.finalize();
    }

    // HEP-CORE-0041 §D1(d) — generate broker's ephemeral observer
    // keypair (task #317 C.2.a).  Called ONCE at broker startup.
    // The pubkey is cached on `pImpl->broker_observer_pubkey_z85`
    // for REG_ACK emission (see PRODUCER_REG_ACK build site).  The
    // seckey stays in KeyStore under `"broker.observer"` — accessed
    // via `with_seckey` in future C.2.d observer-dial code.
    //
    // Broker restart rotates the keypair; producers relearn via the
    // #317 D2 (f7d3a51e) extraction path.  Under no circumstances
    // does the seckey leave the KeyStore module.
    try
    {
        // Idempotent under process-wide KeyStore reuse (L3 test
        // fixtures spin up multiple brokers in one process).
        // `remove` is a no-op if the entry is absent per the
        // HEP-CORE-0040 §5.2 contract, so this is safe on cold
        // start too.  Broker restart semantics = fresh keypair,
        // exactly matching HEP-CORE-0041 §D1(d) design.
        sec::secure().keys().remove("broker.observer");
        pImpl->broker_observer_pubkey_z85 =
            sec::secure().keys().generate_and_add_identity("broker.observer");
        LOGGER_INFO("[broker] event=BrokerObserverKeypairGenerated "
                    "pubkey_z85='{}'",
                    pImpl->broker_observer_pubkey_z85);
    }
    catch (const std::exception &e)
    {
        // Non-fatal: broker still functions without observer
        // capability; SHM channel metrics fall back to
        // heartbeat-piggyback.  Log at ERROR so the degradation is
        // visible without crashing production brokers on a corner
        // case (e.g., name collision under test double-init).
        LOGGER_ERROR("[broker] event=BrokerObserverKeypairGenerationFailed "
                     "reason='{}' (SHM metrics will fall back to heartbeat "
                     "source; HEP-CORE-0041 §D1(d) task #317 C.2.a)",
                     e.what());
    }
}

BrokerService::~BrokerService() = default;

const pylabhub::hub::HubState &BrokerService::hub_state() const
{
    return *pImpl->hub_state_;
}

void BrokerService::run()
{
    pImpl->run();
}

void BrokerService::stop()
{
    pImpl->stop_requested.store(true, std::memory_order_release);
}

std::string BrokerService::list_channels_json_str() const
{
    // HubState snapshot takes its own shared lock internally; no m_query_mu
    // needed here.
    const auto snap = pImpl->hub_state_->snapshot();
    nlohmann::json result = nlohmann::json::array();
    for (const auto &[name, entry] : snap.channels)
    {
        // HEP-CORE-0023 §2.2: `observable` is the protocol-defined
        // wire field — derived from the producer-presence row.
        // Producer fields surface the first producer for back-compat
        // with single-producer admin clients; full list is in
        // `producer_pids`.
        nlohmann::json producer_pids = nlohmann::json::array();
        for (const auto &p : entry.producers)
            producer_pids.push_back(p.producer_pid);
        result.push_back(nlohmann::json{
            {"name", name},
            {"schema_hash", entry.schema_hash},
            {"consumer_count", static_cast<int>(entry.consumers.size())},
            {"producer_pid",
             entry.producers.empty() ? uint64_t{0} : entry.producers.front().producer_pid},
            {"producer_pids", std::move(producer_pids)},
            {"observable", pylabhub::hub::to_string(pylabhub::hub::observe_channel(entry, snap))}});
    }
    return result.dump();
}

ChannelSnapshot BrokerService::query_channel_snapshot() const
{
    const auto hub_snap = pImpl->hub_state_->snapshot();
    ChannelSnapshot snap;
    snap.channels.reserve(hub_snap.channels.size());
    for (const auto &[name, entry] : hub_snap.channels)
    {
        ChannelSnapshotEntry e;
        e.name = name;
        e.observable = pylabhub::hub::to_string(pylabhub::hub::observe_channel(entry, hub_snap));
        e.consumer_count = static_cast<int>(entry.consumers.size());
        e.schema_hash = entry.schema_hash;
        // Multi-producer (HEP-CORE-0023 §2.1.1): parallel uid/pid
        // vectors; no first-producer back-compat scalars (retired in
        // Wave M2.5 step 2c — admin clients must iterate the lists).
        e.producer_uids.reserve(entry.producers.size());
        e.producer_pids.reserve(entry.producers.size());
        for (const auto &prod : entry.producers)
        {
            e.producer_uids.push_back(prod.role_uid);
            e.producer_pids.push_back(prod.producer_pid);
        }
        snap.channels.push_back(std::move(e));
    }
    return snap;
}

RoleStateMetrics BrokerService::query_role_state_metrics() const
{
    // Single-source-of-truth via HubState (HEP-CORE-0033 §8).  HubState
    // takes its own internal lock; m_query_mu not needed here.
    const auto c = pImpl->hub_state_->counters();
    return RoleStateMetrics{
        c.connected_to_pending_total,
        c.pending_to_disconnected_total,
        c.pending_to_connected_total,
    };
}

std::string BrokerService::query_metrics_json_str(const std::string &channel) const
{
    // M1.4 (2026-05-11): metrics live on HubState's per-presence rows
    // (HEP-CORE-0019 §2.3 Phase 6).  Same shape as legacy query_metrics:
    // `{status, channel/channels, metrics}`.
    nlohmann::json resp;
    resp["status"] = "success";
    if (!channel.empty())
    {
        resp["channel"] = channel;
        resp["metrics"] = pImpl->hub_state_->channel_metrics_snapshot(channel);
    }
    else
    {
        nlohmann::json channels = nlohmann::json::object();
        for (const auto &[name, ch] : pImpl->hub_state_->snapshot().channels)
            channels[name] = pImpl->hub_state_->channel_metrics_snapshot(name);
        resp["channels"] = std::move(channels);
    }
    return resp.dump();
}

nlohmann::json BrokerServiceImpl::handle_shm_block_query(const nlohmann::json &req) const
{
    return collect_shm_info(req.value("channel", ""));
}

nlohmann::json BrokerServiceImpl::collect_shm_info(const std::string &channel) const
{
    // Snapshot under HubState's own lock (inside snapshot()), then read SHM
    // outside any broker locks.  HEP-CORE-0036 §5b.4: the SHM segment name
    // is the channel name; no separate `shm_name` field exists anywhere.
    struct BlockInfo
    {
        std::string channel;
        uint64_t producer_pid{0};
        std::string producer_uid;
        std::string producer_name;
        std::vector<pylabhub::hub::ConsumerEntry> consumers;
    };

    std::vector<BlockInfo> blocks;
    {
        const auto snap = hub_state_->snapshot();
        for (const auto &[name, entry] : snap.channels)
        {
            if (!channel.empty() && name != channel)
                continue;
            // SHM channels are identified by data_transport == "shm";
            // the block name is the channel name.
            if (entry.data_transport != "shm")
                continue;
            BlockInfo bi;
            bi.channel = name;
            // SHM channels are physically single-producer (HEP-CORE-0023
            // §2.1.1), so reading the first producer is correct here.
            if (const auto *fp = entry.first_producer())
            {
                bi.producer_pid = fp->producer_pid;
                bi.producer_uid = fp->role_uid;
                bi.producer_name = fp->role_name;
            }
            bi.consumers = entry.consumers;
            blocks.push_back(std::move(bi));
        }
    }

    // For each block, read DataBlockMetrics directly from the SHM header.
    // datablock_get_metrics() opens read-only, reads relaxed-atomic fields, closes.
    nlohmann::json result;
    result["status"] = "success";
    nlohmann::json arr = nlohmann::json::array();

    for (const auto &bi : blocks)
    {
        nlohmann::json blk;
        blk["channel"] = bi.channel;

        nlohmann::json prod;
        prod["pid"] = bi.producer_pid;
        prod["uid"] = bi.producer_uid;
        prod["name"] = bi.producer_name;
        blk["producer"] = std::move(prod);

        nlohmann::json cons_arr = nlohmann::json::array();
        for (const auto &ce : bi.consumers)
        {
            nlohmann::json c;
            c["pid"] = ce.consumer_pid;
            c["uid"] = ce.role_uid;
            c["name"] = ce.role_name;
            cons_arr.push_back(std::move(c));
        }
        blk["consumers"] = std::move(cons_arr);

        DataBlockMetrics m{};
        if (::datablock_get_metrics(bi.channel.c_str(), &m) == 0)
        {
            nlohmann::json sm;
            sm["slot_count"] = m.slot_count;
            sm["commit_index"] = m.commit_index;
            sm["total_slots_written"] = m.total_slots_written;
            sm["total_slots_read"] = m.total_slots_read;
            sm["total_bytes_written"] = m.total_bytes_written;
            sm["total_bytes_read"] = m.total_bytes_read;
            sm["writer_timeout_count"] = m.writer_timeout_count;
            sm["writer_lock_timeout_count"] = m.writer_lock_timeout_count;
            sm["writer_reader_timeout_count"] = m.writer_reader_timeout_count;
            sm["writer_blocked_total_ns"] = m.writer_blocked_total_ns;
            sm["write_lock_contention"] = m.write_lock_contention;
            sm["write_generation_wraps"] = m.write_generation_wraps;
            sm["reader_not_ready_count"] = m.reader_not_ready_count;
            sm["reader_race_detected"] = m.reader_race_detected;
            sm["reader_validation_failed"] = m.reader_validation_failed;
            sm["reader_peak_count"] = m.reader_peak_count;
            sm["checksum_failures"] = m.checksum_failures;
            sm["slot_acquire_errors"] = m.slot_acquire_errors;
            sm["slot_commit_errors"] = m.slot_commit_errors;
            sm["schema_mismatch_count"] = m.schema_mismatch_count;
            sm["recovery_actions_count"] = m.recovery_actions_count;
            sm["last_error_code"] = m.last_error_code;
            sm["last_error_timestamp_ns"] = m.last_error_timestamp_ns;
            sm["uptime_seconds"] = m.uptime_seconds;
            sm["creation_timestamp_ns"] = m.creation_timestamp_ns;
            blk["shm_metrics"] = std::move(sm);
        }
        else
        {
            blk["shm_metrics"] = nullptr; // segment gone (producer already exited)
        }

        arr.push_back(std::move(blk));
    }

    result["blocks"] = std::move(arr);
    return result;
}

std::string BrokerService::collect_shm_info_json(const std::string &channel) const
{
    return pImpl->collect_shm_info(channel).dump();
}

// ============================================================================
// Unified hub-state query engine (HEP-CORE-0033 §10.3)
// ============================================================================

namespace
{

/// Format a system_clock time point for inline use here (filter_to_json /
/// query_metrics top-level fields).  Distinct from the canonical
/// `fmt_time` in `hub_state_json.cpp` only because this file has the
/// existing local helper convention; the two formats are identical.
inline std::string fmt_time(std::chrono::system_clock::time_point tp)
{
    if (tp.time_since_epoch().count() == 0)
        return {};
    return pylabhub::format_tools::formatted_time(tp);
}

/// Predicate: identity selector matches when the list is empty (no filter)
/// or the candidate appears in it.
inline bool include(const std::vector<std::string> &filter, const std::string &candidate)
{
    return filter.empty() || std::find(filter.begin(), filter.end(), candidate) != filter.end();
}

// Entry-type serializers (`channel_to_json` / `role_to_json` /
// `band_to_json` / `peer_to_json` / `broker_counters_to_json`) live in
// `utils/hub_state_json.{hpp,cpp}` so AdminService can produce the same
// on-the-wire shape for HEP-CORE-0033 §11.2 query RPCs.  Pulled in via
// the include at the top of this file.

nlohmann::json filter_to_json(const pylabhub::hub::MetricsFilter &f)
{
    nlohmann::json j;
    j["categories"] = std::vector<std::string>(f.categories.begin(), f.categories.end());
    j["channels"] = f.channels;
    j["roles"] = f.roles;
    j["bands"] = f.bands;
    j["peers"] = f.peers;
    return j;
}

} // anonymous namespace

nlohmann::json BrokerService::query_metrics(const pylabhub::hub::MetricsFilter &filter) const
{
    namespace mc = pylabhub::hub::metrics_category;

    // Snapshot HubState under its own lock; release before SHM reads.
    // M1.4 (2026-05-11): legacy `metrics_store_` snapshot retired —
    // metrics live on `HubState.roles[uid].presences[(ch, role_type)].latest_metrics`
    // and are aggregated per-channel via `channel_metrics_snapshot`.
    pylabhub::hub::HubStateSnapshot snap = pImpl->hub_state_->snapshot();

    nlohmann::json result;
    result["status"] = "success";
    result["queried_at"] = fmt_time(std::chrono::system_clock::now());
    result["filter"] = filter_to_json(filter);

    // ── channels ───────────────────────────────────────────────────────
    if (filter.wants(mc::kChannel))
    {
        nlohmann::json channels = nlohmann::json::object();
        for (const auto &[name, ch] : snap.channels)
        {
            if (!include(filter.channels, name))
                continue;
            auto cj = channel_to_json(ch, observe_channel(ch, snap));
            // M1.4 (2026-05-11): metrics now read from HubState's
            // per-presence rows (HEP-CORE-0019 §2.3 Phase 6) via
            // `channel_metrics_snapshot`.  Wave M2.5 G1's per-uid
            // tree shape is preserved.  Pre-M1.4 path read from
            // `metrics_store_`; that storage layer is retired.
            auto pm = pImpl->hub_state_->channel_metrics_snapshot(name);
            if (pm.contains("producers"))
                cj["producer_metrics"] = std::move(pm["producers"]);
            if (pm.contains("consumers"))
                cj["consumer_metrics"] = std::move(pm["consumers"]);
            channels[name] = std::move(cj);
        }
        result["channels"] = std::move(channels);
    }

    // ── roles ──────────────────────────────────────────────────────────
    if (filter.wants(mc::kRole))
    {
        nlohmann::json roles = nlohmann::json::object();
        for (const auto &[uid, r] : snap.roles)
        {
            if (!include(filter.roles, uid))
                continue;
            roles[uid] = role_to_json(r);
        }
        result["roles"] = std::move(roles);
    }

    // ── bands ──────────────────────────────────────────────────────────
    if (filter.wants(mc::kBand))
    {
        nlohmann::json bands = nlohmann::json::object();
        for (const auto &[name, b] : snap.bands)
        {
            if (!include(filter.bands, name))
                continue;
            bands[name] = band_to_json(b);
        }
        result["bands"] = std::move(bands);
    }

    // ── peers ──────────────────────────────────────────────────────────
    if (filter.wants(mc::kPeer))
    {
        nlohmann::json peers = nlohmann::json::object();
        for (const auto &[uid, p] : snap.peers)
        {
            if (!include(filter.peers, uid))
                continue;
            peers[uid] = peer_to_json(p);
        }
        result["peers"] = std::move(peers);
    }

    // ── broker counters ────────────────────────────────────────────────
    if (filter.wants(mc::kBroker))
    {
        result["broker"] = broker_counters_to_json(snap.counters);
        result["broker"]["_collected_at"] = result["queried_at"];
    }

    // ── shm (pointer-to-collect; reads live shared memory) ─────────────
    // Done last so the broker's internal locks are released before SHM
    // shared-spinlock acquisition.
    if (filter.wants(mc::kShm))
    {
        nlohmann::json shm_blocks = nlohmann::json::object();
        for (const auto &[ch, ref] : snap.shm_blocks)
        {
            if (!include(filter.channels, ch))
                continue;
            // Reuse the existing collector (returns {status, blocks:[...]} ).
            auto info = pImpl->collect_shm_info(ch);
            if (info.contains("blocks") && info["blocks"].is_array() && !info["blocks"].empty())
            {
                auto block = info["blocks"].front();
                block["_collected_at"] = fmt_time(std::chrono::system_clock::now());
                shm_blocks[ch] = std::move(block);
            }
        }
        result["shm"] = std::move(shm_blocks);
    }

    // ── schemas (HEP-CORE-0034 §11) ────────────────────────────────────
    if (filter.wants(mc::kSchema))
    {
        nlohmann::json schemas = nlohmann::json::object();
        for (const auto &[key, rec] : snap.schemas)
        {
            const std::string flat = key.first + ":" + key.second;
            nlohmann::json sj;
            sj["owner_uid"] = key.first;
            sj["schema_id"] = key.second;
            // Both zones, un-merged (HEP-CORE-0034 §6.3); empty ⇒ zone absent.
            sj["packing"] = rec.packing;
            sj["blds"] = rec.blds;
            sj["flexzone_packing"] = rec.flexzone_packing;
            sj["flexzone_blds"] = rec.flexzone_blds;
            sj["hash"] = pylabhub::format_tools::bytes_to_hex(
                std::string_view(reinterpret_cast<const char *>(rec.hash.data()), rec.hash.size()));
            sj["_collected_at"] = fmt_time(rec.registered_at);
            schemas[flat] = std::move(sj);
        }
        result["schemas"] = std::move(schemas);
    }

    return result;
}

void BrokerService::request_close_channel(const std::string &name, const std::string &origin_uid,
                                          const std::string &request_id)
{
    LOGGER_TRACE("Broker: request_close_channel('{}')", name);
    std::lock_guard<std::mutex> lk(pImpl->m_close_req_mu);
    pImpl->close_request_queue_.push_back({name, origin_uid, request_id});
}

void BrokerService::request_broadcast_channel(const std::string &channel,
                                              const std::string &message, const std::string &data,
                                              const std::string &origin_uid,
                                              const std::string &request_id)
{
    std::lock_guard<std::mutex> lk(pImpl->m_broadcast_req_mu);
    pImpl->broadcast_request_queue_.push_back({channel, message, data, origin_uid, request_id});
}

void BrokerService::send_hub_targeted_msg(const std::string &target_hub_uid,
                                          const std::string &channel, const std::string &payload)
{
    std::lock_guard<std::mutex> lk(pImpl->m_hub_targeted_mu);
    pImpl->hub_targeted_queue_.push_back({target_hub_uid, channel, payload});
}

// M1.4 (2026-05-11): `update_producer_metrics`, `update_consumer_metrics`,
// `handle_metrics_report_req` deleted.  Metrics now piggyback on
// HEARTBEAT_NOTIFY and live on `HubState.roles[uid].presences[(ch, role_type)].latest_metrics`.
// See `hub_state.cpp:_on_heartbeat` for the write path and
// `HubState::channel_metrics_snapshot` for the read path.

nlohmann::json BrokerServiceImpl::handle_metrics_req(const nlohmann::json &req)
{
    // M1.4 (2026-05-11): metrics sourced from `HubState`'s per-presence
    // rows (HEP-CORE-0019 §2.3 Phase 6) via `channel_metrics_snapshot`.
    // Legacy `metrics_store_` retired.
    //
    // Per-channel, member-gated pull (2026-07-26): the wire form
    // REQUIRES `channel_name` and answers only channel MEMBERS —
    // metrics are push-in (heartbeat), pull-out per channel.  The
    // former all-channels wire branch is retired: hub-wide aggregation
    // stays on the hub-script / admin plane (`BrokerService::
    // query_metrics`) until an observer role kind exists (#292).
    // `role_uid` is the caller's uid, authenticated by the admission
    // tier (Control_EnvelopeWithRoleUid).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel = req.value("channel_name", "");
    const std::string caller_uid = req.value("role_uid", "");
    if (channel.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "METRICS_REQ requires channel_name (hub-wide aggregation "
                          "is hub-script / admin-plane only)");
    }
    if (caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "METRICS_REQ requires role_uid (the caller's own uid — "
                          "the caller's own uid)");
    }
    if (!hub_state_->channel(channel).has_value())
    {
        // Queries answer from state — Absent is terminal.
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel + "' is not registered");
    }
    if (!hub_state_->is_role_registered_on_channel(channel, caller_uid, "producer") &&
        !hub_state_->is_role_registered_on_channel(channel, caller_uid, "consumer"))
    {
        LOGGER_WARN("Broker: METRICS_REQ rejected — role_uid='{}' is not a member of "
                    "channel '{}'",
                    caller_uid, channel);
        return make_error(corr_id, "NOT_A_ROLE_OF_CHANNEL",
                          "Caller role_uid='" + caller_uid +
                              "' is not a registered role of "
                              "channel '" +
                              channel + "'");
    }

    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel"] = channel;
    resp["metrics"] = hub_state_->channel_metrics_snapshot(channel);
    // HEP-CORE-0019 §3.2: merge live SHM-derived block metrics into the response.
    resp["shm_blocks"] = collect_shm_info(channel);
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// M1.4 (2026-05-11): `BrokerServiceImpl::query_metrics(channel)` deleted.
// Replaced by `HubState::channel_metrics_snapshot(channel)` called from
// `handle_metrics_req` and `BrokerService::query_metrics(MetricsFilter)`.

// ============================================================================
// Hub Federation handlers (HEP-CORE-0022)
// ============================================================================

void BrokerServiceImpl::handle_hub_peer_hello(zmq::socket_t &socket, const zmq::message_t &identity,
                                              const nlohmann::json &payload)
{
    const std::string peer_hub_uid = payload.value("hub_uid", "");
    if (peer_hub_uid.empty())
    {
        LOGGER_WARN("Broker: HUB_PEER_HELLO missing hub_uid — ignored");
        return;
    }

    // Find the relay_channels this hub is configured to send to this peer.
    // HEP-CORE-0022 §3: "static topology — configured peers only, no runtime discovery."
    // Reject inbound HELLO from any hub_uid not found in cfg.peers.
    std::vector<std::string> relay_channels;
    bool peer_configured = false;
    for (const auto &pc : cfg.peers)
    {
        if (pc.hub_uid == peer_hub_uid)
        {
            relay_channels = pc.channels;
            peer_configured = true;
            break;
        }
    }
    if (!peer_configured)
    {
        LOGGER_WARN("Broker: rejecting HUB_PEER_HELLO from unconfigured hub '{}' — "
                    "only configured peers are accepted (HEP-0022 static topology).",
                    peer_hub_uid);
        nlohmann::json nack;
        nack["status"] = "rejected";
        nack["reason"] = "hub_uid not in configured peers";
        send_reply(socket, identity, "HUB_PEER_HELLO_ACK", nack);
        return;
    }

    const std::string identity_str(static_cast<const char *>(identity.data()), identity.size());

    // [BR2] If this peer was already Connected (reconnect after crash), fire
    // on_hub_disconnected for the old entry before overwriting it.  HubState's
    // _on_peer_connected does insert_or_assign internally; we just need to
    // emit the disconnected callback once before overwriting.
    if (auto pre = hub_state_->peer(peer_hub_uid);
        pre.has_value() && pre->state == pylabhub::hub::PeerState::Connected)
    {
        LOGGER_INFO("Broker: federation peer '{}' re-connected — treating as reconnect",
                    peer_hub_uid);
        hub_connected_notified_.erase(peer_hub_uid); // Allow re-notification after reconnect.
        if (cfg.on_hub_disconnected)
            cfg.on_hub_disconnected(peer_hub_uid);
    }

    pylabhub::hub::PeerEntry pe;
    pe.uid = peer_hub_uid;
    pe.zmq_identity = identity_str;
    pe.relay_channels = relay_channels;
    pe.last_seen = std::chrono::steady_clock::now();
    // state defaults to Connecting; _on_peer_connected forces Connected.
    hub_state_->_on_peer_connected(std::move(pe));

    LOGGER_INFO("Broker: federation peer '{}' connected; relay_channels=[{}]", peer_hub_uid,
                [&]
                {
                    std::string s;
                    for (const auto &c : relay_channels)
                    {
                        if (!s.empty())
                            s += ',';
                        s += c;
                    }
                    return s;
                }());

    // Send ACK.
    nlohmann::json ack;
    ack["status"] = "ok";
    ack["accepted_channels"] = relay_channels;
    ack["hub_uid"] = cfg.self_hub_uid;
    send_reply(socket, identity, "HUB_PEER_HELLO_ACK", ack);

    // [BR1] Fire on_hub_connected only if not already notified (prevents double-fire
    // in bidirectional federation where each side sends HELLO and receives an ACK).
    if (cfg.on_hub_connected && hub_connected_notified_.insert(peer_hub_uid).second)
        cfg.on_hub_connected(peer_hub_uid);
}

void BrokerServiceImpl::handle_hub_peer_bye(const nlohmann::json &payload)
{
    const std::string peer_hub_uid = payload.value("hub_uid", "");
    if (peer_hub_uid.empty())
        return;

    auto pre = hub_state_->peer(peer_hub_uid);
    if (!pre.has_value() || pre->state != pylabhub::hub::PeerState::Connected)
        return;

    // HEP-CORE-0033 §8 retention: peer entry stays with state=Disconnected;
    // observable via snapshot until grace eviction (deferred work).
    hub_state_->_on_peer_disconnected(peer_hub_uid);
    hub_connected_notified_.erase(peer_hub_uid); // [BR1] Allow re-notification on reconnect.

    LOGGER_INFO("Broker: federation peer '{}' sent BYE — marked Disconnected", peer_hub_uid);
    if (cfg.on_hub_disconnected)
        cfg.on_hub_disconnected(peer_hub_uid);
}

void BrokerServiceImpl::handle_hub_peer_hello_ack(const std::string &peer_hub_uid,
                                                  const nlohmann::json &payload)
{
    const std::string status = payload.value("status", "error");
    if (status == "ok")
    {
        LOGGER_INFO("Broker: federation HUB_PEER_HELLO_ACK from '{}' — connected", peer_hub_uid);
        // [BR1] Only fire on_hub_connected once per peer (insert returns false if already present).
        if (cfg.on_hub_connected && hub_connected_notified_.insert(peer_hub_uid).second)
            cfg.on_hub_connected(peer_hub_uid);
    }
    else
    {
        LOGGER_WARN("Broker: federation HUB_PEER_HELLO_ACK from '{}': status={}", peer_hub_uid,
                    status);
    }
}

void BrokerServiceImpl::handle_hub_relay_msg(zmq::socket_t &socket, const nlohmann::json &payload)
{
    // Protocol invariant: relay=true means never re-relay to our own peers.
    // [BR7] Check dedup using O(1) set lookup; insert into ordered deque for O(expired) prune.
    const std::string msg_id = payload.value("msg_id", "");
    if (!msg_id.empty())
    {
        if (relay_dedup_set_.count(msg_id) > 0)
        {
            LOGGER_DEBUG("Broker: HUB_RELAY_MSG dedup drop msg_id='{}'", msg_id);
            return;
        }
        const auto expiry = std::chrono::steady_clock::now() + kRelayDedupeWindow;
        relay_dedup_set_.insert(msg_id);
        relay_dedup_queue_.push_back({msg_id, expiry});
    }

    const std::string channel = payload.value("channel_name", "");
    const std::string event = payload.value("event", "");
    const std::string sender_uid = payload.value("sender_uid", "");
    const std::string originator = payload.value("originator_uid", "");

    if (channel.empty())
    {
        LOGGER_WARN("Broker: HUB_RELAY_MSG missing channel_name");
        return;
    }

    // Deliver locally as CHANNEL_EVENT_NOTIFY to every registered
    // producer (HEP-CORE-0023 §2.1.1 multi-producer fan-out — relayed
    // events apply to all producer-presences on the channel).
    auto entry = hub_state_->channel(channel);
    if (!entry || entry->producers.empty())
    {
        LOGGER_DEBUG("Broker: HUB_RELAY_MSG for '{}' — no local channel or producer", channel);
        return;
    }

    // [BR3] Include originator_uid (non-empty = relayed from federation peer) so the script
    // can detect the relay origin and avoid re-notifying (which would create an app-level loop).
    nlohmann::json fwd;
    fwd["channel_name"] = channel;
    fwd["event"] = event;
    fwd["sender_uid"] = sender_uid;
    fwd["originator_uid"] = originator; // Non-empty = relayed from a federation peer
    if (payload.contains("payload"))
        fwd["data"] = payload["payload"];

    for (const auto &prod : entry->producers)
    {
        if (prod.zmq_identity.empty())
            continue;
        send_to_identity(socket, prod.zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
    }
    LOGGER_DEBUG("Broker: HUB_RELAY_MSG '{}' event='{}' from hub '{}' ->{} local producer(s)",
                 channel, event, originator, entry->producers.size());
}

void BrokerServiceImpl::handle_hub_targeted_msg(const nlohmann::json &payload)
{
    // [BR4] Log a warning so operators know targeted messages are being silently dropped.
    if (!cfg.on_hub_message)
    {
        const std::string ch = payload.value("channel_name", "?");
        LOGGER_WARN(
            "Broker: HUB_TARGETED_MSG for channel '{}' dropped — on_hub_message not configured",
            ch);
        return;
    }

    const std::string channel = payload.value("channel_name", "");
    const std::string p_payload = payload.value("payload", "");
    const std::string sender_uid = payload.value("sender_uid", "");
    cfg.on_hub_message(channel, p_payload, sender_uid);
}

void BrokerServiceImpl::relay_notify_to_peers(zmq::socket_t &socket, const std::string &channel,
                                              const std::string &event,
                                              const std::string &sender_uid,
                                              const std::string &data)
{
    if (cfg.self_hub_uid.empty())
        return;

    // Compute relay targets from HubState's PeerEntry data (state==Connected
    // AND channel ∈ relay_channels).  For a small N of peers this is fine;
    // a precomputed reverse index is documented as a future optimization.
    const auto snap = hub_state_->snapshot();
    std::vector<std::string> targets;
    for (const auto &[uid, peer] : snap.peers)
    {
        if (peer.state != pylabhub::hub::PeerState::Connected)
            continue;
        if (peer.zmq_identity.empty())
            continue;
        if (std::find(peer.relay_channels.begin(), peer.relay_channels.end(), channel) ==
            peer.relay_channels.end())
            continue;
        targets.push_back(peer.zmq_identity);
    }
    if (targets.empty())
        return;

    const std::string msg_id = cfg.self_hub_uid + ":" + std::to_string(relay_seq_++);

    for (const auto &peer_identity : targets)
    {
        nlohmann::json relay;
        relay["relay"] = true;
        relay["channel_name"] = channel;
        relay["originator_uid"] = cfg.self_hub_uid;
        relay["msg_id"] = msg_id;
        relay["event"] = event;
        relay["sender_uid"] = sender_uid;
        if (!data.empty())
            relay["payload"] = data;

        try
        {
            send_to_identity(socket, peer_identity, "HUB_RELAY_MSG", relay);
            LOGGER_DEBUG("Broker: relayed '{}' event='{}' to peer identity '{}'", channel, event,
                         peer_identity);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("Broker: relay to peer '{}' for channel '{}' failed: {}", peer_identity,
                        channel, e.what());
        }
    }
}

void BrokerServiceImpl::prune_relay_dedup()
{
    // [BR7] O(expired) prune: pop from the front of the ordered deque while expired.
    const auto now = std::chrono::steady_clock::now();
    while (!relay_dedup_queue_.empty() && relay_dedup_queue_.front().expiry <= now)
    {
        relay_dedup_set_.erase(relay_dedup_queue_.front().msg_id);
        relay_dedup_queue_.pop_front();
    }
}

// ============================================================================
// Band pub/sub handlers (HEP-CORE-0030)
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_band_join_req(const nlohmann::json &req,
                                                       const zmq::message_t &identity,
                                                       zmq::socket_t &socket)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.  The C++
    // variable holds the band identifier (`!`-prefixed per §3); name
    // it `band` to match the wire and the HEP — completes the
    // 2026-04-11 rename refactor (`8d3ee1e`) for the wire layer.
    const std::string corr_id = req.value("correlation_id", "");
    const std::string band = req.value("band", "");
    const std::string role_uid = req.value("role_uid", "");
    const std::string role_name = req.value("role_name", "");

    if (band.empty() || role_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing band or role_uid");
    }

    // Audit R3.5 (2026-05-17): validate the band identifier
    // explicitly at the handler boundary BEFORE invoking
    // `hub_state_->_on_band_joined`.  Pre-fix, an invalid identifier
    // (e.g., no `!` prefix per HEP-CORE-0030 §3) was silently
    // swallowed by `_on_band_joined`'s validator + counter-bump
    // pattern — but the handler ignored the validation outcome and
    // still returned `status: success` to the role, creating a
    // phantom "joined" state on the role side with no broker-side
    // membership.  Fix returns a typed error so the role can act.
    if (!pylabhub::hub::is_valid_identifier(band, pylabhub::hub::IdentifierKind::Band))
    {
        LOGGER_WARN("Broker: BAND_JOIN_REQ rejected — invalid band "
                    "identifier '{}' (HEP-CORE-0030 §3 — must be "
                    "`!`-prefixed dotted identifier)",
                    band);
        return make_error(corr_id, "INVALID_BAND_NAME",
                          "Band identifier failed validation "
                          "(HEP-CORE-0030 §3 grammar)");
    }
    // Audit R3.5b (2026-05-19): role_uid grammar + tag check — any
    // role may join a band, so accept {prod, cons, proc}.  Pre-fix, a
    // malformed value (e.g. empty) would survive into `BandMember.
    // role_uid` and fail downstream BAND_LEAVE matches + BAND_LEAVE_
    // NOTIFY fan-out.
    // Audit B1 (2026-05-20): corr_id is now threaded into the
    // validator error so the role-side response matcher routes the
    // rejection to the right pending `do_request` (other gates were
    // already doing this; this one was missed).
    // BAND_JOIN_REQ is in Tier::Control_EnvelopeWithRoleUid, so
    // `run_control_gates` has already vetted `role_uid` — including
    // that this connection owns it, without which one role could join
    // a band under another's name.  What that runner covers is
    // documented there, not restated here.  What it does NOT cover is
    // the reason for the checks below: the pipeline validates
    // `role_uid` and `channel_name`, never `band` or `role_name`.
    if (!role_name.empty() &&
        !pylabhub::hub::is_valid_identifier(role_name, pylabhub::hub::IdentifierKind::RoleName))
    {
        LOGGER_WARN("Broker: BAND_JOIN_REQ rejected on band '{}' "
                    "uid='{}' — invalid role_name '{}' "
                    "(HEP-CORE-0033 §G2.2.0b)",
                    band, role_uid, role_name);
        return make_error(corr_id, "INVALID_REQUEST",
                          "role_name '" + role_name +
                              "' failed grammar validation "
                              "(HEP-CORE-0033 §G2.2.0b)");
    }

    const std::string id_str(static_cast<const char *>(identity.data()), identity.size());

    // Notify existing members before adding the new one.  Read from HubState
    // snapshot so the strict identifier validation has the final say on
    // which bands/members exist.
    nlohmann::json notify;
    notify["band"] = band;
    notify["role_uid"] = role_uid;
    notify["role_name"] = role_name;
    if (auto pre_band = hub_state_->band(band); pre_band.has_value())
    {
        for (const auto &m : pre_band->members)
        {
            if (!m.zmq_identity.empty())
                send_to_identity(socket, m.zmq_identity, "BAND_JOIN_NOTIFY", notify);
        }
    }

    pylabhub::hub::BandMember member;
    member.role_uid = role_uid;
    member.role_name = role_name;
    member.zmq_identity = id_str;
    hub_state_->_on_band_joined(band, std::move(member));

    LOGGER_INFO("Broker: BAND_JOIN '{}' role='{}'", band, role_uid);

    nlohmann::json members_json = nlohmann::json::array();
    if (auto post_band = hub_state_->band(band); post_band.has_value())
    {
        for (const auto &m : post_band->members)
        {
            members_json.push_back({{"role_uid", m.role_uid}, {"role_name", m.role_name}});
        }
    }

    nlohmann::json resp;
    resp["status"] = "success";
    resp["band"] = band;
    resp["members"] = std::move(members_json);
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_band_leave_req(const nlohmann::json &req,
                                                        zmq::socket_t & /*socket*/)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    const std::string corr_id = req.value("correlation_id", "");
    const std::string band = req.value("band", "");
    const std::string role_uid = req.value("role_uid", "");

    if (band.empty() || role_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing band or role_uid");
    }

    // Audit R3.5 (2026-05-17): explicit band-name validation —
    // mirror of handle_band_join_req.  An invalid identifier hitting
    // `_on_band_left` would be silently swallowed without us telling
    // the caller.
    if (!pylabhub::hub::is_valid_identifier(band, pylabhub::hub::IdentifierKind::Band))
    {
        LOGGER_WARN("Broker: BAND_LEAVE_REQ rejected — invalid band "
                    "identifier '{}'",
                    band);
        return make_error(corr_id, "INVALID_BAND_NAME",
                          "Band identifier failed validation "
                          "(HEP-CORE-0030 §3 grammar)");
    }
    // Audit R3.5b (2026-05-19): role_uid grammar + tag check (HEP-
    // CORE-0033 §G2.2.0b).  Any role may leave a band — same tag set
    // as BAND_JOIN_REQ.  Pre-fix, malformed uid would scan-miss in
    // the membership loop and skip the LEAVE log.
    // Audit B1 (2026-05-20): corr_id is now threaded through (was
    // empty, response matcher couldn't route).
    // BAND_LEAVE_REQ mirrors BAND_JOIN_REQ in
    // Tier::Control_EnvelopeWithRoleUid, so `run_control_gates` has
    // already vetted `role_uid` — ownership included, without which one
    // role could remove another from a band.  Same division as the join
    // handler: `band` is not a field that pipeline looks at.

    // Wave M3 step 5f (2026-05-11): BAND_LEAVE_NOTIFY fanout is
    // handler-driven via `subscribe_band_left` wired in run().  The
    // subscriber's `send_band_leave_notify` fires only on real removal
    // (because `_on_band_left` fires its handler only when a member
    // was actually removed).
    //
    // S4-1 (2026-05-19, HEP-CORE-0030 amendment): sender-must-be-member
    // gate.  Pre-fix the handler always returned `status: success`
    // even when the sender wasn't actually in the band — the
    // `was_member` flag was used only for the INFO log gate, not the
    // response shape.  That violates the broker-authority principle:
    // the role-side bookkeeping cannot mirror a truth the broker
    // hides.  Now a `LEAVE` from a non-member returns typed
    // `NOT_A_MEMBER` so the role-side `band_leave` on `{status:
    // error}` can erase its stale `band_index_` entry.
    bool was_member = false;
    if (auto pre = hub_state_->band(band); pre.has_value())
    {
        for (const auto &m : pre->members)
        {
            if (m.role_uid == role_uid)
            {
                was_member = true;
                break;
            }
        }
    }
    if (!was_member)
    {
        LOGGER_WARN("Broker: BAND_LEAVE_REQ from '{}' rejected — not a "
                    "member of band '{}' (HEP-CORE-0030 §5.1 "
                    "membership rule)",
                    role_uid, band);
        return make_error(corr_id, "NOT_A_MEMBER",
                          "Sender '" + role_uid +
                              "' is not a member "
                              "of band '" +
                              band + "'");
    }
    hub_state_->_on_band_left(band, role_uid);
    LOGGER_INFO("Broker: BAND_LEAVE '{}' role='{}'", band, role_uid);

    nlohmann::json resp;
    resp["status"] = "success";
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

void BrokerServiceImpl::handle_band_broadcast_req(zmq::socket_t &socket, const nlohmann::json &req,
                                                  const zmq::message_t & /*identity*/)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): the sender field
    // was renamed `sender_uid` → `role_uid` for consistency with all
    // other gates.  The sender IS a role, no different identifier
    // shape — uniform naming simplifies role-side code.
    const std::string band_name = req.value("band", "");
    const std::string role_uid = req.value("role_uid", "");

    if (band_name.empty())
        return;

    // Audit R3.5 (2026-05-17): silent-drop on invalid identifier.
    // BAND_BROADCAST_SEND_NOTIFY is fire-and-forget so there is no error
    // response to return; we log + drop.  The caller cannot
    // observe the failure (matches existing fire-and-forget
    // semantics) — operators see the WARN log.
    if (!pylabhub::hub::is_valid_identifier(band_name, pylabhub::hub::IdentifierKind::Band))
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_SEND_NOTIFY dropped — invalid band "
                    "identifier '{}' (HEP-CORE-0030 §3)",
                    band_name);
        return;
    }
    // Audit R3.5b (2026-05-19): role_uid grammar + tag check.  Any
    // role may broadcast — accept {prod, cons, proc}.  Drop with
    // WARN log (fire-and-forget).
    if (!pylabhub::hub::is_valid_identifier(role_uid, pylabhub::hub::IdentifierKind::RoleUid))
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_SEND_NOTIFY dropped on band '{}' — "
                    "invalid role_uid '{}' (HEP-CORE-0033 §G2.2.0b)",
                    band_name, role_uid);
        return;
    }

    // S4-2 (2026-05-19, HEP-CORE-0030 amendment): sender-must-be-member
    // gate on broadcast.  Pre-fix, the handler fanned out the
    // broadcast to all members regardless of whether the sender was
    // actually a member of the band — accepting broadcasts from
    // non-members.  That undermines the broker-authority principle:
    // membership rules don't apply uniformly across band ops.  Now
    // a non-member's BAND_BROADCAST_SEND_NOTIFY is dropped + WARN'd.
    // Fire-and-forget so no reply is emitted; operators see the WARN.
    auto band = hub_state_->band(band_name);
    if (!band.has_value())
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_SEND_NOTIFY dropped — band '{}' "
                    "does not exist (sender uid='{}')",
                    band_name, role_uid);
        return;
    }
    bool sender_is_member = false;
    for (const auto &m : band->members)
    {
        if (m.role_uid == role_uid)
        {
            sender_is_member = true;
            break;
        }
    }
    if (!sender_is_member)
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_SEND_NOTIFY dropped — sender '{}' "
                    "is not a member of band '{}' (HEP-CORE-0030 §5.2 "
                    "sender-must-be-member rule)",
                    role_uid, band_name);
        return;
    }

    nlohmann::json notify;
    notify["band"] = band_name;
    notify["role_uid"] = role_uid;
    notify["body"] = req.value("body", nlohmann::json::object());

    std::size_t recipients = 0;
    for (const auto &m : band->members)
    {
        if (m.role_uid == role_uid)
            continue;
        if (m.zmq_identity.empty())
            continue;
        send_to_identity(socket, m.zmq_identity, "BAND_BROADCAST_DELIVER_NOTIFY", notify);
        ++recipients;
    }

    LOGGER_DEBUG("Broker: BAND_BROADCAST '{}' from '{}' ->{} recipients", band_name, role_uid,
                 recipients);
}

nlohmann::json BrokerServiceImpl::handle_band_members_req(const nlohmann::json &req)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    const std::string band_name = req.value("band", "");

    // I-CORRELATION-STABLE: BAND_MEMBERS_REQ is a REQ, so BAND_MEMBERS_ACK
    // and ERROR replies MUST echo the caller's correlation_id.  Missing
    // this echo makes `WireEnvelope::build_router_send` throw at send
    // time (empty correlation_id on non-NOTIFY = wire violation), the
    // reply gets dropped, and the caller sees a 5000 ms timeout.
    const std::string corr_id = req.value("correlation_id", std::string{});

    // Audit R3.5 (2026-05-17): explicit band-name validation.  Pre-fix
    // an invalid identifier would silently miss in `hub_state_->band()`
    // (because no band by that invalid name exists) and we'd return
    // an empty members array — indistinguishable from "valid name,
    // empty membership".  Returning a typed error lets callers
    // distinguish the two.
    if (!band_name.empty() &&
        !pylabhub::hub::is_valid_identifier(band_name, pylabhub::hub::IdentifierKind::Band))
    {
        LOGGER_WARN("Broker: BAND_MEMBERS_REQ rejected — invalid band "
                    "identifier '{}'",
                    band_name);
        return make_error(corr_id, "INVALID_BAND_NAME",
                          "Band identifier failed validation "
                          "(HEP-CORE-0030 §3 grammar)");
    }

    nlohmann::json members_json = nlohmann::json::array();
    if (auto band = hub_state_->band(band_name); band.has_value())
    {
        for (const auto &m : band->members)
        {
            members_json.push_back({{"role_uid", m.role_uid}, {"role_name", m.role_name}});
        }
    }

    nlohmann::json resp;
    resp["band"] = band_name;
    resp["members"] = std::move(members_json);
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

} // namespace pylabhub::broker
