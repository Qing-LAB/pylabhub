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
 *   "channel":     "lab.sensors.temperature",
 *   "interval_ms": 100,
 *   "timeout_ms":  5000,
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

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace pylabhub::producer
{

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

    /// Timer interval between on_produce calls (ms). 0 = free-run (no sleep).
    int interval_ms{100};

    /// SHM acquire-write timeout (ms). -1 = block up to 5 s.
    int timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // SHM — output side
    bool     shm_enabled{true};
    uint64_t shm_secret{0};
    uint32_t shm_slot_count{8}; ///< Must be > 0 when shm_enabled.

    // Schemas
    nlohmann::json slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // Script
    std::string script_type{"python"};   ///< Language selector: "python" or "lua".
    std::string script_path{"."}; ///< Parent dir of the script/<type>/ package.

    // Auth
    ProducerAuthConfig auth{};

    // Validation
    bool update_checksum{true};       ///< Update BLAKE2b checksum on commit.
    bool stop_on_script_error{false}; ///< Fatal on Python exception in callback.

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
