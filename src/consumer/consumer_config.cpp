/**
 * @file consumer_config.cpp
 * @brief ConsumerConfig JSON parsing.
 */
#include "consumer_config.hpp"

#include "utils/actor_vault.hpp"
#include "utils/config/auth_config.hpp"
#include "utils/config/identity_config.hpp"
#include "utils/config/inbox_config.hpp"
#include "utils/config/monitoring_config.hpp"
#include "utils/config/script_config.hpp"
#include "utils/config/startup_config.hpp"
#include "utils/config/timing_config.hpp"
#include "utils/config/validation_config.hpp"
#include "utils/hub_zmq_queue.hpp"  // kZmqDefaultBufferDepth
#include "utils/logger.hpp"
#include "utils/role_directory.hpp"
#include "utils/uid_utils.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::consumer
{

namespace cfg = pylabhub::config;

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
    constexpr const char *tag = "Consumer config";

    // ── Identity (shared parser) ─────────────────────────────────────────────
    {
        const auto id = cfg::parse_identity_config(j, "consumer");
        cfg.consumer_uid  = id.uid;
        cfg.consumer_name = id.name;
        cfg.log_level     = id.log_level;
    }

    // ── Auth (shared parser) ─────────────────────────────────────────────────
    {
        const auto ac = cfg::parse_auth_config(j, "consumer");
        cfg.auth.keyfile = ac.keyfile;
    }

    if (j.contains("hub_dir") && j["hub_dir"].is_string())
        cfg.hub_dir = j["hub_dir"].get<std::string>();

    cfg.broker        = j.value("broker",       "tcp://127.0.0.1:5570");
    cfg.broker_pubkey = j.value("broker_pubkey", "");

    cfg.channel = j.value("channel", "");
    if (cfg.channel.empty())
        throw std::runtime_error("Consumer config: missing 'channel'");

    // ── Queue type ─────────────────────────────────────────────────────────────
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

    // ── Timing (shared parser) ──────────────────────────────────────────────
    {
        const auto tc = cfg::parse_timing_config(j, tag, /*default_period=*/0.0);
        cfg.target_period_ms           = tc.target_period_ms;
        cfg.target_rate_hz             = tc.target_rate_hz;
        cfg.loop_timing                = tc.loop_timing;
        cfg.queue_io_wait_timeout_ratio = tc.queue_io_wait_timeout_ratio;
        cfg.slot_acquire_timeout_ms    = tc.slot_acquire_timeout_ms;
        cfg.heartbeat_interval_ms      = tc.heartbeat_interval_ms;
    }

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

    // ── ZMQ transport fields (consumer-specific: no direction prefix) ────────
    cfg.zmq_buffer_depth = j.value("zmq_buffer_depth", hub::kZmqDefaultBufferDepth);
    cfg.zmq_packing      = j.value("zmq_packing",      std::string{"aligned"});
    if (cfg.zmq_packing != "aligned" && cfg.zmq_packing != "packed")
        throw std::runtime_error(
            "Consumer config: invalid 'zmq_packing': '" + cfg.zmq_packing +
            "' (expected 'aligned' or 'packed')");
    if (cfg.zmq_buffer_depth == 0)
        throw std::runtime_error("Consumer config: 'zmq_buffer_depth' must be > 0");

    // ── Inbox (shared parser) ────────────────────────────────────────────────
    {
        const auto ic = cfg::parse_inbox_config(j, tag);
        cfg.inbox_schema_json     = ic.schema_json;
        cfg.inbox_endpoint        = ic.endpoint;
        cfg.inbox_buffer_depth    = ic.buffer_depth;
        cfg.inbox_overflow_policy = ic.overflow_policy;
    }

    // ── Startup coordination (shared parser) ─────────────────────────────────
    {
        const auto sc = cfg::parse_startup_config(j, tag);
        cfg.wait_for_roles = std::move(sc.wait_for_roles);
    }

    // ── Monitoring (shared parser) ───────────────────────────────────────────
    {
        const auto mc = cfg::parse_monitoring_config(j);
        cfg.ctrl_queue_max_depth = mc.ctrl_queue_max_depth;
        cfg.peer_dead_timeout_ms = mc.peer_dead_timeout_ms;
    }

    // ── Script (shared parser — includes type validation) ────────────────────
    {
        const auto sc = cfg::parse_script_config(j, {}, tag);
        cfg.script_type          = sc.type;
        cfg.script_path          = sc.path;
        cfg.python_venv          = sc.python_venv;
        cfg.script_type_explicit = sc.type_explicit;
    }

    // ── Validation (shared parser) ───────────────────────────────────────────
    {
        const auto vc = cfg::parse_validation_config(j);
        cfg.stop_on_script_error = vc.stop_on_script_error;
        cfg.verify_checksum      = vc.verify_checksum;
    }

    return cfg;
}

ConsumerConfig ConsumerConfig::from_directory(const std::string &cons_dir)
{
    using pylabhub::utils::RoleDirectory;
    namespace fs = std::filesystem;

    const RoleDirectory rd  = RoleDirectory::open(cons_dir);
    auto                cfg = from_json_file(rd.config_file("consumer.json").string());

    cfg.role_dir = rd.base().string();

    // Warn if the vault keyfile is stored inside the role directory.
    RoleDirectory::warn_if_keyfile_in_role_dir(rd.base(), cfg.auth.keyfile);

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
