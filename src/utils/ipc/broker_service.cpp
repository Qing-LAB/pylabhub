#include "utils/broker_service.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_state.hpp"
#include "utils/net_address.hpp"

#include "utils/channel_pattern.hpp"

#include "utils/recovery_api.hpp"
#include "utils/schema_library.hpp"

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
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

// Bring shared pattern helpers into scope (defined in utils/channel_pattern.hpp).
using pylabhub::hub::channel_pattern_to_str;
using pylabhub::hub::channel_pattern_from_str;

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

constexpr std::array<std::string_view, 16> kRequestReplyTypes = {
    "REG_REQ", "DISC_REQ", "DEREG_REQ",
    "CONSUMER_REG_REQ", "CONSUMER_DEREG_REQ",
    "ENDPOINT_UPDATE_REQ", "SCHEMA_REQ",
    "CHANNEL_LIST_REQ", "METRICS_REQ", "SHM_BLOCK_QUERY_REQ",
    "ROLE_PRESENCE_REQ", "ROLE_INFO_REQ",
    "BAND_JOIN_REQ", "BAND_LEAVE_REQ", "BAND_MEMBERS_REQ",
    "HUB_PEER_HELLO",
};

constexpr std::array<std::string_view, 9> kFireAndForgetTypes = {
    "HEARTBEAT_REQ", "METRICS_REPORT_REQ", "CHECKSUM_ERROR_REPORT",
    "CHANNEL_NOTIFY_REQ", "CHANNEL_BROADCAST_REQ", "BAND_BROADCAST_REQ",
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

} // namespace

// ============================================================================
// BrokerServiceImpl — all private state and logic
// ============================================================================

class BrokerServiceImpl
{
public:
    /// [IPC-H2] Zero secret key material before member destructors run.
    ~BrokerServiceImpl()
    {
        if (!server_secret_z85.empty())
            sodium_memzero(server_secret_z85.data(), server_secret_z85.size());
        if (!cfg.server_secret_key.empty())
            sodium_memzero(cfg.server_secret_key.data(), cfg.server_secret_key.size());
    }

    BrokerService::Config cfg;
    std::string           server_public_z85;
    std::string           server_secret_z85;

    /// HEP-CORE-0033 §8 state aggregate.  G2.2.0 introduces this field;
    /// subsequent G2.2.x commits migrate handler mutations from the
    /// private registries above into `hub_state_` via the capability
    /// operations (`_on_channel_registered`, `_on_heartbeat`, ...).
    /// Owned here for now; G2.3+ moves ownership to `HubHost`.
    pylabhub::hub::HubState hub_state_;
    std::atomic<bool>     stop_requested{false};

    /// Guards registry reads/writes from external threads (e.g., list_channels_json_str).
    /// The run() thread holds this lock during post-poll registry operations (not during poll).
    mutable std::mutex    m_query_mu;

    /// Guards close_request_queue_ for thread-safe request_close_channel().
    mutable std::mutex    m_close_req_mu;
    /// Script-requested channel closes; drained in run() post-poll phase under m_query_mu.
    std::deque<std::string> close_request_queue_;

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
    struct ParticipantMetrics
    {
        std::string                                uid;
        uint64_t                                   pid{0};
        std::chrono::steady_clock::time_point      last_report;
        nlohmann::json                             data; ///< {base: {...}, custom: {...}}
    };
    struct ChannelMetrics
    {
        ParticipantMetrics                                          producer;
        std::unordered_map<std::string, ParticipantMetrics>         consumers;
    };
    /// channel_name ->aggregated metrics.  Protected by m_query_mu.
    std::unordered_map<std::string, ChannelMetrics> metrics_store_;

    void update_producer_metrics(const std::string &channel,
                                 const nlohmann::json &metrics, uint64_t pid);
    void update_consumer_metrics(const std::string &channel,
                                 const std::string &uid,
                                 const nlohmann::json &metrics);
    nlohmann::json query_metrics(const std::string &channel) const;

