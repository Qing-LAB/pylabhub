/**
 * @file actor_config.cpp
 * @brief ActorConfig JSON parsing — multi-role format.
 *
 * Supports both the new "roles" map format and the legacy single-role
 * flat format (deprecated, logged with a warning).
 */
#include "actor_config.hpp"

#include "plh_service.hpp"        // LOGGER_WARN (used for post-lifecycle contexts)
#include "utils/uid_utils.hpp"    // generate_actor_uid, has_actor_prefix

#include <cstdio>                 // std::fprintf (for pre-lifecycle warnings)

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace pylabhub::actor
{

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

ValidationPolicy::OnFail parse_on_fail(const std::string &s)
{
    if (s == "skip") return ValidationPolicy::OnFail::Skip;
    if (s == "pass") return ValidationPolicy::OnFail::Pass;
    throw std::runtime_error(
        "Actor config: invalid 'on_checksum_fail' = '" + s +
        "' (must be 'skip' or 'pass')");
}

ValidationPolicy::OnPyError parse_on_py_error(const std::string &s)
{
    if (s == "continue") return ValidationPolicy::OnPyError::Continue;
    if (s == "stop")     return ValidationPolicy::OnPyError::Stop;
    throw std::runtime_error(
        "Actor config: invalid 'on_python_error' = '" + s +
        "' (must be 'continue' or 'stop')");
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
    v.on_checksum_fail  = parse_on_fail(j.value("on_checksum_fail",  "skip"));
    v.on_python_error   = parse_on_py_error(j.value("on_python_error", "continue"));
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

    // producer timing
    rc.interval_ms = j.value("interval_ms", int{0});

    // consumer timing
    rc.timeout_ms = j.value("timeout_ms", int{-1});

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

    return rc;
}

/// Parse the legacy single-role flat format, wrapping it as a single-entry
/// roles map. The role name is "<kind>:<channel>".
ActorConfig parse_legacy_flat(const nlohmann::json &j, const std::string &path)
{
    LOGGER_WARN("[actor] config '{}': flat single-role format is deprecated. "
                "Use the new 'roles' map format.", path);

    ActorConfig cfg;
    cfg.script_path = j.value("script", std::string{});
    cfg.log_level   = j.value("log_level", std::string{"info"});

    const std::string kind_str = j.value("role", "producer");
    RoleConfig rc;
    if (kind_str == "producer")
        rc.kind = RoleConfig::Kind::Producer;
    else
        rc.kind = RoleConfig::Kind::Consumer;

    if (!j.contains("channel") || !j["channel"].is_string())
        throw std::runtime_error(
            "Actor config: legacy format requires 'channel' field in '" + path + "'");
    rc.channel = j["channel"].get<std::string>();
    rc.broker  = j.value("broker", std::string{"tcp://127.0.0.1:5570"});

    // Legacy interval
    rc.interval_ms = static_cast<int>(j.value("write_interval_ms", uint32_t{0}));

    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &s     = j["shm"];
        rc.has_shm        = s.value("enabled",    false);
        rc.shm_secret     = s.value("secret",     uint64_t{0});
        rc.shm_slot_count = s.value("slot_count", uint32_t{4});
        const uint32_t legacy_size = s.value("slot_size", uint32_t{0});
        if (legacy_size > 0 && !j.contains("slot_schema"))
            rc.shm_slot_size = legacy_size;
    }
    if (j.contains("slot_schema") && j["slot_schema"].is_object())
        rc.slot_schema_json = j["slot_schema"];
    if (j.contains("flexzone_schema") && j["flexzone_schema"].is_object())
        rc.flexzone_schema_json = j["flexzone_schema"];
    if (j.contains("validation"))
        rc.validation = parse_validation(j["validation"]);

    // Name the synthetic role after its channel
    const std::string role_name = rc.channel;
    cfg.roles[role_name] = std::move(rc);
    return cfg;
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

    // ── Detect format (new "roles" map vs legacy flat) ────────────────────────
    if (!j.contains("roles"))
    {
        // Legacy flat format.
        ActorConfig cfg = parse_legacy_flat(j, path);
        if (cfg.script_path.empty())
            throw std::runtime_error(
                "Actor config: missing required field 'script' in '" + path + "'");
        return cfg;
    }

    // ── New multi-role format ─────────────────────────────────────────────────
    ActorConfig cfg;

    // script (required)
    if (!j.contains("script") || !j["script"].is_string())
        throw std::runtime_error(
            "Actor config: missing required field 'script' in '" + path + "'");
    cfg.script_path = j["script"].get<std::string>();

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

    // roles map (required; at least one entry)
    if (!j["roles"].is_object() || j["roles"].empty())
        throw std::runtime_error(
            "Actor config: 'roles' must be a non-empty object in '" + path + "'");

    for (const auto &[role_name, role_json] : j["roles"].items())
    {
        if (!role_json.is_object())
            throw std::runtime_error(
                "Actor config: role '" + role_name +
                "' must be a JSON object in '" + path + "'");
        cfg.roles[role_name] = parse_role(role_name, role_json);
    }

    return cfg;
}

} // namespace pylabhub::actor
