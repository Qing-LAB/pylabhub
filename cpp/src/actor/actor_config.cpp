/**
 * @file actor_config.cpp
 * @brief ActorConfig JSON parsing — multi-role format.
 *
 * Parses the "roles" map and optional "script" / "actor" blocks from JSON.
 * The "script" field, when present, must be an object with "module" and "path".
 * Cross-field validation: loop_trigger="shm" requires shm.enabled=true.
 */
#include "actor_config.hpp"

#include "utils/logger.hpp"
#include "utils/uid_utils.hpp"    // generate_actor_uid, has_actor_prefix

#include <cstdio>                 // std::fprintf (for pre-lifecycle warnings)

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace pylabhub::actor
{

// ============================================================================
// ActorAuthConfig::load_keypair
// ============================================================================

bool ActorAuthConfig::load_keypair()
{
    if (keyfile.empty())
        return true; // no keyfile configured — not an error

    std::ifstream f(keyfile);
    if (!f.is_open())
    {
        LOGGER_WARN("[actor] auth.keyfile '{}': cannot open — actor will use ephemeral CURVE identity",
                    keyfile);
        return false;
    }

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(f);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        LOGGER_WARN("[actor] auth.keyfile '{}': JSON parse error: {} — using ephemeral identity",
                    keyfile, e.what());
        return false;
    }

    const auto pub = j.value("public_key", std::string{});
    const auto sec = j.value("secret_key", std::string{});

    if (pub.size() != 40 || sec.size() != 40)
    {
        LOGGER_WARN("[actor] auth.keyfile '{}': public_key/secret_key must be 40-char Z85 strings "
                    "— using ephemeral identity", keyfile);
        return false;
    }

    client_pubkey = pub;
    client_seckey = sec;
    LOGGER_INFO("[actor] Loaded actor keypair from '{}' (pubkey: {}...)", keyfile, pub.substr(0, 8));
    return true;
}

// ============================================================================
// Parsing helpers (anonymous namespace)
// ============================================================================

