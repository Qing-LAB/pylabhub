#include "utils/broker_service.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_queries.hpp"  // HEP-CORE-0039 for_each_presence_matching
#include "utils/hub_state_json.hpp"
#include "utils/naming.hpp"   // is_valid_identifier (audit R3.5)
#include "utils/net_address.hpp"


#include "utils/recovery_api.hpp"
#include "utils/schema_loader.hpp"
#include "utils/schema_utils.hpp"  // canonical_fields_str, compute_canonical_hash_from_wire

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/curve_keypair.hpp"    // HEP-CORE-0035 §2 — shared keygen
#include "utils/security/key_store.hpp"        // HEP-CORE-0040 §172 — hub identity
#include "utils/security/peer_admission.hpp"   // HEP-CORE-0035 Phase D
#include "utils/security/zap_router.hpp"       // HEP-CORE-0035 Phase D
#include "portable_atomic_shared_ptr.hpp"      // sibling header in src/utils/
#include "utils/timeout_constants.hpp"
#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "utils/json_fwd.hpp"
#include <sodium.h>
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

/// Convert a 64-char hex-encoded schema hash string ->std::array<uint8_t, 32>.
/// Returns a zero-filled array on format error (wrong length or invalid hex).
std::array<uint8_t, 32> hex_to_hash_array(const std::string& hex) noexcept
{
    std::array<uint8_t, 32> result{};
    if (hex.size() != 64) return result;
    const auto decoded = format_tools::bytes_from_hex(hex);
    if (decoded.size() != 32) return result; // invalid chars ->bytes_from_hex returned original
    std::memcpy(result.data(), decoded.data(), 32);
    return result;
}

