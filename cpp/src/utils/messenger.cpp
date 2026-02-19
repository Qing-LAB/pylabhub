#include "plh_service.hpp"
#include "channel_handle_factory.hpp"
#include "utils/channel_handle.hpp"
#include "utils/messenger.hpp"
#include "utils/zmq_context.hpp"

#include "sodium.h"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// Constants and Helper Functions
// ============================================================================
namespace
{
constexpr size_t kZ85KeyChars = 40;
constexpr size_t kSchemaHashHexLen = 64;
constexpr size_t kSchemaHashBytes = 32;
constexpr unsigned int kNibbleMask = 0x0FU;
constexpr int kHexLetterOffset = 10;
constexpr size_t kZ85KeyBufSize = 41;
constexpr int kDefaultRegisterTimeoutMs = 5000;
constexpr std::chrono::milliseconds kHubShutdownTimeoutMs{5000};
/// How often the worker loop sends HEARTBEAT_REQ for registered producer channels.
constexpr std::chrono::seconds kHeartbeatInterval{2};
/// How often the worker loop wakes up (even if idle) to check the heartbeat timer.
constexpr std::chrono::milliseconds kWorkerPollInterval{200};

// Universal framing: Frame 0 type byte for all ZMQ messages in this project.
// 'C' = Control (Frame 1: type string, Frame 2: JSON body)
// 'A' = ASCII/JSON data   (Frame 1: JSON payload)
// 'M' = MessagePack data  (Frame 1: MessagePack bytes)
constexpr char kFrameTypeControl = 'C';

bool is_valid_z85_key(const std::string &key)
{
    return key.length() == kZ85KeyChars;
}

std::string hex_encode_schema_hash(const std::string &raw)
{
    static const std::array<char, 17> kHexChars = {"0123456789abcdef"};
    std::string out;
    out.reserve(kSchemaHashHexLen);
    for (unsigned char byte : raw)
    {
        out += kHexChars[(byte >> 4) & kNibbleMask];
        out += kHexChars[byte & kNibbleMask];
    }
    return out;
}

std::string hex_decode_schema_hash(const std::string &hex_str)
{
    auto hex_val = [](char hex_char) -> int {
        if (hex_char >= '0' && hex_char <= '9') return hex_char - '0';
        if (hex_char >= 'a' && hex_char <= 'f') return hex_char - 'a' + kHexLetterOffset;
        if (hex_char >= 'A' && hex_char <= 'F') return hex_char - 'A' + kHexLetterOffset;
        return -1;
    };
    if (hex_str.size() != kSchemaHashHexLen) return {};
    std::string out;
    out.reserve(kSchemaHashBytes);
    for (size_t i = 0; i < kSchemaHashHexLen; i += 2)
    {
        int high_val = hex_val(hex_str[i]);
        int low_val  = hex_val(hex_str[i + 1]);
        if (high_val < 0 || low_val < 0) return {};
        out += static_cast<char>((high_val << 4) | low_val);
    }
    return out;
}

/// Convert ChannelPattern enum to JSON wire string.
constexpr const char *pattern_to_wire(ChannelPattern p) noexcept
{
    switch (p)
    {
    case ChannelPattern::Pipeline: return "Pipeline";
    case ChannelPattern::Bidir:    return "Bidir";
    default:                       return "PubSub";
    }
}

/// Parse ChannelPattern from JSON wire string.
ChannelPattern pattern_from_wire(const std::string &s) noexcept
{
    if (s == "Pipeline") return ChannelPattern::Pipeline;
    if (s == "Bidir")    return ChannelPattern::Bidir;
    return ChannelPattern::PubSub;
}
} // namespace

// ============================================================================
// Async Queue Commands
// ============================================================================

