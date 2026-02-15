#include "plh_service.hpp"
#include "utils/message_hub.hpp"

#include "sodium.h"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include <array>
#include <cassert>
#include <vector>
#include <atomic>

namespace pylabhub::hub
{

// ============================================================================
// Constants and Helper Functions
// ============================================================================
namespace
{
constexpr size_t kZ85KeyChars = 40;       // Z85-encoded 32-byte key length
constexpr size_t kSchemaHashHexLen = 64; // hex length for 32 bytes
constexpr size_t kSchemaHashBytes = 32;
constexpr unsigned int kNibbleMask = 0x0FU;
constexpr int kHexLetterOffset = 10; // 'a'-'f' and 'A'-'F' value offset
constexpr size_t kZ85KeyBufSize = 41;    // 40 chars + null
constexpr int kHubShutdownTimeoutMs = 5000;

bool is_valid_z85_key(const std::string &key)
{
    return key.length() == kZ85KeyChars;
}

/** Encodes 32 raw bytes to a 64-char hex string for JSON-safe storage. */
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

/** Decodes a 64-char hex string back to 32 bytes. Returns empty string on error. */
std::string hex_decode_schema_hash(const std::string &hex_str)
{
    auto hex_val = [](char hex_char) -> int {
        if (hex_char >= '0' && hex_char <= '9')
        {
            return hex_char - '0';
        }
        if (hex_char >= 'a' && hex_char <= 'f')
        {
            return hex_char - 'a' + kHexLetterOffset;
        }
        if (hex_char >= 'A' && hex_char <= 'F')
        {
            return hex_char - 'A' + kHexLetterOffset;
        }
        return -1;
    };
    if (hex_str.size() != kSchemaHashHexLen)
    {
        return {};
    }
    std::string out;
    out.reserve(kSchemaHashBytes);
    for (size_t i = 0; i < kSchemaHashHexLen; i += 2)
    {
        int high_val = hex_val(hex_str[i]);
        int low_val = hex_val(hex_str[i + 1]);
        if (high_val < 0 || low_val < 0)
        {
            return {};
        }
        out += static_cast<char>((high_val << 4) | low_val);
    }
    return out;
}
} // namespace

// ============================================================================
// Impl Class Definition
// ============================================================================

class MessageHubImpl
{
  public:
    zmq::context_t m_context;
    zmq::socket_t m_socket;
    std::string m_client_public_key_z85;
    std::string m_client_secret_key_z85;
    std::atomic<bool> m_is_connected{false};
    mutable std::mutex m_socket_mutex; // For thread-safety

    MessageHubImpl() : m_context(1), m_socket(m_context, zmq::socket_type::dealer) {}
};

// ============================================================================
// MessageHub Public Method Implementations
// ============================================================================

MessageHub::MessageHub() : pImpl(std::make_unique<MessageHubImpl>()) {}
MessageHub::~MessageHub()
{
    if (pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        disconnect();
    }
}

// Move constructor and assignment
MessageHub::MessageHub(MessageHub &&other) noexcept = default;
MessageHub &MessageHub::operator=(MessageHub &&other) noexcept = default;

void MessageHub::disconnect()
{
    std::lock_guard<std::mutex> lock(pImpl->m_socket_mutex);

    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        return; // Already disconnected
    }

