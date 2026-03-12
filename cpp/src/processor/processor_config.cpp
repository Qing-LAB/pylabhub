/**
 * @file processor_config.cpp
 * @brief ProcessorConfig JSON parsing.
 */
#include "processor_config.hpp"

#include "utils/actor_vault.hpp"    // ActorVault::open (reused for processor vault)
#include "utils/logger.hpp"
#include "utils/role_directory.hpp" // RoleDirectory — canonical directory layout
#include "utils/uid_utils.hpp"      // has_processor_prefix, generate_processor_uid

#include <cstdio>   // std::fprintf (pre-lifecycle warnings)
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pylabhub::processor
{

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

} // anonymous namespace

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

    // ── Processor identity ─────────────────────────────────────────────────────
    if (!j.contains("processor") || !j["processor"].is_object())
        throw std::runtime_error("Processor config: missing 'processor' object");

    const auto &proc = j["processor"];
    cfg.processor_uid  = proc.value("uid",       "");
    cfg.processor_name = proc.value("name",      "");
    cfg.log_level      = proc.value("log_level", "info");

    if (cfg.processor_uid.empty())
    {
        // Auto-generate from name — printed to stderr before lifecycle starts.
        cfg.processor_uid = pylabhub::uid::generate_processor_uid(cfg.processor_name);
        std::fprintf(stderr,
                     "[proc] No 'processor.uid' in config — generated: %s\n"
                     "  Add this to processor.json to make the UID stable.\n",
                     cfg.processor_uid.c_str());
    }
    else if (!pylabhub::uid::has_processor_prefix(cfg.processor_uid))
    {
        std::fprintf(stderr,
                     "[proc] Warning: 'processor.uid' = '%s' does not start with 'PROC-'.\n",
                     cfg.processor_uid.c_str());
    }

    // ── Auth ──────────────────────────────────────────────────────────────────
    if (proc.contains("auth") && proc["auth"].is_object())
        cfg.auth.keyfile = proc["auth"].value("keyfile", "");

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

    // ── Timing / policy ───────────────────────────────────────────────────────
    cfg.overflow_policy  = parse_overflow_policy(j.value("overflow_policy", "block"));
    cfg.timeout_ms       = j.value("timeout_ms",       -1);
    cfg.heartbeat_interval_ms = j.value("heartbeat_interval_ms", 0);

    if (cfg.timeout_ms < -1)
        throw std::runtime_error("Processor config: 'timeout_ms' must be >= -1 (-1=infinite, 0=non-blocking, >0=ms)");

    cfg.target_period_ms = j.value("target_period_ms", 0);
    if (cfg.target_period_ms < 0)
        throw std::runtime_error("Processor config: 'target_period_ms' must be >= 0");

    if (j.contains("loop_timing")) {
        cfg.loop_timing = ::pylabhub::parse_loop_timing_policy(
            j["loop_timing"].get<std::string>(), cfg.target_period_ms, "Processor config");
    } else {
        cfg.loop_timing = ::pylabhub::default_loop_timing_policy(cfg.target_period_ms);
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
        }
    }

    // ── Transport — input ────────────────────────────────────────────────────
    cfg.in_transport        = parse_transport(j.value("in_transport", "shm"));
    cfg.zmq_in_endpoint     = j.value("zmq_in_endpoint", "");
    cfg.zmq_in_bind         = j.value("zmq_in_bind", false);
    cfg.in_zmq_buffer_depth = j.value("in_zmq_buffer_depth", size_t{64});
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
    cfg.out_zmq_buffer_depth = j.value("out_zmq_buffer_depth", size_t{64});
    cfg.out_zmq_packing      = j.value("out_zmq_packing", std::string{"aligned"});

    if (cfg.out_transport == Transport::Zmq && cfg.zmq_out_endpoint.empty())
        throw std::runtime_error(
            "Processor config: 'out_transport' is 'zmq' but 'zmq_out_endpoint' is empty");
    if (cfg.out_zmq_buffer_depth == 0)
        throw std::runtime_error("Processor config: 'out_zmq_buffer_depth' must be > 0");
    if (cfg.out_zmq_packing != "aligned" && cfg.out_zmq_packing != "packed")
        throw std::runtime_error(
            "Processor config: invalid 'out_zmq_packing': '" + cfg.out_zmq_packing +
            "' (expected 'aligned' or 'packed')");

    // ── Schemas ───────────────────────────────────────────────────────────────
    if (j.contains("in_slot_schema") && !j["in_slot_schema"].is_null())
        cfg.in_slot_schema_json = j["in_slot_schema"];
    if (j.contains("out_slot_schema") && !j["out_slot_schema"].is_null())
        cfg.out_slot_schema_json = j["out_slot_schema"];
    if (j.contains("flexzone_schema") && !j["flexzone_schema"].is_null())
        cfg.flexzone_schema_json = j["flexzone_schema"];

    // ── Inbox (optional) ─────────────────────────────────────────────────────
    if (j.contains("inbox_schema") && !j["inbox_schema"].is_null())
        cfg.inbox_schema_json = j["inbox_schema"];
    cfg.inbox_endpoint        = j.value("inbox_endpoint",        std::string{});
    cfg.inbox_buffer_depth    = j.value("inbox_buffer_depth",    size_t{64});
    cfg.inbox_overflow_policy = j.value("inbox_overflow_policy", std::string{"drop"});
    if (cfg.inbox_buffer_depth == 0)
        throw std::runtime_error("Processor config: 'inbox_buffer_depth' must be > 0");
    if (cfg.inbox_overflow_policy != "drop" && cfg.inbox_overflow_policy != "block")
        throw std::runtime_error(
            "Processor config: invalid 'inbox_overflow_policy': '" + cfg.inbox_overflow_policy +
            "' (expected 'drop' or 'block')");
    if (cfg.has_inbox())
    {
        if (!cfg.inbox_schema_json.is_string() && !cfg.inbox_schema_json.is_object())
            throw std::runtime_error(
                "Processor config: 'inbox_schema' must be a JSON object (inline schema) "
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

    // ── Script ────────────────────────────────────────────────────────────────
    if (j.contains("script") && j["script"].is_object())
    {
        const auto &s = j["script"];
        cfg.script_type_explicit = s.contains("type");
        cfg.script_type = s.value("type", std::string{"python"});
        cfg.script_path = s.value("path", std::string{"."});
    }

    // ── Validation ────────────────────────────────────────────────────────────
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &val = j["validation"];
        cfg.update_checksum      = val.value("update_checksum",      true);
        cfg.verify_checksum      = val.value("verify_checksum",      false);
        cfg.stop_on_script_error = val.value("stop_on_script_error", false);
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
