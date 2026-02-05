#include "gtest/gtest.h"
#include "plh_datahub.hpp" // Main include for Lifecycle, Logger, etc.

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "sodium.h"
#include <thread>
#include <chrono>

#include "test_process_utils.h"
#include "test_entrypoint.h" // For g_self_exe_path

// ============================================================================
// Lifecycle Tests (Worker-Based)
// ============================================================================
TEST(MessageHubLifecycle, FollowsState)
{
    pylabhub::tests::helper::WorkerProcess worker(
        g_self_exe_path, "messagehub.lifecycle_initialized_follows_state", {});
    ASSERT_TRUE(worker.valid());
    worker.wait_for_exit();
    pylabhub::tests::helper::expect_worker_ok(worker);
}

// ============================================================================
// Mock Broker for Connection Tests
// ============================================================================

class MockBroker
{
  public:
    enum class ResponseMode
    {
        Ok,
        Timeout,
        Malformed_NoPayload,
        Malformed_BadPayload
    };

    MockBroker()
    {
        unsigned char server_public_key[crypto_box_PUBLICKEYBYTES];
        unsigned char server_secret_key[crypto_box_SECRETKEYBYTES];
        crypto_box_keypair(server_public_key, server_secret_key);

        char z85_public[41], z85_secret[41];
        zmq_z85_encode(z85_public, server_public_key, crypto_box_PUBLICKEYBYTES);
        zmq_z85_encode(z85_secret, server_secret_key, crypto_box_SECRETKEYBYTES);
        m_public_key = z85_public;
        m_secret_key = z85_secret;
    }

    void start()
    {
        m_running = true;
        m_thread = std::thread(&MockBroker::run, this);
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    void set_next_response(ResponseMode mode) { m_response_mode = mode; }

    const std::string &get_public_key() const { return m_public_key; }
    const std::string &get_endpoint() const { return m_endpoint; }
    int get_notifications_received() const { return m_notifications_received.load(); }

  private:
    void run()
    {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::router);

        socket.set(zmq::sockopt::curve_server, 1);
        socket.set(zmq::sockopt::curve_secretkey, m_secret_key);

        socket.bind(m_endpoint);

        while (m_running)
        {
            std::vector<zmq::pollitem_t> items = {{socket, 0, ZMQ_POLLIN, 0}};
            zmq::poll(items, std::chrono::milliseconds(100));

            if (items[0].revents & ZMQ_POLLIN)
            {
                std::vector<zmq::message_t> request_parts;
                auto result = zmq::recv_multipart(socket, std::back_inserter(request_parts));

                if (!result || request_parts.size() < 2)
                    continue;

                auto &identity = request_parts[0];
                const auto &header_msg = request_parts[1];
                std::string_view header_sv(header_msg.data<const char>(), header_msg.size());

                // Notifications should not receive a reply. Only reply in OK mode if it's not a
                // notification.
                if (m_response_mode.load() == ResponseMode::Ok &&
                    header_sv.find("NOTIFY") != std::string_view::npos)
                {
                    m_notifications_received++; // Increment counter for received notifications
                    continue; // Do not reply to notifications
                }

                switch (m_response_mode.load())
                {
                case ResponseMode::Ok:
                {
                    nlohmann::json ok_payload = {{"status", "OK"}};
                    auto msgpack = nlohmann::json::to_msgpack(ok_payload);

                    socket.send(identity, zmq::send_flags::sndmore);
                    socket.send(zmq::str_buffer("PYLABHUB_ACK"), zmq::send_flags::sndmore);
                    socket.send(zmq::buffer(msgpack));
                    break;
                }
                case ResponseMode::Malformed_NoPayload:
                {
                    socket.send(identity, zmq::send_flags::sndmore);
                    socket.send(zmq::str_buffer("PYLABHUB_ACK"));
                    break;
                }
                case ResponseMode::Malformed_BadPayload:
                {
                    std::vector<uint8_t> bad_msgpack = {0xDE, 0xAD, 0xBE, 0xEF};
                    socket.send(identity, zmq::send_flags::sndmore);
                    socket.send(zmq::str_buffer("PYLABHUB_ACK"), zmq::send_flags::sndmore);
                    socket.send(zmq::buffer(bad_msgpack));
                    break;
                }
                case ResponseMode::Timeout:
                default:
                    break;
                }
            }
        }
    }

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<ResponseMode> m_response_mode{ResponseMode::Ok};
    std::string m_public_key;
    std::string m_secret_key;
    std::atomic<int> m_notifications_received{0}; // Atomic counter for received notifications
    const std::string m_endpoint = "tcp://127.0.0.1:5557"; // Use a different port
};

