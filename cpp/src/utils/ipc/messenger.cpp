#include "messenger_internal.hpp"
#include "channel_handle_factory.hpp"

namespace pylabhub::hub
{

// Import shared constants and helpers (TU-local; does not affect other files).
using namespace internal;

// ============================================================================
// Worker-loop-only constants
// ============================================================================
namespace
{
/// Timeout applied to the Messenger lifecycle module's shutdown wait.
constexpr std::chrono::milliseconds kHubShutdownTimeoutMs{5000};
/// How often the worker loop sends HEARTBEAT_REQ for registered producer channels.
constexpr std::chrono::seconds kHeartbeatInterval{2};
/// How often the worker loop wakes up (even if idle) to check the heartbeat timer.
constexpr std::chrono::milliseconds kWorkerPollInterval{200};
} // namespace

// ============================================================================
// Worker loop
// ============================================================================

void MessengerImpl::worker_loop()
{
    std::optional<zmq::socket_t> socket;
    m_next_heartbeat = std::chrono::steady_clock::now() + kHeartbeatInterval;

    while (true)
    {
        std::deque<MessengerCommand> batch;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);

            if (m_is_connected.load(std::memory_order_acquire))
            {
                // When connected, cap the wait so heartbeats and incoming
                // message polling happen on schedule.
                auto now = std::chrono::steady_clock::now();
                auto until = m_next_heartbeat;
                if (until <= now) until = now + kWorkerPollInterval;
                m_queue_cv.wait_until(lock, until,
                                      [this] { return !m_queue.empty(); });
            }
            else
            {
                m_queue_cv.wait(lock, [this] { return !m_queue.empty(); });
            }
            std::swap(batch, m_queue);
        }

        for (auto &cmd : batch)
        {
            bool stop = std::visit(
                [&](auto &&variant_cmd) { return handle_command(variant_cmd, socket); },
                cmd);
            if (stop)
            {
                m_running.store(false, std::memory_order_release);
                return;
            }
        }

        // Process any unsolicited incoming broker messages
        // (e.g. CHANNEL_CLOSING_NOTIFY).
        if (socket.has_value())
        {
            process_incoming(*socket);
        }

        // Send heartbeats if due.
        auto now = std::chrono::steady_clock::now();
        if (socket.has_value() && now >= m_next_heartbeat)
        {
            send_heartbeats(*socket);
            m_next_heartbeat = now + kHeartbeatInterval;
        }
    }
}

// ============================================================================
// Incoming unsolicited message handler
// ============================================================================

