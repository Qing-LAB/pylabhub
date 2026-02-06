#include "plh_service.hpp"
#include "utils/MessageHub.hpp"

#include "sodium.h"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include <vector>
#include <atomic>

namespace pylabhub::hub
{

// ============================================================================
// Constants and Helper Functions
// ============================================================================
namespace
{
bool is_valid_z85_key(const std::string &key)
{
    // Z85-encoded 32-byte keys are 40 characters long.
    return key.length() == 40;
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

bool MessageHub::connect(const std::string &endpoint, const std::string &server_key)
{
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
        char z85_public[41];
        char z85_secret[41];
        if (zmq_curve_keypair(z85_public, z85_secret) != 0)
        {
            LOGGER_ERROR("MessageHub: Failed to generate CurveZMQ key pair.");
            return false;
        }
        pImpl->m_client_public_key_z85 = z85_public;
        pImpl->m_client_secret_key_z85 = z85_secret;

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

void MessageHub::disconnect()
{
    if (pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        try
        {
            LOGGER_WARN("MessageHub: Disconnecting. Unsent 'fire-and-forget' notifications may be "
                        "discarded.");
            pImpl->m_socket.set(zmq::sockopt::linger, 0); // Discard unsent messages immediately
            pImpl->m_socket.close();
            pImpl->m_is_connected.store(false, std::memory_order_release);
            LOGGER_INFO("MessageHub: Disconnected from broker.");
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("MessageHub: Error during disconnect: {}", e.what());
        }
    }
}

bool MessageHub::send_request(const char *header, const nlohmann::json &payload,
                              nlohmann::json &response, int timeout_ms)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("MessageHub: Not connected.");
        return false;
    }

    try
    {
        std::vector<zmq::const_buffer> request_parts;
        request_parts.emplace_back(header, 16);
        std::vector<uint8_t> msgpack_payload = nlohmann::json::to_msgpack(payload);
        request_parts.emplace_back(msgpack_payload.data(), msgpack_payload.size());

        if (!zmq::send_multipart(pImpl->m_socket, request_parts))
        {
            LOGGER_ERROR("MessageHub: Failed to send request, socket not ready (EAGAIN).");
            return false;
        }

        std::vector<zmq::pollitem_t> items = {{pImpl->m_socket, 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds(timeout_ms));

        if (!(items[0].revents & ZMQ_POLLIN))
        {
            LOGGER_ERROR("MessageHub: Timeout waiting for broker response ({}ms).", timeout_ms);
            return false;
        }

        std::vector<zmq::message_t> response_parts;
        auto result = zmq::recv_multipart(pImpl->m_socket, std::back_inserter(response_parts),
                                          zmq::recv_flags::dontwait);

        if (!result || *result < 2)
        {
            LOGGER_ERROR("MessageHub: Invalid response from broker: expected 2 parts, got {}.",
                         result.value_or(0));
            return false;
        }

        const auto &payload_part = response_parts.back();
        response =
            nlohmann::json::from_msgpack(payload_part.data<const uint8_t>(),
                                         payload_part.data<const uint8_t>() + payload_part.size());
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: 0MQ error during send_request: {}", e.what());
        return false;
    }
    catch (const nlohmann::json::parse_error &e)
    {
        LOGGER_ERROR("MessageHub: Failed to parse response payload: {}", e.what());
        return false;
    }

    return true;
}

bool MessageHub::send_notification(const char *header, const nlohmann::json &payload)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("MessageHub: Not connected.");
        return false;
    }

    try
    {
        std::vector<zmq::const_buffer> request_parts;
        request_parts.emplace_back(header, 16);
        std::vector<uint8_t> msgpack_payload = nlohmann::json::to_msgpack(payload);
        request_parts.emplace_back(msgpack_payload.data(), msgpack_payload.size());

        return zmq::send_multipart(pImpl->m_socket, request_parts).has_value();
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("MessageHub: 0MQ error during send_notification: {}", e.what());
        return false;
    }
}

// ============================================================================
// Lifecycle Integration
// ============================================================================
namespace
{
static std::atomic<bool> g_hub_initialized{false};

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
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_hub_startup);
    module.set_shutdown(&do_hub_shutdown, 5000); // Add 5000ms timeout
    module.set_as_persistent(true);
    return module;
}

bool lifecycle_initialized() noexcept
{
    return g_hub_initialized.load(std::memory_order_acquire);
}

} // namespace pylabhub::hub