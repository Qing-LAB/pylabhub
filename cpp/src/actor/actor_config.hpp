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
    /**
     * @enum ValidationPolicy::Checksum
     * @brief Per-cycle checksum computation and verification policy for one role.
     *
     * Applied independently to the slot data region (`slot_checksum`) and the
     * flexible zone (`flexzone_checksum`) via two separate fields in ValidationPolicy.
     *
     * **Where set:** actor_config.cpp — parse_checksum() parses JSON string values.
     *   JSON keys inside role `"validation"` block:
     *   `"slot_checksum"`: "none" | "update" (default) | "enforce"
     *   `"flexzone_checksum"`: "none" | "update" (default) | "enforce"
     * **Where applied:** actor_host.cpp —
     *   ProducerRoleWorker: calls update_slot_checksum() / update_flexzone_checksum()
     *     on the slot before committing, when Checksum != None.
     *   ConsumerRoleWorker: calls verify_slot_checksum() / verify_flexzone_checksum()
     *     on the acquired slot when Checksum == Enforce; flags api.slot_valid() = false
     *     on mismatch (behaviour then determined by skip_on_validation_error).
     *
     * | Value   | Producer side                  | Consumer side                       |
     * |---------|-------------------------------|--------------------------------------|
     * | None    | No checksum written            | No checksum verified                 |
     * | Update  | Writes BLAKE2b hash on commit  | Does NOT verify — slot used as-is    |
     * | Enforce | Writes BLAKE2b hash on commit  | Verifies hash; sets slot_valid=false |
     * |         |                                | and logs Cat-2 warning on mismatch   |
     *
     * **Design doc:** HEP-CORE-0007-DataHub-Protocol-and-Policy.md §5.2
     */
    enum class Checksum
    {
        None,    ///< No checksum operations — fastest path, no integrity guarantee
        Update,  ///< Producer writes hash; consumer reads without verification (default)
        Enforce  ///< Producer writes hash; consumer verifies; mismatch → slot_valid=false
    };

    Checksum slot_checksum{Checksum::Update};
    Checksum flexzone_checksum{Checksum::Update};
    /// true (default): discard slot and log Cat 2 warning on checksum failure.
    /// false: call on_iteration() with api.slot_valid() == false.
    bool skip_on_validation_error{true};
    /// false (default): log traceback and keep running on Python exception.
    /// true: log traceback and stop the actor cleanly.
    bool stop_on_python_error{false};
};

// ============================================================================
// ActorAuthConfig
// ============================================================================

/**
 * @struct ActorAuthConfig
 * @brief Optional NaCl keypair auth for actor identity on ZMQ connections.
 *
 * When keyfile is non-empty, the actor vault is decrypted at startup using the
 * password provided interactively or via PYLABHUB_ACTOR_PASSWORD env var.
 * No password is stored in any config file.
 */
struct ActorAuthConfig
{
    std::string keyfile;  ///< Path to encrypted actor vault file; empty = no CURVE auth

    // Resolved at runtime by load_keypair() — never parsed from JSON config:
    std::string client_pubkey;  ///< Z85 CURVE25519 public key (40 chars); empty = no client auth
    std::string client_seckey;  ///< Z85 CURVE25519 secret key (40 chars); never stored on disk

    /**
     * @brief Decrypt the vault at keyfile and populate client_pubkey / client_seckey.
     *
     * No-op if keyfile is empty (ephemeral CURVE identity).
     * Logs LOGGER_WARN and returns false if the vault file does not exist.
     * Throws std::runtime_error (fatal) if the file exists but decryption fails
     * (wrong password or corrupted vault).
     * Must be called after the Logger lifecycle module is initialized.
     *
     * @param actor_uid  Actor UID — used as Argon2id KDF domain separator.
     * @param password   Vault password (already resolved from env or prompt by caller).
     * @return true if keys were loaded, false if keyfile is absent (no CURVE auth).
     */
    bool load_keypair(const std::string &actor_uid, const std::string &password);
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
    /**
     * @enum RoleConfig::Kind
     * @brief Fundamental role type — determines which DataBlock API and loop path to use.
     *
     * **Where set:** actor_config.cpp — JSON `"kind"` field inside a role object.
     * **Where checked:** actor_host.cpp — ActorHost::create_role_worker() dispatches
     *   on this value to instantiate either ProducerRoleWorker or ConsumerRoleWorker.
     *
     * | Value    | DataBlock / hub API              | Loop worker class         |
     * |----------|----------------------------------|---------------------------|
     * | Producer | hub::Producer, DataBlockProducer | ProducerRoleWorker        |
     * | Consumer | hub::Consumer, DataBlockConsumer | ConsumerRoleWorker        |
     *
     * JSON values: `"producer"` | `"consumer"`. No default — must be explicit.
     */
    enum class Kind { Producer, Consumer };