namespace
{

ValidationPolicy::Checksum parse_checksum(const std::string &s, const std::string &key)
{
    if (s == "none")    return ValidationPolicy::Checksum::None;
    if (s == "update")  return ValidationPolicy::Checksum::Update;
    if (s == "enforce") return ValidationPolicy::Checksum::Enforce;
    throw std::runtime_error(
        "Actor config: invalid '" + key + "' = '" + s +
        "' (must be 'none', 'update', or 'enforce')");
}

bool parse_on_fail(const std::string &s)
{
    if (s == "skip") return true;
    if (s == "pass") return false;
    throw std::runtime_error(
        "Actor config: invalid 'on_checksum_fail' = '" + s +
        "' (must be 'skip' or 'pass')");
}

bool parse_on_py_error(const std::string &s)
{
    if (s == "continue") return false;
    if (s == "stop")     return true;
    throw std::runtime_error(
        "Actor config: invalid 'on_python_error' = '" + s +
        "' (must be 'continue' or 'stop')");
}

RoleConfig::LoopTimingPolicy parse_loop_timing(const std::string &s)
{
    if (s == "fixed_pace")   return RoleConfig::LoopTimingPolicy::FixedPace;
    if (s == "compensating") return RoleConfig::LoopTimingPolicy::Compensating;
    throw std::runtime_error(
        "Actor config: invalid 'loop_timing' = '" + s +
        "' (must be 'fixed_pace' or 'compensating')");
}

RoleConfig::LoopTrigger parse_loop_trigger(const std::string &s)
{
    if (s == "shm")       return RoleConfig::LoopTrigger::Shm;
    if (s == "messenger") return RoleConfig::LoopTrigger::Messenger;
    throw std::runtime_error(
        "Actor config: invalid 'loop_trigger' = '" + s +
        "' (must be 'shm' or 'messenger')");
}

hub::LoopPolicy parse_loop_policy(const std::string &s)
{
    if (s == "max_rate")   return hub::LoopPolicy::MaxRate;
    if (s == "fixed_rate") return hub::LoopPolicy::FixedRate;
    throw std::runtime_error(
        "Actor config: invalid 'loop_policy' = '" + s +
        "' (must be 'max_rate' or 'fixed_rate')");
}

ValidationPolicy parse_validation(const nlohmann::json &j)
{
    ValidationPolicy v;
    if (!j.is_object())
        return v;
    v.slot_checksum =
        parse_checksum(j.value("slot_checksum", "update"), "slot_checksum");
    v.flexzone_checksum =
        parse_checksum(j.value("flexzone_checksum", "update"), "flexzone_checksum");
    v.skip_on_validation_error = parse_on_fail(j.value("on_checksum_fail",  "skip"));
    v.stop_on_python_error     = parse_on_py_error(j.value("on_python_error", "continue"));
    return v;
}

/// Resolve "env:VAR" → actual environment variable; otherwise return s unchanged.
std::string resolve_env_value(const std::string &s)
{
    if (s.size() > 4 && s.substr(0, 4) == "env:")
    {
        const char *val = std::getenv(s.c_str() + 4);
        return (val != nullptr) ? std::string(val) : std::string{};
    }
    return s;
}

RoleConfig parse_role(const std::string &role_name, const nlohmann::json &j)
{
    RoleConfig rc;

    // kind
    const std::string kind_str = j.value("kind", "producer");
    if (kind_str == "producer")
        rc.kind = RoleConfig::Kind::Producer;
    else if (kind_str == "consumer")
        rc.kind = RoleConfig::Kind::Consumer;
    else
        throw std::runtime_error(
            "Actor config: role '" + role_name +
            "': invalid kind '" + kind_str + "' (must be 'producer' or 'consumer')");

    // channel (required)
    if (!j.contains("channel") || !j["channel"].is_string())
        throw std::runtime_error(
            "Actor config: role '" + role_name + "': missing required field 'channel'");
    rc.channel = j["channel"].get<std::string>();

    // broker (optional)
    rc.broker = j.value("broker", std::string{"tcp://127.0.0.1:5570"});
    if (j.contains("broker_pubkey"))
        rc.broker_pubkey = j.at("broker_pubkey").get<std::string>();

    // producer timing
    rc.interval_ms = j.value("interval_ms", int{0});

    // consumer timing
    rc.timeout_ms = j.value("timeout_ms", int{-1});

    // loop timing policy (applies to both producer interval and consumer timeout)
    rc.loop_timing = parse_loop_timing(j.value("loop_timing", std::string{"fixed_pace"}));

    // loop trigger (Phase 1): shm (default) or messenger
    rc.loop_trigger = parse_loop_trigger(j.value("loop_trigger", std::string{"shm"}));

    // messenger_poll_ms: warn if >= 10 (values >= 10 risk adding latency)
    rc.messenger_poll_ms = j.value("messenger_poll_ms", int{5});
    if (rc.messenger_poll_ms >= 10)
    {
        std::fprintf(stderr,
                     "[actor] WARN: role '%s': messenger_poll_ms=%d is >= 10ms; "
                     "consider a smaller value for tighter message latency.\n",
                     role_name.c_str(), rc.messenger_poll_ms);
    }

    // heartbeat_interval_ms: 0 = 10 × interval_ms (Phase 2 acts on this)
    rc.heartbeat_interval_ms = j.value("heartbeat_interval_ms", int{0});

    // DataBlock LoopPolicy (HEP-CORE-0008) — pacing for acquire-side overrun detection
    rc.loop_policy = parse_loop_policy(j.value("loop_policy", std::string{"max_rate"}));
    rc.period_ms   = std::chrono::milliseconds{j.value("period_ms", int{0})};

    // shm block
    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &s     = j["shm"];
        rc.has_shm        = s.value("enabled",    false);
        rc.shm_secret     = s.value("secret",     uint64_t{0});
        rc.shm_slot_count = s.value("slot_count", uint32_t{4});

        const uint32_t legacy_size = s.value("slot_size", uint32_t{0});
        if (legacy_size > 0)
        {
            if (j.contains("slot_schema") && j["slot_schema"].is_object())
            {
                LOGGER_WARN("[actor] role '{}': 'shm.slot_size' ignored when "
                            "'slot_schema' is present", role_name);
            }
            else
            {
                rc.shm_slot_size = legacy_size;
                LOGGER_WARN("[actor] role '{}': 'shm.slot_size' without 'slot_schema' "
                            "uses deprecated raw bytearray mode", role_name);
            }
        }
    }

