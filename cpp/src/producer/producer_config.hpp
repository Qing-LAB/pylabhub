#pragma once
/**
 * @file producer_config.hpp
 * @brief Standalone producer configuration — loaded from a flat JSON config file.
 *
 * ## Standard producer directory layout
 *
 * The canonical layout created by `pylabhub-producer --init <prod_dir>`:
 *
 * @code
 * <prod_dir>/
 *   producer.json        ← producer identity + channel config
 *   vault/
 *     producer.vault     ← encrypted NaCl keypair (optional)
 *   script/
 *     python/
 *       __init__.py      ← callbacks: on_init / on_produce / on_stop
 *   logs/                ← rotating log files
 *   run/
 *     producer.pid       ← PID of the running process
 * @endcode
 *
 * ## JSON format
 *
 * @code{.json}
 * {
 *   "hub_dir": "/opt/pylabhub/hubs/lab",
 *
 *   "producer": {
 *     "uid":       "PROD-TEMPSENS-12345678",
 *     "name":      "TempSensor",
 *     "log_level": "info",
 *     "auth": { "keyfile": "" }
 *   },
 *
 *   "channel":          "lab.sensors.temperature",
 *   "target_period_ms": 100,
 *   "timeout_ms":       5000,
 *
 *   "slot_schema":     { "fields": [{"name": "value", "type": "float32"}] },
 *   "flexzone_schema": null,
 *
 *   "shm": { "enabled": true, "secret": 0, "slot_count": 8 },
 *
 *   "script": { "path": "." }
 * }
 * @endcode
 */

#include "utils/loop_timing_policy.hpp"
#include "utils/startup_wait.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pylabhub::producer
{

// ============================================================================
// Transport
// ============================================================================

/**
 * @enum Transport
 * @brief Data-plane transport for the producer's output channel.
 *
 * | Value | Backing | Discovery |
 * |-------|---------|-----------|
 * | Shm   | Shared-memory DataBlock (ShmQueue) | Broker stores SHM secret; consumer attaches |
 * | Zmq   | ZMQ PUSH socket (ZmqQueue) | Broker stores endpoint; consumer connects |
 *
 * JSON key: `"transport": "shm" | "zmq"` (default: `"shm"`).
 *
 * When `transport == Zmq`:
 *  - `zmq_out_endpoint` is required (e.g. `"tcp://0.0.0.0:5581"`).
 *  - `zmq_out_bind` controls whether the PUSH socket binds (default: true).
 *  - `zmq_buffer_depth` is the internal send-ring depth (default: 64).
 *  - `flexzone_schema` is silently ignored (no flexzone in ZMQ transport).
 *  - `shm.enabled` should be false (no SHM block allocated for the data plane).
 */
enum class Transport : uint8_t
{
    Shm, ///< SHM-backed (ShmQueue), default
    Zmq, ///< ZMQ PUSH/PULL-backed (ZmqQueue)
};

/// LoopTimingPolicy and WaitForRole are defined in shared headers.
using ::pylabhub::LoopTimingPolicy;
using ::pylabhub::WaitForRole;

// ============================================================================
// ProducerAuthConfig
// ============================================================================

struct ProducerAuthConfig
{
    std::string keyfile;       ///< Path to encrypted producer vault; empty = no CURVE auth.
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); resolved at runtime.
    std::string client_seckey; ///< Z85 CURVE25519 secret key; resolved at runtime.

    /**
     * @brief Decrypt the vault at keyfile and populate client_pubkey / client_seckey.
     * No-op if keyfile is empty.
     * @param prod_uid  Producer UID — used as Argon2id KDF domain separator.
     * @param password  Vault password.
     * @return true if keys were loaded; false if keyfile absent (no CURVE auth).
     * @throws std::runtime_error if vault exists but decryption fails.
     */
    bool load_keypair(const std::string &prod_uid, const std::string &password);
};

// ============================================================================
// ProducerConfig
// ============================================================================

struct ProducerConfig
{
    std::string producer_uid;
    std::string producer_name;
    std::string log_level{"info"};

    /// Hub directory. When non-empty, broker endpoint and pubkey are loaded
    /// from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
    std::string hub_dir;
    std::string broker{"tcp://127.0.0.1:5570"};
    std::string broker_pubkey;

    /// Output channel (produced by this producer).
    std::string channel;

