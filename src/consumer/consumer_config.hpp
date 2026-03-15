#pragma once
/**
 * @file consumer_config.hpp
 * @brief Standalone consumer configuration — loaded from a flat JSON config file.
 *
 * ## Standard consumer directory layout
 *
 * The canonical layout created by `pylabhub-consumer --init <cons_dir>`:
 *
 * @code
 * <cons_dir>/
 *   consumer.json        ← consumer identity + channel config
 *   vault/
 *     consumer.vault     ← encrypted NaCl keypair (optional)
 *   script/
 *     python/
 *       __init__.py      ← callbacks: on_init / on_consume / on_stop
 *   logs/                ← rotating log files
 *   run/
 *     consumer.pid       ← PID of the running process
 * @endcode
 *
 * ## JSON format
 *
 * @code{.json}
 * {
 *   "hub_dir": "/opt/pylabhub/hubs/lab",
 *
 *   "consumer": {
 *     "uid":       "CONS-LOGGER-12345678",
 *     "name":      "Logger",
 *     "log_level": "info",
 *     "auth": { "keyfile": "" }
 *   },
 *
 *   "channel":          "lab.sensors.temperature",
 *   "slot_acquire_timeout_ms": -1,
 *   "queue_type":       "shm",
 *   "target_period_ms": 0,
 *   "loop_timing":      "fixed_pace",
 *
 *   "slot_schema":     { "fields": [{"name": "value", "type": "float32"}] },
 *   "flexzone_schema": null,
 *
 *   "inbox_schema":      null,
 *   "inbox_endpoint":    "",
 *   "inbox_buffer_depth": 64,
 *   "zmq_packing":       "aligned",
 *
 *   "shm": { "enabled": true, "secret": 0 },
 *
 *   "script": { "path": "." }
 * }
 * @endcode
 */

#include "utils/hub_zmq_queue.hpp"   // kZmqDefaultBufferDepth
#include "utils/loop_timing_policy.hpp"
#include "utils/startup_wait.hpp"

#include <cstdint>
#include "utils/json_fwd.hpp"
#include <string>
#include <vector>

namespace pylabhub::consumer
{

// ============================================================================
// QueueType
// ============================================================================

/**
 * @enum QueueType
 * @brief Selects the queue implementation backing the consumer's data channel.
 *
 * A consumer connects to exactly one data queue: either an SHM ring buffer
 * (DataBlock) or a ZMQ PULL socket (HEP-CORE-0021). This selects which one.
 *
 * | Value | Queue backing | Notes |
 * |-------|---------------|-------|
 * | Shm   | SHM DataBlock (ShmQueue) | Default; ZMQ control runs in background thread |
 * | Zmq   | ZMQ PULL socket (ZmqQueue, HEP-0021) | Requires producer to use ZMQ transport |
 *
 * JSON key: `"queue_type": "shm"` (default) | `"zmq"`.
 */
enum class QueueType : uint8_t
{
    Shm, ///< SHM DataBlock (ShmQueue) — default
    Zmq, ///< ZMQ PULL socket (ZmqQueue) — requires ZMQ transport producer
};

/// LoopTimingPolicy and WaitForRole are defined in shared headers.
using ::pylabhub::LoopTimingPolicy;
using ::pylabhub::WaitForRole;

// ============================================================================
// ConsumerAuthConfig
// ============================================================================

struct ConsumerAuthConfig
{
    std::string keyfile;       ///< Path to encrypted consumer vault; empty = no CURVE auth.
    std::string client_pubkey; ///< Z85 CURVE25519 public key; resolved at runtime.
    std::string client_seckey; ///< Z85 CURVE25519 secret key; resolved at runtime.

    bool load_keypair(const std::string &cons_uid, const std::string &password);
};

// ============================================================================
// ConsumerConfig
// ============================================================================

struct ConsumerConfig
{
    std::string consumer_uid;
    std::string consumer_name;
    std::string log_level{"info"};