/// Returns true if channel_name matches the glob pattern ('*' wildcard only).
/// Dots are treated as literal characters, not anchors.
bool channel_name_matches_glob(const std::string& name, const std::string& glob) noexcept
{
    const char* n       = name.c_str();
    const char* g       = glob.c_str();
    const char* star_g  = nullptr; ///< Position of the last '*' in glob
    const char* star_n  = nullptr; ///< Saved name position after last '*' mismatch

    while (*n != '\0')
    {
        if (*g == '*')
        {
            star_g = g++;
            star_n = n;
        }
        else if (*g == *n)
        {
            ++g;
            ++n;
        }
        else if (star_g != nullptr)
        {
            g = star_g + 1;
            n = ++star_n;
        }
        else
        {
            return false;
        }
    }
    while (*g == '*')
    {
        ++g;
    }
    return *g == '\0';
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

constexpr std::array<std::string_view, 18> kRequestReplyTypes = {
    "REG_REQ", "DISC_REQ", "DEREG_REQ",
    "CONSUMER_REG_REQ", "CONSUMER_DEREG_REQ",
    "ENDPOINT_UPDATE_REQ", "SCHEMA_REQ",
    "CHANNEL_LIST_REQ", "METRICS_REQ", "SHM_BLOCK_QUERY_REQ",
    "ROLE_PRESENCE_REQ", "ROLE_INFO_REQ",
    "BAND_JOIN_REQ", "BAND_LEAVE_REQ", "BAND_MEMBERS_REQ",
    "HUB_PEER_HELLO",
    // Producer queries the current channel-scope allowlist on receipt
    // of CHANNEL_AUTH_CHANGED_NOTIFY (or proactively at setup) per
    // HEP-CORE-0036 §6.5.  Standard request-reply via existing
    // do_request infrastructure.
    "GET_CHANNEL_AUTH_REQ",
    // Producer asks the broker to confirm one specific consumer is
    // authorized for a channel before sending the SHM capability fd
    // (HEP-CORE-0041 §9 D4 pre-attach confirmation).  Reply is
    // CONSUMER_ATTACH_ACK with status=success|denied for the auth
    // decision, or ERROR for protocol-level failures.  Read-only
    // against HubState — pure query, no mutation.
    "CONSUMER_ATTACH_REQ",
};

constexpr std::array<std::string_view, 8> kFireAndForgetTypes = {
    // M1.4 (2026-05-11) — `METRICS_REPORT_REQ` retired.  Metrics now
    // piggyback on `HEARTBEAT_REQ` per HEP-CORE-0019 §2.3 Phase 6.
    "HEARTBEAT_REQ", "CHECKSUM_ERROR_REPORT",
    // Audit R3.6 (2026-05-17): CHANNEL_NOTIFY_REQ removed.  Role-side
    // BRC::send_notify was deleted in O1; federation peer-relay uses
    // HUB_RELAY_MSG (broker↔broker), not CHANNEL_NOTIFY_REQ.  No
    // caller exists anywhere in src/.  Old clients sending
    // CHANNEL_NOTIFY_REQ now receive UNKNOWN_MSG_TYPE.
    "CHANNEL_BROADCAST_REQ", "BAND_BROADCAST_REQ",
    "HUB_PEER_BYE",
    // Inbound on outbound DEALER (peer→us); not request-reply but valid:
    "HUB_RELAY_MSG", "HUB_TARGETED_MSG",
};

[[nodiscard]] bool is_request_reply(std::string_view t) noexcept
{
    for (auto x : kRequestReplyTypes) if (x == t) return true;
    return false;
}

[[nodiscard]] bool is_fire_and_forget(std::string_view t) noexcept
{
    for (auto x : kFireAndForgetTypes) if (x == t) return true;
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
        current_.store(std::make_shared<
            pylabhub::utils::security::PeerAllowlist>(std::move(initial)));
    }

    bool set_peer_allowlist(
        pylabhub::utils::security::PeerAllowlist allowlist) override
    {
        current_.store(std::make_shared<
            pylabhub::utils::security::PeerAllowlist>(std::move(allowlist)));
        return true;
    }

    [[nodiscard]] std::optional<pylabhub::utils::security::PeerAllowlist>
    peer_allowlist_snapshot() const override
    {
        auto p = current_.load();
        if (!p) return std::nullopt;
        return *p;
    }

    [[nodiscard]] bool
    is_peer_allowed(
        const pylabhub::utils::security::PeerIdentity &peer) const override
    {
        auto p = current_.load();
        if (!p) return false;
        return p->contains(peer);
    }

private:
    pylabhub::utils::detail::PortableAtomicSharedPtr<
        pylabhub::utils::security::PeerAllowlist>
        current_;
};

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
        if (hub_state_ != nullptr &&
            band_left_handler_id_ != pylabhub::hub::kInvalidHandlerId)
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

    /// HEP-CORE-0033 §8 state aggregate.  Sole owner of channel / role /
    /// band / peer / shm / counter state; updated only via the broker's
    /// `_on_*` capability ops (friend access).  Per HEP-CORE-0033 §4,
    /// ownership lives in `HubHost`; the broker holds a non-owning
    /// pointer.  In tests, the L3 fixture (`LocalBrokerHandle`) plays
    /// the HubHost role and owns the HubState alongside the broker.
    pylabhub::hub::HubState *hub_state_{nullptr};
    std::atomic<bool>     stop_requested{false};

    /// Serializes the run() thread's post-poll work against external
    /// readers (e.g. `list_channels_json_str()`).  HubState has its own
    /// internal mutex; this one only protects broker-private structures
    /// (request queues, federation session flags) and orders post-poll
    /// drainage with respect to external query callers.
    mutable std::mutex    m_query_mu;

    /// Guards close_request_queue_ for thread-safe request_close_channel().
    mutable std::mutex    m_close_req_mu;
    /// Script-requested channel closes; drained in run() post-poll phase under m_query_mu.
    std::deque<std::string> close_request_queue_;

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
    zmq::socket_t          *active_router_{nullptr};
    pylabhub::hub::HandlerId band_left_handler_id_{pylabhub::hub::kInvalidHandlerId};

    // The CTRL ZAP admission (HEP-CORE-0035 §4.8 + HEP-CORE-0036 §4.2)
    // is wired inside `run()` as locals so its `ZapDomainHandle`
    // unregisters at run-exit (same scope as the ROUTER socket).
    // Storing it as a member would let the registration outlive a
    // hypothetical run() restart and throw on second `register_domain`
    // (per `zap_router.hpp:99`).

    /// Guards broadcast_request_queue_ for thread-safe request_broadcast_channel().
    mutable std::mutex    m_broadcast_req_mu;
    /// Admin-shell-requested broadcasts; drained in run() post-poll phase under m_query_mu.
    struct BroadcastRequest { std::string channel, message, data; };
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
        std::string                            msg_id;
        std::chrono::steady_clock::time_point  expiry;
    };
    std::deque<RelayDedupEntry>       relay_dedup_queue_; ///< Ordered by expiry (monotone insert)
    std::unordered_set<std::string>   relay_dedup_set_;   ///< O(1) existence check

    /// Monotone sequence counter for outgoing HUB_RELAY_MSG msg_id.
    uint64_t relay_seq_{0};

    /// Thread-safe queue for send_hub_targeted_msg().
    struct HubTargetedRequest { std::string target_hub_uid, channel, payload; };
    mutable std::mutex             m_hub_targeted_mu;
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

    void process_message(zmq::socket_t&       socket,
                         const zmq::message_t& identity,
                         const std::string&    msg_type,
                         const nlohmann::json& payload,
                         std::size_t           bytes_in);

    /// HEP-CORE-0033 §9.6: invoke `cfg.on_processing_error` if configured,
    /// catching any user-supplied callback exception (R2).  `identity` may
    /// be nullptr for failures that happen before the routing identity is
    /// known (peer-DEALER inbound, S1/S2 errors).
    void emit_processing_error(const std::string&    msg_type,
                               const std::string&    error_kind,
                               const std::string&    detail,
                               const zmq::message_t* identity);

    nlohmann::json handle_reg_req(const nlohmann::json& req,
                                   const zmq::message_t& identity,
                                   zmq::socket_t&        socket);
    /// Always returns a response. The returned JSON's status field indicates
    /// the DISC response variant: "success" (DISC_ACK), "pending" (DISC_PENDING),
    /// or "error" (CHANNEL_NOT_FOUND). See HEP-CORE-0023 §2.2.
    nlohmann::json handle_disc_req(const nlohmann::json& req);
    nlohmann::json handle_dereg_req(const nlohmann::json& req, zmq::socket_t& socket);
    nlohmann::json handle_consumer_reg_req(const nlohmann::json& req,
                                           const zmq::message_t& identity,
                                           zmq::socket_t&        socket);
    nlohmann::json handle_consumer_dereg_req(zmq::socket_t& socket,
                                              const nlohmann::json& req);
    void           handle_heartbeat_req(const nlohmann::json& req);

    /// `GET_CHANNEL_AUTH_REQ` handler (HEP-CORE-0036 §6.5).  Returns
    /// the channel's current `authorized_consumer_pubkeys` allowlist
    /// to a producer that asks.  Errors: `CHANNEL_NOT_FOUND` (channel
    /// does not exist), `PRODUCER_NOT_AUTHORIZED` (caller's role_uid
    /// is not a registered producer of the named channel).
    /// Defence-in-depth: never return another channel's allowlist to a
    /// non-producer caller.
    nlohmann::json handle_get_channel_auth_req(const nlohmann::json& req);

    /// `CONSUMER_ATTACH_REQ` handler (HEP-CORE-0041 §9 D4 step 4-5).
    /// Pre-attach confirmation: the producer asks the broker whether
    /// one specific consumer is currently authorized for a channel,
    /// before sending the SHM capability fd.  Reply carries either
    /// `status="success"` or `status="denied"` (both are normal auth
    /// decisions wrapped in a CONSUMER_ATTACH_ACK frame so the
    /// producer-side cache divergence WARN logic can distinguish a
    /// clean broker "no" from a wire error).  Protocol-level failures
    /// (shape error, channel not found, caller not a producer,
    /// HubState invariant broken) return the standard error envelope
    /// and are surfaced as ERROR replies by the dispatcher.
    /// Read-only against HubState — pure query.
    nlohmann::json handle_consumer_attach_req(const nlohmann::json& req);

    /// Fire-and-forget `CHANNEL_AUTH_CHANGED_NOTIFY` to every producer
    /// of the named channel (HEP-CORE-0036 §6.5).  Same fan-out shape
    /// as `CHANNEL_CLOSING_NOTIFY` / `CONSUMER_DIED_NOTIFY`.  No ACK
    /// awaited; the producer's response is to fire its own
    /// `GET_CHANNEL_AUTH_REQ` pull.  Producers without a captured ZMQ
    /// identity are skipped (no transport to reach them).  Caller has
    /// already mutated the channel's allowlist via the matching
    /// `_on_consumer_authorized` / `_on_consumer_revoked` HubState op.
    void fire_channel_auth_changed_notify(zmq::socket_t&     socket,
                                           const std::string& channel_name,
                                           const std::string& reason);

    /// HEP-CORE-0023 §2.5 — heartbeat negotiation block carried in
    /// REG_ACK / CONSUMER_REG_ACK.  Communicates the hub's tolerated
    /// timeout contract so the registering role can validate its own
    /// configured cadence (`role.heartbeat_interval_ms ≤ hub_max`) and
    /// align its periodic-task schedule.  Always populated from
    /// `cfg.heartbeat_*` — no per-channel override.
    nlohmann::json heartbeat_ack_block() const;
    nlohmann::json handle_endpoint_update_req(const nlohmann::json& req,
                                               const zmq::message_t& identity);
    nlohmann::json handle_schema_req(const nlohmann::json& req);

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

    void check_heartbeat_timeouts(zmq::socket_t& socket);
    void check_dead_consumers(zmq::socket_t& socket);

    // ── Role-close cleanup API (HEP-CORE-0023 §2.5) ───────────────────────
    // Central hooks called at every dereg site so federation/band/future
    // modules can clean up per-role state consistently. Called under m_query_mu
    // from the broker run() thread; `socket` is the broker's ROUTER for
    // emitting implicit notifications (e.g. BAND_LEAVE_NOTIFY to remaining
    // band members).
    void on_channel_closed(zmq::socket_t& socket,
                           const std::string& channel_name,
                           const pylabhub::hub::ChannelEntry& entry,
                           const std::string& reason);
    void on_consumer_closed(zmq::socket_t& socket,
                            const std::string& channel_name,
                            const pylabhub::hub::ConsumerEntry& consumer,
                            const std::string& reason);

    // Per-module cleanup helpers (invoked by the hooks above).
    void federation_on_channel_closed(const std::string& channel_name,
                                       const pylabhub::hub::ChannelEntry& entry,
                                       const std::string& reason);
    /// Wave M3 step 5f (2026-05-11): handler-driven BAND_LEAVE_NOTIFY
    /// fanout helper.  Reads the current member list (HubState already
    /// removed the leaving uid via its band cascade) and sends the
    /// notification to remaining members.  Replaces the prior
    /// `band_on_role_closed` that also mutated HubState — now HubState
    /// owns the state mutation and this helper only fans out the wire
    /// notification.
    void send_band_leave_notify(zmq::socket_t&    socket,
                                 const std::string& band_name,
                                 const std::string& role_uid,
                                 const std::string& reason);

    void send_closing_notify(zmq::socket_t&                            socket,
                             const std::string&                        channel_name,
                             const pylabhub::hub::ChannelEntry&        entry,
                             const std::string&                        reason);

    void handle_checksum_error_report(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    // CHANNEL_NOTIFY_REQ handler removed — audit R3.6 (2026-05-17).

    void handle_channel_broadcast_req(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    nlohmann::json handle_channel_list_req(const nlohmann::json &req);

    // ── Band pub/sub (HEP-CORE-0030) ───────────────────────────────────
    nlohmann::json handle_band_join_req(const nlohmann::json& req,
                                        const zmq::message_t& identity,
                                        zmq::socket_t& socket);
    nlohmann::json handle_band_leave_req(const nlohmann::json& req,
                                          zmq::socket_t& socket);
    void handle_band_broadcast_req(zmq::socket_t& socket,
                                   const nlohmann::json& req,
                                   const zmq::message_t& identity);
    nlohmann::json handle_band_members_req(const nlohmann::json& req);

    // Phase 4: role presence + info queries.
    nlohmann::json handle_role_presence_req(const nlohmann::json& req);
    nlohmann::json handle_role_info_req(const nlohmann::json& req);

    /// Push an unsolicited message to a specific ZMQ ROUTER identity (raw bytes).
    static void send_to_identity(zmq::socket_t&     socket,
                                 const std::string& identity,
                                 const std::string& msg_type,
                                 const nlohmann::json& body);

    static void send_reply(zmq::socket_t&       socket,
                           const zmq::message_t& identity,
                           const std::string&    msg_type_ack,
                           const nlohmann::json& body);

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static nlohmann::json make_error(const std::string& correlation_id,
                                     const std::string& error_code,
                                     const std::string& message);

    /// Timestamp of last consumer liveness check (for interval gating).
    std::chrono::steady_clock::time_point m_last_consumer_check{};

    // ── Role state-machine metrics (HEP-CORE-0023 §2.5) ─────────────────
    // Owned by HubState (`BrokerCounters`) per HEP-CORE-0033 §8;
    // accessed via `hub_state_->counters()`.

    // ── Hub federation handlers (HEP-CORE-0022) ───────────────────────────
    void handle_hub_peer_hello(zmq::socket_t&       socket,
                               const zmq::message_t& identity,
                               const nlohmann::json& payload);
    void handle_hub_peer_bye(const nlohmann::json& payload);
    void handle_hub_peer_hello_ack(const std::string& peer_hub_uid,
                                   const nlohmann::json& payload);
    void handle_hub_relay_msg(zmq::socket_t& socket, const nlohmann::json& payload);
    void handle_hub_targeted_msg(const nlohmann::json& payload);

    /// Relay a CHANNEL_NOTIFY_REQ event to all inbound peers subscribed to channel.
    void relay_notify_to_peers(zmq::socket_t&    socket,
                                const std::string& channel,
                                const std::string& event,
                                const std::string& sender_uid,
                                const std::string& data);

    /// Prune expired entries from relay_dedup_.
    void prune_relay_dedup();

    // ── Role identity policy (placeholder — pending HEP-CORE-0035) ─────────────
    /// Resolve effective policy for a channel: per-channel override > hub-wide default.
    [[nodiscard]] RoleIdentityPolicy
        effective_role_identity_policy(const std::string& channel_name) const noexcept;

    /// Check whether the connecting role's self-asserted identity (role_name +
    /// role_uid) is allowed by the effective policy.  Returns a non-empty
    /// error JSON if the registration should be rejected; std::nullopt if
    /// allowed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<nlohmann::json>
    check_role_identity(const std::string& channel_name,
                        const std::string& role_name,
                        const std::string& role_uid,
                        const std::string& corr_id,
                        bool               is_consumer) const;

    /// HEP-CORE-0036 §6.1 / §6.3 Layer-2 identity verification.  Checks
    /// that `(role_uid, claimed_pubkey)` is a single matching record in
    /// `cfg.known_roles[]`.  Returns std::nullopt on pass; populated
    /// error JSON on UNKNOWN_ROLE (uid not registered) or
    /// PUBKEY_MISMATCH (uid registered but with a different pubkey).
    /// Unconditional — runs regardless of `RoleIdentityPolicy`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<nlohmann::json>
    verify_known_role_binding(const std::string& role_uid,
                              const std::string& claimed_pubkey,
                              const std::string& corr_id,
                              const char*        request_kind) const;

    // ── Audit R3.5b (2026-05-19) — wire-boundary grammar + side-aware tag ───
    /// Validate identifier-field grammar at the gate per HEP-CORE-0033
    /// §G2.2.0b + enforce per-handler role-tag policy.
    ///
    /// Pre-fix, `check_role_identity` only enforced emptiness under
    /// policy ≥ Required; the default Open policy admitted empty /
    /// malformed uids that broke downstream presence indexing silently
    /// (e.g. _on_heartbeat / _on_consumer_joined skip when uid empty).
    /// Grammar is now REQUIRED unconditionally — RoleIdentityPolicy
    /// controls known_roles verification ON TOP of valid grammar, not
    /// in place of.
    ///
    /// `expected_tags` constrains the role tag carried inside the uid
    /// (e.g. {"prod","proc"} on REG_REQ rejects a `cons.x.y` uid).
    /// Processor roles register on both sides under one `proc.*` uid,
    /// so the producer-side handlers accept {"prod","proc"} and the
    /// consumer-side accept {"cons","proc"}.  Side-agnostic handlers
    /// (ROLE_INFO, BAND_*) pass {"prod","cons","proc"}.
    ///
    /// Validates channel_name (Channel kind), role_uid (RoleUid kind,
    /// empty rejected, tag must be in expected_tags), role_name
    /// (RoleName kind, only when non-empty — Open policy still permits
    /// anonymous registration).  Returns INVALID_REQUEST on grammar
    /// failure or INVALID_ROLE_TAG on tag-set mismatch, with
    /// LOGGER_ERROR + handler_label context.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<nlohmann::json>
    validate_identity_fields(const std::string& channel_name,
                             const std::string& role_uid,
                             const std::string& role_name,
                             std::initializer_list<std::string_view> expected_tags,
                             const std::string& corr_id,
                             const char*        handler_label) const;

    /// Narrower form for handlers that don't carry a channel context
    /// (ROLE_INFO_REQ, ROLE_PRESENCE_REQ, BAND_*_REQ — band already
    /// validates the band identifier).  Returns INVALID_REQUEST on
    /// empty/malformed role_uid or INVALID_ROLE_TAG when the tag is
    /// not in `expected_tags`.
    [[nodiscard]] std::optional<nlohmann::json>
    validate_role_uid_only(const std::string& role_uid,
                           std::initializer_list<std::string_view> expected_tags,
                           const std::string& corr_id,
                           const char*        handler_label) const;
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
    router.set(zmq::sockopt::linger, 0); // policy: always LINGER=0; see §ZMQ socket policy

    // Locals live for the entire run() call — same scope as the
    // ROUTER socket.  Storing the ZAP handle as a member would let
    // the registration outlive run() and throw on a hypothetical
    // restart (`zap_router.hpp:99` — per-domain uniqueness).
    std::unique_ptr<BrokerCtrlAdmission>                       ctrl_admission;
    std::optional<pylabhub::utils::security::ZapDomainHandle>  ctrl_zap_handle;

    // HEP-CORE-0035 §2 + §4.6.5 — CURVE-server and ZAP admission are
    // unconditional.  KeyStore presence of "hub_identity" is enforced
    // at BrokerService ctor (HEP-CORE-0040 §172), so by the time we
    // reach run() the lookup is guaranteed to succeed.  The secret
    // half flows from LockedKey → libzmq inside `with_seckey` scope;
    // no std::string copy of the seckey lives at broker scope.
    namespace sec = pylabhub::utils::security;
    auto      &ks = sec::key_store();
    router.set(zmq::sockopt::curve_server, 1);
    router.set(zmq::sockopt::curve_publickey,
               ks.pubkey(sec::kHubIdentityName));
    ks.with_seckey(sec::kHubIdentityName,
        [&](std::string_view seckey)
        {
            router.set(zmq::sockopt::curve_secretkey, seckey);
        });

    // Build the initial CTRL allowlist.  Per HEP-CORE-0035 §4.2 the
    // allowlist is the UNION of two operator-managed inputs:
    //   - `known_roles[]` (roles allowed to register; loaded from
    //     `<hub_dir>/vault/known_roles.json` by HubHost,
    //     HEP-CORE-0035 §4.8)
    //   - `peers[].pubkey_z85` (federation peer hubs allowed to
    //     connect their DEALER → this broker's ROUTER,
    //     HEP-CORE-0022 + HEP-CORE-0035 §4.2)
    // Entries with empty `pubkey_z85` are skipped.  Empty allowlist
    // is the legal deny-all state per HEP-CORE-0035 §4.8.4.
    pylabhub::utils::security::PeerAllowlist initial;
    for (const auto &kr : cfg.known_roles)
    {
        if (kr.pubkey_z85.empty()) continue;
        initial.peers.insert(
            pylabhub::utils::security::PeerIdentity{"curve",
                                                    kr.pubkey_z85});
    }
    for (const auto &peer : cfg.peers)
    {
        if (peer.pubkey_z85.empty()) continue;
        initial.peers.insert(
            pylabhub::utils::security::PeerIdentity{"curve",
                                                    peer.pubkey_z85});
    }
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
        const auto seq =
            ctrl_zap_seq.fetch_add(1, std::memory_order_relaxed);
        zap_domain = "broker.ctrl." + std::to_string(seq);
    }
    ctrl_admission = std::make_unique<BrokerCtrlAdmission>(std::move(initial));
    router.set(zmq::sockopt::zap_domain, zap_domain);
    ctrl_zap_handle.emplace(
        pylabhub::utils::security::ZapRouter::instance()
            .register_domain(zap_domain, *ctrl_admission));

    LOGGER_INFO("Broker: CTRL ZAP installed enforced on domain '{}' "
                "({} known_roles + {} federation peers = {} allowed)",
                zap_domain,
                cfg.known_roles.size(),
                cfg.peers.size(),
                allowlist_size);

    router.bind(cfg.endpoint);
    const std::string bound = router.get(zmq::sockopt::last_endpoint);
    // Pubkey is non-secret — materialize from KeyStore once for
    // diagnostics + the on_ready callback that publishes it to test
    // harnesses / federation peer config.
    const std::string hub_pubkey_z85(ks.pubkey(sec::kHubIdentityName));
    if (cfg.on_ready)
    {
        cfg.on_ready(bound, hub_pubkey_z85);
    }
    LOGGER_INFO("Broker: listening on {}", bound);
    LOGGER_INFO("Broker: hub identity pubkey = {}", hub_pubkey_z85);

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
    active_router_       = &router;
    band_left_handler_id_ = hub_state_->subscribe_band_left(
        [this](const std::string &band, const std::string &uid,
               const std::string &reason) {
            if (active_router_ == nullptr) return;
            send_band_leave_notify(*active_router_, band, uid, reason);
        });

    // ── Federation: outbound DEALER sockets per peer (HEP-CORE-0022) ────────
    // Stored as unique_ptr so socket handles remain stable in the pollitem_t vector.
    struct OutboundPeer
    {
        const FederationPeer* cfg_entry;
        zmq::socket_t                  socket;
    };
    std::vector<std::unique_ptr<OutboundPeer>> peer_sockets;

    if (!cfg.self_hub_uid.empty())
    {
        for (const auto& peer_cfg : cfg.peers)
        {
            if (peer_cfg.broker_endpoint.empty()) continue;
            auto ps = std::make_unique<OutboundPeer>();
            ps->cfg_entry = &peer_cfg;
            ps->socket = zmq::socket_t(ctx, zmq::socket_type::dealer);
            ps->socket.set(zmq::sockopt::linger, 0); // policy: always LINGER=0; see §ZMQ socket policy

            // HEP-CORE-0035 §2 — CURVE is unconditional on every wire.
            // A federation peer configured without a pubkey cannot
            // be reached (the remote broker's ROUTER will reject the
            // unauthenticated handshake); the DEALER is wired plain
            // and connect() proceeds so the diagnostic path
            // (`HANDSHAKE_FAILED_*` on the monitor) surfaces the
            // misconfiguration.  Federation peer pubkey enforcement
            // is finalized in #105 (HEP-CORE-0037).
            if (!peer_cfg.pubkey_z85.empty())
            {
                // HEP-CORE-0040 §172: same use-not-export pattern as
                // the bind ROUTER above.  Seckey bytes live only in
                // LockedKey region; copied into libzmq's internal CURVE
                // state inside `with_seckey` scope.
                ps->socket.set(zmq::sockopt::curve_serverkey,
                               peer_cfg.pubkey_z85);
                ps->socket.set(zmq::sockopt::curve_publickey,
                               ks.pubkey(sec::kHubIdentityName));
                ks.with_seckey(sec::kHubIdentityName,
                    [&](std::string_view seckey)
                    {
                        ps->socket.set(zmq::sockopt::curve_secretkey, seckey);
                    });
            }

            // Give the DEALER a stable routing-id so the peer can identify us.
            const std::string dealer_id = cfg.self_hub_uid;
            ps->socket.set(zmq::sockopt::routing_id, dealer_id);

            ps->socket.connect(peer_cfg.broker_endpoint);
            LOGGER_INFO("Broker: federation DEALER connected to peer '{}' at {}",
                        peer_cfg.hub_uid, peer_cfg.broker_endpoint);

            // Send HUB_PEER_HELLO immediately after connect.
            // ZMQ queues the message until the connection is established.
            nlohmann::json hello;
            hello["hub_uid"]              = cfg.self_hub_uid;
            hello["subscribed_channels"]  = nlohmann::json::array(); // let peer decide from its config
            hello["protocol_version"]     = 1;
            const std::string hello_body  = hello.dump();
            ps->socket.send(zmq::message_t(&kFrameTypeControl, 1),         zmq::send_flags::sndmore);
            ps->socket.send(zmq::message_t("HUB_PEER_HELLO", 14),          zmq::send_flags::sndmore);
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
        for (const auto& ps : peer_sockets)
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
        (void) pylabhub::utils::security::ZapRouter::instance().pump_one(
            std::chrono::milliseconds{0});

        // --- Post-poll phase (mutex held: all registry reads/writes are protected) ---
        {
            std::lock_guard<std::mutex> lock(m_query_mu);

            // Drain script-requested channel closes.
            std::vector<std::string> pending_closes;
            {
                std::lock_guard<std::mutex> close_lk(m_close_req_mu);
                pending_closes.assign(close_request_queue_.begin(), close_request_queue_.end());
                close_request_queue_.clear();
            }
            for (const auto& ch : pending_closes)
            {
                auto hub_entry = hub_state_->channel(ch);
                if (hub_entry.has_value())
                {
                    // HEP-CORE-0023 §2.1 atomic teardown: emit a best-effort
                    // CHANNEL_CLOSING_NOTIFY (informational; consumers may
                    // race with the close), then call _on_channel_closed
                    // which marks the producer-presence Disconnected, fires
                    // CHANNEL_CLOSED handlers, and erases the channel entry.
                    LOGGER_INFO("Broker: script-requested close for channel '{}'", ch);
                    send_closing_notify(router, ch, *hub_entry, "script_requested");
                    on_channel_closed(router, ch, *hub_entry, "script_requested");
                    hub_state_->_on_channel_closed(
                        ch, pylabhub::hub::ChannelCloseReason::AdminClose);
                }
            }

            // Drain hub-side broadcast requests (originating from
            // BrokerService::request_broadcast_channel — called by
            // HubAPI::broadcast_channel control delegate / AdminService
            // broadcast RPC / future hub-internal callers).
            std::vector<BroadcastRequest> pending_broadcasts;
            {
                std::lock_guard<std::mutex> bcast_lk(m_broadcast_req_mu);
                pending_broadcasts.assign(
                    std::make_move_iterator(broadcast_request_queue_.begin()),
                    std::make_move_iterator(broadcast_request_queue_.end()));
                broadcast_request_queue_.clear();
            }
            for (const auto& br : pending_broadcasts)
            {
                // Build a synthetic CHANNEL_BROADCAST_REQ payload and delegate.
                // sender_uid = self_hub_uid: the broadcast originates inside
                // this hub instance (the request_broadcast_channel API has
                // no caller-identity field — the natural sender is the hub).
                // Falls back to "hub" when self_hub_uid is unset (test
                // configurations + early HubHost::startup before identity
                // is wired).
                nlohmann::json req;
                req["target_channel"] = br.channel;
                req["sender_uid"]     = cfg.self_hub_uid.empty()
                                            ? std::string("hub")
                                            : cfg.self_hub_uid;
                req["message"]        = br.message;
                if (!br.data.empty())
                    req["data"] = br.data;
                handle_channel_broadcast_req(router, req);
            }

            // Drain hub_targeted_msg queue (thread-safe send from external callers).
            {
                std::vector<HubTargetedRequest> pending_targeted;
                {
                    std::lock_guard<std::mutex> ht_lk(m_hub_targeted_mu);
                    pending_targeted.assign(
                        std::make_move_iterator(hub_targeted_queue_.begin()),
                        std::make_move_iterator(hub_targeted_queue_.end()));
                    hub_targeted_queue_.clear();
                }
                for (const auto& ht : pending_targeted)
                {
                    auto peer = hub_state_->peer(ht.target_hub_uid);
                    if (!peer.has_value() ||
                        peer->state != pylabhub::hub::PeerState::Connected ||
                        peer->zmq_identity.empty())
                    {
                        LOGGER_WARN("Broker: send_hub_targeted_msg: peer '{}' not connected",
                                    ht.target_hub_uid);
                        continue;
                    }
                    nlohmann::json msg;
                    msg["target_hub_uid"] = ht.target_hub_uid;
                    msg["channel_name"]   = ht.channel;
                    msg["sender_uid"]     = cfg.self_hub_uid;
                    msg["payload"]        = ht.payload;
                    send_to_identity(router, peer->zmq_identity, "HUB_TARGETED_MSG", msg);
                    LOGGER_DEBUG("Broker: sent HUB_TARGETED_MSG to peer '{}'", ht.target_hub_uid);
                }
            }

            // Check heartbeat timeouts and consumer liveness every poll cycle (~100ms resolution).
            check_heartbeat_timeouts(router);
            check_dead_consumers(router);
            prune_relay_dedup();

            // --- Handle ROUTER socket (local clients + inbound peer DEALERs) ---
            if ((items[0].revents & ZMQ_POLLIN) != 0)
            {
                std::vector<zmq::message_t> frames;
                auto num_parts = zmq::recv_multipart(router, std::back_inserter(frames));
                if (!num_parts.has_value())
                {
                    LOGGER_WARN("Broker: recv_multipart failed, possible ZMQ error or context termination.");
                }
                // Expected layout: [identity, 'C', msg_type_string, json_body]
                else if (*num_parts < 4)
                {
                    LOGGER_WARN("Broker: malformed message (expected ≥4 frames, got {})",
                                *num_parts);
                }
                else
                {
                    try
                    {
                        // S1 frame validation — reject oversized payloads
                        // before string construction and JSON parse.
                        // ROUTER socket: no mandatory reply (HEP-0033 §9.3).
                        static constexpr size_t kMaxPayloadBytes = 1u << 20; // 1 MB
                        if (frames[3].size() > kMaxPayloadBytes)
                        {
                            LOGGER_WARN("Broker: oversized payload ({} bytes) — dropped",
                                        frames[3].size());
                            hub_state_->_bump_counter("sys.malformed_frame");
                            emit_processing_error(/*msg_type=*/"",
                                                  "malformed_frame",
                                                  "oversized payload " +
                                                      std::to_string(frames[3].size()) +
                                                      " bytes",
                                                  &frames[0]);
                        }
                        else
                        {
                            const std::string msg_type    = frames[2].to_string();
                            const std::string payload_raw = frames[3].to_string();
                            LOGGER_TRACE("Broker: recv msg_type='{}' payload_len={}",
                                         msg_type, payload_raw.size());
                            nlohmann::json payload = nlohmann::json::parse(payload_raw);
                            process_message(router, frames[0], msg_type, payload,
                                            payload_raw.size());
                        }
                    }
                    catch (const nlohmann::json::exception& e)
                    {
                        // S2 body parse — drop, count, hook (no reply path
                        // because we don't yet know msg_type / corr_id).
                        LOGGER_WARN("Broker: malformed JSON: {}", e.what());
                        hub_state_->_bump_counter("sys.malformed_json");
                        emit_processing_error(/*msg_type=*/"",
                                              "malformed_json", e.what(),
                                              &frames[0]);
                    }
                }
            }

            // --- Handle outbound peer DEALER sockets (ACKs and relays from peers) ---
            // DEALER receives: ['C', msg_type, json_body]  (3 frames, no identity)
            for (size_t i = 0; i < peer_sockets.size(); ++i)
            {
                if ((items[1 + i].revents & ZMQ_POLLIN) == 0) continue;
                std::vector<zmq::message_t> peer_frames;
                auto np = zmq::recv_multipart(peer_sockets[i]->socket,
                                              std::back_inserter(peer_frames));
                if (!np.has_value() || *np < 3) continue;
                try
                {
                    const std::string peer_msg_type = peer_frames[1].to_string();
                    const std::string peer_body_raw = peer_frames[2].to_string();
                    nlohmann::json peer_payload = nlohmann::json::parse(peer_body_raw);
                    if (peer_msg_type == "HUB_PEER_HELLO_ACK")
                    {
                        handle_hub_peer_hello_ack(
                            peer_sockets[i]->cfg_entry->hub_uid, peer_payload);
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
                catch (const nlohmann::json::exception& e)
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
        for (auto& ps : peer_sockets)
        {
            try
            {
                ps->socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
                ps->socket.send(zmq::message_t("HUB_PEER_BYE", 12),    zmq::send_flags::sndmore);
                ps->socket.send(zmq::message_t(bye_body.data(), bye_body.size()),
                                zmq::send_flags::none);
            }
            catch (const zmq::error_t&) {} // best-effort on shutdown
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
    active_router_ = nullptr;

    router.close();
    LOGGER_INFO("Broker: stopped.");
}

// ============================================================================
// Message dispatch
// ============================================================================

void BrokerServiceImpl::process_message(zmq::socket_t&       socket,
                                        const zmq::message_t& identity,
                                        const std::string&    msg_type,
                                        const nlohmann::json& payload,
                                        std::size_t           bytes_in)
{
    LOGGER_TRACE("Broker: dispatch {} channel='{}'", msg_type,
                 payload.is_object()
                     ? payload.value("channel_name", std::string{}) // HEP-0036 §5b
                     : std::string{});

    // HEP-CORE-0033 §9.3 S3: unknown msg_type — bump only sys.unknown_msg_type
    // (R1: do NOT pollute msg_type_counts with attacker-supplied strings),
    // ERROR reply, fire hook, return.
    if (!is_known_msg_type(msg_type))
    {
        LOGGER_WARN("Broker: unknown msg_type '{}'", msg_type);
        const std::string corr_id = payload.value("correlation_id", "");
        try {
            send_reply(socket, identity, "ERROR",
                       make_error(corr_id, "UNKNOWN_MSG_TYPE",
                                  "Unknown message type: " + msg_type));
        } catch (const std::exception &re) {
            LOGGER_WARN("Broker: failed to send UNKNOWN_MSG_TYPE ERROR reply: {}",
                        re.what());
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

    if (msg_type == "REG_REQ")
    {
        nlohmann::json resp = handle_reg_req(payload, identity, socket);
        const std::string ack = (resp.value("status", "") == "success") ? "REG_ACK" : "ERROR";
        if (ack == "REG_ACK")
        {
            // "REG_ACK sending" marker (rung 2 of Pattern 4 ladder).
            // Pin: channel + allowlist payload distinguish a registration ACK
            // from REG_REQ rejections that already log via existing ERROR/WARN.
            // `heartbeat_interval_ms` extracted from the REG_ACK heartbeat
            // block (HEP-CORE-0023 §2.5) so Pattern 4 rung 3 can verify
            // role honored the hub-authoritative cadence.  Defaults to 0
            // if the broker shipped without a heartbeat block (legacy).
            int hb_interval_ms = 0;
            if (resp.contains("heartbeat") && resp["heartbeat"].is_object())
                hb_interval_ms = resp["heartbeat"].value(
                    "heartbeat_interval_ms", 0);
            LOGGER_INFO("[broker] event=RegAckSending channel='{}' "
                        "heartbeat_interval_ms={} initial_allowlist={}",
                        resp.value("channel_name", "?"), // HEP-0036 §5b.5
                        hb_interval_ms,
                        resp.value("initial_allowlist",
                                   nlohmann::json::array()).dump());
        }
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "DISC_REQ")
    {
        // Three-response dispatch (HEP-CORE-0023 §2.2):
        //   "success" -> DISC_ACK (channel Ready)
        //   "pending" -> DISC_PENDING (client retries)
        //   otherwise -> ERROR (CHANNEL_NOT_FOUND)
        nlohmann::json resp = handle_disc_req(payload);
        const std::string status = resp.value("status", "");
        std::string ack;
        if (status == "success")      ack = "DISC_ACK";
        else if (status == "pending") ack = "DISC_PENDING";
        else                          ack = "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "DEREG_REQ")
    {
        nlohmann::json resp = handle_dereg_req(payload, socket);
        const std::string ack = (resp.value("status", "") == "success") ? "DEREG_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "CONSUMER_REG_REQ")
    {
        nlohmann::json resp = handle_consumer_reg_req(payload, identity, socket);
        const std::string ack =
            (resp.value("status", "") == "success") ? "CONSUMER_REG_ACK" : "ERROR";
        if (ack == "CONSUMER_REG_ACK")
        {
            // Parallel to "REG_ACK sending" above.  Pins the
            // HEP-CORE-0036 §6.4 producers[] payload the consumer
            // role-host needs for its rx-queue CURVE allowlist.
            // Empty `[]` is a legal value (no producers attached yet);
            // ZMQ channels emit a populated list, SHM channels omit
            // the key entirely (HEP-0036 §5.6).  The "[]" literal
            // avoids constructing a temporary nlohmann::json::array()
            // on every accepted REG_REQ; the .dump() cost is intrinsic
            // to the marker shape (operational diagnostic value).
            const std::string producers_dump =
                resp.contains("producers") ? resp["producers"].dump() : "[]";
            LOGGER_INFO("[broker] event=ConsumerRegAckSending channel='{}' "
                        "producers={}",
                        resp.value("channel_name", "?"),
                        producers_dump);
        }
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "GET_CHANNEL_AUTH_REQ")
    {
        // HEP-CORE-0036 §6.5 — producer pulls the current channel-scope
        // allowlist.
        nlohmann::json resp = handle_get_channel_auth_req(payload);
        const std::string ack =
            (resp.value("status", "") == "success") ? "GET_CHANNEL_AUTH_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "CONSUMER_ATTACH_REQ")
    {
        // HEP-CORE-0041 §9 D4 — producer's pre-attach confirmation.
        // Special-cased dispatch: "denied" is a normal auth decision,
        // NOT a wire error.  Both "success" and "denied" map to
        // CONSUMER_ATTACH_ACK so the producer's cache-divergence
        // observer (substep 1e) can distinguish a clean broker "no"
        // from a transport failure.
        nlohmann::json resp = handle_consumer_attach_req(payload);
        const std::string status = resp.value("status", "");
        const std::string ack =
            (status == "success" || status == "denied")
                ? "CONSUMER_ATTACH_ACK"
                : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "CONSUMER_DEREG_REQ")
    {
        nlohmann::json resp = handle_consumer_dereg_req(socket, payload);
        const std::string ack =
            (resp.value("status", "") == "success") ? "CONSUMER_DEREG_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "HEARTBEAT_REQ")
    {
        // Fire-and-forget from client.  Presence FSM transitions (e.g.
        // first-heartbeat sub-Live → Live; Pending → Connected recovery)
        // happen inside hub_state_->_on_heartbeat() called from the handler.
        handle_heartbeat_req(payload);
    }
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
    else if (msg_type == "CHANNEL_BROADCAST_REQ")
    {
        // Fire-and-forget: fan out broadcast to ALL members of a channel.
        handle_channel_broadcast_req(socket, payload);
    }
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
        send_reply(socket, identity, "SHM_BLOCK_QUERY_ACK",
                   handle_shm_block_query(payload));
    }
    else if (msg_type == "ROLE_PRESENCE_REQ")
    {
        // Phase 4: check if a role UID is alive in any channel.
        nlohmann::json resp = handle_role_presence_req(payload);
        send_reply(socket, identity, "ROLE_PRESENCE_ACK", resp);
    }
    else if (msg_type == "ROLE_INFO_REQ")
    {
        // Phase 4: return inbox connection info for a producer UID.
        nlohmann::json resp = handle_role_info_req(payload);
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
    else if (msg_type == "ENDPOINT_UPDATE_REQ")
    {
        // HEP-0021 §16: update a channel's endpoint after ephemeral port bind.
        nlohmann::json resp = handle_endpoint_update_req(payload, identity);
        const std::string ack =
            (resp.value("status", "") == "success") ? "ENDPOINT_UPDATE_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
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
    else if (msg_type == "BAND_BROADCAST_REQ")
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
        LOGGER_ERROR("Broker: handler exception for msg_type='{}': {}",
                     msg_type, e.what());
        // R3: ERROR reply only for request-reply types — the protocol
        // shape is fixed per msg_type, not per outcome.  Fire-and-forget
        // clients never expect a reply, even when the server errored.
        if (is_request_reply(msg_type))
        {
            try {
                send_reply(socket, identity, "ERROR",
                           make_error(payload.value("correlation_id", ""),
                                      "INTERNAL_ERROR", e.what()));
            } catch (const std::exception &re) {
                // R11: best-effort; swallow.
                LOGGER_WARN("Broker: failed to send INTERNAL_ERROR reply for "
                            "msg_type='{}': {}", msg_type, re.what());
            } catch (...) {
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
void BrokerServiceImpl::emit_processing_error(const std::string&    msg_type,
                                               const std::string&    error_kind,
                                               const std::string&    detail,
                                               const zmq::message_t* identity)
{
    if (!cfg.on_processing_error) return;
    pylabhub::broker::ProcessingError err;
    err.msg_type   = msg_type;
    err.error_kind = error_kind;
    err.detail     = detail;
    if (identity != nullptr && identity->size() > 0)
        err.peer_identity = std::string(static_cast<const char*>(identity->data()),
                                        identity->size());
    try {
        cfg.on_processing_error(err);
    }
    catch (const std::exception &he) {
        LOGGER_ERROR("Broker: on_processing_error callback threw "
                     "(msg_type='{}', kind='{}'): {}",
                     msg_type, error_kind, he.what());
    }
    catch (...) {
        LOGGER_ERROR("Broker: on_processing_error callback threw "
                     "(unknown exception type, msg_type='{}', kind='{}')",
                     msg_type, error_kind);
    }
}

// ============================================================================
// Handlers
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_reg_req(const nlohmann::json& req,
                                                   const zmq::message_t& identity,
                                                   zmq::socket_t&        socket)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    const std::string role_name    = req.value("role_name", "");
    const std::string role_uid     = req.value("role_uid", "");

    // Audit R3.5b (2026-05-19): wire-boundary grammar + side-aware tag
    // check.  Empty or malformed channel_name / role_uid are rejected
    // unconditionally before any state-machine entry — downstream
    // presence indexing (_on_producer_added, _on_heartbeat) is keyed on
    // these and would silently no-op an empty uid.  Tag policy on
    // REG_REQ accepts {prod, proc} — `proc.*` registers on the
    // producer side for its output channel.  Runs before
    // check_role_identity so policy verification never sees a
    // malformed input.
    if (auto err = validate_identity_fields(channel_name, role_uid, role_name,
                                            {"prod", "proc"},
                                            corr_id, "REG_REQ"))
    {
        return *err;
    }

    const std::string attempted_schema = req.value("schema_hash", "");
    const uint64_t    attempted_pid    = req.value("producer_pid", uint64_t{0});

    // ── Role identity policy check (placeholder — pending HEP-CORE-0035) ────
    if (auto err = check_role_identity(channel_name, role_name, role_uid, corr_id,
                                       /*is_consumer=*/false))
    {
        return *err;
    }

    // HEP-CORE-0036 §4.1 + §5.1 + §6.4 — producer's CURVE identity
    // pubkey is REQUIRED on REG_REQ.  Broker stores it on
    // `ChannelEntry::producers[i].zmq_pubkey` and emits it back via
    // `CONSUMER_REG_ACK.producers[]` so each consumer has the
    // producer's `curve_serverkey` for its data-plane PULL socket.
    // HEP-CORE-0035 §2 makes CURVE unconditional — empty or
    // wrong-length values are programmer errors and rejected at wire
    // admission, mirroring the consumer-side CONSUMER_REG_REQ check.
    const std::string producer_pubkey = req.value("zmq_pubkey", "");
    if (producer_pubkey.empty())
    {
        LOGGER_WARN(
            "Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
            "missing required `zmq_pubkey` (HEP-CORE-0036 §4.1 broker_proto>=6).",
            channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ requires non-empty `zmq_pubkey` "
                          "(broker_proto>=6 / HEP-CORE-0036 §4.1)");
    }
    if (producer_pubkey.size() != 40)
    {
        // CURVE pubkeys are Z85-encoded 32-byte blobs = exactly 40
        // ASCII chars.  Any other length cannot match a real CURVE
        // handshake; reject at the wire to avoid polluting the
        // producer entry with a value that consumers would then use
        // as a broken `curve_serverkey`.
        LOGGER_WARN(
            "Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
            "`zmq_pubkey` length is {}, expected 40 (Z85-encoded CURVE25519).",
            channel_name, role_uid, producer_pubkey.size());
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ `zmq_pubkey` length is " +
                              std::to_string(producer_pubkey.size()) +
                              ", expected 40 (Z85-encoded CURVE25519 pubkey)");
    }

    // HEP-CORE-0036 §6.1 Layer-2 identity verification — the wire-
    // claimed (role_uid, zmq_pubkey) pair MUST match a single
    // known_roles.json record.  Layer-1 ZAP already proved the
    // connecting socket holds a known_roles pubkey; this binds the
    // REG_REQ to the specific uid claimed in the body.
    if (auto err = verify_known_role_binding(role_uid, producer_pubkey,
                                              corr_id, "REG_REQ"))
    {
        return *err;
    }

    // Wave M2.5 step 3 — build the new producer entry from the wire
    // payload.  All per-producer attributes (inbox_*, zmq_node_endpoint,
    // zmq_pubkey, metadata) live on `ProducerEntry`; the deprecated
    // channel-scope versions on `ChannelEntry` are no longer written
    // from REG_REQ.  See `docs/tech_draft/controlled_access_api_design.md`
    // §7.5.3 + `REVIEW_WaveM2.5_2026-05-10.md` F6/F7.
    pylabhub::hub::ProducerEntry primary_producer;
    primary_producer.producer_pid      = attempted_pid;
    primary_producer.producer_hostname = req.value("producer_hostname", "");
    primary_producer.role_name         = role_name;
    primary_producer.role_uid          = role_uid;
    primary_producer.inbox_endpoint    = req.value("inbox_endpoint", "");
    primary_producer.inbox_schema_json = req.value("inbox_schema_json", "");
    primary_producer.inbox_packing     = req.value("inbox_packing", "");
    primary_producer.inbox_checksum    = req.value("inbox_checksum", "");
    primary_producer.zmq_node_endpoint = req.value("zmq_node_endpoint", "");
    primary_producer.zmq_pubkey        = producer_pubkey;
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
    // with the `zmq_pubkey` enforcement above for ZMQ channels.
    primary_producer.shm_capability_endpoint =
        req.value("shm_capability_endpoint", "");

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
    if (!req.contains("data_transport") || !req["data_transport"].is_string())
    {
        LOGGER_WARN(
            "Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
            "missing required `data_transport` field "
            "(HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1 — must be string "
            "'shm' or 'zmq').",
            channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ requires `data_transport` field "
                          "(string, 'shm' or 'zmq')");
    }
    const std::string data_transport_req =
        req["data_transport"].get<std::string>();
    if (data_transport_req != "shm" && data_transport_req != "zmq")
    {
        LOGGER_WARN(
            "Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
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
    // registration.  Symmetric with `zmq_pubkey` enforcement above for
    // ZMQ channels.
    if (data_transport_req == "shm" &&
        primary_producer.shm_capability_endpoint.empty())
    {
        LOGGER_WARN(
            "Broker: REG_REQ rejected — channel '{}' role_uid='{}' "
            "data_transport='shm' but `shm_capability_endpoint` is empty "
            "(HEP-CORE-0041 §5.1 — SHM channels MUST publish their L2 "
            "capability endpoint so the broker can echo it to authorized "
            "consumers in CONSUMER_REG_ACK).",
            channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "REG_REQ data_transport='shm' requires non-empty "
                          "`shm_capability_endpoint` (HEP-CORE-0041 §5.1)");
    }
    if (req.contains("metadata") && req["metadata"].is_object())
    {
        primary_producer.metadata = req["metadata"];
    }
    // Producer ZMQ identity: captured here for future unsolicited pushes
    // (CHANNEL_CLOSING_NOTIFY / CHANNEL_ERROR_NOTIFY etc.).
    primary_producer.zmq_identity.assign(
        static_cast<const char*>(identity.data()), identity.size());

    // HEP-0021 §16: reject registration if inbox_endpoint has unresolved port 0.
    if (!primary_producer.inbox_endpoint.empty())
    {
        auto inbox_ep = pylabhub::validate_tcp_endpoint(
            primary_producer.inbox_endpoint);
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
    const std::string schema_blds_in = req.value("schema_blds", "");

    // ── Channel-mismatch early gate (audit fix — must precede
    //    schema-record creation so a failed REG_REQ leaves no orphan
    //    records in HubState.schemas) ─────────────────────────────────
    //
    // HEP-0007 Cat-1 invariant: re-registration of an existing channel
    // with a different schema_hash is rejected.  `_on_producer_added`
    // would also catch this as RejectedMismatch on `schema_hash`, but
    // running the check here lets us short-circuit BEFORE creating
    // schema records (path B/C / inbox), preventing orphans.  The
    // other invariant mismatches (schema_version / schema_blds /
    // schema_owner / schema_id / transport_*) are caught by
    // `_on_producer_added` after record creation; an orphan record
    // there is a rare anomaly the broker logs as an ERROR.
    //
    // CHANNEL_ERROR_NOTIFY fan-out per HEP-CORE-0007: notify every
    // existing producer so all co-producers on this channel see the
    // schema-mismatch event from the rejected newcomer.
    if (auto existing_opt = hub_state_->channel(channel_name); existing_opt.has_value())
    {
        if (existing_opt->schema_hash != attempted_schema)
        {
            const std::string &existing_schema = existing_opt->schema_hash;
            LOGGER_ERROR(
                "Broker: Cat1 schema mismatch on '{}': existing={} attempted={} attempted_pid={}",
                channel_name, existing_schema, attempted_schema, attempted_pid);
            nlohmann::json err;
            err["channel_name"]          = channel_name;
            err["event"]                 = "schema_mismatch_attempt";
            err["existing_schema_hash"]  = existing_schema;
            err["attempted_schema_hash"] = attempted_schema;
            err["attempted_pid"]         = attempted_pid;
            for (const auto &prod : existing_opt->producers)
            {
                if (prod.zmq_identity.empty()) continue;
                send_to_identity(socket, prod.zmq_identity,
                                 "CHANNEL_ERROR_NOTIFY", err);
                LOGGER_ERROR("Broker: CHANNEL_ERROR_NOTIFY to producer of '{}': event={}, "
                             "existing_hash={}, attempted_hash={}, attempted_pid={}, target={}",
                             channel_name, "schema_mismatch_attempt",
                             existing_schema, attempted_schema, attempted_pid,
                             prod.role_uid);
            }
            return make_error(corr_id, "SCHEMA_MISMATCH",
                              "Schema hash differs from existing registration for channel '" +
                                  channel_name + "'");
        }
        // Same schema — admission will append to existing producers[]
        // via _on_producer_added; consumers are preserved automatically.
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
    const std::string req_schema_packing = req.value("schema_packing", "");
    const std::string req_schema_id_raw  = req.value("schema_id", "");
    const std::string req_schema_owner   = req.value("schema_owner", "");

    // Resolved schema_id / schema_owner for the channel invariants; set
    // by the path B/C blocks below, or left empty for anonymous channels.
    std::string final_schema_id;
    std::string final_schema_owner;

    if (!req_schema_id_raw.empty())
    {
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
        if (role_uid.empty())
            return make_error(corr_id, "MISSING_ROLE_UID",
                              "REG_REQ with schema_id requires role_uid for "
                              "owner attribution (HEP-CORE-0034 §10.1)");

        // Stage-2 fingerprint check (slot + flexzone) — common to both
        // path B and path C.  HEP-CORE-0034 §6.3 / §10.1 — the canonical
        // form covers BOTH the slot and the flexzone when present.
        const std::string req_flexzone_blds    = req.value("flexzone_blds", "");
        const std::string req_flexzone_packing = req.value("flexzone_packing", "");
        const auto h_recomputed = pylabhub::hub::compute_canonical_hash_from_wire(
            schema_blds_in, req_schema_packing,
            req_flexzone_blds, req_flexzone_packing);
        const auto h_claimed = hex_to_hash_array(attempted_schema);
        if (h_recomputed != h_claimed)
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

        const bool is_path_c = (req_schema_owner == "hub");
        const bool is_path_b = req_schema_owner.empty() ||
                               req_schema_owner == role_uid;
        if (!is_path_b && !is_path_c)
        {
            // Cross-owner attempt — third role's uid claimed.
            LOGGER_WARN("Broker: REG_REQ rejected — producer '{}' attempted "
                        "to register under foreign owner '{}'",
                        role_uid, req_schema_owner);
            return make_error(corr_id, "SCHEMA_FORBIDDEN_OWNER",
                              "Producer cannot register schema under owner '" +
                                  req_schema_owner + "' — only \"hub\" "
                                  "(adopt global) or self are permitted");
        }

        if (is_path_c)
        {
            // Path C: adopt an existing hub-global.  No new record is
            // created; we only verify that the producer's fingerprint
            // matches the global's stored fingerprint.
            const auto existing = hub_state_->schema("hub", req_schema_id_raw);
            if (!existing.has_value())
            {
                LOGGER_WARN("Broker: REG_REQ path-C rejected for '{}' — "
                            "no hub-global record under (hub, {})",
                            channel_name, req_schema_id_raw);
                return make_error(corr_id, "SCHEMA_UNKNOWN",
                                  "Hub-global schema (hub, " + req_schema_id_raw +
                                      ") not registered; cannot adopt");
            }
            if (existing->hash != h_recomputed ||
                existing->packing != req_schema_packing)
            {
                LOGGER_WARN("Broker: REG_REQ path-C rejected for '{}' — "
                            "producer's fingerprint does not match hub-global "
                            "(hub, {})",
                            channel_name, req_schema_id_raw);
                return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                                  "Producer's schema fingerprint does not "
                                  "match hub-global (hub, " +
                                      req_schema_id_raw + "); cannot adopt");
            }
            // Adoption succeeds.  Channel is owned by the hub-global,
            // not by the producer.
            final_schema_id    = req_schema_id_raw;
            final_schema_owner = "hub";
        }
        else
        {
            // Path B: self-registration.
            pylabhub::schema::SchemaRecord rec;
            rec.owner_uid = role_uid;
            rec.schema_id = req_schema_id_raw;
            rec.hash      = h_recomputed;
            rec.packing   = req_schema_packing;
            rec.blds      = schema_blds_in;

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
            final_schema_id    = req_schema_id_raw;
            final_schema_owner = role_uid;
        }
    }

    // ── HEP-CORE-0034 §11.4 Phase 3b — inbox schema record (path A) ─────
    //
    // When the producer's REG_REQ carries inbox metadata
    // (`inbox_endpoint` + `inbox_schema_json` + `inbox_packing`), the
    // broker constructs a SchemaRecord under (role_uid, "inbox") so
    // sender-side citers can resolve it via `(role_uid, "inbox")`.
    //
    // Canonical form mirrors `compute_inbox_schema_tag` exactly so the
    // 32-byte SchemaRecord.hash and the 8-byte wire `schema_tag` agree
    // bytewise (tag = hash[0..7]).
    //
    // Backward compat: REG_REQs without `inbox_packing` skip this block;
    // inbox metadata is still stored on `entry.producers.front()` but no
    // SchemaRecord exists for it.
    //
    // Inbox info lives on ProducerEntry (HEP-CORE-0023 §2.1.1 + §2.6 +
    // HEP-CORE-0027) — read from the ProducerEntry we just built.
    const auto &p_inbox = primary_producer;
    if (!p_inbox.inbox_endpoint.empty() &&
        !p_inbox.inbox_schema_json.empty() &&
        !p_inbox.inbox_packing.empty() &&
        !role_uid.empty())
    {
        // Reject invalid packing strings up-front — mirrors the queue-layer
        // check in `hub_inbox_queue.cpp::validate_inbox_packing`.  Without
        // this, the broker would compute a canonical form with a bogus
        // packing string and persist it, but no inbox queue could later
        // bind/connect against it.
        if (p_inbox.inbox_packing != "aligned" && p_inbox.inbox_packing != "packed")
        {
            LOGGER_WARN("Broker: REG_REQ for '{}' rejected — inbox_packing '{}' "
                        "must be 'aligned' or 'packed'",
                        channel_name, p_inbox.inbox_packing);
            return make_error(corr_id, "INVALID_INBOX_PACKING",
                              "inbox_packing '" + p_inbox.inbox_packing +
                                  "' must be 'aligned' or 'packed'");
        }

        // Compute the 32-byte fingerprint over inbox fields + packing.
        std::string canonical;
        try
        {
            const auto schema_arr = nlohmann::json::parse(p_inbox.inbox_schema_json);
            if (!schema_arr.is_array())
                throw std::runtime_error("inbox_schema_json is not an array");
            canonical.reserve(schema_arr.size() * 16 + 16);
            for (const auto &f : schema_arr)
            {
                canonical += f.value("type", "");
                canonical += ':';
                canonical += std::to_string(f.value("count", uint32_t{1}));
                canonical += ':';
                canonical += std::to_string(f.value("length", uint32_t{0}));
                canonical += ';';
            }
        }
        catch (const std::exception &ex)
        {
            LOGGER_WARN("Broker: REG_REQ inbox_schema_json invalid for '{}': {}",
                        channel_name, ex.what());
            return make_error(corr_id, "INBOX_SCHEMA_INVALID",
                              std::string("inbox_schema_json parse error: ") + ex.what());
        }
        canonical += "|pack:";
        canonical += p_inbox.inbox_packing;

        pylabhub::schema::SchemaRecord rec;
        rec.owner_uid = role_uid;
        rec.schema_id = "inbox";
        rec.hash      = pylabhub::crypto::compute_blake2b_array(
            canonical.data(), canonical.size());
        rec.packing   = p_inbox.inbox_packing;
        rec.blds      = p_inbox.inbox_schema_json;

        using O = pylabhub::schema::SchemaRegOutcome;
        const auto outcome = hub_state_->_on_schema_registered(rec);
        if (outcome == O::kHashMismatchSelf)
        {
            LOGGER_WARN("Broker: REG_REQ inbox schema record mismatch for ({}, inbox): "
                        "existing record under producer's namespace differs",
                        role_uid);
            return make_error(corr_id, "SCHEMA_HASH_MISMATCH_SELF",
                              "Inbox schema record (" + role_uid +
                                  ", inbox) already exists with a different "
                                  "hash or packing");
        }
        if (outcome == O::kForbiddenOwner)
        {
            return make_error(corr_id, "SCHEMA_FORBIDDEN_OWNER",
                              "Inbox schema record (" + role_uid +
                                  ", inbox) rejected — missing required fields");
        }
        // Created or Idempotent → success.  No need to set anything on
        // `entry`: the inbox schema record is keyed by the producer's
        // role uid (not by channel name), so citers always look up
        // `(<producer.role_uid>, "inbox")` against the matching
        // `ProducerEntry` — there is no per-channel inbox-record
        // reference to maintain.  See HEP-CORE-0027 §3
        // (per-producer/per-consumer inbox ownership).
    }

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
    // residue), MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM (SHM cardinality),
    // and the broader invariant-mismatch class (schema_version,
    // schema_blds, schema_owner, schema_id, data_transport).
    // (HEP-CORE-0036 §5b.4 retired the legacy duplicates
    // has_shared_memory / shm_name / channel_pattern.)
    // A reject in those remaining cases CAN leave an orphan schema record in
    // HubState.schemas (rare anomaly); broker logs ERROR.
    pylabhub::hub::ChannelSchemaInvariants schema_inv;
    schema_inv.schema_hash    = attempted_schema;
    schema_inv.schema_version = req.value("schema_version", uint32_t{0});
    schema_inv.schema_id      = final_schema_id;
    schema_inv.schema_blds    = schema_blds_in;
    schema_inv.schema_owner   = final_schema_owner;

    pylabhub::hub::ChannelTransportInvariants transport_inv;
    // HEP-CORE-0036 §5b.4: `data_transport` is the only canonical
    // transport-classification field on REG_REQ.  Validated above
    // (§6.1 + HEP-CORE-0041 §5.1); reuse the same local so the
    // persisted ChannelEntry.data_transport cannot drift from the
    // value the §5.1 endpoint check accepted.
    transport_inv.data_transport    = data_transport_req;

    const auto admission = hub_state_->_on_producer_added(
        channel_name,
        std::move(schema_inv),
        std::move(transport_inv),
        std::move(primary_producer));

    // Typed result dispatch:
    if (admission.invariant_result ==
        pylabhub::hub::InvariantSetResult::RejectedMismatch)
    {
        const auto &field = admission.mismatched_invariant;
        // schema_* mismatches → SCHEMA_MISMATCH (the schema_hash case
        // was already handled by the early gate above; this path
        // catches schema_version / schema_id / schema_blds /
        // schema_owner divergences).
        if (field == "schema_hash" || field == "schema_version" ||
            field == "schema_id"   || field == "schema_blds"    ||
            field == "schema_owner")
        {
            LOGGER_ERROR(
                "Broker: REG_REQ for '{}' rejected — invariant mismatch on '{}' "
                "(schema-class).  Anomaly: orphan schema record possible.",
                channel_name, field);
            return make_error(corr_id, "SCHEMA_MISMATCH",
                              "REG_REQ rejected — schema invariant '" + field +
                                  "' differs from existing channel registration");
        }
        // Transport-class mismatches → TRANSPORT_MISMATCH (HEP-CORE-0007
        // §12.4a — same code as the consumer-side transport check).
        LOGGER_ERROR(
            "Broker: REG_REQ for '{}' rejected — invariant mismatch on '{}' "
            "(transport-class)", channel_name, field);
        return make_error(corr_id, "TRANSPORT_MISMATCH",
                          "REG_REQ rejected — transport invariant '" + field +
                              "' differs from existing channel registration");
    }
    if (admission.producer_result ==
        pylabhub::hub::AddProducerResult::RejectedUidConflict)
    {
        // HEP-CORE-0007 §12.4a `UID_CONFLICT` — strict reject per
        // controlled_access_api_design.md §6.2.  Logged at ERROR
        // because either a uid collision (effectively impossible with
        // proper construction → indicates hub-side residue or remote
        // spoof) or a same-uid re-register attempt.
        LOGGER_ERROR(
            "Broker: REG_REQ for '{}' uid='{}' rejected — UID_CONFLICT "
            "(uid already exists in HubState; possible residue or spoof)",
            channel_name, role_uid);
        return make_error(corr_id, "UID_CONFLICT",
                          "uid conflict, not trusting this connection, "
                          "try again with clean state");
    }
    if (admission.producer_result ==
        pylabhub::hub::AddProducerResult::RejectedShmCardinality)
    {
        // HEP-CORE-0007 §12.4a `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`.
        LOGGER_WARN(
            "Broker: REG_REQ for '{}' uid='{}' rejected — SHM channels are "
            "physically single-producer (HEP-CORE-0023 §2.1.1)",
            channel_name, role_uid);
        return make_error(corr_id, "MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM",
                          "Channel uses SHM transport which admits exactly "
                          "one producer; use ZMQ transport for Fan-In");
    }

    // HEP-CORE-0036 §6.5: a freshly-opened channel needs a
    // `ChannelAccessEntry` so subsequent CONSUMER_REG_REQ accepts can
    // populate the allowlist via `_on_consumer_authorized` (no-op
    // without an existing access record per its safe-default
    // invariant).  SHM secret stays zero — SHM auth wiring is tracked
    // separately (HEP-CORE-0036 §12 Phase 5 + task #106).
    if (admission.channel_opened)
    {
        hub_state_->_on_channel_access_opened(channel_name, /*shm_secret=*/0);
    }

    // "REG_REQ accepted" marker (rung 2 of Pattern 4 ladder).
    // Pin: role_uid + channel + producer_pubkey identify the registration
    // uniquely; channel_opened distinguishes first-producer vs subsequent.
    LOGGER_INFO("[broker] event=RegReqAccepted role='{}' channel='{}' producer_pubkey='{}' "
                "(pending first heartbeat){}",
                role_uid, channel_name, producer_pubkey,
                admission.channel_opened ? " - channel opened"
                                         : " - appended to existing");
    nlohmann::json resp;
    resp["status"]       = "success";
    resp["channel_name"] = channel_name; // HEP-CORE-0036 §5b.5 canonical
    resp["message"]      = "Producer registered successfully";
    resp["heartbeat"]    = heartbeat_ack_block(); // HEP-CORE-0023 §2.5

    // HEP-CORE-0036 §5.1 + §6.5: REG_ACK carries the channel's current
    // authorized-consumer allowlist so a producer reconnecting to an
    // already-populated channel observes the live set without waiting
    // for the next `CHANNEL_AUTH_CHANGED_NOTIFY`.  Empty array for a
    // freshly-opened channel.  This is the producer-offline recovery
    // path called out in §6.5 (replaces the retired snapshot-push-with-
    // ACK design).
    nlohmann::json allowlist = nlohmann::json::array();
    if (auto access = hub_state_->channel_access(channel_name);
        access.has_value())
    {
        for (const auto &pk : access->authorized_consumer_pubkeys)
        {
            allowlist.push_back(pk);
        }
    }
    resp["initial_allowlist"] = std::move(allowlist);

    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_disc_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    // Take a single snapshot so the channel + producer-presence lookup
    // observe consistent state — channel teardown is atomic with the
    // producer-presence Disconnected transition (HEP-CORE-0023 §2.1).
    const auto snap   = hub_state_->snapshot();
    const auto cit    = snap.channels.find(channel_name);

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
        if (rit == snap.roles.end()) continue;
        const auto *p = rit->second.find_presence(channel_name, "producer");
        if (p == nullptr) continue;
        if (p->state == pylabhub::hub::RoleState::Disconnected) continue;
        if (p->state == pylabhub::hub::RoleState::Connected &&
            p->first_heartbeat_seen)
        {
            presence = p;
            break;
        }
        if (fallback == nullptr) fallback = p;
    }
    if (presence == nullptr) presence = fallback;

    if (presence == nullptr)
    {
        // Channel exists but all producer-presences absent / Disconnected
        // (presence rows reaped while atomic teardown is in flight).
        // From the consumer's perspective this channel is not discoverable.
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> CHANNEL_NOT_FOUND "
                     "(no live producer-presence)", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    if (!presence->first_heartbeat_seen ||
        presence->state == pylabhub::hub::RoleState::Pending)
    {
        // Producer registered but not yet Live — either we're still
        // awaiting the first heartbeat (presence Connected without
        // first_heartbeat_seen), or the heartbeat has stalled (presence
        // Pending).  Client is responsible for retry; reason field
        // distinguishes the two for diagnostics.
        const char *reason = !presence->first_heartbeat_seen
                                 ? "awaiting_first_heartbeat"
                                 : "heartbeat_stalled";
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> DISC_PENDING ({})",
                     channel_name, reason);
        nlohmann::json resp;
        resp["status"]       = "pending";
        resp["channel_name"] = channel_name;
        resp["reason"]       = reason;
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
    resp["status"]            = "success";
    resp["schema_hash"]       = entry_ref.schema_hash;
    resp["schema_version"]    = entry_ref.schema_version;
    // metadata wire shape decided in §6.1: per-producer tree keyed by
    // role_uid (HEP-CORE-0007 §12.4 commit 25dc376).
    resp["metadata"]          = entry_ref.aggregate_metadata_tree();
    resp["consumer_count"]    =
        static_cast<uint32_t>(entry_ref.consumers.size());
    // HEP-CORE-0036 §5b.4: `data_transport` is the canonical transport
    // classification; pre-§5b duplicates `shm_name`, `has_shared_memory`,
    // `channel_pattern` retired.
    // zmq_ctrl_endpoint / zmq_data_endpoint retired in Wave M2.5 step 2c.
    // zmq_pubkey + zmq_node_endpoint use first-producer transitional shape
    // until step 5 lifts them to per-producer arrays.
    resp["zmq_pubkey"]        = (first_prod != nullptr ? first_prod->zmq_pubkey
                                                       : std::string{});
    resp["data_transport"]    = entry_ref.data_transport;
    resp["zmq_node_endpoint"] = (first_prod != nullptr ? first_prod->zmq_node_endpoint
                                                       : std::string{});
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_dereg_req(const nlohmann::json& req,
                                                    zmq::socket_t&        socket)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const uint64_t    producer_pid = req.value("producer_pid", uint64_t{0});
    const std::string wire_role_uid = req.value("role_uid", "");

    // broker_proto 2→3 (audit C3, 2026-05-15): `role_uid` REQUIRED for
    // multi-producer DEREG target resolution.
    // Audit R3.5b (2026-05-19): grammar + side-aware tag check
    // (HEP-CORE-0033 §G2.2.0b).  Tag {prod, proc} — producer-side
    // deregistration only.  Pre-fix, a malformed uid would fall
    // through to the NOT_REGISTERED branch — typed grammar error is
    // more diagnostic.
    if (auto err = validate_identity_fields(channel_name, wire_role_uid,
                                            /*role_name=*/"",
                                            {"prod", "proc"},
                                            corr_id, "DEREG_REQ"))
    {
        return *err;
    }

    // Resolve via (pid, role_uid) tuple — both must match the same
    // admitted producer.  HEP-CORE-0023 §2.1.1 + atomic-teardown
    // contract: removing one producer leaves the channel alive iff
    // other producers remain; channel teardown fires only when the
    // LAST producer leaves.  `_on_producer_dropped` encapsulates this.
    auto entry = hub_state_->channel(channel_name);
    std::string target_role_uid;
    if (entry.has_value())
    {
        for (const auto &prod : entry->producers)
        {
            if (prod.producer_pid == producer_pid &&
                prod.role_uid     == wire_role_uid)
            {
                target_role_uid = prod.role_uid;
                break;
            }
        }
    }
    if (!entry.has_value() || target_role_uid.empty())
    {
        LOGGER_WARN("Broker: DEREG_REQ failed for channel '{}' "
                    "(pid={} role_uid='{}')",
                    channel_name, producer_pid, wire_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name +
                              "' not registered or no producer matches "
                              "(pid, role_uid)");
    }

    // Capture the channel state (with the to-be-dropped producer
    // still admitted) so we can fan-out CHANNEL_CLOSING_NOTIFY to
    // every party iff this is the LAST producer's leave.  The
    // snapshot is from before the drop; `_on_producer_dropped`
    // tells us whether the channel was torn down or not.
    const pylabhub::hub::ChannelEntry pre_drop = *entry;

    auto drop = hub_state_->_on_producer_dropped(
        channel_name, target_role_uid,
        pylabhub::hub::ChannelCloseReason::VoluntaryDereg);

    if (!drop.removed)
    {
        // Should not happen — we just resolved a matching producer
        // before the call.  Race condition (presence already reaped
        // between resolve and drop) → treat as NOT_REGISTERED.
        LOGGER_WARN("Broker: DEREG_REQ for '{}' uid='{}' lost the race "
                    "(producer no longer admitted)", channel_name, target_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name +
                              "' producer no longer admitted (race)");
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
        // M1.4 (2026-05-11): no metrics_store_.erase needed — metrics
        // live on per-presence rows which are erased atomically by
        // `_on_channel_closed`'s cascade.
        LOGGER_INFO("Broker: deregistered channel '{}' (last producer left — channel torn down)",
                    channel_name);
    }
    else
    {
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
    resp["status"]  = "success";
    resp["message"] = "Producer deregistered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_consumer_reg_req(const nlohmann::json& req,
                                                           const zmq::message_t& identity,
                                                           zmq::socket_t&        socket)
{
    const std::string corr_id      = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): wire fields unified
    // across all gates onto `role_uid`/`role_name` (HEP-CORE-0033
    // §G2.2.0b — role tag is embedded in the uid; consumer-specific
    // names are redundant).  Old `consumer_uid`/`consumer_name` removed.
    const std::string role_name    = req.value("role_name", "");
    const std::string role_uid     = req.value("role_uid",  "");

    // Audit R3.5b (2026-05-19): wire-boundary grammar + side-aware tag
    // check before any state-machine entry.  Pre-fix, empty uid sailed
    // through check_role_identity under default Open policy, was stored
    // on ConsumerEntry, then `_on_consumer_joined` silently skipped
    // `upsert_role_locked` (gated on `!role_uid.empty()`) — no
    // role-presence row was created, so subsequent heartbeats / inbox
    // discovery silently no-op'd.  Tag policy {cons, proc} — `proc.*`
    // registers on the consumer side for its input channel.
    if (auto err = validate_identity_fields(channel_name, role_uid, role_name,
                                            {"cons", "proc"},
                                            corr_id, "CONSUMER_REG_REQ"))
    {
        return *err;
    }

    // Single HubState snapshot covers channel-existence, endpoint
    // freshness, R6 producer-readiness gate, and downstream schema
    // checks — matches DISC_REQ's pattern at line ~1909 and avoids
    // any drift between separate snapshot calls.
    const auto snap = hub_state_->snapshot();
    const auto cit  = snap.channels.find(channel_name);
    if (cit == snap.channels.end())
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }
    const auto &channel_entry = cit->second;

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
            LOGGER_INFO("Broker: CONSUMER_REG_REQ channel '{}' ZMQ endpoint has port 0 (awaiting_endpoint)",
                        channel_name);
            return make_error(corr_id, "CHANNEL_NOT_READY",
                              "ZMQ endpoint for channel '" + channel_name +
                                  "' has unresolved port 0 (awaiting_endpoint)");
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
    {
        bool any_live          = false;
        bool any_kRegistering  = false;  // Connected, !first_heartbeat
        bool any_kStalled      = false;  // Pending
        for (const auto &prod : channel_entry.producers)
        {
            auto rit = snap.roles.find(prod.role_uid);
            if (rit == snap.roles.end()) continue;
            const auto *p =
                rit->second.find_presence(channel_name, "producer");
            if (p == nullptr) continue;
            if (p->state == pylabhub::hub::RoleState::Connected)
            {
                if (p->first_heartbeat_seen) { any_live = true; break; }
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
                LOGGER_INFO(
                    "Broker: CONSUMER_REG_REQ channel '{}' deferred — "
                    "awaiting_first_heartbeat", channel_name);
                return make_error(
                    corr_id, "CHANNEL_NOT_READY",
                    "Channel '" + channel_name +
                        "' is not ready (awaiting_first_heartbeat — "
                        "producer registered but has not yet sent its "
                        "first heartbeat)");
            }
            if (any_kStalled)
            {
                LOGGER_WARN(
                    "Broker: CONSUMER_REG_REQ channel '{}' deferred — "
                    "heartbeat_stalled", channel_name);
                return make_error(
                    corr_id, "CHANNEL_NOT_READY",
                    "Channel '" + channel_name +
                        "' is not ready (heartbeat_stalled — producer "
                        "missed its heartbeat window; HEP-CORE-0023 §2.6)");
            }
            LOGGER_WARN(
                "Broker: CONSUMER_REG_REQ channel '{}' rejected — all "
                "producer-presences absent or Disconnected", channel_name);
            return make_error(
                corr_id, "CHANNEL_NOT_FOUND",
                "Channel '" + channel_name +
                    "' has no producer-presence alive");
        }
    }

    // ── Transport arbitration (Phase 6) ─────────────────────────────────────
    const std::string consumer_queue_type = req.value("consumer_queue_type", "");
    if (!consumer_queue_type.empty() && consumer_queue_type != channel_entry.data_transport)
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ transport mismatch on '{}': "
                    "consumer wants '{}' but channel uses '{}'",
                    channel_name, consumer_queue_type, channel_entry.data_transport);
        return make_error(corr_id, "TRANSPORT_MISMATCH",
                          "Consumer queue_type '" + consumer_queue_type +
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
    const std::string expected_schema_id    = req.value("expected_schema_id", "");
    const std::string expected_hash_hex     = req.value("expected_schema_hash", "");
    const std::string expected_blds         = req.value("expected_schema_blds", "");
    const std::string expected_packing      = req.value("expected_schema_packing", "");
    // HEP-0034 §10.3 — flexzone mirrors the producer-side wire fields
    // (Phase 5a).  When the consumer's structure includes flexzone, the
    // recomputed fingerprint must include it too — otherwise the
    // consumer-recomputed hash (slot+fz) won't match the channel's
    // stored hash (slot+fz from REG_REQ).  Same correctness gap that
    // Phase 4a fixed on the REG_REQ side; mirrored here for symmetry.
    const std::string expected_fz_blds      = req.value("expected_flexzone_blds", "");
    const std::string expected_fz_packing   = req.value("expected_flexzone_packing", "");

    if (!expected_schema_id.empty())
    {
        // Named-citation mode.
        if (expected_hash_hex.empty())
            return make_error(corr_id, "MISSING_HASH_FOR_NAMED_CITATION",
                              "CONSUMER_REG_REQ with expected_schema_id requires "
                              "expected_schema_hash (HEP-CORE-0034 §10.3)");

        if (channel_entry.schema_id != expected_schema_id)
        {
            LOGGER_WARN("Broker: CONSUMER_REG_REQ schema_id mismatch on '{}': "
                        "expected='{}' channel_schema_id='{}'",
                        channel_name, expected_schema_id, channel_entry.schema_id);
            return make_error(corr_id, "SCHEMA_ID_MISMATCH",
                              "Consumer expected schema_id '" + expected_schema_id +
                                  "' does not match channel '" + channel_name +
                                  "' (channel id='" + channel_entry.schema_id + "')");
        }
        if (expected_hash_hex != channel_entry.schema_hash)
            return make_error(corr_id, "SCHEMA_CITATION_REJECTED",
                              "Consumer's expected_schema_hash does not match "
                              "channel's stored hash (HEP-CORE-0034 §10.3)");

        // Defense-in-depth: if the consumer also provided the full
        // structure, verify it hashes to the channel's hash.  Catches
        // a class of consumer-side bugs where the local ctypes struct
        // diverges from the named schema definition.
        if (!expected_blds.empty() || !expected_packing.empty())
        {
            if (expected_blds.empty())
                return make_error(corr_id, "MISSING_BLDS",
                                  "expected_schema_blds required when "
                                  "expected_schema_packing is provided alongside "
                                  "named citation");
            if (expected_packing.empty())
                return make_error(corr_id, "MISSING_PACKING",
                                  "expected_schema_packing required when "
                                  "expected_schema_blds is provided alongside "
                                  "named citation");
            const auto h_c = pylabhub::hub::compute_canonical_hash_from_wire(
                expected_blds, expected_packing,
                expected_fz_blds, expected_fz_packing);
            if (h_c != hex_to_hash_array(channel_entry.schema_hash))
                return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                                  "Consumer's expected_schema_blds + "
                                  "expected_schema_packing does not hash to the "
                                  "channel's schema_hash; named-citation "
                                  "defense-in-depth (HEP-0034 §10.3)");
        }
    }
    else if (!expected_hash_hex.empty() || !expected_blds.empty() || !expected_packing.empty()
             || !expected_fz_blds.empty() || !expected_fz_packing.empty())
    {
        // Anonymous-citation mode.  HEP-0034 §10.3 — full structure required.
        if (expected_blds.empty())
            return make_error(corr_id, "MISSING_BLDS_FOR_ANONYMOUS_CITATION",
                              "Anonymous citation (no expected_schema_id) requires "
                              "expected_schema_blds (HEP-CORE-0034 §10.3)");
        if (expected_packing.empty())
            return make_error(corr_id, "MISSING_PACKING_FOR_ANONYMOUS_CITATION",
                              "Anonymous citation requires expected_schema_packing");

        const auto h_c = pylabhub::hub::compute_canonical_hash_from_wire(
            expected_blds, expected_packing,
            expected_fz_blds, expected_fz_packing);

        // Self-consistency check: if consumer also supplied a hash, it
        // must equal the recomputed hash.  Catches consumer-local
        // structure-vs-hash drift (Stage-2 verification).
        if (!expected_hash_hex.empty())
        {
            if (h_c != hex_to_hash_array(expected_hash_hex))
                return make_error(corr_id, "FINGERPRINT_INCONSISTENT",
                                  "expected_schema_hash does not match BLAKE2b-256 of "
                                  "canonical(expected_schema_blds || \"|pack:\" + "
                                  "expected_schema_packing)");
        }

        // Compare against producer's hash on the channel.
        if (h_c != hex_to_hash_array(channel_entry.schema_hash))
            return make_error(corr_id, "SCHEMA_CITATION_REJECTED",
                              "Consumer's recomputed fingerprint does not match the "
                              "channel's schema_hash (HEP-CORE-0034 §10.3)");
    }
    // else: all expected_* empty → consumer opts out of schema validation
    // (HEP-CORE-0034 §10.3 "I don't care about schema" path).

    // ── Role identity policy check (placeholder — pending HEP-CORE-0035) ────
    // Grammar check already ran at handler entry (audit R3.5b); this
    // is the policy / known_roles verification layer on top.
    if (auto err = check_role_identity(channel_name, role_name, role_uid, corr_id,
                                       /*is_consumer=*/true))
    {
        return *err;
    }

    pylabhub::hub::ConsumerEntry entry;
    entry.consumer_pid      = req.value("consumer_pid", uint64_t{0});
    entry.consumer_hostname = req.value("consumer_hostname", "");
    entry.role_name         = role_name;
    entry.role_uid          = role_uid;
    entry.inbox_endpoint    = req.value("inbox_endpoint", "");
    entry.inbox_schema_json = req.value("inbox_schema_json", "");
    entry.inbox_packing     = req.value("inbox_packing", "");
    entry.inbox_checksum    = req.value("inbox_checksum", "");
    // HEP-CORE-0036 §6.5: the consumer's CURVE pubkey is REQUIRED on
    // the wire so the broker can populate the channel-scope
    // authorized-consumer allowlist via `_on_consumer_authorized` and
    // revoke it on DEREG / heartbeat timeout.  HEP-CORE-0035 §2 makes
    // CURVE unconditional.  Empty or wrong-length values are
    // programmer errors and rejected at wire admission, matching the
    // producer-side REG_REQ check.
    const std::string consumer_pubkey = req.value("zmq_pubkey", "");
    if (consumer_pubkey.empty())
    {
        LOGGER_WARN(
            "Broker: CONSUMER_REG_REQ rejected — channel '{}' role_uid='{}' "
            "missing required `zmq_pubkey` (HEP-CORE-0036 §6.5 broker_proto>=6).",
            channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_REG_REQ requires non-empty `zmq_pubkey` "
                          "(broker_proto>=6 / HEP-CORE-0036 §6.5)");
    }
    if (consumer_pubkey.size() != 40)
    {
        // CURVE pubkeys are Z85-encoded 32-byte blobs = exactly 40
        // ASCII chars.  Any other length cannot match a real CURVE
        // handshake; reject at the wire to avoid polluting the
        // channel allowlist with values that would silently deny
        // every connection attempt.
        LOGGER_WARN(
            "Broker: CONSUMER_REG_REQ rejected — channel '{}' role_uid='{}' "
            "`zmq_pubkey` length is {}, expected 40 (Z85-encoded CURVE25519).",
            channel_name, role_uid, consumer_pubkey.size());
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_REG_REQ `zmq_pubkey` length is " +
                              std::to_string(consumer_pubkey.size()) +
                              ", expected 40 (Z85-encoded CURVE25519 pubkey)");
    }

    // HEP-CORE-0036 §6.3 Layer-2 identity verification — symmetric
    // with the producer-side REG_REQ check (§6.1).  Binds the
    // CONSUMER_REG_REQ to a specific (role_uid, pubkey) pair within
    // the operator-authorized known_roles allowlist.
    if (auto err = verify_known_role_binding(role_uid, consumer_pubkey,
                                              corr_id, "CONSUMER_REG_REQ"))
    {
        return *err;
    }
    entry.zmq_pubkey = consumer_pubkey;
    // Capture ZMQ identity for future CHANNEL_CLOSING_NOTIFY.
    entry.zmq_identity.assign(static_cast<const char*>(identity.data()), identity.size());

    hub_state_->_on_consumer_joined(channel_name, std::move(entry));

    // HEP-CORE-0036 §6.5: pubkey was validated non-empty + 40-char Z85
    // above.  Add it to the channel-scope allowlist and fire the
    // change notify to all producers.
    hub_state_->_on_consumer_authorized(channel_name, consumer_pubkey);
    fire_channel_auth_changed_notify(socket, channel_name, "consumer_joined");

    // HEP-CORE-0036 §6.4 — one-shot per accepted CONSUMER_REG_REQ; format
    // parallels REG_REQ accepted (line ~1148) so test harnesses can grep
    // by uid/channel/pubkey.  Pubkey is Z85 (40 chars).
    LOGGER_INFO("[broker] event=ConsumerRegReqAccepted role='{}' channel='{}' "
                "consumer_pubkey='{}'",
                role_uid, channel_name, consumer_pubkey);
    nlohmann::json resp;
    resp["status"]       = "success";
    resp["channel_name"] = channel_name;
    resp["message"]      = "Consumer registered successfully";
    resp["heartbeat"]    = heartbeat_ack_block(); // HEP-CORE-0023 §2.5
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }

    // HEP-CORE-0036 §6.4 — emit `producers[]` so the consumer's
    // role-host can populate `RxQueueOptions::producer_peers` for the
    // data-plane PULL socket's CURVE handshake.  ZMQ transport only.
    // Length 1 for single-producer; length N for fan-in (HEP-CORE-0023
    // §2.1.1) — same wire shape either way.
    //
    // SHM transport: today, this builder emits nothing transport-
    // specific for SHM channels (the ZMQ branch falls through with no
    // `else`).  HEP-CORE-0041 substep 1g (#254) lands the SHM branch:
    // `shm_capability_endpoint` (from `ProducerEntry::shm_capability_endpoint`,
    // populated by the producer's `default_shm_capability_endpoint` helper
    // and stored by the REG_REQ handler) + `producer_pubkey_z85` (from
    // `ProducerEntry::zmq_pubkey`, reused for the `crypto_box`
    // challenge-response in HEP-0041 §5.5).  The legacy AUTH-4 /
    // task #164 design (a single `shm_secret` field) is SUPERSEDED by
    // HEP-CORE-0041 and was never wired on the wire — do not add it.
    if (auto ch_opt = hub_state_->channel(channel_name); ch_opt.has_value())
    {
        if (ch_opt->data_transport == "zmq")
        {
            nlohmann::json producers_array = nlohmann::json::array();
            for (const auto& p : ch_opt->producers)
            {
                producers_array.push_back({
                    {"role_uid", p.role_uid},
                    {"pubkey",   p.zmq_pubkey},
                    {"endpoint", p.zmq_node_endpoint},
                });
            }
            resp["producers"] = std::move(producers_array);
        }
        else if (ch_opt->data_transport == "shm" && !ch_opt->producers.empty())
        {
            // HEP-CORE-0041 §5.3 (substep 1g #254) — SHM channels carry
            // the producer's L2 capability-transport endpoint and pubkey
            // so the consumer can dial via `attach_shm_capability_consumer`
            // and crypto_box-encrypt its attach challenge (§5.5 frame 2)
            // against the producer's pubkey.  SHM is single-producer per
            // HEP-CORE-0023 §2.1.1 cardinality, so we always take the
            // first (and only) producer; the broker has already rejected
            // a second SHM producer at REG_REQ time with
            // MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM.
            const auto &p = ch_opt->producers.front();
            resp["shm_capability_endpoint"] = p.shm_capability_endpoint;
            resp["producer_pubkey_z85"]     = p.zmq_pubkey;
        }
    }

    return resp;
}

nlohmann::json BrokerServiceImpl::handle_consumer_dereg_req(zmq::socket_t& socket,
                                                             const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const uint64_t    consumer_pid  = req.value("consumer_pid", uint64_t{0});
    const std::string wire_role_uid = req.value("role_uid", "");

    // broker_proto 2→3 (audit C3, 2026-05-15): `role_uid` REQUIRED for
    // multi-consumer DEREG target resolution.
    // Audit R3.5b (2026-05-19): grammar + side-aware tag check
    // (HEP-CORE-0033 §G2.2.0b).  Tag {cons, proc} — consumer-side.
    if (auto err = validate_identity_fields(channel_name, wire_role_uid,
                                            /*role_name=*/"",
                                            {"cons", "proc"},
                                            corr_id, "CONSUMER_DEREG_REQ"))
    {
        return *err;
    }

    // Fetch consumer entry BEFORE removal so the cleanup hook can read role_uid.
    // Resolution by (pid, role_uid) tuple — both must match.
    pylabhub::hub::ConsumerEntry closing_entry{};
    bool have_entry = false;
    {
        auto ch = hub_state_->channel(channel_name);
        if (ch.has_value())
        {
            for (const auto& c : ch->consumers)
            {
                if (c.consumer_pid == consumer_pid &&
                    c.role_uid     == wire_role_uid)
                {
                    closing_entry = c;
                    have_entry = true;
                    break;
                }
            }
        }
    }

    if (!have_entry)
    {
        LOGGER_WARN("Broker: CONSUMER_DEREG_REQ failed for channel '{}' "
                    "(pid={} role_uid='{}')",
                    channel_name, consumer_pid, wire_role_uid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Consumer (pid=" + std::to_string(consumer_pid) +
                              ", role_uid='" + wire_role_uid +
                              "') not registered for channel '" +
                              channel_name + "'");
    }

    // Consumer voluntarily left.  `role_uid` was validated non-empty
    // at handler entry (validate_identity_fields), so the role-side
    // cleanup branch in `_on_consumer_left` always runs.
    hub_state_->_on_consumer_left(channel_name, closing_entry.role_uid);
    on_consumer_closed(socket, channel_name, closing_entry, "voluntary_close");

    // HEP-CORE-0036 §6.5: revoke this consumer's pubkey from the
    // channel-scope allowlist + notify producers.  CONSUMER_REG_REQ
    // hard-rejects empty / non-40-char `zmq_pubkey` at the wire
    // (HEP-CORE-0035 §2 unconditional CURVE), so every stored
    // ConsumerEntry MUST carry a 40-char Z85 key.  A value that
    // fails that invariant indicates HubState corruption — log
    // loudly and skip the revoke (passing a malformed pubkey to
    // `_on_consumer_revoked` would be a no-op anyway).
    if (closing_entry.zmq_pubkey.size() != 40)
    {
        LOGGER_ERROR(
            "Broker: ConsumerEntry on channel='{}' role_uid='{}' has "
            "invalid zmq_pubkey (length {}, expected 40 Z85 chars).  "
            "This SHOULD NOT happen — CONSUMER_REG_REQ hard-rejects "
            "empty / wrong-length pubkey at the wire (HEP-CORE-0035 §2 "
            "unconditional CURVE).  System may be compromised; restart "
            "the hub ASAP.  Skipping channel-access revocation.",
            channel_name, closing_entry.role_uid,
            closing_entry.zmq_pubkey.size());
    }
    else
    {
        hub_state_->_on_consumer_revoked(channel_name,
                                          closing_entry.zmq_pubkey);
        fire_channel_auth_changed_notify(socket, channel_name, "consumer_left");
    }

    LOGGER_INFO("Broker: consumer deregistered from channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"]  = "success";
    resp["message"] = "Consumer deregistered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ─── Channel-auth pull + notify helpers (HEP-CORE-0036 §6.5) ───────────────

nlohmann::json
BrokerServiceImpl::handle_get_channel_auth_req(const nlohmann::json &req)
{
    // HEP-CORE-0036 §6.5 — producer pulls the channel-scope
    // authorized-consumer allowlist.
    // Request shape: { channel_name, role_uid, [correlation_id] }.
    // Reply (success): { status="success", allowlist=[z85, ...], corr_id }.
    // Reply (error):   { status="error", error_code, message, corr_id }.
    const std::string corr_id      = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    const std::string caller_uid   = req.value("role_uid", "");

    if (channel_name.empty() || caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "GET_CHANNEL_AUTH_REQ requires non-empty "
                          "channel_name and role_uid");
    }

    // Authorization: the caller MUST be a registered producer of the
    // requested channel.  Defence-in-depth — even though Layer-1 ZAP
    // already gated who can speak to the broker, we never reveal one
    // channel's allowlist to a non-producer caller (HEP-CORE-0036 §6.6).
    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' does not exist");
    }
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
        // Defence-in-depth: a misbehaving (or compromised) role
        // querying another channel's allowlist is observable here,
        // separately from the wire-level CURVE+ZAP layer that gates
        // who can speak to the broker at all.
        LOGGER_WARN(
            "Broker: GET_CHANNEL_AUTH_REQ rejected — role_uid='{}' is "
            "not a registered producer of channel '{}' (HEP-CORE-0036 "
            "§6.6 PRODUCER_NOT_AUTHORIZED)",
            caller_uid, channel_name);
        return make_error(corr_id, "PRODUCER_NOT_AUTHORIZED",
                          "Caller role_uid='" + caller_uid +
                              "' is not a registered producer of channel '" +
                              channel_name + "'");
    }

    // Read the current authoritative allowlist.  REG_REQ wires
    // `_on_channel_access_opened` on every channel admission, so the
    // access record MUST exist for any channel that passed the
    // producer-membership check above.  A missing record here means
    // HubState is corrupted (e.g., a teardown path didn't call
    // `_on_channel_access_closed` symmetrically) — return INTERNAL_ERROR
    // rather than degrade to deny-all, because the caller would
    // otherwise apply an empty allowlist and silently lock every
    // consumer out.
    auto access = hub_state_->channel_access(channel_name);
    if (!access.has_value())
    {
        LOGGER_ERROR(
            "Broker: GET_CHANNEL_AUTH_REQ for channel '{}' from role_uid='{}' "
            "found a ChannelEntry but no ChannelAccessEntry — HubState "
            "invariant broken (REG path wires _on_channel_access_opened "
            "for every channel admission).  Returning INTERNAL_ERROR.",
            channel_name, caller_uid);
        return make_error(corr_id, "INTERNAL_ERROR",
                          "ChannelAccessEntry missing for channel '" +
                              channel_name +
                              "' — broker invariant broken; restart the hub "
                              "to recover");
    }
    nlohmann::json resp;
    resp["status"]       = "success";
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
    for (const auto &pk : access->authorized_consumer_pubkeys)
    {
        allowlist_arr.push_back(pk);
    }
    resp["allowlist"] = std::move(allowlist_arr);
    return resp;
}

nlohmann::json
BrokerServiceImpl::handle_consumer_attach_req(const nlohmann::json &req)
{
    // HEP-CORE-0041 §9 D4 step 4-5 — pre-attach broker confirmation.
    // Producer asks whether one specific consumer is currently
    // authorized for a channel before sending the SHM capability fd.
    //
    // Request shape:
    //   { channel_name, consumer_pubkey, consumer_role_uid,
    //     role_uid (= caller's = producer's), [correlation_id] }
    //
    // Reply (auth decision — both wrapped in CONSUMER_ATTACH_ACK by
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

    const std::string corr_id           = req.value("correlation_id", "");
    const std::string channel_name      = req.value("channel_name", "");
    const std::string consumer_pubkey   = req.value("consumer_pubkey", "");
    const std::string consumer_role_uid = req.value("consumer_role_uid", "");
    const std::string caller_uid        = req.value("role_uid", "");

    if (channel_name.empty() || consumer_pubkey.empty() ||
        consumer_role_uid.empty() || caller_uid.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST",
                          "CONSUMER_ATTACH_REQ requires non-empty "
                          "channel_name, consumer_pubkey, "
                          "consumer_role_uid, and role_uid");
    }

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
        LOGGER_WARN(
            "Broker: CONSUMER_ATTACH_REQ rejected — role_uid='{}' is "
            "not a registered producer of channel '{}' "
            "(HEP-CORE-0041 §9 D4 PRODUCER_NOT_AUTHORIZED)",
            caller_uid, channel_name);
        return make_error(corr_id, "PRODUCER_NOT_AUTHORIZED",
                          "Caller role_uid='" + caller_uid +
                              "' is not a registered producer of channel '" +
                              channel_name + "'");
    }

    auto access = hub_state_->channel_access(channel_name);
    if (!access.has_value())
    {
        // Same broker-invariant-broken short-circuit as
        // handle_get_channel_auth_req: degrading to deny-all here
        // would silently lock every consumer out of a working
        // channel; INTERNAL_ERROR forces a hub restart instead.
        LOGGER_ERROR(
            "Broker: CONSUMER_ATTACH_REQ for channel '{}' from role_uid='{}' "
            "found a ChannelEntry but no ChannelAccessEntry — HubState "
            "invariant broken; returning INTERNAL_ERROR.",
            channel_name, caller_uid);
        return make_error(corr_id, "INTERNAL_ERROR",
                          "ChannelAccessEntry missing for channel '" +
                              channel_name + "'");
    }

    const bool authorized =
        access->authorized_consumer_pubkeys.find(consumer_pubkey) !=
        access->authorized_consumer_pubkeys.end();

    nlohmann::json resp;
    resp["channel_name"]    = channel_name;
    resp["consumer_pubkey"] = consumer_pubkey;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;

    if (authorized)
    {
        resp["status"] = "success";
        LOGGER_INFO(
            "[broker] event=ConsumerAttachAuthorized channel='{}' "
            "consumer_pubkey='{}' consumer_uid='{}' producer_uid='{}'",
            channel_name, consumer_pubkey, consumer_role_uid, caller_uid);
    }
    else
    {
        resp["status"]        = "denied";
        resp["denial_reason"] = "consumer_pubkey not in channel allowlist";
        LOGGER_INFO(
            "[broker] event=ConsumerAttachDenied channel='{}' "
            "consumer_pubkey='{}' consumer_uid='{}' producer_uid='{}' "
            "reason='not_in_allowlist'",
            channel_name, consumer_pubkey, consumer_role_uid, caller_uid);
    }
    return resp;
}

void BrokerServiceImpl::fire_channel_auth_changed_notify(
    zmq::socket_t&     socket,
    const std::string& channel_name,
    const std::string& reason)
{
    // HEP-CORE-0036 §6.5 amended 2026-06-04 — fan a fire-and-forget
    // CHANNEL_AUTH_CHANGED_NOTIFY out to every producer of the named
    // channel.  Same shape + threading as CHANNEL_CLOSING_NOTIFY /
    // CONSUMER_DIED_NOTIFY (see line 3328 area).  Producers without a
    // captured ZMQ identity (legacy or partial-state entries) are
    // skipped: no transport reaches them; recovery is via REG_ACK
    // on reconnect.
    auto ch = hub_state_->channel(channel_name);
    if (!ch.has_value())
    {
        // Caller (REG / DEREG / heartbeat-timeout handler) just
        // mutated the channel's allowlist; the channel SHOULD exist.
        // A missing channel here means the lookup races with a
        // concurrent teardown — observable but harmless (producers
        // who were live during the mutation will pick up the change
        // via REG_ACK.initial_allowlist on the next reconnect).
        LOGGER_WARN(
            "Broker: CHANNEL_AUTH_CHANGED_NOTIFY fan-out skipped — "
            "channel '{}' no longer exists in HubState (probable race "
            "with concurrent channel teardown; reason='{}').",
            channel_name, reason);
        return;
    }
    nlohmann::json notify;
    notify["channel_name"] = channel_name;
    notify["reason"]       = reason;
    std::size_t fanned = 0;
    std::size_t skipped_no_identity = 0;
    for (const auto &prod : ch->producers)
    {
        if (prod.zmq_identity.empty())
        {
            ++skipped_no_identity;
            continue;
        }
        send_to_identity(socket, prod.zmq_identity,
                          "CHANNEL_AUTH_CHANGED_NOTIFY", notify);
        ++fanned;
    }
    if (skipped_no_identity > 0)
    {
        // Identity-less producers are a transient state (pre-REG_ACK
        // capture, or post-teardown partial entries).  Reveal them so
        // operators can investigate whether a producer is stuck not
        // receiving notifies — which would degrade to "stale allowlist
        // until producer's next reconnect" per the §6.5 drift window.
        LOGGER_WARN(
            "Broker: CHANNEL_AUTH_CHANGED_NOTIFY for channel '{}' "
            "skipped {} producer(s) with empty zmq_identity (transient "
            "or partial state); fanned to {} producer(s).",
            channel_name, skipped_no_identity, fanned);
    }
    LOGGER_DEBUG("Broker: CHANNEL_AUTH_CHANGED_NOTIFY fan-out for "
                 "channel '{}' reason='{}' to {} of {} producer(s)",
                 channel_name, reason, fanned, ch->producers.size());
}

void BrokerServiceImpl::handle_heartbeat_req(const nlohmann::json& req)
{
    const std::string channel_name = req.value("channel_name", "");
    // Audit R3.5b (2026-05-19): channel_name grammar check at the gate
    // (HEP-CORE-0033 §G2.2.0b).  HEARTBEAT_REQ is fire-and-forget so
    // we log + drop on failure (no reply path) — matches the existing
    // pattern for missing uid/role_type further below.
    if (!pylabhub::hub::is_valid_identifier(
            channel_name, pylabhub::hub::IdentifierKind::Channel))
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ dropped — invalid "
                    "channel_name '{}' (HEP-CORE-0033 §G2.2.0b)",
                    channel_name);
        return;
    }
    // Peek existence + producer-presence state before applying the
    // heartbeat so we can log the Pending->Live channel-observable
    // transition (the actual mutation + counter bump happens inside
    // hub_state_->_on_heartbeat below).  Per HEP-CORE-0023 §2.2 the
    // channel observable is derived from the producer-presence FSM,
    // so a transition is only "channel-level" when this heartbeat is
    // the producer's; consumer heartbeats refresh their own presence
    // and never flip the channel observable.
    const auto snap = hub_state_->snapshot();
    const auto cit  = snap.channels.find(channel_name);
    if (cit == snap.channels.end())
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for unknown channel '{}'", channel_name);
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
        if (rit == snap.roles.end()) continue;
        const auto *pp = rit->second.find_presence(channel_name, "producer");
        if (pp == nullptr) continue;
        if (pp->state == pylabhub::hub::RoleState::Connected &&
            pp->first_heartbeat_seen)
        {
            was_pending = false;
            break;
        }
    }
    // Wire-format enforcement — HEP-CORE-0019 §4.1 (Phase 6).  The
    // role-side `BrokerRequestComm::send_heartbeat(channel, role_uid,
    // role_type, metrics)` MUST populate `role_uid` + `role_type` on
    // the wire; broker_proto 2→3 (audit C4, 2026-05-15) removed the
    // pre-Phase-6 fallback that derived uid from the channel's first
    // producer.  broker_proto 4→5 (audit R3.5b, 2026-05-19) renamed
    // the wire key `uid` → `role_uid` for cross-message consistency.
    // Missing/empty fields → silent drop with WARN log (HEARTBEAT_REQ
    // is fire-and-forget so there is no reply path for INVALID_REQUEST).
    const std::string wire_uid       = req.value("role_uid",  std::string{});
    const std::string wire_role_type = req.value("role_type", std::string{});
    LOGGER_DEBUG("Broker: HEARTBEAT_REQ channel='{}' role_uid='{}' role_type='{}'",
                 channel_name, wire_uid, wire_role_type);

    if (wire_uid.empty() || wire_role_type.empty())
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for '{}' rejected — missing "
                    "'role_uid' or 'role_type' (HEP-CORE-0019 §4.1 "
                    "Phase 6 wire format; broker_proto >=5)",
                    channel_name);
        return;
    }
    // Audit R3.5b (2026-05-19): role_uid grammar + side-aware tag
    // check (HEP-CORE-0033 §G2.2.0b).  Tag must match role_type:
    // producer → {prod, proc}; consumer → {cons, proc}.  Drop with
    // WARN log (fire-and-forget; no reply path).
    if (!pylabhub::hub::is_valid_identifier(
            wire_uid, pylabhub::hub::IdentifierKind::RoleUid))
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for '{}' dropped — invalid "
                    "role_uid '{}' (HEP-CORE-0033 §G2.2.0b)",
                    channel_name, wire_uid);
        return;
    }
    {
        const auto tag_opt = pylabhub::hub::extract_short_tag(wire_uid);
        const std::string_view tag = tag_opt.value_or(std::string_view{"?"});
        bool tag_ok = false;
        if (wire_role_type == "producer")
            tag_ok = (tag == "prod" || tag == "proc");
        else if (wire_role_type == "consumer")
            tag_ok = (tag == "cons" || tag == "proc");
        if (!tag_ok)
        {
            LOGGER_WARN("Broker: HEARTBEAT_REQ for '{}' dropped — "
                        "role_uid tag '{}' does not match role_type "
                        "'{}' (producer expects prod/proc; consumer "
                        "expects cons/proc — HEP-CORE-0033 §G2.2.0b)",
                        channel_name, tag, wire_role_type);
            return;
        }
    }

    // Producer-presence-sub-Live diagnostic: gate on `role_type ==
    // "producer"` so consumer heartbeats don't inflate log volume
    // (audit L3 closure).  The actual FSM transition + counter bump
    // happens inside `_on_heartbeat`; this is pre-emptive observability.
    if (was_pending && wire_role_type == "producer")
    {
        LOGGER_INFO("Broker: channel '{}' producer-presence sub-Live "
                    "(may transition to Live on this heartbeat)", channel_name);
    }

    if (!req.contains("producer_pid") || req["producer_pid"].get<uint64_t>() == 0)
    {
        // `producer_pid` is retained on the wire from Phase 1 for
        // diagnostic / audit purposes only; the broker no longer uses
        // it for presence resolution (the Phase 6 `(channel, uid,
        // role_type)` tuple is authoritative).  Missing or zero pid
        // is logged for diagnostics but does not reject the heartbeat.
        LOGGER_ERROR("Broker: HEARTBEAT_REQ for '{}' missing or zero producer_pid",
                     channel_name);
    }

    // Refresh the presence row keyed on `(role_uid, channel, role_type)`
    // per HEP-CORE-0023 §2.5.2 + HEP-CORE-0019 §2.3.  Each heartbeat
    // refreshes ONLY its own presence row.
    std::optional<nlohmann::json> metrics_opt;
    if (req.contains("metrics") && req["metrics"].is_object())
        metrics_opt = req["metrics"];

    const auto eff = hub_state_->_on_heartbeat(channel_name,
                                                wire_uid,
                                                wire_role_type,
                                                std::chrono::steady_clock::now(),
                                                metrics_opt);

    // HEP-CORE-0023 §2.5 telemetry — first-heartbeat observability.
    // One-shot per presence per session: gate
    // `!was_first_heartbeat_seen` is true on the SAME tick that
    // flipped `first_heartbeat_seen` to true inside `_on_heartbeat`.
    // Bounded by role count × channel count; never per-tick.  Logged
    // at the wire layer (here) rather than inside HubState because
    // HubState is a pure state machine; wire-protocol observability
    // belongs at the wire layer.
    if (eff.presence_found && !eff.was_first_heartbeat_seen)
    {
        LOGGER_INFO("Broker: first heartbeat received from role='{}' "
                    "channel='{}' role_type='{}'",
                    wire_uid, channel_name, wire_role_type);
    }
}

// ============================================================================
// ENDPOINT_UPDATE_REQ handler (HEP-0021 §16)
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_endpoint_update_req(
    const nlohmann::json &req, const zmq::message_t &identity)
{
    const std::string corr_id       = req.value("correlation_id", "");
    const std::string channel_name  = req.value("channel_name", "");
    const std::string endpoint_type = req.value("endpoint_type", "");
    const std::string endpoint      = req.value("endpoint", "");

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

    // Wave M2.5 step 5: resolve which producer the ENDPOINT_UPDATE_REQ
    // targets by matching the sender's ZMQ identity to a registered
    // producer's `zmq_identity`.  Identity-based resolution is more
    // secure than trusting a wire `role_uid` (the identity is bound
    // to the actual connection by ZMTP).  Per HEP-CORE-0021 §16.3
    // each Fan-In producer has its own zmq_node_endpoint — the
    // sender can only update its own, not a sibling's.
    const std::string sender_id(static_cast<const char *>(identity.data()), identity.size());
    std::string sender_role_uid;
    for (const auto &prod : entry->producers)
    {
        if (sender_id == prod.zmq_identity)
        {
            sender_role_uid = prod.role_uid;
            break;
        }
    }
    if (sender_role_uid.empty())
    {
        LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' rejected — sender is not a producer",
                    channel_name);
        return make_error(corr_id, "NOT_CHANNEL_OWNER",
                          "Sender is not a registered producer of channel '" + channel_name + "'");
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
            if (prod.inbox_endpoint.empty()) continue;
            auto inbox_check = pylabhub::validate_tcp_endpoint(prod.inbox_endpoint);
            if (inbox_check.ok() && inbox_check.port == 0)
            {
                LOGGER_ERROR("Broker: ENDPOINT_UPDATE_REQ for '{}' inbox (producer '{}') — "
                             "current port is 0; inbox endpoint should be resolved before "
                             "registration", channel_name, prod.role_uid);
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
                          "endpoint_type must be 'zmq_node', got: '" +
                              endpoint_type + "'");
    }

    // Wave M2.5 step 5 idempotency check: look up the SENDER's
    // current per-producer endpoint (HEP-CORE-0021 §16.3 — Fan-In
    // producers each have their own).  Per-producer scope means
    // siblings' endpoints are untouched regardless of outcome.
    const auto current_opt =
        entry->producer_zmq_node_endpoint(sender_role_uid);
    const std::string current_value = current_opt.value_or(std::string{});
    auto current = pylabhub::validate_tcp_endpoint(current_value);
    if (current.ok() && current.port != 0)
    {
        if (current_value == endpoint)
        {
            LOGGER_DEBUG("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} — "
                         "already set to '{}' (idempotent)",
                         channel_name, sender_role_uid, endpoint_type, endpoint);
        }
        else
        {
            LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} "
                        "rejected — already set to '{}', cannot change to '{}'",
                        channel_name, sender_role_uid, endpoint_type,
                        current_value, endpoint);
            return make_error(corr_id, "ENDPOINT_ALREADY_SET",
                              endpoint_type + " endpoint already set to '" +
                                  current_value + "'");
        }
    }
    else
    {
        LOGGER_INFO("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' {} "
                    "updated to '{}'",
                    channel_name, sender_role_uid, endpoint_type, endpoint);
        // Route to per-producer op: only the sender's endpoint is mutated;
        // sibling producers on the same channel are untouched
        // (HEP-CORE-0021 §16.3).
        if (!hub_state_->_set_producer_zmq_node_endpoint(
                channel_name, sender_role_uid, endpoint))
        {
            // Race: sender was admitted at the start of this handler
            // but the producer has been removed (DEREG / heartbeat
            // timeout) before we got here.
            LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' uid='{}' lost the race "
                        "(producer no longer admitted)",
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
    std::size_t created    = 0;
    std::size_t idempotent = 0;
    std::size_t conflicted = 0;

    for (const auto &[path, entry] : entries)
    {
        // Translate file-form SchemaEntry → wire-form SchemaRecord.  This
        // recomputes the hash under HEP-CORE-0034 §6.3 canonical form;
        // the SHM-header form (`SchemaInfo::hash`) is NOT used here
        // (§2.4 I6 — the two forms are different by design).
        auto rec = pylabhub::hub::to_hub_schema_record(entry);
        const std::string id_for_log = rec.schema_id;  // captured before move

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
                LOGGER_WARN(
                    "Broker: hub-global schema '{}' from '{}' rejected as "
                    "hash_mismatch_self — another (hub, {}) entry already "
                    "registered with a different fingerprint",
                    id_for_log, path, id_for_log);
                ++conflicted;
                break;
            case O::kForbiddenOwner:
                // Defensive — owner is set to "hub" by to_hub_schema_record;
                // this branch indicates an invariant violation in the
                // translator.
                LOGGER_ERROR(
                    "Broker: hub-global schema '{}' from '{}' rejected as "
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

nlohmann::json BrokerServiceImpl::handle_schema_req(const nlohmann::json& req)
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

    // HEP-CORE-0034 §10.3 — owner+id keying.  When both `owner` and
    // `schema_id` are present, look up the SchemaRecord directly in
    // HubState.schemas and return it.  This is the preferred form going
    // forward; the legacy `channel_name` form below is retained for
    // backward compatibility (Phase 3a clients that still ask for
    // schemas by channel).
    const std::string req_owner     = req.value("owner", "");
    const std::string req_schema_id = req.value("schema_id", "");
    if (!req_owner.empty() && !req_schema_id.empty())
    {
        const auto rec = hub_state_->schema(req_owner, req_schema_id);
        if (!rec.has_value())
        {
            LOGGER_WARN("Broker: SCHEMA_REQ no record under ({}, {})",
                        req_owner, req_schema_id);
            return make_error(corr_id, "SCHEMA_UNKNOWN",
                              "No schema record under (" + req_owner + ", " +
                                  req_schema_id + ")");
        }
        nlohmann::json resp;
        resp["status"]    = "success";
        resp["owner"]     = rec->owner_uid;
        resp["schema_id"] = rec->schema_id;
        resp["packing"]   = rec->packing;
        resp["blds"]      = rec->blds;
        resp["schema_hash"] =
            format_tools::bytes_to_hex({reinterpret_cast<const char *>(rec->hash.data()),
                                         rec->hash.size()});
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
        LOGGER_WARN("Broker: SCHEMA_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }
    nlohmann::json resp;
    resp["status"]       = "success";
    resp["channel_name"] = channel_name;
    resp["schema_id"]    = entry->schema_id;
    resp["schema_owner"] = entry->schema_owner;  // HEP-0034 — empty for legacy channels
    resp["blds"]         = entry->schema_blds;
    resp["schema_hash"]  = entry->schema_hash;
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ============================================================================
// Role identity policy helpers (placeholder — pending HEP-CORE-0035)
// ============================================================================

RoleIdentityPolicy
BrokerServiceImpl::effective_role_identity_policy(const std::string& channel_name) const noexcept
{
    for (const auto& cp : cfg.channel_policy_overrides)
    {
        if (channel_name_matches_glob(channel_name, cp.channel_glob))
        {
            return cp.policy;
        }
    }
    return cfg.role_identity_policy;
}

std::optional<nlohmann::json>
BrokerServiceImpl::check_role_identity(const std::string& channel_name,
                                       const std::string& role_name,
                                       const std::string& role_uid,
                                       const std::string& corr_id,
                                       bool               is_consumer) const
{
    const RoleIdentityPolicy policy   = effective_role_identity_policy(channel_name);
    const std::string        role_str = is_consumer ? "consumer" : "producer";

    if (policy == RoleIdentityPolicy::Required || policy == RoleIdentityPolicy::Verified)
    {
        if (role_name.empty() || role_uid.empty())
        {
            LOGGER_WARN("Broker: policy={} rejected {} for '{}': missing role_name/uid",
                        role_identity_policy_to_str(policy), role_str, channel_name);
            return make_error(corr_id, "IDENTITY_REQUIRED",
                              fmt::format("Role identity policy '{}' requires role_name and role_uid",
                                          role_identity_policy_to_str(policy)));
        }
    }

    if (policy == RoleIdentityPolicy::Verified)
    {
        const bool found = std::any_of(
            cfg.known_roles.begin(), cfg.known_roles.end(),
            [&](const KnownRole& ka)
            {
                if (ka.name != role_name || ka.uid != role_uid)
                {
                    return false;
                }
                const bool role_ok = ka.role.empty() || ka.role == "any" || ka.role == role_str;
                return role_ok;
            });
        if (!found)
        {
            LOGGER_WARN("Broker: Verified policy rejected {} '{}' uid='{}' for '{}': "
                        "not in known_roles",
                        role_str, role_name, role_uid, channel_name);
            return make_error(corr_id, "NOT_IN_KNOWN_ROLES",
                              fmt::format("Role '{}' (uid={}) is not in the hub's known_roles list",
                                          role_name, role_uid));
        }
    }

    if (policy != RoleIdentityPolicy::Open && (!role_name.empty() || !role_uid.empty()))
    {
        LOGGER_INFO("Broker: {} identity recorded for '{}': name='{}' uid='{}'",
                    role_str, channel_name, role_name, role_uid);
    }

    return std::nullopt;
}

std::optional<nlohmann::json>
BrokerServiceImpl::verify_known_role_binding(const std::string& role_uid,
                                              const std::string& claimed_pubkey,
                                              const std::string& corr_id,
                                              const char*        request_kind) const
{
    auto it = std::find_if(cfg.known_roles.begin(), cfg.known_roles.end(),
                           [&](const KnownRole& kr) { return kr.uid == role_uid; });
    if (it == cfg.known_roles.end())
    {
        LOGGER_WARN("Broker: {} rejected — role_uid='{}' not in known_roles "
                    "(HEP-CORE-0036 §6.1/§6.3 Layer-2 verification)",
                    request_kind, role_uid);
        return make_error(corr_id, "UNKNOWN_ROLE",
                          fmt::format("role_uid '{}' is not registered in "
                                      "known_roles.json",
                                      role_uid));
    }

    if (it->pubkey_z85 != claimed_pubkey)
    {
        LOGGER_WARN("Broker: {} rejected — role_uid='{}' presented pubkey "
                    "that does not match known_roles record "
                    "(HEP-CORE-0036 §6.1/§6.3 Layer-2 verification)",
                    request_kind, role_uid);
        return make_error(corr_id, "PUBKEY_MISMATCH",
                          fmt::format("zmq_pubkey for role_uid '{}' does not "
                                      "match the configured pubkey in "
                                      "known_roles.json",
                                      role_uid));
    }

    return std::nullopt;
}

// ============================================================================
// Audit R3.5b (2026-05-19) — wire-boundary grammar + side-aware tag check
// ============================================================================

namespace
{

/// Build a comma-separated list of allowed tags for error/log messages.
std::string format_expected_tags(std::initializer_list<std::string_view> tags)
{
    std::string out;
    bool first = true;
    for (auto t : tags)
    {
        if (!first) out += ',';
        out += t;
        first = false;
    }
    return out;
}

/// Returns true iff the uid's role tag is in `expected_tags`.  Requires
/// the uid already grammar-valid (caller checks `is_valid_identifier`
/// with `RoleUid` first; `extract_short_tag` is defined as nullopt on
/// invalid input).
bool short_tag_matches(const std::string& role_uid,
                      std::initializer_list<std::string_view> expected_tags)
{
    const auto tag_opt = pylabhub::hub::extract_short_tag(role_uid);
    if (!tag_opt.has_value()) return false;
    for (auto t : expected_tags)
    {
        if (*tag_opt == t) return true;
    }
    return false;
}

} // anonymous namespace

std::optional<nlohmann::json>
BrokerServiceImpl::validate_identity_fields(const std::string& channel_name,
                                            const std::string& role_uid,
                                            const std::string& role_name,
                                            std::initializer_list<std::string_view> expected_tags,
                                            const std::string& corr_id,
                                            const char*        handler_label) const
{
    using pylabhub::hub::is_valid_identifier;
    using pylabhub::hub::IdentifierKind;

    if (!is_valid_identifier(channel_name, IdentifierKind::Channel))
    {
        LOGGER_WARN("Broker: {} rejected — invalid channel_name '{}' "
                     "(HEP-CORE-0033 §G2.2.0b — empty or grammar violation)",
                     handler_label, channel_name);
        return make_error(corr_id, "INVALID_REQUEST",
                          "channel_name '" + channel_name +
                              "' failed grammar validation "
                              "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (!is_valid_identifier(role_uid, IdentifierKind::RoleUid))
    {
        LOGGER_WARN("Broker: {} rejected on channel '{}' — invalid "
                     "role_uid '{}' (HEP-CORE-0033 §G2.2.0b — must be "
                     "(prod|cons|proc).<name>.<unique>, non-empty)",
                     handler_label, channel_name, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "role_uid '" + role_uid +
                              "' failed grammar validation "
                              "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (!short_tag_matches(role_uid, expected_tags))
    {
        const auto tag_opt = pylabhub::hub::extract_short_tag(role_uid);
        const auto tag = tag_opt.has_value() ? std::string{*tag_opt} : std::string{"?"};
        const auto allowed = format_expected_tags(expected_tags);
        LOGGER_WARN("Broker: {} rejected on channel '{}' — role_uid '{}' "
                     "has tag '{}' but handler expects {{{}}} "
                     "(HEP-CORE-0033 §G2.2.0b — processor roles use "
                     "'proc.' on both sides)",
                     handler_label, channel_name, role_uid, tag, allowed);
        return make_error(corr_id, "INVALID_ROLE_TAG",
                          "role_uid tag '" + tag + "' not allowed on this "
                          "wire message (expected one of: " + allowed + ")");
    }
    if (!role_name.empty() &&
        !is_valid_identifier(role_name, IdentifierKind::RoleName))
    {
        LOGGER_WARN("Broker: {} rejected on channel '{}' uid='{}' — "
                     "invalid role_name '{}' (HEP-CORE-0033 §G2.2.0b)",
                     handler_label, channel_name, role_uid, role_name);
        return make_error(corr_id, "INVALID_REQUEST",
                          "role_name '" + role_name +
                              "' failed grammar validation "
                              "(HEP-CORE-0033 §G2.2.0b)");
    }
    return std::nullopt;
}

std::optional<nlohmann::json>
BrokerServiceImpl::validate_role_uid_only(const std::string& role_uid,
                                          std::initializer_list<std::string_view> expected_tags,
                                          const std::string& corr_id,
                                          const char*        handler_label) const
{
    if (!pylabhub::hub::is_valid_identifier(
            role_uid, pylabhub::hub::IdentifierKind::RoleUid))
    {
        LOGGER_WARN("Broker: {} rejected — invalid role_uid '{}' "
                     "(HEP-CORE-0033 §G2.2.0b — must be "
                     "(prod|cons|proc).<name>.<unique>, non-empty)",
                     handler_label, role_uid);
        return make_error(corr_id, "INVALID_REQUEST",
                          "role_uid '" + role_uid +
                              "' failed grammar validation "
                              "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (!short_tag_matches(role_uid, expected_tags))
    {
        const auto tag_opt = pylabhub::hub::extract_short_tag(role_uid);
        const auto tag = tag_opt.has_value() ? std::string{*tag_opt} : std::string{"?"};
        const auto allowed = format_expected_tags(expected_tags);
        LOGGER_WARN("Broker: {} rejected — role_uid '{}' has tag '{}' "
                     "but handler expects {{{}}} "
                     "(HEP-CORE-0033 §G2.2.0b)",
                     handler_label, role_uid, tag, allowed);
        return make_error(corr_id, "INVALID_ROLE_TAG",
                          "role_uid tag '" + tag + "' not allowed on this "
                          "wire message (expected one of: " + allowed + ")");
    }
    return std::nullopt;
}

// ============================================================================
// Heartbeat negotiation block (HEP-CORE-0023 §2.5)
// ============================================================================

nlohmann::json BrokerServiceImpl::heartbeat_ack_block() const
{
    nlohmann::json hb;
    hb["heartbeat_interval_ms"]    = static_cast<int64_t>(
        cfg.heartbeat_interval.count());
    hb["ready_miss_heartbeats"]    = cfg.ready_miss_heartbeats;
    hb["pending_miss_heartbeats"]  = cfg.pending_miss_heartbeats;
    return hb;
}

// ============================================================================
// Heartbeat timeout detection
// ============================================================================

void BrokerServiceImpl::check_heartbeat_timeouts(zmq::socket_t& socket)
{
    // Two-pass role liveness state machine (HEP-CORE-0023 §2.5).
    // Pass 1: Ready -> Pending on heartbeat absence (producer presumed unresponsive).
    // Pass 2: Pending -> deregistered immediately on extended absence
    //         (no Closing/grace — producer is presumed dead, no one to wait for).
    // Timeouts are ALWAYS enforced; effective_*_timeout() is floored at 1 heartbeat.
    const auto ready_timeout   = cfg.effective_ready_timeout();
    const auto pending_timeout = cfg.effective_pending_timeout();

    const auto snap = hub_state_->snapshot();
    const auto now  = std::chrono::steady_clock::now();

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
        std::string role_type;  ///< "producer" | "consumer"
    };
    std::vector<Pass1Decision> p1;
    for (const auto &[channel_name, entry] : snap.channels)
    {
        pylabhub::hub::for_each_presence_matching(
            entry, snap.roles,
            [&](const pylabhub::hub::RolePresence &p) {
                return p.state == pylabhub::hub::RoleState::Connected
                    && (now - p.last_heartbeat) >= ready_timeout;
            },
            [&](const pylabhub::hub::PresenceSweepTarget &t) {
                Pass1Decision d;
                d.channel  = t.channel;
                d.role_uid =
                    (t.party == pylabhub::hub::PartyKind::Producer
                         ? t.producer->role_uid
                         : t.consumer->role_uid);
                d.role_type =
                    (t.party == pylabhub::hub::PartyKind::Producer
                         ? "producer" : "consumer");
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
        pylabhub::hub::PartyKind                       party;
        std::string                                    channel;
        std::string                                    role_uid;
        pylabhub::hub::ChannelEntry                    pre_drop_channel;
        std::optional<pylabhub::hub::ConsumerEntry>    pre_drop_consumer;
    };
    for (const auto &[channel_name, entry] : snap2.channels)
    {
        std::vector<Pass2Decision> p2;
        pylabhub::hub::for_each_presence_matching(
            entry, snap2.roles,
            [&](const pylabhub::hub::RolePresence &p) {
                return p.state == pylabhub::hub::RoleState::Pending
                    && (now - p.state_since) >= pending_timeout;
            },
            [&](const pylabhub::hub::PresenceSweepTarget &t) {
                Pass2Decision d;
                d.party             = t.party;
                d.channel           = t.channel;
                d.pre_drop_channel  = *t.channel_entry;
                if (t.party == pylabhub::hub::PartyKind::Producer)
                {
                    d.role_uid = t.producer->role_uid;
                }
                else
                {
                    d.role_uid          = t.consumer->role_uid;
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
            if (d.party != pylabhub::hub::PartyKind::Producer) continue;

            LOGGER_WARN(
                "Broker: producer '{}' on '{}' reclaimed from Pending "
                "(no heartbeat within {} ms)",
                d.role_uid, d.channel, pending_timeout.count());

            auto drop = hub_state_->_on_pending_timeout(
                d.channel, d.role_uid, "producer");
            if (drop.removed && drop.channel_now_empty)
            {
                // Last-producer drop → atomic channel teardown.
                // Notify consumers + federation peers using the
                // pre_drop snapshot (the channel record is now gone).
                send_closing_notify(socket, d.channel, d.pre_drop_channel,
                                    "pending_timeout");
                on_channel_closed(socket, d.channel, d.pre_drop_channel,
                                  "pending_timeout");
                // M1.4 (2026-05-11): no metrics_store_.erase — see
                // comment at handle_dereg_req last-producer path.
                LOGGER_INFO("Broker: channel '{}' torn down (last "
                            "producer presence-timeout)", d.channel);
                channel_torn_down = true;
                break;
            }
            else if (drop.removed)
            {
                LOGGER_INFO("Broker: producer '{}' dropped on '{}' "
                            "(presence-timeout; {} producer(s) remain — "
                            "channel survives)",
                            d.role_uid, d.channel,
                            static_cast<uint32_t>(
                                d.pre_drop_channel.producer_count() - 1));
            }
        }

        // Wave-B M2 (3/3): consumer-presence Pending → Disconnected.
        // Skip on torn-down channels — `_on_channel_closed` already
        // demoted every consumer-presence on this channel to
        // Disconnected and the surviving consumer list is empty.
        if (channel_torn_down) continue;

        for (const auto &d : p2)
        {
            if (d.party != pylabhub::hub::PartyKind::Consumer) continue;

            LOGGER_WARN(
                "Broker: consumer '{}' on '{}' reclaimed from Pending "
                "(no heartbeat within {} ms)",
                d.role_uid, d.channel, pending_timeout.count());

            auto drop = hub_state_->_on_pending_timeout(
                d.channel, d.role_uid, "consumer");
            if (!drop.removed) continue;

            // Fan out CONSUMER_DIED_NOTIFY with reason="heartbeat_timeout"
            // to every producer on the channel — symmetric with the
            // PID-death path at `check_dead_consumers` (which uses
            // reason="process_dead").  Producers consume the
            // notification per HEP-CORE-0023 §2.1.1 to drop their
            // per-consumer bookkeeping.
            // broker_proto 4→5 (audit R3.5b, 2026-05-19): notify body
            // uses `role_uid` (the consumer's role.uid; tag `cons.` or
            // `proc.` is embedded in the value) — replaces the legacy
            // `consumer_uid` field for cross-message uniformity.
            const auto &pre_drop_consumer = *d.pre_drop_consumer;
            const auto &pre_drop_channel  = d.pre_drop_channel;
            nlohmann::json notify;
            notify["channel_name"]      = d.channel;
            notify["role_uid"]          = pre_drop_consumer.role_uid;
            notify["consumer_pid"]      = pre_drop_consumer.consumer_pid;
            notify["consumer_hostname"] = pre_drop_consumer.consumer_hostname;
            notify["reason"]            = "heartbeat_timeout";
            pylabhub::hub::for_each_party_identity(
                pre_drop_channel, pylabhub::hub::PartyKind::Producer,
                [&](std::string_view zmq_identity,
                    std::string_view producer_uid) {
                    send_to_identity(socket, std::string(zmq_identity),
                                     "CONSUMER_DIED_NOTIFY", notify);
                    LOGGER_INFO("Broker: CONSUMER_DIED_NOTIFY to producer of '{}': "
                                "role_uid={} reason=heartbeat_timeout target_role={}",
                                d.channel, d.role_uid, producer_uid);
                });
            // Federation / observer fan-out (mirrors the PID-death path).
            on_consumer_closed(socket, d.channel, pre_drop_consumer,
                               "heartbeat_timeout");
        }
    }
}

void BrokerServiceImpl::check_dead_consumers(zmq::socket_t& socket)
{
    if (cfg.consumer_liveness_check_interval.count() <= 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_consumer_check < cfg.consumer_liveness_check_interval)
    {
        return;
    }
    m_last_consumer_check = now;

    const auto snap = hub_state_->snapshot();
    for (const auto& [channel_name, entry] : snap.channels)
    {
        std::vector<pylabhub::hub::ConsumerEntry> dead;
        for (const auto& c : entry.consumers)
        {
            if (!pylabhub::platform::is_process_alive(c.consumer_pid))
            {
                dead.push_back(c);
            }
        }
        for (const auto& dead_consumer : dead)
        {
            // Cat 2: dead consumer — notify every producer + clean state.
            // Multi-producer fan-out per HEP-CORE-0023 §2.1.1.
            LOGGER_WARN(
                "Broker: Cat2 dead consumer pid={} host='{}' on channel '{}' — removing",
                dead_consumer.consumer_pid, dead_consumer.consumer_hostname, channel_name);
            // broker_proto 4→5 (audit R3.5b, 2026-05-19): `consumer_uid`
            // → `role_uid` for cross-message uniformity.
            nlohmann::json notify;
            notify["channel_name"]      = channel_name;
            notify["role_uid"]          = dead_consumer.role_uid;
            notify["consumer_pid"]      = dead_consumer.consumer_pid;
            notify["consumer_hostname"] = dead_consumer.consumer_hostname;
            notify["reason"]            = "process_dead";
            for (const auto &prod : entry.producers)
            {
                if (prod.zmq_identity.empty()) continue;
                send_to_identity(socket, prod.zmq_identity, "CONSUMER_DIED_NOTIFY",
                                 notify);
                LOGGER_INFO("Broker: CONSUMER_DIED_NOTIFY to producer of '{}': "
                            "consumer_pid={}, reason=process_dead, target_role={}",
                            channel_name, dead_consumer.consumer_pid, prod.role_uid);
            }
            hub_state_->_on_consumer_left(channel_name, dead_consumer.role_uid);
            on_consumer_closed(socket, channel_name, dead_consumer, "process_dead");
            // HEP-CORE-0036 §6.5: revoke + notify on heartbeat-timeout
            // path.  CONSUMER_REG_REQ hard-rejects empty / non-40-char
            // `zmq_pubkey` at the wire (HEP-CORE-0035 §2 unconditional
            // CURVE), so every stored ConsumerEntry MUST carry a
            // 40-char Z85 key.  See the matching tripwire in
            // handle_consumer_dereg_req for rationale.
            if (dead_consumer.zmq_pubkey.size() != 40)
            {
                LOGGER_ERROR(
                    "Broker: ConsumerEntry on channel='{}' role_uid='{}' "
                    "has invalid zmq_pubkey (length {}, expected 40 Z85 "
                    "chars).  This SHOULD NOT happen — CONSUMER_REG_REQ "
                    "hard-rejects empty / wrong-length pubkey at the wire "
                    "(HEP-CORE-0035 §2 unconditional CURVE).  System may "
                    "be compromised; restart the hub ASAP.  Skipping "
                    "channel-access revocation.",
                    channel_name, dead_consumer.role_uid,
                    dead_consumer.zmq_pubkey.size());
            }
            else
            {
                hub_state_->_on_consumer_revoked(channel_name,
                                                  dead_consumer.zmq_pubkey);
                fire_channel_auth_changed_notify(socket, channel_name,
                                                  "consumer_timeout");
            }
        }
    }
}

void BrokerServiceImpl::send_closing_notify(zmq::socket_t&                     socket,
                                             const std::string&                 channel_name,
                                             const pylabhub::hub::ChannelEntry& entry,
                                             const std::string&                 reason)
{
    nlohmann::json body;
    body["channel_name"] = channel_name;
    body["reason"]       = reason;

    // Notify all registered consumers.
    for (const auto& consumer : entry.consumers)
    {
        if (consumer.zmq_identity.empty())
        {
            continue;
        }
        try
        {
            send_to_identity(socket, consumer.zmq_identity, "CHANNEL_CLOSING_NOTIFY", body);
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' ->consumer pid={}",
                        channel_name, consumer.consumer_pid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to notify consumer pid={} for '{}': {}",
                        consumer.consumer_pid, channel_name, e.what());
        }
    }

    // Also notify every registered producer (HEP-CORE-0023 §2.1.1).
    for (const auto &prod : entry.producers)
    {
        if (prod.zmq_identity.empty()) continue;
        try
        {
            send_to_identity(socket, prod.zmq_identity, "CHANNEL_CLOSING_NOTIFY",
                             body);
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' ->producer pid={} role={}",
                        channel_name, prod.producer_pid, prod.role_uid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to notify producer {} for '{}': {}",
                        prod.role_uid, channel_name, e.what());
        }
    }
}

// ============================================================================
// Role-close cleanup API (HEP-CORE-0023 §2.5)
// ============================================================================

void BrokerServiceImpl::on_channel_closed(zmq::socket_t&                     /*socket*/,
                                           const std::string&                 channel_name,
                                           const pylabhub::hub::ChannelEntry& entry,
                                           const std::string&                 reason)
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

void BrokerServiceImpl::on_consumer_closed(zmq::socket_t&                      /*socket*/,
                                            const std::string&                  /*channel_name*/,
                                            const pylabhub::hub::ConsumerEntry& /*consumer*/,
                                            const std::string&                  /*reason*/)
{
    // Wave M3 step 5f (2026-05-11) — see `on_channel_closed`.  Consumer
    // role-disconnect band cleanup is also handler-driven now.  This
    // hook is kept for future broker-side reactions to consumer-close
    // that don't fit the role-disconnect path (e.g., per-consumer
    // observability).
}

void BrokerServiceImpl::federation_on_channel_closed(
    const std::string&                          /*channel_name*/,
    const pylabhub::hub::ChannelEntry&          /*entry*/,
    const std::string&                          /*reason*/)
{
    // No broker-internal index to maintain (relay targets are computed
    // on-the-fly from `hub_state_->snapshot().peers`).  Stale channel-name
    // entries in a peer's `relay_channels` are benign: relay_notify_to_peers
    // only fires when a NOTIFY arrives for a live channel name; if the
    // channel is gone, no NOTIFY arrives.
}

void BrokerServiceImpl::send_band_leave_notify(zmq::socket_t&    socket,
                                                 const std::string& band_name,
                                                 const std::string& role_uid,
                                                 const std::string& reason)
{
    if (role_uid.empty() || band_name.empty()) return;
    // HubState has already removed the leaving uid from band members
    // (under the writer lock, before firing this handler).  Query the
    // current member list and notify each remaining member.  If the
    // band was auto-deleted (uid was its last member), `band(name)` is
    // nullopt and there's nothing to notify.
    // Log the leave regardless of whether the band survived (matches
    // prior imperative behaviour so diagnostic output is consistent).
    LOGGER_INFO("Broker: role '{}' removed from band '{}' (reason={})",
                role_uid, band_name, reason);
    auto remaining = hub_state_->band(band_name);
    if (!remaining.has_value()) return;  // auto-deleted; no NOTIFY targets
    nlohmann::json notify;
    notify["band"]     = band_name;     // HEP-CORE-0030 §5.1 wire key
    notify["role_uid"] = role_uid;
    notify["reason"]   = reason;
    for (const auto& m : remaining->members)
    {
        if (!m.zmq_identity.empty())
            send_to_identity(socket, m.zmq_identity, "BAND_LEAVE_NOTIFY", notify);
    }
}

void BrokerServiceImpl::handle_checksum_error_report(zmq::socket_t&        socket,
                                                      const nlohmann::json& req)
{
    const auto channel   = req.value("channel_name", std::string{});
    const auto slot      = req.value("slot_index", -1);
    const auto pid       = req.value("reporter_pid", uint64_t{0});
    const auto error_str = req.value("error", std::string{});
    LOGGER_WARN("Broker: Cat2 checksum error on '{}' slot={} pid={} err='{}'", channel, slot,
                pid, error_str);

    if (cfg.checksum_repair_policy == ChecksumRepairPolicy::NotifyOnly)
    {
        auto entry = hub_state_->channel(channel);
        if (entry)
        {
            nlohmann::json fwd = req;
            fwd["broker_action"] = "notify_only";
            for (const auto& consumer : entry->consumers)
            {
                if (!consumer.zmq_identity.empty())
                {
                    send_to_identity(socket, consumer.zmq_identity, "CHANNEL_EVENT_NOTIFY",
                                     fwd);
                }
            }
            // Fan-out to every producer (HEP-CORE-0023 §2.1.1).
            for (const auto &prod : entry->producers)
            {
                if (prod.zmq_identity.empty()) continue;
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
// CHANNEL_BROADCAST_REQ — fan out message to ALL members of a channel
// ============================================================================

void BrokerServiceImpl::handle_channel_broadcast_req(zmq::socket_t&        socket,
                                                      const nlohmann::json& req)
{
    const auto target_channel = req.value("target_channel", std::string{});
    const auto sender_uid     = req.value("sender_uid",     std::string{});
    const auto message        = req.value("message",        std::string{});

    if (target_channel.empty())
    {
        LOGGER_WARN("Broker: CHANNEL_BROADCAST_REQ with empty target_channel");
        return;
    }

    auto entry = hub_state_->channel(target_channel);
    if (!entry)
    {
        LOGGER_DEBUG("Broker: CHANNEL_BROADCAST_REQ for '{}' — channel not found",
                     target_channel);
        return;
    }

    // Build the broadcast notification body.
    nlohmann::json fwd;
    fwd["channel_name"] = target_channel;
    fwd["event"]        = "broadcast";
    fwd["sender_uid"]   = sender_uid;
    fwd["message"]      = message;
    if (req.contains("data") && req["data"].is_string())
        fwd["data"] = req["data"];

    // Fan out to ALL consumers.
    for (const auto& consumer : entry->consumers)
    {
        if (consumer.zmq_identity.empty())
            continue;
        try
        {
            send_to_identity(socket, consumer.zmq_identity, "CHANNEL_BROADCAST_NOTIFY", fwd);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: broadcast to consumer pid={} for '{}' failed: {}",
                        consumer.consumer_pid, target_channel, e.what());
        }
    }

    // Also send to every registered producer (HEP-CORE-0023 §2.1.1).
    for (const auto &prod : entry->producers)
    {
        if (prod.zmq_identity.empty()) continue;
        try
        {
            send_to_identity(socket, prod.zmq_identity,
                             "CHANNEL_BROADCAST_NOTIFY", fwd);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: broadcast to producer {} for '{}' failed: {}",
                        prod.role_uid, target_channel, e.what());
        }
    }

    LOGGER_DEBUG("Broker: CHANNEL_BROADCAST_REQ '{}' msg='{}' ->{} consumers + {} producer(s)",
                 target_channel, message, entry->consumers.size(),
                 entry->producers.size());

    // HEP-CORE-0022: relay to federation peers subscribed to this channel.
    // [BR6] Use fixed event name "broadcast" and put the message in the payload field,
    // consistent with how CHANNEL_EVENT_NOTIFY delivers local broadcast events.
    const std::string data_str = req.contains("data") && req["data"].is_string()
                                     ? req["data"].get<std::string>() : std::string{};
    const std::string relay_payload = data_str.empty() ? message
                                                       : message + "|" + data_str;
    relay_notify_to_peers(socket, target_channel, "broadcast", sender_uid, relay_payload);
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
    for (const auto& [name, entry] : snap.channels)
    {
        nlohmann::json ch;
        ch["name"]            = name;
        // Multi-producer channels (HEP-CORE-0023 §2.1.1): expose the
        // full list as `producer_uids`; `producer_uid` is the first
        // for back-compat with single-producer admin clients.
        nlohmann::json producer_uids = nlohmann::json::array();
        for (const auto &p : entry.producers) producer_uids.push_back(p.role_uid);
        ch["producer_uids"]  = std::move(producer_uids);
        ch["producer_uid"]   = entry.producers.empty()
                                   ? std::string{}
                                   : entry.producers.front().role_uid;
        ch["schema_id"]      = entry.schema_id;
        ch["consumer_count"] = entry.consumers.size();
        // HEP-CORE-0023 §2.2 — channel state is the protocol-defined
        // `observable`, derived from the producer-presence row.
        ch["observable"] = pylabhub::hub::to_string(
            pylabhub::hub::observe_channel(entry, snap));
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

nlohmann::json BrokerServiceImpl::handle_role_presence_req(const nlohmann::json& req)
{
    // HEP-CORE-0007 §"ROLE_PRESENCE_REQ" — wire field `role_uid`
    // (unified with REG_REQ / CONSUMER_REG_REQ; old `uid` form retired
    // 2026-05-09 as part of the protocol-doc-vs-code unification).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string uid     = req.value("role_uid", "");
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
    // Audit R3.5b (2026-05-19): grammar check (HEP-CORE-0033 §G2.2.0b).
    // Side-agnostic — any role kind may be queried.  Pre-fix, a
    // malformed role_uid would scan-miss and return `present:false`,
    // making "absent" indistinguishable from "malformed query".
    if (auto err = validate_role_uid_only(uid,
                                          {"prod", "cons", "proc"},
                                          corr_id, "ROLE_PRESENCE_REQ"))
    {
        return *err;
    }

    // Scan all channels: check every producer + each consumer
    // (HEP-CORE-0023 §2.1.1 multi-producer aware).
    const auto snap = hub_state_->snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        if (entry.find_producer(uid) != nullptr)
        {
            nlohmann::json resp;
            resp["present"] = true;
            resp["channel"] = name;
            resp["role"]    = "producer";
            if (!corr_id.empty())
                resp["correlation_id"] = corr_id;
            LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' found as producer on '{}'",
                         uid, name);
            return resp;
        }
        for (const auto& c : entry.consumers)
        {
            if (!c.role_uid.empty() && c.role_uid == uid)
            {
                nlohmann::json resp;
                resp["present"] = true;
                resp["channel"] = name;
                resp["role"]    = "consumer";
                if (!corr_id.empty())
                    resp["correlation_id"] = corr_id;
                LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' found as consumer on '{}'",
                             uid, name);
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

nlohmann::json BrokerServiceImpl::handle_role_info_req(const nlohmann::json& req)
{
    // HEP-CORE-0007 §"ROLE_INFO_REQ" — wire field `role_uid`
    // (unified with REG_REQ / CONSUMER_REG_REQ / ROLE_PRESENCE_REQ;
    // old `uid` form retired 2026-05-09).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string uid     = req.value("role_uid", "");
    if (uid.empty())
    {
        // Standard error envelope per HEP-CORE-0007 §12.3 + §12.4a
        // (`MISSING_ROLE_UID`).  Pre-2026-05-10 this handler emitted
        // an ad-hoc `{"found": false, "error": "..."}` shape that
        // diverged from the broker-wide error envelope.
        return make_error(corr_id, "MISSING_ROLE_UID", "missing role_uid");
    }
    // Audit R3.5b (2026-05-19): grammar check (HEP-CORE-0033 §G2.2.0b).
    // Mirrors ROLE_PRESENCE_REQ — side-agnostic.
    if (auto err = validate_role_uid_only(uid,
                                          {"prod", "cons", "proc"},
                                          corr_id, "ROLE_INFO_REQ"))
    {
        return *err;
    }

    // Search for a channel where `uid` is a registered producer
    // (HEP-CORE-0023 §2.1.1 multi-producer aware).  Inbox info lives
    // per-ProducerEntry (HEP-CORE-0027) — read from the matched producer.
    const auto snap = hub_state_->snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        const auto *prod = entry.find_producer(uid);
        if (prod != nullptr)
        {
            nlohmann::json resp;
            resp["found"]           = !prod->inbox_endpoint.empty();
            resp["channel"]         = name;
            resp["inbox_endpoint"]  = prod->inbox_endpoint;
            resp["inbox_packing"]   = prod->inbox_packing;
            resp["inbox_checksum"]  = prod->inbox_checksum;
            if (!prod->inbox_schema_json.empty())
            {
                try
                {
                    resp["inbox_schema"] = nlohmann::json::parse(prod->inbox_schema_json);
                }
                catch (const nlohmann::json::exception &je)
                {
                    // Stored schema string is malformed.  REG_REQ
                    // validation in handle_reg_req should have rejected
                    // this — reaching here means stored state is
                    // corrupt.  Surface a warning instead of silently
                    // returning an empty schema (which the consumer
                    // would happily use, masking the corruption).
                    LOGGER_WARN("Broker: stored inbox_schema_json for "
                                "channel '{}' producer '{}' is malformed: {}; "
                                "returning empty array",
                                name, uid, je.what());
                    resp["inbox_schema"] = nlohmann::json::array();
                }
            }
            else
            {
                resp["inbox_schema"] = nlohmann::json::array();
            }
            if (!corr_id.empty())
                resp["correlation_id"] = corr_id;
            LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found on '{}', inbox='{}'",
                         uid, name, prod->inbox_endpoint);
            return resp;
        }
    }

    // Search consumer entries across all channels.
    for (const auto& [name, entry] : snap.channels)
    {
        for (const auto& cons : entry.consumers)
        {
            if (!cons.role_uid.empty() && cons.role_uid == uid)
            {
                nlohmann::json resp;
                resp["found"]           = !cons.inbox_endpoint.empty();
                resp["channel"]         = name;
                resp["inbox_endpoint"]  = cons.inbox_endpoint;
                resp["inbox_packing"]   = cons.inbox_packing;
                resp["inbox_checksum"]  = cons.inbox_checksum;
                if (!cons.inbox_schema_json.empty())
                {
                    try
                    {
                        resp["inbox_schema"] = nlohmann::json::parse(cons.inbox_schema_json);
                    }
                    catch (const nlohmann::json::exception &je)
                    {
                        // Same rationale as the producer-entry path
                        // above — stored consumer inbox_schema_json is
                        // corrupt; log instead of silently returning
                        // an empty array.
                        LOGGER_WARN("Broker: stored consumer "
                                    "inbox_schema_json for channel '{}' "
                                    "uid='{}' is malformed: {}; "
                                    "returning empty array",
                                    name, uid, je.what());
                        resp["inbox_schema"] = nlohmann::json::array();
                    }
                }
                else
                {
                    resp["inbox_schema"] = nlohmann::json::array();
                }
                if (!corr_id.empty())
                    resp["correlation_id"] = corr_id;
                LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found as consumer on '{}', inbox='{}'",
                             uid, name, cons.inbox_endpoint);
                return resp;
            }
        }
    }

    LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' not found", uid);
    nlohmann::json resp;
    resp["found"] = false;
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// ============================================================================
// send_to_identity — push unsolicited message to a connected DEALER by raw identity
// ============================================================================

void BrokerServiceImpl::send_to_identity(zmq::socket_t&        socket,
                                          const std::string&    identity,
                                          const std::string&    msg_type,
                                          const nlohmann::json& body)
{
    const std::string body_str = body.dump();
    socket.send(zmq::message_t(identity.data(), identity.size()), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(msg_type.data(), msg_type.size()), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(body_str.data(), body_str.size()), zmq::send_flags::none);
}

// ============================================================================
// Helpers
// ============================================================================

void BrokerServiceImpl::send_reply(zmq::socket_t&       socket,
                                   const zmq::message_t& identity,
                                   const std::string&    msg_type_ack,
                                   const nlohmann::json& body)
{
    LOGGER_TRACE("Broker: send {} status='{}'", msg_type_ack, body.value("status", ""));
    // Reply layout: [identity, 'C', ack_type_string, json_body]
    const std::string body_str = body.dump();
    socket.send(zmq::message_t(identity.data(), identity.size()), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(msg_type_ack.data(), msg_type_ack.size()),
                zmq::send_flags::sndmore);
    socket.send(zmq::message_t(body_str.data(), body_str.size()), zmq::send_flags::none);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
nlohmann::json BrokerServiceImpl::make_error(const std::string& correlation_id,
                                              const std::string& error_code,
                                              const std::string& message)
{
    nlohmann::json err;
    err["status"]     = "error";
    err["error_code"] = error_code;
    err["message"]    = message;
    if (!correlation_id.empty())
    {
        err["correlation_id"] = correlation_id;
    }
    return err;
}

// ============================================================================
// BrokerService — Pimpl delegation
// ============================================================================

BrokerService::BrokerService(Config cfg, pylabhub::hub::HubState& state)
    : pImpl(std::make_unique<BrokerServiceImpl>())
{
    // HEP-CORE-0035 §2 — CURVE is unconditional.
    // HEP-CORE-0040 §172 — hub identity bytes live in the process
    // KeyStore under `"hub_identity"`; production seeds it via
    // `HubConfig::load_keypair(password)` before `HubHost::startup`
    // constructs the broker, tests via `CurveKeyStoreFixture`.  An
    // absent KeyStore entry is a programmer error (no-bypass
    // discipline, §4.6.5).
    namespace sec = pylabhub::utils::security;
    if (!sec::key_store_ready() || !sec::key_store().has(sec::kHubIdentityName))
        throw std::logic_error(
            "BrokerService: KeyStore entry 'hub_identity' is REQUIRED "
            "(HEP-CORE-0035 §2; HEP-CORE-0040 §172).  Production: "
            "route through HubHost::startup (HubConfig::load_keypair "
            "seeds the KeyStore from HubVault).  Tests: construct a "
            "`pylabhub::tests::CurveKeyStoreFixture` before building "
            "the broker.");
    pImpl->cfg = std::move(cfg);
    pImpl->hub_state_ = &state;  // non-owning; HubHost (or test fixture) owns it
}

BrokerService::~BrokerService() = default;

const pylabhub::hub::HubState& BrokerService::hub_state() const
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
            {"name",           name},
            {"schema_hash",    entry.schema_hash},
            {"consumer_count", static_cast<int>(entry.consumers.size())},
            {"producer_pid",   entry.producers.empty()
                                 ? uint64_t{0}
                                 : entry.producers.front().producer_pid},
            {"producer_pids",  std::move(producer_pids)},
            {"observable",     pylabhub::hub::to_string(
                                   pylabhub::hub::observe_channel(entry, snap))}
        });
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
        e.name               = name;
        e.observable         = pylabhub::hub::to_string(
            pylabhub::hub::observe_channel(entry, hub_snap));
        e.consumer_count     = static_cast<int>(entry.consumers.size());
        e.schema_hash        = entry.schema_hash;
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
        c.ready_to_pending_total,
        c.pending_to_deregistered_total,
        c.pending_to_ready_total,
    };
}

std::string BrokerService::query_metrics_json_str(const std::string& channel) const
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

nlohmann::json BrokerServiceImpl::handle_shm_block_query(const nlohmann::json& req) const
{
    return collect_shm_info(req.value("channel", ""));
}

nlohmann::json BrokerServiceImpl::collect_shm_info(const std::string& channel) const
{
    // Snapshot under HubState's own lock (inside snapshot()), then read SHM
    // outside any broker locks.  HEP-CORE-0036 §5b.4: the SHM segment name
    // is the channel name; no separate `shm_name` field exists anywhere.
    struct BlockInfo
    {
        std::string                                channel;
        uint64_t                                   producer_pid{0};
        std::string                                producer_uid;
        std::string                                producer_name;
        std::vector<pylabhub::hub::ConsumerEntry>  consumers;
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
            bi.channel       = name;
            // SHM channels are physically single-producer (HEP-CORE-0023
            // §2.1.1), so reading the first producer is correct here.
            if (const auto *fp = entry.first_producer())
            {
                bi.producer_pid  = fp->producer_pid;
                bi.producer_uid  = fp->role_uid;
                bi.producer_name = fp->role_name;
            }
            bi.consumers     = entry.consumers;
            blocks.push_back(std::move(bi));
        }
    }

    // For each block, read DataBlockMetrics directly from the SHM header.
    // datablock_get_metrics() opens read-only, reads relaxed-atomic fields, closes.
    nlohmann::json result;
    result["status"] = "success";
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& bi : blocks)
    {
        nlohmann::json blk;
        blk["channel"]  = bi.channel;

        nlohmann::json prod;
        prod["pid"]  = bi.producer_pid;
        prod["uid"]  = bi.producer_uid;
        prod["name"] = bi.producer_name;
        blk["producer"] = std::move(prod);

        nlohmann::json cons_arr = nlohmann::json::array();
        for (const auto& ce : bi.consumers)
        {
            nlohmann::json c;
            c["pid"]  = ce.consumer_pid;
            c["uid"]  = ce.role_uid;
            c["name"] = ce.role_name;
            cons_arr.push_back(std::move(c));
        }
        blk["consumers"] = std::move(cons_arr);

        DataBlockMetrics m{};
        if (::datablock_get_metrics(bi.channel.c_str(), &m) == 0)
        {
            nlohmann::json sm;
            sm["slot_count"]                  = m.slot_count;
            sm["commit_index"]                = m.commit_index;
            sm["total_slots_written"]         = m.total_slots_written;
            sm["total_slots_read"]            = m.total_slots_read;
            sm["total_bytes_written"]         = m.total_bytes_written;
            sm["total_bytes_read"]            = m.total_bytes_read;
            sm["writer_timeout_count"]        = m.writer_timeout_count;
            sm["writer_lock_timeout_count"]   = m.writer_lock_timeout_count;
            sm["writer_reader_timeout_count"] = m.writer_reader_timeout_count;
            sm["writer_blocked_total_ns"]     = m.writer_blocked_total_ns;
            sm["write_lock_contention"]       = m.write_lock_contention;
            sm["write_generation_wraps"]      = m.write_generation_wraps;
            sm["reader_not_ready_count"]      = m.reader_not_ready_count;
            sm["reader_race_detected"]        = m.reader_race_detected;
            sm["reader_validation_failed"]    = m.reader_validation_failed;
            sm["reader_peak_count"]           = m.reader_peak_count;
            sm["checksum_failures"]           = m.checksum_failures;
            sm["slot_acquire_errors"]         = m.slot_acquire_errors;
            sm["slot_commit_errors"]          = m.slot_commit_errors;
            sm["schema_mismatch_count"]       = m.schema_mismatch_count;
            sm["recovery_actions_count"]      = m.recovery_actions_count;
            sm["last_error_code"]             = m.last_error_code;
            sm["last_error_timestamp_ns"]     = m.last_error_timestamp_ns;
            sm["uptime_seconds"]              = m.uptime_seconds;
            sm["creation_timestamp_ns"]       = m.creation_timestamp_ns;
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

std::string BrokerService::collect_shm_info_json(const std::string& channel) const
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
inline bool include(const std::vector<std::string> &filter,
                     const std::string &candidate)
{
    return filter.empty() ||
           std::find(filter.begin(), filter.end(), candidate) != filter.end();
}

// Entry-type serializers (`channel_to_json` / `role_to_json` /
// `band_to_json` / `peer_to_json` / `broker_counters_to_json`) live in
// `utils/hub_state_json.{hpp,cpp}` so AdminService can produce the same
// on-the-wire shape for HEP-CORE-0033 §11.2 query RPCs.  Pulled in via
// the include at the top of this file.

nlohmann::json filter_to_json(const pylabhub::hub::MetricsFilter &f)
{
    nlohmann::json j;
    j["categories"] = std::vector<std::string>(f.categories.begin(),
                                                 f.categories.end());
    j["channels"] = f.channels;
    j["roles"]    = f.roles;
    j["bands"]    = f.bands;
    j["peers"]    = f.peers;
    return j;
}

} // anonymous namespace

nlohmann::json
BrokerService::query_metrics(const pylabhub::hub::MetricsFilter &filter) const
{
    namespace mc = pylabhub::hub::metrics_category;

    // Snapshot HubState under its own lock; release before SHM reads.
    // M1.4 (2026-05-11): legacy `metrics_store_` snapshot retired —
    // metrics live on `HubState.roles[uid].presences[(ch, role_type)].latest_metrics`
    // and are aggregated per-channel via `channel_metrics_snapshot`.
    pylabhub::hub::HubStateSnapshot snap = pImpl->hub_state_->snapshot();

    nlohmann::json result;
    result["status"]     = "success";
    result["queried_at"] = fmt_time(std::chrono::system_clock::now());
    result["filter"]     = filter_to_json(filter);

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
            if (info.contains("blocks") && info["blocks"].is_array() &&
                !info["blocks"].empty())
            {
                auto block = info["blocks"].front();
                block["_collected_at"] = fmt_time(
                    std::chrono::system_clock::now());
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
            sj["owner_uid"]    = key.first;
            sj["schema_id"]    = key.second;
            sj["packing"]      = rec.packing;
            sj["blds"]         = rec.blds;
            sj["hash"]         = pylabhub::format_tools::bytes_to_hex(
                std::string_view(reinterpret_cast<const char *>(rec.hash.data()),
                                  rec.hash.size()));
            sj["_collected_at"] = fmt_time(rec.registered_at);
            schemas[flat] = std::move(sj);
        }
        result["schemas"] = std::move(schemas);
    }

    return result;
}

void BrokerService::request_close_channel(const std::string& name)
{
    LOGGER_TRACE("Broker: request_close_channel('{}')", name);
    std::lock_guard<std::mutex> lk(pImpl->m_close_req_mu);
    pImpl->close_request_queue_.push_back(name);
}

void BrokerService::request_broadcast_channel(const std::string& channel,
                                              const std::string& message,
                                              const std::string& data)
{
    std::lock_guard<std::mutex> lk(pImpl->m_broadcast_req_mu);
    pImpl->broadcast_request_queue_.push_back({channel, message, data});
}

void BrokerService::send_hub_targeted_msg(const std::string& target_hub_uid,
                                          const std::string& channel,
                                          const std::string& payload)
{
    std::lock_guard<std::mutex> lk(pImpl->m_hub_targeted_mu);
    pImpl->hub_targeted_queue_.push_back({target_hub_uid, channel, payload});
}

// M1.4 (2026-05-11): `update_producer_metrics`, `update_consumer_metrics`,
// `handle_metrics_report_req` deleted.  Metrics now piggyback on
// HEARTBEAT_REQ and live on `HubState.roles[uid].presences[(ch, role_type)].latest_metrics`.
// See `hub_state.cpp:_on_heartbeat` for the write path and
// `HubState::channel_metrics_snapshot` for the read path.

nlohmann::json BrokerServiceImpl::handle_metrics_req(const nlohmann::json &req)
{
    // M1.4 (2026-05-11): metrics sourced from `HubState`'s per-presence
    // rows (HEP-CORE-0019 §2.3 Phase 6) via `channel_metrics_snapshot`.
    // Legacy `metrics_store_` retired; the shape is preserved
    // (`status`, `channel`/`channels`, `metrics`).
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel = req.value("channel_name", "");

    nlohmann::json resp;
    resp["status"] = "success";
    if (!channel.empty())
    {
        resp["channel"] = channel;
        resp["metrics"] = hub_state_->channel_metrics_snapshot(channel);
    }
    else
    {
        // All-channels query: iterate snapshot to aggregate.  Pre-fix
        // this iterated `metrics_store_`; post-M1.4 iterate
        // `pImpl->hub_state_->snapshot().channels` and call the helper
        // per channel.
        nlohmann::json channels = nlohmann::json::object();
        for (const auto &[name, ch] : hub_state_->snapshot().channels)
            channels[name] = hub_state_->channel_metrics_snapshot(name);
        resp["channels"] = std::move(channels);
    }
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

void BrokerServiceImpl::handle_hub_peer_hello(zmq::socket_t&        socket,
                                               const zmq::message_t& identity,
                                               const nlohmann::json& payload)
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
    for (const auto& pc : cfg.peers)
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

    const std::string identity_str(static_cast<const char*>(identity.data()), identity.size());

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
    pe.uid             = peer_hub_uid;
    pe.zmq_identity    = identity_str;
    pe.relay_channels  = relay_channels;
    pe.last_seen       = std::chrono::steady_clock::now();
    // state defaults to Connecting; _on_peer_connected forces Connected.
    hub_state_->_on_peer_connected(std::move(pe));

    LOGGER_INFO("Broker: federation peer '{}' connected; relay_channels=[{}]",
                peer_hub_uid, [&]{
                    std::string s;
                    for (const auto& c : relay_channels) { if (!s.empty()) s+=','; s+=c; }
                    return s; }());

    // Send ACK.
    nlohmann::json ack;
    ack["status"]            = "ok";
    ack["accepted_channels"] = relay_channels;
    ack["hub_uid"]           = cfg.self_hub_uid;
    send_reply(socket, identity, "HUB_PEER_HELLO_ACK", ack);

    // [BR1] Fire on_hub_connected only if not already notified (prevents double-fire
    // in bidirectional federation where each side sends HELLO and receives an ACK).
    if (cfg.on_hub_connected && hub_connected_notified_.insert(peer_hub_uid).second)
        cfg.on_hub_connected(peer_hub_uid);
}

void BrokerServiceImpl::handle_hub_peer_bye(const nlohmann::json& payload)
{
    const std::string peer_hub_uid = payload.value("hub_uid", "");
    if (peer_hub_uid.empty()) return;

    auto pre = hub_state_->peer(peer_hub_uid);
    if (!pre.has_value() || pre->state != pylabhub::hub::PeerState::Connected) return;

    // HEP-CORE-0033 §8 retention: peer entry stays with state=Disconnected;
    // observable via snapshot until grace eviction (deferred work).
    hub_state_->_on_peer_disconnected(peer_hub_uid);
    hub_connected_notified_.erase(peer_hub_uid); // [BR1] Allow re-notification on reconnect.

    LOGGER_INFO("Broker: federation peer '{}' sent BYE — marked Disconnected", peer_hub_uid);
    if (cfg.on_hub_disconnected)
        cfg.on_hub_disconnected(peer_hub_uid);
}

void BrokerServiceImpl::handle_hub_peer_hello_ack(const std::string&    peer_hub_uid,
                                                    const nlohmann::json& payload)
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
        LOGGER_WARN("Broker: federation HUB_PEER_HELLO_ACK from '{}': status={}",
                    peer_hub_uid, status);
    }
}

void BrokerServiceImpl::handle_hub_relay_msg(zmq::socket_t&        socket,
                                              const nlohmann::json& payload)
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

    const std::string channel      = payload.value("channel_name",   "");
    const std::string event        = payload.value("event",          "");
    const std::string sender_uid   = payload.value("sender_uid",     "");
    const std::string originator   = payload.value("originator_uid", "");

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
    fwd["channel_name"]   = channel;
    fwd["event"]          = event;
    fwd["sender_uid"]     = sender_uid;
    fwd["originator_uid"] = originator;  // Non-empty = relayed from a federation peer
    if (payload.contains("payload"))
        fwd["data"] = payload["payload"];

    for (const auto &prod : entry->producers)
    {
        if (prod.zmq_identity.empty()) continue;
        send_to_identity(socket, prod.zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
    }
    LOGGER_DEBUG("Broker: HUB_RELAY_MSG '{}' event='{}' from hub '{}' ->{} local producer(s)",
                 channel, event, originator, entry->producers.size());
}

void BrokerServiceImpl::handle_hub_targeted_msg(const nlohmann::json& payload)
{
    // [BR4] Log a warning so operators know targeted messages are being silently dropped.
    if (!cfg.on_hub_message)
    {
        const std::string ch = payload.value("channel_name", "?");
        LOGGER_WARN("Broker: HUB_TARGETED_MSG for channel '{}' dropped — on_hub_message not configured",
                    ch);
        return;
    }

    const std::string channel    = payload.value("channel_name", "");
    const std::string p_payload  = payload.value("payload",      "");
    const std::string sender_uid = payload.value("sender_uid",   "");
    cfg.on_hub_message(channel, p_payload, sender_uid);
}

void BrokerServiceImpl::relay_notify_to_peers(zmq::socket_t&     socket,
                                               const std::string& channel,
                                               const std::string& event,
                                               const std::string& sender_uid,
                                               const std::string& data)
{
    if (cfg.self_hub_uid.empty()) return;

    // Compute relay targets from HubState's PeerEntry data (state==Connected
    // AND channel ∈ relay_channels).  For a small N of peers this is fine;
    // a precomputed reverse index is documented as a future optimization.
    const auto snap = hub_state_->snapshot();
    std::vector<std::string> targets;
    for (const auto& [uid, peer] : snap.peers)
    {
        if (peer.state != pylabhub::hub::PeerState::Connected) continue;
        if (peer.zmq_identity.empty()) continue;
        if (std::find(peer.relay_channels.begin(), peer.relay_channels.end(), channel)
            == peer.relay_channels.end()) continue;
        targets.push_back(peer.zmq_identity);
    }
    if (targets.empty()) return;

    const std::string msg_id = cfg.self_hub_uid + ":" + std::to_string(relay_seq_++);

    for (const auto& peer_identity : targets)
    {
        nlohmann::json relay;
        relay["relay"]          = true;
        relay["channel_name"]   = channel;
        relay["originator_uid"] = cfg.self_hub_uid;
        relay["msg_id"]         = msg_id;
        relay["event"]          = event;
        relay["sender_uid"]     = sender_uid;
        if (!data.empty()) relay["payload"] = data;

        try
        {
            send_to_identity(socket, peer_identity, "HUB_RELAY_MSG", relay);
            LOGGER_DEBUG("Broker: relayed '{}' event='{}' to peer identity '{}'",
                         channel, event, peer_identity);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: relay to peer '{}' for channel '{}' failed: {}",
                        peer_identity, channel, e.what());
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

nlohmann::json BrokerServiceImpl::handle_band_join_req(
    const nlohmann::json& req,
    const zmq::message_t& identity,
    zmq::socket_t& socket)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.  The C++
    // variable holds the band identifier (`!`-prefixed per §3); name
    // it `band` to match the wire and the HEP — completes the
    // 2026-04-11 rename refactor (`8d3ee1e`) for the wire layer.
    const std::string corr_id   = req.value("correlation_id", "");
    const std::string band      = req.value("band", "");
    const std::string role_uid  = req.value("role_uid", "");
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
                    "`!`-prefixed dotted identifier)", band);
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
    if (auto err = validate_role_uid_only(role_uid,
                                          {"prod", "cons", "proc"},
                                          corr_id,
                                          "BAND_JOIN_REQ"))
    {
        return *err;
    }
    if (!role_name.empty() &&
        !pylabhub::hub::is_valid_identifier(
            role_name, pylabhub::hub::IdentifierKind::RoleName))
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

    const std::string id_str(
        static_cast<const char *>(identity.data()), identity.size());

    // Notify existing members before adding the new one.  Read from HubState
    // snapshot so the strict identifier validation has the final say on
    // which bands/members exist.
    nlohmann::json notify;
    notify["band"]      = band;
    notify["role_uid"]  = role_uid;
    notify["role_name"] = role_name;
    if (auto pre_band = hub_state_->band(band); pre_band.has_value())
    {
        for (const auto& m : pre_band->members)
        {
            if (!m.zmq_identity.empty())
                send_to_identity(socket, m.zmq_identity, "BAND_JOIN_NOTIFY", notify);
        }
    }

    pylabhub::hub::BandMember member;
    member.role_uid     = role_uid;
    member.role_name    = role_name;
    member.zmq_identity = id_str;
    hub_state_->_on_band_joined(band, std::move(member));

    LOGGER_INFO("Broker: BAND_JOIN '{}' role='{}'", band, role_uid);

    nlohmann::json members_json = nlohmann::json::array();
    if (auto post_band = hub_state_->band(band); post_band.has_value())
    {
        for (const auto& m : post_band->members)
        {
            members_json.push_back({
                {"role_uid",  m.role_uid},
                {"role_name", m.role_name}
            });
        }
    }

    nlohmann::json resp;
    resp["status"]  = "success";
    resp["band"]    = band;
    resp["members"] = std::move(members_json);
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_band_leave_req(
    const nlohmann::json& req,
    zmq::socket_t& /*socket*/)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    const std::string corr_id  = req.value("correlation_id", "");
    const std::string band     = req.value("band", "");
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
                    "identifier '{}'", band);
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
    if (auto err = validate_role_uid_only(role_uid,
                                          {"prod", "cons", "proc"},
                                          corr_id,
                                          "BAND_LEAVE_REQ"))
    {
        return *err;
    }

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
        for (const auto& m : pre->members)
        {
            if (m.role_uid == role_uid) { was_member = true; break; }
        }
    }
    if (!was_member)
    {
        LOGGER_WARN("Broker: BAND_LEAVE_REQ from '{}' rejected — not a "
                    "member of band '{}' (HEP-CORE-0030 §5.1 "
                    "membership rule)",
                    role_uid, band);
        return make_error(corr_id, "NOT_A_MEMBER",
                          "Sender '" + role_uid + "' is not a member "
                          "of band '" + band + "'");
    }
    hub_state_->_on_band_left(band, role_uid);
    LOGGER_INFO("Broker: BAND_LEAVE '{}' role='{}'", band, role_uid);

    nlohmann::json resp;
    resp["status"] = "success";
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

void BrokerServiceImpl::handle_band_broadcast_req(
    zmq::socket_t& socket,
    const nlohmann::json& req,
    const zmq::message_t& /*identity*/)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): the sender field
    // was renamed `sender_uid` → `role_uid` for consistency with all
    // other gates.  The sender IS a role, no different identifier
    // shape — uniform naming simplifies role-side code.
    const std::string band_name = req.value("band",     "");
    const std::string role_uid  = req.value("role_uid", "");

    if (band_name.empty()) return;

    // Audit R3.5 (2026-05-17): silent-drop on invalid identifier.
    // BAND_BROADCAST_REQ is fire-and-forget so there is no error
    // response to return; we log + drop.  The caller cannot
    // observe the failure (matches existing fire-and-forget
    // semantics) — operators see the WARN log.
    if (!pylabhub::hub::is_valid_identifier(band_name, pylabhub::hub::IdentifierKind::Band))
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_REQ dropped — invalid band "
                    "identifier '{}' (HEP-CORE-0030 §3)", band_name);
        return;
    }
    // Audit R3.5b (2026-05-19): role_uid grammar + tag check.  Any
    // role may broadcast — accept {prod, cons, proc}.  Drop with
    // WARN log (fire-and-forget).
    if (!pylabhub::hub::is_valid_identifier(
            role_uid, pylabhub::hub::IdentifierKind::RoleUid))
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_REQ dropped on band '{}' — "
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
    // a non-member's BAND_BROADCAST_REQ is dropped + WARN'd.
    // Fire-and-forget so no reply is emitted; operators see the WARN.
    auto band = hub_state_->band(band_name);
    if (!band.has_value())
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_REQ dropped — band '{}' "
                    "does not exist (sender uid='{}')",
                    band_name, role_uid);
        return;
    }
    bool sender_is_member = false;
    for (const auto& m : band->members)
    {
        if (m.role_uid == role_uid) { sender_is_member = true; break; }
    }
    if (!sender_is_member)
    {
        LOGGER_WARN("Broker: BAND_BROADCAST_REQ dropped — sender '{}' "
                    "is not a member of band '{}' (HEP-CORE-0030 §5.2 "
                    "sender-must-be-member rule)",
                    role_uid, band_name);
        return;
    }

    nlohmann::json notify;
    notify["band"]      = band_name;
    notify["role_uid"]  = role_uid;
    notify["body"]      = req.value("body", nlohmann::json::object());

    std::size_t recipients = 0;
    for (const auto& m : band->members)
    {
        if (m.role_uid == role_uid) continue;
        if (m.zmq_identity.empty()) continue;
        send_to_identity(socket, m.zmq_identity, "BAND_BROADCAST_NOTIFY", notify);
        ++recipients;
    }

    LOGGER_DEBUG("Broker: BAND_BROADCAST '{}' from '{}' ->{} recipients",
                 band_name, role_uid, recipients);
}

nlohmann::json BrokerServiceImpl::handle_band_members_req(
    const nlohmann::json& req)
{
    // Wire payload key is `band` per HEP-CORE-0030 §5.1.
    const std::string band_name = req.value("band", "");

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
                    "identifier '{}'", band_name);
        return make_error("", "INVALID_BAND_NAME",
                          "Band identifier failed validation "
                          "(HEP-CORE-0030 §3 grammar)");
    }

    nlohmann::json members_json = nlohmann::json::array();
    if (auto band = hub_state_->band(band_name); band.has_value())
    {
        for (const auto& m : band->members)
        {
            members_json.push_back({
                {"role_uid",  m.role_uid},
                {"role_name", m.role_name}
            });
        }
    }

    nlohmann::json resp;
    resp["band"]    = band_name;
    resp["members"] = std::move(members_json);
    return resp;
}

} // namespace pylabhub::broker