struct ConnectCmd
{
    std::string endpoint;
    std::string server_key;
    std::promise<bool> result;
};
struct DisconnectCmd
{
    std::promise<void> result;
};
struct RegisterProducerCmd
{
    std::string channel;
    ProducerInfo info;
};
struct RegisterConsumerCmd
{
    std::string channel;
    ConsumerInfo info;
};
struct DeregisterConsumerCmd
{
    std::string channel;
};
struct DiscoverProducerCmd
{
    std::string channel;
    int timeout_ms;
    std::promise<std::optional<ConsumerInfo>> result;
};
/// Internal: sent by create_channel() after binding P2C sockets.
/// Worker sends REG_REQ with the P2C endpoints, waits for ACK, registers heartbeat.
struct CreateChannelCmd
{
    std::string    channel;
    ChannelPattern pattern;
    bool           has_shared_memory;
    std::string    schema_hash; ///< raw bytes (not hex-encoded)
    uint32_t       schema_version;
    uint64_t       producer_pid;
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey; ///< Z85 producer public key for P2C sockets
    int            timeout_ms;
    std::promise<bool> result; ///< true = broker accepted (REG_ACK received)
};
/// Internal: sent by connect_channel() to discover and register as consumer.
/// Worker sends DISC_REQ (retrying on CHANNEL_NOT_READY), then CONSUMER_REG_REQ.
struct ConnectChannelCmd
{
    std::string channel;
    int         timeout_ms;
    std::string expected_schema_hash; ///< raw bytes; empty = accept any
    std::promise<std::optional<ConsumerInfo>> result;
};
struct StopCmd
{
};

using MessengerCommand = std::variant<ConnectCmd, DisconnectCmd, RegisterProducerCmd,
                                      RegisterConsumerCmd, DiscoverProducerCmd,
                                      DeregisterConsumerCmd, CreateChannelCmd,
                                      ConnectChannelCmd, StopCmd>;

// ============================================================================
// MessengerImpl
// ============================================================================

struct HeartbeatEntry
{
    std::string channel;
    uint64_t    producer_pid;
};

class MessengerImpl
{
  public:
    // Worker thread state
    std::deque<MessengerCommand> m_queue;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::thread m_worker;
    std::atomic<bool> m_running{false};

    // Connection state (set by worker thread only after connect)
    std::atomic<bool> m_is_connected{false};

    // Guards connect/disconnect (public API side)
    std::mutex m_connect_mutex;

    // Client key pair (set in ConnectCmd handler)
    std::string m_client_public_key_z85;
    std::string m_client_secret_key_z85;

    // Heartbeat entries (worker-thread-owned; channels registered via register_producer
    // or create_channel).  Protected implicitly by the fact that only the worker thread
    // reads/writes this after startup.
    std::vector<HeartbeatEntry> m_heartbeat_channels;
    std::chrono::steady_clock::time_point m_next_heartbeat{};

    // CHANNEL_CLOSING_NOTIFY callback (guarded by m_cb_mutex for on_channel_closing())
    std::mutex m_cb_mutex;
    std::function<void(const std::string &)> m_channel_closing_cb;

    MessengerImpl() = default;

    ~MessengerImpl()
    {
        if (m_running.load(std::memory_order_acquire))
        {
            enqueue(StopCmd{});
            if (m_worker.joinable())
            {
                m_worker.join();
            }
        }
    }

