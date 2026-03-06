// messenger_protocol.cpp
//
// Messenger broker protocol handler implementations.
//
// Defines MessengerImpl::handle_command for all broker protocol operations:
//   RegisterProducerCmd   — REG_REQ / REG_ACK
//   RegisterConsumerCmd   — CONSUMER_REG_REQ / CONSUMER_REG_ACK
//   DeregisterConsumerCmd — CONSUMER_DEREG_REQ / CONSUMER_DEREG_ACK
//   UnregisterChannelCmd  — DEREG_REQ / DEREG_ACK (non-fatal on timeout)
//   ChecksumErrorReportCmd— CHECKSUM_ERROR_REPORT (fire-and-forget)
//   DiscoverProducerCmd   — DISC_REQ / DISC_ACK (retry on CHANNEL_NOT_READY)
//   CreateChannelCmd      — REG_REQ / REG_ACK + immediate heartbeat + heartbeat registration
//   ConnectChannelCmd     — DISC + schema validation + CONSUMER_REG_REQ
//
// Also defines:
//   MessengerImpl::send_disc_req         — single DISC_REQ round-trip
//   MessengerImpl::send_immediate_heartbeat — fire-and-forget HEARTBEAT_REQ

#include "messenger_internal.hpp"

namespace pylabhub::hub
{

// Import shared constants and helpers (TU-local; does not affect other files).
using namespace internal;

// ============================================================================
// Broker Protocol Handlers
// ============================================================================

bool MessengerImpl::handle_command(RegisterProducerCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: register_producer('{}') skipped — not connected.",
                    cmd.channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["msg_type"]            = "REG_REQ";
        payload["channel_name"]        = cmd.channel;
        payload["shm_name"]            = cmd.info.shm_name;
        payload["producer_pid"]        = cmd.info.producer_pid;
        payload["schema_hash"]         = hex_encode_schema_hash(cmd.info.schema_hash);
        payload["schema_version"]      = cmd.info.schema_version;
        payload["has_shared_memory"]   = cmd.info.has_shared_memory;
        payload["channel_pattern"]     = channel_pattern_to_str(cmd.info.pattern);
        payload["zmq_ctrl_endpoint"]   = cmd.info.zmq_ctrl_endpoint;
        payload["zmq_data_endpoint"]   = cmd.info.zmq_data_endpoint;
        payload["zmq_pubkey"]          = cmd.info.zmq_pubkey;

        const std::string msg_type    = "REG_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: register_producer('{}') send failed.", cmd.channel);
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(kDefaultRegisterTimeoutMs));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_ERROR("Messenger: register_producer('{}') timed out.", cmd.channel);
            return false;
        }
        std::vector<zmq::message_t> recv_msgs;
        static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
        if (recv_msgs.size() < 3)
        {
            LOGGER_ERROR("Messenger: register_producer('{}') invalid response.", cmd.channel);
            return false;
        }
        nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
        const bool ok = response.value("status", "") == "success";
        if (!ok)
        {
            LOGGER_ERROR("Messenger: register_producer('{}') failed: {}", cmd.channel,
                         response.value("message", std::string("unknown")));
            return false;
        }
        // Send one immediate heartbeat so the channel transitions to Ready.
        // This makes discover_producer work without waiting for the periodic timer.
        send_immediate_heartbeat(*socket, cmd.channel, cmd.info.producer_pid);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in register_producer('{}'): {}",
                     cmd.channel, e.what());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in register_producer('{}'): {}",
                     cmd.channel, e.what());
    }
    return false;
}

bool MessengerImpl::handle_command(RegisterConsumerCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: register_consumer('{}') skipped — not connected.",
                    cmd.channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"]      = cmd.channel;
        payload["consumer_pid"]      = pylabhub::platform::get_pid();
        payload["consumer_hostname"] = "";

        const std::string msg_type    = "CONSUMER_REG_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: register_consumer('{}') send failed.", cmd.channel);
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(kDefaultRegisterTimeoutMs));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_ERROR("Messenger: register_consumer('{}') timed out.", cmd.channel);
            return false;
        }
        std::vector<zmq::message_t> recv_msgs;
        static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
        if (recv_msgs.size() < 3)
        {
            LOGGER_ERROR("Messenger: register_consumer('{}') invalid response.", cmd.channel);
            return false;
        }
        nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
        if (response.value("status", "") != "success")
        {
            LOGGER_ERROR("Messenger: register_consumer('{}') failed: {}", cmd.channel,
                         response.value("message", std::string("unknown")));
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in register_consumer('{}'): {}",
                     cmd.channel, e.what());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in register_consumer('{}'): {}",
                     cmd.channel, e.what());
    }
    return false;
}

