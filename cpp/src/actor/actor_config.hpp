#pragma once
/**
 * @file actor_config.hpp
 * @brief ActorConfig — JSON-driven configuration for pylabhub-actor.
 *
 * ## Minimal producer example (schema mode)
 * @code{.json}
 * {
 *   "role": "producer",
 *   "channel": "lab.sensor.temperature",
 *   "broker": "tcp://127.0.0.1:5570",
 *   "script": "my_producer.py",
 *   "slot_schema": {
 *     "fields": [
 *       {"name": "count", "type": "int64"},
 *       {"name": "ts",    "type": "float64"},
 *       {"name": "value", "type": "float32"}
 *     ]
 *   },
 *   "flexzone_schema": {
 *     "fields": [
 *       {"name": "device_id",   "type": "uint16"},
 *       {"name": "sample_rate", "type": "uint32"}
 *     ]
 *   },
 *   "shm": {
 *     "enabled":    true,
 *     "slot_count": 4,
 *     "secret":     0
 *   },
 *   "validation": {
 *     "slot_checksum":     "enforce",
 *     "flexzone_checksum": "enforce",
 *     "on_checksum_fail":  "pass",
 *     "on_python_error":   "continue"
 *   },
 *   "log_level": "info"
 * }
 * @endcode
 *
 * ## Backward-compatible legacy mode (shm.slot_size, no slot_schema)
 * If `slot_schema` is absent and `shm.slot_size` is present, a raw
 * bytearray / bytes mode is used (deprecated — prefer slot_schema).
 */

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace pylabhub::actor
{

// ============================================================================
// ValidationPolicy
// ============================================================================

/**
 * @struct ValidationPolicy
 * @brief Per-cycle checksum and error-handling policies.
 */
struct ValidationPolicy
{
    /**
     * @enum Checksum
     * @brief When to run slot or flexzone checksum operations.
     *
     * - None:    No checksum calls made.
     * - Update:  Producer writes checksum; consumer does NOT verify.
     * - Enforce: Producer writes checksum; consumer verifies before on_read.
     *            Default — this is the normal operating mode.
     */
    enum class Checksum
    {
        None,
        Update,
        Enforce
    };

    /**
     * @enum OnFail
     * @brief What the actor does when a slot checksum verification fails.
     *
     * - Skip: Discard the slot; do NOT call on_read(). Log a Cat 2 warning.
     * - Pass: Call on_read() with api.slot_valid() == false. Default —
     *         the script decides whether to accept the data.
     */
    enum class OnFail
    {
        Skip,
        Pass
    };

    /**
     * @enum OnPyError
     * @brief What the actor does when a Python callback raises an exception.
     *
     * - Continue: Log the full traceback and keep running.
     * - Stop:     Log the traceback and stop the actor cleanly.
     */
    enum class OnPyError
    {
        Continue,
        Stop
    };

    Checksum  slot_checksum{Checksum::Enforce};    ///< Slot checksum policy.
    Checksum  flexzone_checksum{Checksum::Enforce};///< FlexZone checksum policy.
    OnFail    on_checksum_fail{OnFail::Pass};       ///< What to do on slot checksum failure.
    OnPyError on_python_error{OnPyError::Continue}; ///< What to do on Python exception.
};

// ============================================================================
// ActorConfig
// ============================================================================

/**
 * @struct ActorConfig
 * @brief All configuration for one producer or consumer actor instance.
 *
 * Loaded from a JSON file via `from_json_file()`.
 * Required fields: role, channel, script.
 */
struct ActorConfig
{
    enum class Role
    {
        Producer, ///< Publishes on a channel
        Consumer  ///< Subscribes to a channel
    };

    Role        role{Role::Producer};
    std::string channel_name;
    std::string broker_endpoint{"tcp://127.0.0.1:5570"};
    std::string script_path;

    // ── Schema (new; preferred) ───────────────────────────────────────────────
    nlohmann::json slot_schema_json{};     ///< Parsed "slot_schema" JSON object (or null)
    nlohmann::json flexzone_schema_json{}; ///< Parsed "flexzone_schema" JSON object (or null)

    // ── SHM (shared memory DataBlock) — optional ──────────────────────────────
    bool     has_shm{false};
    uint64_t shm_secret{0};          ///< Shared secret for SHM create/attach
    uint32_t shm_slot_count{4};      ///< Ring buffer capacity

    /// Legacy: slot byte size when no slot_schema is present.
    /// 0 means "compute from slot_schema at start()". Non-zero + no schema =
    /// raw bytearray mode (deprecated — will be removed in a future release).
    uint32_t shm_slot_size{0};

    // ── Validation ────────────────────────────────────────────────────────────
    ValidationPolicy validation{};

    // ── Producer-specific ─────────────────────────────────────────────────────
    /// Interval between on_write() calls in ZMQ-only mode (ms). 0 = as fast as possible.
    uint32_t write_interval_ms{0};

    // ── Logging ───────────────────────────────────────────────────────────────
    std::string log_level{"info"};

    /**
     * @brief Load and validate a JSON config file.
     * @throws std::runtime_error on file-not-found, parse error, or missing
     *         required fields.
     */
    static ActorConfig from_json_file(const std::string &path);
};

} // namespace pylabhub::actor