    void enqueue(MessengerCommand cmd)
    {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_queue.push_back(std::move(cmd));
        }
        m_queue_cv.notify_one();
    }

    void start_worker()
    {
        m_running.store(true, std::memory_order_release);
        m_worker = std::thread(&MessengerImpl::worker_loop, this);
    }

    // ── Worker loop ──────────────────────────────────────────────────────────

    void worker_loop()
    {
        std::optional<zmq::socket_t> socket;
        m_next_heartbeat = std::chrono::steady_clock::now() + kHeartbeatInterval;

        while (true)
        {
            std::deque<MessengerCommand> batch;
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);

                if (m_is_connected.load(std::memory_order_acquire))
                {
                    // When connected, cap the wait so heartbeats and incoming
                    // message polling happen on schedule.
                    auto now = std::chrono::steady_clock::now();
                    auto until = m_next_heartbeat;
                    if (until <= now) until = now + kWorkerPollInterval;
                    m_queue_cv.wait_until(lock, until,
                                          [this] { return !m_queue.empty(); });
                }
                else
                {
                    m_queue_cv.wait(lock, [this] { return !m_queue.empty(); });
                }
                std::swap(batch, m_queue);
            }

            for (auto &cmd : batch)
            {
                bool stop = std::visit(
                    [&](auto &&variant_cmd) { return handle_command(variant_cmd, socket); },
                    cmd);
                if (stop)
                {
                    m_running.store(false, std::memory_order_release);
                    return;
                }
            }

            // Process any unsolicited incoming broker messages
            // (e.g. CHANNEL_CLOSING_NOTIFY).
            if (socket.has_value())
            {
                process_incoming(*socket);
            }

            // Send heartbeats if due.
            auto now = std::chrono::steady_clock::now();
            if (socket.has_value() && now >= m_next_heartbeat)
            {
                send_heartbeats(*socket);
                m_next_heartbeat = now + kHeartbeatInterval;
            }
        }
    }

    // ── Incoming unsolicited message handler ─────────────────────────────────

    void process_incoming(zmq::socket_t &socket)
    {
        while (true)
        {
            std::vector<zmq::pollitem_t> items = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds{0}); // non-blocking
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                break;
            }
            std::vector<zmq::message_t> msgs;
            static_cast<void>(
                zmq::recv_multipart(socket, std::back_inserter(msgs),
                                    zmq::recv_flags::dontwait));
            // Layout: ['C', msg_type, json_body]
            if (msgs.size() < 3)
            {
                continue;
            }
            const std::string msg_type = msgs[1].to_string();
            if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            {
                try
                {
                    nlohmann::json body =
                        nlohmann::json::parse(msgs[2].to_string());
                    std::string channel = body.value("channel_name", "");
                    LOGGER_INFO("Messenger: CHANNEL_CLOSING_NOTIFY for '{}'", channel);
                    std::function<void(const std::string &)> cb;
                    {
                        std::lock_guard<std::mutex> lock(m_cb_mutex);
                        cb = m_channel_closing_cb;
                    }
                    if (cb && !channel.empty())
                    {
                        cb(channel);
                    }
                }
                catch (const nlohmann::json::exception &e)
                {
                    LOGGER_WARN("Messenger: bad CHANNEL_CLOSING_NOTIFY JSON: {}", e.what());
                }
            }
            // Other unsolicited types can be added here in future.
        }
    }

    // ── Heartbeat sender ─────────────────────────────────────────────────────

    void send_heartbeats(zmq::socket_t &socket)
    {
        for (const auto &entry : m_heartbeat_channels)
        {
            try
            {
                nlohmann::json payload;
                payload["channel_name"] = entry.channel;
                payload["producer_pid"] = entry.producer_pid;
                const std::string msg_type   = "HEARTBEAT_REQ";
                const std::string payload_str = payload.dump();
                std::vector<zmq::const_buffer> msgs = {
                    zmq::buffer(&kFrameTypeControl, 1),
                    zmq::buffer(msg_type),
                    zmq::buffer(payload_str)};
                static_cast<void>(zmq::send_multipart(socket, msgs));
                LOGGER_DEBUG("Messenger: heartbeat sent for '{}'", entry.channel);
            }
            catch (const zmq::error_t &e)
            {
                LOGGER_ERROR("Messenger: heartbeat send failed for '{}': {}",
                             entry.channel, e.what());
            }
        }
    }

    // ── Command handlers ─────────────────────────────────────────────────────

    // Returns true if the worker should stop.
    bool handle_command(ConnectCmd &cmd, std::optional<zmq::socket_t> &socket)
    {
        if (m_is_connected.load(std::memory_order_acquire))
        {
            LOGGER_WARN("Messenger: Already connected.");
            cmd.result.set_value(true);
            return false;
        }
        if (cmd.endpoint.empty())
        {
            LOGGER_ERROR("Messenger: Broker endpoint cannot be empty.");
            cmd.result.set_value(false);
            return false;
        }
        if (!is_valid_z85_key(cmd.server_key))
        {
            LOGGER_ERROR("Messenger: Invalid broker public key format.");
            cmd.result.set_value(false);
            return false;
        }
        try
        {
            std::array<char, kZ85KeyBufSize> z85_public{};
            std::array<char, kZ85KeyBufSize> z85_secret{};
            if (zmq_curve_keypair(z85_public.data(), z85_secret.data()) != 0)
            {
                LOGGER_ERROR("Messenger: Failed to generate CurveZMQ key pair.");
                cmd.result.set_value(false);
                return false;
            }
            m_client_public_key_z85 = z85_public.data();
            m_client_secret_key_z85 = z85_secret.data();

            socket.emplace(get_zmq_context(), zmq::socket_type::dealer);
            socket->set(zmq::sockopt::curve_serverkey, cmd.server_key);
            socket->set(zmq::sockopt::curve_publickey, m_client_public_key_z85);
            socket->set(zmq::sockopt::curve_secretkey, m_client_secret_key_z85);
            socket->connect(cmd.endpoint);
            m_is_connected.store(true, std::memory_order_release);
            LOGGER_INFO("Messenger: Connected to broker at {}.", cmd.endpoint);
            cmd.result.set_value(true);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: Failed to connect to broker at {}: {} ({})",
                         cmd.endpoint, e.what(), e.num());
            socket.reset();
            m_is_connected.store(false, std::memory_order_release);
            cmd.result.set_value(false);
        }
        return false;
    }

    bool handle_command(DisconnectCmd &cmd, std::optional<zmq::socket_t> &socket)
    {
        if (!m_is_connected.load(std::memory_order_acquire))
        {
            cmd.result.set_value();
            return false;
        }
        try
        {
            if (socket.has_value()) socket->close();
            socket.reset();
            m_is_connected.store(false, std::memory_order_release);
            m_heartbeat_channels.clear();
            LOGGER_INFO("Messenger: Disconnected from broker.");
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: Error during disconnect: {}", e.what());
            socket.reset();
            m_is_connected.store(false, std::memory_order_release);
        }
        cmd.result.set_value();
        return false;
    }

    bool handle_command(RegisterProducerCmd &cmd,
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
            payload["channel_pattern"]     = pattern_to_wire(cmd.info.pattern);
            payload["zmq_ctrl_endpoint"]   = cmd.info.zmq_ctrl_endpoint;
            payload["zmq_data_endpoint"]   = cmd.info.zmq_data_endpoint;
            payload["zmq_pubkey"]          = cmd.info.zmq_pubkey;

            const std::string msg_type   = "REG_REQ";
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
                LOGGER_ERROR("Messenger: register_producer('{}') invalid response.",
                             cmd.channel);
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

    bool handle_command(RegisterConsumerCmd &cmd,
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
            payload["channel_name"]     = cmd.channel;
            payload["consumer_pid"]     = pylabhub::platform::get_pid();
            payload["consumer_hostname"] = "";

            const std::string msg_type   = "CONSUMER_REG_REQ";
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
                LOGGER_ERROR("Messenger: register_consumer('{}') invalid response.",
                             cmd.channel);
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

    bool handle_command(DeregisterConsumerCmd &cmd,
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

            const std::string msg_type   = "CONSUMER_DEREG_REQ";
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

    /// Send DISC_REQ and return the response JSON. Sends a single request.
    /// Returns nullopt if timeout, parse error, or send failure.
    std::optional<nlohmann::json> send_disc_req(zmq::socket_t &socket,
                                                 const std::string &channel,
                                                 int timeout_ms) const
    {
        nlohmann::json payload;
        payload["msg_type"]    = "DISC_REQ";
        payload["channel_name"] = channel;
        const std::string msg_type   = "DISC_REQ";
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
            LOGGER_ERROR("Messenger: discover_producer('{}') invalid response format.",
                         channel);
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

    bool handle_command(DiscoverProducerCmd &cmd,
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
            constexpr int kRetrySliceMs = 500; // per-attempt budget on retry

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
                const int slice = std::min(remaining_ms, kRetrySliceMs);

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
                    if (!resp->contains("shm_name")     || !(*resp)["shm_name"].is_string() ||
                        !resp->contains("schema_hash")  || !(*resp)["schema_hash"].is_string() ||
                        !resp->contains("schema_version") ||
                        !(*resp)["schema_version"].is_number_unsigned())
                    {
                        LOGGER_ERROR("Messenger: discover_producer('{}') missing fields.",
                                     cmd.channel);
                        cmd.result.set_value(std::nullopt);
                        return false;
                    }
                    ConsumerInfo cinfo{};
                    cinfo.shm_name      = (*resp)["shm_name"].get<std::string>();
                    cinfo.schema_hash   =
                        hex_decode_schema_hash((*resp)["schema_hash"].get<std::string>());
                    cinfo.schema_version    = (*resp)["schema_version"].get<uint32_t>();
                    cinfo.has_shared_memory = resp->value("has_shared_memory", false);
                    cinfo.pattern           = pattern_from_wire(
                        resp->value("channel_pattern", std::string("PubSub")));
                    cinfo.zmq_ctrl_endpoint = resp->value("zmq_ctrl_endpoint", "");
                    cinfo.zmq_data_endpoint = resp->value("zmq_data_endpoint", "");
                    cinfo.zmq_pubkey        = resp->value("zmq_pubkey", "");
                    cinfo.consumer_count    = resp->value("consumer_count", uint32_t{0});
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

    bool handle_command(CreateChannelCmd &cmd, std::optional<zmq::socket_t> &socket)
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
            payload["schema_hash"]       = hex_encode_schema_hash(cmd.schema_hash);
            payload["schema_version"]    = cmd.schema_version;
            payload["producer_pid"]      = cmd.producer_pid;
            payload["has_shared_memory"] = cmd.has_shared_memory;
            payload["channel_pattern"]   = pattern_to_wire(cmd.pattern);
            payload["zmq_ctrl_endpoint"] = cmd.zmq_ctrl_endpoint;
            payload["zmq_data_endpoint"] = cmd.zmq_data_endpoint;
            payload["zmq_pubkey"]        = cmd.zmq_pubkey;

            const std::string msg_type   = "REG_REQ";
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

    bool handle_command(ConnectChannelCmd &cmd, std::optional<zmq::socket_t> &socket) const
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
            reg_payload["channel_name"]     = cmd.channel;
            reg_payload["consumer_pid"]     = pylabhub::platform::get_pid();
            reg_payload["consumer_hostname"] = "";
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

    bool handle_command(StopCmd & /*cmd*/, std::optional<zmq::socket_t> &socket)
    {
        if (socket.has_value())
        {
            try { socket->close(); }
            catch (const zmq::error_t &e)
            {
                LOGGER_ERROR("Messenger: Error closing socket on stop: {}", e.what());
            }
            socket.reset();
        }
        m_is_connected.store(false, std::memory_order_release);
        m_heartbeat_channels.clear();
        return true; // signal worker to exit
    }

    // ── Internal helpers ─────────────────────────────────────────────────────

    /// Send one HEARTBEAT_REQ immediately (fire-and-forget, no poll for reply).
    static void send_immediate_heartbeat(zmq::socket_t &socket,
                                         const std::string &channel,
                                         uint64_t producer_pid)
    {
        try
        {
            nlohmann::json hb;
            hb["channel_name"] = channel;
            hb["producer_pid"] = producer_pid;
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
};

// ============================================================================
// Lifecycle state
// ============================================================================
namespace
{
std::atomic<bool> g_hub_initialized{false};
Messenger *g_messenger_instance = nullptr;
} // namespace

// ============================================================================
// Messenger Public Methods
// ============================================================================

Messenger::Messenger() : pImpl(std::make_unique<MessengerImpl>()) {}
Messenger::~Messenger() = default;
Messenger::Messenger(Messenger &&) noexcept = default;
Messenger &Messenger::operator=(Messenger &&) noexcept = default;

bool Messenger::connect(const std::string &endpoint, const std::string &server_key)
{
    std::lock_guard<std::mutex> lock(pImpl->m_connect_mutex);
    if (pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: Already connected.");
        return true;
    }
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        pImpl->start_worker();
    }
    std::promise<bool> promise;
    auto future = promise.get_future();
    pImpl->enqueue(ConnectCmd{endpoint, server_key, std::move(promise)});
    return future.get();
}

void Messenger::disconnect()
{
    std::lock_guard<std::mutex> lock(pImpl->m_connect_mutex);
    if (!pImpl->m_is_connected.load(std::memory_order_acquire)) return;

    std::promise<void> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DisconnectCmd{std::move(promise)});
    future.get();
}

void Messenger::register_producer(const std::string &channel, const ProducerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_producer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(RegisterProducerCmd{channel, info});
}

void Messenger::register_consumer(const std::string &channel, const ConsumerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_consumer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(RegisterConsumerCmd{channel, info});
}

void Messenger::deregister_consumer(const std::string &channel)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: deregister_consumer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(DeregisterConsumerCmd{channel});
}

std::optional<ConsumerInfo> Messenger::discover_producer(const std::string &channel,
                                                          int timeout_ms)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: discover_producer('{}') — worker not started.", channel);
        return std::nullopt;
    }
    std::promise<std::optional<ConsumerInfo>> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DiscoverProducerCmd{channel, timeout_ms, std::move(promise)});
    return future.get();
}

