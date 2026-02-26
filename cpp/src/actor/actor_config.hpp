#pragma once
/**
 * @file actor_config.hpp
 * @brief Multi-role actor configuration — loaded from a JSON config file.
 *
 * ## JSON format
 *
 * ## Standard actor directory layout
 *
 * The canonical layout created by `pylabhub-actor --init <actor_dir>`:
 *
 * @code
 * <actor_dir>/
 *   actor.json          ← actor identity + all role configs
 *   roles/
 *     <role_name>/      ← one subdirectory per role (standard convention)
 *       script/         ← Python package directory (module name = "script")
 *         __init__.py   ← callbacks: on_init / on_iteration / on_stop
 *         helpers.py    ← optional helper submodules (from . import helpers)
 *   logs/               ← rotating log files (created at startup)
 *   run/
 *     actor.pid         ← PID of the running process
 * @endcode
 *
 * `hub_dir` in `actor.json` is the only link from actor to hub:
 *   - `<hub_dir>/hub.json`   → `broker_endpoint`
 *   - `<hub_dir>/hub.pubkey` → broker CurveZMQ public key (Z85, 40 chars)
 *
 * ## JSON format
 *
 * @code{.json}
 * {
 *   "hub_dir": "/opt/pylabhub/hubs/lab",
 *
 *   "actor": {
 *     "uid":       "ACTOR-SENSOR-12345678",
 *     "name":      "TemperatureSensor",
 *     "log_level": "info",
 *     "auth": {
 *       "keyfile":  "~/.pylabhub/sensor_node.key",
 *       "password": "env:PLH_ACTOR_PASSWORD"
 *     }
 *   },
 *
 *   "roles": {
 *     "raw_out": {
 *       "kind":        "producer",
 *       "channel":     "lab.sensor.temperature",
 *       "interval_ms": 100,
 *       "loop_timing": "fixed_pace",
 *       "loop_trigger": "shm",
 *       "script": {"module": "raw_out", "path": "./roles"},
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
 *       "timeout_ms": 5000,
 *       "loop_timing": "fixed_pace",
 *       "script": {"module": "cfg_in", "path": "./roles"},
 *       "slot_schema": {
 *         "fields": [{"name": "setpoint", "type": "float32"}]
 *       }
 *     }
 *   }
 * }
 * @endcode
 *
 * ## Script resolution (per-role vs. actor-level)
 *
 * Each role resolves its Python package via a two-level lookup:
 *   1. **Per-role** (preferred): `"script"` key inside the role config object.
 *      Standard convention: `{"module": "script", "path": "./roles/<role_name>"}`.
 *      This loads `./roles/<role_name>/script/__init__.py` as a Python package.
 *      The package is loaded via `importlib.util.spec_from_file_location` under a
 *      role-unique alias `_plh_{uid_hex}_{role_name}`, fully isolating it from other
 *      roles — even those using the same module name.
 *      Relative imports within the package (`from . import helpers`) work normally.
 *   2. **Actor-level fallback**: top-level `"script"` block (shared across all roles).
 *      Useful for single-role actors or actors that dispatch on `api.role_name()`.
 *
 * A role with no reachable `on_iteration` function is skipped (logged as a warning).
 *
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

    /**
     * @brief Loop thread blocking strategy (Phase 1 implementation).
     *
     * | Policy    | Loop thread blocks on                          | Requires SHM |
     * |-----------|------------------------------------------------|--------------|
     * | Shm       | acquire_*_slot(timeout)                        | Yes          |
     * | Messenger | incoming_cv_.wait_for(messenger_poll_ms)        | No           |
     *
     * JSON: `"loop_trigger": "shm"` (default) | `"messenger"`.
     */
    enum class LoopTrigger
    {
        Shm,      ///< Phase 1: block on acquire_*_slot(timeout) [default; requires SHM]
        Messenger ///< Phase 1: block on incoming_cv_.wait_for(messenger_poll_ms)
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
    ///  -1  = reserved; currently treated as 0 (runs at SHM slot rate)
    int interval_ms{0};

    /// Deadline advancement policy for the write loop (interval_ms > 0 only).
    LoopTimingPolicy loop_timing{LoopTimingPolicy::FixedPace};

    /// Loop thread blocking strategy. `Shm` requires `has_shm = true`.
    LoopTrigger loop_trigger{LoopTrigger::Shm};

    /// For `loop_trigger = Messenger`: wait timeout per iteration (ms).
    /// Values ≥ 10 produce a warning at config load.
    int messenger_poll_ms{5};

    /// Heartbeat interval (ms). 0 = 10 × interval_ms. Phase 2 acts on this.
    int heartbeat_interval_ms{0};

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

    // ── Script (per-role, optional) ───────────────────────────────────────────
    /// Python module/package name for this role.  Empty = use actor-level "script" fallback.
    /// Standard: `"script"` (loads `<script_base_dir>/script/__init__.py` as a package).
    std::string script_module{};
    /// Directory that is the parent of the `script/` package.  Empty = actor-level fallback.
    /// Standard: `"./roles/<role_name>"` so that `./roles/<role_name>/script/__init__.py`
    /// is the resolved entry point.
    std::string script_base_dir{};
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
    std::string script_module;                                ///< Python module name (e.g. "sensor_node")
    std::string script_base_dir;                              ///< Directory prepended to sys.path
    std::string log_level{"info"};                            ///< debug/info/warn/error
    ActorAuthConfig auth{};                                   ///< Optional CurveZMQ identity

    /// Hub directory path resolved by from_directory(); empty in flat-config mode.
    /// When non-empty, broker endpoint and broker_pubkey for all roles are overridden
    /// from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
    std::string hub_dir{};

    std::unordered_map<std::string, RoleConfig> roles;        ///< Named role map

    /**
     * @brief Load and validate a JSON config file.
     * @throws std::runtime_error on file-not-found, parse error, or missing
     *         required fields.
     */
    static ActorConfig from_json_file(const std::string &path);

    /**
     * @brief Load from an actor directory.
     *
     * Reads <actor_dir>/actor.json using from_json_file(), then checks for a
     * top-level "hub_dir" key. When present, reads <hub_dir>/hub.json for the
     * broker endpoint and <hub_dir>/hub.pubkey for the CurveZMQ public key,
     * overriding broker + broker_pubkey in every role.
     *
     * @throws std::runtime_error on file-not-found, parse error, or missing
     *         hub.broker_endpoint.
     */
    static ActorConfig from_directory(const std::string &actor_dir);
};

} // namespace pylabhub::actor