void MessengerImpl::process_incoming(zmq::socket_t &socket)
{
    while (true)
    {
        std::vector<zmq::pollitem_t> items = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
        zmq::poll(items, std::chrono::milliseconds{0}); // non-blocking
        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            break;
        }
        std::vector<zmq::message_t> msgs;
        static_cast<void>(
            zmq::recv_multipart(socket, std::back_inserter(msgs),
                                zmq::recv_flags::dontwait));
        // Layout: ['C', msg_type, json_body]
        if (msgs.size() < 3)
        {
            continue;
        }
        const std::string msg_type = msgs[1].to_string();
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
        {
            try
            {
                nlohmann::json body = nlohmann::json::parse(msgs[2].to_string());
                std::string channel = body.value("channel_name", "");
                LOGGER_INFO("Messenger: CHANNEL_CLOSING_NOTIFY for '{}'", channel);
                std::function<void()>                    per_cb;
                std::function<void(const std::string &)> global_cb;
                {
                    std::lock_guard<std::mutex> lock(m_cb_mutex);
                    auto it = m_channel_closing_cbs.find(channel);
                    if (it != m_channel_closing_cbs.end()) per_cb = it->second;
                    global_cb = m_global_channel_closing_cb;
                }
                try
                {
                    if (per_cb) per_cb();
                    else if (global_cb && !channel.empty()) global_cb(channel);
                }
                catch (const std::exception &ex)
                {
                    LOGGER_ERROR("Messenger: on_channel_closing callback threw: {}",
                                 ex.what());
                }
                catch (...)
                {
                    LOGGER_ERROR("Messenger: on_channel_closing callback threw unknown "
                                 "exception");
                }
            }
            catch (const nlohmann::json::exception &e)
            {
                LOGGER_WARN("Messenger: bad CHANNEL_CLOSING_NOTIFY JSON: {}", e.what());
            }
        }
        else if (msg_type == "CONSUMER_DIED_NOTIFY")
        {
            try
            {
                nlohmann::json body = nlohmann::json::parse(msgs[2].to_string());
                std::string channel = body.value("channel_name", "");
                uint64_t    pid     = body.value("consumer_pid", uint64_t{0});
                std::string reason  = body.value("reason", "");
                LOGGER_WARN("Messenger: CONSUMER_DIED_NOTIFY '{}' pid={} ({})",
                            channel, pid, reason);
                std::function<void(uint64_t, std::string)> cb;
                {
                    std::lock_guard<std::mutex> lock(m_cb_mutex);
                    auto it = m_consumer_died_cbs.find(channel);
                    if (it != m_consumer_died_cbs.end()) cb = it->second;
                }
                try
                {
                    if (cb) cb(pid, reason);
                }
                catch (const std::exception &ex)
                {
                    LOGGER_ERROR("Messenger: on_consumer_died callback threw: {}",
                                 ex.what());
                }
                catch (...)
                {
                    LOGGER_ERROR("Messenger: on_consumer_died callback threw unknown "
                                 "exception");
                }
            }
            catch (const nlohmann::json::exception &e)
            {
                LOGGER_WARN("Messenger: bad CONSUMER_DIED_NOTIFY JSON: {}", e.what());
            }
        }
        else if (msg_type == "CHANNEL_ERROR_NOTIFY" || msg_type == "CHANNEL_EVENT_NOTIFY")
        {
            try
            {
                nlohmann::json body    = nlohmann::json::parse(msgs[2].to_string());
                std::string    channel = body.value("channel_name", "");
                std::string    event   = body.value("event", msg_type);
                if (msg_type == "CHANNEL_ERROR_NOTIFY")
                    LOGGER_ERROR("Messenger: CHANNEL_ERROR_NOTIFY '{}' event={}",
                                 channel, event);
                else
                    LOGGER_WARN("Messenger: CHANNEL_EVENT_NOTIFY '{}' event={}",
                                channel, event);
                std::function<void(std::string, nlohmann::json)> cb;
                {
                    std::lock_guard<std::mutex> lock(m_cb_mutex);
                    auto it = m_channel_error_cbs.find(channel);
                    if (it != m_channel_error_cbs.end()) cb = it->second;
                }
                try
                {
                    if (cb) cb(event, body);
                }
                catch (const std::exception &ex)
                {
                    LOGGER_ERROR("Messenger: on_channel_error callback threw: {}",
                                 ex.what());
                }
                catch (...)
                {
                    LOGGER_ERROR("Messenger: on_channel_error callback threw unknown "
                                 "exception");
                }
            }
            catch (const nlohmann::json::exception &e)
            {
                LOGGER_WARN("Messenger: bad {} JSON: {}", msg_type, e.what());
            }
        }
    }
}

// ============================================================================
// Heartbeat sender
// ============================================================================

void MessengerImpl::send_heartbeats(zmq::socket_t &socket)
{
    for (const auto &entry : m_heartbeat_channels)
    {
        if (entry.suppressed)
            continue; // Phase 3: actor zmq_thread_ owns this channel's heartbeat
        try
        {
            nlohmann::json payload;
            payload["channel_name"] = entry.channel;
            payload["producer_pid"] = entry.producer_pid;
            const std::string msg_type    = "HEARTBEAT_REQ";
            const std::string payload_str = payload.dump();
            std::vector<zmq::const_buffer> msgs = {
                zmq::buffer(&kFrameTypeControl, 1),
                zmq::buffer(msg_type),
                zmq::buffer(payload_str)};
            static_cast<void>(zmq::send_multipart(socket, msgs));
            LOGGER_DEBUG("Messenger: heartbeat sent for '{}'", entry.channel);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: heartbeat send failed for '{}': {}",
                         entry.channel, e.what());
        }
    }
}

// ============================================================================
// Command handlers — connection management
// ============================================================================

