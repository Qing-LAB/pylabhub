#pragma once
/**
 * @file actor_config.hpp
 * @brief Multi-role actor configuration — loaded from a JSON config file.
 *
 * ## JSON format
 *
 * @code{.json}
 * {
 *   "actor": {
 *     "uid":       "sensor_node_001",
 *     "name":      "TemperatureSensor",
 *     "log_level": "info",
 *     "auth": {
 *       "keyfile":  "~/.pylabhub/sensor_node_001.key",
 *       "password": "env:PLH_ACTOR_PASSWORD"
 *     }
 *   },
 *   "script": "sensor_node.py",
 *
 *   "roles": {
 *     "raw_out": {
 *       "kind":        "producer",
 *       "channel":     "lab.sensor.temperature",
 *       "broker":      "tcp://127.0.0.1:5570",
 *       "interval_ms": 100,
 *       "loop_timing": "fixed_pace",
 *       "slot_schema": {
 *         "packing": "natural",
 *         "fields": [
 *           {"name": "ts",    "type": "float64"},
 *           {"name": "value", "type": "float32"},
 *           {"name": "flags", "type": "uint8"}
 *         ]
 *       },
 *       "flexzone_schema": {
 *         "fields": [
 *           {"name": "device_id",   "type": "uint16"},
 *           {"name": "sample_rate", "type": "uint32"},
 *           {"name": "label",       "type": "string", "length": 32}
 *         ]
 *       },
 *       "shm": {"enabled": true, "slot_count": 8, "secret": 0},
 *       "validation": {
 *         "slot_checksum":     "update",
 *         "flexzone_checksum": "update",
 *         "on_checksum_fail":  "skip",
 *         "on_python_error":   "continue"
 *       }
 *     },
 *
 *     "cfg_in": {
 *       "kind":       "consumer",
 *       "channel":    "lab.config.setpoints",
 *       "broker":     "tcp://127.0.0.1:5570",
 *       "timeout_ms": 5000,
 *       "loop_timing": "fixed_pace",
 *       "slot_schema": {
 *         "fields": [{"name": "setpoint", "type": "float32"}]
 *       }
 *     }
 *   }
 * }
 * @endcode
 *
 * ## Backward-compatible single-role format
 *
 * The old single-role format (flat JSON with "role", "channel", "broker",
 * "script") is still parsed: it is treated as a single-role actor whose role
 * name is the value of "channel" (for producers) or "channel" (consumers).
 * A deprecation warning is logged; prefer the new "roles" map format.
 */

