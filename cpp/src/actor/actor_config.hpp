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

    Kind        kind{Kind::Producer};
    std::string channel;
    std::string broker{"tcp://127.0.0.1:5570"};

    // ── Producer-specific ─────────────────────────────────────────────────────
    /// Write loop interval in ms.
    ///   0  = as fast as SHM slots allow (no sleep)
    ///  >0  = sleep N ms between writes (best-effort poll)
    ///  -1  = write only on api.trigger_write()
    int interval_ms{0};

    // ── Consumer-specific ─────────────────────────────────────────────────────
    /// Read loop timeout in ms.
    ///  -1  = wait indefinitely for a slot
    ///  >0  = call on_read(slot=None, timed_out=True) after N ms of silence
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