    /**
     * @enum RoleConfig::LoopTimingPolicy
     * @brief Write-loop deadline advancement policy (producer only; ignored for consumers).
     *
     * Controls how the next iteration deadline is computed after on_iteration() returns.
     * This is the **actor layer** policy — distinct from hub::LoopPolicy, which is
     * the **DataBlock layer** policy that controls sleep inside acquire_write_slot().
     *
     * **Where set:** actor_config.cpp — JSON `"loop_timing"` field inside a role object.
     * **Where checked:** actor_host.cpp — ProducerRoleWorker::run_loop_thread_():
     *   after each on_iteration() call, the next deadline is computed based on this value.
     *   Only active when interval_ms > 0; ignored for interval_ms == 0 (max-rate mode).
     *
     * | Policy       | Formula                    | Behaviour on overrun                    |
     * |--------------|----------------------------|-----------------------------------------|
     * | FixedPace    | next = now() + interval_ms | Resets from actual wakeup time.         |
     * |              |                            | No catch-up burst. Rate ≤ target.       |
     * |              |                            | Suitable for most production uses.      |
     * | Compensating | next += interval_ms        | Advances deadline by one tick each run. |
     * |              |                            | Fires immediately after an overrun to   |
     * |              |                            | catch up. Average rate converges to     |
     * |              |                            | target over time. Use only when         |
     * |              |                            | average throughput matters more than    |
     * |              |                            | burst avoidance.                        |
     *
     * **Contrast with hub::LoopPolicy:**
     *   LoopTimingPolicy governs *when the next wakeup is scheduled* (actor layer).
     *   hub::LoopPolicy governs *how long acquire_write_slot() sleeps* (DataBlock layer).
     *
     * JSON: `"loop_timing": "fixed_pace"` (default) | `"compensating"`.
     * **Design doc:** HEP-CORE-0010-Actor-Thread-Model.md §3.2
     */
    enum class LoopTimingPolicy
    {
        FixedPace,   ///< next_deadline = now() + interval_ms  — safe default; no catch-up
        Compensating ///< next_deadline += interval_ms         — catches up after slow writes
    };

    /**
     * @enum RoleConfig::LoopTrigger
     * @brief What event drives each iteration of the role loop thread.
     *
     * Determines the primary blocking call in the zmq_thread_ and loop_thread_ pair
     * (HEP-CORE-0010 Phase 2). Both threads are always started; LoopTrigger controls
     * only which one acts as the iteration clock.
     *
     * **Where set:** actor_config.cpp — JSON `"loop_trigger"` field inside a role object.
     * **Where checked:** actor_host.cpp — ProducerRoleWorker::start() and
     *   ConsumerRoleWorker::start() use this to select run_loop_shm_() or
     *   run_loop_messenger_() as the loop body.
     *
     * | Value     | Loop thread blocks on                     | Requires SHM enabled |
     * |-----------|-------------------------------------------|----------------------|
     * | Shm       | acquire_*_slot(timeout_ms or interval_ms) | Yes (has_shm = true) |
     * | Messenger | incoming_queue_ condvar (messenger_poll_ms)| No                   |
     *
     * **Constraint:** `Shm` requires `"shm": {"enabled": true}` in the role config;
     *   parsing throws std::runtime_error if Shm is selected without SHM enabled.
     *
     * JSON: `"loop_trigger": "shm"` (default) | `"messenger"`.
     * **Design doc:** HEP-CORE-0010-Actor-Thread-Model.md §2.1
     */
    enum class LoopTrigger
    {
        Shm,      ///< Block on acquire_*_slot(timeout) — requires has_shm = true [default]
        Messenger ///< Block on incoming_queue_ condvar — messenger-only roles
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
    /// Scripting language type for this role.  Default: `"python"`.
    /// Future: `"lua"` — selects LuaScriptHost.
    /// JSON: `"script": {"type": "python", "module": "...", "path": "..."}`.
    std::string script_type{"python"};
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