std::optional<ChannelHandle>
Messenger::create_channel(const std::string &channel_name,
                           ChannelPattern     pattern,
                           bool               has_shared_memory,
                           const std::string &schema_hash,
                           uint32_t           schema_version,
                           int                timeout_ms)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("Messenger: create_channel('{}') — not connected.", channel_name);
        return std::nullopt;
    }

    // Generate CurveZMQ keypair for P2C sockets.
    std::array<char, 41> z85_pub{}, z85_sec{};
    if (zmq_curve_keypair(z85_pub.data(), z85_sec.data()) != 0)
    {
        LOGGER_ERROR("Messenger: create_channel('{}') — keypair generation failed.",
                     channel_name);
        return std::nullopt;
    }
    const std::string pubkey(z85_pub.data(), 40);
    const std::string seckey(z85_sec.data(), 40);

    // Bind P2C sockets in the calling thread (sockets will be owned by ChannelHandle).
    zmq::context_t &ctx = get_zmq_context();

    zmq::socket_t ctrl_sock(ctx, zmq::socket_type::router);
    ctrl_sock.set(zmq::sockopt::curve_server, 1);
    ctrl_sock.set(zmq::sockopt::curve_secretkey, seckey);
    ctrl_sock.set(zmq::sockopt::curve_publickey, pubkey);
    ctrl_sock.bind("tcp://127.0.0.1:0");
    const std::string ctrl_endpoint = ctrl_sock.get(zmq::sockopt::last_endpoint);

    std::string data_endpoint;
    zmq::socket_t data_sock(ctx, zmq::socket_type::push); // placeholder
    bool has_data_sock = false;

    if (pattern != ChannelPattern::Bidir)
    {
        zmq::socket_type data_type = (pattern == ChannelPattern::PubSub)
                                         ? zmq::socket_type::xpub
                                         : zmq::socket_type::push;
        data_sock = zmq::socket_t(ctx, data_type);
        data_sock.set(zmq::sockopt::curve_server, 1);
        data_sock.set(zmq::sockopt::curve_secretkey, seckey);
        data_sock.set(zmq::sockopt::curve_publickey, pubkey);
        data_sock.bind("tcp://127.0.0.1:0");
        data_endpoint = data_sock.get(zmq::sockopt::last_endpoint);
        has_data_sock = true;
    }

    // Enqueue REG_REQ to broker via worker thread.
    std::promise<bool> reg_promise;
    auto reg_future = reg_promise.get_future();
    CreateChannelCmd cmd;
    cmd.channel            = channel_name;
    cmd.pattern            = pattern;
    cmd.has_shared_memory  = has_shared_memory;
    cmd.schema_hash        = schema_hash;
    cmd.schema_version     = schema_version;
    cmd.producer_pid       = pylabhub::platform::get_pid();
    cmd.zmq_ctrl_endpoint  = ctrl_endpoint;
    cmd.zmq_data_endpoint  = data_endpoint;
    cmd.zmq_pubkey         = pubkey;
    cmd.timeout_ms         = timeout_ms;
    cmd.result             = std::move(reg_promise);
    pImpl->enqueue(std::move(cmd));

    if (!reg_future.get())
    {
        // Broker rejected — sockets go out of scope and are closed.
        return std::nullopt;
    }

    // Build and return ChannelHandle owning the pre-bound sockets.
    return make_producer_handle(channel_name, pattern, has_shared_memory,
                                std::move(ctrl_sock), std::move(data_sock), has_data_sock);
}

