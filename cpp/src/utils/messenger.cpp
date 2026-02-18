#include "plh_service.hpp"
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
constexpr int kDefaultRegisterTimeoutMs = 5000; // ms to wait for REG_REQ broker response
constexpr std::chrono::milliseconds kHubShutdownTimeoutMs{5000};

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
struct DiscoverProducerCmd
{
    std::string channel;
    int timeout_ms;
    std::promise<std::optional<ConsumerInfo>> result;
};
struct StopCmd
{
};

using MessengerCommand = std::variant<ConnectCmd, DisconnectCmd, RegisterProducerCmd,
                                      RegisterConsumerCmd, DiscoverProducerCmd, StopCmd>;

// ============================================================================
// MessengerImpl
// ============================================================================

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

    void worker_loop()
    {
        // The socket lives entirely in this worker thread.
        std::optional<zmq::socket_t> socket;

        while (true)
        {
            std::deque<MessengerCommand> batch;
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);
                m_queue_cv.wait(lock, [this] { return !m_queue.empty(); });
                std::swap(batch, m_queue);
            }

            for (auto &cmd : batch)
            {
                bool stop = std::visit(
                    [&](auto &&variant_cmd) { return handle_command(variant_cmd, socket); }, cmd);
                if (stop)
                {
                    m_running.store(false, std::memory_order_release);
                    return;
                }
            }
        }
    }

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
            LOGGER_ERROR("Messenger: Failed to connect to broker at {}: {} ({})", cmd.endpoint,
                         e.what(), e.num());
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
            if (socket.has_value())
            {
                socket->close();
            }
            socket.reset();
            m_is_connected.store(false, std::memory_order_release);
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

    bool handle_command(RegisterProducerCmd &cmd, std::optional<zmq::socket_t> &socket) const
    {
        if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
        {
            LOGGER_WARN("Messenger: register_producer('{}') skipped — not connected.", cmd.channel);
            return false;
        }
        try
        {
            nlohmann::json payload;
            payload["msg_type"] = "REG_REQ";
            payload["channel_name"] = cmd.channel;
            payload["shm_name"] = cmd.info.shm_name;
            payload["producer_pid"] = cmd.info.producer_pid;
            payload["schema_hash"] = hex_encode_schema_hash(cmd.info.schema_hash);
            payload["schema_version"] = cmd.info.schema_version;

            // Keep strings alive for the duration of send_multipart.
            const std::string msg_type = "REG_REQ";
            const std::string payload_str = payload.dump();
            std::vector<zmq::const_buffer> msgs = {zmq::buffer(msg_type),
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
                LOGGER_ERROR("Messenger: register_producer('{}') timed out waiting for response.",
                             cmd.channel);
                return false;
            }
            std::vector<zmq::message_t> recv_msgs;
            static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
            if (recv_msgs.size() < 2)
            {
                LOGGER_ERROR("Messenger: register_producer('{}') invalid response format.",
                             cmd.channel);
                return false;
            }
            nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
            if (!response.contains("status") || response["status"] != "success")
            {
                std::string msg =
                    response.value("message", std::string("unknown"));
                LOGGER_ERROR("Messenger: register_producer('{}') failed: {}", cmd.channel, msg);
            }
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: ZMQ error in register_producer('{}'): {}", cmd.channel,
                         e.what());
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGGER_ERROR("Messenger: JSON error in register_producer('{}'): {}", cmd.channel,
                         e.what());
        }
        return false;
    }

    static bool handle_command(RegisterConsumerCmd & /*cmd*/,
                               std::optional<zmq::socket_t> & /*socket*/)
    {
        // Not yet implemented: consumer registration protocol not defined.
        return false;
    }

    bool handle_command(DiscoverProducerCmd &cmd, std::optional<zmq::socket_t> &socket) const
    {
        if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
        {
            LOGGER_WARN("Messenger: discover_producer('{}') — not connected.", cmd.channel);
            cmd.result.set_value(std::nullopt);
            return false;
        }
        try
        {
            nlohmann::json payload;
            payload["msg_type"] = "DISC_REQ";
            payload["channel_name"] = cmd.channel;

            // Keep strings alive for the duration of send_multipart.
            const std::string msg_type = "DISC_REQ";
            const std::string payload_str = payload.dump();
            std::vector<zmq::const_buffer> msgs = {zmq::buffer(msg_type),
                                                   zmq::buffer(payload_str)};
            if (!zmq::send_multipart(*socket, msgs))
            {
                LOGGER_ERROR("Messenger: discover_producer('{}') send failed.", cmd.channel);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            std::vector<zmq::pollitem_t> items = {{socket->handle(), 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds(cmd.timeout_ms));
            if ((items[0].revents & ZMQ_POLLIN) == 0)
            {
                LOGGER_ERROR("Messenger: discover_producer('{}') timed out ({}ms).", cmd.channel,
                             cmd.timeout_ms);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            std::vector<zmq::message_t> recv_msgs;
            static_cast<void>(zmq::recv_multipart(*socket, std::back_inserter(recv_msgs)));
            if (recv_msgs.size() < 2)
            {
                LOGGER_ERROR("Messenger: discover_producer('{}') invalid response format.",
                             cmd.channel);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            nlohmann::json response = nlohmann::json::parse(recv_msgs.back().to_string());
            if (!response.contains("status") || response["status"] != "success")
            {
                std::string msg = response.value("message", std::string("unknown"));
                LOGGER_ERROR("Messenger: discover_producer('{}') failed: {}", cmd.channel, msg);
                cmd.result.set_value(std::nullopt);
                return false;
            }
            if (!response.contains("shm_name") || !response["shm_name"].is_string() ||
                !response.contains("schema_hash") || !response["schema_hash"].is_string() ||
                !response.contains("schema_version") ||
                !response["schema_version"].is_number_unsigned())
            {
                LOGGER_ERROR("Messenger: discover_producer('{}') response missing required fields.",
                             cmd.channel);
                cmd.result.set_value(std::nullopt);
                return false;
            }

            ConsumerInfo cinfo{};
            cinfo.shm_name = response["shm_name"].get<std::string>();
            cinfo.schema_hash =
                hex_decode_schema_hash(response["schema_hash"].get<std::string>());
            cinfo.schema_version = response["schema_version"].get<uint32_t>();
            cmd.result.set_value(cinfo);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: ZMQ error in discover_producer('{}'): {}", cmd.channel,
                         e.what());
            cmd.result.set_value(std::nullopt);
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGGER_ERROR("Messenger: JSON error in discover_producer('{}'): {}", cmd.channel,
                         e.what());
            cmd.result.set_value(std::nullopt);
        }
        return false;
    }

    bool handle_command(StopCmd & /*cmd*/, std::optional<zmq::socket_t> &socket)
    {
        if (socket.has_value())
        {
            try
            {
                socket->close();
            }
            catch (const zmq::error_t &e)
            {
                LOGGER_ERROR("Messenger: Error closing socket on stop: {}", e.what());
            }
            socket.reset();
        }
        m_is_connected.store(false, std::memory_order_release);
        return true; // signal worker to exit
    }
};

// ============================================================================
// Lifecycle state (declared before Messenger public methods so get_instance() can reference them)
// ============================================================================
namespace
{
std::atomic<bool> g_hub_initialized{false};
// Messenger instance owned by the lifecycle module.
// Created in do_hub_startup, destroyed in do_hub_shutdown before ZMQ context teardown.
// This prevents the static-destruction-order hazard where a function-local static Messenger
// could outlive the ZMQ context and access it from its worker thread during teardown.
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

    // Start worker thread if not yet running
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        pImpl->start_worker();
    }

    // Enqueue connect command and wait for synchronous result
    std::promise<bool> promise;
    auto future = promise.get_future();
    pImpl->enqueue(ConnectCmd{endpoint, server_key, std::move(promise)});
    return future.get();
}

void Messenger::disconnect()
{
    std::lock_guard<std::mutex> lock(pImpl->m_connect_mutex);

    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        return;
    }

    std::promise<void> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DisconnectCmd{std::move(promise)});
    future.get();
}

