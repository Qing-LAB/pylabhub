#include "utils/broker_service.hpp"

#include "channel_registry.hpp"

#include "utils/schema_library.hpp"

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>
#include <zmq.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
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

// Bring shared pattern helpers into scope (defined in utils/channel_pattern.hpp,
// included via channel_registry.hpp → utils/channel_pattern.hpp).
using pylabhub::hub::channel_pattern_to_str;
using pylabhub::hub::channel_pattern_from_str;

/// Convert a 64-char hex-encoded schema hash string → std::array<uint8_t, 32>.
/// Returns a zero-filled array on format error (wrong length or invalid hex).
std::array<uint8_t, 32> hex_to_hash_array(const std::string& hex) noexcept
{
    std::array<uint8_t, 32> result{};
    if (hex.size() != 64) return result;
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i)
    {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return {};
        result[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
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
} // namespace

// ============================================================================
// BrokerServiceImpl — all private state and logic
// ============================================================================

class BrokerServiceImpl
{
public:
    BrokerService::Config cfg;
    std::string           server_public_z85;
    std::string           server_secret_z85;
    ChannelRegistry       registry;
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
    /// channel_name → aggregated metrics.  Protected by m_query_mu.
    std::unordered_map<std::string, ChannelMetrics> metrics_store_;

    void update_producer_metrics(const std::string &channel,
                                 const nlohmann::json &metrics, uint64_t pid);
    void update_consumer_metrics(const std::string &channel,
                                 const std::string &uid,
                                 const nlohmann::json &metrics);
    nlohmann::json query_metrics(const std::string &channel) const;

    void handle_metrics_report_req(const nlohmann::json &req);
    nlohmann::json handle_metrics_req(const nlohmann::json &req);

    void run();

    void process_message(zmq::socket_t&       socket,
                         const zmq::message_t& identity,
                         const std::string&    msg_type,
                         const nlohmann::json& payload);

    nlohmann::json handle_reg_req(const nlohmann::json& req,
                                   const zmq::message_t& identity,
                                   zmq::socket_t&        socket);
    nlohmann::json handle_disc_req(const nlohmann::json& req);
    nlohmann::json handle_dereg_req(const nlohmann::json& req);
    nlohmann::json handle_consumer_reg_req(const nlohmann::json& req,
                                           const zmq::message_t& identity);
    nlohmann::json handle_consumer_dereg_req(const nlohmann::json& req);
    void           handle_heartbeat_req(const nlohmann::json& req);
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

    void send_closing_notify(zmq::socket_t&                    socket,
                             const std::string&                channel_name,
                             const ChannelEntry&               entry,
                             const std::string&                reason);
    void send_force_shutdown(zmq::socket_t&      socket,
                             const std::string&   channel_name,
                             const ChannelEntry&  entry);

    void handle_checksum_error_report(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    void handle_channel_notify_req(zmq::socket_t&        socket,
                                   const nlohmann::json& req);

    void handle_channel_broadcast_req(zmq::socket_t&        socket,
                                      const nlohmann::json& req);

    nlohmann::json handle_channel_list_req();

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

    // ── Connection policy (Phase 3) ────────────────────────────────────────────
    /// Resolve effective policy for a channel: per-channel override > hub-wide default.
    [[nodiscard]] ConnectionPolicy effective_policy(const std::string& channel_name) const noexcept;

    /// Check whether the given identity is allowed by the effective policy.
    /// Returns a non-empty error JSON if the connection should be rejected; std::nullopt if allowed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<nlohmann::json>
    check_connection_policy(const std::string& channel_name,
                            const std::string& actor_name,
                            const std::string& actor_uid,
                            const std::string& corr_id,
                            bool               is_consumer) const;
};

// ============================================================================
// BrokerServiceImpl::run() — main event loop
// ============================================================================

void BrokerServiceImpl::run()
{
    zmq::context_t ctx(1);
    zmq::socket_t router(ctx, zmq::socket_type::router);

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

    while (!stop_requested.load(std::memory_order_acquire))
    {
        // --- Poll phase (no mutex: zmq_poll blocks up to kPollTimeout, no registry access) ---
        std::vector<zmq::pollitem_t> items = {{router.handle(), 0, ZMQ_POLLIN, 0}};
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
                auto* entry = registry.find_channel_mutable(ch);
                if (entry != nullptr && entry->status != ChannelStatus::Closing)
                {
                    LOGGER_INFO("Broker: script-requested close for channel '{}'", ch);
                    send_closing_notify(router, ch, *entry, "script_requested");
                    // Transition to Closing with a grace deadline — don't deregister yet.
                    // Clients have until the deadline to process queued messages and deregister.
                    entry->status = ChannelStatus::Closing;
                    entry->closing_deadline =
                        std::chrono::steady_clock::now() + cfg.channel_shutdown_grace;
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

            // Check heartbeat timeouts and consumer liveness every poll cycle (≈100ms resolution).
            check_heartbeat_timeouts(router);
            check_dead_consumers(router);
            check_closing_deadlines(router);

            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                continue;
            }

            std::vector<zmq::message_t> frames;
            auto num_parts = zmq::recv_multipart(router, std::back_inserter(frames));
            if (!num_parts.has_value())
            {
                LOGGER_WARN("Broker: recv_multipart failed, possible ZMQ error or context termination.");
                continue;
            }

            // Expected layout: [identity, 'C', msg_type_string, json_body]
            if (*num_parts < 4)
            {
                LOGGER_WARN("Broker: malformed message (expected ≥4 frames, got {})",
                            *num_parts);
                continue;
            }

            try
            {
                // Reject oversized payloads before string construction and JSON parse.
                // ROUTER socket: silently drop — no mandatory reply.
                static constexpr size_t kMaxPayloadBytes = 1u << 20; // 1 MB
                if (frames[3].size() > kMaxPayloadBytes)
                {
                    LOGGER_WARN("Broker: oversized payload ({} bytes) — dropped",
                                frames[3].size());
                    continue;
                }
                const std::string msg_type = frames[2].to_string();
                const std::string payload_raw = frames[3].to_string();
                LOGGER_TRACE("Broker: recv msg_type='{}' payload_len={}",
                             msg_type, payload_raw.size());
                nlohmann::json payload = nlohmann::json::parse(payload_raw);
                process_message(router, frames[0], msg_type, payload);
            }
            catch (const nlohmann::json::exception& e)
            {
                LOGGER_WARN("Broker: malformed JSON: {}", e.what());
            }
        } // mutex released before next poll
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
                                        const nlohmann::json& payload)
{
    LOGGER_TRACE("Broker: dispatch {} channel='{}'", msg_type,
                 payload.is_object()
                     ? payload.value("channel_name", payload.value("channel", std::string{}))
                     : std::string{});

    if (msg_type == "REG_REQ")
    {
        nlohmann::json resp = handle_reg_req(payload, identity, socket);
        const std::string ack = (resp.value("status", "") == "success") ? "REG_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "DISC_REQ")
    {
        nlohmann::json resp = handle_disc_req(payload);
        const std::string ack = (resp.value("status", "") == "success") ? "DISC_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "DEREG_REQ")
    {
        nlohmann::json resp = handle_dereg_req(payload);
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
        nlohmann::json resp = handle_consumer_dereg_req(payload);
        const std::string ack =
            (resp.value("status", "") == "success") ? "CONSUMER_DEREG_ACK" : "ERROR";
        send_reply(socket, identity, ack, resp);
    }
    else if (msg_type == "HEARTBEAT_REQ")
    {
        // Fire-and-forget: update timestamp, no reply.
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
    else
    {
        LOGGER_WARN("Broker: unknown msg_type '{}'", msg_type);
        const std::string corr_id = payload.value("correlation_id", "");
        send_reply(socket, identity, "ERROR",
                   make_error(corr_id, "UNKNOWN_MSG_TYPE",
                              "Unknown message type: " + msg_type));
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
    const std::string actor_name = req.value("actor_name", "");
    const std::string actor_uid  = req.value("actor_uid", "");
    if (auto err = check_connection_policy(channel_name, actor_name, actor_uid, corr_id,
                                           /*is_consumer=*/false))
    {
        return *err;
    }

    ChannelEntry entry;
    entry.shm_name              = req.value("shm_name", "");
    entry.schema_hash           = attempted_schema;
    entry.schema_version        = req.value("schema_version", uint32_t{0});
    entry.producer_pid          = attempted_pid;
    entry.producer_hostname     = req.value("producer_hostname", "");
    entry.producer_actor_name   = actor_name;
    entry.producer_actor_uid    = actor_uid;
    entry.has_shared_memory     = req.value("has_shared_memory", false);
    entry.pattern               = channel_pattern_from_str(req.value("channel_pattern", "PubSub"));
    entry.zmq_ctrl_endpoint     = req.value("zmq_ctrl_endpoint", "");
    entry.zmq_data_endpoint     = req.value("zmq_data_endpoint", "");
    entry.zmq_pubkey            = req.value("zmq_pubkey", "");
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

    if (!registry.register_channel(channel_name, std::move(entry)))
    {
        // Cat 1: schema mismatch — invariant violation. Log + notify existing producer.
        auto* existing = registry.find_channel_mutable(channel_name);
        const std::string existing_schema = existing ? existing->schema_hash : "(unknown)";
        LOGGER_ERROR(
            "Broker: Cat1 schema mismatch on '{}': existing={} attempted={} attempted_pid={}",
            channel_name, existing_schema, attempted_schema, attempted_pid);
        if (existing && !existing->producer_zmq_identity.empty())
        {
            nlohmann::json err;
            err["channel_name"]          = channel_name;
            err["event"]                 = "schema_mismatch_attempt";
            err["existing_schema_hash"]  = existing_schema;
            err["attempted_schema_hash"] = attempted_schema;
            err["attempted_pid"]         = attempted_pid;
            send_to_identity(socket, existing->producer_zmq_identity, "CHANNEL_ERROR_NOTIFY",
                             err);
        }
        return make_error(corr_id, "SCHEMA_MISMATCH",
                          "Schema hash differs from existing registration for channel '" +
                              channel_name + "'");
    }

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

    auto entry = registry.find_channel(channel_name);
    if (!entry.has_value())
    {
        LOGGER_WARN("Broker: DISC_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    // Gate consumer access on the producer having sent its first heartbeat.
    if (entry->status == ChannelStatus::PendingReady)
    {
        LOGGER_INFO("Broker: DISC_REQ channel '{}' pending first heartbeat", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_READY",
                          "Producer has not sent first heartbeat yet for channel '" +
                              channel_name + "'");
    }

    LOGGER_INFO("Broker: discovered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"]            = "success";
    resp["shm_name"]          = entry->shm_name;
    resp["schema_hash"]       = entry->schema_hash;
    resp["schema_version"]    = entry->schema_version;
    resp["metadata"]          = entry->metadata;
    resp["consumer_count"]    =
        static_cast<uint32_t>(registry.find_consumers(channel_name).size());
    resp["has_shared_memory"] = entry->has_shared_memory;
    resp["channel_pattern"]   = channel_pattern_to_str(entry->pattern);
    resp["zmq_ctrl_endpoint"] = entry->zmq_ctrl_endpoint;
    resp["zmq_data_endpoint"] = entry->zmq_data_endpoint;
    resp["zmq_pubkey"]        = entry->zmq_pubkey;
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerServiceImpl::handle_dereg_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const uint64_t producer_pid = req.value("producer_pid", uint64_t{0});

    if (!registry.deregister_channel(channel_name, producer_pid))
    {
        LOGGER_WARN("Broker: DEREG_REQ failed for channel '{}' (pid={})", channel_name,
                    producer_pid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name + "' not registered or pid mismatch");
    }

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

    const auto channel_entry = registry.find_channel(channel_name);
    if (!channel_entry.has_value())
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
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
    const std::string actor_name = req.value("consumer_name", "");
    const std::string actor_uid  = req.value("consumer_uid", "");
    if (auto err = check_connection_policy(channel_name, actor_name, actor_uid, corr_id,
                                           /*is_consumer=*/true))
    {
        return *err;
    }

    ConsumerEntry entry;
    entry.consumer_pid       = req.value("consumer_pid", uint64_t{0});
    entry.consumer_hostname  = req.value("consumer_hostname", "");
    entry.actor_name         = actor_name;
    entry.actor_uid          = actor_uid;
    // Capture ZMQ identity for future CHANNEL_CLOSING_NOTIFY.
    entry.zmq_identity.assign(static_cast<const char*>(identity.data()), identity.size());
    registry.register_consumer(channel_name, std::move(entry));

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

nlohmann::json BrokerServiceImpl::handle_consumer_dereg_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const uint64_t consumer_pid = req.value("consumer_pid", uint64_t{0});
    if (!registry.deregister_consumer(channel_name, consumer_pid))
    {
        LOGGER_WARN("Broker: CONSUMER_DEREG_REQ failed for channel '{}' (pid={})", channel_name,
                    consumer_pid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Consumer pid " + std::to_string(consumer_pid) +
                              " not registered for channel '" + channel_name + "'");
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

void BrokerServiceImpl::handle_heartbeat_req(const nlohmann::json& req)
{
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ missing channel_name");
        return;
    }
    if (registry.update_heartbeat(channel_name))
    {
        LOGGER_DEBUG("Broker: heartbeat for channel '{}'", channel_name);
        // HEP-CORE-0019: extract piggybacked metrics (if present).
        if (req.contains("metrics") && req["metrics"].is_object())
        {
            const uint64_t pid = req.value("producer_pid", uint64_t{0});
            update_producer_metrics(channel_name, req["metrics"], pid);
        }
    }
    else
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for unknown channel '{}'", channel_name);
    }
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
    const auto entry = registry.find_channel(channel_name);
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
                                            const std::string& actor_name,
                                            const std::string& actor_uid,
                                            const std::string& corr_id,
                                            bool               is_consumer) const
{
    const ConnectionPolicy policy  = effective_policy(channel_name);
    const std::string      role_str = is_consumer ? "consumer" : "producer";

    if (policy == ConnectionPolicy::Required || policy == ConnectionPolicy::Verified)
    {
        if (actor_name.empty() || actor_uid.empty())
        {
            LOGGER_WARN("Broker: policy={} rejected {} for '{}': missing actor_name/uid",
                        connection_policy_to_str(policy), role_str, channel_name);
            return make_error(corr_id, "IDENTITY_REQUIRED",
                              fmt::format("Connection policy '{}' requires actor_name and actor_uid",
                                          connection_policy_to_str(policy)));
        }
    }

    if (policy == ConnectionPolicy::Verified)
    {
        const bool found = std::any_of(
            cfg.known_actors.begin(), cfg.known_actors.end(),
            [&](const KnownActor& ka)
            {
                if (ka.name != actor_name || ka.uid != actor_uid)
                {
                    return false;
                }
                const bool role_ok = ka.role.empty() || ka.role == "any" || ka.role == role_str;
                return role_ok;
            });
        if (!found)
        {
            LOGGER_WARN("Broker: Verified policy rejected {} '{}' uid='{}' for '{}': "
                        "not in known_actors",
                        role_str, actor_name, actor_uid, channel_name);
            return make_error(corr_id, "NOT_IN_KNOWN_ACTORS",
                              fmt::format("Actor '{}' (uid={}) is not in the hub's known_actors list",
                                          actor_name, actor_uid));
        }
    }

    if (policy != ConnectionPolicy::Open && (!actor_name.empty() || !actor_uid.empty()))
    {
        LOGGER_INFO("Broker: {} identity recorded for '{}': name='{}' uid='{}'",
                    role_str, channel_name, actor_name, actor_uid);
    }

    return std::nullopt;
}

// ============================================================================
// Heartbeat timeout detection
// ============================================================================

void BrokerServiceImpl::check_heartbeat_timeouts(zmq::socket_t& socket)
{
    if (cfg.channel_timeout.count() <= 0)
    {
        return;
    }
    const auto timed_out = registry.find_timed_out_channels(cfg.channel_timeout);
    for (const auto& channel_name : timed_out)
    {
        auto* entry = registry.find_channel_mutable(channel_name);
        if (entry == nullptr || entry->status == ChannelStatus::Closing)
        {
            continue;
        }
        // Cat 1: heartbeat timeout — log + notify, then enter Closing with grace period.
        LOGGER_WARN("Broker: Cat1 channel '{}' timed out (no heartbeat within {}s); closing",
                    channel_name, cfg.channel_timeout.count());
        send_closing_notify(socket, channel_name, *entry, "heartbeat_timeout");
        entry->status = ChannelStatus::Closing;
        entry->closing_deadline =
            std::chrono::steady_clock::now() + cfg.channel_shutdown_grace;
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

    for (auto& [channel_name, entry] : registry.all_channels())
    {
        std::vector<ConsumerEntry> dead;
        for (const auto& c : entry.consumers)
        {
            if (!pylabhub::platform::is_process_alive(c.consumer_pid))
            {
                dead.push_back(c);
            }
        }
        for (const auto& dead_consumer : dead)
        {
            // Cat 2: dead consumer — notify producer + clean registry.
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
            }
            registry.deregister_consumer(channel_name, dead_consumer.consumer_pid);
        }
    }
}

void BrokerServiceImpl::send_closing_notify(zmq::socket_t&     socket,
                                             const std::string& channel_name,
                                             const ChannelEntry& entry,
                                             const std::string& reason)
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
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' → consumer pid={}",
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
            LOGGER_INFO("Broker: CHANNEL_CLOSING_NOTIFY for '{}' → producer pid={}",
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
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_remove;

    for (auto& [name, entry] : registry.all_channels())
    {
        if (entry.status != ChannelStatus::Closing)
            continue;

        // Grace period not yet expired.
        // Note: if all consumers deregister AND the producer sends DEREG_REQ,
        // deregister_channel() erases the entry from the registry — so no
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
        auto entry = registry.find_channel(name);
        if (entry.has_value())
            registry.deregister_channel(name, entry->producer_pid);
    }
}

// ============================================================================
// send_force_shutdown — bypass client message queue, immediate shutdown
// ============================================================================

void BrokerServiceImpl::send_force_shutdown(zmq::socket_t&     socket,
                                             const std::string& channel_name,
                                             const ChannelEntry& entry)
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
            LOGGER_INFO("Broker: FORCE_SHUTDOWN for '{}' → consumer pid={}",
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
            LOGGER_INFO("Broker: FORCE_SHUTDOWN for '{}' → producer pid={}",
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
        auto entry = registry.find_channel(channel);
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

    auto entry = registry.find_channel(target_channel);
    if (!entry || entry->producer_zmq_identity.empty())
    {
        LOGGER_DEBUG("Broker: CHANNEL_NOTIFY_REQ for '{}' — channel not found or no producer",
                     target_channel);
        return;
    }

    // Forward as CHANNEL_EVENT_NOTIFY to the producer.
    nlohmann::json fwd;
    fwd["channel_name"] = target_channel;
    fwd["event"]        = event;
    fwd["sender_uid"]   = sender_uid;
    if (req.contains("data") && req["data"].is_string())
        fwd["data"] = req["data"];

    send_to_identity(socket, entry->producer_zmq_identity, "CHANNEL_EVENT_NOTIFY", fwd);
    LOGGER_DEBUG("Broker: relayed CHANNEL_NOTIFY_REQ to producer of '{}' event='{}'",
                 target_channel, event);
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

    auto entry = registry.find_channel(target_channel);
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

    LOGGER_DEBUG("Broker: CHANNEL_BROADCAST_REQ '{}' msg='{}' → {} consumers + producer",
                 target_channel, message, entry->consumers.size());
}

// ============================================================================
// CHANNEL_LIST_REQ — return list of registered channels
// ============================================================================

nlohmann::json BrokerServiceImpl::handle_channel_list_req()
{
    nlohmann::json resp;
    resp["status"] = "success";

    nlohmann::json channels = nlohmann::json::array();
    for (const auto& [name, entry] : registry.all_channels())
    {
        nlohmann::json ch;
        ch["name"]           = name;
        ch["producer_uid"]   = entry.producer_actor_uid;
        ch["schema_id"]      = entry.schema_id;
        ch["consumer_count"] = entry.consumers.size();
        switch (entry.status)
        {
            case ChannelStatus::PendingReady: ch["status"] = "PendingReady"; break;
            case ChannelStatus::Ready:        ch["status"] = "Ready";        break;
            case ChannelStatus::Closing:      ch["status"] = "Closing";      break;
        }
        channels.push_back(std::move(ch));
    }
    resp["channels"] = std::move(channels);
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
    nlohmann::json result = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(pImpl->m_query_mu);
    for (const auto& [name, entry] : pImpl->registry.all_channels())
    {
        const char* status_str = "Unknown";
        switch (entry.status)
        {
        case ChannelStatus::PendingReady: status_str = "PendingReady"; break;
        case ChannelStatus::Ready:        status_str = "Ready";        break;
        case ChannelStatus::Closing:      status_str = "Closing";      break;
        }
        result.push_back(nlohmann::json{
            {"name",           name},
            {"schema_hash",    entry.schema_hash},
            {"consumer_count", static_cast<int>(entry.consumers.size())},
            {"producer_pid",   entry.producer_pid},
            {"status",         status_str}
        });
    }
    return result.dump();
}

ChannelSnapshot BrokerService::query_channel_snapshot() const
{
    ChannelSnapshot snap;
    std::lock_guard<std::mutex> lock(pImpl->m_query_mu);
    snap.channels.reserve(pImpl->registry.size());
    for (const auto& [name, entry] : pImpl->registry.all_channels())
    {
        const char* status_str = "Unknown";
        switch (entry.status)
        {
        case ChannelStatus::PendingReady: status_str = "PendingReady"; break;
        case ChannelStatus::Ready:        status_str = "Ready";        break;
        case ChannelStatus::Closing:      status_str = "Closing";      break;
        }
        ChannelSnapshotEntry e;
        e.name                = name;
        e.status              = status_str;
        e.consumer_count      = static_cast<int>(entry.consumers.size());
        e.producer_pid        = entry.producer_pid;
        e.schema_hash         = entry.schema_hash;
        e.producer_actor_name = entry.producer_actor_name;
        e.producer_actor_uid  = entry.producer_actor_uid;
        snap.channels.push_back(std::move(e));
    }
    return snap;
}

std::string BrokerService::query_metrics_json_str(const std::string& channel) const
{
    std::lock_guard<std::mutex> lock(pImpl->m_query_mu);
    return pImpl->query_metrics(channel).dump();
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
    return query_metrics(channel);
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

} // namespace pylabhub::broker