// Returns true if the worker should stop.
bool MessengerImpl::handle_command(ConnectCmd &cmd, std::optional<zmq::socket_t> &socket)
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

    const bool use_curve = !cmd.server_key.empty();
    if (use_curve && !is_valid_z85_key(cmd.server_key))
    {
        LOGGER_ERROR("Messenger: broker_pubkey must be 40 Z85 chars or empty (plain TCP).");
        cmd.result.set_value(false);
        return false;
    }

    try
    {
        socket.emplace(get_zmq_context(), zmq::socket_type::dealer);

        if (use_curve)
        {
            // Resolve client keypair: use actor's own identity if provided, else ephemeral.
            const bool have_actor_keys =
                is_valid_z85_key(cmd.client_pubkey) &&
                is_valid_z85_key(cmd.client_seckey);

            if (have_actor_keys)
            {
                m_client_public_key_z85 = cmd.client_pubkey;
                m_client_secret_key_z85 = cmd.client_seckey;
                LOGGER_INFO("Messenger: Using actor keypair for CurveZMQ (pubkey: {}...)",
                            cmd.client_pubkey.substr(0, 8));
            }
            else
            {
                std::array<char, kZ85KeyBufSize> z85_public{};
                std::array<char, kZ85KeyBufSize> z85_secret{};
                if (zmq_curve_keypair(z85_public.data(), z85_secret.data()) != 0)
                {
                    LOGGER_ERROR("Messenger: Failed to generate ephemeral CurveZMQ key pair.");
                    cmd.result.set_value(false);
                    socket.reset();
                    return false;
                }
                m_client_public_key_z85 = z85_public.data();
                m_client_secret_key_z85 = z85_secret.data();
                LOGGER_DEBUG("Messenger: Using ephemeral CurveZMQ keypair.");
            }

            socket->set(zmq::sockopt::curve_serverkey, cmd.server_key);
            socket->set(zmq::sockopt::curve_publickey, m_client_public_key_z85);
            socket->set(zmq::sockopt::curve_secretkey, m_client_secret_key_z85);
        }

        socket->connect(cmd.endpoint);
        m_is_connected.store(true, std::memory_order_release);
        LOGGER_INFO("Messenger: Connected to broker at {} ({}).",
                    cmd.endpoint, use_curve ? "CurveZMQ" : "plain TCP");
        cmd.result.set_value(true);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("Messenger: Failed to connect to broker at {}: {} ({})",
                     cmd.endpoint, e.what(), e.num());
        socket.reset();
        m_is_connected.store(false, std::memory_order_release);
        cmd.result.set_value(false);
    }
    return false;
}

bool MessengerImpl::handle_command(DisconnectCmd &cmd, std::optional<zmq::socket_t> &socket)
{
    if (!m_is_connected.load(std::memory_order_acquire))
    {
        cmd.result.set_value();
        return false;
    }
    try
    {
        if (socket.has_value()) socket->close();
        socket.reset();
        m_is_connected.store(false, std::memory_order_release);
        m_heartbeat_channels.clear();
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

// ============================================================================
// Command handlers — Phase 3 heartbeat control + lifecycle
// ============================================================================

bool MessengerImpl::handle_command(SuppressHeartbeatCmd &cmd,
                                   std::optional<zmq::socket_t> & /*socket*/)
{
    for (auto &entry : m_heartbeat_channels)
    {
        if (entry.channel == cmd.channel)
        {
            entry.suppressed = cmd.suppress;
            LOGGER_DEBUG("Messenger: periodic heartbeat for '{}' {}.",
                         cmd.channel, cmd.suppress ? "suppressed" : "restored");
            return false;
        }
    }
    // Channel not in heartbeat list (e.g. consumer role); silently ignore.
    return false;
}

bool MessengerImpl::handle_command(HeartbeatNowCmd &cmd,
                                   std::optional<zmq::socket_t> &socket)
{
    if (!m_is_connected.load(std::memory_order_acquire) || !socket.has_value())
        return false;
    auto it = std::find_if(m_heartbeat_channels.begin(), m_heartbeat_channels.end(),
                           [&](const HeartbeatEntry &e)
                           { return e.channel == cmd.channel; });
    if (it == m_heartbeat_channels.end())
        return false; // Channel not registered; silently ignore.
    send_immediate_heartbeat(*socket, it->channel, it->producer_pid);
    return false;
}

bool MessengerImpl::handle_command(StopCmd & /*cmd*/, std::optional<zmq::socket_t> &socket)
{
    if (socket.has_value())
    {
        try { socket->close(); }
        catch (const zmq::error_t &e)
        {
            LOGGER_ERROR("Messenger: Error closing socket on stop: {}", e.what());
        }
        socket.reset();
    }
    m_is_connected.store(false, std::memory_order_release);
    m_heartbeat_channels.clear();
    return true; // signal worker to exit
}

// ============================================================================
// Lifecycle state
// ============================================================================
namespace
{
std::atomic<bool>        g_hub_initialized{false};
// Use std::atomic for formal memory-model correctness when get_instance() is called
// from any thread concurrently with startup/shutdown.
std::atomic<Messenger *> g_messenger_instance{nullptr};
} // namespace

// ============================================================================
// Messenger Public Methods
// ============================================================================

Messenger::Messenger() : pImpl(std::make_unique<MessengerImpl>()) {}
Messenger::~Messenger() = default;
Messenger::Messenger(Messenger &&) noexcept = default;
Messenger &Messenger::operator=(Messenger &&) noexcept = default;

bool Messenger::connect(const std::string &endpoint,
                         const std::string &server_key,
                         const std::string &client_pubkey,
                         const std::string &client_seckey)
{
    std::lock_guard<std::mutex> lock(pImpl->m_connect_mutex);
    if (pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: Already connected.");
        return true;
    }
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        pImpl->start_worker();
    }
    std::promise<bool> promise;
    auto future = promise.get_future();
    pImpl->enqueue(ConnectCmd{endpoint, server_key, client_pubkey, client_seckey,
                              std::move(promise)});
    return future.get();
}

void Messenger::disconnect()
{
    std::lock_guard<std::mutex> lock(pImpl->m_connect_mutex);
    if (!pImpl->m_is_connected.load(std::memory_order_acquire)) return;

    std::promise<void> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DisconnectCmd{std::move(promise)});
    future.get();
}

