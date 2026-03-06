/**
 * @file processor_config.cpp
 * @brief ProcessorConfig JSON parsing.
 */
#include "processor_config.hpp"

#include "utils/actor_vault.hpp"  // ActorVault::open (reused for processor vault)
#include "utils/logger.hpp"
#include "utils/uid_utils.hpp"    // has_processor_prefix, generate_processor_uid

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

/// Load broker endpoint and pubkey from a hub directory.
/// On success, sets ep and pubkey; on failure logs a warning and returns false.
bool load_broker_from_hub_dir(const std::string &hub_dir,
                               std::string &ep, std::string &pubkey)
{
    namespace fs = std::filesystem;
    const fs::path hub_json   = fs::path(hub_dir) / "hub.json";
    const fs::path hub_pubkey = fs::path(hub_dir) / "hub.pubkey";

    std::ifstream f(hub_json);
    if (!f.is_open())
    {
        std::fprintf(stderr,
                     "[proc] Warning: cannot open hub.json at '%s'\n",
                     hub_json.c_str());
        return false;
    }

    nlohmann::json hj = nlohmann::json::parse(f);
    ep = hj.at("hub").at("broker_endpoint").get<std::string>();

    if (fs::exists(hub_pubkey))
    {
        std::ifstream pk(hub_pubkey);
        std::getline(pk, pubkey);
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
        }
    }

    // ── Transport ─────────────────────────────────────────────────────────────
    // Input transport is auto-discovered via HEP-CORE-0021 (Consumer::queue()).
    // Only output transport is configured here.
    cfg.out_transport     = parse_transport(j.value("out_transport", "shm"));
    cfg.zmq_out_endpoint  = j.value("zmq_out_endpoint", "");
    cfg.zmq_out_bind      = j.value("zmq_out_bind", true);

    if (cfg.out_transport == Transport::Zmq && cfg.zmq_out_endpoint.empty())
        throw std::runtime_error(
            "Processor config: 'out_transport' is 'zmq' but 'zmq_out_endpoint' is empty");

    // ── Schemas ───────────────────────────────────────────────────────────────
    if (j.contains("in_slot_schema") && !j["in_slot_schema"].is_null())
        cfg.in_slot_schema_json = j["in_slot_schema"];
    if (j.contains("out_slot_schema") && !j["out_slot_schema"].is_null())
        cfg.out_slot_schema_json = j["out_slot_schema"];
    if (j.contains("flexzone_schema") && !j["flexzone_schema"].is_null())
        cfg.flexzone_schema_json = j["flexzone_schema"];

    // ── Script ────────────────────────────────────────────────────────────────
    // script.path is the base directory containing the "script/" package.
    // Default "." means the script package is at <proc_dir>/script/__init__.py.
    if (j.contains("script") && j["script"].is_object())
    {
        cfg.script_type = j["script"].value("type", std::string{"python"});
        cfg.script_path = j["script"].value("path", std::string{"."});
    }

    // ── Validation ────────────────────────────────────────────────────────────
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &val = j["validation"];
        cfg.update_checksum    = val.value("update_checksum",    true);
        cfg.stop_on_script_error = val.value("stop_on_script_error", false);
    }

    return cfg;
}

// ============================================================================
// ProcessorConfig::from_directory
// ============================================================================

ProcessorConfig ProcessorConfig::from_directory(const std::string &proc_dir)
{
    namespace fs = std::filesystem;

    const fs::path dir  = proc_dir;
    const fs::path json = dir / "processor.json";

    auto cfg = from_json_file(json.string());

    // Resolve relative paths relative to proc_dir so that `pylabhub-processor <proc_dir>`
    // works from any working directory.
    auto resolve_rel = [&](std::string &p)
    {
        if (!p.empty() && !fs::path(p).is_absolute())
            p = fs::weakly_canonical(dir / p).string();
    };
    resolve_rel(cfg.hub_dir);
    resolve_rel(cfg.in_hub_dir);
    resolve_rel(cfg.out_hub_dir);
    resolve_rel(cfg.script_path);

    // ── Override broker from hub_dir ──────────────────────────────────────────
    if (!cfg.hub_dir.empty())
    {
        const fs::path hub_json   = fs::path(cfg.hub_dir) / "hub.json";
        const fs::path hub_pubkey = fs::path(cfg.hub_dir) / "hub.pubkey";

        // Read broker endpoint from hub.json.
        std::ifstream f(hub_json);
        if (!f.is_open())
            throw std::runtime_error(
                "Processor config: cannot open hub.json at '" + hub_json.string() + "'");

        nlohmann::json hj = nlohmann::json::parse(f);
        cfg.broker = hj.at("hub").at("broker_endpoint").get<std::string>();

        // Read broker public key.
        if (fs::exists(hub_pubkey))
        {
            std::ifstream pk(hub_pubkey);
            std::getline(pk, cfg.broker_pubkey);
        }
    }

    // ── Per-direction hub_dir overrides ──────────────────────────────────────
    if (!cfg.in_hub_dir.empty())
        (void)load_broker_from_hub_dir(cfg.in_hub_dir, cfg.in_broker, cfg.in_broker_pubkey);
    if (!cfg.out_hub_dir.empty())
        (void)load_broker_from_hub_dir(cfg.out_hub_dir, cfg.out_broker, cfg.out_broker_pubkey);

    return cfg;
}

} // namespace pylabhub::processor