bool MessengerImpl::handle_command(DeregisterConsumerCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: deregister_consumer('{}') skipped — not connected.",
                    cmd.channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"] = cmd.channel;
        payload["consumer_pid"] = pylabhub::platform::get_pid();

        const std::string msg_type    = "CONSUMER_DEREG_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: deregister_consumer('{}') send failed.", cmd.channel);
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(kDefaultRegisterTimeoutMs));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_ERROR("Messenger: deregister_consumer('{}') timed out.", cmd.channel);
            return false;
        }
        std::vector<zmq::message_t> recv_msgs;
        static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
        if (recv_msgs.size() < 3)
        {
            LOGGER_ERROR("Messenger: deregister_consumer('{}') invalid response.",
                         cmd.channel);
            return false;
        }
        nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
        if (response.value("status", "") != "success")
        {
            LOGGER_ERROR("Messenger: deregister_consumer('{}') failed: {}", cmd.channel,
                         response.value("message", std::string("unknown")));
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in deregister_consumer('{}'): {}",
                     cmd.channel, e.what());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in deregister_consumer('{}'): {}",
                     cmd.channel, e.what());
    }
    return false;
}

bool MessengerImpl::handle_command(UnregisterChannelCmd &cmd,
                                   std::optional<zmq::socket_t> &socket)
{
    // Remove from heartbeat list first (even if not connected).
    m_heartbeat_channels.erase(
        std::remove_if(m_heartbeat_channels.begin(), m_heartbeat_channels.end(),
                       [&cmd](const HeartbeatEntry &entry)
                       { return entry.channel == cmd.channel; }),
        m_heartbeat_channels.end());

    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: unregister_channel('{}') skipped — not connected.",
                    cmd.channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"] = cmd.channel;
        payload["producer_pid"] = pylabhub::platform::get_pid();

        const std::string msg_type    = "DEREG_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: unregister_channel('{}') send failed.", cmd.channel);
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(kDefaultRegisterTimeoutMs));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_WARN("Messenger: unregister_channel('{}') timed out (non-fatal).",
                        cmd.channel);
            return false;
        }
        std::vector<zmq::message_t> recv_msgs;
        static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
        if (recv_msgs.size() >= 3)
        {
            try
            {
                nlohmann::json response =
                    nlohmann::json::parse(recv_msgs.back().to_string());
                if (response.value("status", "") != "success")
                    LOGGER_WARN("Messenger: unregister_channel('{}') broker response: {}",
                                cmd.channel,
                                response.value("message", std::string("?")));
            }
            catch (...)
            {
            }
        }
        LOGGER_INFO("Messenger: unregister_channel('{}') sent.", cmd.channel);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in unregister_channel('{}'): {}",
                     cmd.channel, e.what());
    }
    return false;
}

bool MessengerImpl::handle_command(ChecksumErrorReportCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: report_checksum_error('{}') skipped — not connected.",
                    cmd.channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"] = cmd.channel;
        payload["slot_index"]   = cmd.slot_index;
        payload["error"]        = cmd.error_description;
        payload["reporter_pid"] = pylabhub::platform::get_pid();

        const std::string msg_type    = "CHECKSUM_ERROR_REPORT";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: report_checksum_error('{}') send failed.", cmd.channel);
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in report_checksum_error('{}'): {}",
                     cmd.channel, e.what());
    }
    return false;
}

// ── DISC_REQ round-trip helper ────────────────────────────────────────────────