#include "utils/data_block.hpp" // hub::LoopPolicy enum (DataBlock pacing)

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace pylabhub::actor
{

// ============================================================================
// ValidationPolicy
// ============================================================================

/**
 * @struct ValidationPolicy
 * @brief Per-cycle checksum and error-handling policies for one role.
 */
struct ValidationPolicy
{
    enum class Checksum
    {
        None,    ///< No checksum calls
        Update,  ///< Producer writes checksum; consumer does NOT verify
        Enforce  ///< Producer writes; consumer verifies before on_read
    };

    enum class OnFail
    {
        Skip, ///< Discard slot; do NOT call on_read(). Log Cat 2 warning.
        Pass  ///< Call on_read() with api.slot_valid() == false.
    };

    enum class OnPyError
    {
        Continue, ///< Log full traceback and keep running
        Stop      ///< Log traceback and stop the actor cleanly
    };

    Checksum  slot_checksum{Checksum::Update};
    Checksum  flexzone_checksum{Checksum::Update};
    OnFail    on_checksum_fail{OnFail::Skip};
    OnPyError on_python_error{OnPyError::Continue};
};

// ============================================================================
// ActorAuthConfig
// ============================================================================

/**
 * @struct ActorAuthConfig
 * @brief Optional NaCl keypair auth for actor identity on ZMQ connections.
 *
 * When keyfile is non-empty, the actor uses CurveZMQ CURVE client mode for
 * all broker connections. The private key is protected by password.
 * "env:VAR" in the password field reads from the named environment variable.
 */
struct ActorAuthConfig
{
    std::string keyfile;   ///< Path to NaCl keypair file; empty = no CURVE auth
    std::string password;  ///< Passphrase; "env:VAR" reads $VAR at startup

    // Resolved at runtime by load_keypair() — never parsed from the main JSON config:
    std::string client_pubkey;  ///< Z85 CURVE25519 public key (40 chars); empty = no client auth
    std::string client_seckey;  ///< Z85 CURVE25519 secret key (40 chars); never stored in JSON

    /**
     * @brief Load public and secret keys from the keyfile JSON into client_pubkey / client_seckey.
     *
     * No-op if keyfile is empty. Logs LOGGER_WARN and returns false on any error
     * (file not found, malformed JSON, wrong key length). Must be called after the
     * Logger lifecycle module is initialized.
     *
     * @return true if keys were loaded successfully, false otherwise.
     */
    bool load_keypair();
};

// ============================================================================
// RoleConfig
// ============================================================================

/**
 * @struct RoleConfig
 * @brief Configuration for a single named role (producer or consumer).
 */
struct RoleConfig
{
    enum class Kind { Producer, Consumer };

    /**
     * @brief Write-loop deadline advancement policy (producer only; ignored for consumers).
     *
     * Controls how the next write deadline is set after each iteration completes.
     *
     * | Policy        | Formula                          | Behaviour on overrun            |
     * |---------------|----------------------------------|---------------------------------|
     * | FixedPace     | next = now() + interval          | Resets from actual wakeup time; |
     * |               |                                  | no catch-up burst; rate ≤ target|
     * | Compensating  | next += interval                 | Advances by one tick regardless;|
     * |               |                                  | fires immediately after overrun;|
     * |               |                                  | average rate converges to target|
     *
     * JSON: `"loop_timing": "fixed_pace"` (default) | `"compensating"`.
     */
    enum class LoopTimingPolicy
    {
        FixedPace,   ///< next_deadline = now() + interval  — safe default; no catch-up
        Compensating ///< next_deadline += interval         — catches up after slow writes
    };

    Kind        kind{Kind::Producer};
    std::string channel;
    std::string broker{"tcp://127.0.0.1:5570"};
    /// Broker CurveZMQ public key (Z85, 40 chars). Empty = no CURVE auth.
    std::string broker_pubkey;

    // ── Producer-specific ─────────────────────────────────────────────────────
    /// Write loop interval in ms.
    ///   0  = as fast as SHM slots allow (no sleep)
    ///  >0  = deadline-scheduled writes; see loop_timing for overrun policy
    ///  -1  = write only on api.trigger_write()
    int interval_ms{0};

    /// Deadline advancement policy for the write loop (interval_ms > 0 only).
    LoopTimingPolicy loop_timing{LoopTimingPolicy::FixedPace};

    // ── DataBlock LoopPolicy (HEP-CORE-0008) ──────────────────────────────────
    /// DataBlock-layer acquire pacing policy. Controls overrun detection and
    /// (in a future pass) the RAII SlotIterator sleep.
    /// JSON: "loop_policy": "max_rate" | "fixed_rate"  (default: max_rate)
    hub::LoopPolicy loop_policy{hub::LoopPolicy::MaxRate};

    /// Target start-to-start period for LoopPolicy::FixedRate.
    /// JSON: "period_ms": <int>  (0 = same as MaxRate)
    std::chrono::milliseconds period_ms{0};

    // ── Consumer-specific ─────────────────────────────────────────────────────
    /// Read loop timeout in ms.
    ///  -1  = wait indefinitely for a slot (no timed_out callbacks)
    ///  >0  = call on_read(slot=None, timed_out=True) after N ms of silence;
    ///        see loop_timing for how the next timeout window is scheduled
    int timeout_ms{-1};

    // ── SHM ───────────────────────────────────────────────────────────────────
    bool     has_shm{false};
    uint64_t shm_secret{0};
    uint32_t shm_slot_count{4};

    /// Legacy: raw slot size when no slot_schema is present (deprecated).
    uint32_t shm_slot_size{0};

    // ── Schema ────────────────────────────────────────────────────────────────
    nlohmann::json slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // ── Validation ────────────────────────────────────────────────────────────
    ValidationPolicy validation{};
};

// ============================================================================
// ActorConfig
// ============================================================================

/**
 * @struct ActorConfig
 * @brief Top-level actor configuration.
 *
 * One actor has a single identity (uid/name) and a map of named roles.
 * Each role is either a producer or a consumer. Multiple roles may connect
 * to different channels and brokers.
 *
 * Load from a JSON file with `from_json_file()`.
 */
struct ActorConfig
{
    std::string actor_uid;                                    ///< Stable unique ID (UUID or custom)
    std::string actor_name;                                   ///< Human-readable name
    std::string script_path;                                  ///< Python script path
    std::string log_level{"info"};                            ///< debug/info/warn/error
    ActorAuthConfig auth{};                                   ///< Optional CurveZMQ identity

    std::unordered_map<std::string, RoleConfig> roles;        ///< Named role map

    /**
     * @brief Load and validate a JSON config file.
     * @throws std::runtime_error on file-not-found, parse error, or missing
     *         required fields.
     */
    static ActorConfig from_json_file(const std::string &path);
};

} // namespace pylabhub::actor