    // schemas
    if (j.contains("slot_schema") && j["slot_schema"].is_object())
        rc.slot_schema_json = j["slot_schema"];
    if (j.contains("flexzone_schema") && j["flexzone_schema"].is_object())
        rc.flexzone_schema_json = j["flexzone_schema"];

    // validation
    if (j.contains("validation"))
        rc.validation = parse_validation(j["validation"]);

    // per-role script (optional; falls back to actor-level "script" block when empty)
    if (j.contains("script"))
    {
        const auto &s = j["script"];
        if (!s.is_object())
            throw std::runtime_error(
                "Actor config: role '" + role_name +
                "': 'script' must be an object "
                "{\"type\": \"python\", \"module\": \"...\", \"path\": \"...\"}");
        rc.script_type     = s.value("type",   std::string{"python"});
        rc.script_module   = s.value("module", std::string{});
        rc.script_base_dir = s.value("path",   std::string{});
    }

    return rc;
}

} // anonymous namespace

// ============================================================================
// ActorConfig::from_json_file
// ============================================================================

ActorConfig ActorConfig::from_json_file(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Actor config: cannot open file: " + path);

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(f);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "Actor config: JSON parse error in '" + path + "': " + e.what());
    }

    // ── Parse config ─────────────────────────────────────────────────────────
    if (!j.contains("roles"))
        throw std::runtime_error(
            "Actor config: missing required field 'roles' in '" + path + "'");

    ActorConfig cfg;

    // script — optional; omit for --keygen / --list-roles.
    // Format: {"module": "sensor_node", "path": "/opt/scripts"}
    // Both fields may be empty if "script" is absent.
    if (j.contains("script"))
    {
        const auto &s = j["script"];
        if (!s.is_object())
            throw std::runtime_error(
                "Actor config: 'script' must be an object "
                "{\"module\": \"...\", \"path\": \"...\"} in '" + path + "'");
        cfg.script_module   = s.value("module", std::string{});
        cfg.script_base_dir = s.value("path",   std::string{});
    }

    // actor block
    if (j.contains("actor") && j["actor"].is_object())
    {
        const auto &a   = j["actor"];
        cfg.actor_uid   = a.value("uid",       std::string{});
        cfg.actor_name  = a.value("name",      std::string{});
        cfg.log_level   = a.value("log_level", std::string{"info"});

        if (a.contains("auth") && a["auth"].is_object())
        {
            const auto &auth = a["auth"];
            cfg.auth.keyfile  = auth.value("keyfile",  std::string{});
            cfg.auth.password = resolve_env_value(auth.value("password", std::string{}));
        }
    }
    else
    {
        cfg.log_level = j.value("log_level", std::string{"info"});
    }

    // --- UID: auto-generate if absent; warn if non-conforming ---
    // NOTE: from_json_file() is called before the Logger lifecycle starts,
    //       so pre-lifecycle messages use std::fprintf(stderr,...) directly.
    if (cfg.actor_uid.empty())
    {
        cfg.actor_uid = pylabhub::uid::generate_actor_uid(cfg.actor_name);
        std::fprintf(stderr, "[actor] auto-generated uid '%s' from name '%s'\n",
                     cfg.actor_uid.c_str(), cfg.actor_name.c_str());
    }
    else if (!pylabhub::uid::has_actor_prefix(cfg.actor_uid))
    {
        std::fprintf(stderr,
                     "[actor] WARN: uid '%s' does not start with 'ACTOR-'; "
                     "recommend the ACTOR-{NAME}-{8HEX} format.\n",
                     cfg.actor_uid.c_str());
    }

    // roles map (object required; may be empty for keygen/auth-only configs)
    if (j.contains("roles") && !j["roles"].is_object())
        throw std::runtime_error(
            "Actor config: 'roles' must be a JSON object in '" + path + "'");

    for (const auto &[role_name, role_json] : j["roles"].items())
    {
        if (!role_json.is_object())
            throw std::runtime_error(
                "Actor config: role '" + role_name +
                "' must be a JSON object in '" + path + "'");
        cfg.roles[role_name] = parse_role(role_name, role_json);
    }

    // ── Post-parse cross-field validation ─────────────────────────────────────
    for (const auto &[role_name, rc] : cfg.roles)
    {
        if (rc.loop_trigger == RoleConfig::LoopTrigger::Shm && !rc.has_shm)
        {
            throw std::runtime_error(
                "Actor config: role '" + role_name +
                "': loop_trigger='shm' requires shm.enabled=true");
        }
    }

    return cfg;
}