/// Send DISC_REQ and return the response JSON. Sends a single request.
/// Returns nullopt if timeout, parse error, or send failure.
std::optional<nlohmann::json> MessengerImpl::send_disc_req(zmq::socket_t &socket,
                                                            const std::string &channel,
                                                            int timeout_ms) const
{
    nlohmann::json payload;
    payload["msg_type"]     = "DISC_REQ";
    payload["channel_name"] = channel;
    const std::string msg_type    = "DISC_REQ";
    const std::string payload_str = payload.dump();
    std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                           zmq::buffer(msg_type),
                                           zmq::buffer(payload_str)};
    if (!zmq::send_multipart(socket, msgs))
    {
        LOGGER_ERROR("Messenger: discover_producer('{}') send failed.", channel);
        return std::nullopt;
    }

    std::vector<zmq::pollitem_t> items = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    if ((items[0].revents & ZMQ_POLLIN) == 0)
    {
        return std::nullopt; // timeout (not an error log — caller decides)
    }
    std::vector<zmq::message_t> recv_msgs;
    static_cast<void>(zmq::recv_multipart(socket, std::back_inserter(recv_msgs)));
    if (recv_msgs.size() < 3)
    {
        LOGGER_ERROR("Messenger: discover_producer('{}') invalid response format.", channel);
        return std::nullopt;
    }
    try
    {
        return nlohmann::json::parse(recv_msgs.back().to_string());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: discover_producer('{}') JSON parse error: {}",
                     channel, e.what());
        return std::nullopt;
    }
}

bool MessengerImpl::handle_command(DiscoverProducerCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: discover_producer('{}') — not connected.", cmd.channel);
        cmd.result.set_value(std::nullopt);
        return false;
    }
    try
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(cmd.timeout_ms);
        const int kRetrySlice = pylabhub::kRetrySliceMs; // per-attempt budget on retry

        while (true)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                LOGGER_ERROR("Messenger: discover_producer('{}') timed out ({}ms).",
                             cmd.channel, cmd.timeout_ms);
                cmd.result.set_value(std::nullopt);
                return false;
            }
            const int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count());
            const int slice = std::min(remaining_ms, kRetrySlice);

            auto resp = send_disc_req(*socket, cmd.channel, slice);
            if (!resp.has_value())
            {
                // Timeout on this slice — check overall deadline.
                continue;
            }
            const std::string status     = resp->value("status", "");
            const std::string error_code = resp->value("error_code", "");

            if (status == "success")
            {
                if (!resp->contains("shm_name")       || !(*resp)["shm_name"].is_string() ||
                    !resp->contains("schema_hash")     || !(*resp)["schema_hash"].is_string() ||
                    !resp->contains("schema_version")  ||
                    !(*resp)["schema_version"].is_number_unsigned())
                {
                    LOGGER_ERROR("Messenger: discover_producer('{}') missing fields.",
                                 cmd.channel);
                    cmd.result.set_value(std::nullopt);
                    return false;
                }
                ConsumerInfo cinfo{};
                cinfo.shm_name          = (*resp)["shm_name"].get<std::string>();
                cinfo.schema_hash       =
                    hex_decode_schema_hash((*resp)["schema_hash"].get<std::string>());
                cinfo.schema_version    = (*resp)["schema_version"].get<uint32_t>();
                cinfo.has_shared_memory = resp->value("has_shared_memory", false);
                cinfo.pattern           = channel_pattern_from_str(
                    resp->value("channel_pattern", std::string("PubSub")));
                cinfo.zmq_ctrl_endpoint  = resp->value("zmq_ctrl_endpoint", "");
                cinfo.zmq_data_endpoint  = resp->value("zmq_data_endpoint", "");
                cinfo.zmq_pubkey         = resp->value("zmq_pubkey", "");
                cinfo.consumer_count     = resp->value("consumer_count", uint32_t{0});
                // HEP-CORE-0021: ZMQ Virtual Channel Node transport.
                cinfo.data_transport     = resp->value("data_transport", std::string{"shm"});
                cinfo.zmq_node_endpoint  = resp->value("zmq_node_endpoint", "");
                cmd.result.set_value(cinfo);
                return false;
            }
            if (error_code == "CHANNEL_NOT_READY")
            {
                // Channel registered but awaiting first heartbeat — retry.
                LOGGER_INFO("Messenger: discover_producer('{}') CHANNEL_NOT_READY — "
                            "retrying.",
                            cmd.channel);
                // Brief sleep so we don't spin-hammer the broker.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(pylabhub::kZmqPollIntervalMs));
                continue;
            }
            // Other errors (CHANNEL_NOT_FOUND, etc.) — give up.
            LOGGER_ERROR("Messenger: discover_producer('{}') failed: {}", cmd.channel,
                         resp->value("message", std::string("unknown")));
            cmd.result.set_value(std::nullopt);
            return false;
        }
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in discover_producer('{}'): {}",
                     cmd.channel, e.what());
        cmd.result.set_value(std::nullopt);
    }
    return false;
}

