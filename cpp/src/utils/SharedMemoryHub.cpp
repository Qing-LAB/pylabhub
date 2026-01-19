/*******************************************************************************
 * @file SharedMemoryHub.cpp
 * @brief Implementation of the Data Exchange Hub.
 *
 * This implements the Data Exchange Hub framework as specified in HEP core-0002.
 ******************************************************************************/

#include "utils/SharedMemoryHub.hpp"
#include "platform.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <zmq.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <synchapi.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace pylabhub::hub
{

namespace
{
std::atomic<bool> g_hub_initialized{false};
} // namespace

// Forward declaration
bool validate_broker_public_key(const std::string &key);

// ============================================================================
// Impl Class Definitions
// ============================================================================

class HubImpl
{
  public:
    BrokerConfig m_config;
    void *m_context{nullptr};       // ZeroMQ context
    void *m_broker_socket{nullptr}; // ZeroMQ socket for broker communication
    std::string m_client_secret_key;
    std::string m_client_public_key;
    std::atomic<bool> m_running{false};
    std::thread m_heartbeat_thread;

    bool initialize(const BrokerConfig &config);
    void start_heartbeat_thread();
    void stop_heartbeat_thread();
    bool send_heartbeat();

    // Broker protocol methods
    bool register_channel(const std::string &channel_type, const std::string &channel_name,
                          const nlohmann::json &metadata);
    bool discover_channel(const std::string &channel_type, const std::string &channel_name,
                          nlohmann::json &metadata);
    bool send_broker_message(const nlohmann::json &request, nlohmann::json &response,
                             uint32_t timeout_ms = 5000);
};

class SharedMemoryProducerImpl
{
  public:
    void *m_mapped_memory{nullptr};
    SharedMemoryHeader *m_header{nullptr};
    size_t m_size{0};
    size_t m_data_size{0};
    std::string m_name;
    Hub *m_hub{nullptr};

#if defined(PYLABHUB_PLATFORM_WIN64)
    HANDLE m_file_handle{nullptr};
    HANDLE m_mutex{nullptr};
    HANDLE m_event{nullptr};
#else
    int m_shm_fd{-1};
#endif

    bool initialize(const std::string &name, size_t size, Hub *hub);
    void cleanup();
};

class SharedMemoryConsumerImpl
{
  public:
    void *m_mapped_memory{nullptr};
    const SharedMemoryHeader *m_header{nullptr};
    size_t m_size{0};
    size_t m_data_size{0};
    std::string m_name;

#if defined(PYLABHUB_PLATFORM_WIN64)
    HANDLE m_file_handle{nullptr};
    HANDLE m_mutex{nullptr};
    HANDLE m_event{nullptr};
#else
    int m_shm_fd{-1};
#endif

    bool initialize(const std::string &name, Hub *hub, size_t size = 0);
    void cleanup();
};

class ZmqPublisherImpl
{
  public:
    void *m_socket{nullptr};
    std::string m_service_name;
    Hub *m_hub{nullptr};

    bool initialize(const std::string &service_name, Hub *hub);
};

class ZmqSubscriberImpl
{
  public:
    void *m_socket{nullptr};
    std::string m_service_name;
    Hub *m_hub{nullptr};

    bool initialize(const std::string &service_name, Hub *hub);
};

class ZmqRequestServerImpl
{
  public:
    void *m_socket{nullptr};
    std::string m_service_name;
    Hub *m_hub{nullptr};

    bool initialize(const std::string &service_name, Hub *hub);
};

class ZmqRequestClientImpl
{
  public:
    void *m_socket{nullptr};
    std::string m_service_name;
    Hub *m_hub{nullptr};

    bool initialize(const std::string &service_name, Hub *hub);
};

// ============================================================================
// Hub Implementation
// ============================================================================

Hub::Hub() : pImpl(std::make_unique<HubImpl>()) {}

std::unique_ptr<Hub> Hub::connect(const BrokerConfig &config)
{
    auto hub = std::unique_ptr<Hub>(new Hub());
    if (!hub->pImpl->initialize(config))
    {
        LOGGER_ERROR("Hub: Failed to connect to broker at {}", config.endpoint);
        return nullptr;
    }
    hub->pImpl->start_heartbeat_thread();
    return hub;
}

Hub::~Hub()
{
    if (pImpl)
    {
        pImpl->stop_heartbeat_thread();
    }
}

Hub::Hub(Hub &&other) noexcept : pImpl(std::move(other.pImpl)) {}

Hub &Hub::operator=(Hub &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

bool HubImpl::initialize(const BrokerConfig &config)
{
    // Input validation
    if (config.endpoint.empty())
    {
        LOGGER_ERROR("Hub: Broker endpoint cannot be empty");
        return false;
    }

    if (!validate_broker_public_key(config.broker_public_key))
    {
        LOGGER_ERROR("Hub: Invalid broker public key format");
        return false;
    }

    if (config.heartbeat_interval_ms == 0)
    {
        LOGGER_ERROR("Hub: Heartbeat interval must be greater than 0");
        return false;
    }

    m_config = config;

    // Create ZeroMQ context
    m_context = zmq_ctx_new();
    if (!m_context)
    {
        LOGGER_ERROR("Hub: Failed to create ZeroMQ context: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    // Create DEALER socket for broker communication
    m_broker_socket = zmq_socket(m_context, ZMQ_DEALER);
    if (!m_broker_socket)
    {
        LOGGER_ERROR("Hub: Failed to create broker socket: {}", zmq_strerror(zmq_errno()));
        zmq_ctx_destroy(m_context);
        m_context = nullptr;
        return false;
    }

    // Generate client key pair for CurveZMQ
    unsigned char client_public_key[32];
    unsigned char client_secret_key[32];
    if (crypto_box_keypair(client_public_key, client_secret_key) != 0)
    {
        LOGGER_ERROR("Hub: Failed to generate CurveZMQ key pair");
        zmq_close(m_broker_socket);
        zmq_ctx_destroy(m_context);
        m_broker_socket = nullptr;
        m_context = nullptr;
        return false;
    }

    // Convert keys to Z85 format for ZeroMQ
    char z85_public[41];
    char z85_secret[41];
    zmq_z85_encode(z85_public, client_public_key, 32);
    zmq_z85_encode(z85_secret, client_secret_key, 32);
    m_client_public_key = std::string(z85_public);
    m_client_secret_key = std::string(z85_secret);

    // Set client's secret key
    if (zmq_setsockopt(m_broker_socket, ZMQ_CURVE_SECRETKEY, z85_secret, 41) != 0)
    {
        LOGGER_ERROR("Hub: Failed to set client secret key: {}", zmq_strerror(zmq_errno()));
        zmq_close(m_broker_socket);
        zmq_ctx_destroy(m_context);
        m_broker_socket = nullptr;
        m_context = nullptr;
        return false;
    }

    // Set client's public key
    if (zmq_setsockopt(m_broker_socket, ZMQ_CURVE_PUBLICKEY, z85_public, 41) != 0)
    {
        LOGGER_ERROR("Hub: Failed to set client public key: {}", zmq_strerror(zmq_errno()));
        zmq_close(m_broker_socket);
        zmq_ctx_destroy(m_context);
        m_broker_socket = nullptr;
        m_context = nullptr;
        return false;
    }

    // Set server's public key (broker's public key)
    if (zmq_setsockopt(m_broker_socket, ZMQ_CURVE_SERVERKEY, m_config.broker_public_key.c_str(),
                       m_config.broker_public_key.length()) != 0)
    {
        LOGGER_ERROR("Hub: Failed to set broker public key: {}", zmq_strerror(zmq_errno()));
        zmq_close(m_broker_socket);
        zmq_ctx_destroy(m_context);
        m_broker_socket = nullptr;
        m_context = nullptr;
        return false;
    }

    // Connect to broker endpoint
    if (zmq_connect(m_broker_socket, m_config.endpoint.c_str()) != 0)
    {
        LOGGER_ERROR("Hub: Failed to connect to broker at {}: {}", m_config.endpoint,
                     zmq_strerror(zmq_errno()));
        zmq_close(m_broker_socket);
        zmq_ctx_destroy(m_context);
        m_broker_socket = nullptr;
        m_context = nullptr;
        return false;
    }

    LOGGER_INFO("Hub: Initialized connection to broker at {}", m_config.endpoint);
    return true;
}

void HubImpl::start_heartbeat_thread()
{
    m_running.store(true);
    m_heartbeat_thread = std::thread(
        [this]()
        {
            while (m_running.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(m_config.heartbeat_interval_ms));
                if (m_running.load(std::memory_order_acquire))
                {
                    send_heartbeat();
                }
            }
        });
}

void HubImpl::stop_heartbeat_thread()
{
    m_running.store(false, std::memory_order_release);
    if (m_heartbeat_thread.joinable())
    {
        m_heartbeat_thread.join();
    }
    if (m_broker_socket)
    {
        zmq_close(m_broker_socket);
        m_broker_socket = nullptr;
    }
    if (m_context)
    {
        zmq_ctx_destroy(m_context);
        m_context = nullptr;
    }
}

bool HubImpl::send_heartbeat()
{
    if (!m_broker_socket)
        return false;

    // Send heartbeat message as JSON
    nlohmann::json heartbeat_msg = {{"type", "heartbeat"},
                                    {"client_public_key", m_client_public_key}};

    std::string msg_str = heartbeat_msg.dump();
    int rc = zmq_send(m_broker_socket, msg_str.c_str(), msg_str.size(), ZMQ_DONTWAIT);
    if (rc < 0)
    {
        if (zmq_errno() != EAGAIN)
        {
            LOGGER_ERROR("Hub: Failed to send heartbeat: {}", zmq_strerror(zmq_errno()));
            return false;
        }
    }
    return true;
}

bool HubImpl::send_broker_message(const nlohmann::json &request, nlohmann::json &response,
                                  uint32_t timeout_ms)
{
    if (!m_broker_socket)
    {
        LOGGER_ERROR("Hub: Broker socket not initialized");
        return false;
    }

    // Serialize request to JSON string
    std::string request_str = request.dump();

    // Send request
    int rc = zmq_send(m_broker_socket, request_str.c_str(), request_str.size(), 0);
    if (rc < 0)
    {
        LOGGER_ERROR("Hub: Failed to send broker message: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    // Set receive timeout
    int timeout = static_cast<int>(timeout_ms);
    zmq_setsockopt(m_broker_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    // Receive response
    zmq_msg_t response_msg;
    zmq_msg_init(&response_msg);
    rc = zmq_msg_recv(&response_msg, m_broker_socket, 0);
    if (rc < 0)
    {
        zmq_msg_close(&response_msg);
        if (zmq_errno() == EAGAIN)
        {
            LOGGER_ERROR("Hub: Broker message timeout after {} ms", timeout_ms);
        }
        else
        {
            LOGGER_ERROR("Hub: Failed to receive broker response: {}", zmq_strerror(zmq_errno()));
        }
        return false;
    }

    // Parse response JSON
    try
    {
        std::string response_str(static_cast<const char *>(zmq_msg_data(&response_msg)),
                                 zmq_msg_size(&response_msg));
        response = nlohmann::json::parse(response_str);
    }
    catch (const nlohmann::json::exception &e)
    {
        zmq_msg_close(&response_msg);
        LOGGER_ERROR("Hub: Failed to parse broker response JSON: {}", e.what());
        return false;
    }

    zmq_msg_close(&response_msg);
    return true;
}

bool HubImpl::register_channel(const std::string &channel_type, const std::string &channel_name,
                               const nlohmann::json &metadata)
{
    nlohmann::json request = {{"type", "register"},
                              {"channel_type", channel_type},
                              {"channel_name", channel_name},
                              {"client_public_key", m_client_public_key},
                              {"metadata", metadata}};

    nlohmann::json response;
    if (!send_broker_message(request, response, 5000))
    {
        LOGGER_ERROR("Hub: Failed to register channel '{}' of type '{}'", channel_name,
                     channel_type);
        return false;
    }

    // Check response
    if (!response.is_object())
    {
        LOGGER_ERROR("Hub: Broker response is not a JSON object");
        return false;
    }

    if (!response.contains("status"))
    {
        LOGGER_ERROR("Hub: Broker response missing 'status' field");
        return false;
    }

    if (response["status"] != "ok")
    {
        std::string error_msg = response.value("error", "Unknown error");
        LOGGER_ERROR("Hub: Broker rejected channel registration '{}': {}", channel_name, error_msg);
        return false;
    }

    LOGGER_INFO("Hub: Successfully registered channel '{}' of type '{}'", channel_name,
                channel_type);
    return true;
}

bool HubImpl::discover_channel(const std::string &channel_type, const std::string &channel_name,
                               nlohmann::json &metadata)
{
    nlohmann::json request = {{"type", "discover"},
                              {"channel_type", channel_type},
                              {"channel_name", channel_name},
                              {"client_public_key", m_client_public_key}};

    nlohmann::json response;
    if (!send_broker_message(request, response, 5000))
    {
        LOGGER_ERROR("Hub: Failed to discover channel '{}' of type '{}'", channel_name,
                     channel_type);
        return false;
    }

    // Check response
    if (!response.is_object())
    {
        LOGGER_ERROR("Hub: Broker response is not a JSON object");
        return false;
    }

    if (!response.contains("status"))
    {
        LOGGER_ERROR("Hub: Broker response missing 'status' field");
        return false;
    }

    if (response["status"] != "ok")
    {
        std::string error_msg = response.value("error", "Channel not found");
        LOGGER_ERROR("Hub: Channel '{}' not found: {}", channel_name, error_msg);
        return false;
    }

    // Extract metadata
    if (response.contains("metadata"))
    {
        metadata = response["metadata"];
    }
    else
    {
        metadata = nlohmann::json::object();
    }

    LOGGER_INFO("Hub: Successfully discovered channel '{}' of type '{}'", channel_name,
                channel_type);
    return true;
}

// Input validation helpers (in namespace scope for access from HubImpl)
namespace
{
bool validate_channel_name(const std::string &name)
{
    if (name.empty())
    {
        LOGGER_ERROR("Hub: Channel name cannot be empty");
        return false;
    }

    if (name.length() > 255)
    {
        LOGGER_ERROR("Hub: Channel name too long (max 255 characters): {}", name.length());
        return false;
    }

    // Allow alphanumeric, underscores, and hyphens
    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
        {
            LOGGER_ERROR("Hub: Invalid character in channel name '{}': '{}' (only alphanumeric, "
                         "underscore, and hyphen allowed)",
                         name, c);
            return false;
        }
    }

    return true;
}
} // namespace

bool validate_broker_public_key(const std::string &key)
{
    // CurveZMQ public keys are Z85-encoded, 40 characters + null terminator = 41
    if (key.length() != 40)
    {
        LOGGER_ERROR("Hub: Invalid broker public key length: {} (expected 40 for Z85 encoding)",
                     key.length());
        return false;
    }

    // Validate Z85 encoding (base85: 0-9, A-Z, a-z, ., -, :, +, =, ^, !, /, *, ?, &, <, >, (, ), [,
    // ], {, }, @, %, $, #)
    for (char c : key)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '.' || c == '-' || c == ':' || c == '+' || c == '=' || c == '^' || c == '!' ||
              c == '/' || c == '*' || c == '?' || c == '&' || c == '<' || c == '>' || c == '(' ||
              c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '@' || c == '%' ||
              c == '$' || c == '#'))
        {
            LOGGER_ERROR("Hub: Invalid character in broker public key (not valid Z85)");
            return false;
        }
    }

    return true;
}

namespace
{
constexpr size_t MAX_SHARED_MEMORY_SIZE = 1024ULL * 1024ULL * 1024ULL; // 1GB max
constexpr size_t MIN_SHARED_MEMORY_SIZE = sizeof(SharedMemoryHeader);
} // namespace

std::unique_ptr<SharedMemoryProducer> Hub::create_shm_producer(const std::string &name, size_t size)
{
    // Input validation
    if (!validate_channel_name(name))
    {
        return nullptr;
    }

    if (size < MIN_SHARED_MEMORY_SIZE)
    {
        LOGGER_ERROR("Hub: Shared memory size must be at least {} bytes, got {}",
                     MIN_SHARED_MEMORY_SIZE, size);
        return nullptr;
    }

    if (size > MAX_SHARED_MEMORY_SIZE)
    {
        LOGGER_ERROR("Hub: Shared memory size too large: {} (max {} bytes)", size,
                     MAX_SHARED_MEMORY_SIZE);
        return nullptr;
    }

    auto producer = std::unique_ptr<SharedMemoryProducer>(new SharedMemoryProducer());
    if (!producer->pImpl->initialize(name, size, this))
    {
        return nullptr;
    }

    // Register with broker
    nlohmann::json metadata = {
        {"channel_type", "shared_memory"}, {"size", size}, {"shm_name", "/pylabhub_shm_" + name}};

    if (!pImpl->register_channel("shared_memory", name, metadata))
    {
        // Registration failed, but producer is already created
        // Clean up and return nullptr
        producer.reset();
        return nullptr;
    }

    LOGGER_INFO("Hub: Created shared memory producer '{}'", name);
    return producer;
}

std::unique_ptr<SharedMemoryConsumer> Hub::find_shm_consumer(const std::string &name)
{
    // Query broker for channel details
    nlohmann::json metadata;
    if (!pImpl->discover_channel("shared_memory", name, metadata))
    {
        LOGGER_ERROR("Hub: Failed to discover shared memory channel '{}'", name);
        return nullptr;
    }

    // Extract shared memory segment name from metadata
    std::string shm_name = metadata.value("shm_name", "/pylabhub_shm_" + name);
    size_t size = metadata.value("size", 0ULL);

    if (size == 0)
    {
        LOGGER_ERROR("Hub: Invalid size in channel metadata for '{}'", name);
        return nullptr;
    }

    auto consumer = std::unique_ptr<SharedMemoryConsumer>(new SharedMemoryConsumer());
    if (!consumer->pImpl->initialize(name, this, size))
    {
        return nullptr;
    }

    LOGGER_INFO("Hub: Found shared memory consumer '{}'", name);
    return consumer;
}

std::unique_ptr<ZmqPublisher> Hub::create_publisher(const std::string &service_name)
{
    // Input validation
    if (!validate_channel_name(service_name))
    {
        return nullptr;
    }

    auto publisher = std::unique_ptr<ZmqPublisher>(new ZmqPublisher());
    if (!publisher->pImpl->initialize(service_name, this))
    {
        return nullptr;
    }

    // Get the actual endpoint for registration
    char actual_endpoint[256];
    size_t endpoint_len = sizeof(actual_endpoint);
    std::string endpoint_str;
    if (publisher->pImpl->m_socket && zmq_getsockopt(publisher->pImpl->m_socket, ZMQ_LAST_ENDPOINT,
                                                     actual_endpoint, &endpoint_len) == 0)
    {
        endpoint_str = std::string(actual_endpoint);
    }

    // Register with broker
    nlohmann::json metadata = {
        {"channel_type", "zmq_pub_sub"}, {"pattern", "publish"}, {"endpoint", endpoint_str}};

    if (!pImpl->register_channel("zmq_pub_sub", service_name, metadata))
    {
        publisher.reset();
        return nullptr;
    }

    LOGGER_INFO("Hub: Created publisher '{}'", service_name);
    return publisher;
}

std::unique_ptr<ZmqSubscriber> Hub::find_subscriber(const std::string &service_name)
{
    // Query broker for publisher endpoint
    nlohmann::json metadata;
    if (!pImpl->discover_channel("zmq_pub_sub", service_name, metadata))
    {
        LOGGER_ERROR("Hub: Failed to discover publisher '{}'", service_name);
        return nullptr;
    }

    std::string endpoint = metadata.value("endpoint", "");
    if (endpoint.empty())
    {
        LOGGER_ERROR("Hub: No endpoint found in metadata for '{}'", service_name);
        return nullptr;
    }

    auto subscriber = std::unique_ptr<ZmqSubscriber>(new ZmqSubscriber());
    if (!subscriber->pImpl->initialize(service_name, this))
    {
        return nullptr;
    }

    // Connect to discovered endpoint
    if (subscriber->pImpl->m_socket &&
        zmq_connect(subscriber->pImpl->m_socket, endpoint.c_str()) != 0)
    {
        LOGGER_ERROR("ZmqSubscriber: Failed to connect to endpoint {}: {}", endpoint,
                     zmq_strerror(zmq_errno()));
        subscriber.reset();
        return nullptr;
    }

    LOGGER_INFO("Hub: Found subscriber '{}' connected to {}", service_name, endpoint);
    return subscriber;
}

std::unique_ptr<ZmqRequestServer> Hub::create_req_server(const std::string &service_name)
{
    // Input validation
    if (!validate_channel_name(service_name))
    {
        return nullptr;
    }

    auto server = std::unique_ptr<ZmqRequestServer>(new ZmqRequestServer());
    if (!server->pImpl->initialize(service_name, this))
    {
        return nullptr;
    }

    // Get the actual endpoint for registration
    char actual_endpoint[256];
    size_t endpoint_len = sizeof(actual_endpoint);
    std::string endpoint_str;
    if (server->pImpl->m_socket && zmq_getsockopt(server->pImpl->m_socket, ZMQ_LAST_ENDPOINT,
                                                  actual_endpoint, &endpoint_len) == 0)
    {
        endpoint_str = std::string(actual_endpoint);
    }

    // Register with broker
    nlohmann::json metadata = {
        {"channel_type", "zmq_req_rep"}, {"pattern", "server"}, {"endpoint", endpoint_str}};

    if (!pImpl->register_channel("zmq_req_rep", service_name, metadata))
    {
        server.reset();
        return nullptr;
    }

    LOGGER_INFO("Hub: Created request server '{}'", service_name);
    return server;
}

std::unique_ptr<ZmqRequestClient> Hub::find_req_client(const std::string &service_name)
{
    // Input validation
    if (!validate_channel_name(service_name))
    {
        return nullptr;
    }

    // Query broker for server endpoint
    nlohmann::json metadata;
    if (!pImpl->discover_channel("zmq_req_rep", service_name, metadata))
    {
        LOGGER_ERROR("Hub: Failed to discover request server '{}'", service_name);
        return nullptr;
    }

    std::string endpoint = metadata.value("endpoint", "");
    if (endpoint.empty())
    {
        LOGGER_ERROR("Hub: No endpoint found in metadata for '{}'", service_name);
        return nullptr;
    }

    auto client = std::unique_ptr<ZmqRequestClient>(new ZmqRequestClient());
    if (!client->pImpl->initialize(service_name, this))
    {
        return nullptr;
    }

    // Connect to discovered endpoint
    if (client->pImpl->m_socket && zmq_connect(client->pImpl->m_socket, endpoint.c_str()) != 0)
    {
        LOGGER_ERROR("ZmqRequestClient: Failed to connect to endpoint {}: {}", endpoint,
                     zmq_strerror(zmq_errno()));
        client.reset();
        return nullptr;
    }

    LOGGER_INFO("Hub: Found request client '{}' connected to {}", service_name, endpoint);
    return client;
}

void *Hub::get_context() const
{
    return pImpl ? pImpl->m_context : nullptr;
}

// ============================================================================
// SharedMemoryProducer Implementation
// ============================================================================

SharedMemoryProducer::SharedMemoryProducer() : pImpl(std::make_unique<SharedMemoryProducerImpl>())
{
}

SharedMemoryProducer::~SharedMemoryProducer()
{
    if (pImpl)
    {
        pImpl->cleanup();
    }
}

SharedMemoryProducer::SharedMemoryProducer(SharedMemoryProducer &&other) noexcept
    : pImpl(std::move(other.pImpl))
{
}

SharedMemoryProducer &SharedMemoryProducer::operator=(SharedMemoryProducer &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

size_t SharedMemoryProducer::buffer_size() const noexcept
{
    return pImpl ? pImpl->m_data_size : 0;
}

void *SharedMemoryProducer::begin_publish()
{
    if (!pImpl || !pImpl->m_header)
        return nullptr;

#if defined(PYLABHUB_PLATFORM_WIN64)
    if (pImpl->m_mutex)
    {
        WaitForSingleObject(pImpl->m_mutex, INFINITE);
    }
#else
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(pImpl->m_header->mutex_storage);
    pthread_mutex_lock(mutex);
#endif

    pImpl->m_header->is_writing.store(1, std::memory_order_release);
    pImpl->m_header->data_ready.store(0, std::memory_order_release);

    return reinterpret_cast<char *>(pImpl->m_header) + sizeof(SharedMemoryHeader);
}

void SharedMemoryProducer::end_publish(uint64_t data_size, double timestamp,
                                       uint32_t data_type_hash, const uint64_t dimensions[4])
{
    if (!pImpl || !pImpl->m_header)
        return;

    pImpl->m_header->data_size = data_size;
    pImpl->m_header->timestamp = timestamp;
    pImpl->m_header->data_type_hash = data_type_hash;
    std::memcpy(pImpl->m_header->dimensions, dimensions, sizeof(pImpl->m_header->dimensions));

    // Fix race condition: use atomic fetch_add instead of load+store
    pImpl->m_header->frame_id.fetch_add(1, std::memory_order_acq_rel);
    pImpl->m_header->is_writing.store(0, std::memory_order_release);
    pImpl->m_header->data_ready.store(1, std::memory_order_release);

#if defined(PYLABHUB_PLATFORM_WIN64)
    if (pImpl->m_event)
    {
        SetEvent(pImpl->m_event);
    }
    if (pImpl->m_mutex)
    {
        ReleaseMutex(pImpl->m_mutex);
    }
#else
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(pImpl->m_header->condition_storage);
    pthread_cond_broadcast(cond);
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(pImpl->m_header->mutex_storage);
    pthread_mutex_unlock(mutex);
#endif
}

void SharedMemoryProducerImpl::cleanup()
{
    if (m_mapped_memory)
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        UnmapViewOfFile(m_mapped_memory);
        if (m_event)
        {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        if (m_mutex)
        {
            CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
        if (m_file_handle)
        {
            CloseHandle(m_file_handle);
            m_file_handle = nullptr;
        }
        // Note: On Windows, the file mapping is automatically cleaned up when
        // the last handle is closed. We don't need to explicitly delete it.
#else
        munmap(m_mapped_memory, m_size);
        if (m_shm_fd >= 0)
        {
            close(m_shm_fd);
            m_shm_fd = -1;
        }
        // Resource cleanup improvement: Only unlink if we're the last reference
        // For now, we unlink on cleanup. In a full implementation, we would use
        // reference counting or a separate cleanup mechanism that doesn't break
        // active consumers. The broker could track references and handle cleanup.
        //
        // TODO: Implement reference counting or broker-managed cleanup
        shm_unlink(("/pylabhub_shm_" + m_name).c_str());
        LOGGER_INFO("SharedMemoryProducer: Unlinked shared memory segment '{}'", m_name);
#endif
        m_mapped_memory = nullptr;
        m_header = nullptr;
    }
}

bool SharedMemoryProducerImpl::initialize(const std::string &name, size_t size, Hub *hub)
{
    m_name = name;
    m_size = size;
    m_hub = hub;
    m_data_size = size - sizeof(SharedMemoryHeader);

    std::string shm_name = "/pylabhub_shm_" + m_name;

#if defined(PYLABHUB_PLATFORM_WIN64)
    std::string win_name = "Global\\pylabhub_shm_" + m_name;
    m_file_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(size), win_name.c_str());
    if (!m_file_handle)
    {
        LOGGER_ERROR("SharedMemoryProducer: CreateFileMapping failed");
        return false;
    }

    m_mapped_memory = MapViewOfFile(m_file_handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!m_mapped_memory)
    {
        CloseHandle(m_file_handle);
        m_file_handle = nullptr;
        return false;
    }

    // Initialize synchronization primitives for Windows
    std::memset(m_mapped_memory, 0, sizeof(SharedMemoryHeader));
    std::string mutex_name = "Global\\pylabhub_shm_mutex_" + m_name;
    std::string event_name = "Global\\pylabhub_shm_event_" + m_name;

    m_mutex = CreateMutexA(nullptr, FALSE, mutex_name.c_str());
    if (!m_mutex)
    {
        LOGGER_ERROR("SharedMemoryProducer: CreateMutex failed");
        UnmapViewOfFile(m_mapped_memory);
        CloseHandle(m_file_handle);
        m_mapped_memory = nullptr;
        m_file_handle = nullptr;
        return false;
    }

    m_event = CreateEventA(nullptr, TRUE, FALSE, event_name.c_str());
    if (!m_event)
    {
        LOGGER_ERROR("SharedMemoryProducer: CreateEvent failed");
        CloseHandle(m_mutex);
        UnmapViewOfFile(m_mapped_memory);
        CloseHandle(m_file_handle);
        m_mutex = nullptr;
        m_mapped_memory = nullptr;
        m_file_handle = nullptr;
        return false;
    }
#else
    m_shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (m_shm_fd < 0)
    {
        LOGGER_ERROR("SharedMemoryProducer: shm_open failed: {}", strerror(errno));
        return false;
    }

    if (ftruncate(m_shm_fd, size) < 0)
    {
        close(m_shm_fd);
        shm_unlink(shm_name.c_str());
        m_shm_fd = -1;
        LOGGER_ERROR("SharedMemoryProducer: ftruncate failed: {}", strerror(errno));
        return false;
    }

    m_mapped_memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
    if (m_mapped_memory == MAP_FAILED)
    {
        close(m_shm_fd);
        shm_unlink(shm_name.c_str());
        m_shm_fd = -1;
        LOGGER_ERROR("SharedMemoryProducer: mmap failed: {}", strerror(errno));
        return false;
    }

    // Initialize synchronization primitives
    std::memset(m_mapped_memory, 0, sizeof(SharedMemoryHeader));
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(
        reinterpret_cast<SharedMemoryHeader *>(m_mapped_memory)->mutex_storage);
    if (pthread_mutex_init(mutex, &mutex_attr) != 0)
    {
        munmap(m_mapped_memory, size);
        close(m_shm_fd);
        shm_unlink(shm_name.c_str());
        m_shm_fd = -1;
        pthread_mutexattr_destroy(&mutex_attr);
        LOGGER_ERROR("SharedMemoryProducer: pthread_mutex_init failed");
        return false;
    }
    pthread_mutexattr_destroy(&mutex_attr);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_t *cond = reinterpret_cast<pthread_cond_t *>(
        reinterpret_cast<SharedMemoryHeader *>(m_mapped_memory)->condition_storage);
    if (pthread_cond_init(cond, &cond_attr) != 0)
    {
        pthread_mutex_t *mutex = reinterpret_cast<pthread_mutex_t *>(
            reinterpret_cast<SharedMemoryHeader *>(m_mapped_memory)->mutex_storage);
        pthread_mutex_destroy(mutex);
        munmap(m_mapped_memory, size);
        close(m_shm_fd);
        shm_unlink(shm_name.c_str());
        m_shm_fd = -1;
        pthread_condattr_destroy(&cond_attr);
        LOGGER_ERROR("SharedMemoryProducer: pthread_cond_init failed");
        return false;
    }
    pthread_condattr_destroy(&cond_attr);
#endif

    m_header = reinterpret_cast<SharedMemoryHeader *>(m_mapped_memory);
    return true;
}

// ============================================================================
// SharedMemoryConsumer Implementation
// ============================================================================

SharedMemoryConsumer::SharedMemoryConsumer() : pImpl(std::make_unique<SharedMemoryConsumerImpl>())
{
}

SharedMemoryConsumer::~SharedMemoryConsumer()
{
    if (pImpl)
    {
        pImpl->cleanup();
    }
}

SharedMemoryConsumer::SharedMemoryConsumer(SharedMemoryConsumer &&other) noexcept
    : pImpl(std::move(other.pImpl))
{
}

SharedMemoryConsumer &SharedMemoryConsumer::operator=(SharedMemoryConsumer &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

const SharedMemoryHeader *SharedMemoryConsumer::header() const noexcept
{
    return pImpl ? pImpl->m_header : nullptr;
}

size_t SharedMemoryConsumer::buffer_size() const noexcept
{
    return pImpl ? pImpl->m_data_size : 0;
}

const void *SharedMemoryConsumer::consume(uint32_t timeout_ms)
{
    if (!pImpl || !pImpl->m_header)
        return nullptr;

    // Fix race condition: lock BEFORE checking data_ready
#if defined(PYLABHUB_PLATFORM_WIN64)
    if (!pImpl->m_mutex)
    {
        return nullptr;
    }

    // Acquire mutex first
    DWORD wait_result =
        WaitForSingleObject(pImpl->m_mutex, timeout_ms == 0 ? INFINITE : timeout_ms);
    if (wait_result != WAIT_OBJECT_0)
    {
        return nullptr; // Timeout or error acquiring mutex
    }

    // Calculate timeout deadline if timeout is specified
    auto deadline = timeout_ms > 0
                        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
                        : std::chrono::steady_clock::time_point::max();

    // Wait for data to be ready and not being written
    while (pImpl->m_header->data_ready.load(std::memory_order_acquire) == 0 ||
           pImpl->m_header->is_writing.load(std::memory_order_acquire) != 0)
    {
        // Check if timeout has expired
        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline)
        {
            ReleaseMutex(pImpl->m_mutex);
            return nullptr; // Timeout
        }

        // Calculate remaining timeout for event wait
        DWORD remaining_ms = INFINITE;
        if (timeout_ms > 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                ReleaseMutex(pImpl->m_mutex);
                return nullptr; // Timeout expired
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            remaining_ms = static_cast<DWORD>(remaining.count());
        }

        // Reset event before releasing mutex (for manual-reset event)
        // This prevents race condition where event is set between reset and wait
        if (pImpl->m_event)
        {
            ResetEvent(pImpl->m_event);
        }

        // Release mutex before waiting on event (standard condition variable pattern)
        ReleaseMutex(pImpl->m_mutex);

        // Wait for event signal
        if (pImpl->m_event)
        {
            wait_result = WaitForSingleObject(pImpl->m_event, remaining_ms);
            // Note: Even if timeout occurs, we re-check condition below
            // because data might have become ready during the wait
        }
        else if (timeout_ms > 0)
        {
            // No event handle - sleep for a short time then re-check
            std::this_thread::sleep_for(
                std::chrono::milliseconds(remaining_ms < 100 ? remaining_ms : 100));
        }

        // Re-acquire mutex to check condition again
        wait_result = WaitForSingleObject(pImpl->m_mutex, INFINITE);
        if (wait_result != WAIT_OBJECT_0)
        {
            return nullptr; // Error re-acquiring mutex
        }
        // Loop continues to re-check condition
    }

    // Data is ready - mutex is still held
    // Release mutex before returning
    ReleaseMutex(pImpl->m_mutex);
#else
    pthread_mutex_t *mutex = const_cast<pthread_mutex_t *>(
        reinterpret_cast<const pthread_mutex_t *>(pImpl->m_header->mutex_storage));
    pthread_mutex_lock(mutex);

    // Wait for data to be ready and not being written
    while (pImpl->m_header->data_ready.load(std::memory_order_acquire) == 0 ||
           pImpl->m_header->is_writing.load(std::memory_order_acquire) != 0)
    {
        pthread_cond_t *cond = const_cast<pthread_cond_t *>(
            reinterpret_cast<const pthread_cond_t *>(pImpl->m_header->condition_storage));
        if (timeout_ms > 0)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            int result = pthread_cond_timedwait(cond, mutex, &ts);
            if (result == ETIMEDOUT)
            {
                pthread_mutex_unlock(mutex);
                return nullptr;
            }
        }
        else
        {
            pthread_cond_wait(cond, mutex);
        }
    }
    pthread_mutex_unlock(mutex);
#endif

    return reinterpret_cast<const char *>(pImpl->m_header) + sizeof(SharedMemoryHeader);
}

void SharedMemoryConsumerImpl::cleanup()
{
    if (m_mapped_memory)
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        UnmapViewOfFile(m_mapped_memory);
        if (m_event)
        {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        if (m_mutex)
        {
            CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
        if (m_file_handle)
        {
            CloseHandle(m_file_handle);
            m_file_handle = nullptr;
        }
#else
        munmap(m_mapped_memory, m_size);
        if (m_shm_fd >= 0)
        {
            close(m_shm_fd);
            m_shm_fd = -1;
        }
#endif
        m_mapped_memory = nullptr;
        m_header = nullptr;
    }
}

bool SharedMemoryConsumerImpl::initialize(const std::string &name, Hub *hub, size_t size)
{
    m_name = name;
    (void)hub; // Hub is used for broker queries

    std::string shm_name = "/pylabhub_shm_" + m_name;

    // If size was provided from broker metadata, use it; otherwise use default
    if (size > 0)
    {
        m_size = size;
    }
    else
    {
        // Fallback: use default size (broker should provide this)
        m_size = 1024 * 1024; // 1MB default
        LOGGER_WARN("SharedMemoryConsumer: Size not provided, using default {} bytes", m_size);
    }

#if defined(PYLABHUB_PLATFORM_WIN64)
    std::string win_name = "Global\\pylabhub_shm_" + m_name;
    m_file_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, win_name.c_str());
    if (!m_file_handle)
    {
        LOGGER_ERROR("SharedMemoryConsumer: Failed to open shared memory segment");
        return false;
    }

    m_mapped_memory = MapViewOfFile(m_file_handle, FILE_MAP_ALL_ACCESS, 0, 0, m_size);
    if (!m_mapped_memory)
    {
        CloseHandle(m_file_handle);
        m_file_handle = nullptr;
        LOGGER_ERROR("SharedMemoryConsumer: Failed to map view of file");
        return false;
    }

    // Open synchronization primitives
    std::string mutex_name = "Global\\pylabhub_shm_mutex_" + m_name;
    std::string event_name = "Global\\pylabhub_shm_event_" + m_name;

    m_mutex = OpenMutexA(SYNCHRONIZE, FALSE, mutex_name.c_str());
    if (!m_mutex)
    {
        LOGGER_ERROR("SharedMemoryConsumer: Failed to open mutex");
        UnmapViewOfFile(m_mapped_memory);
        CloseHandle(m_file_handle);
        m_mapped_memory = nullptr;
        m_file_handle = nullptr;
        return false;
    }

    m_event = OpenEventA(EVENT_ALL_ACCESS, FALSE, event_name.c_str());
    if (!m_event)
    {
        LOGGER_ERROR("SharedMemoryConsumer: Failed to open event");
        CloseHandle(m_mutex);
        UnmapViewOfFile(m_mapped_memory);
        CloseHandle(m_file_handle);
        m_mutex = nullptr;
        m_mapped_memory = nullptr;
        m_file_handle = nullptr;
        return false;
    }
#else
    m_shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (m_shm_fd < 0)
    {
        LOGGER_ERROR("SharedMemoryConsumer: Failed to open shared memory: {}", strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(m_shm_fd, &st) < 0)
    {
        close(m_shm_fd);
        m_shm_fd = -1;
        LOGGER_ERROR("SharedMemoryConsumer: fstat failed: {}", strerror(errno));
        return false;
    }
    m_size = st.st_size;

    m_mapped_memory = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
    if (m_mapped_memory == MAP_FAILED)
    {
        close(m_shm_fd);
        m_shm_fd = -1;
        LOGGER_ERROR("SharedMemoryConsumer: mmap failed: {}", strerror(errno));
        return false;
    }
#endif

    m_header = reinterpret_cast<const SharedMemoryHeader *>(m_mapped_memory);
    m_data_size = m_size - sizeof(SharedMemoryHeader);
    return true;
}

// ============================================================================
// ZeroMQ Channel Implementations
// ============================================================================

ZmqPublisher::ZmqPublisher() : pImpl(std::make_unique<ZmqPublisherImpl>()) {}

ZmqPublisher::~ZmqPublisher()
{
    if (pImpl && pImpl->m_socket)
    {
        zmq_close(pImpl->m_socket);
        pImpl->m_socket = nullptr;
    }
}

ZmqPublisher::ZmqPublisher(ZmqPublisher &&other) noexcept : pImpl(std::move(other.pImpl)) {}

ZmqPublisher &ZmqPublisher::operator=(ZmqPublisher &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

bool ZmqPublisher::publish(const char *topic, const void *data, size_t size)
{
    if (!pImpl || !pImpl->m_socket)
        return false;

    // Send topic frame (if provided)
    if (topic && strlen(topic) > 0)
    {
        int rc = zmq_send(pImpl->m_socket, topic, strlen(topic), ZMQ_SNDMORE);
        if (rc < 0)
        {
            LOGGER_ERROR("ZmqPublisher: Failed to send topic: {}", zmq_strerror(zmq_errno()));
            return false;
        }
    }

    // Send data frame
    int rc = zmq_send(pImpl->m_socket, data, size, 0);
    if (rc < 0)
    {
        LOGGER_ERROR("ZmqPublisher: Failed to send data: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    return true;
}

bool ZmqPublisherImpl::initialize(const std::string &service_name, Hub *hub)
{
    m_service_name = service_name;
    m_hub = hub;

    void *context = hub ? hub->get_context() : nullptr;
    if (!hub || !context)
    {
        LOGGER_ERROR("ZmqPublisher: Invalid hub or context");
        return false;
    }

    // Create PUB socket
    m_socket = zmq_socket(context, ZMQ_PUB);
    if (!m_socket)
    {
        LOGGER_ERROR("ZmqPublisher: Failed to create PUB socket: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    // Bind to a local endpoint (in production, this would be registered with broker)
    std::string endpoint = "tcp://*:0"; // Let ZeroMQ choose port
    if (zmq_bind(m_socket, endpoint.c_str()) != 0)
    {
        LOGGER_ERROR("ZmqPublisher: Failed to bind socket: {}", zmq_strerror(zmq_errno()));
        zmq_close(m_socket);
        m_socket = nullptr;
        return false;
    }

    // Get the actual endpoint (for broker registration)
    char actual_endpoint[256];
    size_t endpoint_len = sizeof(actual_endpoint);
    if (zmq_getsockopt(m_socket, ZMQ_LAST_ENDPOINT, actual_endpoint, &endpoint_len) == 0)
    {
        LOGGER_INFO("ZmqPublisher: Bound to {}", actual_endpoint);
    }

    return true;
}

ZmqSubscriber::ZmqSubscriber() : pImpl(std::make_unique<ZmqSubscriberImpl>()) {}

ZmqSubscriber::~ZmqSubscriber()
{
    if (pImpl && pImpl->m_socket)
    {
        zmq_close(pImpl->m_socket);
        pImpl->m_socket = nullptr;
    }
}

ZmqSubscriber::ZmqSubscriber(ZmqSubscriber &&other) noexcept : pImpl(std::move(other.pImpl)) {}

ZmqSubscriber &ZmqSubscriber::operator=(ZmqSubscriber &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

void ZmqSubscriber::subscribe(const char *topic_filter)
{
    if (!pImpl || !pImpl->m_socket)
        return;

    const char *filter = topic_filter ? topic_filter : "";
    if (zmq_setsockopt(pImpl->m_socket, ZMQ_SUBSCRIBE, filter, strlen(filter)) != 0)
    {
        LOGGER_ERROR("ZmqSubscriber: Failed to set subscription filter: {}",
                     zmq_strerror(zmq_errno()));
    }
}

bool ZmqSubscriber::receive(std::string &topic, std::vector<uint8_t> &data, uint32_t timeout_ms)
{
    if (!pImpl || !pImpl->m_socket)
        return false;

    // Set receive timeout
    if (timeout_ms > 0)
    {
        int timeout = static_cast<int>(timeout_ms);
        zmq_setsockopt(pImpl->m_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }
    else
    {
        int timeout = -1; // Wait indefinitely
        zmq_setsockopt(pImpl->m_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }

    // Receive topic frame (if present)
    zmq_msg_t topic_msg;
    zmq_msg_init(&topic_msg);
    int rc = zmq_msg_recv(&topic_msg, pImpl->m_socket, 0);
    if (rc < 0)
    {
        if (zmq_errno() != EAGAIN)
        {
            LOGGER_ERROR("ZmqSubscriber: Failed to receive topic: {}", zmq_strerror(zmq_errno()));
        }
        zmq_msg_close(&topic_msg);
        return false;
    }

    topic.assign(static_cast<const char *>(zmq_msg_data(&topic_msg)), zmq_msg_size(&topic_msg));
    zmq_msg_close(&topic_msg);

    // Check if there's more data
    int more;
    size_t more_size = sizeof(more);
    zmq_getsockopt(pImpl->m_socket, ZMQ_RCVMORE, &more, &more_size);

    if (more)
    {
        // Receive data frame
        zmq_msg_t data_msg;
        zmq_msg_init(&data_msg);
        rc = zmq_msg_recv(&data_msg, pImpl->m_socket, 0);
        if (rc < 0)
        {
            LOGGER_ERROR("ZmqSubscriber: Failed to receive data: {}", zmq_strerror(zmq_errno()));
            zmq_msg_close(&data_msg);
            return false;
        }

        data.assign(static_cast<const uint8_t *>(zmq_msg_data(&data_msg)),
                    static_cast<const uint8_t *>(zmq_msg_data(&data_msg)) +
                        zmq_msg_size(&data_msg));
        zmq_msg_close(&data_msg);
    }
    else
    {
        data.clear();
    }

    return true;
}

bool ZmqSubscriberImpl::initialize(const std::string &service_name, Hub *hub)
{
    m_service_name = service_name;
    m_hub = hub;

    void *context = hub ? hub->get_context() : nullptr;
    if (!hub || !context)
    {
        LOGGER_ERROR("ZmqSubscriber: Invalid hub or context");
        return false;
    }

    // Create SUB socket
    m_socket = zmq_socket(context, ZMQ_SUB);
    if (!m_socket)
    {
        LOGGER_ERROR("ZmqSubscriber: Failed to create SUB socket: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    // In production, would query broker for publisher endpoint and connect
    // For now, placeholder - would connect to discovered endpoint
    LOGGER_INFO("ZmqSubscriber: Created subscriber for service '{}'", service_name);
    return true;
}

ZmqRequestServer::ZmqRequestServer() : pImpl(std::make_unique<ZmqRequestServerImpl>()) {}

ZmqRequestServer::~ZmqRequestServer()
{
    if (pImpl && pImpl->m_socket)
    {
        zmq_close(pImpl->m_socket);
        pImpl->m_socket = nullptr;
    }
}

ZmqRequestServer::ZmqRequestServer(ZmqRequestServer &&other) noexcept
    : pImpl(std::move(other.pImpl))
{
}

ZmqRequestServer &ZmqRequestServer::operator=(ZmqRequestServer &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

bool ZmqRequestServer::handle_request(std::vector<uint8_t> &request_data, const void *reply_data,
                                      size_t reply_size, uint32_t timeout_ms)
{
    if (!pImpl || !pImpl->m_socket)
        return false;

    // Set receive timeout
    if (timeout_ms > 0)
    {
        int timeout = static_cast<int>(timeout_ms);
        zmq_setsockopt(pImpl->m_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }

    // Receive request
    zmq_msg_t request_msg;
    zmq_msg_init(&request_msg);
    int rc = zmq_msg_recv(&request_msg, pImpl->m_socket, 0);
    if (rc < 0)
    {
        if (zmq_errno() != EAGAIN)
        {
            LOGGER_ERROR("ZmqRequestServer: Failed to receive request: {}",
                         zmq_strerror(zmq_errno()));
        }
        zmq_msg_close(&request_msg);
        return false;
    }

    request_data.assign(static_cast<const uint8_t *>(zmq_msg_data(&request_msg)),
                        static_cast<const uint8_t *>(zmq_msg_data(&request_msg)) +
                            zmq_msg_size(&request_msg));
    zmq_msg_close(&request_msg);

    // Send reply
    rc = zmq_send(pImpl->m_socket, reply_data, reply_size, 0);
    if (rc < 0)
    {
        LOGGER_ERROR("ZmqRequestServer: Failed to send reply: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    return true;
}

bool ZmqRequestServerImpl::initialize(const std::string &service_name, Hub *hub)
{
    m_service_name = service_name;
    m_hub = hub;

    void *context = hub ? hub->get_context() : nullptr;
    if (!hub || !context)
    {
        LOGGER_ERROR("ZmqRequestServer: Invalid hub or context");
        return false;
    }

    // Create REP socket
    m_socket = zmq_socket(context, ZMQ_REP);
    if (!m_socket)
    {
        LOGGER_ERROR("ZmqRequestServer: Failed to create REP socket: {}",
                     zmq_strerror(zmq_errno()));
        return false;
    }

    // Bind to a local endpoint
    std::string endpoint = "tcp://*:0"; // Let ZeroMQ choose port
    if (zmq_bind(m_socket, endpoint.c_str()) != 0)
    {
        LOGGER_ERROR("ZmqRequestServer: Failed to bind socket: {}", zmq_strerror(zmq_errno()));
        zmq_close(m_socket);
        m_socket = nullptr;
        return false;
    }

    LOGGER_INFO("ZmqRequestServer: Created server for service '{}'", service_name);
    return true;
}

ZmqRequestClient::ZmqRequestClient() : pImpl(std::make_unique<ZmqRequestClientImpl>()) {}

ZmqRequestClient::~ZmqRequestClient()
{
    if (pImpl && pImpl->m_socket)
    {
        zmq_close(pImpl->m_socket);
        pImpl->m_socket = nullptr;
    }
}

ZmqRequestClient::ZmqRequestClient(ZmqRequestClient &&other) noexcept
    : pImpl(std::move(other.pImpl))
{
}

ZmqRequestClient &ZmqRequestClient::operator=(ZmqRequestClient &&other) noexcept
{
    if (this != &other)
    {
        pImpl = std::move(other.pImpl);
    }
    return *this;
}

bool ZmqRequestClient::send_request(const void *request_data, size_t request_size,
                                    std::vector<uint8_t> &reply_data, uint32_t timeout_ms)
{
    if (!pImpl || !pImpl->m_socket)
        return false;

    // Set receive timeout
    if (timeout_ms > 0)
    {
        int timeout = static_cast<int>(timeout_ms);
        zmq_setsockopt(pImpl->m_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    }

    // Send request
    int rc = zmq_send(pImpl->m_socket, request_data, request_size, 0);
    if (rc < 0)
    {
        LOGGER_ERROR("ZmqRequestClient: Failed to send request: {}", zmq_strerror(zmq_errno()));
        return false;
    }

    // Receive reply
    zmq_msg_t reply_msg;
    zmq_msg_init(&reply_msg);
    rc = zmq_msg_recv(&reply_msg, pImpl->m_socket, 0);
    if (rc < 0)
    {
        if (zmq_errno() != EAGAIN)
        {
            LOGGER_ERROR("ZmqRequestClient: Failed to receive reply: {}",
                         zmq_strerror(zmq_errno()));
        }
        zmq_msg_close(&reply_msg);
        return false;
    }

    reply_data.assign(static_cast<const uint8_t *>(zmq_msg_data(&reply_msg)),
                      static_cast<const uint8_t *>(zmq_msg_data(&reply_msg)) +
                          zmq_msg_size(&reply_msg));
    zmq_msg_close(&reply_msg);

    return true;
}

bool ZmqRequestClientImpl::initialize(const std::string &service_name, Hub *hub)
{
    m_service_name = service_name;
    m_hub = hub;

    void *context = hub ? hub->get_context() : nullptr;
    if (!hub || !context)
    {
        LOGGER_ERROR("ZmqRequestClient: Invalid hub or context");
        return false;
    }

    // Create REQ socket
    m_socket = zmq_socket(context, ZMQ_REQ);
    if (!m_socket)
    {
        LOGGER_ERROR("ZmqRequestClient: Failed to create REQ socket: {}",
                     zmq_strerror(zmq_errno()));
        return false;
    }

    // In production, would query broker for server endpoint and connect
    // For now, placeholder
    LOGGER_INFO("ZmqRequestClient: Created client for service '{}'", service_name);
    return true;
}

// ============================================================================
// Lifecycle Integration
// ============================================================================

namespace
{
void do_hub_startup(const char *arg)
{
    (void)arg;
    g_hub_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module initialized");
}

void do_hub_shutdown(const char *arg)
{
    (void)arg;
    g_hub_initialized.store(false, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module shutdown");
}
} // namespace

pylabhub::utils::ModuleDef GetLifecycleModule()
{
    pylabhub::utils::ModuleDef module("pylabhub::hub::DataExchangeHub");
    // The hub depends on Logger being available
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_hub_startup);
    module.set_shutdown(&do_hub_shutdown, 5000 /*ms timeout*/);
    // Mark as dynamic module (can be loaded/unloaded at runtime)
    module.set_as_permanent(false);
    return module;
}

bool lifecycle_initialized() noexcept
{
    return g_hub_initialized.load(std::memory_order_acquire);
}

} // namespace pylabhub::hub
