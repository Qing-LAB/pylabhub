#pragma once
/**
 * @file broker_service.hpp
 * @brief Central broker service for channel lifecycle management.
 *
 * BrokerService runs the channel discovery hub: producers register channels,
 * consumers discover them via REG/DISC/DEREG messages over a ZMQ ROUTER socket.
 * Channels start in PendingReady state; the first HEARTBEAT_REQ transitions them
 * to Ready. Dead channels (heartbeat timeout) trigger CHANNEL_CLOSING_NOTIFY to
 * registered consumers then are removed.
 *
 * All socket I/O is single-threaded (run() loop); only stop() is thread-safe.
 */
#include "pylabhub_utils_export.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pylabhub::broker
{

class BrokerServiceImpl;

class PYLABHUB_UTILS_EXPORT BrokerService
{
public:
    struct Config
    {
        std::string endpoint{"tcp://0.0.0.0:5570"};
        bool use_curve{true};

        /// Timeout for dead channel detection. A channel that has not sent a
        /// HEARTBEAT_REQ within this window is closed and consumers notified.
        std::chrono::seconds channel_timeout{10};

        /// Optional: called from run() after bind() with (bound_endpoint, server_public_key).
        /// Useful for tests using dynamic port assignment (endpoint="tcp://127.0.0.1:0").
        std::function<void(const std::string& bound_endpoint,
                           const std::string& pubkey)> on_ready;
    };

    explicit BrokerService(Config cfg);
    ~BrokerService();

    BrokerService(const BrokerService&) = delete;
    BrokerService& operator=(const BrokerService&) = delete;

    /**
     * @brief Server public key (Z85-encoded, 40 chars).
     * Logged at startup; clients pass this to Messenger::connect().
     */
    [[nodiscard]] const std::string& server_public_key() const;

    /**
     * @brief Main event loop. Blocks until stop() is called.
     * Polls ROUTER socket with 100ms timeout; checks heartbeat timeouts each cycle.
     */
    void run();

    /**
     * @brief Signal the run() loop to exit. Thread-safe.
     */
    void stop();

private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<BrokerServiceImpl> pImpl;
};

} // namespace pylabhub::broker
