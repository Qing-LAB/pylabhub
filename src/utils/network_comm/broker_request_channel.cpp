/**
 * @file broker_request_channel.cpp
 * @brief BrokerRequestChannel — role-to-broker ZMQ DEALER protocol.
 *
 * Implements the broker protocol using a single DEALER socket, MonitoredQueue
 * command queue, and the redesigned ZmqPollLoop with inproc wake-up.
 */

#include "utils/broker_request_channel.hpp"

#include "plh_platform.hpp"   // platform::get_pid()
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"
#include "utils/zmq_poll_loop.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include "monitored_queue.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace pylabhub::hub
{

// ============================================================================
// Wire protocol constant
// ============================================================================

static constexpr char kFrameTypeControl = 'C';

// ============================================================================
// Command types for the MonitoredQueue
// ============================================================================

namespace
{

/// Fire-and-forget: send 3 frames and forget.
struct SendCmd
{
    std::string    msg_type;
    nlohmann::json payload;
};

/// Request-reply: send 3 frames, wait for matching reply.
struct RequestCmd
{
    std::string    msg_type;
    std::string    expected_ack;     ///< Expected reply msg_type (e.g. "REG_ACK")
    nlohmann::json payload;

    // Signaling: broker thread sets result and notifies.
    std::mutex                        mu;
    std::condition_variable           cv;
    bool                              done{false};
    std::optional<nlohmann::json>     result;
};

using BrokerCommand = std::variant<SendCmd, std::shared_ptr<RequestCmd>>;

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct BrokerRequestChannel::Impl
{
    // DEALER socket + monitor.
    std::optional<zmq::socket_t> dealer;
    std::optional<zmq::socket_t> monitor_sock;
    std::string                  monitor_endpoint;

    // Inproc PAIR for wake-up.
    std::optional<zmq::socket_t> signal_read;
    std::optional<zmq::socket_t> signal_write;

    // Command queue.
    MonitoredQueue<BrokerCommand> cmd_queue;

    // Pending request-reply map: keyed by expected_ack msg_type.
    // Supports concurrent requests with different expected reply types.
    std::unordered_map<std::string, std::shared_ptr<RequestCmd>> pending_requests;

    // Callbacks.
    NotificationCallback on_notification_cb;
    std::function<void()> on_hub_dead_cb;

    // Identity (from Config, used by channel join/leave/msg).
    std::string role_uid;
    std::string role_name;

    // Periodic tasks (added before run_poll_loop, consumed during run).
    std::vector<scripting::PeriodicTask> periodic_tasks;
    std::atomic<bool> poll_loop_running{false};

    // State.
    std::atomic<bool> connected{false};
    std::atomic<bool> stop_requested{false};

    // ── Wire protocol helpers ──────────────────────────────────────────

    void send_message(const std::string &msg_type, const nlohmann::json &payload)
    {
        if (!dealer)
            return;
        const std::string body = payload.dump();
        std::vector<zmq::const_buffer> frames = {
            zmq::buffer(&kFrameTypeControl, 1),
            zmq::buffer(msg_type),
            zmq::buffer(body)};
        try
        {
            zmq::send_multipart(*dealer, frames);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("BrokerRequestChannel: send '{}' failed: {}",
                        msg_type, e.what());
        }
    }

    void recv_and_dispatch()
    {
        if (!dealer)
            return;

        // Non-blocking receive all pending messages.
        while (true)
        {
            std::vector<zmq::message_t> msgs;
            auto n = zmq::recv_multipart(*dealer, std::back_inserter(msgs),
                                         zmq::recv_flags::dontwait);
            if (!n.has_value() || *n == 0)
                break;

            // Wire format: ['C'] [msg_type] [json_body]
            if (msgs.size() < 3)
                continue;

            const std::string msg_type = msgs[1].to_string();
            nlohmann::json body;
            try
            {
                body = nlohmann::json::parse(msgs[2].to_string());
            }
            catch (const nlohmann::json::exception &)
            {
                LOGGER_WARN("BrokerRequestChannel: bad JSON in '{}' reply", msg_type);
                continue;
            }

            // Check if this is a reply to a pending request.
            auto it = pending_requests.find(msg_type);
            if (it != pending_requests.end())
            {
                auto req = std::move(it->second);
                pending_requests.erase(it);
                {
                    std::lock_guard<std::mutex> lk(req->mu);
                    req->result = std::move(body);
                    req->done = true;
                }
                req->cv.notify_one();
                continue;
            }

            // ERROR replies: try to match against any pending request.
            // The broker sends ERROR when a request fails. We deliver it
            // to the oldest pending request (FIFO order not guaranteed by
            // unordered_map, but in practice only one request is outstanding).
            if (msg_type == "ERROR" && !pending_requests.empty())
            {
                auto first = pending_requests.begin();
                auto req = std::move(first->second);
                pending_requests.erase(first);
                {
                    std::lock_guard<std::mutex> lk(req->mu);
                    req->result = std::nullopt;
                    req->done = true;
                }
                req->cv.notify_one();
                continue;
            }

            // Unsolicited notification — dispatch to callback.
            if (on_notification_cb)
                on_notification_cb(msg_type, body);
        }
    }

    void check_monitor()
    {
        if (!monitor_sock)
            return;

        // Non-blocking poll the monitor socket.
        zmq::pollitem_t pi{};
        pi.socket = monitor_sock->handle();
        pi.events = ZMQ_POLLIN;
        try
        {
            zmq::poll(&pi, 1, std::chrono::milliseconds{0});
        }
        catch (const zmq::error_t &)
        {
            return;
        }
        if (!(pi.revents & ZMQ_POLLIN))
            return;

        // Monitor frame: [event_data(6 bytes)] [address]
        zmq::message_t ev_msg, addr_msg;
        try
        {
            (void)monitor_sock->recv(ev_msg, zmq::recv_flags::dontwait);
            (void)monitor_sock->recv(addr_msg, zmq::recv_flags::dontwait);
        }
        catch (const zmq::error_t &)
        {
            return;
        }

        if (ev_msg.size() >= 6) // ZMQ monitor frame: uint16 event_id + uint32 value
        {
            const auto *data = static_cast<const uint8_t *>(ev_msg.data());
            const uint16_t event_id =
                static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8);
            if (event_id == ZMQ_EVENT_DISCONNECTED)
            {
                LOGGER_WARN("BrokerRequestChannel: hub-dead (ZMQ_EVENT_DISCONNECTED)");
                if (on_hub_dead_cb)
                    on_hub_dead_cb();
            }
        }
    }

    void drain_command_queue()
    {
        cmd_queue.drain([this](BrokerCommand &cmd) {
            std::visit([this](auto &c) { handle_command(c); }, cmd);
        });
    }

    void handle_command(SendCmd &cmd)
    {
        send_message(cmd.msg_type, cmd.payload);
    }

    void handle_command(std::shared_ptr<RequestCmd> &cmd)
    {
        pending_requests[cmd->expected_ack] = cmd;
        send_message(cmd->msg_type, cmd->payload);
    }

    /// Submit a request-reply and block until reply or timeout.
    std::optional<nlohmann::json> do_request(
        const std::string &msg_type,
        const std::string &expected_ack,
        nlohmann::json payload,
        int timeout_ms)
    {
        auto req = std::make_shared<RequestCmd>();
        req->msg_type = msg_type;
        req->expected_ack = expected_ack;
        req->payload = std::move(payload);

        cmd_queue.push(req);

        std::unique_lock<std::mutex> lk(req->mu);
        if (!req->cv.wait_for(lk, std::chrono::milliseconds{timeout_ms},
                              [&] { return req->done; }))
        {
            LOGGER_WARN("BrokerRequestChannel: {} timed out after {}ms",
                        msg_type, timeout_ms);
            return std::nullopt;
        }
        return req->result;
    }
};

// ============================================================================
// Lifecycle
// ============================================================================

BrokerRequestChannel::BrokerRequestChannel()
    : pImpl(std::make_unique<Impl>())
{}

BrokerRequestChannel::~BrokerRequestChannel()
{
    disconnect();
}

BrokerRequestChannel::BrokerRequestChannel(BrokerRequestChannel &&) noexcept = default;
BrokerRequestChannel &BrokerRequestChannel::operator=(BrokerRequestChannel &&) noexcept = default;

bool BrokerRequestChannel::connect(const Config &cfg)
{
    if (pImpl->connected.load(std::memory_order_acquire))
        return true;

    if (cfg.broker_endpoint.empty())
    {
        LOGGER_ERROR("BrokerRequestChannel: broker_endpoint cannot be empty");
        return false;
    }

    try
    {
        auto &ctx = get_zmq_context();

        // Create DEALER socket.
        pImpl->dealer.emplace(ctx, zmq::socket_type::dealer);
        pImpl->dealer->set(zmq::sockopt::linger, 0);

        // CurveZMQ.
        if (!cfg.broker_pubkey.empty())
        {
            if (!cfg.client_pubkey.empty() && !cfg.client_seckey.empty())
            {
                pImpl->dealer->set(zmq::sockopt::curve_serverkey, cfg.broker_pubkey);
                pImpl->dealer->set(zmq::sockopt::curve_publickey, cfg.client_pubkey);
                pImpl->dealer->set(zmq::sockopt::curve_secretkey, cfg.client_seckey);
            }
            else
            {
                // Generate ephemeral client keypair.
                char pub_z85[41]{}, sec_z85[41]{};
                if (zmq_curve_keypair(pub_z85, sec_z85) != 0)
                {
                    LOGGER_ERROR("BrokerRequestChannel: zmq_curve_keypair failed");
                    pImpl->dealer.reset();
                    return false;
                }
                pImpl->dealer->set(zmq::sockopt::curve_serverkey, cfg.broker_pubkey);
                pImpl->dealer->set(zmq::sockopt::curve_publickey,
                                   std::string(pub_z85, 40));
                pImpl->dealer->set(zmq::sockopt::curve_secretkey,
                                   std::string(sec_z85, 40));
            }
        }

        // ZMTP heartbeats for hub-dead detection.
        pImpl->dealer->set(zmq::sockopt::heartbeat_ivl, 5000);
        pImpl->dealer->set(zmq::sockopt::heartbeat_timeout, 30000);

        pImpl->dealer->connect(cfg.broker_endpoint);

        // Socket monitor for ZMQ_EVENT_DISCONNECTED.
        static std::atomic<int> s_monitor_id{0};
        pImpl->monitor_endpoint = fmt::format(
            "inproc://broker-req-monitor-{}",
            s_monitor_id.fetch_add(1, std::memory_order_relaxed));
        if (zmq_socket_monitor(pImpl->dealer->handle(),
                               pImpl->monitor_endpoint.c_str(),
                               ZMQ_EVENT_DISCONNECTED) == 0)
        {
            pImpl->monitor_sock.emplace(ctx, zmq::socket_type::pair);
            pImpl->monitor_sock->set(zmq::sockopt::linger, 0);
            pImpl->monitor_sock->connect(pImpl->monitor_endpoint);
        }

        // Inproc PAIR for command queue wake-up.
        static std::atomic<int> s_signal_id{0};
        std::string sig_addr = fmt::format(
            "inproc://broker-req-signal-{}",
            s_signal_id.fetch_add(1, std::memory_order_relaxed));
        pImpl->signal_read.emplace(ctx, zmq::socket_type::pair);
        pImpl->signal_write.emplace(ctx, zmq::socket_type::pair);
        pImpl->signal_read->set(zmq::sockopt::linger, 0);
        pImpl->signal_write->set(zmq::sockopt::linger, 0);
        pImpl->signal_read->bind(sig_addr);
        pImpl->signal_write->connect(sig_addr);

        // Wire MonitoredQueue push → signal socket.
        auto *sig_w = &(*pImpl->signal_write);
        pImpl->cmd_queue.set_on_push_signal([sig_w] {
            zmq::message_t wake("W", 1);
            try
            {
                sig_w->send(wake, zmq::send_flags::dontwait);
            }
            catch (const zmq::error_t &)
            {
                // Signal socket full — poll loop will drain on next cycle.
            }
        });

        pImpl->role_uid = cfg.role_uid;
        pImpl->role_name = cfg.role_name;
        pImpl->connected.store(true, std::memory_order_release);
        pImpl->stop_requested.store(false, std::memory_order_release);

        LOGGER_INFO("BrokerRequestChannel: connected to {} ({})",
                    cfg.broker_endpoint,
                    cfg.broker_pubkey.empty() ? "plain TCP" : "CurveZMQ");
        return true;
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("BrokerRequestChannel: connect failed: {}", e.what());
        pImpl->monitor_sock.reset();
        pImpl->dealer.reset();
        pImpl->signal_read.reset();
        pImpl->signal_write.reset();
        return false;
    }
}

void BrokerRequestChannel::disconnect()
{
    pImpl->stop_requested.store(true, std::memory_order_release);
    pImpl->cmd_queue.set_on_push_signal(nullptr);

    // Close monitor before dealer to avoid spurious hub-dead on voluntary disconnect.
    if (pImpl->monitor_sock)
    {
        if (pImpl->dealer)
            zmq_socket_monitor(pImpl->dealer->handle(), nullptr, 0);
        pImpl->monitor_sock.reset();
    }

    pImpl->signal_write.reset();
    pImpl->signal_read.reset();
    pImpl->dealer.reset();
    pImpl->connected.store(false, std::memory_order_release);
}

bool BrokerRequestChannel::is_connected() const noexcept
{
    return pImpl->connected.load(std::memory_order_acquire);
}

// ============================================================================
// Callbacks
// ============================================================================

void BrokerRequestChannel::on_notification(NotificationCallback cb)
{
    pImpl->on_notification_cb = std::move(cb);
}

void BrokerRequestChannel::on_hub_dead(std::function<void()> cb)
{
    pImpl->on_hub_dead_cb = std::move(cb);
}

// ============================================================================
// Periodic tasks
// ============================================================================

void BrokerRequestChannel::set_periodic_task(std::function<void()> action,
                                              int interval_ms,
                                              std::function<uint64_t()> get_iteration)
{
    if (pImpl->poll_loop_running.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("BrokerRequestChannel: set_periodic_task called after run_poll_loop started");
        return;
    }
    pImpl->periodic_tasks.emplace_back(
        std::move(action), interval_ms, std::move(get_iteration));
}

// ============================================================================
// Poll loop
// ============================================================================

void BrokerRequestChannel::run_poll_loop(std::function<bool()> should_run)
{
    if (!pImpl->dealer)
    {
        LOGGER_ERROR("BrokerRequestChannel: run_poll_loop called without connect");
        return;
    }

    scripting::ZmqPollLoop loop{
        [this, &should_run] {
            return should_run() &&
                   !pImpl->stop_requested.load(std::memory_order_acquire);
        },
        "broker"};

    // Poll the DEALER socket for incoming broker messages.
    loop.sockets.push_back(
        {zmq::socket_ref(zmq::from_handle, pImpl->dealer->handle()),
         [this] {
             pImpl->recv_and_dispatch();
             pImpl->check_monitor();
         }});

    // Signal socket for command queue wake-up.
    if (pImpl->signal_read)
    {
        loop.signal_socket =
            zmq::socket_ref(zmq::from_handle, pImpl->signal_read->handle());
        loop.drain_commands = [this] { pImpl->drain_command_queue(); };
    }

    // Move periodic tasks into the loop.
    pImpl->poll_loop_running.store(true, std::memory_order_release);
    loop.periodic_tasks = std::move(pImpl->periodic_tasks);

    loop.run();

    pImpl->poll_loop_running.store(false, std::memory_order_release);
}

void BrokerRequestChannel::stop() noexcept
{
    pImpl->stop_requested.store(true, std::memory_order_release);

    // Send a wake-up to break the poll.
    if (pImpl->signal_write)
    {
        try
        {
            pImpl->signal_write->send(zmq::message_t("S", 1),
                                      zmq::send_flags::dontwait);
        }
        catch (...) {}
    }
}

// ============================================================================
// Fire-and-forget messages
// ============================================================================

void BrokerRequestChannel::send_heartbeat(const std::string &channel,
                                           const nlohmann::json &metrics)
{
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["producer_pid"] = pylabhub::platform::get_pid();
    if (!metrics.empty())
        payload["metrics"] = metrics;
    pImpl->cmd_queue.push(SendCmd{"HEARTBEAT_REQ", std::move(payload)});
}

void BrokerRequestChannel::send_metrics_report(const std::string &channel,
                                                 const std::string &uid,
                                                 const nlohmann::json &metrics)
{
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["uid"] = uid;
    payload["metrics"] = metrics;
    pImpl->cmd_queue.push(SendCmd{"METRICS_REPORT_REQ", std::move(payload)});
}

void BrokerRequestChannel::send_notify(const std::string &target,
                                        const std::string &sender_uid,
                                        const std::string &event,
                                        const std::string &data)
{
    nlohmann::json payload;
    payload["target_channel"] = target;
    payload["sender_uid"] = sender_uid;
    payload["event"] = event;
    payload["data"] = data;
    pImpl->cmd_queue.push(SendCmd{"CHANNEL_NOTIFY_REQ", std::move(payload)});
}

void BrokerRequestChannel::send_broadcast(const std::string &target,
                                            const std::string &sender_uid,
                                            const std::string &msg,
                                            const std::string &data)
{
    nlohmann::json payload;
    payload["target_channel"] = target;
    payload["sender_uid"] = sender_uid;
    payload["message"] = msg;
    payload["data"] = data;
    pImpl->cmd_queue.push(SendCmd{"CHANNEL_BROADCAST_REQ", std::move(payload)});
}

void BrokerRequestChannel::send_checksum_error(const nlohmann::json &report)
{
    pImpl->cmd_queue.push(SendCmd{"CHECKSUM_ERROR_REPORT", report});
}

void BrokerRequestChannel::send_endpoint_update(const std::string &channel,
                                                  const std::string &key,
                                                  const std::string &endpoint)
{
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["endpoint_type"] = key;
    payload["endpoint"] = endpoint;
    pImpl->cmd_queue.push(SendCmd{"ENDPOINT_UPDATE_REQ", std::move(payload)});
}

// ============================================================================
// Request-reply methods
// ============================================================================

std::optional<nlohmann::json>
BrokerRequestChannel::register_channel(const nlohmann::json &opts, int timeout_ms)
{
    return pImpl->do_request( "REG_REQ", "REG_ACK", opts, timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestChannel::discover_channel(const std::string &channel,
                                        const nlohmann::json &opts,
                                        int timeout_ms)
{
    nlohmann::json payload = opts;
    payload["channel_name"] = channel;
    return pImpl->do_request( "DISC_REQ", "DISC_ACK", std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestChannel::register_consumer(const nlohmann::json &opts, int timeout_ms)
{
    return pImpl->do_request( "CONSUMER_REG_REQ", "CONSUMER_REG_ACK", opts, timeout_ms);
}

bool BrokerRequestChannel::deregister_channel(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel_name"] = channel;
    auto result = pImpl->do_request("DEREG_REQ", "DEREG_ACK",
                             std::move(payload), timeout_ms);
    return result.has_value() && result->value("status", "") == "success";
}

bool BrokerRequestChannel::deregister_consumer(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel_name"] = channel;
    auto result = pImpl->do_request("CONSUMER_DEREG_REQ", "CONSUMER_DEREG_ACK",
                             std::move(payload), timeout_ms);
    return result.has_value() && result->value("status", "") == "success";
}

bool BrokerRequestChannel::query_role_presence(const std::string &uid, int timeout_ms)
{
    nlohmann::json payload;
    payload["uid"] = uid;
    auto result = pImpl->do_request("ROLE_PRESENCE_REQ", "ROLE_PRESENCE_ACK",
                             std::move(payload), timeout_ms);
    return result.has_value() && result->value("present", false);
}

std::optional<nlohmann::json>
BrokerRequestChannel::query_role_info(const std::string &uid, int timeout_ms)
{
    nlohmann::json payload;
    payload["uid"] = uid;
    return pImpl->do_request( "ROLE_INFO_REQ", "ROLE_INFO_ACK",
                      std::move(payload), timeout_ms);
}

std::vector<nlohmann::json>
BrokerRequestChannel::list_channels(int timeout_ms)
{
    auto result = pImpl->do_request("CHANNEL_LIST_REQ", "CHANNEL_LIST_ACK",
                             nlohmann::json::object(), timeout_ms);
    if (!result.has_value())
        return {};
    auto it = result->find("channels");
    if (it == result->end() || !it->is_array())
        return {};
    return it->get<std::vector<nlohmann::json>>();
}

std::optional<nlohmann::json>
BrokerRequestChannel::query_shm_info(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    return pImpl->do_request( "SHM_BLOCK_QUERY_REQ", "SHM_BLOCK_QUERY_ACK",
                      std::move(payload), timeout_ms);
}

// ============================================================================
// Channel pub/sub messaging (HEP-CORE-0030)
// ============================================================================

std::optional<nlohmann::json>
BrokerRequestChannel::join_channel(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["role_uid"] = pImpl->role_uid;
    payload["role_name"] = pImpl->role_name;
    return pImpl->do_request("CHANNEL_JOIN_REQ", "CHANNEL_JOIN_ACK",
                             std::move(payload), timeout_ms);
}

bool BrokerRequestChannel::leave_channel(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["role_uid"] = pImpl->role_uid;
    auto result = pImpl->do_request("CHANNEL_LEAVE_REQ", "CHANNEL_LEAVE_ACK",
                                    std::move(payload), timeout_ms);
    return result.has_value() && result->value("status", "") == "success";
}

void BrokerRequestChannel::send_channel_msg(const std::string &channel,
                                             const nlohmann::json &body)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["sender_uid"] = pImpl->role_uid;
    payload["body"] = body;
    pImpl->cmd_queue.push(SendCmd{"CHANNEL_MSG_REQ", std::move(payload)});
}

std::optional<nlohmann::json>
BrokerRequestChannel::query_channel_members(const std::string &channel,
                                             int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    return pImpl->do_request("CHANNEL_MEMBERS_REQ", "CHANNEL_MEMBERS_ACK",
                             std::move(payload), timeout_ms);
}

} // namespace pylabhub::hub
