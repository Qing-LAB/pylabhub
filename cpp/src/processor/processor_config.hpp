#pragma once
/**
 * @file processor_config.hpp
 * @brief Standalone processor configuration — loaded from a flat JSON config file.
 *
 * ## Standard processor directory layout
 *
 * The canonical layout created by `pylabhub-processor --init <proc_dir>`:
 *
 * @code
 * <proc_dir>/
 *   processor.json       ← processor identity + channel config
 *   vault/
 *     processor.vault    ← encrypted NaCl keypair (optional)
 *   script/
 *     python/
 *       __init__.py      ← callbacks: on_init / on_process / on_stop
 *   logs/                ← rotating log files
 *   run/
 *     processor.pid      ← PID of the running process
 * @endcode
 *
 * ## JSON format
 *
 * @code{.json}
 * {
 *   "hub_dir": "/opt/pylabhub/hubs/lab",
 *
 *   "processor": {
 *     "uid":       "PROC-SCALER-12345678",
 *     "name":      "Scaler",
 *     "log_level": "info",
 *     "auth": { "keyfile": "" }
 *   },
 *
 *   "in_channel":       "lab.raw.data",
 *   "out_channel":      "lab.scaled.data",
 *   "overflow_policy":  "block",
 *   "target_period_ms": 0,
 *   "loop_timing":      "max_rate",
 *
 *   "in_transport":         "shm",
 *   "zmq_in_endpoint":      "",
 *   "zmq_in_bind":          false,
 *   "in_zmq_buffer_depth":  64,
 *   "in_zmq_packing":       "aligned",
 *
 *   "out_transport":        "shm",
 *   "zmq_out_endpoint":     "",
 *   "zmq_out_bind":         true,
 *   "out_zmq_buffer_depth": 64,
 *   "out_zmq_packing":      "aligned",
 *
 *   "in_slot_schema":  { "fields": [{"name": "value", "type": "float32"}] },
 *   "out_slot_schema": { "fields": [{"name": "value", "type": "float32"}] },
 *
 *   "inbox_schema":          null,
 *   "inbox_endpoint":        "",
 *   "inbox_buffer_depth":    64,
 *   "inbox_overflow_policy": "drop",
 *
 *   "shm": {
 *     "in":  { "enabled": true, "secret": 0 },
 *     "out": { "enabled": true, "secret": 0, "slot_count": 4 }
 *   },
 *
 *   "script": { "path": "." },
 *   "validation": { "update_checksum": true, "verify_checksum": false, "stop_on_script_error": false }
 * }
 * @endcode
 */

#include "utils/loop_timing_policy.hpp"
#include "utils/startup_wait.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pylabhub::processor
{

/// LoopTimingPolicy and WaitForRole are defined in shared headers.
using ::pylabhub::LoopTimingPolicy;
using ::pylabhub::WaitForRole;

// ============================================================================
// OverflowPolicy
// ============================================================================

enum class OverflowPolicy
{
    Block, ///< Wait (up to 5 s) for a free output slot before calling on_process.
    Drop   ///< Skip the input slot immediately; increment out_drop_count().
};

// ============================================================================
// Transport
// ============================================================================

enum class Transport
{
    Shm, ///< Shared-memory data path via DataBlock (default).
    Zmq  ///< Direct point-to-point ZMQ PUSH/PULL (no broker relay).
};

// ============================================================================
// ProcessorAuthConfig
// ============================================================================

struct ProcessorAuthConfig
{
    std::string keyfile;      ///< Path to encrypted processor vault; empty = no CURVE auth.
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); resolved at runtime.
    std::string client_seckey; ///< Z85 CURVE25519 secret key; resolved at runtime.

    /**
     * @brief Decrypt the vault at keyfile and populate client_pubkey / client_seckey.
     *
     * No-op if keyfile is empty.
     * @param proc_uid   Processor UID — used as Argon2id KDF domain separator.
     * @param password   Vault password.
     * @return true if keys were loaded; false if keyfile absent (no CURVE auth).
     * @throws std::runtime_error if vault exists but decryption fails.
     */
    bool load_keypair(const std::string &proc_uid, const std::string &password);
};

// ============================================================================
// ProcessorConfig
// ============================================================================

struct ProcessorConfig
{
    std::string processor_uid;
    std::string processor_name;
    std::string log_level{"info"};

    /// Hub directory. When non-empty, broker endpoint and pubkey are loaded
    /// from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
    std::string hub_dir;
    std::string broker{"tcp://127.0.0.1:5570"};
    std::string broker_pubkey;

    /// Per-direction hub directory overrides. When non-empty, the corresponding
    /// broker endpoint and pubkey are loaded from <dir>/hub.json + <dir>/hub.pubkey
    /// instead of from hub_dir.
    std::string in_hub_dir;
    std::string out_hub_dir;

    /// Per-direction broker overrides. When non-empty, take precedence over the
    /// global broker / broker_pubkey (and over hub_dir-derived values).
    std::string in_broker;
    std::string out_broker;
    std::string in_broker_pubkey;
    std::string out_broker_pubkey;

    /// @brief Resolve input broker endpoint: in_broker > broker.
    [[nodiscard]] const std::string &resolved_in_broker() const noexcept;
    /// @brief Resolve output broker endpoint: out_broker > broker.
    [[nodiscard]] const std::string &resolved_out_broker() const noexcept;
    /// @brief Resolve input broker pubkey: in_broker_pubkey > broker_pubkey.
    [[nodiscard]] const std::string &resolved_in_broker_pubkey() const noexcept;
    /// @brief Resolve output broker pubkey: out_broker_pubkey > broker_pubkey.
    [[nodiscard]] const std::string &resolved_out_broker_pubkey() const noexcept;