std::optional<ChannelHandle>
Messenger::connect_channel(const std::string &channel_name,
                            int                timeout_ms,
                            const std::string &schema_hash)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("Messenger: connect_channel('{}') — not connected.", channel_name);
        return std::nullopt;
    }

    // Discover and register consumer (done in worker thread via FIFO queue).
    std::promise<std::optional<ConsumerInfo>> cc_promise;
    auto cc_future = cc_promise.get_future();
    ConnectChannelCmd cmd;
    cmd.channel              = channel_name;
    cmd.timeout_ms           = timeout_ms;
    cmd.expected_schema_hash = schema_hash;
    cmd.result               = std::move(cc_promise);
    pImpl->enqueue(std::move(cmd));

    auto cinfo = cc_future.get();
    if (!cinfo.has_value())
    {
        return std::nullopt;
    }

    // Connect P2C sockets in the calling thread using info from DISC_ACK.
    zmq::context_t &ctx = get_zmq_context();

    zmq::socket_t ctrl_sock(ctx, zmq::socket_type::dealer);
    if (!cinfo->zmq_pubkey.empty())
    {
        // Generate a client keypair for the P2C ctrl socket.
        std::array<char, 41> cli_pub{}, cli_sec{};
        if (zmq_curve_keypair(cli_pub.data(), cli_sec.data()) == 0)
        {
            ctrl_sock.set(zmq::sockopt::curve_serverkey, cinfo->zmq_pubkey);
            ctrl_sock.set(zmq::sockopt::curve_publickey, std::string(cli_pub.data(), 40));
            ctrl_sock.set(zmq::sockopt::curve_secretkey, std::string(cli_sec.data(), 40));
        }
    }
    if (!cinfo->zmq_ctrl_endpoint.empty())
    {
        ctrl_sock.connect(cinfo->zmq_ctrl_endpoint);
    }

    bool has_data_sock = !cinfo->zmq_data_endpoint.empty() &&
                         cinfo->pattern != ChannelPattern::Bidir;
    zmq::socket_t data_sock(ctx, zmq::socket_type::pull); // placeholder type

    if (has_data_sock)
    {
        zmq::socket_type data_type = (cinfo->pattern == ChannelPattern::PubSub)
                                         ? zmq::socket_type::sub
                                         : zmq::socket_type::pull;
        data_sock = zmq::socket_t(ctx, data_type);
        if (!cinfo->zmq_pubkey.empty())
        {
            std::array<char, 41> cli_pub{}, cli_sec{};
            if (zmq_curve_keypair(cli_pub.data(), cli_sec.data()) == 0)
            {
                data_sock.set(zmq::sockopt::curve_serverkey, cinfo->zmq_pubkey);
                data_sock.set(zmq::sockopt::curve_publickey,
                              std::string(cli_pub.data(), 40));
                data_sock.set(zmq::sockopt::curve_secretkey,
                              std::string(cli_sec.data(), 40));
            }
        }
        if (cinfo->pattern == ChannelPattern::PubSub)
        {
            // Subscribe to all messages (empty string = subscribe to all).
            data_sock.set(zmq::sockopt::subscribe, "");
        }
        data_sock.connect(cinfo->zmq_data_endpoint);
    }

    // Build and return ChannelHandle owning the connected sockets.
    return make_consumer_handle(channel_name, cinfo->pattern, cinfo->has_shared_memory,
                                std::move(ctrl_sock), std::move(data_sock), has_data_sock);
}

