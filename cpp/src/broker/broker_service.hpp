#pragma once
/**
 * @file broker_service.hpp
 * @brief Central broker service for channel discovery (REG/DISC/DEREG).
 *
 * BrokerService listens on a ZMQ ROUTER socket. Producers register channels,
 * consumers discover them. All socket I/O is single-threaded (run() loop);
 * only stop() is called from another thread.
 */
#include "channel_registry.hpp"

#include "cppzmq/zmq.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>

namespace pylabhub::broker
{

class BrokerService
{
public:
    struct Config
    {
        std::string endpoint{"tcp://0.0.0.0:5570"};
        bool use_curve{true};
        /// Optional: called from run() after bind() with (bound_endpoint, server_public_key).
        /// Useful for tests using dynamic port assignment (endpoint="tcp://127.0.0.1:0").
        std::function<void(const std::string& bound_endpoint,
                           const std::string& pubkey)> on_ready;
    };

    explicit BrokerService(Config cfg);
    ~BrokerService() = default;

    BrokerService(const BrokerService&) = delete;
    BrokerService& operator=(const BrokerService&) = delete;

    /**
     * @brief Server public key (Z85-encoded, 40 chars).
     * Logged at startup; clients pass this to Messenger::connect().
     */
    [[nodiscard]] const std::string& server_public_key() const;

    /**
     * @brief Main event loop. Blocks until stop() is called.
     * Polls ROUTER socket with 100ms timeout; checks m_stop_requested each cycle.
     */
    void run();

    /**
     * @brief Signal the run() loop to exit. Thread-safe.
     */
    void stop();

private:
    Config m_cfg;
    std::string m_server_public_z85;
    std::string m_server_secret_z85;
    ChannelRegistry m_registry;
    std::atomic<bool> m_stop_requested{false};

    void process_message(zmq::socket_t& socket,
                         const zmq::message_t& identity,
                         const std::string& msg_type,
                         const nlohmann::json& payload);

    nlohmann::json handle_reg_req(const nlohmann::json& req);
    nlohmann::json handle_disc_req(const nlohmann::json& req);
    nlohmann::json handle_dereg_req(const nlohmann::json& req);

    static void send_reply(zmq::socket_t& socket,
                           const zmq::message_t& identity,
                           const std::string& msg_type_ack,
                           const nlohmann::json& body);

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static nlohmann::json make_error(const std::string& correlation_id,
                                     const std::string& error_code,
                                     const std::string& message);
};

} // namespace pylabhub::broker