    /// Hub directory. When non-empty, broker endpoint and pubkey are loaded
    /// from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
    std::string hub_dir;
    std::string broker{"tcp://127.0.0.1:5570"};
    std::string broker_pubkey;

    /// Input channel (consumed by this consumer).
    std::string channel;

    /// Which data channel drives the main on_consume callback loop.
    /// Default: Shm (block on SHM acquire). Zmq requires ZMQ transport (HEP-CORE-0021).
    QueueType queue_type{QueueType::Shm};

    /// Target start-to-start period for DataBlock acquire pacing (ms).
    /// 0 = free-run (block until next slot, no sleep). Default for consumers.
    /// >0 with FixedRate/FixedRateWithCompensation: activates DataBlock overrun_count tracking.
    int target_period_ms{0};

    /// Loop timing policy. Default: MaxRate (since target_period_ms defaults to 0).
    /// See loop_timing_policy.hpp for cross-field constraints.
    LoopTimingPolicy loop_timing{LoopTimingPolicy::MaxRate};

    /// Slot acquire timeout (ms). -1 = derive from target_period_ms (see
    /// compute_slot_acquire_timeout), 0 = non-blocking, >0 = explicit ms.
    int slot_acquire_timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // SHM — input side
    bool     shm_enabled{true};
    uint64_t shm_secret{0};

    // Schemas
    nlohmann::json slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // ZMQ packing for inbox messages. "aligned" (default) | "packed".
    std::string zmq_packing{"aligned"};

    // ── Inbox facility (optional — active when inbox_schema_json is non-null and non-empty) ─
    nlohmann::json inbox_schema_json{};             ///< Schema for inbox messages. Null/empty = no inbox.
    std::string    inbox_endpoint;                  ///< ROUTER bind endpoint. Empty = auto-assign (port 0).
    size_t         inbox_buffer_depth{hub::kZmqDefaultBufferDepth}; ///< ZMQ RCVHWM for the inbox socket.
    std::string    inbox_overflow_policy{"drop"};   ///< "drop" (finite RCVHWM) or "block" (unlimited queue).

    // ZMQ buffer depth for ZMQ-transport data plane (PULL ring depth).
    size_t zmq_buffer_depth{hub::kZmqDefaultBufferDepth}; ///< Internal recv-ring buffer depth for ZMQ transport. Must be > 0.

    /// Returns true when an inbox is configured (inbox_schema_json is non-null and non-empty).
    [[nodiscard]] bool has_inbox() const noexcept
        { return !inbox_schema_json.is_null() && !inbox_schema_json.empty(); }

    // Script
    std::string script_type{"python"};
    std::string script_path{"."};
    std::string role_dir;         ///< Absolute base of the role directory (set by from_directory(); empty from from_json_file()).
    bool script_type_explicit{false}; ///< True when "type" was present in JSON; false = defaulted.

    /// Virtual environment name for Python scripts. Empty = use base environment.
    /// When set, activates opt/python/venvs/<name>/ at interpreter startup.
    std::string python_venv;

    // Auth
    ConsumerAuthConfig auth{};

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    /// Roles that must be present in the broker before on_init is called.
    std::vector<WaitForRole> wait_for_roles;

    // ── Peer/hub-dead monitoring ────────────────────────────────────────────
    size_t ctrl_queue_max_depth{256}; ///< Max depth of ctrl send queue before oldest dropped.
    int    peer_dead_timeout_ms{30000}; ///< Peer (producer) silence timeout (ms). 0=disabled.

    // Validation
    bool stop_on_script_error{false};
    /// When true, ConsumerScriptHost calls set_verify_checksum(true, has_fz) on the QueueReader.
    /// SHM: BLAKE2b slot+fz checksum verified on each acquire; returns nullptr on mismatch.
    /// ZMQ: no-op (TCP provides transport integrity).
    bool verify_checksum{false};

    static ConsumerConfig from_json_file(const std::string &path);
    static ConsumerConfig from_directory(const std::string &cons_dir);
};

} // namespace pylabhub::consumer
