#include "broker_service.hpp"

#include "plh_service.hpp"

#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>
#include <zmq.h>

#include <array>
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
// Broker poll timeout in milliseconds
constexpr std::chrono::milliseconds kPollTimeout{100};
} // namespace

// ============================================================================
// Construction / key generation
// ============================================================================

BrokerService::BrokerService(Config cfg) : m_cfg(std::move(cfg))
{
    if (m_cfg.use_curve)
    {
        std::array<char, kZ85KeyBufSize> pub{};
        std::array<char, kZ85KeyBufSize> sec{};
        if (zmq_curve_keypair(pub.data(), sec.data()) != 0)
        {
            throw std::runtime_error("BrokerService: zmq_curve_keypair failed");
        }
        m_server_public_z85.assign(pub.data(), kZ85KeyLen);
        m_server_secret_z85.assign(sec.data(), kZ85KeyLen);
    }
}

const std::string& BrokerService::server_public_key() const
{
    return m_server_public_z85;
}

// ============================================================================
// stop() — thread-safe
// ============================================================================

void BrokerService::stop()
{
    m_stop_requested.store(true, std::memory_order_release);
}

// ============================================================================
// run() — main event loop
// ============================================================================

void BrokerService::run()
{
    zmq::context_t ctx(1);
    zmq::socket_t router(ctx, zmq::socket_type::router);

    if (m_cfg.use_curve)
    {
        router.set(zmq::sockopt::curve_server, 1);
        router.set(zmq::sockopt::curve_secretkey, m_server_secret_z85);
        router.set(zmq::sockopt::curve_publickey, m_server_public_z85);
    }

    router.bind(m_cfg.endpoint);
    const std::string bound = router.get(zmq::sockopt::last_endpoint);
    if (m_cfg.on_ready)
    {
        m_cfg.on_ready(bound, m_server_public_z85);
    }
    LOGGER_INFO("Broker: listening on {}", bound);
    if (m_cfg.use_curve)
    {
        LOGGER_INFO("Broker: server_public_key = {}", m_server_public_z85);
    }

    while (!m_stop_requested.load(std::memory_order_acquire))
    {
        std::vector<zmq::pollitem_t> items = {{router.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, kPollTimeout);
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        std::vector<zmq::message_t> frames;
        static_cast<void>(zmq::recv_multipart(router, std::back_inserter(frames)));
        if (frames.size() < 3)
        {
            LOGGER_WARN("Broker: malformed message (expected ≥3 frames, got {})", frames.size());
            continue;
        }

        try
        {
            const std::string msg_type = frames[1].to_string();
            nlohmann::json payload = nlohmann::json::parse(frames[2].to_string());
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

void BrokerService::process_message(zmq::socket_t& socket,
                                    const zmq::message_t& identity,
                                    const std::string& msg_type,
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

nlohmann::json BrokerService::handle_reg_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    ChannelEntry entry;
    entry.shm_name = req.value("shm_name", "");
    entry.schema_hash = req.value("schema_hash", "");
    entry.schema_version = req.value("schema_version", uint32_t{0});
    entry.producer_pid = req.value("producer_pid", uint64_t{0});
    entry.producer_hostname = req.value("producer_hostname", "");
    if (req.contains("metadata") && req["metadata"].is_object())
    {
        entry.metadata = req["metadata"];
    }

    if (!m_registry.register_channel(channel_name, std::move(entry)))
    {
        LOGGER_WARN("Broker: REG_REQ schema mismatch for channel '{}'", channel_name);
        return make_error(corr_id, "SCHEMA_MISMATCH",
                          "Schema hash differs from existing registration for channel '" +
                              channel_name + "'");
    }

    LOGGER_INFO("Broker: registered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["channel_id"] = channel_name;
    resp["message"] = "Producer registered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerService::handle_disc_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    auto entry = m_registry.find_channel(channel_name);
    if (!entry.has_value())
    {
        LOGGER_WARN("Broker: DISC_REQ channel '{}' not found", channel_name);
        return make_error(corr_id, "CHANNEL_NOT_FOUND",
                          "Channel '" + channel_name + "' is not registered");
    }

    LOGGER_INFO("Broker: discovered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["shm_name"] = entry->shm_name;
    resp["schema_hash"] = entry->schema_hash;
    resp["schema_version"] = entry->schema_version;
    resp["metadata"] = entry->metadata;
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

nlohmann::json BrokerService::handle_dereg_req(const nlohmann::json& req)
{
    const std::string corr_id = req.value("correlation_id", "");
    const std::string channel_name = req.value("channel_name", "");
    if (channel_name.empty())
    {
        return make_error(corr_id, "INVALID_REQUEST", "Missing or empty 'channel_name'");
    }

    const uint64_t producer_pid = req.value("producer_pid", uint64_t{0});
    if (!m_registry.deregister_channel(channel_name, producer_pid))
    {
        LOGGER_WARN("Broker: DEREG_REQ failed for channel '{}' (pid={})", channel_name,
                    producer_pid);
        return make_error(corr_id, "NOT_REGISTERED",
                          "Channel '" + channel_name + "' not registered or pid mismatch");
    }

    LOGGER_INFO("Broker: deregistered channel '{}'", channel_name);
    nlohmann::json resp;
    resp["status"] = "success";
    resp["message"] = "Producer deregistered successfully";
    if (!corr_id.empty())
    {
        resp["correlation_id"] = corr_id;
    }
    return resp;
}

// ============================================================================
// Helpers
// ============================================================================

void BrokerService::send_reply(zmq::socket_t& socket,
                               const zmq::message_t& identity,
                               const std::string& msg_type_ack,
                               const nlohmann::json& body)
{
    const std::string body_str = body.dump();
    socket.send(zmq::message_t(identity.data(), identity.size()), zmq::send_flags::sndmore);
    socket.send(zmq::message_t(msg_type_ack.data(), msg_type_ack.size()),
                zmq::send_flags::sndmore);
    socket.send(zmq::message_t(body_str.data(), body_str.size()), zmq::send_flags::none);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
nlohmann::json BrokerService::make_error(const std::string& correlation_id,
                                         const std::string& error_code,
                                         const std::string& message)
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

} // namespace pylabhub::broker