void Messenger::register_producer(const std::string &channel, const ProducerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_producer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(RegisterProducerCmd{channel, info});
}

void Messenger::register_consumer(const std::string &channel, const ConsumerInfo &info)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: register_consumer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(RegisterConsumerCmd{channel, info});
}

void Messenger::deregister_consumer(const std::string &channel)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: deregister_consumer('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(DeregisterConsumerCmd{channel});
}

std::optional<ConsumerInfo> Messenger::discover_producer(const std::string &channel,
                                                          int timeout_ms)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: discover_producer('{}') — worker not started.", channel);
        return std::nullopt;
    }
    std::promise<std::optional<ConsumerInfo>> promise;
    auto future = promise.get_future();
    pImpl->enqueue(DiscoverProducerCmd{channel, timeout_ms, std::move(promise)});
    return future.get();
}

std::optional<ChannelHandle>
Messenger::create_channel(const std::string &channel_name,
                           ChannelPattern     pattern,
                           bool               has_shared_memory,
                           const std::string &schema_hash,
                           uint32_t           schema_version,
                           int                timeout_ms,
                           const std::string &actor_name,
                           const std::string &actor_uid)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("Messenger: create_channel('{}') — not connected.", channel_name);
        return std::nullopt;
    }

    // Generate CurveZMQ keypair for P2C sockets.
    std::array<char, 41> z85_pub{}, z85_sec{};
    if (zmq_curve_keypair(z85_pub.data(), z85_sec.data()) != 0)
    {
        LOGGER_ERROR("Messenger: create_channel('{}') — keypair generation failed.",
                     channel_name);
        return std::nullopt;
    }
    const std::string pubkey(z85_pub.data(), 40);
    const std::string seckey(z85_sec.data(), 40);

    // Bind P2C sockets in the calling thread (sockets will be owned by ChannelHandle).
    zmq::context_t &ctx = get_zmq_context();

    zmq::socket_t ctrl_sock(ctx, zmq::socket_type::router);
    ctrl_sock.set(zmq::sockopt::curve_server, 1);
    ctrl_sock.set(zmq::sockopt::curve_secretkey, seckey);
    ctrl_sock.set(zmq::sockopt::curve_publickey, pubkey);
    ctrl_sock.bind("tcp://127.0.0.1:0");
    const std::string ctrl_endpoint = ctrl_sock.get(zmq::sockopt::last_endpoint);

    std::string data_endpoint;
    zmq::socket_t data_sock(ctx, zmq::socket_type::push); // placeholder
    bool has_data_sock = false;

    if (pattern != ChannelPattern::Bidir)
    {
        zmq::socket_type data_type = (pattern == ChannelPattern::PubSub)
                                         ? zmq::socket_type::xpub
                                         : zmq::socket_type::push;
        data_sock = zmq::socket_t(ctx, data_type);
        data_sock.set(zmq::sockopt::curve_server, 1);
        data_sock.set(zmq::sockopt::curve_secretkey, seckey);
        data_sock.set(zmq::sockopt::curve_publickey, pubkey);
        data_sock.bind("tcp://127.0.0.1:0");
        data_endpoint = data_sock.get(zmq::sockopt::last_endpoint);
        has_data_sock = true;
    }

    // Enqueue REG_REQ to broker via worker thread.
    std::promise<bool> reg_promise;
    auto reg_future = reg_promise.get_future();
    CreateChannelCmd cmd;
    cmd.channel           = channel_name;
    cmd.shm_name          = has_shared_memory ? channel_name : std::string{};
    cmd.pattern           = pattern;
    cmd.has_shared_memory = has_shared_memory;
    cmd.schema_hash       = schema_hash;
    cmd.schema_version    = schema_version;
    cmd.producer_pid      = pylabhub::platform::get_pid();
    cmd.zmq_ctrl_endpoint = ctrl_endpoint;
    cmd.zmq_data_endpoint = data_endpoint;
    cmd.zmq_pubkey        = pubkey;
    cmd.timeout_ms        = timeout_ms;
    cmd.actor_name        = actor_name;
    cmd.actor_uid         = actor_uid;
    cmd.result            = std::move(reg_promise);
    pImpl->enqueue(std::move(cmd));

    if (!reg_future.get())
    {
        // Broker rejected — sockets go out of scope and are closed.
        return std::nullopt;
    }

    // Build and return ChannelHandle owning the pre-bound sockets.
    const std::string producer_shm_name = has_shared_memory ? channel_name : std::string{};
    return make_producer_handle(channel_name, pattern, has_shared_memory,
                                std::move(ctrl_sock), std::move(data_sock), has_data_sock,
                                producer_shm_name);
}