    try
    {
        pImpl->m_socket.close();
        pImpl->m_is_connected.store(false, std::memory_order_release);
        LOGGER_INFO("MessageHub: Disconnected from broker.");
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: Error during disconnect: {}", e.what());
        pImpl->m_is_connected.store(false, std::memory_order_release);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- endpoint and server_key are semantically distinct
bool MessageHub::connect(const std::string &endpoint, const std::string &server_key)
{
    std::lock_guard<std::mutex> lock(pImpl->m_socket_mutex); // Protect socket operations

    if (!pImpl->m_socket)
    { // operator bool() checks if the socket is valid
        pImpl->m_socket = zmq::socket_t(pImpl->m_context, zmq::socket_type::dealer);
    }

    if (pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_WARN("MessageHub: Already connected.");
        return true;
    }

    if (endpoint.empty())
    {
        LOGGER_ERROR("MessageHub: Broker endpoint cannot be empty.");
        return false;
    }

    if (!is_valid_z85_key(server_key))
    {
        LOGGER_ERROR("MessageHub: Invalid broker public key format.");
        return false;
    }

    try
    {
        // Generate client key pair using zmq_curve_keypair.
        // This abstracts away the direct libsodium calls and Z85 encoding.
        std::array<char, kZ85KeyBufSize> z85_public{};
        std::array<char, kZ85KeyBufSize> z85_secret{};
        if (zmq_curve_keypair(z85_public.data(), z85_secret.data()) != 0)
        {
            LOGGER_ERROR("MessageHub: Failed to generate CurveZMQ key pair.");
            return false;
        }
        pImpl->m_client_public_key_z85 = z85_public.data();
        pImpl->m_client_secret_key_z85 = z85_secret.data();

        pImpl->m_socket.set(zmq::sockopt::curve_serverkey, server_key);
        pImpl->m_socket.set(zmq::sockopt::curve_publickey, pImpl->m_client_public_key_z85);
        pImpl->m_socket.set(zmq::sockopt::curve_secretkey, pImpl->m_client_secret_key_z85);

        pImpl->m_socket.connect(endpoint);
        pImpl->m_is_connected.store(true, std::memory_order_release);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: Failed to connect to broker at {}: {} ({})", endpoint, e.what(),
                     e.num());
        pImpl->m_is_connected.store(false, std::memory_order_release);
        return false;
    }

    LOGGER_INFO("MessageHub: Connection to broker at {} established.", endpoint);
    return true;
}

std::optional<std::string> MessageHub::send_message(const std::string &message_type,
                                                    const std::string &json_payload, int timeout_ms)
{
    std::lock_guard<std::mutex> lock(pImpl->m_socket_mutex);

    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_WARN("MessageHub: Not connected.");
        return std::nullopt;
    }