bool MessengerImpl::handle_command(CreateChannelCmd &cmd,
                                   std::optional<zmq::socket_t> &socket)
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_ERROR("Messenger: create_channel('{}') — not connected.", cmd.channel);
        cmd.result.set_value(false);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"]      = cmd.channel;
        payload["shm_name"]          = cmd.shm_name;
        payload["schema_hash"]       = hex_encode_schema_hash(cmd.schema_hash);
        payload["schema_version"]    = cmd.schema_version;
        payload["producer_pid"]      = cmd.producer_pid;
        payload["has_shared_memory"] = cmd.has_shared_memory;
        payload["channel_pattern"]   = channel_pattern_to_str(cmd.pattern);
        payload["zmq_ctrl_endpoint"] = cmd.zmq_ctrl_endpoint;
        payload["zmq_data_endpoint"] = cmd.zmq_data_endpoint;
        payload["zmq_pubkey"]        = cmd.zmq_pubkey;
        // Actor identity fields are optional (omitted when not configured).
        if (!cmd.actor_name.empty()) payload["actor_name"]  = cmd.actor_name;
        if (!cmd.actor_uid.empty())  payload["actor_uid"]   = cmd.actor_uid;
        // Named schema fields (HEP-CORE-0016 Phase 3) are optional.
        if (!cmd.schema_id.empty())  payload["schema_id"]   = cmd.schema_id;
        if (!cmd.schema_blds.empty()) payload["schema_blds"] = cmd.schema_blds;
        // HEP-CORE-0021: ZMQ Virtual Channel Node transport.
        if (cmd.data_transport != "shm") payload["data_transport"] = cmd.data_transport;
        if (!cmd.zmq_node_endpoint.empty()) payload["zmq_node_endpoint"] = cmd.zmq_node_endpoint;

        const std::string msg_type    = "REG_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: create_channel('{}') REG_REQ send failed.",
                         cmd.channel);
            cmd.result.set_value(false);
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(cmd.timeout_ms));
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_ERROR("Messenger: create_channel('{}') timed out waiting for REG_ACK.",
                         cmd.channel);
            cmd.result.set_value(false);
            return false;
        }
        std::vector<zmq::message_t> recv_msgs;
        static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
        if (recv_msgs.size() < 3)
        {
            LOGGER_ERROR("Messenger: create_channel('{}') invalid REG_ACK format.",
                         cmd.channel);
            cmd.result.set_value(false);
            return false;
        }
        nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
        if (response.value("status", "") != "success")
        {
            LOGGER_ERROR("Messenger: create_channel('{}') REG_ACK failed: {}", cmd.channel,
                         response.value("message", std::string("unknown")));
            cmd.result.set_value(false);
            return false;
        }

        // Send immediate heartbeat → channel becomes Ready on broker.
        send_immediate_heartbeat(*socket, cmd.channel, cmd.producer_pid);

        // Register for periodic heartbeat.
        m_heartbeat_channels.push_back({cmd.channel, cmd.producer_pid});
        LOGGER_INFO("Messenger: create_channel('{}') succeeded; heartbeat registered.",
                    cmd.channel);
        cmd.result.set_value(true);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in create_channel('{}'): {}", cmd.channel,
                     e.what());
        cmd.result.set_value(false);
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in create_channel('{}'): {}", cmd.channel,
                     e.what());
        cmd.result.set_value(false);
    }
    return false;
}

