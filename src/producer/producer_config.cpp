/**
 * @file producer_config.cpp
 * @brief ProducerConfig JSON parsing.
 */
#include "producer_config.hpp"

#include "utils/actor_vault.hpp"    // ActorVault::open (reused for producer vault)
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

using ::pylabhub::kDefaultQueueIoWaitRatio;
using ::pylabhub::kMinQueueIoWaitRatio;
using ::pylabhub::kMaxQueueIoWaitRatio;

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
    cfg.target_period_ms = j.value("target_period_ms", 100.0);
    cfg.target_rate_hz   = j.value("target_rate_hz",   0.0);
    cfg.queue_io_wait_timeout_ratio = j.value("queue_io_wait_timeout_ratio",
                                               kDefaultQueueIoWaitRatio);
    cfg.slot_acquire_timeout_ms = j.value("slot_acquire_timeout_ms", -1);
    cfg.heartbeat_interval_ms   = j.value("heartbeat_interval_ms",   0);

    if (cfg.target_period_ms < 0.0)
    {
        throw std::runtime_error("Producer config: 'target_period_ms' must be >= 0");
    }
    if (cfg.target_rate_hz < 0.0)
    {
        throw std::runtime_error("Producer config: 'target_rate_hz' must be >= 0");
    }
    if (cfg.queue_io_wait_timeout_ratio < kMinQueueIoWaitRatio ||
        cfg.queue_io_wait_timeout_ratio > kMaxQueueIoWaitRatio)
    {
        throw std::runtime_error(
            "Producer config: 'queue_io_wait_timeout_ratio' must be between " +
            std::to_string(kMinQueueIoWaitRatio) + " and " +
            std::to_string(kMaxQueueIoWaitRatio));
    }

    // Resolve period: rate_hz and period_ms are mutually exclusive.
    // resolve_period_us validates mutual exclusion and minimum period.
    const double period_us = ::pylabhub::resolve_period_us(
        cfg.target_rate_hz, cfg.target_period_ms, "Producer config");

    if (j.contains("loop_timing"))
    {
        cfg.loop_timing = ::pylabhub::parse_loop_timing_policy(
            j["loop_timing"].get<std::string>(), period_us, "Producer config");
    }
    else
    {
        cfg.loop_timing = ::pylabhub::default_loop_timing_policy(period_us);
    }

    // Warn if deprecated slot_acquire_timeout_ms is set.
    if (j.contains("slot_acquire_timeout_ms"))
    {
        LOGGER_WARN("[prod] 'slot_acquire_timeout_ms' is deprecated; use "
                    "'queue_io_wait_timeout_ratio' instead (current: {})",
                    cfg.queue_io_wait_timeout_ratio);
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

    // ── Inbox (Phase 3) ────────────────────────────────────────────────────────
    if (j.contains("inbox_schema") && !j["inbox_schema"].is_null())
        cfg.inbox_schema_json = j["inbox_schema"];
    cfg.inbox_endpoint          = j.value("inbox_endpoint",          std::string{});
    cfg.inbox_buffer_depth      = j.value("inbox_buffer_depth",      hub::kZmqDefaultBufferDepth);
    cfg.inbox_overflow_policy   = j.value("inbox_overflow_policy",   std::string{"drop"});
    if (cfg.inbox_buffer_depth == 0)
        throw std::runtime_error("Producer config: 'inbox_buffer_depth' must be > 0");
    if (cfg.inbox_overflow_policy != "drop" && cfg.inbox_overflow_policy != "block")
        throw std::runtime_error(
            "Producer config: invalid 'inbox_overflow_policy': '" + cfg.inbox_overflow_policy +
            "' (expected 'drop' or 'block')");
    if (cfg.has_inbox())
    {
        if (!cfg.inbox_schema_json.is_string() && !cfg.inbox_schema_json.is_object())
            throw std::runtime_error(
                "Producer config: 'inbox_schema' must be a JSON object (inline schema) "
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
        if (cfg.script_type != "python" && cfg.script_type != "lua")
            throw std::invalid_argument(
                "script.type must be \"python\" or \"lua\", got: \"" + cfg.script_type + "\"");
        cfg.script_path = s.value("path", std::string{"."});
    }

    // ── Python virtual environment (optional) ────────────────────────────────
    cfg.python_venv = j.value("python_venv", std::string{});

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
