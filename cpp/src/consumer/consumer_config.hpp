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
 *   "channel":    "lab.sensors.temperature",
 *   "timeout_ms": 5000,
 *
 *   "slot_schema":     { "fields": [{"name": "value", "type": "float32"}] },
 *   "flexzone_schema": null,
 *
 *   "shm": { "enabled": true, "secret": 0 },
 *
 *   "script": { "path": "." }
 * }
 * @endcode
 */

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace pylabhub::consumer
{

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

    /// SHM acquire-consume timeout (ms). -1 = block up to 5 s (default).
    int timeout_ms{-1};
    int heartbeat_interval_ms{0};

    // SHM — input side
    bool     shm_enabled{true};
    uint64_t shm_secret{0};

    // Schemas
    nlohmann::json slot_schema_json{};
    nlohmann::json flexzone_schema_json{};

    // Script
    std::string script_type{"python"};
    std::string script_path{"."};

    // Auth
    ConsumerAuthConfig auth{};

    // Validation
    bool stop_on_script_error{false};

    static ConsumerConfig from_json_file(const std::string &path);
    static ConsumerConfig from_directory(const std::string &cons_dir);
};

} // namespace pylabhub::consumer