bool MessengerImpl::handle_command(ConnectChannelCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_ERROR("Messenger: connect_channel('{}') — not connected.", cmd.channel);
        cmd.result.set_value(std::nullopt);
        return false;
    }
    try
    {
        // Discover channel (retry on CHANNEL_NOT_READY until timeout).
        DiscoverProducerCmd disc_cmd;
        disc_cmd.channel    = cmd.channel;
        disc_cmd.timeout_ms = cmd.timeout_ms;
        // We borrow a promise/future just to get the ConsumerInfo from the handler.
        disc_cmd.result = std::promise<std::optional<ConsumerInfo>>{};
        auto disc_future = disc_cmd.result.get_future();
        handle_command(disc_cmd, socket);
        auto cinfo = disc_future.get();

        if (!cinfo.has_value())
        {
            // discover already logged the error.
            cmd.result.set_value(std::nullopt);
            return false;
        }

        // Validate schema hash if an expectation was provided.
        if (!cmd.expected_schema_hash.empty())
        {
            const std::string expected_hex =
                hex_encode_schema_hash(cmd.expected_schema_hash);
            if (cinfo->schema_hash != hex_decode_schema_hash(expected_hex) &&
                !cinfo->schema_hash.empty())
            {
                // Re-check by comparing hex-encoded versions.
                const std::string got_hex =
                    hex_encode_schema_hash(cinfo->schema_hash);
                if (got_hex != hex_encode_schema_hash(cmd.expected_schema_hash))
                {
                    LOGGER_ERROR("Messenger: connect_channel('{}') schema mismatch.",
                                 cmd.channel);
                    cmd.result.set_value(std::nullopt);
                    return false;
                }
            }
        }

        // Register this process as a consumer with the broker.
        nlohmann::json reg_payload;
        reg_payload["channel_name"]      = cmd.channel;
        reg_payload["consumer_pid"]      = pylabhub::platform::get_pid();
        reg_payload["consumer_hostname"] = "";
        // Actor identity fields are optional (omitted when not configured).
        if (!cmd.consumer_uid.empty())  reg_payload["consumer_uid"]          = cmd.consumer_uid;
        if (!cmd.consumer_name.empty()) reg_payload["consumer_name"]         = cmd.consumer_name;
        // Named schema field (HEP-CORE-0016 Phase 3) is optional.
        if (!cmd.expected_schema_id.empty()) reg_payload["expected_schema_id"] = cmd.expected_schema_id;
        const std::string msg_type   = "CONSUMER_REG_REQ";
        const std::string reg_str    = reg_payload.dump();
        std::vector<zmq::const_buffer> reg_msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                                    zmq::buffer(msg_type),
                                                    zmq::buffer(reg_str)};
        if (zmq::send_multipart(*socket, reg_msgs))
        {
            // Wait briefly for ACK (fire-and-forget on timeout — non-fatal).
            std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds(kDefaultRegisterTimeoutMs));
            if ((items[0].revents & ZMQ_POLLIN) != 0)
            {
                std::vector<zmq::message_t> ack_msgs;
                static_cast<void>(
                    zmq::recv_multipart(*socket, std::back_inserter(ack_msgs)));
                // Logged if failed; result still returned to caller.
                if (ack_msgs.size() >= 3)
                {
                    try
                    {
                        nlohmann::json ack =
                            nlohmann::json::parse(ack_msgs.back().to_string());
                        if (ack.value("status", "") != "success")
                        {
                            LOGGER_WARN("Messenger: connect_channel('{}') "
                                        "CONSUMER_REG failed: {}",
                                        cmd.channel,
                                        ack.value("message", std::string("?")));
                            // Broker explicitly rejected this consumer (e.g., schema_id
                            // mismatch). Return nullopt — the connection must not proceed.
                            cmd.result.set_value(std::nullopt);
                            return false;
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        cmd.result.set_value(cinfo);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in connect_channel('{}'): {}", cmd.channel,
                     e.what());
        cmd.result.set_value(std::nullopt);
    }
    return false;
}

// ── SCHEMA_REQ handler (HEP-CORE-0016 Phase 3) ───────────────────────────────

bool MessengerImpl::handle_command(QuerySchemaCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: query_channel_schema('{}') — not connected.", cmd.channel);
        cmd.result.set_value(std::nullopt);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["channel_name"] = cmd.channel;

        const std::string msg_type    = "SCHEMA_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: query_channel_schema('{}') send failed.", cmd.channel);
            cmd.result.set_value(std::nullopt);
            return false;
        }

        // Poll+recv loop: skip non-matching messages (e.g. heartbeat ACKs).
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(cmd.timeout_ms);
        nlohmann::json response;
        bool got_response = false;

        while (!got_response)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
            {
                LOGGER_WARN("Messenger: query_channel_schema('{}') timed out.", cmd.channel);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds(remaining));
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                LOGGER_WARN("Messenger: query_channel_schema('{}') timed out.", cmd.channel);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            std::vector<zmq::message_t> recv_msgs;
            static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
            if (recv_msgs.size() < 3)
                continue; // malformed — retry

            const std::string resp_type(static_cast<const char*>(recv_msgs[1].data()),
                                        recv_msgs[1].size());
            if (resp_type != "SCHEMA_ACK")
                continue; // not our response — discard and retry

            response = nlohmann::json::parse(recv_msgs.back().to_string());
            got_response = true;
        }

        if (response.value("status", "") != "success")
        {
            LOGGER_WARN("Messenger: query_channel_schema('{}') failed: {}", cmd.channel,
                        response.value("message", std::string("unknown")));
            cmd.result.set_value(std::nullopt);
            return false;
        }

        ChannelSchemaInfo info;
        info.schema_id = response.value("schema_id", "");
        info.blds      = response.value("blds", "");
        info.hash_hex  = response.value("schema_hash", "");
        cmd.result.set_value(std::move(info));
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in query_channel_schema('{}'): {}",
                     cmd.channel, e.what());
        cmd.result.set_value(std::nullopt);
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in query_channel_schema('{}'): {}",
                     cmd.channel, e.what());
        cmd.result.set_value(std::nullopt);
    }
    return false;
}

