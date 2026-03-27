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
#include "utils/format_tools.hpp"
#include "utils/messenger.hpp"
#include "utils/zmq_context.hpp"

#include "sodium.h"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "utils/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
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
    return format_tools::bytes_to_hex(raw);
}

inline std::string hex_decode_schema_hash(const std::string &hex_str)
{
    if (hex_str.size() != kSchemaHashHexLen) return {};
    auto decoded = format_tools::bytes_from_hex(hex_str);
    if (decoded.size() != kSchemaHashBytes) return {}; // invalid chars → bytes_from_hex returned original
    return decoded;
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
    std::string client_pubkey; ///< Role Z85 public key; empty = ephemeral (only when CURVE)
    std::string client_seckey; ///< Role Z85 secret key; empty = ephemeral (only when CURVE)
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
    // Phase 2: role identity included in REG_REQ payload.
    std::string role_name; ///< Human-readable role name; empty = omit from payload
    std::string role_uid;  ///< Role UID; empty = omit from payload
    // Phase 3: named schema (HEP-CORE-0016).
    std::string schema_id;   ///< Named schema ID (e.g. "lab.sensors.temperature.raw@1"); empty = unnamed
    std::string schema_blds; ///< BLDS string; empty when DataT has no PYLABHUB_SCHEMA macros
    // HEP-CORE-0021: ZMQ Virtual Channel Node transport.
    std::string data_transport{"shm"}; ///< "shm" (default) or "zmq"
    std::string zmq_node_endpoint;     ///< PUSH bind endpoint; non-empty only when data_transport="zmq"
    // Phase 3 (Inbox Facility): producer inbox ROUTER endpoint.
    std::string inbox_endpoint;        ///< ROUTER bind endpoint; empty when no inbox configured.
    // Phase 4: inbox schema info (stored by broker for ROLE_INFO_REQ responses).
    std::string inbox_schema_json;     ///< JSON-serialized ZmqSchemaField list; empty = no inbox.
    std::string inbox_packing;         ///< "aligned" or "packed"; empty = no inbox.
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
    std::string consumer_uid;  ///< Consumer role UID; empty = omit from payload
    std::string consumer_name; ///< Consumer role name; empty = omit from payload
    // Phase 3: named schema (HEP-CORE-0016).
    std::string expected_schema_id; ///< If non-empty, consumer requests named schema validation
    // Phase 6: transport arbitration — sent to broker in CONSUMER_REG_REQ.
    std::string consumer_queue_type; ///< "shm" or "zmq"; empty = no server-side validation
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

/// Query broker for SHM block topology + DataBlockMetrics.
/// Worker sends SHM_BLOCK_QUERY_REQ and waits for SHM_BLOCK_QUERY_ACK.
/// Returns the raw JSON string from the broker (empty on error/timeout).
struct QueryShmBlocksCmd
{
    std::string channel;    ///< Channel name to query; empty = all channels.
    int         timeout_ms{5000};
    std::promise<std::string> result;
};

/// Phase 4: query broker for role liveness — returns true if UID is present in any channel.
struct RolePresenceReqCmd
{
    std::string        uid;
    int                timeout_ms{5000};
    std::promise<bool> result;
};
/// Phase 4: query broker for inbox connection info for a producer UID.
struct RoleInfoReqCmd
{
    std::string                                     uid;
    int                                             timeout_ms{5000};
    std::promise<std::optional<RoleInfoResult>>     result;
};

/// HEP-0021 §16: update a channel's endpoint after ephemeral port bind.
struct EndpointUpdateCmd
{
    std::string        channel_name;
    std::string        endpoint_type;  ///< "zmq_node" or "inbox"
    std::string        endpoint;       ///< Resolved endpoint (e.g., "tcp://127.0.0.1:45782")
    int                timeout_ms{5000};
    std::promise<bool> result;
};

struct StopCmd
{
};
/// Phase 3: suppress or restore the periodic heartbeat for one channel.
/// When suppressed, the role's zmq_thread_ takes over heartbeat responsibility.
struct SuppressHeartbeatCmd
{
    std::string channel;
    bool        suppress; ///< true = suppress periodic; false = restore
};
/// Phase 3: send HEARTBEAT_REQ immediately for one channel (fire-and-forget).
/// Used by the role's zmq_thread_ to deliver application-level heartbeats.
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
                                      MetricsReportCmd, QueryShmBlocksCmd,
                                      RolePresenceReqCmd, RoleInfoReqCmd,
                                      EndpointUpdateCmd>;

// ============================================================================
// MessengerImpl
// ============================================================================

struct HeartbeatEntry
{
    std::string channel;
    uint64_t    producer_pid;
    bool        suppressed{false}; ///< Phase 3: true when role zmq_thread_ owns this heartbeat
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

    // Hub-dead callback: fired from the worker thread when ZMQ_EVENT_DISCONNECTED is
    // received on the monitor socket (i.e. ZMTP heartbeat timeout expired or TCP dropped).
    // Guarded by m_cb_mutex for registration; called only from the worker thread.
    std::function<void()> m_on_hub_dead_cb;

    // ZMQ socket monitor — worker-thread-only state (no locking needed).
    // Created in handle_command(ConnectCmd) and torn down in DisconnectCmd/StopCmd.
    std::optional<zmq::socket_t> m_monitor_sock_;
    std::string                  m_monitor_endpoint_; ///< inproc address of the monitor

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
    void process_monitor_events(zmq::socket_t &monitor); ///< Fire m_on_hub_dead_cb on disconnect
    void send_heartbeats(zmq::socket_t &socket);
    void close_monitor(); ///< Tear down monitor socket (call before closing main socket)

    // ── Connection management (defined in messenger.cpp) ─────────────────────

    void handle_command(ConnectCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(DisconnectCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(StopCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(SuppressHeartbeatCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(HeartbeatNowCmd &cmd, std::optional<zmq::socket_t> &socket);

    // ── Broker protocol (defined in messenger_protocol.cpp) ──────────────────

    void handle_command(RegisterProducerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(RegisterConsumerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(DeregisterConsumerCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(UnregisterChannelCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(ChecksumErrorReportCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(DiscoverChannelCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(CreateChannelCmd &cmd, std::optional<zmq::socket_t> &socket);
    void handle_command(ConnectChannelCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(QuerySchemaCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(ChannelNotifyCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(ChannelBroadcastCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(ChannelListCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(MetricsReportCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(QueryShmBlocksCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(RolePresenceReqCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(RoleInfoReqCmd &cmd,
                        std::optional<zmq::socket_t> &socket) const;
    void handle_command(EndpointUpdateCmd &cmd,
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