    /// Target start-to-start period between successive on_produce calls (ms).
    /// 0 = free-run (no sleep, produce as fast as SHM allows).
    /// When > 0, activates DataBlock overrun detection at the same period.
    int target_period_ms{100};

    /// Loop timing policy. Default: FixedRate (since target_period_ms defaults to 100).
    /// See loop_timing_policy.hpp for cross-field constraints.
    LoopTimingPolicy loop_timing{LoopTimingPolicy::FixedRate};

    /// SHM acquire-write timeout (ms). -1 = block up to 5 s.
    int timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // ── Transport ──────────────────────────────────────────────────────────────
    /// Data-plane transport for the output channel. Default: Shm.
    Transport transport{Transport::Shm};

    /// ZMQ PUSH endpoint. Required when transport == Zmq.
    /// Example: "tcp://0.0.0.0:5581"
    std::string zmq_out_endpoint;

    /// If true, the PUSH socket binds (stable side); if false, it connects.
    /// Default true: producer owns the endpoint and consumers connect to it.
    bool zmq_out_bind{true};

    /// Internal send-ring buffer depth (items) for the ZMQ PUSH path.
    /// write_acquire() is non-blocking when space is available; OverflowPolicy::Drop applies.
    size_t zmq_buffer_depth{64};

    /// Struct alignment for ZMQ-transport schema buffers.
    /// "aligned" (C-struct natural alignment, default) or "packed" (no padding, _pack_=1).
    /// Must match the ZmqQueue packing on the consumer side.
    std::string zmq_packing{"aligned"};

    // SHM — output side
    bool     shm_enabled{true};
    uint64_t shm_secret{0};
    /// SHM ring-buffer capacity. Must be > 0 when shm_enabled.
    /// Default 8: producers typically write faster than scripts consume, so a small buffer
    /// absorbs bursts without dropping frames. Processors use 4 (smaller pipeline buffer)
    /// because the processor loop is tightly coupled to input availability.
    uint32_t shm_slot_count{8};

    // Schemas
    nlohmann::json slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // Inbox — optional typed message receiver (Phase 3)
    nlohmann::json inbox_schema_json{};             ///< Schema for inbox messages. Null/empty = no inbox.
    std::string    inbox_endpoint;                  ///< ROUTER bind endpoint. Empty = auto-assign (port 0).
    size_t         inbox_buffer_depth{64};          ///< ZMQ RCVHWM for the inbox socket.
    std::string    inbox_overflow_policy{"drop"};   ///< "drop" (finite RCVHWM) or "block" (unlimited queue).

    /// Returns true when an inbox is configured (inbox_schema_json is non-null and non-empty).
    [[nodiscard]] bool has_inbox() const noexcept
        { return !inbox_schema_json.is_null() && !inbox_schema_json.empty(); }

    // Script
    std::string script_type{"python"};   ///< Language selector: "python" or "lua".
    std::string script_path{"."}; ///< Parent dir of the script/<type>/ package.
    std::string role_dir;         ///< Absolute base of the role directory (set by from_directory(); empty from from_json_file()).
    bool script_type_explicit{false}; ///< True when "type" was present in JSON; false = defaulted.

    // Auth
    ProducerAuthConfig auth{};

    // Validation
    bool update_checksum{true};       ///< Update BLAKE2b checksum on commit.
    bool stop_on_script_error{false}; ///< Fatal on Python exception in callback.

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    /// Roles that must be present in the broker before on_init is called.
    /// Each entry blocks start_role() for up to timeout_ms milliseconds.
    std::vector<WaitForRole> wait_for_roles;

    // ── Peer/hub-dead monitoring ────────────────────────────────────────────
    size_t ctrl_queue_max_depth{256}; ///< Max depth of ctrl send queue before oldest dropped.
    int    peer_dead_timeout_ms{30000}; ///< Peer (consumer) silence timeout (ms). 0=disabled.

    /**
     * @brief Load and validate a JSON config file.
     * @throws std::runtime_error on file-not-found, parse error, or missing fields.
     */
    static ProducerConfig from_json_file(const std::string &path);

    /**
     * @brief Load from a producer directory.
     *
     * Reads <prod_dir>/producer.json, then checks for "hub_dir" and loads
     * broker endpoint + pubkey from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
     *
     * @throws std::runtime_error on file-not-found, parse error, or missing fields.
     */
    static ProducerConfig from_directory(const std::string &prod_dir);
};

} // namespace pylabhub::producer