// ── CHANNEL_NOTIFY_REQ (fire-and-forget) ─────────────────────────────────────

bool MessengerImpl::handle_command(ChannelNotifyCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: channel_notify('{}') skipped — not connected.",
                    cmd.target_channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["target_channel"] = cmd.target_channel;
        payload["sender_uid"]     = cmd.sender_uid;
        payload["event"]          = cmd.event;
        if (!cmd.data.empty())
            payload["data"] = cmd.data;

        const std::string msg_type = "CHANNEL_NOTIFY_REQ";
        const std::string body_str = payload.dump();
        std::vector<zmq::const_buffer> frames = {zmq::buffer(&kFrameTypeControl, 1),
                                                   zmq::buffer(msg_type),
                                                   zmq::buffer(body_str)};
        static_cast<void>(zmq::send_multipart(*socket, frames));
        LOGGER_DEBUG("Messenger: CHANNEL_NOTIFY_REQ sent to '{}' event='{}'",
                     cmd.target_channel, cmd.event);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("Messenger: CHANNEL_NOTIFY_REQ failed for '{}': {}",
                    cmd.target_channel, e.what());
    }
    return false;
}

// ── CHANNEL_BROADCAST_REQ (fire-and-forget) ──────────────────────────────────

bool MessengerImpl::handle_command(ChannelBroadcastCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: channel_broadcast('{}') skipped — not connected.",
                    cmd.target_channel);
        return false;
    }
    try
    {
        nlohmann::json payload;
        payload["target_channel"] = cmd.target_channel;
        payload["sender_uid"]     = cmd.sender_uid;
        payload["message"]        = cmd.message;
        if (!cmd.data.empty())
            payload["data"] = cmd.data;

        const std::string msg_type = "CHANNEL_BROADCAST_REQ";
        const std::string body_str = payload.dump();
        std::vector<zmq::const_buffer> frames = {zmq::buffer(&kFrameTypeControl, 1),
                                                   zmq::buffer(msg_type),
                                                   zmq::buffer(body_str)};
        static_cast<void>(zmq::send_multipart(*socket, frames));
        LOGGER_DEBUG("Messenger: CHANNEL_BROADCAST_REQ sent to '{}' msg='{}'",
                     cmd.target_channel, cmd.message);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("Messenger: CHANNEL_BROADCAST_REQ failed for '{}': {}",
                    cmd.target_channel, e.what());
    }
    return false;
}

// ── CHANNEL_LIST_REQ (synchronous) ───────────────────────────────────────────

