/**
 * @file producer_config.cpp
 * @brief ProducerConfig JSON parsing.
 */
#include "producer_config.hpp"

#include "utils/actor_vault.hpp"  // ActorVault::open (reused for producer vault)
#include "utils/uid_utils.hpp"    // has_producer_prefix, generate_producer_uid

#include <cstdio>   // std::fprintf (pre-lifecycle warnings)
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::producer
{

// ============================================================================
// ProducerAuthConfig::load_keypair
// ============================================================================

bool ProducerAuthConfig::load_keypair(const std::string &prod_uid,
                                       const std::string &password)
{
    if (keyfile.empty())
        return false;

    if (!std::filesystem::exists(keyfile))
    {
        std::fprintf(stderr,
                     "[prod] auth.keyfile '%s': not found — using ephemeral CURVE identity\n",
                     keyfile.c_str());
        return false;
    }

    const auto vault = pylabhub::utils::ActorVault::open(keyfile, prod_uid, password);
    client_pubkey = vault.public_key();
    client_seckey = vault.secret_key();
    std::fprintf(stderr, "[prod] Loaded producer vault from '%s' (pubkey: %.8s...)\n",
                 keyfile.c_str(), vault.public_key().c_str());
    return true;
}

// ============================================================================
// ProducerConfig::from_json_file
// ============================================================================

ProducerConfig ProducerConfig::from_json_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Producer config: cannot open '" + path + "'");

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(file);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "Producer config: JSON parse error in '" + path + "': " + e.what());
    }

    ProducerConfig cfg;

    // ── Producer identity ──────────────────────────────────────────────────────
    if (!j.contains("producer") || !j["producer"].is_object())
        throw std::runtime_error("Producer config: missing 'producer' object");

    const auto &prod = j["producer"];
    cfg.producer_uid  = prod.value("uid",       "");
    cfg.producer_name = prod.value("name",      "");
    cfg.log_level     = prod.value("log_level", "info");

    if (cfg.producer_uid.empty())
    {
        cfg.producer_uid = pylabhub::uid::generate_producer_uid(cfg.producer_name);
        std::fprintf(stderr,
                     "[prod] No 'producer.uid' in config — generated: %s\n"
                     "  Add this to producer.json to make the UID stable.\n",
                     cfg.producer_uid.c_str());
    }
    else if (!pylabhub::uid::has_producer_prefix(cfg.producer_uid))
    {
        std::fprintf(stderr,
                     "[prod] Warning: 'producer.uid' = '%s' does not start with 'PROD-'.\n",
                     cfg.producer_uid.c_str());
    }

    // ── Auth ──────────────────────────────────────────────────────────────────
    if (prod.contains("auth") && prod["auth"].is_object())
        cfg.auth.keyfile = prod["auth"].value("keyfile", "");

    // ── Hub dir / broker ──────────────────────────────────────────────────────
    if (j.contains("hub_dir") && j["hub_dir"].is_string())
        cfg.hub_dir = j["hub_dir"].get<std::string>();

    cfg.broker        = j.value("broker",       "tcp://127.0.0.1:5570");
    cfg.broker_pubkey = j.value("broker_pubkey", "");

    // ── Channel ───────────────────────────────────────────────────────────────
    cfg.channel = j.value("channel", "");
    if (cfg.channel.empty())
        throw std::runtime_error("Producer config: missing 'channel'");

    // ── Timing ────────────────────────────────────────────────────────────────
    cfg.interval_ms           = j.value("interval_ms",           100);
    cfg.timeout_ms            = j.value("timeout_ms",            -1);
    cfg.heartbeat_interval_ms = j.value("heartbeat_interval_ms", 0);

    if (cfg.interval_ms <= 0)
        throw std::runtime_error("Producer config: 'interval_ms' must be > 0");
    if (cfg.timeout_ms < -1)
        throw std::runtime_error("Producer config: 'timeout_ms' must be >= -1 (-1=infinite, 0=non-blocking, >0=ms)");

    // ── SHM ───────────────────────────────────────────────────────────────────
    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &shm  = j["shm"];
        cfg.shm_enabled   = shm.value("enabled",    true);
        cfg.shm_secret    = shm.value("secret",     uint64_t{0});
        cfg.shm_slot_count = shm.value("slot_count", uint32_t{8});
        if (cfg.shm_enabled && cfg.shm_slot_count == 0)
            throw std::runtime_error("Producer config: 'shm.slot_count' must be > 0");
    }

    // ── Schemas ───────────────────────────────────────────────────────────────
    if (j.contains("slot_schema") && !j["slot_schema"].is_null())
        cfg.slot_schema_json = j["slot_schema"];
    if (j.contains("flexzone_schema") && !j["flexzone_schema"].is_null())
        cfg.flexzone_schema_json = j["flexzone_schema"];

    // ── Script ────────────────────────────────────────────────────────────────
    if (j.contains("script") && j["script"].is_object())
    {
        cfg.script_type = j["script"].value("type", std::string{"python"});
        cfg.script_path = j["script"].value("path", std::string{"./script"});
    }

    // ── Validation ────────────────────────────────────────────────────────────
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &val = j["validation"];
        cfg.update_checksum      = val.value("update_checksum",      true);
        cfg.stop_on_script_error = val.value("stop_on_script_error", false);
    }

    return cfg;
}

// ============================================================================
// ProducerConfig::from_directory
// ============================================================================

ProducerConfig ProducerConfig::from_directory(const std::string &prod_dir)
{
    namespace fs = std::filesystem;

    const fs::path dir  = prod_dir;
    const fs::path json = dir / "producer.json";

    auto cfg = from_json_file(json.string());

    // Resolve hub_dir relative to prod_dir.
    if (!cfg.hub_dir.empty() && !fs::path(cfg.hub_dir).is_absolute())
        cfg.hub_dir = fs::weakly_canonical(dir / cfg.hub_dir).string();

    // Resolve script_path relative to prod_dir.
    if (!cfg.script_path.empty() && !fs::path(cfg.script_path).is_absolute())
        cfg.script_path = fs::weakly_canonical(dir / cfg.script_path).string();

    // ── Override broker from hub_dir ──────────────────────────────────────────
    if (!cfg.hub_dir.empty())
    {
        const fs::path hub_json   = fs::path(cfg.hub_dir) / "hub.json";
        const fs::path hub_pubkey = fs::path(cfg.hub_dir) / "hub.pubkey";

        std::ifstream f(hub_json);
        if (!f.is_open())
            throw std::runtime_error(
                "Producer config: cannot open hub.json at '" + hub_json.string() + "'");

        nlohmann::json hj = nlohmann::json::parse(f);
        cfg.broker = hj.at("hub").at("broker_endpoint").get<std::string>();

        if (fs::exists(hub_pubkey))
        {
            std::ifstream pk(hub_pubkey);
            std::getline(pk, cfg.broker_pubkey);
        }
    }

    return cfg;
}

} // namespace pylabhub::producer