    void handle_metrics_report_req(const nlohmann::json &req);
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
                                           const zmq::message_t& identity);
    nlohmann::json handle_consumer_dereg_req(zmq::socket_t& socket,
                                              const nlohmann::json& req);
    void           handle_heartbeat_req(const nlohmann::json& req);
    nlohmann::json handle_endpoint_update_req(const nlohmann::json& req,
                                               const zmq::message_t& identity);
    nlohmann::json handle_schema_req(const nlohmann::json& req);

    // ── Schema library (HEP-CORE-0016 Phase 3) ──────────────────────────────
    /// Lazily-initialized named schema library. Loaded on first call from cfg.schema_search_dirs
    /// (or SchemaLibrary::default_search_dirs() if empty). nullptr_t pattern: nullopt = not yet
    /// loaded; value = library (may be empty if no schema files found).
    mutable std::optional<pylabhub::schema::SchemaLibrary> schema_lib_;

    /// Returns a reference to the lazily-loaded SchemaLibrary.
    /// Thread-unsafe: caller must hold m_query_mu.
    pylabhub::schema::SchemaLibrary& get_schema_library() noexcept;

    void check_heartbeat_timeouts(zmq::socket_t& socket);
    void check_dead_consumers(zmq::socket_t& socket);
    void check_closing_deadlines(zmq::socket_t& socket);

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
    void band_on_role_closed(zmq::socket_t& socket,
                             const std::string& role_uid);

    void send_closing_notify(zmq::socket_t&                            socket,
                             const std::string&                        channel_name,
                             const pylabhub::hub::ChannelEntry&        entry,
                             const std::string&                        reason);
    void send_force_shutdown(zmq::socket_t&                            socket,
                             const std::string&                        channel_name,
                             const pylabhub::hub::ChannelEntry&        entry);

    void handle_checksum_error_report(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    void handle_channel_notify_req(zmq::socket_t&        socket,
                                   const nlohmann::json& req);

    void handle_channel_broadcast_req(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    nlohmann::json handle_channel_list_req();

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
    // accessed via `hub_state_.counters()`.

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

    // ── Connection policy (Phase 3) ────────────────────────────────────────────
    /// Resolve effective policy for a channel: per-channel override > hub-wide default.
    [[nodiscard]] ConnectionPolicy effective_policy(const std::string& channel_name) const noexcept;

    /// Check whether the given identity is allowed by the effective policy.
    /// Returns a non-empty error JSON if the connection should be rejected; std::nullopt if allowed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<nlohmann::json>
    check_connection_policy(const std::string& channel_name,
                            const std::string& role_name,
                            const std::string& role_uid,
                            const std::string& corr_id,
                            bool               is_consumer) const;
};

// ============================================================================
// BrokerServiceImpl::run() — main event loop
// ============================================================================

void BrokerServiceImpl::run()
{
    // Use the shared process-wide zmq::context_t owned by the ZMQContext
    // lifecycle module. The broker binary (hubshell) already includes
    // GetZMQContextModule() in its LifecycleGuard; so do all BrokerService
    // tests. No per-instance context — matches the pattern used by
    // ZmqQueue, InboxQueue, BrokerRequestComm, AdminShell, and Messenger.
    zmq::context_t &ctx = pylabhub::hub::get_zmq_context();
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.set(zmq::sockopt::linger, 0); // policy: always LINGER=0; see §ZMQ socket policy

    if (cfg.use_curve)
    {
        router.set(zmq::sockopt::curve_server, 1);
        router.set(zmq::sockopt::curve_secretkey, server_secret_z85);
        router.set(zmq::sockopt::curve_publickey, server_public_z85);
    }

    router.bind(cfg.endpoint);
    const std::string bound = router.get(zmq::sockopt::last_endpoint);
    if (cfg.on_ready)
    {
        cfg.on_ready(bound, server_public_z85);
    }
    LOGGER_INFO("Broker: listening on {}", bound);
    if (cfg.use_curve)
    {
        LOGGER_INFO("Broker: server_public_key = {}", server_public_z85);
    }

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

            if (cfg.use_curve && !peer_cfg.pubkey_z85.empty())
            {
                ps->socket.set(zmq::sockopt::curve_serverkey, peer_cfg.pubkey_z85);
                ps->socket.set(zmq::sockopt::curve_publickey, server_public_z85);
                ps->socket.set(zmq::sockopt::curve_secretkey, server_secret_z85);
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

    while (!stop_requested.load(std::memory_order_acquire))
    {
        // --- Poll phase (no mutex: zmq_poll blocks up to kPollTimeout, no registry access) ---
        std::vector<zmq::pollitem_t> items;
        items.reserve(1 + peer_sockets.size());
        items.push_back({router.handle(), 0, ZMQ_POLLIN, 0});
        for (const auto& ps : peer_sockets)
            items.push_back({ps->socket.handle(), 0, ZMQ_POLLIN, 0});

        zmq::poll(items, kPollTimeout);

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
                auto hub_entry = hub_state_.channel(ch);
                if (hub_entry.has_value() &&
                    hub_entry->status != pylabhub::hub::ChannelStatus::Closing)
                {
                    LOGGER_INFO("Broker: script-requested close for channel '{}'", ch);
                    send_closing_notify(router, ch, *hub_entry, "script_requested");
                    // Transition to Closing with a grace deadline — don't deregister yet.
                    // Clients have until the deadline to process queued messages and deregister.
                    const auto deadline =
                        std::chrono::steady_clock::now() + cfg.effective_grace();
                    hub_state_._set_channel_status(
                        ch, pylabhub::hub::ChannelStatus::Closing);
                    hub_state_._set_channel_closing_deadline(ch, deadline);
                }
            }

            // Drain admin-shell-requested broadcasts.
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
                nlohmann::json req;
                req["target_channel"] = br.channel;
                req["sender_uid"]     = "admin_shell";
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
                    auto peer = hub_state_.peer(ht.target_hub_uid);
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
            check_closing_deadlines(router);
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
                            hub_state_._bump_counter("sys.malformed_frame");
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
                        hub_state_._bump_counter("sys.malformed_json");
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
                     ? payload.value("channel_name", payload.value("channel", std::string{}))
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
        hub_state_._bump_counter("sys.unknown_msg_type");
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
        nlohmann::json resp = handle_consumer_reg_req(payload, identity);
        const std::string ack =
            (resp.value("status", "") == "success") ? "CONSUMER_REG_ACK" : "ERROR";
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
        // Fire-and-forget from client. State transitions (PendingReady -> Ready)
        // happen inside hub_state_._on_heartbeat() called from the handler.
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
    else if (msg_type == "CHANNEL_NOTIFY_REQ")
    {
        // Fire-and-forget: relay event to target channel's producer.
        handle_channel_notify_req(socket, payload);
    }
    else if (msg_type == "CHANNEL_BROADCAST_REQ")
    {
        // Fire-and-forget: fan out broadcast to ALL members of a channel.
        handle_channel_broadcast_req(socket, payload);
    }
    else if (msg_type == "CHANNEL_LIST_REQ")
    {
        // Synchronous: return list of registered channels.
        nlohmann::json resp = handle_channel_list_req();
        LOGGER_TRACE("Broker: CHANNEL_LIST_ACK channels={}",
                     resp.value("channels", nlohmann::json::array()).size());
        send_reply(socket, identity, "CHANNEL_LIST_ACK", resp);
    }
    else if (msg_type == "METRICS_REPORT_REQ")
    {
        // HEP-CORE-0019: consumer metrics report (fire-and-forget).
        handle_metrics_report_req(payload);
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
        send_reply(socket, identity, "BAND_MEMBERS_ACK", resp);
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
            } catch (...) {}
        }
        hub_state_._bump_counter("sys.handler_exception");
        emit_processing_error(msg_type, "exception", e.what(), &identity);
    }

    // HEP-CORE-0033 §9.4 wire metric: always bump for known msg_types,
    // regardless of error outcome (counts dispatch-completed messages).
    // bytes_out=0 because multi-target fan-out (broadcast/relay) makes a
    // single per-message accounting ambiguous; per-target byte tracking
    // deferred.
    hub_state_._on_message_processed(msg_type, bytes_in, /*bytes_out=*/0);
    if (errored)
        hub_state_._bump_msg_type_error(msg_type);
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
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const std::string attempted_schema = req.value("schema_hash", "");
    const uint64_t    attempted_pid    = req.value("producer_pid", uint64_t{0});

    // ── Connection policy check (Phase 3) ───────────────────────────────────
    const std::string role_name = req.value("role_name", "");
    const std::string role_uid  = req.value("role_uid", "");
    if (auto err = check_connection_policy(channel_name, role_name, role_uid, corr_id,
                                           /*is_consumer=*/false))
    {
        return *err;
    }

    pylabhub::hub::ChannelEntry entry;
    entry.name                  = channel_name;
    entry.shm_name              = req.value("shm_name", "");
    entry.schema_hash           = attempted_schema;
    entry.schema_version        = req.value("schema_version", uint32_t{0});
    entry.producer_pid          = attempted_pid;
    entry.producer_hostname     = req.value("producer_hostname", "");
    entry.producer_role_name    = role_name;
    entry.producer_role_uid     = role_uid;
    entry.has_shared_memory     = req.value("has_shared_memory", false);
    entry.pattern               = channel_pattern_from_str(req.value("channel_pattern", "PubSub"));
    entry.zmq_ctrl_endpoint     = req.value("zmq_ctrl_endpoint", "");
    entry.zmq_data_endpoint     = req.value("zmq_data_endpoint", "");
    entry.zmq_pubkey            = req.value("zmq_pubkey", "");
    // HEP-CORE-0021: ZMQ endpoint registry (broker records peer endpoint for discovery).
    entry.data_transport        = req.value("data_transport", std::string{"shm"});
    entry.zmq_node_endpoint     = req.value("zmq_node_endpoint", "");
    entry.inbox_endpoint        = req.value("inbox_endpoint", "");
    entry.inbox_schema_json     = req.value("inbox_schema_json", "");
    entry.inbox_packing         = req.value("inbox_packing", "");
    entry.inbox_checksum        = req.value("inbox_checksum", "");

    // HEP-0021 §16: reject registration if inbox_endpoint has unresolved port 0.
    if (!entry.inbox_endpoint.empty())
    {
        auto inbox_ep = pylabhub::validate_tcp_endpoint(entry.inbox_endpoint);
        if (inbox_ep.ok() && inbox_ep.port == 0)
        {
            LOGGER_WARN("Broker: REG_REQ for '{}' rejected — inbox_endpoint '{}' has port 0",
                        channel_name, entry.inbox_endpoint);
            return make_error(corr_id, "INVALID_INBOX_ENDPOINT",
                              "inbox_endpoint '" + entry.inbox_endpoint +
                                  "' has unresolved port 0");
        }
    }

    if (req.contains("metadata") && req["metadata"].is_object())
    {
        entry.metadata = req["metadata"];
    }
    // Producer ZMQ identity: captured here for future unsolicited pushes.
    entry.producer_zmq_identity.assign(static_cast<const char*>(identity.data()),
                                        identity.size());
    // status starts as PendingReady (default); last_heartbeat = now() (default).

    // ── Schema annotation (HEP-CORE-0016 Phase 3) ──────────────────────────
    const std::string req_schema_id   = req.value("schema_id", "");
    const std::string req_schema_blds = req.value("schema_blds", "");
    entry.schema_blds = req_schema_blds;

    if (!req_schema_id.empty())
    {
        // Case A: producer asserts a named schema_id — validate hash against library.
        const auto schema_entry = get_schema_library().get(req_schema_id);
        if (schema_entry)
        {
            const auto producer_hash = hex_to_hash_array(attempted_schema);
            if (producer_hash != schema_entry->slot_info.hash)
            {
                LOGGER_WARN("Broker: schema_id '{}' hash mismatch for channel '{}': "
                            "producer hash does not match schema library",
                            req_schema_id, channel_name);
                return make_error(corr_id, "SCHEMA_ID_MISMATCH",
                                  "Producer schema_id '" + req_schema_id +
                                      "' does not match the BLDS hash in the schema library "
                                      "for channel '" + channel_name + "'");
            }
            entry.schema_id = req_schema_id;
            LOGGER_INFO("Broker: channel '{}' confirmed named schema '{}'",
                        channel_name, req_schema_id);
        }
        else
        {
            // schema_id not in library — accept but log a warning; store the ID as-is
            // (library might not be populated; Case B annotation still applies later).
            entry.schema_id = req_schema_id;
            LOGGER_WARN("Broker: schema_id '{}' not found in schema library for channel '{}'"
                        " — stored as-is (library may be empty or schema files missing)",
                        req_schema_id, channel_name);
        }
    }
    else if (!attempted_schema.empty() && attempted_schema.size() == 64)
    {
        // Case B: anonymous schema — attempt reverse hash lookup to auto-annotate.
        const auto producer_hash = hex_to_hash_array(attempted_schema);
        const auto found_id = get_schema_library().identify(producer_hash);
        if (found_id)
        {
            entry.schema_id = *found_id;
            // Populate BLDS from library if not provided by producer.
            if (entry.schema_blds.empty())
            {
                const auto sch_entry = get_schema_library().get(*found_id);
                if (sch_entry) entry.schema_blds = sch_entry->slot_info.blds;
            }
            LOGGER_INFO("Broker: channel '{}' auto-annotated with schema_id '{}' via hash lookup",
                        channel_name, *found_id);
        }
    }

    // HubState is the sole channel store.  Replicate the historical
    // ChannelRegistry::register_channel gating semantics:
    //   - new channel: insert.
    //   - same schema_hash: re-register, preserving existing consumers.
    //   - different schema_hash: SCHEMA_MISMATCH error + notify existing.
    if (auto existing_opt = hub_state_.channel(channel_name); existing_opt.has_value())
    {
        if (existing_opt->schema_hash != entry.schema_hash)
        {
            // Cat 1: schema mismatch — invariant violation.
            const std::string &existing_schema = existing_opt->schema_hash;
            LOGGER_ERROR(
                "Broker: Cat1 schema mismatch on '{}': existing={} attempted={} attempted_pid={}",
                channel_name, existing_schema, attempted_schema, attempted_pid);
            if (!existing_opt->producer_zmq_identity.empty())
            {
                nlohmann::json err;
                err["channel_name"]          = channel_name;
                err["event"]                 = "schema_mismatch_attempt";
                err["existing_schema_hash"]  = existing_schema;
                err["attempted_schema_hash"] = attempted_schema;
                err["attempted_pid"]         = attempted_pid;
                send_to_identity(socket, existing_opt->producer_zmq_identity,
                                 "CHANNEL_ERROR_NOTIFY", err);
                LOGGER_ERROR("Broker: CHANNEL_ERROR_NOTIFY to producer of '{}': event={}, "
                             "existing_hash={}, attempted_hash={}, attempted_pid={}",
                             channel_name, "schema_mismatch_attempt",
                             existing_schema, attempted_schema, attempted_pid);
            }
            return make_error(corr_id, "SCHEMA_MISMATCH",
                              "Schema hash differs from existing registration for channel '" +
                                  channel_name + "'");
        }
        // Same schema — re-registration (producer restart).  Preserve
        // existing consumers so they are still notified on close.
        entry.consumers = std::move(existing_opt->consumers);
    }

    hub_state_._on_channel_registered(std::move(entry));

    LOGGER_INFO("Broker: registered channel '{}' (pending first heartbeat)", channel_name);
    nlohmann::json resp;
    resp["status"]     = "success";
    resp["channel_id"] = channel_name;
    resp["message"]    = "Producer registered successfully";
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

    auto entry = hub_state_.channel(channel_name);

    // ── HEP-CORE-0023 §2.2: Three-response state-machine dispatch ──────
    // Broker replies immediately based on current role state. No queuing.
    if (!entry.has_value())
    {
        // No role registered for this channel.
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> CHANNEL_NOT_FOUND", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    if (entry->status == pylabhub::hub::ChannelStatus::PendingReady)
    {
        // Role registered but not yet Ready (no heartbeat received).
        // Client is responsible for retry.
        LOGGER_DEBUG("Broker: DISC_REQ for '{}' -> DISC_PENDING (awaiting heartbeat)",
                     channel_name);
        nlohmann::json resp;
        resp["status"]       = "pending";
        resp["channel_name"] = channel_name;
        resp["reason"]       = "awaiting_first_heartbeat";
        if (!corr_id.empty())
            resp["correlation_id"] = corr_id;
        return resp;
    }

    // entry->status == Ready — fall through to normal DISC_ACK payload.

    // HEP-0021 §16: reject if ZMQ endpoint has unresolved port 0.
    if (entry->data_transport == "zmq" && !entry->zmq_node_endpoint.empty())
    {
        auto ep_check = pylabhub::validate_tcp_endpoint(entry->zmq_node_endpoint);
        if (ep_check.ok() && ep_check.port == 0)
        {
            LOGGER_INFO("Broker: DISC_REQ channel '{}' ZMQ endpoint has port 0 (not ready)",
                        channel_name);
            return make_error(corr_id, "CHANNEL_NOT_READY",
                              "ZMQ endpoint for channel '" + channel_name +
                                  "' has unresolved port 0");
        }
    }

    LOGGER_INFO("Broker: discovered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"]            = "success";
    resp["shm_name"]          = entry->shm_name;
    resp["schema_hash"]       = entry->schema_hash;
    resp["schema_version"]    = entry->schema_version;
    resp["metadata"]          = entry->metadata;
    resp["consumer_count"]    =
        static_cast<uint32_t>(entry->consumers.size());
    resp["has_shared_memory"] = entry->has_shared_memory;
    resp["channel_pattern"]   = channel_pattern_to_str(entry->pattern);
    resp["zmq_ctrl_endpoint"]  = entry->zmq_ctrl_endpoint;
    resp["zmq_data_endpoint"]  = entry->zmq_data_endpoint;
    resp["zmq_pubkey"]         = entry->zmq_pubkey;
    // HEP-CORE-0021: ZMQ endpoint registry (echo stored peer endpoint for discovery).
    resp["data_transport"]     = entry->data_transport;
    resp["zmq_node_endpoint"]  = entry->zmq_node_endpoint;
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

    const uint64_t producer_pid = req.value("producer_pid", uint64_t{0});

    // HubState is the sole channel store; replicate the historical
    // ChannelRegistry::deregister_channel NOT_REGISTERED gate (channel
    // exists AND producer_pid matches).
    auto entry = hub_state_.channel(channel_name);
    if (!entry.has_value() || entry->producer_pid != producer_pid)
    {
        LOGGER_WARN("Broker: DEREG_REQ failed for channel '{}' (pid={})", channel_name,
                    producer_pid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name + "' not registered or pid mismatch");
    }

    // Notify consumers BEFORE removing the entry so the consumer list is still intact.
    send_closing_notify(socket, channel_name, *entry, "producer_deregistered");
    on_channel_closed(socket, channel_name, *entry, "producer_deregistered");

    // Producer voluntarily closed the channel — HubState authoritative remove.
    hub_state_._on_channel_closed(channel_name,
                                  pylabhub::hub::ChannelCloseReason::VoluntaryDereg);

    // Remove accumulated metrics so the store doesn't grow unboundedly.
    metrics_store_.erase(channel_name);

    LOGGER_INFO("Broker: deregistered channel '{}'", channel_name);
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
                                                           const zmq::message_t& identity)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const auto channel_entry = hub_state_.channel(channel_name);
    if (!channel_entry.has_value())
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    // HEP-0021 §16: reject if ZMQ endpoint has unresolved port 0.
    if (channel_entry->data_transport == "zmq" && !channel_entry->zmq_node_endpoint.empty())
    {
        auto ep_check = pylabhub::validate_tcp_endpoint(channel_entry->zmq_node_endpoint);
        if (ep_check.ok() && ep_check.port == 0)
        {
            LOGGER_INFO("Broker: CONSUMER_REG_REQ channel '{}' ZMQ endpoint has port 0 (not ready)",
                        channel_name);
            return make_error(corr_id, "CHANNEL_NOT_READY",
                              "ZMQ endpoint for channel '" + channel_name +
                                  "' has unresolved port 0");
        }
    }

    // ── Transport arbitration (Phase 6) ─────────────────────────────────────
    const std::string consumer_queue_type = req.value("consumer_queue_type", "");
    if (!consumer_queue_type.empty() && consumer_queue_type != channel_entry->data_transport)
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ transport mismatch on '{}': "
                    "consumer wants '{}' but channel uses '{}'",
                    channel_name, consumer_queue_type, channel_entry->data_transport);
        return make_error(corr_id, "TRANSPORT_MISMATCH",
                          "Consumer queue_type '" + consumer_queue_type +
                              "' does not match channel transport '" +
                              channel_entry->data_transport + "'");
    }

    // ── Named schema validation (HEP-CORE-0016 Phase 3) ─────────────────────
    const std::string expected_schema_id = req.value("expected_schema_id", "");
    if (!expected_schema_id.empty())
    {
        const bool id_match = (channel_entry && channel_entry->schema_id == expected_schema_id);
        if (!id_match)
        {
            // ID mismatch: try hash comparison against the library.
            bool hash_match = false;
            if (channel_entry)
            {
                const auto named = get_schema_library().get(expected_schema_id);
                if (named)
                {
                    const auto ch_hash = hex_to_hash_array(channel_entry->schema_hash);
                    hash_match = (ch_hash == named->slot_info.hash);
                }
                else
                {
                    LOGGER_WARN("Broker: expected_schema_id '{}' not found in schema library "
                                "for CONSUMER_REG_REQ on channel '{}'",
                                expected_schema_id, channel_name);
                    return make_error(corr_id, "SCHEMA_ID_UNKNOWN",
                                      "Expected schema_id '" + expected_schema_id +
                                          "' not found in schema library");
                }
            }
            if (!hash_match)
            {
                LOGGER_WARN("Broker: CONSUMER_REG_REQ schema_id mismatch on '{}': "
                            "expected='{}' channel_schema_id='{}'",
                            channel_name, expected_schema_id,
                            channel_entry ? channel_entry->schema_id : "(none)");
                return make_error(corr_id, "SCHEMA_ID_MISMATCH",
                                  "Consumer expected schema_id '" + expected_schema_id +
                                      "' does not match channel '" + channel_name + "'");
            }
        }
    }

    // ── Connection policy check (Phase 3) ───────────────────────────────────
    const std::string role_name = req.value("consumer_name", "");
    const std::string role_uid  = req.value("consumer_uid", "");
    if (auto err = check_connection_policy(channel_name, role_name, role_uid, corr_id,
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
    // Capture ZMQ identity for future CHANNEL_CLOSING_NOTIFY.
    entry.zmq_identity.assign(static_cast<const char*>(identity.data()), identity.size());

    hub_state_._on_consumer_joined(channel_name, std::move(entry));

    LOGGER_INFO("Broker: consumer registered for channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"]       = "success";
    resp["channel_name"] = channel_name;
    resp["message"]      = "Consumer registered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
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

    const uint64_t consumer_pid = req.value("consumer_pid", uint64_t{0});

    // Fetch consumer entry BEFORE removal so the cleanup hook can read role_uid.
    pylabhub::hub::ConsumerEntry closing_entry{};
    bool have_entry = false;
    {
        auto ch = hub_state_.channel(channel_name);
        if (ch.has_value())
        {
            for (const auto& c : ch->consumers)
            {
                if (c.consumer_pid == consumer_pid)
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
        LOGGER_WARN("Broker: CONSUMER_DEREG_REQ failed for channel '{}' (pid={})", channel_name,
                    consumer_pid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Consumer pid " + std::to_string(consumer_pid) +
                              " not registered for channel '" + channel_name + "'");
    }

    // Consumer voluntarily left.  `closing_entry.role_uid` may be empty
    // (legacy consumers), in which case HubState's _on_consumer_left
    // silent-drops the role-side cleanup and still erases from the channel.
    hub_state_._on_consumer_left(channel_name, closing_entry.role_uid);
    on_consumer_closed(socket, channel_name, closing_entry, "voluntary_close");

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

void BrokerServiceImpl::handle_heartbeat_req(const nlohmann::json& req)
{
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ missing channel_name");
        return;
    }
    // Peek existence + status before applying the heartbeat so we can log
    // the Pending->Ready transition (the actual mutation + counter bump
    // happens inside hub_state_._on_heartbeat below).
    auto pre = hub_state_.channel(channel_name);
    if (!pre.has_value())
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for unknown channel '{}'", channel_name);
        return;
    }
    const bool        was_pending       = (pre->status == pylabhub::hub::ChannelStatus::PendingReady);
    const std::string producer_role_uid = pre->producer_role_uid;

    if (was_pending)
    {
        // `pending_to_ready_total` is bumped by `_on_heartbeat` itself
        // when the PendingReady -> Ready transition fires.
        LOGGER_INFO("Broker: role '{}' transitioned Pending -> Ready", channel_name);
    }
    LOGGER_DEBUG("Broker: heartbeat for channel '{}'", channel_name);

    if (!req.contains("producer_pid") || req["producer_pid"].get<uint64_t>() == 0)
    {
        LOGGER_ERROR("Broker: HEARTBEAT_REQ for '{}' missing or zero producer_pid",
                     channel_name);
    }

    // `_on_heartbeat` unconditionally refreshes channel.last_heartbeat,
    // transitions PendingReady→Ready when applicable (state_since auto-
    // stamped), updates role.last_heartbeat (role-side liveness), and
    // absorbs piggybacked metrics into role.latest_metrics (HEP-0033 §9.1).
    std::optional<nlohmann::json> metrics_opt;
    if (req.contains("metrics") && req["metrics"].is_object())
        metrics_opt = req["metrics"];
    hub_state_._on_heartbeat(channel_name,
                             producer_role_uid,
                             std::chrono::steady_clock::now(),
                             metrics_opt);

    // HEP-CORE-0019: keep the legacy metrics_store_ in sync until
    // G2.2.4 absorbs metrics.  After that, `_on_heartbeat`'s role-
    // side metrics update is the only source.
    if (req.contains("metrics") && req["metrics"].is_object())
    {
        const uint64_t pid = req.value("producer_pid", uint64_t{0});
        update_producer_metrics(channel_name, req["metrics"], pid);
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

    auto entry = hub_state_.channel(channel_name);
    if (!entry.has_value())
    {
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    // Verify sender is the channel creator.
    const std::string sender_id(static_cast<const char *>(identity.data()), identity.size());
    if (sender_id != entry->producer_zmq_identity)
    {
        LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' rejected — sender is not creator",
                    channel_name);
        return make_error(corr_id, "NOT_CHANNEL_OWNER",
                          "Sender is not the creator of channel '" + channel_name + "'");
    }

    // Only `zmq_node` is mutable post-registration; reject everything else.
    if (endpoint_type == "inbox")
    {
        // Inbox endpoints must be resolved before REG_REQ — runtime update is not
        // supported. If the inbox port is 0, that's a registration bug, not an
        // update scenario.
        auto inbox_check = pylabhub::validate_tcp_endpoint(entry->inbox_endpoint);
        if (inbox_check.ok() && inbox_check.port == 0)
        {
            LOGGER_ERROR("Broker: ENDPOINT_UPDATE_REQ for '{}' inbox — current port is 0; "
                         "inbox endpoint should be resolved before registration",
                         channel_name);
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

    // Check current value: already non-zero ->reject unless same value (idempotent).
    const std::string &current_value = entry->zmq_node_endpoint;
    auto current = pylabhub::validate_tcp_endpoint(current_value);
    if (current.ok() && current.port != 0)
    {
        if (current_value == endpoint)
        {
            LOGGER_DEBUG("Broker: ENDPOINT_UPDATE_REQ for '{}' {} — "
                         "already set to '{}' (idempotent)",
                         channel_name, endpoint_type, endpoint);
        }
        else
        {
            LOGGER_WARN("Broker: ENDPOINT_UPDATE_REQ for '{}' {} rejected — "
                        "already set to '{}', cannot change to '{}'",
                        channel_name, endpoint_type, current_value, endpoint);
            return make_error(corr_id, "ENDPOINT_ALREADY_SET",
                              endpoint_type + " endpoint already set to '" +
                                  current_value + "'");
        }
    }
    else
    {
        LOGGER_INFO("Broker: ENDPOINT_UPDATE_REQ for '{}' {} updated to '{}'",
                    channel_name, endpoint_type, endpoint);
        hub_state_._set_channel_zmq_node_endpoint(channel_name, endpoint);
    }

    nlohmann::json resp;
    resp["status"] = "success";
    if (!corr_id.empty())
        resp["correlation_id"] = corr_id;
    return resp;
}

// ============================================================================
// Schema library + SCHEMA_REQ handler (HEP-CORE-0016 Phase 3)
// ============================================================================

pylabhub::schema::SchemaLibrary& BrokerServiceImpl::get_schema_library() noexcept
{
    if (!schema_lib_.has_value())
    {
        const auto dirs = cfg.schema_search_dirs.empty()
            ? pylabhub::schema::SchemaLibrary::default_search_dirs()
            : cfg.schema_search_dirs;
        schema_lib_.emplace(dirs);
        const size_t loaded = schema_lib_->load_all();
        LOGGER_INFO("Broker: SchemaLibrary loaded {} schema(s) from {} dir(s)",
                    loaded, dirs.size());
    }
    return *schema_lib_;
}

nlohmann::json BrokerServiceImpl::handle_schema_req(const nlohmann::json& req)
{
    const std::string corr_id      = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing 'channel_name'");
    }
    const auto entry = hub_state_.channel(channel_name);
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
    resp["blds"]         = entry->schema_blds;
    resp["schema_hash"]  = entry->schema_hash;
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ============================================================================
// Connection policy helpers (Phase 3)
// ============================================================================

ConnectionPolicy BrokerServiceImpl::effective_policy(const std::string& channel_name) const noexcept
{
    for (const auto& cp : cfg.channel_policies)
    {
        if (channel_name_matches_glob(channel_name, cp.channel_glob))
        {
            return cp.policy;
        }
    }
    return cfg.connection_policy;
}

std::optional<nlohmann::json>
BrokerServiceImpl::check_connection_policy(const std::string& channel_name,
                                            const std::string& role_name,
                                            const std::string& role_uid,
                                            const std::string& corr_id,
                                            bool               is_consumer) const
{
    const ConnectionPolicy policy  = effective_policy(channel_name);
    const std::string      role_str = is_consumer ? "consumer" : "producer";

    if (policy == ConnectionPolicy::Required || policy == ConnectionPolicy::Verified)
    {
        if (role_name.empty() || role_uid.empty())
        {
            LOGGER_WARN("Broker: policy={} rejected {} for '{}': missing role_name/uid",
                        connection_policy_to_str(policy), role_str, channel_name);
            return make_error(corr_id, "IDENTITY_REQUIRED",
                              fmt::format("Connection policy '{}' requires role_name and role_uid",
                                          connection_policy_to_str(policy)));
        }
    }

    if (policy == ConnectionPolicy::Verified)
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

    if (policy != ConnectionPolicy::Open && (!role_name.empty() || !role_uid.empty()))
    {
        LOGGER_INFO("Broker: {} identity recorded for '{}': name='{}' uid='{}'",
                    role_str, channel_name, role_name, role_uid);
    }

    return std::nullopt;
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

    const auto snap = hub_state_.snapshot();
    const auto now  = std::chrono::steady_clock::now();

    // ── Pass 1: Ready -> Pending demotion (HEP-CORE-0023 §2.1) ───────────
    // Single capability op: HubState atomically transitions + bumps
    // `ready_to_pending_total`.  Role state stays Connected — Pending is
    // "suspicious, may recover via the next heartbeat", not "gone".
    for (const auto& [channel_name, entry] : snap.channels)
    {
        if (entry.status != pylabhub::hub::ChannelStatus::Ready)
            continue;
        if (now - entry.last_heartbeat < ready_timeout)
            continue;
        LOGGER_WARN("Broker: role '{}' demoted Ready -> Pending "
                    "(no heartbeat within {} ms)",
                    channel_name, ready_timeout.count());
        hub_state_._on_heartbeat_timeout(channel_name, entry.producer_role_uid);
    }

    // ── Pass 2: Pending -> deregistered (HEP-CORE-0023 §2.1, no grace) ──
    // Re-snapshot to observe pass 1's transitions: channels demoted Ready
    // -> Pending in this same call appear here with their freshly-stamped
    // state_since (≈ now), so `now - state_since < pending_timeout` skips
    // them — they get one full pending_timeout window before reclaim.
    const auto snap2 = hub_state_.snapshot();
    for (const auto& [channel_name, entry] : snap2.channels)
    {
        if (entry.status != pylabhub::hub::ChannelStatus::PendingReady)
            continue;
        if (now - entry.state_since < pending_timeout)
            continue;
        LOGGER_WARN("Broker: role '{}' reclaimed from Pending "
                    "(no heartbeat within {} ms); sending CHANNEL_CLOSING_NOTIFY + dereg",
                    channel_name, pending_timeout.count());
        // Transport sends + federation/band cleanup happen first, while the
        // entry is still in HubState; `_on_pending_timeout` then erases the
        // entry and bumps `pending_to_deregistered_total`.
        send_closing_notify(socket, channel_name, entry, "pending_timeout");
        on_channel_closed(socket, channel_name, entry, "pending_timeout");
        hub_state_._on_pending_timeout(channel_name);
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

    const auto snap = hub_state_.snapshot();
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
            // Cat 2: dead consumer — notify producer + clean state.
            LOGGER_WARN(
                "Broker: Cat2 dead consumer pid={} host='{}' on channel '{}' — removing",
                dead_consumer.consumer_pid, dead_consumer.consumer_hostname, channel_name);
            if (!entry.producer_zmq_identity.empty())
            {
                nlohmann::json notify;
                notify["channel_name"]      = channel_name;
                notify["consumer_pid"]      = dead_consumer.consumer_pid;
                notify["consumer_hostname"] = dead_consumer.consumer_hostname;
                notify["reason"]            = "process_dead";
                send_to_identity(socket, entry.producer_zmq_identity, "CONSUMER_DIED_NOTIFY",
                                 notify);
                LOGGER_INFO("Broker: CONSUMER_DIED_NOTIFY to producer of '{}': "
                            "consumer_pid={}, reason=process_dead",
                            channel_name, dead_consumer.consumer_pid);
            }
            hub_state_._on_consumer_left(channel_name, dead_consumer.role_uid);
            on_consumer_closed(socket, channel_name, dead_consumer, "process_dead");
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

    // Also notify the producer (new: broker now stores producer_zmq_identity).
    if (!entry.producer_zmq_identity.empty())
    {
        try
        {
            send_to_identity(socket, entry.producer_zmq_identity, "CHANNEL_CLOSING_NOTIFY",
                             body);
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' ->producer pid={}",
                        channel_name, entry.producer_pid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to notify producer for '{}': {}", channel_name,
                        e.what());
        }
    }
}

// ============================================================================
// check_closing_deadlines — escalate Closing channels past grace period
// ============================================================================

void BrokerServiceImpl::check_closing_deadlines(zmq::socket_t& socket)
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_remove;

    const auto snap = hub_state_.snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        if (entry.status != pylabhub::hub::ChannelStatus::Closing)
            continue;

        // Grace period not yet expired.
        // Note: if all consumers deregister AND the producer sends DEREG_REQ,
        // _on_channel_closed() erases the entry from HubState — so no
        // explicit early-cleanup check is needed here. We only act on deadline.
        if (now < entry.closing_deadline)
            continue;

        // Deadline elapsed — escalate to FORCE_SHUTDOWN for remaining members.
        LOGGER_WARN("Broker: channel '{}' — grace period expired with {} consumers "
                    "still registered, sending FORCE_SHUTDOWN",
                    name, entry.consumers.size());
        send_force_shutdown(socket, name, entry);
        to_remove.push_back(name);
    }

    for (const auto& name : to_remove)
    {
        auto entry = hub_state_.channel(name);
        if (entry.has_value())
        {
            on_channel_closed(socket, name, *entry, "grace_expired");
            // HubState authoritative grace-expired close.  Reason =
            // HeartbeatTimeout (this path only triggers for channels
            // already in Closing from a heartbeat timeout).
            hub_state_._on_channel_closed(
                name, pylabhub::hub::ChannelCloseReason::HeartbeatTimeout);
        }
    }
}

// ============================================================================
// Role-close cleanup API (HEP-CORE-0023 §2.5)
// ============================================================================

void BrokerServiceImpl::on_channel_closed(zmq::socket_t&                     socket,
                                           const std::string&                 channel_name,
                                           const pylabhub::hub::ChannelEntry& entry,
                                           const std::string&                 reason)
{
    // Keep this list small and explicit. When a new broker module needs to
    // react to role death, add one line here and implement the helper below.
    federation_on_channel_closed(channel_name, entry, reason);
    band_on_role_closed(socket, entry.producer_role_uid);
}

void BrokerServiceImpl::on_consumer_closed(zmq::socket_t&                      socket,
                                            const std::string&                  /*channel_name*/,
                                            const pylabhub::hub::ConsumerEntry& consumer,
                                            const std::string&                  /*reason*/)
{
    // Consumer's role may have joined bands independently of its channel
    // participation — remove from all band memberships.
    band_on_role_closed(socket, consumer.role_uid);
}

void BrokerServiceImpl::federation_on_channel_closed(
    const std::string&                          /*channel_name*/,
    const pylabhub::hub::ChannelEntry&          /*entry*/,
    const std::string&                          /*reason*/)
{
    // No broker-internal index to maintain (relay targets are computed
    // on-the-fly from `hub_state_.snapshot().peers`).  Stale channel-name
    // entries in a peer's `relay_channels` are benign: relay_notify_to_peers
    // only fires when a NOTIFY arrives for a live channel name; if the
    // channel is gone, no NOTIFY arrives.
}

void BrokerServiceImpl::band_on_role_closed(zmq::socket_t& socket,
                                             const std::string& role_uid)
{
    if (role_uid.empty())
        return;  // Anonymous role — never joined any band under a uid.
    // Snapshot bands; identify those containing this role; for each, fire
    // _on_band_left and notify the remaining members (HEP-CORE-0030: band
    // fan-out correctness requires up-to-date membership).
    const auto snap = hub_state_.snapshot();
    std::vector<std::string> affected_bands;
    for (const auto& [band_name, band_entry] : snap.bands)
    {
        const bool is_member = std::any_of(
            band_entry.members.begin(), band_entry.members.end(),
            [&](const pylabhub::hub::BandMember& m) { return m.role_uid == role_uid; });
        if (is_member) affected_bands.push_back(band_name);
    }
    for (const auto& band_name : affected_bands)
    {
        LOGGER_INFO("Broker: role '{}' removed from band '{}' (role closed)",
                    role_uid, band_name);
        hub_state_._on_band_left(band_name, role_uid);
        // Notify remaining members from a fresh snapshot of this band.
        auto remaining = hub_state_.band(band_name);
        if (!remaining.has_value()) continue; // band evicted (was last member)
        nlohmann::json notify;
        notify["channel"]  = band_name;
        notify["role_uid"] = role_uid;
        notify["reason"]   = "role_closed";
        for (const auto& m : remaining->members)
        {
            if (!m.zmq_identity.empty())
                send_to_identity(socket, m.zmq_identity, "BAND_LEAVE_NOTIFY", notify);
        }
    }
}

// ============================================================================
// send_force_shutdown — bypass client message queue, immediate shutdown
// ============================================================================

void BrokerServiceImpl::send_force_shutdown(zmq::socket_t&                     socket,
                                             const std::string&                 channel_name,
                                             const pylabhub::hub::ChannelEntry& entry)
{
    nlohmann::json body;
    body["channel_name"] = channel_name;
    body["reason"]       = "grace_period_expired";

    for (const auto& consumer : entry.consumers)
    {
        if (consumer.zmq_identity.empty())
            continue;
        try
        {
            send_to_identity(socket, consumer.zmq_identity, "FORCE_SHUTDOWN", body);
            LOGGER_INFO("Broker: FORCE_SHUTDOWN for '{}' ->consumer pid={}",
                        channel_name, consumer.consumer_pid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to send FORCE_SHUTDOWN to consumer pid={}: {}",
                        consumer.consumer_pid, e.what());
        }
    }

    if (!entry.producer_zmq_identity.empty())
    {
        try
        {
            send_to_identity(socket, entry.producer_zmq_identity, "FORCE_SHUTDOWN", body);
            LOGGER_INFO("Broker: FORCE_SHUTDOWN for '{}' ->producer pid={}",
                        channel_name, entry.producer_pid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to send FORCE_SHUTDOWN to producer: {}",
                        e.what());
        }
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
        auto entry = hub_state_.channel(channel);
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
            if (!entry->producer_zmq_identity.empty())
            {
                send_to_identity(socket, entry->producer_zmq_identity, "CHANNEL_EVENT_NOTIFY",
                                 fwd);
            }
            LOGGER_INFO("Broker: CHANNEL_EVENT_NOTIFY ->all members of '{}': "
                        "checksum_error slot={}, action=notify_only",
                        channel, slot);
        }
    }
    // ChecksumRepairPolicy::Repair — deferred; requires WriteAttach slot repair path.
}

// ============================================================================
// CHANNEL_NOTIFY_REQ — relay event to target channel's producer
// ============================================================================

void BrokerServiceImpl::handle_channel_notify_req(zmq::socket_t&        socket,
                                                   const nlohmann::json& req)
{
    const auto target_channel = req.value("target_channel", std::string{});
    const auto sender_uid     = req.value("sender_uid",     std::string{});
    const auto event          = req.value("event",          std::string{});

    if (target_channel.empty() || event.empty())
    {
        LOGGER_WARN("Broker: CHANNEL_NOTIFY_REQ with empty target_channel or event");
        return;
    }

    auto entry = hub_state_.channel(target_channel);
    if (!entry || entry->producer_zmq_identity.empty())
    {
        LOGGER_DEBUG("Broker: CHANNEL_NOTIFY_REQ for '{}' — channel not found or no producer",
                     target_channel);
        return;
    }

    // Forward as CHANNEL_EVENT_NOTIFY to the producer.
    // [BR3] Include originator_uid="" (empty = local origin) so the script can detect
    // whether an event was originated locally or relayed from a federation peer, allowing
    // it to avoid re-notifying in response to a relayed event (application-layer loop guard).
    nlohmann::json fwd;
    fwd["channel_name"]   = target_channel;
    fwd["event"]          = event;
    fwd["sender_uid"]     = sender_uid;
    fwd["originator_uid"] = "";   // Empty = originated on this hub
    if (req.contains("data") && req["data"].is_string())
        fwd["data"] = req["data"];

    send_to_identity(socket, entry->producer_zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
    LOGGER_DEBUG("Broker: relayed CHANNEL_NOTIFY_REQ to producer of '{}' event='{}'",
                 target_channel, event);

    // HEP-CORE-0022: relay to federation peers subscribed to this channel.
    const std::string data_str = req.contains("data") && req["data"].is_string()
                                     ? req["data"].get<std::string>() : std::string{};
    relay_notify_to_peers(socket, target_channel, event, sender_uid, data_str);
}

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

    auto entry = hub_state_.channel(target_channel);
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

    // Also send to the producer.
    if (!entry->producer_zmq_identity.empty())
    {
        try
        {
            send_to_identity(socket, entry->producer_zmq_identity,
                             "CHANNEL_BROADCAST_NOTIFY", fwd);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: broadcast to producer for '{}' failed: {}",
                        target_channel, e.what());
        }
    }

    LOGGER_DEBUG("Broker: CHANNEL_BROADCAST_REQ '{}' msg='{}' ->{} consumers + producer",
                 target_channel, message, entry->consumers.size());

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

nlohmann::json BrokerServiceImpl::handle_channel_list_req()
{
    nlohmann::json resp;
    resp["status"] = "success";

    nlohmann::json channels = nlohmann::json::array();
    const auto snap = hub_state_.snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        nlohmann::json ch;
        ch["name"]           = name;
        ch["producer_uid"]   = entry.producer_role_uid;
        ch["schema_id"]      = entry.schema_id;
        ch["consumer_count"] = entry.consumers.size();
        switch (entry.status)
        {
            case pylabhub::hub::ChannelStatus::PendingReady: ch["status"] = "PendingReady"; break;
            case pylabhub::hub::ChannelStatus::Ready:        ch["status"] = "Ready";        break;
            case pylabhub::hub::ChannelStatus::Closing:      ch["status"] = "Closing";      break;
        }
        channels.push_back(std::move(ch));
    }
    resp["channels"] = std::move(channels);
    return resp;
}

// ============================================================================
// ROLE_PRESENCE_REQ / ROLE_INFO_REQ (Phase 4)
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_role_presence_req(const nlohmann::json& req)
{
    const std::string uid = req.value("uid", "");
    if (uid.empty())
    {
        nlohmann::json resp;
        resp["present"] = false;
        resp["error"]   = "missing uid";
        return resp;
    }

    // Scan all channels: check producer_role_uid and each consumer's role_uid.
    const auto snap = hub_state_.snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        if (!entry.producer_role_uid.empty() && entry.producer_role_uid == uid)
        {
            nlohmann::json resp;
            resp["present"] = true;
            resp["channel"] = name;
            resp["role"]    = "producer";
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
                LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' found as consumer on '{}'",
                             uid, name);
                return resp;
            }
        }
    }

    LOGGER_DEBUG("Broker: ROLE_PRESENCE_REQ uid='{}' not found", uid);
    nlohmann::json resp;
    resp["present"] = false;
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_role_info_req(const nlohmann::json& req)
{
    const std::string uid = req.value("uid", "");
    if (uid.empty())
    {
        nlohmann::json resp;
        resp["found"] = false;
        resp["error"] = "missing uid";
        return resp;
    }

    // Search for a channel whose producer_role_uid matches.
    const auto snap = hub_state_.snapshot();
    for (const auto& [name, entry] : snap.channels)
    {
        if (!entry.producer_role_uid.empty() && entry.producer_role_uid == uid)
        {
            nlohmann::json resp;
            resp["found"]           = !entry.inbox_endpoint.empty();
            resp["channel"]         = name;
            resp["inbox_endpoint"]  = entry.inbox_endpoint;
            resp["inbox_packing"]   = entry.inbox_packing;
            resp["inbox_checksum"]  = entry.inbox_checksum;
            if (!entry.inbox_schema_json.empty())
            {
                try
                {
                    resp["inbox_schema"] = nlohmann::json::parse(entry.inbox_schema_json);
                }
                catch (const nlohmann::json::exception &)
                {
                    resp["inbox_schema"] = nlohmann::json::array();
                }
            }
            else
            {
                resp["inbox_schema"] = nlohmann::json::array();
            }
            LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found on '{}', inbox='{}'",
                         uid, name, entry.inbox_endpoint);
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
                    catch (const nlohmann::json::exception &)
                    {
                        resp["inbox_schema"] = nlohmann::json::array();
                    }
                }
                else
                {
                    resp["inbox_schema"] = nlohmann::json::array();
                }
                LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' found as consumer on '{}', inbox='{}'",
                             uid, name, cons.inbox_endpoint);
                return resp;
            }
        }
    }

    LOGGER_DEBUG("Broker: ROLE_INFO_REQ uid='{}' not found", uid);
    nlohmann::json resp;
    resp["found"] = false;
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

