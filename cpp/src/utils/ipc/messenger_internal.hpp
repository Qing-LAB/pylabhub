// messenger_internal.hpp
//
// PRIVATE header — include ONLY from messenger.cpp and messenger_protocol.cpp.
// Not part of the public API; not installed.
//
// Contains:
//   1. Common #includes for both messenger translation units.
//   2. namespace internal — shared inline constants + helpers.
//   3. Command struct definitions (ConnectCmd, RegisterProducerCmd, ...).
//   4. MessengerCommand variant type.
//   5. HeartbeatEntry struct.
//   6. MessengerImpl class declaration (methods defined in the .cpp files).
//
// Method ownership:
//   messenger.cpp          — worker_loop, process_incoming, send_heartbeats,
//                            ConnectCmd/DisconnectCmd/StopCmd/SuppressHeartbeatCmd/
//                            HeartbeatNowCmd handlers, public Messenger API, lifecycle.
//   messenger_protocol.cpp — all broker protocol handlers (REG_REQ, DISC_REQ, …),
//                            send_disc_req, send_immediate_heartbeat.

#pragma once

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/channel_handle.hpp"
#include "utils/messenger.hpp"
#include "utils/zmq_context.hpp"

#include "sodium.h"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
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
#include <unordered_map>
#include <variant>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// Constants and Helper Functions
// (shared between messenger.cpp and messenger_protocol.cpp)
// ============================================================================

namespace internal
{

inline constexpr size_t       kZ85KeyChars             = 40;
inline constexpr size_t       kSchemaHashHexLen         = 64;
inline constexpr size_t       kSchemaHashBytes          = 32;
inline constexpr unsigned int kNibbleMask               = 0x0FU;
inline constexpr int          kHexLetterOffset          = 10;
inline constexpr size_t       kZ85KeyBufSize            = 41;
inline constexpr int          kDefaultRegisterTimeoutMs = 5000;

// Universal framing: Frame 0 type byte for all ZMQ messages in this project.
// 'C' = Control (Frame 1: type string, Frame 2: JSON body)
// 'A' = ASCII/JSON data   (Frame 1: JSON payload)
// 'M' = MessagePack data  (Frame 1: MessagePack bytes)
inline constexpr char kFrameTypeControl = 'C';

inline bool is_valid_z85_key(const std::string &key)
{
    return key.length() == kZ85KeyChars;
}

inline std::string hex_encode_schema_hash(const std::string &raw)
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

inline std::string hex_decode_schema_hash(const std::string &hex_str)
{
    auto hex_val = [](char hex_char) -> int {
        if (hex_char >= '0' && hex_char <= '9') return hex_char - '0';
        if (hex_char >= 'a' && hex_char <= 'f') return hex_char - 'a' + kHexLetterOffset;
        if (hex_char >= 'A' && hex_char <= 'F') return hex_char - 'A' + kHexLetterOffset;
        return -1;
    };
    if (hex_str.size() != kSchemaHashHexLen) return {};
    std::string out;
    out.reserve(kSchemaHashBytes);
    for (size_t i = 0; i < kSchemaHashHexLen; i += 2)
    {
        int high_val = hex_val(hex_str[i]);
        int low_val  = hex_val(hex_str[i + 1]);
        if (high_val < 0 || low_val < 0) return {};
        out += static_cast<char>((high_val << 4) | low_val);
    }
    return out;
}

// channel_pattern_to_str() and channel_pattern_from_str() are defined in
// utils/channel_pattern.hpp (included via messenger.hpp).  No local duplicates needed.

} // namespace internal

// ============================================================================
// Async Queue Commands
// ============================================================================