std::optional<ChannelHandle>
Messenger::connect_channel(const std::string &channel_name,
                            int                timeout_ms,
                            const std::string &schema_hash,
                            const std::string &consumer_uid,
                            const std::string &consumer_name)
{
    if (!pImpl->m_is_connected.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("Messenger: connect_channel('{}') — not connected.", channel_name);
        return std::nullopt;
    }

    // Discover and register consumer (done in worker thread via FIFO queue).
    std::promise<std::optional<ConsumerInfo>> cc_promise;
    auto cc_future = cc_promise.get_future();
    ConnectChannelCmd cmd;
    cmd.channel              = channel_name;
    cmd.timeout_ms           = timeout_ms;
    cmd.expected_schema_hash = schema_hash;
    cmd.consumer_uid         = consumer_uid;
    cmd.consumer_name        = consumer_name;
    cmd.result               = std::move(cc_promise);
    pImpl->enqueue(std::move(cmd));

    auto cinfo = cc_future.get();
    if (!cinfo.has_value())
    {
        return std::nullopt;
    }

    // Connect P2C sockets in the calling thread using info from DISC_ACK.
    zmq::context_t &ctx = get_zmq_context();

    zmq::socket_t ctrl_sock(ctx, zmq::socket_type::dealer);
    if (!cinfo->zmq_pubkey.empty())
    {
        // Generate a client keypair for the P2C ctrl socket.
        std::array<char, 41> cli_pub{}, cli_sec{};
        if (zmq_curve_keypair(cli_pub.data(), cli_sec.data()) == 0)
        {
            ctrl_sock.set(zmq::sockopt::curve_serverkey, cinfo->zmq_pubkey);
            ctrl_sock.set(zmq::sockopt::curve_publickey, std::string(cli_pub.data(), 40));
            ctrl_sock.set(zmq::sockopt::curve_secretkey, std::string(cli_sec.data(), 40));
        }
    }
    if (!cinfo->zmq_ctrl_endpoint.empty())
    {
        ctrl_sock.connect(cinfo->zmq_ctrl_endpoint);
    }

    bool has_data_sock = !cinfo->zmq_data_endpoint.empty() &&
                         cinfo->pattern != ChannelPattern::Bidir;
    zmq::socket_t data_sock(ctx, zmq::socket_type::pull); // placeholder type

    if (has_data_sock)
    {
        zmq::socket_type data_type = (cinfo->pattern == ChannelPattern::PubSub)
                                         ? zmq::socket_type::sub
                                         : zmq::socket_type::pull;
        data_sock = zmq::socket_t(ctx, data_type);
        if (!cinfo->zmq_pubkey.empty())
        {
            std::array<char, 41> cli_pub{}, cli_sec{};
            if (zmq_curve_keypair(cli_pub.data(), cli_sec.data()) == 0)
            {
                data_sock.set(zmq::sockopt::curve_serverkey, cinfo->zmq_pubkey);
                data_sock.set(zmq::sockopt::curve_publickey,
                              std::string(cli_pub.data(), 40));
                data_sock.set(zmq::sockopt::curve_secretkey,
                              std::string(cli_sec.data(), 40));
            }
        }
        if (cinfo->pattern == ChannelPattern::PubSub)
        {
            // Subscribe to all messages (empty string = subscribe to all).
            data_sock.set(zmq::sockopt::subscribe, "");
        }
        data_sock.connect(cinfo->zmq_data_endpoint);
    }

    // Build and return ChannelHandle owning the connected sockets.
    return make_consumer_handle(channel_name, cinfo->pattern, cinfo->has_shared_memory,
                                std::move(ctrl_sock), std::move(data_sock), has_data_sock,
                                cinfo->shm_name);
}