BrokerService::BrokerService(Config cfg) : pImpl(std::make_unique<BrokerServiceImpl>())
{
    pImpl->cfg = std::move(cfg);
    if (pImpl->cfg.use_curve)
    {
        if (!pImpl->cfg.server_secret_key.empty() && !pImpl->cfg.server_public_key.empty())
        {
            // Stable keypair supplied from HubVault — use as-is.
            pImpl->server_secret_z85 = pImpl->cfg.server_secret_key;
            pImpl->server_public_z85 = pImpl->cfg.server_public_key;
        }
        else
        {
            // Generate ephemeral keypair (--dev mode; no vault).
            std::array<char, kZ85KeyBufSize> pub{};
            std::array<char, kZ85KeyBufSize> sec{};
            if (zmq_curve_keypair(pub.data(), sec.data()) != 0)
            {
                throw std::runtime_error("BrokerService: zmq_curve_keypair failed");
            }
            pImpl->server_public_z85.assign(pub.data(), kZ85KeyLen);
            pImpl->server_secret_z85.assign(sec.data(), kZ85KeyLen);
        }
    }
}

BrokerService::~BrokerService() = default;

const std::string& BrokerService::server_public_key() const
{
    return pImpl->server_public_z85;
}

const pylabhub::hub::HubState& BrokerService::hub_state() const
{
    return pImpl->hub_state_;
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
    const auto snap = pImpl->hub_state_.snapshot();
    nlohmann::json result = nlohmann::json::array();
    for (const auto &[name, entry] : snap.channels)
    {
        result.push_back(nlohmann::json{
            {"name",           name},
            {"schema_hash",    entry.schema_hash},
            {"consumer_count", static_cast<int>(entry.consumers.size())},
            {"producer_pid",   entry.producer_pid},
            {"status",         pylabhub::hub::to_string(entry.status)}
        });
    }
    return result.dump();
}

