#pragma once
/**
 * @file message_hub.hpp
 * @brief ZeroMQ-based messaging with the central broker.
 */
#include "pylabhub_utils_export.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pylabhub::hub
{

class MessageHubImpl;

struct ProducerInfo
{
    std::string shm_name;
    uint64_t producer_pid;
    std::string schema_hash;
    uint32_t schema_version;
    // Add more metadata fields as needed based on the spec
};

struct ConsumerInfo
{
    std::string shm_name;
    std::string schema_hash;
    uint32_t schema_version;
    // Add more metadata fields as needed based on the spec
};

/**
 * @class MessageHub
 * @brief Manages communication with the central broker.
 *
 * This class handles connecting to the broker and sending messages according to the
 * strict two-part protocol defined in HEP-core-0002.
 *
 * All public methods are thread-safe and can be called concurrently from
 * multiple threads. Internal locking ensures ZeroMQ socket operations
 * are serialized.
 */
class PYLABHUB_UTILS_EXPORT MessageHub
{
  public:
    MessageHub();
    ~MessageHub();

    MessageHub(const MessageHub &) = delete;
    MessageHub &operator=(const MessageHub &) = delete;
    MessageHub(MessageHub &&) noexcept;
    MessageHub &operator=(MessageHub &&) noexcept;

    /**
     * @brief Connects to the broker at the specified endpoint.
     * @param endpoint The ZeroMQ endpoint of the broker (e.g., "tcp://localhost:5555").
     * @param server_key The Z85-encoded public key of the broker for CurveZMQ.
     * @return True if the connection was successful, false otherwise.
     */
    bool connect(const std::string &endpoint, const std::string &server_key);

    /**
     * @brief Disconnects from the broker and cleans up resources.
     */
    void disconnect();

    /**
     * @brief Sends a message to the broker and waits for a response.
     * @param channel The channel name.
     * @param message The message content.
     * @param timeout_ms Timeout for waiting for a response.
     * @return The response message as a string, or std::nullopt on failure or timeout.
     */
    std::optional<std::string> send_message(const std::string &channel, const std::string &message,
                                            int timeout_ms = 5000);

    /**
     * @brief Receives a message from the broker.
     * @param timeout_ms Timeout for waiting for a message.
     * @return The received message as a string, or std::nullopt on failure or timeout.
     */
    std::optional<std::string> receive_message(int timeout_ms);

    /**
     * @brief Registers a producer with the broker.
     * @param channel The channel name.
     * @param info Producer information.
     * @return True on success, false on failure.
     */
    bool register_producer(const std::string &channel, const ProducerInfo &info);

    /**
     * @brief Discovers a producer's information from the broker.
     * @param channel The channel name.
     * @return ConsumerInfo on success, or std::nullopt on failure.
     */
    std::optional<ConsumerInfo> discover_producer(const std::string &channel);

    /**
     * @brief Registers a consumer with the broker.
     * @param channel The channel name.
     * @param info Consumer information.
     * @return True on success, false on failure.
     */
    bool register_consumer(const std::string &channel, const ConsumerInfo &info);

    /**
     * @brief Returns the singleton instance of the MessageHub.
     * @return Reference to the MessageHub instance.
     */
    static MessageHub &get_instance();

  private:
    std::unique_ptr<MessageHubImpl> pImpl;
};

/**
 * @brief Checks if the Data Exchange Hub module has been initialized by the Lifecycle manager.
 * @return True if the module is initialized, false otherwise.
 */
PYLABHUB_UTILS_EXPORT bool lifecycle_initialized() noexcept;

/**
 * @brief Factory function to get the ModuleDef for the Data Exchange Hub.
 * @return A ModuleDef object that can be registered with the LifecycleManager.
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetLifecycleModule();

} // namespace pylabhub::hub
