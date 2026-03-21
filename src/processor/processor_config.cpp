/**
 * @file processor_config.cpp
 * @brief ProcessorConfig JSON parsing.
 */
#include "processor_config.hpp"

#include "utils/actor_vault.hpp"    // ActorVault::open (reused for processor vault)
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
#include "utils/uid_utils.hpp"      // has_processor_prefix, generate_processor_uid

#include <cstdio>   // std::fprintf (pre-lifecycle warnings)
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::processor
{

namespace cfg = pylabhub::config;

// ============================================================================
// ProcessorAuthConfig::load_keypair
// ============================================================================

bool ProcessorAuthConfig::load_keypair(const std::string &proc_uid,
                                        const std::string &password)
{
    if (keyfile.empty())
        return false;

    if (!std::filesystem::exists(keyfile))
    {
        LOGGER_WARN("[proc] auth.keyfile '{}': not found — using ephemeral CURVE identity",
                    keyfile);
        return false;
    }

    // Reuse ActorVault (domain = proc_uid).
    const auto vault = pylabhub::utils::ActorVault::open(keyfile, proc_uid, password);
    client_pubkey = vault.public_key();
    client_seckey = vault.secret_key();
    LOGGER_INFO("[proc] Loaded processor vault from '{}' (pubkey: {}...)",
                keyfile, vault.public_key().substr(0, 8));
    return true;
}

// ============================================================================
// Parsing helpers (anonymous namespace)
// ============================================================================

namespace
{

OverflowPolicy parse_overflow_policy(const std::string &s)
{
    if (s == "block") return OverflowPolicy::Block;
    if (s == "drop")  return OverflowPolicy::Drop;
    throw std::runtime_error(
        "Processor config: invalid 'overflow_policy' = '" + s +
        "' (must be 'block' or 'drop')");
}

Transport parse_transport(const std::string &s)
{
    if (s == "shm") return Transport::Shm;
    if (s == "zmq") return Transport::Zmq;
    throw std::runtime_error(
        "Processor config: invalid transport = '" + s +
        "' (must be 'shm' or 'zmq')");
}

/// Load broker endpoint and pubkey from a hub directory via RoleDirectory.
/// On success, sets ep and pubkey; on failure logs a warning and returns false.
bool load_broker_from_hub_dir(const std::string &hub_dir,
                               std::string &ep, std::string &pubkey)
{
    using pylabhub::utils::RoleDirectory;
    namespace fs = std::filesystem;

    try
    {
        ep     = RoleDirectory::hub_broker_endpoint(fs::path(hub_dir));
        pubkey = RoleDirectory::hub_broker_pubkey(fs::path(hub_dir));
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[proc] Warning: %s\n", e.what());
        return false;
    }
    return true;
}

} // namespace

// ============================================================================
// Resolver methods
// ============================================================================

const std::string &ProcessorConfig::resolved_in_broker() const noexcept
{
    return in_broker.empty() ? broker : in_broker;
}

const std::string &ProcessorConfig::resolved_out_broker() const noexcept
{
    return out_broker.empty() ? broker : out_broker;
}

const std::string &ProcessorConfig::resolved_in_broker_pubkey() const noexcept
{
    return in_broker_pubkey.empty() ? broker_pubkey : in_broker_pubkey;
}

const std::string &ProcessorConfig::resolved_out_broker_pubkey() const noexcept
{
    return out_broker_pubkey.empty() ? broker_pubkey : out_broker_pubkey;
}

// ============================================================================
// ProcessorConfig::from_json_file
// ============================================================================

ProcessorConfig ProcessorConfig::from_json_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Processor config: cannot open '" + path + "'");

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(file);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "Processor config: JSON parse error in '" + path + "': " + e.what());
    }

    ProcessorConfig cfg;
    constexpr const char *tag = "Processor config";

    // ── Identity (shared parser) ─────────────────────────────────────────────
    {
        const auto id = cfg::parse_identity_config(j, "processor");
        cfg.processor_uid  = id.uid;
        cfg.processor_name = id.name;
        cfg.log_level      = id.log_level;
    }

    // ── Auth (shared parser) ─────────────────────────────────────────────────
    {
        const auto ac = cfg::parse_auth_config(j, "processor");
        cfg.auth.keyfile = ac.keyfile;
    }

    // ── Hub dir / broker ──────────────────────────────────────────────────────
    if (j.contains("hub_dir") && j["hub_dir"].is_string())
        cfg.hub_dir = j["hub_dir"].get<std::string>();

    cfg.broker        = j.value("broker",        "tcp://127.0.0.1:5570");
    cfg.broker_pubkey = j.value("broker_pubkey", "");

    // ── Per-direction overrides ──────────────────────────────────────────────
    if (j.contains("in_hub_dir") && j["in_hub_dir"].is_string())
        cfg.in_hub_dir = j["in_hub_dir"].get<std::string>();
    if (j.contains("out_hub_dir") && j["out_hub_dir"].is_string())
        cfg.out_hub_dir = j["out_hub_dir"].get<std::string>();

    cfg.in_broker          = j.value("in_broker",          "");
    cfg.out_broker         = j.value("out_broker",         "");
    cfg.in_broker_pubkey   = j.value("in_broker_pubkey",   "");
    cfg.out_broker_pubkey  = j.value("out_broker_pubkey",  "");

    // ── Channels ──────────────────────────────────────────────────────────────
    cfg.in_channel  = j.value("in_channel",  "");
    cfg.out_channel = j.value("out_channel", "");

    if (cfg.in_channel.empty())
        throw std::runtime_error("Processor config: missing 'in_channel'");
    if (cfg.out_channel.empty())
        throw std::runtime_error("Processor config: missing 'out_channel'");

    // ── Overflow policy (processor-specific) ────────────────────────────────
    cfg.overflow_policy = parse_overflow_policy(j.value("overflow_policy", "block"));

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

    // ── SHM ───────────────────────────────────────────────────────────────────
    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &shm = j["shm"];
        if (shm.contains("in") && shm["in"].is_object())
        {
            cfg.in_shm_enabled = shm["in"].value("enabled", true);
            cfg.in_shm_secret  = shm["in"].value("secret",  uint64_t{0});
        }
        if (shm.contains("out") && shm["out"].is_object())
        {
            cfg.out_shm_enabled    = shm["out"].value("enabled",    true);
            cfg.out_shm_secret     = shm["out"].value("secret",     uint64_t{0});
            cfg.out_shm_slot_count = shm["out"].value("slot_count", uint32_t{4});
            if (cfg.out_shm_enabled && cfg.out_shm_slot_count == 0)
                throw std::runtime_error("Processor config: 'shm.out.slot_count' must be > 0");
            cfg.out_shm_consumer_sync_policy = ::pylabhub::parse_consumer_sync_policy(
                shm["out"].value("reader_sync_policy", std::string{"sequential"}),
                "Processor config");
        }
    }

    // ── Transport — input ────────────────────────────────────────────────────
    cfg.in_transport        = parse_transport(j.value("in_transport", "shm"));
    cfg.zmq_in_endpoint     = j.value("zmq_in_endpoint", "");
    cfg.zmq_in_bind         = j.value("zmq_in_bind", false);
    cfg.in_zmq_buffer_depth = j.value("in_zmq_buffer_depth", hub::kZmqDefaultBufferDepth);
    cfg.in_zmq_packing      = j.value("in_zmq_packing", std::string{"aligned"});

    if (cfg.in_transport == Transport::Zmq && cfg.zmq_in_endpoint.empty())
        throw std::runtime_error(
            "Processor config: 'in_transport' is 'zmq' but 'zmq_in_endpoint' is empty");
    if (cfg.in_zmq_buffer_depth == 0)
        throw std::runtime_error("Processor config: 'in_zmq_buffer_depth' must be > 0");
    if (cfg.in_zmq_packing != "aligned" && cfg.in_zmq_packing != "packed")
        throw std::runtime_error(
            "Processor config: invalid 'in_zmq_packing': '" + cfg.in_zmq_packing +
            "' (expected 'aligned' or 'packed')");

    // ── Transport — output ───────────────────────────────────────────────────
    cfg.out_transport        = parse_transport(j.value("out_transport", "shm"));
    cfg.zmq_out_endpoint     = j.value("zmq_out_endpoint", "");
    cfg.zmq_out_bind         = j.value("zmq_out_bind", true);
    cfg.out_zmq_buffer_depth = j.value("out_zmq_buffer_depth", hub::kZmqDefaultBufferDepth);
    cfg.out_zmq_packing          = j.value("out_zmq_packing",          std::string{"aligned"});
    cfg.zmq_out_overflow_policy  = j.value("zmq_out_overflow_policy",  std::string{""});

    if (cfg.out_transport == Transport::Zmq && cfg.zmq_out_endpoint.empty())
        throw std::runtime_error(
            "Processor config: 'out_transport' is 'zmq' but 'zmq_out_endpoint' is empty");
    if (cfg.out_zmq_buffer_depth == 0)
        throw std::runtime_error("Processor config: 'out_zmq_buffer_depth' must be > 0");
    if (cfg.out_zmq_packing != "aligned" && cfg.out_zmq_packing != "packed")
        throw std::runtime_error(
            "Processor config: invalid 'out_zmq_packing': '" + cfg.out_zmq_packing +
            "' (expected 'aligned' or 'packed')");
    if (!cfg.zmq_out_overflow_policy.empty() &&
        cfg.zmq_out_overflow_policy != "drop" && cfg.zmq_out_overflow_policy != "block")
        throw std::runtime_error(
            "Processor config: invalid 'zmq_out_overflow_policy': '" +
            cfg.zmq_out_overflow_policy + "' (expected 'drop', 'block', or absent)");

    // ── Schemas ───────────────────────────────────────────────────────────────
    if (j.contains("in_slot_schema") && !j["in_slot_schema"].is_null())
        cfg.in_slot_schema_json = j["in_slot_schema"];
    if (j.contains("out_slot_schema") && !j["out_slot_schema"].is_null())
        cfg.out_slot_schema_json = j["out_slot_schema"];
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
        cfg.verify_checksum      = vc.verify_checksum;
        cfg.stop_on_script_error = vc.stop_on_script_error;
    }

    return cfg;
}