ChannelSnapshot BrokerService::query_channel_snapshot() const
{
    const auto hub_snap = pImpl->hub_state_.snapshot();
    ChannelSnapshot snap;
    snap.channels.reserve(hub_snap.channels.size());
    for (const auto &[name, entry] : hub_snap.channels)
    {
        ChannelSnapshotEntry e;
        e.name               = name;
        e.status             = pylabhub::hub::to_string(entry.status);
        e.consumer_count     = static_cast<int>(entry.consumers.size());
        e.producer_pid       = entry.producer_pid;
        e.schema_hash        = entry.schema_hash;
        e.producer_role_name = entry.producer_role_name;
        e.producer_role_uid  = entry.producer_role_uid;
        snap.channels.push_back(std::move(e));
    }
    return snap;
}

RoleStateMetrics BrokerService::query_role_state_metrics() const
{
    // Single-source-of-truth via HubState (HEP-CORE-0033 §8).  HubState
    // takes its own internal lock; m_query_mu not needed here.
    const auto c = pImpl->hub_state_.counters();
    return RoleStateMetrics{
        c.ready_to_pending_total,
        c.pending_to_deregistered_total,
        c.pending_to_ready_total,
    };
}

std::string BrokerService::query_metrics_json_str(const std::string& channel) const
{
    std::lock_guard<std::mutex> lock(pImpl->m_query_mu);
    return pImpl->query_metrics(channel).dump();
}