void Messenger::on_channel_closing(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lock(pImpl->m_cb_mutex);
    pImpl->m_global_channel_closing_cb = std::move(cb);
}

void Messenger::on_channel_closing(const std::string &channel, std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(pImpl->m_cb_mutex);
    if (cb)
        pImpl->m_channel_closing_cbs[channel] = std::move(cb);
    else
        pImpl->m_channel_closing_cbs.erase(channel);
}

void Messenger::on_consumer_died(
    const std::string &channel,
    std::function<void(uint64_t consumer_pid, std::string reason)> cb)
{
    std::lock_guard<std::mutex> lock(pImpl->m_cb_mutex);
    if (cb)
        pImpl->m_consumer_died_cbs[channel] = std::move(cb);
    else
        pImpl->m_consumer_died_cbs.erase(channel);
}

void Messenger::on_channel_error(
    const std::string &channel,
    std::function<void(std::string event, nlohmann::json details)> cb)
{
    std::lock_guard<std::mutex> lock(pImpl->m_cb_mutex);
    if (cb)
        pImpl->m_channel_error_cbs[channel] = std::move(cb);
    else
        pImpl->m_channel_error_cbs.erase(channel);
}

void Messenger::unregister_channel(const std::string &channel)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: unregister_channel('{}') — worker not started.", channel);
        return;
    }
    pImpl->enqueue(UnregisterChannelCmd{channel});
}

void Messenger::report_checksum_error(const std::string &channel, int32_t slot_index,
                                       std::string_view error_description)
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
    {
        LOGGER_WARN("Messenger: report_checksum_error('{}') — worker not started.",
                    channel);
        return;
    }
    pImpl->enqueue(ChecksumErrorReportCmd{channel, slot_index,
                                          std::string(error_description)});
}

// Phase 3 — actor zmq_thread_ heartbeat integration

void Messenger::suppress_periodic_heartbeat(const std::string &channel,
                                             bool suppress) noexcept
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
        return; // worker not started; silently ignore
    pImpl->enqueue(SuppressHeartbeatCmd{channel, suppress});
}

void Messenger::enqueue_heartbeat(const std::string &channel) noexcept
{
    if (!pImpl->m_running.load(std::memory_order_acquire))
        return; // worker not started; silently ignore
    pImpl->enqueue(HeartbeatNowCmd{channel});
}

Messenger &Messenger::get_instance()
{
    assert(g_hub_initialized.load(std::memory_order_acquire) &&
           "Messenger::get_instance() called before registration and initialization "
           "through Lifecycle");
    auto *inst = g_messenger_instance.load(std::memory_order_acquire);
    assert(inst != nullptr);
    return *inst;
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
    zmq_context_startup();
    g_messenger_instance.store(new Messenger(), std::memory_order_release);
    g_hub_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("Data Exchange Hub: Module initialized and ready.");
}

void do_hub_shutdown(const char * /*arg*/)
{
    g_hub_initialized.store(false, std::memory_order_release);
    delete g_messenger_instance.load(std::memory_order_acquire);
    g_messenger_instance.store(nullptr, std::memory_order_release);
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
