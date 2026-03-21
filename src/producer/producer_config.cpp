/**
 * @file producer_config.cpp
 * @brief ProducerConfig JSON parsing.
 */
#include "producer_config.hpp"

#include "utils/actor_vault.hpp"    // ActorVault::open (reused for producer vault)
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
#include "utils/role_directory.hpp" // RoleDirectory — canonical directory layout
#include "utils/uid_utils.hpp"      // has_producer_prefix, generate_producer_uid

#include <cstdio>   // std::fprintf (pre-lifecycle warnings)
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::producer
{

namespace cfg = pylabhub::config;

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
    constexpr const char *tag = "Producer config";

    // ── Identity (shared parser) ─────────────────────────────────────────────
    {
        const auto id = cfg::parse_identity_config(j, "producer");
        cfg.producer_uid  = id.uid;
        cfg.producer_name = id.name;
        cfg.log_level     = id.log_level;
    }

    // ── Auth (shared parser) ─────────────────────────────────────────────────
    {
        const auto ac = cfg::parse_auth_config(j, "producer");
        cfg.auth.keyfile = ac.keyfile;
    }

    // ── Hub dir / broker ──────────────────────────────────────────────────────
    if (j.contains("hub_dir") && j["hub_dir"].is_string())
        cfg.hub_dir = j["hub_dir"].get<std::string>();

    cfg.broker        = j.value("broker",       "tcp://127.0.0.1:5570");
    cfg.broker_pubkey = j.value("broker_pubkey", "");

    // ── Channel ───────────────────────────────────────────────────────────────
    cfg.channel = j.value("channel", "");
    if (cfg.channel.empty())
        throw std::runtime_error("Producer config: missing 'channel'");

    // ── Timing (shared parser) ──────────────────────────────────────────────
    {
        const auto tc = cfg::parse_timing_config(j, tag, /*default_period=*/100.0);
        cfg.target_period_ms           = tc.target_period_ms;
        cfg.target_rate_hz             = tc.target_rate_hz;
        cfg.loop_timing                = tc.loop_timing;
        cfg.queue_io_wait_timeout_ratio = tc.queue_io_wait_timeout_ratio;
        cfg.slot_acquire_timeout_ms    = tc.slot_acquire_timeout_ms;
        cfg.heartbeat_interval_ms      = tc.heartbeat_interval_ms;
    }

    // ── Transport ─────────────────────────────────────────────────────────────
    {
        const std::string t = j.value("transport", "shm");
        if (t == "shm")
            cfg.transport = Transport::Shm;
        else if (t == "zmq")
            cfg.transport = Transport::Zmq;
        else
            throw std::runtime_error(
                "Producer config: invalid 'transport': '" + t +
                "' (expected 'shm' or 'zmq')");
    }

    cfg.zmq_out_endpoint = j.value("zmq_out_endpoint", "");
    cfg.zmq_out_bind     = j.value("zmq_out_bind",     true);
    cfg.zmq_buffer_depth    = j.value("zmq_buffer_depth",    hub::kZmqDefaultBufferDepth);
    cfg.zmq_packing         = j.value("zmq_packing",         std::string{"aligned"});
    cfg.zmq_overflow_policy = j.value("zmq_overflow_policy", std::string{"drop"});

    if (cfg.zmq_packing != "aligned" && cfg.zmq_packing != "packed")
        throw std::runtime_error(
            "Producer config: invalid 'zmq_packing': '" + cfg.zmq_packing +
            "' (expected 'aligned' or 'packed')");

    if (cfg.transport == Transport::Zmq && cfg.zmq_out_endpoint.empty())
        throw std::runtime_error(
            "Producer config: 'transport' is 'zmq' but 'zmq_out_endpoint' is missing or empty");
    if (cfg.zmq_buffer_depth == 0)
        throw std::runtime_error("Producer config: 'zmq_buffer_depth' must be > 0");
    if (cfg.zmq_overflow_policy != "drop" && cfg.zmq_overflow_policy != "block")
        throw std::runtime_error(
            "Producer config: invalid 'zmq_overflow_policy': '" + cfg.zmq_overflow_policy +
            "' (expected 'drop' or 'block')");

    // ── SHM ───────────────────────────────────────────────────────────────────
    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &shm  = j["shm"];
        cfg.shm_enabled   = shm.value("enabled",    true);
        cfg.shm_secret    = shm.value("secret",     uint64_t{0});
        cfg.shm_slot_count = shm.value("slot_count", uint32_t{8});
        if (cfg.shm_enabled && cfg.shm_slot_count == 0)
            throw std::runtime_error("Producer config: 'shm.slot_count' must be > 0");
        cfg.shm_consumer_sync_policy = ::pylabhub::parse_consumer_sync_policy(
            shm.value("reader_sync_policy", std::string{"sequential"}),
            "Producer config");
    }

    // ── Schemas ───────────────────────────────────────────────────────────────
    if (j.contains("slot_schema") && !j["slot_schema"].is_null())
        cfg.slot_schema_json = j["slot_schema"];
    if (j.contains("flexzone_schema") && !j["flexzone_schema"].is_null())
        cfg.flexzone_schema_json = j["flexzone_schema"];

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
        cfg.update_checksum      = vc.update_checksum;
        cfg.stop_on_script_error = vc.stop_on_script_error;
    }

    return cfg;
}

// ============================================================================
// ProducerConfig::from_directory
// ============================================================================

ProducerConfig ProducerConfig::from_directory(const std::string &prod_dir)
{
    using pylabhub::utils::RoleDirectory;
    namespace fs = std::filesystem;

    const RoleDirectory rd  = RoleDirectory::open(prod_dir);
    auto                cfg = from_json_file(rd.config_file("producer.json").string());

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

} // namespace pylabhub::producer