    /// Input channel (consumed by this processor).
    std::string in_channel;
    /// Output channel (produced by this processor).
    std::string out_channel;

    OverflowPolicy overflow_policy{OverflowPolicy::Block};

    /// Target start-to-start period between successive on_process calls (ms).
    /// 0 = free-run (no sleep between iterations). MaxRate requires period == 0.
    int target_period_ms{0};

    /// Loop timing policy. Default: MaxRate (since target_period_ms defaults to 0).
    LoopTimingPolicy loop_timing{LoopTimingPolicy::MaxRate};

    /// Input slot acquire timeout (ms). -1 = block up to 5 s (default).
    int timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // SHM — input side
    bool     in_shm_enabled{true};
    uint64_t in_shm_secret{0};

    // SHM — output side
    bool     out_shm_enabled{true};
    uint64_t out_shm_secret{0};
    /// Output SHM ring-buffer capacity. Must be > 0 when out_shm_enabled.
    /// Default 4: processor output is demand-driven (consumer pulls at its own rate),
    /// so a smaller buffer is sufficient. Producers use 8 to absorb burst writes.
    uint32_t out_shm_slot_count{4};

    // ── Transport — input side ─────────────────────────────────────────────────
    /// Input data path. Shm = auto-discovered via broker DISC_ACK (HEP-CORE-0021).
    /// Zmq = direct ZMQ PULL socket (bypasses broker data relay; control plane still active).
    Transport   in_transport{Transport::Shm};
    /// ZMQ PULL endpoint. Required when in_transport == Zmq.
    std::string zmq_in_endpoint;
    /// PULL default=connect (the upstream PUSH socket binds).
    bool        zmq_in_bind{false};
    /// Input ZMQ recv-ring buffer depth. Must be > 0.
    size_t      in_zmq_buffer_depth{64};
    /// Input ZMQ struct packing: "aligned" (C-struct default) or "packed" (no padding).
    std::string in_zmq_packing{"aligned"};

    // ── Transport — output side ────────────────────────────────────────────────
    Transport   out_transport{Transport::Shm};  ///< Output data path: Shm or Zmq.
    std::string zmq_out_endpoint;               ///< ZMQ endpoint for output PUSH socket.
    bool        zmq_out_bind{true};             ///< PUSH default = bind.
    /// Output ZMQ send-ring buffer depth. Must be > 0.
    size_t      out_zmq_buffer_depth{64};
    /// Output ZMQ struct packing: "aligned" (C-struct default) or "packed" (no padding).
    std::string out_zmq_packing{"aligned"};

    // ── Schemas ────────────────────────────────────────────────────────────────
    nlohmann::json in_slot_schema_json{};
    nlohmann::json out_slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // ── Inbox facility (optional) ──────────────────────────────────────────────
    /// Schema for inbox messages. Null/empty = no inbox.
    nlohmann::json inbox_schema_json{};
    /// ROUTER bind endpoint. Empty = auto-assign (port 0).
    std::string    inbox_endpoint;
    /// ZMQ RCVHWM for the inbox socket. Must be > 0.
    size_t         inbox_buffer_depth{64};
    /// Overflow policy for inbox: "drop" (finite RCVHWM) or "block" (unlimited queue).
    std::string    inbox_overflow_policy{"drop"};

    /// Returns true when an inbox is configured (inbox_schema_json is non-null and non-empty).
    [[nodiscard]] bool has_inbox() const noexcept
        { return !inbox_schema_json.is_null() && !inbox_schema_json.empty(); }

    // Script
    std::string script_type{"python"};   ///< Language selector: `"python"` or `"lua"`.
    std::string script_path{"."}; ///< Parent dir of the script/ package.
    bool script_type_explicit{false}; ///< True when "type" was present in JSON; false = defaulted.

    // Auth
    ProcessorAuthConfig auth{};

    // Validation
    bool update_checksum{true};        ///< Update BLAKE2b checksum on output slot commit.
    /// When true, verify BLAKE2b checksum on input slot acquire; returns nullptr on mismatch.
    /// SHM only (ZMQ transport has no checksum — TCP provides integrity).
    bool verify_checksum{false};
    bool stop_on_script_error{false};  ///< Fatal on Python exception in callback.

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    /// Roles that must be present in the broker before on_init is called.
    std::vector<WaitForRole> wait_for_roles;

    // ── Peer/hub-dead monitoring ────────────────────────────────────────────
    size_t ctrl_queue_max_depth{256}; ///< Max depth of ctrl send queue before oldest dropped.
    int    peer_dead_timeout_ms{30000}; ///< Peer silence timeout (ms). 0=disabled.

    /**
     * @brief Load and validate a JSON config file.
     * @throws std::runtime_error on file-not-found, parse error, or missing fields.
     */
    static ProcessorConfig from_json_file(const std::string &path);

    /**
     * @brief Load from a processor directory.
     *
     * Reads <proc_dir>/processor.json, then checks for "hub_dir" and loads
     * broker endpoint + pubkey from <hub_dir>/hub.json and <hub_dir>/hub.pubkey.
     *
     * @throws std::runtime_error on file-not-found, parse error, or missing fields.
     */
    static ProcessorConfig from_directory(const std::string &proc_dir);
};

} // namespace pylabhub::processor
