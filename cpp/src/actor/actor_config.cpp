/**
 * @file actor_config.cpp
 * @brief ActorConfig JSON parsing implementation.
 */
#include "actor_config.hpp"

#include "plh_service.hpp" // LOGGER_WARN

#include <fstream>
#include <stdexcept>

namespace pylabhub::actor
{

// ── helpers ───────────────────────────────────────────────────────────────────

namespace
{

ValidationPolicy::Checksum parse_checksum(const std::string &s, const std::string &key)
{
    if (s == "none")    return ValidationPolicy::Checksum::None;
    if (s == "update")  return ValidationPolicy::Checksum::Update;
    if (s == "enforce") return ValidationPolicy::Checksum::Enforce;
    throw std::runtime_error(
        "Runner: invalid '" + key + "' value '" + s +
        "' (must be 'none', 'update', or 'enforce')");
}

ValidationPolicy::OnFail parse_on_fail(const std::string &s)
{
    if (s == "skip") return ValidationPolicy::OnFail::Skip;
    if (s == "pass") return ValidationPolicy::OnFail::Pass;
    throw std::runtime_error(
        "Runner: invalid 'on_checksum_fail' value '" + s +
        "' (must be 'skip' or 'pass')");
}

ValidationPolicy::OnPyError parse_on_py_error(const std::string &s)
{
    if (s == "continue") return ValidationPolicy::OnPyError::Continue;
    if (s == "stop")     return ValidationPolicy::OnPyError::Stop;
    throw std::runtime_error(
        "Runner: invalid 'on_python_error' value '" + s +
        "' (must be 'continue' or 'stop')");
}

} // anonymous namespace

// ── from_json_file ────────────────────────────────────────────────────────────

ActorConfig ActorConfig::from_json_file(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        throw std::runtime_error("Runner: cannot open config file: " + path);
    }

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(f);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "Runner: JSON parse error in '" + path + "': " + e.what());
    }

    ActorConfig cfg;

    // ── role ──────────────────────────────────────────────────────────────────
    {
        const std::string role_str = j.value("role", "producer");
        if (role_str == "producer")
            cfg.role = ActorConfig::Role::Producer;
        else if (role_str == "consumer")
            cfg.role = ActorConfig::Role::Consumer;
        else
            throw std::runtime_error(
                "Runner: invalid role '" + role_str +
                "' in '" + path + "' (must be 'producer' or 'consumer')");
    }

    // ── required fields ───────────────────────────────────────────────────────
    if (!j.contains("channel") || !j["channel"].is_string())
    {
        throw std::runtime_error(
            "Runner: missing required string field 'channel' in '" + path + "'");
    }
    cfg.channel_name = j["channel"].get<std::string>();

    if (!j.contains("script") || !j["script"].is_string())
    {
        throw std::runtime_error(
            "Runner: missing required string field 'script' in '" + path + "'");
    }
    cfg.script_path = j["script"].get<std::string>();

    // ── optional top-level fields ─────────────────────────────────────────────
    cfg.broker_endpoint   = j.value("broker",            "tcp://127.0.0.1:5570");
    cfg.log_level         = j.value("log_level",         "info");
    cfg.write_interval_ms = j.value("write_interval_ms", uint32_t{0});

    // ── slot_schema / flexzone_schema ─────────────────────────────────────────
    if (j.contains("slot_schema") && j["slot_schema"].is_object())
    {
        cfg.slot_schema_json = j["slot_schema"];
    }
    if (j.contains("flexzone_schema") && j["flexzone_schema"].is_object())
    {
        cfg.flexzone_schema_json = j["flexzone_schema"];
    }

    // ── shm block ─────────────────────────────────────────────────────────────
    if (j.contains("shm") && j["shm"].is_object())
    {
        const auto &s     = j["shm"];
        cfg.has_shm       = s.value("enabled",    false);
        cfg.shm_secret    = s.value("secret",     uint64_t{0});
        cfg.shm_slot_count = s.value("slot_count", uint32_t{4});

        // Legacy slot_size: accepted only when no slot_schema is given.
        const uint32_t legacy_size = s.value("slot_size", uint32_t{0});
        if (legacy_size > 0)
        {
            if (!cfg.slot_schema_json.is_null())
            {
                // Ignore legacy slot_size when schema is present.
                LOGGER_WARN("[actor] config '{}': 'shm.slot_size' is ignored when "
                            "'slot_schema' is present — size is computed from schema",
                            path);
            }
            else
            {
                cfg.shm_slot_size = legacy_size;
                LOGGER_WARN("[actor] config '{}': 'shm.slot_size' without 'slot_schema' "
                            "uses legacy raw bytearray mode (deprecated)", path);
            }
        }
    }

    // ── validation block ──────────────────────────────────────────────────────
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &v = j["validation"];

        cfg.validation.slot_checksum =
            parse_checksum(v.value("slot_checksum", "enforce"), "slot_checksum");

        cfg.validation.flexzone_checksum =
            parse_checksum(v.value("flexzone_checksum", "enforce"), "flexzone_checksum");

        cfg.validation.on_checksum_fail =
            parse_on_fail(v.value("on_checksum_fail", "pass"));

        cfg.validation.on_python_error =
            parse_on_py_error(v.value("on_python_error", "continue"));
    }

    return cfg;
}

} // namespace pylabhub::actor
