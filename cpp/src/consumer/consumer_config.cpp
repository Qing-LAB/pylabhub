/**
 * @file consumer_config.cpp
 * @brief ConsumerConfig JSON parsing.
 */
#include "consumer_config.hpp"

#include "utils/actor_vault.hpp"
#include "utils/role_directory.hpp"
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

    // ── Queue type / timing ───────────────────────────────────────────────────
    {
        const std::string qt = j.value("queue_type", "shm");
        if (qt == "shm")
            cfg.queue_type = QueueType::Shm;
        else if (qt == "zmq")
            cfg.queue_type = QueueType::Zmq;
        else
            throw std::runtime_error("Consumer config: invalid 'queue_type': '" + qt +
                                     "' (expected 'shm' or 'zmq')");
    }

    cfg.target_period_ms = j.value("target_period_ms", 0);
    if (cfg.target_period_ms < 0) {
        throw std::runtime_error("Consumer config: 'target_period_ms' must be >= 0");
    }

    if (j.contains("loop_timing")) {
        cfg.loop_timing = ::pylabhub::parse_loop_timing_policy(
            j["loop_timing"].get<std::string>(), cfg.target_period_ms, "Consumer config");
    }
    else {
        cfg.loop_timing = ::pylabhub::default_loop_timing_policy(cfg.target_period_ms);
    }

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

    // ── Inbox (optional) ─────────────────────────────────────────────────────
    if (j.contains("inbox_schema") && !j["inbox_schema"].is_null())
        cfg.inbox_schema_json = j["inbox_schema"];
    cfg.inbox_endpoint          = j.value("inbox_endpoint",          std::string{});
    cfg.inbox_buffer_depth      = j.value("inbox_buffer_depth",      size_t{64});
    cfg.inbox_overflow_policy   = j.value("inbox_overflow_policy",   std::string{"drop"});
    cfg.zmq_buffer_depth        = j.value("zmq_buffer_depth",        size_t{64});
    cfg.zmq_packing             = j.value("zmq_packing",             std::string{"aligned"});
    if (cfg.zmq_packing != "aligned" && cfg.zmq_packing != "packed")
        throw std::runtime_error(
            "Consumer config: invalid 'zmq_packing': '" + cfg.zmq_packing +
            "' (expected 'aligned' or 'packed')");
    if (cfg.inbox_buffer_depth == 0)
        throw std::runtime_error("Consumer config: 'inbox_buffer_depth' must be > 0");
    if (cfg.zmq_buffer_depth == 0)
        throw std::runtime_error("Consumer config: 'zmq_buffer_depth' must be > 0");
    if (cfg.inbox_overflow_policy != "drop" && cfg.inbox_overflow_policy != "block")
        throw std::runtime_error(
            "Consumer config: invalid 'inbox_overflow_policy': '" + cfg.inbox_overflow_policy +
            "' (expected 'drop' or 'block')");
    if (cfg.has_inbox())
    {
        if (!cfg.inbox_schema_json.is_string() && !cfg.inbox_schema_json.is_object())
            throw std::runtime_error(
                "Consumer config: 'inbox_schema' must be a JSON object (inline schema) "
                "or string (named schema reference)");
    }

    // ── Startup coordination (HEP-0023) ──────────────────────────────────────
    if (j.contains("startup") && j.at("startup").is_object())
    {
        const auto &s = j.at("startup");
        if (s.contains("wait_for_roles") && s.at("wait_for_roles").is_array())
        {
            for (const auto &w : s.at("wait_for_roles"))
            {
                if (!w.contains("uid") || !w.at("uid").is_string())
                    throw std::runtime_error(
                        "startup.wait_for_roles: each entry must have a string 'uid'");
                WaitForRole wr;
                wr.uid = w.at("uid").get<std::string>();
                if (wr.uid.empty())
                    throw std::runtime_error(
                        "startup.wait_for_roles: 'uid' must not be empty");
                wr.timeout_ms = w.value("timeout_ms", pylabhub::kDefaultStartupWaitTimeoutMs);
                if (wr.timeout_ms <= 0)
                    throw std::runtime_error(
                        "startup.wait_for_roles: timeout_ms must be > 0 for uid='" +
                        wr.uid + "'");
                if (wr.timeout_ms > pylabhub::kMaxStartupWaitTimeoutMs)
                    throw std::runtime_error(
                        "startup.wait_for_roles: timeout_ms exceeds maximum (3600000 ms) for uid='" +
                        wr.uid + "'");
                cfg.wait_for_roles.push_back(std::move(wr));
            }
        }
    }

    // ── Peer/hub-dead monitoring ─────────────────────────────────────────────
    cfg.ctrl_queue_max_depth = j.value("ctrl_queue_max_depth", size_t{256});
    cfg.peer_dead_timeout_ms = j.value("peer_dead_timeout_ms", 30000);

    if (j.contains("script") && j["script"].is_object())
    {
        const auto &s = j["script"];
        cfg.script_type_explicit = s.contains("type");
        cfg.script_type = s.value("type", std::string{"python"});
        cfg.script_path = s.value("path", std::string{"."});
    }

    if (j.contains("validation") && j["validation"].is_object())
    {
        cfg.stop_on_script_error = j["validation"].value("stop_on_script_error", false);
        cfg.verify_checksum      = j["validation"].value("verify_checksum", false);
    }

    return cfg;
}

ConsumerConfig ConsumerConfig::from_directory(const std::string &cons_dir)
{
    using pylabhub::utils::RoleDirectory;
    namespace fs = std::filesystem;

    const RoleDirectory rd  = RoleDirectory::open(cons_dir);
    auto                cfg = from_json_file(rd.config_file("consumer.json").string());

    // Resolve hub_dir relative to the role directory, then read broker info.
    if (const auto hub = rd.resolve_hub_dir(cfg.hub_dir))
    {
        cfg.hub_dir       = hub->string();
        cfg.broker        = RoleDirectory::hub_broker_endpoint(*hub);
        cfg.broker_pubkey = RoleDirectory::hub_broker_pubkey(*hub);
    }

    // Resolve script_path relative to the role directory.
    if (!cfg.script_path.empty() && !fs::path(cfg.script_path).is_absolute())
        cfg.script_path = fs::weakly_canonical(rd.base() / cfg.script_path).string();

    return cfg;
}

} // namespace pylabhub::consumer
