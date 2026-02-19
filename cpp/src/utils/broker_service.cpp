#include "utils/broker_service.hpp"

#include "channel_registry.hpp"

#include "plh_service.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>
#include <zmq.h>

#include <array>
#include <atomic>
#include <chrono>
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

/// Convert a ChannelPattern enum to a JSON-compatible string.
constexpr const char* pattern_to_str(ChannelPattern p) noexcept
{
    switch (p)
    {
    case ChannelPattern::Pipeline: return "Pipeline";
    case ChannelPattern::Bidir:    return "Bidir";
    default:                       return "PubSub";
    }
}

/// Parse a channel pattern string; returns PubSub on unknown values.
ChannelPattern pattern_from_str(const std::string& s) noexcept
{
    if (s == "Pipeline") return ChannelPattern::Pipeline;
    if (s == "Bidir")    return ChannelPattern::Bidir;
    return ChannelPattern::PubSub;
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

    void run();

    void process_message(zmq::socket_t&       socket,
                         const zmq::message_t& identity,
                         const std::string&    msg_type,
                         const nlohmann::json& payload);

    nlohmann::json handle_reg_req(const nlohmann::json& req);
    nlohmann::json handle_disc_req(const nlohmann::json& req);
    nlohmann::json handle_dereg_req(const nlohmann::json& req);
    nlohmann::json handle_consumer_reg_req(const nlohmann::json& req,
                                           const zmq::message_t& identity);
    nlohmann::json handle_consumer_dereg_req(const nlohmann::json& req);
    void           handle_heartbeat_req(const nlohmann::json& req);

    void check_heartbeat_timeouts(zmq::socket_t& socket);

    void send_closing_notify(zmq::socket_t&                    socket,
                             const std::string&                channel_name,
                             const std::vector<ConsumerEntry>& consumers,
                             const std::string&                reason);

    static void send_reply(zmq::socket_t&       socket,
                           const zmq::message_t& identity,
                           const std::string&    msg_type_ack,
                           const nlohmann::json& body);

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static nlohmann::json make_error(const std::string& correlation_id,
                                     const std::string& error_code,
                                     const std::string& message);
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
        std::vector<zmq::pollitem_t> items = {{router.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, kPollTimeout);

        // Check heartbeat timeouts every poll cycle (≈100ms resolution).
        check_heartbeat_timeouts(router);

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        std::vector<zmq::message_t> frames;
        static_cast<void>(zmq::recv_multipart(router, std::back_inserter(frames)));
        // Expected layout: [identity, 'C', msg_type_string, json_body]
        if (frames.size() < 4)
        {
            LOGGER_WARN("Broker: malformed message (expected ≥4 frames, got {})", frames.size());
            continue;
        }

        try
        {
            const std::string msg_type = frames[2].to_string();
            nlohmann::json payload = nlohmann::json::parse(frames[3].to_string());
            process_message(router, frames[0], msg_type, payload);
        }
        catch (const nlohmann::json::exception& e)
        {
            LOGGER_WARN("Broker: malformed JSON: {}", e.what());
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
                                        const nlohmann::json& payload)
{
    if (msg_type == "REG_REQ")
    {
        nlohmann::json resp = handle_reg_req(payload);
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

nlohmann::json BrokerServiceImpl::handle_reg_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    ChannelEntry entry;
    entry.shm_name            = req.value("shm_name", "");
    entry.schema_hash         = req.value("schema_hash", "");
    entry.schema_version      = req.value("schema_version", uint32_t{0});
    entry.producer_pid        = req.value("producer_pid", uint64_t{0});
    entry.producer_hostname   = req.value("producer_hostname", "");
    entry.has_shared_memory   = req.value("has_shared_memory", false);
    entry.pattern             = pattern_from_str(req.value("channel_pattern", "PubSub"));
    entry.zmq_ctrl_endpoint   = req.value("zmq_ctrl_endpoint", "");
    entry.zmq_data_endpoint   = req.value("zmq_data_endpoint", "");
    entry.zmq_pubkey          = req.value("zmq_pubkey", "");
    if (req.contains("metadata") && req["metadata"].is_object())
    {
        entry.metadata = req["metadata"];
    }
    // status starts as PendingReady (default); last_heartbeat = now() (default).

    if (!registry.register_channel(channel_name, std::move(entry)))
    {
        LOGGER_WARN("Broker: REG_REQ schema mismatch for channel '{}'", channel_name);
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
    resp["channel_pattern"]   = pattern_to_str(entry->pattern);
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

    if (!registry.find_channel(channel_name).has_value())
    {
        LOGGER_WARN("Broker: CONSUMER_REG_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    ConsumerEntry entry;
    entry.consumer_pid      = req.value("consumer_pid", uint64_t{0});
    entry.consumer_hostname = req.value("consumer_hostname", "");
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
    }
    else
    {
        LOGGER_WARN("Broker: HEARTBEAT_REQ for unknown channel '{}'", channel_name);
    }
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
        auto entry = registry.find_channel(channel_name);
        if (!entry.has_value())
        {
            continue;
        }
        LOGGER_WARN("Broker: channel '{}' timed out (no heartbeat within {}s); closing",
                    channel_name, cfg.channel_timeout.count());
        send_closing_notify(socket, channel_name, entry->consumers, "heartbeat_timeout");
        registry.deregister_channel(channel_name, entry->producer_pid);
    }
}

void BrokerServiceImpl::send_closing_notify(zmq::socket_t&                    socket,
                                            const std::string&                channel_name,
                                            const std::vector<ConsumerEntry>& consumers,
                                            const std::string&                reason)
{
    if (consumers.empty())
    {
        return;
    }

    nlohmann::json body;
    body["channel_name"] = channel_name;
    body["reason"]       = reason;
    const std::string body_str    = body.dump();
    const std::string notify_type = "CHANNEL_CLOSING_NOTIFY";

    for (const auto& consumer : consumers)
    {
        if (consumer.zmq_identity.empty())
        {
            continue;
        }
        try
        {
            socket.send(zmq::message_t(consumer.zmq_identity.data(),
                                       consumer.zmq_identity.size()),
                        zmq::send_flags::sndmore);
            socket.send(zmq::message_t(&kFrameTypeControl, 1), zmq::send_flags::sndmore);
            socket.send(zmq::message_t(notify_type.data(), notify_type.size()),
                        zmq::send_flags::sndmore);
            socket.send(zmq::message_t(body_str.data(), body_str.size()),
                        zmq::send_flags::none);
            LOGGER_INFO("Broker: sent CHANNEL_CLOSING_NOTIFY for '{}' to consumer pid={}",
                        channel_name, consumer.consumer_pid);
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("Broker: failed to notify consumer (pid={}) for '{}': {}",
                        consumer.consumer_pid, channel_name, e.what());
        }
    }
}

// ============================================================================
// Helpers
// ============================================================================

void BrokerServiceImpl::send_reply(zmq::socket_t&       socket,
                                   const zmq::message_t& identity,
                                   const std::string&    msg_type_ack,
                                   const nlohmann::json& body)
{
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

} // namespace pylabhub::broker