// ============================================================================
// ProcessorConfig::from_directory
// ============================================================================

ProcessorConfig ProcessorConfig::from_directory(const std::string &proc_dir)
{
    using pylabhub::utils::RoleDirectory;
    namespace fs = std::filesystem;

    const RoleDirectory rd  = RoleDirectory::open(proc_dir);
    auto                cfg = from_json_file(rd.config_file("processor.json").string());

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

    // ── Per-direction hub_dir overrides ───────────────────────────────────────
    auto resolve_hub_dir_rel = [&](std::string &hd)
    {
        if (hd.empty()) return;
        if (const auto hub = rd.resolve_hub_dir(hd))
            hd = hub->string();
    };
    resolve_hub_dir_rel(cfg.in_hub_dir);
    resolve_hub_dir_rel(cfg.out_hub_dir);

    if (!cfg.in_hub_dir.empty())
        (void)load_broker_from_hub_dir(cfg.in_hub_dir, cfg.in_broker, cfg.in_broker_pubkey);
    if (!cfg.out_hub_dir.empty())
        (void)load_broker_from_hub_dir(cfg.out_hub_dir, cfg.out_broker, cfg.out_broker_pubkey);

    // Resolve script_path relative to the role directory.
    if (!cfg.script_path.empty() && !fs::path(cfg.script_path).is_absolute())
        cfg.script_path = fs::weakly_canonical(rd.base() / cfg.script_path).string();

    return cfg;
}

} // namespace pylabhub::processor
