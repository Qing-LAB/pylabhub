/**
 * @file consumer_config.cpp
 * @brief ConsumerConfig JSON parsing.
 */
#include "consumer_config.hpp"

#include "utils/actor_vault.hpp"
#include "utils/uid_utils.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::consumer
{

bool ConsumerAuthConfig::load_keypair(const std::string &cons_uid,
                                       const std::string &password)
{
    if (keyfile.empty())
        return false;

    if (!std::filesystem::exists(keyfile))
    {
        std::fprintf(stderr,
                     "[cons] auth.keyfile '%s': not found — using ephemeral CURVE identity\n",
                     keyfile.c_str());
        return false;
    }

    const auto vault = pylabhub::utils::ActorVault::open(keyfile, cons_uid, password);
    client_pubkey = vault.public_key();
    client_seckey = vault.secret_key();
    std::fprintf(stderr, "[cons] Loaded consumer vault from '%s' (pubkey: %.8s...)\n",
                 keyfile.c_str(), vault.public_key().c_str());
    return true;
}

ConsumerConfig ConsumerConfig::from_json_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Consumer config: cannot open '" + path + "'");

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(file);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "Consumer config: JSON parse error in '" + path + "': " + e.what());
    }

    ConsumerConfig cfg;

    if (!j.contains("consumer") || !j["consumer"].is_object())
        throw std::runtime_error("Consumer config: missing 'consumer' object");

    const auto &cons = j["consumer"];
    cfg.consumer_uid  = cons.value("uid",       "");
    cfg.consumer_name = cons.value("name",      "");
    cfg.log_level     = cons.value("log_level", "info");

    if (cfg.consumer_uid.empty())
    {
        cfg.consumer_uid = pylabhub::uid::generate_consumer_uid(cfg.consumer_name);
        std::fprintf(stderr,
                     "[cons] No 'consumer.uid' in config — generated: %s\n"
                     "  Add this to consumer.json to make the UID stable.\n",
                     cfg.consumer_uid.c_str());
    }
    else if (!pylabhub::uid::has_consumer_prefix(cfg.consumer_uid))
    {
        std::fprintf(stderr,
                     "[cons] Warning: 'consumer.uid' = '%s' does not start with 'CONS-'.\n",
                     cfg.consumer_uid.c_str());
    }

    if (cons.contains("auth") && cons["auth"].is_object())
        cfg.auth.keyfile = cons["auth"].value("keyfile", "");

    if (j.contains("hub_dir") && j["hub_dir"].is_string())
        cfg.hub_dir = j["hub_dir"].get<std::string>();

    cfg.broker        = j.value("broker",       "tcp://127.0.0.1:5570");
    cfg.broker_pubkey = j.value("broker_pubkey", "");

    cfg.channel = j.value("channel", "");
    if (cfg.channel.empty())
        throw std::runtime_error("Consumer config: missing 'channel'");

    cfg.timeout_ms            = j.value("timeout_ms",            -1);
    cfg.heartbeat_interval_ms = j.value("heartbeat_interval_ms", 0);

    if (cfg.timeout_ms < -1)
        throw std::runtime_error("Consumer config: 'timeout_ms' must be >= -1 (-1=infinite, 0=non-blocking, >0=ms)");

    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &shm = j["shm"];
        cfg.shm_enabled = shm.value("enabled", true);
        cfg.shm_secret  = shm.value("secret",  uint64_t{0});
    }

    if (j.contains("slot_schema") && !j["slot_schema"].is_null())
        cfg.slot_schema_json = j["slot_schema"];
    if (j.contains("flexzone_schema") && !j["flexzone_schema"].is_null())
        cfg.flexzone_schema_json = j["flexzone_schema"];

    if (j.contains("script") && j["script"].is_object())
    {
        cfg.script_type = j["script"].value("type", std::string{"python"});
        cfg.script_path = j["script"].value("path", std::string{"./script"});
    }

    if (j.contains("validation") && j["validation"].is_object())
        cfg.stop_on_script_error =
            j["validation"].value("stop_on_script_error", false);

    return cfg;
}

ConsumerConfig ConsumerConfig::from_directory(const std::string &cons_dir)
{
    namespace fs = std::filesystem;

    const fs::path dir  = cons_dir;
    const fs::path json = dir / "consumer.json";

    auto cfg = from_json_file(json.string());

    if (!cfg.hub_dir.empty() && !fs::path(cfg.hub_dir).is_absolute())
        cfg.hub_dir = fs::weakly_canonical(dir / cfg.hub_dir).string();

    if (!cfg.script_path.empty() && !fs::path(cfg.script_path).is_absolute())
        cfg.script_path = fs::weakly_canonical(dir / cfg.script_path).string();

    if (!cfg.hub_dir.empty())
    {
        const fs::path hub_json   = fs::path(cfg.hub_dir) / "hub.json";
        const fs::path hub_pubkey = fs::path(cfg.hub_dir) / "hub.pubkey";

        std::ifstream f(hub_json);
        if (!f.is_open())
            throw std::runtime_error(
                "Consumer config: cannot open hub.json at '" + hub_json.string() + "'");

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

} // namespace pylabhub::consumer