void Messenger::register_producer(const std::string &channel, const ProducerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_producer('{}') — worker not started (not connected).",
                    channel);
        return;
    }
    pImpl->enqueue(RegisterProducerCmd{channel, info});
}

void Messenger::register_consumer(const std::string &channel, const ConsumerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_consumer('{}') — worker not started (not connected).",
                    channel);
        return;
    }
    pImpl->enqueue(RegisterConsumerCmd{channel, info});
}

std::optional<ConsumerInfo> Messenger::discover_producer(const std::string &channel,
                                                          int timeout_ms)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: discover_producer('{}') — worker not started (not connected).",
                    channel);
        return std::nullopt;
    }

    std::promise<std::optional<ConsumerInfo>> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DiscoverProducerCmd{channel, timeout_ms, std::move(promise)});
    return future.get();
}

Messenger &Messenger::get_instance()
{
    assert(g_hub_initialized.load(std::memory_order_acquire) &&
           "Messenger::get_instance() called before registration and initialization through Lifecycle");
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
    // Initialize ZMQ context before Messenger (socket creation depends on context).
    zmq_context_startup();
    // Messenger instance is owned by the lifecycle module.
    g_messenger_instance = new Messenger();
    g_hub_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module initialized and ready.");
}

void do_hub_shutdown(const char * /*arg*/)
{
    g_hub_initialized.store(false, std::memory_order_release);
    // Destroy Messenger before ZMQ context: ~MessengerImpl enqueues StopCmd, joins the worker
    // thread, and closes the socket — all before the context pointer is invalidated.
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