// ============================================================================
// Connection and Communication Tests
// ============================================================================

class MessageHubConnectionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        broker.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override
    {
        hub.disconnect();
        broker.stop();
    }

    MockBroker broker;
    pylabhub::hub::MessageHub hub;
};

TEST_F(MessageHubConnectionTest, Connect_FailsWithEmptyEndpoint)
{
    ASSERT_FALSE(hub.connect("", broker.get_public_key()));
}

TEST_F(MessageHubConnectionTest, Connect_FailsWithInvalidServerKey)
{
    ASSERT_FALSE(hub.connect(broker.get_endpoint(), "short-key"));
}

TEST_F(MessageHubConnectionTest, Connect_SucceedsWithValidBroker)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));
}

TEST_F(MessageHubConnectionTest, Disconnect_CleansUpConnection)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));
    hub.disconnect();

    nlohmann::json payload = {{"data", 1}};
    nlohmann::json response;
    ASSERT_FALSE(hub.send_request("PYLABHUB_REQ", payload, response, 500));
}

TEST_F(MessageHubConnectionTest, SendRequest_SucceedsWithOkResponse)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));

    broker.set_next_response(MockBroker::ResponseMode::Ok);
    nlohmann::json payload = {{"req", "echo"}};
    nlohmann::json response;

    ASSERT_TRUE(hub.send_request("PYLABHUB_REQ", payload, response, 1000));
    ASSERT_EQ(response["status"], "OK");
}

TEST_F(MessageHubConnectionTest, SendRequest_FailsOnTimeout)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));

    broker.set_next_response(MockBroker::ResponseMode::Timeout);
    nlohmann::json payload = {{"req", "echo"}};
    nlohmann::json response;

    ASSERT_FALSE(hub.send_request("PYLABHUB_REQ", payload, response, 200));
}

TEST_F(MessageHubConnectionTest, SendRequest_FailsWithMalformedResponse_NoPayload)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));

    broker.set_next_response(MockBroker::ResponseMode::Malformed_NoPayload);
    nlohmann::json payload = {{"req", "echo"}};
    nlohmann::json response;
    ASSERT_FALSE(hub.send_request("PYLABHUB_REQ", payload, response, 500));
}

TEST_F(MessageHubConnectionTest, SendRequest_FailsWithMalformedResponse_BadPayload)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));

    broker.set_next_response(MockBroker::ResponseMode::Malformed_BadPayload);
    nlohmann::json payload = {{"req", "echo"}};
    nlohmann::json response;
    ASSERT_FALSE(hub.send_request("PYLABHUB_REQ", payload, response, 500));
}

TEST_F(MessageHubConnectionTest, SendNotification_Succeeds)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));

    nlohmann::json payload = {{"notify", "something_happened"}};
    ASSERT_TRUE(hub.send_notification("PYLABHUB_NOTIFY", payload));

    // Wait for the notification to be received by the mock broker
    bool notification_received = false;
    for (int i = 0; i < 100; ++i) // Try for up to 100 * 10ms = 1s
    {
        if (broker.get_notifications_received() > 0)
        {
            notification_received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(notification_received) << "Notification was not received by the mock broker.";
}

TEST_F(MessageHubConnectionTest, Send_FailsBeforeConnect)
{
    nlohmann::json payload = {{"data", 1}};
    nlohmann::json response;
    ASSERT_FALSE(hub.send_request("PYLABHUB_REQ", payload, response, 500));
    ASSERT_FALSE(hub.send_notification("PYLABHUB_NOTIFY", payload));
}

TEST_F(MessageHubConnectionTest, Reconnect_Succeeds)
{
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));
    hub.disconnect();
    ASSERT_TRUE(hub.connect(broker.get_endpoint(), broker.get_public_key()));
}