nlohmann::json BrokerServiceImpl::handle_shm_block_query(const nlohmann::json& req) const
{
    return collect_shm_info(req.value("channel", ""));
}

nlohmann::json BrokerServiceImpl::collect_shm_info(const std::string& channel) const
{
    // Snapshot under HubState's own lock (inside snapshot()), then read SHM
    // outside any broker locks.
    struct BlockInfo
    {
        std::string                                channel;
        std::string                                shm_name;
        uint64_t                                   producer_pid{0};
        std::string                                producer_uid;
        std::string                                producer_name;
        std::vector<pylabhub::hub::ConsumerEntry>  consumers;
    };

    std::vector<BlockInfo> blocks;
    {
        const auto snap = hub_state_.snapshot();
        for (const auto &[name, entry] : snap.channels)
        {
            if (!channel.empty() && name != channel)
                continue;
            if (!entry.has_shared_memory || entry.shm_name.empty())
                continue;
            BlockInfo bi;
            bi.channel       = name;
            bi.shm_name      = entry.shm_name;
            bi.producer_pid  = entry.producer_pid;
            bi.producer_uid  = entry.producer_role_uid;
            bi.producer_name = entry.producer_role_name;
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
        blk["shm_name"] = bi.shm_name;

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
        if (::datablock_get_metrics(bi.shm_name.c_str(), &m) == 0)
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

// ============================================================================
// MetricsStore (HEP-CORE-0019)
// ============================================================================

void BrokerServiceImpl::update_producer_metrics(const std::string &channel,
                                                 const nlohmann::json &metrics,
                                                 uint64_t pid)
{
    // Caller holds m_query_mu (called from process_message under lock).
    auto &cm = metrics_store_[channel];
    cm.producer.pid         = pid;
    cm.producer.last_report = std::chrono::steady_clock::now();
    cm.producer.data        = metrics;
    LOGGER_DEBUG("Broker: stored producer metrics for channel '{}'", channel);
}

void BrokerServiceImpl::update_consumer_metrics(const std::string &channel,
                                                 const std::string &uid,
                                                 const nlohmann::json &metrics)
{
    auto &cm = metrics_store_[channel];
    auto &cons = cm.consumers[uid];
    cons.uid         = uid;
    cons.last_report = std::chrono::steady_clock::now();
    cons.data        = metrics;
    LOGGER_DEBUG("Broker: stored consumer metrics for channel '{}' uid='{}'", channel, uid);
}

void BrokerServiceImpl::handle_metrics_report_req(const nlohmann::json &req)
{
    const std::string channel = req.value("channel_name", "");
    const std::string uid     = req.value("uid", "");
    if (channel.empty() || uid.empty())
    {
        LOGGER_WARN("Broker: METRICS_REPORT_REQ missing channel_name or uid");
        return;
    }
    if (req.contains("metrics") && req["metrics"].is_object())
    {
        update_consumer_metrics(channel, uid, req["metrics"]);
    }
}

nlohmann::json BrokerServiceImpl::handle_metrics_req(const nlohmann::json &req)
{
    const std::string channel = req.value("channel_name", "");
    nlohmann::json resp       = query_metrics(channel);
    // HEP-CORE-0019 §3.2: merge live SHM-derived block metrics into the response.
    // When a channel name is given we can look up the SHM block(s) directly.
    // For the all-channels case the shm_info map is also populated.
    resp["shm_blocks"] = collect_shm_info(channel);
    return resp;
}

nlohmann::json BrokerServiceImpl::query_metrics(const std::string &channel) const
{
    nlohmann::json result;
    result["status"] = "success";

    auto build_channel_metrics = [](const ChannelMetrics &cm) -> nlohmann::json
    {
        nlohmann::json ch;
        if (!cm.producer.data.is_null())
        {
            ch["producer"] = cm.producer.data;
            ch["producer"]["pid"] = cm.producer.pid;
        }
        nlohmann::json consumers = nlohmann::json::object();
        for (const auto &[uid, pm] : cm.consumers)
        {
            if (!pm.data.is_null())
                consumers[uid] = pm.data;
        }
        if (!consumers.empty())
            ch["consumers"] = std::move(consumers);
        return ch;
    };

    if (!channel.empty())
    {
        // Single channel query.
        auto it = metrics_store_.find(channel);
        if (it != metrics_store_.end())
        {
            result["channel"]      = channel;
            result["metrics"]      = build_channel_metrics(it->second);
        }
        else
        {
            result["channel"] = channel;
            result["metrics"] = nlohmann::json::object();
        }
    }
    else
    {
        // All channels.
        nlohmann::json channels = nlohmann::json::object();
        for (const auto &[name, cm] : metrics_store_)
        {
            channels[name] = build_channel_metrics(cm);
        }
        result["channels"] = std::move(channels);
    }
    return result;
}

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
    if (auto pre = hub_state_.peer(peer_hub_uid);
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
    hub_state_._on_peer_connected(std::move(pe));

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

    auto pre = hub_state_.peer(peer_hub_uid);
    if (!pre.has_value() || pre->state != pylabhub::hub::PeerState::Connected) return;

    // HEP-CORE-0033 §8 retention: peer entry stays with state=Disconnected;
    // observable via snapshot until grace eviction (deferred work).
    hub_state_._on_peer_disconnected(peer_hub_uid);
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

    // Deliver locally as CHANNEL_EVENT_NOTIFY (to channel producer only, like CHANNEL_NOTIFY_REQ).
    // Include relayed_from so scripts can distinguish relayed events.
    auto entry = hub_state_.channel(channel);
    if (!entry || entry->producer_zmq_identity.empty())
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

    send_to_identity(socket, entry->producer_zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
    LOGGER_DEBUG("Broker: HUB_RELAY_MSG '{}' event='{}' from hub '{}' ->local producer",
                 channel, event, originator);
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
    const auto snap = hub_state_.snapshot();
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
    const std::string channel   = req.value("channel", "");
    const std::string role_uid  = req.value("role_uid", "");
    const std::string role_name = req.value("role_name", "");

    if (channel.empty() || role_uid.empty())
    {
        return make_error("", "INVALID_REQUEST", "Missing channel or role_uid");
    }

    const std::string id_str(
        static_cast<const char *>(identity.data()), identity.size());

    // Notify existing members before adding the new one.  Read from HubState
    // snapshot so the strict identifier validation has the final say on
    // which bands/members exist.
    nlohmann::json notify;
    notify["channel"]   = channel;
    notify["role_uid"]  = role_uid;
    notify["role_name"] = role_name;
    if (auto pre_band = hub_state_.band(channel); pre_band.has_value())
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
    hub_state_._on_band_joined(channel, std::move(member));

    LOGGER_INFO("Broker: BAND_JOIN '{}' role='{}'", channel, role_uid);

    nlohmann::json members_json = nlohmann::json::array();
    if (auto post_band = hub_state_.band(channel); post_band.has_value())
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
    resp["channel"] = channel;
    resp["members"] = std::move(members_json);
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_band_leave_req(
    const nlohmann::json& req,
    zmq::socket_t& socket)
{
    const std::string channel  = req.value("channel", "");
    const std::string role_uid = req.value("role_uid", "");

    if (channel.empty() || role_uid.empty())
    {
        return make_error("", "INVALID_REQUEST", "Missing channel or role_uid");
    }

    // Was the role actually a member?  Check against HubState snapshot so
    // the BAND_LEAVE_NOTIFY only fires on a real removal.
    bool was_member = false;
    if (auto pre = hub_state_.band(channel); pre.has_value())
    {
        for (const auto& m : pre->members)
        {
            if (m.role_uid == role_uid) { was_member = true; break; }
        }
    }

    hub_state_._on_band_left(channel, role_uid);

    if (was_member)
    {
        LOGGER_INFO("Broker: BAND_LEAVE '{}' role='{}'", channel, role_uid);

        // Notify remaining members (if any — leaving the last member also
        // erases the band from HubState; the snapshot is then empty).
        nlohmann::json notify;
        notify["channel"]  = channel;
        notify["role_uid"] = role_uid;
        notify["reason"]   = "voluntary";
        if (auto post = hub_state_.band(channel); post.has_value())
        {
            for (const auto& m : post->members)
            {
                if (!m.zmq_identity.empty())
                    send_to_identity(socket, m.zmq_identity, "BAND_LEAVE_NOTIFY", notify);
            }
        }
    }

    nlohmann::json resp;
    resp["status"] = "success";
    return resp;
}

void BrokerServiceImpl::handle_band_broadcast_req(
    zmq::socket_t& socket,
    const nlohmann::json& req,
    const zmq::message_t& /*identity*/)
{
    const std::string channel    = req.value("channel", "");
    const std::string sender_uid = req.value("sender_uid", "");

    if (channel.empty()) return;

    nlohmann::json notify;
    notify["channel"]    = channel;
    notify["sender_uid"] = sender_uid;
    notify["body"]       = req.value("body", nlohmann::json::object());

    std::size_t recipients = 0;
    if (auto band = hub_state_.band(channel); band.has_value())
    {
        for (const auto& m : band->members)
        {
            if (m.role_uid == sender_uid) continue;
            if (m.zmq_identity.empty()) continue;
            send_to_identity(socket, m.zmq_identity, "BAND_BROADCAST_NOTIFY", notify);
            ++recipients;
        }
    }

    LOGGER_DEBUG("Broker: BAND_BROADCAST '{}' from '{}' ->{} recipients",
                 channel, sender_uid, recipients);
}

nlohmann::json BrokerServiceImpl::handle_band_members_req(
    const nlohmann::json& req)
{
    const std::string channel = req.value("channel", "");

    nlohmann::json members_json = nlohmann::json::array();
    if (auto band = hub_state_.band(channel); band.has_value())
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
    resp["channel"] = channel;
    resp["members"] = std::move(members_json);
    return resp;
}

} // namespace pylabhub::broker
