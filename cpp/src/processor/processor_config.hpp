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
 *     "auth": {
 *       "keyfile": ""
 *     }
 *   },
 *
 *   "in_channel":  "lab.raw.data",
 *   "out_channel": "lab.scaled.data",
 *   "overflow_policy": "block",
 *   "timeout_ms": -1,
 *
 *   "in_slot_schema":  { "fields": [{"name": "value", "type": "float32"}] },
 *   "out_slot_schema": { "fields": [{"name": "value", "type": "float32"}] },
 *
 *   "shm": {
 *     "in":  { "enabled": true, "secret": 0 },
 *     "out": { "enabled": true, "secret": 0, "slot_count": 4 }
 *   },
 *
 *   "script": { "path": "." }
 * }
 * @endcode
 */

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace pylabhub::processor
{

// ============================================================================
// OverflowPolicy
// ============================================================================

enum class OverflowPolicy
{
    Block, ///< Wait (up to 5 s) for a free output slot before calling on_process.
    Drop   ///< Skip the input slot immediately; increment out_drop_count().
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

    /// Input slot acquire timeout (ms). -1 = block up to 5 s (default).
    int timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // SHM — input side
    bool     in_shm_enabled{true};
    uint64_t in_shm_secret{0};

    // SHM — output side
    bool     out_shm_enabled{true};
    uint64_t out_shm_secret{0};
    uint32_t out_shm_slot_count{4}; ///< Must be > 0 when out_shm_enabled.

    // Schemas
    nlohmann::json in_slot_schema_json{};
    nlohmann::json out_slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // Script
    std::string script_type{"python"};   ///< Language selector: `"python"` or `"lua"`.
    std::string script_path{"./script"}; ///< Parent dir of the script/ package.

    // Auth
    ProcessorAuthConfig auth{};

    // Validation
    bool update_checksum{true};      ///< Update BLAKE2b checksum on commit.
    bool stop_on_script_error{false}; ///< Fatal on Python exception in callback.

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