bool MessengerImpl::handle_command(ChannelListCmd &cmd,
                                   std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
    {
        LOGGER_WARN("Messenger: list_channels() — not connected.");
        cmd.result.set_value({});
        return false;
    }
    try
    {
        nlohmann::json payload = nlohmann::json::object();
        // No fields needed — broker returns all channels.

        const std::string msg_type    = "CHANNEL_LIST_REQ";
        const std::string payload_str = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                               zmq::buffer(msg_type),
                                               zmq::buffer(payload_str)};
        LOGGER_TRACE("Messenger: list_channels sending CHANNEL_LIST_REQ timeout={}ms",
                     cmd.timeout_ms);
        if (!zmq::send_multipart(*socket, msgs))
        {
            LOGGER_ERROR("Messenger: list_channels() send failed.");
            cmd.result.set_value({});
            return false;
        }

        // Poll+recv loop: skip non-matching messages (e.g. heartbeat ACKs)
        // that arrive before the CHANNEL_LIST_ACK.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(cmd.timeout_ms);
        nlohmann::json response;
        bool got_response = false;

        while (!got_response)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
            {
                LOGGER_WARN("Messenger: list_channels() timed out.");
                cmd.result.set_value({});
                return false;
            }

            std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds(remaining));
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                LOGGER_WARN("Messenger: list_channels() poll timeout.");
                cmd.result.set_value({});
                return false;
            }

            std::vector<zmq::message_t> recv_msgs;
            static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
            if (recv_msgs.size() < 3)
                continue; // malformed — retry

            const std::string resp_type(static_cast<const char*>(recv_msgs[1].data()),
                                        recv_msgs[1].size());
            LOGGER_TRACE("Messenger: list_channels recv frame[1]='{}' ({} frames)",
                         resp_type, recv_msgs.size());
            if (resp_type != "CHANNEL_LIST_ACK")
            {
                // Not our response — discard and retry (e.g. heartbeat ACK, event notify).
                continue;
            }

            response = nlohmann::json::parse(recv_msgs.back().to_string());
            got_response = true;
        }

        if (response.value("status", "") != "success")
        {
            LOGGER_WARN("Messenger: list_channels() failed: {}",
                        response.value("message", std::string("unknown")));
            cmd.result.set_value({});
            return false;
        }

        std::vector<nlohmann::json> result;
        if (response.contains("channels") && response["channels"].is_array())
        {
            for (auto &ch : response["channels"])
                result.push_back(std::move(ch));
        }
        cmd.result.set_value(std::move(result));
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: ZMQ error in list_channels(): {}", e.what());
        cmd.result.set_value({});
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("Messenger: JSON error in list_channels(): {}", e.what());
        cmd.result.set_value({});
    }
    return false;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

/// Send one HEARTBEAT_REQ immediately (fire-and-forget, no poll for reply).
void MessengerImpl::send_immediate_heartbeat(zmq::socket_t &socket,
                                              const std::string &channel,
                                              uint64_t producer_pid,
                                              const nlohmann::json &metrics)
{
    try
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["producer_pid"] = producer_pid;
        if (!metrics.is_null() && !metrics.empty())
            hb["metrics"] = metrics;
        const std::string hb_type = "HEARTBEAT_REQ";
        const std::string hb_str  = hb.dump();
        std::vector<zmq::const_buffer> hb_msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                                   zmq::buffer(hb_type),
                                                   zmq::buffer(hb_str)};
        static_cast<void>(zmq::send_multipart(socket, hb_msgs));
        LOGGER_DEBUG("Messenger: immediate heartbeat sent for '{}'", channel);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("Messenger: immediate heartbeat failed for '{}': {}", channel,
                    e.what());
    }
}

/// Send METRICS_REPORT_REQ (fire-and-forget, HEP-CORE-0019).
bool MessengerImpl::handle_command(MetricsReportCmd &cmd,
                                    std::optional<zmq::socket_t> &socket) const
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
        return false;
    try
    {
        nlohmann::json payload;
        payload["channel_name"] = cmd.channel;
        payload["uid"]          = cmd.uid;
        payload["metrics"]      = std::move(cmd.metrics);
        const std::string type_str = "METRICS_REPORT_REQ";
        const std::string body     = payload.dump();
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(&kFrameTypeControl, 1),
                                                zmq::buffer(type_str),
                                                zmq::buffer(body)};
        static_cast<void>(zmq::send_multipart(*socket, msgs));
        LOGGER_DEBUG("Messenger: METRICS_REPORT_REQ sent for '{}'", cmd.channel);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_WARN("Messenger: METRICS_REPORT_REQ failed for '{}': {}", cmd.channel,
                    e.what());
    }
    return false;
}

} // namespace pylabhub::hub