// ============================================================================
// ActorConfig::from_directory
// ============================================================================

ActorConfig ActorConfig::from_directory(const std::string &actor_dir)
{
    const std::string actor_json_path = actor_dir + "/actor.json";

    // Phase 1: parse the actor config (full validation via from_json_file).
    ActorConfig cfg = from_json_file(actor_json_path);

    // Phase 2: re-read JSON to extract the optional top-level "hub_dir" key.
    std::ifstream f(actor_json_path);
    if (!f.is_open())
        throw std::runtime_error(
            "ActorConfig::from_directory: cannot re-open: " + actor_json_path);

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(f);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "ActorConfig::from_directory: JSON re-parse error: " + std::string(e.what()));
    }

    const std::string hub_dir = j.value("hub_dir", std::string{});
    if (hub_dir.empty())
        return cfg; // no hub integration — roles use per-role broker/broker_pubkey

    // Phase 3: read hub.json for the broker endpoint.
    const std::string hub_json_path = hub_dir + "/hub.json";
    std::ifstream hf(hub_json_path);
    if (!hf.is_open())
        throw std::runtime_error(
            "ActorConfig::from_directory: cannot open hub config: " + hub_json_path);

    nlohmann::json hj;
    try
    {
        hj = nlohmann::json::parse(hf);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "ActorConfig::from_directory: hub.json parse error: " + std::string(e.what()));
    }

    std::string broker_endpoint;
    if (hj.contains("hub") && hj["hub"].is_object())
        broker_endpoint = hj["hub"].value("broker_endpoint", std::string{});

    if (broker_endpoint.empty())
        throw std::runtime_error(
            "ActorConfig::from_directory: hub.json missing hub.broker_endpoint in: " +
            hub_json_path);

    // Phase 4: read hub.pubkey for the CurveZMQ server public key (optional).
    std::string broker_pubkey;
    const std::string pubkey_path = hub_dir + "/hub.pubkey";
    std::ifstream pkf(pubkey_path);
    if (pkf.is_open())
    {
        std::getline(pkf, broker_pubkey);
        // Trim trailing whitespace / CR / LF.
        while (!broker_pubkey.empty() &&
               (broker_pubkey.back() == ' '  || broker_pubkey.back() == '\t' ||
                broker_pubkey.back() == '\r' || broker_pubkey.back() == '\n'))
            broker_pubkey.pop_back();

        if (broker_pubkey.size() != 40)
        {
            std::fprintf(stderr,
                         "[actor] WARN: hub.pubkey at '%s' is %zu chars (expected 40); "
                         "CurveZMQ disabled — connection will use plain TCP\n",
                         pubkey_path.c_str(), broker_pubkey.size());
            broker_pubkey.clear();
        }
    }
    else
    {
        std::fprintf(stderr,
                     "[actor] WARN: hub.pubkey not found at '%s'; "
                     "CurveZMQ disabled — connection will use plain TCP\n",
                     pubkey_path.c_str());
    }

    // Phase 5: override broker endpoint and pubkey in all roles.
    cfg.hub_dir = hub_dir;
    for (auto &[role_name, rc] : cfg.roles)
    {
        rc.broker      = broker_endpoint;
        rc.broker_pubkey = broker_pubkey;
    }

    std::fprintf(stderr,
                 "[actor] Hub integration: broker='%s', pubkey='%.8s...'\n",
                 broker_endpoint.c_str(),
                 broker_pubkey.empty() ? "(none)" : broker_pubkey.c_str());

    return cfg;
}

} // namespace pylabhub::actor