struct ConnectCmd
{
    std::string endpoint;
    std::string server_key;    ///< Broker Z85 public key; empty = plain TCP
    std::string client_pubkey; ///< Actor Z85 public key; empty = ephemeral (only when CURVE)
    std::string client_seckey; ///< Actor Z85 secret key; empty = ephemeral (only when CURVE)
    std::promise<bool> result;
};
struct DisconnectCmd
{
    std::promise<void> result;
};
struct RegisterProducerCmd
{
    std::string  channel;
    ProducerInfo info;
};
struct RegisterConsumerCmd
{
    std::string  channel;
    ConsumerInfo info;
};
struct DeregisterConsumerCmd
{
    std::string channel;
};
struct UnregisterChannelCmd
{
    std::string channel;
};
struct ChecksumErrorReportCmd
{
    std::string channel;
    int32_t     slot_index;
    std::string error_description;
};
struct DiscoverChannelCmd
{
    std::string channel;
    int         timeout_ms;
    std::promise<std::optional<ConsumerInfo>> result;
};
/// Internal: sent by create_channel() after binding P2C sockets.
/// Worker sends REG_REQ with the P2C endpoints, waits for ACK, registers heartbeat.
struct CreateChannelCmd
{
    std::string    channel;
    std::string    shm_name;   ///< SHM segment name; equals channel when has_shared_memory=true
    ChannelPattern pattern;
    bool           has_shared_memory;
    std::string    schema_hash; ///< raw bytes (not hex-encoded)
    uint32_t       schema_version;
    uint64_t       producer_pid;
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey; ///< Z85 producer public key for P2C sockets
    int            timeout_ms;
    // Phase 2: actor identity included in REG_REQ payload.
    std::string actor_name; ///< Human-readable actor name; empty = omit from payload
    std::string actor_uid;  ///< Actor UUID4; empty = omit from payload
    // Phase 3: named schema (HEP-CORE-0016).
    std::string schema_id;   ///< Named schema ID (e.g. "lab.sensors.temperature.raw@1"); empty = unnamed
    std::string schema_blds; ///< BLDS string; empty when DataT has no PYLABHUB_SCHEMA macros
    // HEP-CORE-0021: ZMQ Virtual Channel Node transport.
    std::string data_transport{"shm"}; ///< "shm" (default) or "zmq"
    std::string zmq_node_endpoint;     ///< PUSH bind endpoint; non-empty only when data_transport="zmq"
    std::promise<bool> result; ///< true = broker accepted (REG_ACK received)
};
/// Internal: sent by connect_channel() to discover and register as consumer.
/// Worker sends DISC_REQ (retrying on CHANNEL_NOT_READY), then CONSUMER_REG_REQ.
struct ConnectChannelCmd
{
    std::string channel;
    int         timeout_ms;
    std::string expected_schema_hash; ///< raw bytes; empty = accept any
    // Phase 2: consumer identity included in CONSUMER_REG_REQ payload.
    std::string consumer_uid;  ///< Consumer actor UUID4; empty = omit from payload
    std::string consumer_name; ///< Consumer actor name; empty = omit from payload
    // Phase 3: named schema (HEP-CORE-0016).
    std::string expected_schema_id; ///< If non-empty, consumer requests named schema validation
    std::promise<std::optional<ConsumerInfo>> result;
};
/// Phase 3 (HEP-CORE-0016): query broker for schema info of a registered channel.
/// Worker sends SCHEMA_REQ and waits for SCHEMA_ACK containing schema_id, blds, hash.
struct QuerySchemaCmd
{
    std::string channel;
    int         timeout_ms;
    std::promise<std::optional<ChannelSchemaInfo>> result;
};

/// Phase 4: fire-and-forget relay to target channel's producer via broker.
struct ChannelNotifyCmd
{
    std::string target_channel;
    std::string sender_uid;
    std::string event;
    std::string data; ///< User data string (passed through transparently).
};

/// Channel broadcast: fan-out a control message to ALL members of a channel.
struct ChannelBroadcastCmd
{
    std::string target_channel;
    std::string sender_uid;
    std::string message;
    std::string data; ///< User data string (passed through transparently).
};

/// Query broker for the list of registered channels.
struct ChannelListCmd
{
    int timeout_ms{5000};
    std::promise<std::vector<nlohmann::json>> result;
};

/// HEP-CORE-0019: consumer metrics report (fire-and-forget).
struct MetricsReportCmd
{
    std::string    channel;
    std::string    uid;
    nlohmann::json metrics;
};