void Messenger::on_channel_closing(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lock(pImpl->m_cb_mutex);
    pImpl->m_channel_closing_cb = std::move(cb);
}

Messenger &Messenger::get_instance()
{
    assert(g_hub_initialized.load(std::memory_order_acquire) &&
           "Messenger::get_instance() called before registration and initialization "
           "through Lifecycle");
    assert(g_messenger_instance != nullptr);
    return *g_messenger_instance;
}

// ============================================================================
// Lifecycle Integration
// ============================================================================
namespace
{
void do_hub_startup(const char * /*arg*/)
{
    if (sodium_init() < 0)
    {
        LOGGER_SYSTEM("Data Exchange Hub: Failed to initialize libsodium!");
        return;
    }
    zmq_context_startup();
    g_messenger_instance = new Messenger();
    g_hub_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module initialized and ready.");
}

void do_hub_shutdown(const char * /*arg*/)
{
    g_hub_initialized.store(false, std::memory_order_release);
    delete g_messenger_instance;
    g_messenger_instance = nullptr;
    zmq_context_shutdown();
    LOGGER_INFO("Data Exchange Hub: Module shut down.");
}
} // namespace

pylabhub::utils::ModuleDef GetLifecycleModule()
{
    pylabhub::utils::ModuleDef module("pylabhub::hub::DataExchangeHub");
    module.add_dependency("CryptoUtils");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_hub_startup);
    module.set_shutdown(&do_hub_shutdown, kHubShutdownTimeoutMs);
    module.set_as_persistent(true);
    return module;
}

bool lifecycle_initialized() noexcept
{
    return g_hub_initialized.load(std::memory_order_acquire);
}

} // namespace pylabhub::hub
