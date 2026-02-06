#pragma once

#include "pylabhub_utils_export.h"
#include "nlohmann/json.hpp" // Direct include for nlohmann::json
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::hub
{

// Forward declare the implementation class
class MessageHubImpl;

/**
 * @class MessageHub
 * @brief Manages communication with the central broker.
 *
 * This class handles connecting to the broker and sending messages according to the
 * strict two-part protocol defined in HEP-core-0002.
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
     * @brief Sends a two-part message to the broker.
     * @param header A 16-byte character array representing the message type.
     * @param payload A JSON object to be serialized with MessagePack.
     * @param response Optional output parameter to store the broker's response.
     * @param timeout_ms Timeout for waiting for a response.
     * @return True on success, false on failure or timeout.
     */
    bool send_request(const char *header, const nlohmann::json &payload, nlohmann::json &response,
                      int timeout_ms = 5000);

    /**
     * @brief Sends a one-way notification to the broker.
     * @param header A 16-byte character array representing the message type.
     * @param payload A JSON object to be serialized with MessagePack.
     * @return True on success, false on failure.
     */
    bool send_notification(const char *header, const nlohmann::json &payload);

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