struct StopCmd
{
};
/// Phase 3: suppress or restore the periodic heartbeat for one channel.
/// When suppressed, the actor's zmq_thread_ takes over heartbeat responsibility.
struct SuppressHeartbeatCmd
{
    std::string channel;
    bool        suppress; ///< true = suppress periodic; false = restore
};
/// Phase 3: send HEARTBEAT_REQ immediately for one channel (fire-and-forget).
/// Used by the actor's zmq_thread_ to deliver application-level heartbeats.
struct HeartbeatNowCmd
{
    std::string    channel;
    nlohmann::json metrics; ///< HEP-0019: optional metrics payload (empty = no metrics).
};

using MessengerCommand = std::variant<ConnectCmd, DisconnectCmd, RegisterProducerCmd,
                                      RegisterConsumerCmd, DiscoverChannelCmd,
                                      DeregisterConsumerCmd, UnregisterChannelCmd,
                                      ChecksumErrorReportCmd, CreateChannelCmd,
                                      ConnectChannelCmd, StopCmd,
                                      SuppressHeartbeatCmd, HeartbeatNowCmd,
                                      QuerySchemaCmd, ChannelNotifyCmd,
                                      ChannelBroadcastCmd, ChannelListCmd,
                                      MetricsReportCmd>;

// ============================================================================
// MessengerImpl
// ============================================================================

struct HeartbeatEntry
{
    std::string channel;
    uint64_t    producer_pid;
    bool        suppressed{false}; ///< Phase 3: true when actor zmq_thread_ owns this heartbeat
};

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

    // Heartbeat entries (worker-thread-owned; channels registered via register_producer
    // or create_channel).  Protected implicitly by the fact that only the worker thread
    // reads/writes this after startup.
    std::vector<HeartbeatEntry> m_heartbeat_channels;
    std::chrono::steady_clock::time_point m_next_heartbeat{};

    // Per-channel event callbacks (guarded by m_cb_mutex).
    std::mutex m_cb_mutex;
    std::unordered_map<std::string, std::function<void()>>                            m_channel_closing_cbs;
    std::unordered_map<std::string, std::function<void()>>                            m_force_shutdown_cbs;
    std::unordered_map<std::string, std::function<void(uint64_t, std::string)>>       m_consumer_died_cbs;
    std::unordered_map<std::string, std::function<void(std::string, nlohmann::json)>> m_channel_error_cbs;
    /// Backward-compat: fires for any channel when no per-channel closing cb registered.
    std::function<void(const std::string &)> m_global_channel_closing_cb;

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

    // ── Worker loop and I/O (defined in messenger.cpp) ───────────────────────

    void worker_loop();
    void process_incoming(zmq::socket_t &socket);
    void send_heartbeats(zmq::socket_t &socket);

    // ── Connection management (defined in messenger.cpp) ─────────────────────

    bool handle_command(ConnectCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(DisconnectCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(StopCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(SuppressHeartbeatCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(HeartbeatNowCmd &cmd, std::optional<zmq::socket_t> &socket);

    // ── Broker protocol (defined in messenger_protocol.cpp) ──────────────────

    bool handle_command(RegisterProducerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(RegisterConsumerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(DeregisterConsumerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(UnregisterChannelCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(ChecksumErrorReportCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(DiscoverChannelCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(CreateChannelCmd &cmd, std::optional<zmq::socket_t> &socket);
    bool handle_command(ConnectChannelCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(QuerySchemaCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(ChannelNotifyCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(ChannelBroadcastCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(ChannelListCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    bool handle_command(MetricsReportCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;

    std::optional<nlohmann::json> send_disc_req(zmq::socket_t &socket,
                                                 const std::string &channel,
                                                 int timeout_ms) const;

    static void send_immediate_heartbeat(zmq::socket_t &socket,
                                         const std::string &channel,
                                         uint64_t producer_pid,
                                         const nlohmann::json &metrics = {});
};

} // namespace pylabhub::hub