    try
    {
        // First frame: message type
        // Second frame: JSON payload
        std::vector<zmq::const_buffer> msgs = {zmq::buffer(message_type),
                                               zmq::buffer(json_payload)};
        zmq::send_multipart(pImpl->m_socket, msgs);

        std::vector<zmq::pollitem_t> items = {{pImpl->m_socket, 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(timeout_ms));

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            LOGGER_ERROR("MessageHub: Timeout waiting for broker response ({}ms).", timeout_ms);
            return std::nullopt;
        }

        std::vector<zmq::message_t> recv_msgs;
        auto recv_result =
            zmq::recv_multipart(pImpl->m_socket, std::back_inserter(recv_msgs));
        if (!recv_result)
        {
            LOGGER_ERROR("MessageHub: recv_multipart failed.");
            return std::nullopt;
        }

        if (recv_msgs.size() < 2)
        {
            LOGGER_ERROR("MessageHub: Invalid response format: expected at least 2 frames, got {}.",
                         recv_msgs.size());
            return std::nullopt;
        }

        // We only care about the last frame as the actual response payload
        return recv_msgs.back().to_string();
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: 0MQ error during send_message: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string> MessageHub::receive_message(int timeout_ms)
{
    std::lock_guard<std::mutex> lock(pImpl->m_socket_mutex);

    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        // This is a passive receive, so no error log, just return nullopt
        return std::nullopt;
    }

    try
    {
        std::vector<zmq::pollitem_t> items = {{pImpl->m_socket, 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(timeout_ms));

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            return std::nullopt; // Timeout or no message
        }

        std::vector<zmq::message_t> recv_msgs;
        auto recv_result =
            zmq::recv_multipart(pImpl->m_socket, std::back_inserter(recv_msgs));
        if (!recv_result)
        {
            LOGGER_ERROR("MessageHub: recv_multipart failed.");
            return std::nullopt;
        }

        if (recv_msgs.size() < 2)
        {
            LOGGER_ERROR(
                "MessageHub: Invalid received message format: expected at least 2 frames, got {}.",
                recv_msgs.size());
            return std::nullopt;
        }

        // We only care about the last frame as the actual message payload
        return recv_msgs.back().to_string();
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: 0MQ error during receive_message: {}", e.what());
        return std::nullopt;
    }
}

bool MessageHub::register_producer(const std::string &channel, const ProducerInfo &info)
{
    // Build JSON payload for registration
    nlohmann::json request_payload;
    request_payload["msg_type"] = "REG_REQ";
    // ... add other fields from ProducerInfo to request_payload
    request_payload["channel_name"] = channel;
    request_payload["shm_name"] = info.shm_name;
    request_payload["producer_pid"] = info.producer_pid;
    request_payload["schema_hash"] = hex_encode_schema_hash(info.schema_hash);
    request_payload["schema_version"] = info.schema_version;
    // Add metadata based on DataBlockConfig if needed

    std::optional<std::string> response_str = send_message("REG_REQ", request_payload.dump());

    if (!response_str)
    {
        return false;
    }

    nlohmann::json response_json;
    try
    {
        response_json = nlohmann::json::parse(response_str.value());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("MessageHub: Producer registration response parse failed: {}", e.what());
        return false;
    }
    if (!response_json.contains("status") || response_json["status"] != "success")
    {
        std::string msg = (response_json.contains("message") && response_json["message"].is_string())
                              ? response_json["message"].get<std::string>()
                              : "unknown";
        LOGGER_ERROR("MessageHub: Producer registration failed: {}", msg);
        return false;
    }

    return true;
}

std::optional<ConsumerInfo> MessageHub::discover_producer(const std::string &channel)
{
    // Build JSON payload for discovery
    nlohmann::json request_payload;
    request_payload["msg_type"] = "DISC_REQ";
    request_payload["channel_name"] = channel;
    // Add consumer_pid and secret_hash if available

    std::optional<std::string> response_str = send_message("DISC_REQ", request_payload.dump());

    if (!response_str)
    {
        return std::nullopt;
    }

    nlohmann::json response_json;
    try
    {
        response_json = nlohmann::json::parse(response_str.value());
    }
    catch (const nlohmann::json::exception &e)
    {
        LOGGER_ERROR("MessageHub: Producer discovery response parse failed: {}", e.what());
        return std::nullopt;
    }
    if (!response_json.contains("status") || response_json["status"] != "success")
    {
        std::string msg = (response_json.contains("message") && response_json["message"].is_string())
                              ? response_json["message"].get<std::string>()
                              : "unknown";
        LOGGER_ERROR("MessageHub: Producer discovery failed: {}", msg);
        return std::nullopt;
    }
    if (!response_json.contains("shm_name") || !response_json["shm_name"].is_string() ||
        !response_json.contains("schema_hash") || !response_json["schema_hash"].is_string() ||
        !response_json.contains("schema_version") || !response_json["schema_version"].is_number_unsigned())
    {
        LOGGER_ERROR("MessageHub: Producer discovery response missing required fields (shm_name, schema_hash, schema_version)");
        return std::nullopt;
    }

    ConsumerInfo consumer_info{};
    consumer_info.shm_name = response_json["shm_name"].get<std::string>();
    consumer_info.schema_hash = hex_decode_schema_hash(response_json["schema_hash"].get<std::string>());
    consumer_info.schema_version = response_json["schema_version"].get<uint32_t>();
    // Extract other metadata if available

    return consumer_info;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- keep as member for API consistency
bool MessageHub::register_consumer(const std::string &channel, const ConsumerInfo &info)
{
    (void)channel;
    (void)info;
    // Not yet implemented: consumer registration to broker when protocol is defined. See DATAHUB_TODO.
    return true;
}

MessageHub &MessageHub::get_instance()
{
    static MessageHub instance; // Guaranteed to be destroyed, instantiated on first use.
    return instance;
}

// ============================================================================
// Lifecycle Integration
// ============================================================================
namespace
{
std::atomic<bool> g_hub_initialized{false};

void do_hub_startup(const char *arg)
{
    (void)arg;
    if (sodium_init() < 0)
    {
        LOGGER_SYSTEM("Data Exchange Hub: Failed to initialize libsodium!");
        return;
    }
    g_hub_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module initialized and ready.");
}

void do_hub_shutdown(const char *arg)
{
    (void)arg;
    g_hub_initialized.store(false, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module shut down.");
}
} // namespace

pylabhub::utils::ModuleDef GetLifecycleModule()
{
    pylabhub::utils::ModuleDef module("pylabhub::hub::DataExchangeHub");
    module.add_dependency("CryptoUtils");         // libsodium for checksums/schema hash; init order
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