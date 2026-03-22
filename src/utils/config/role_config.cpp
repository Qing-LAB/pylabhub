/**
 * @file role_config.cpp
 * @brief RoleConfig implementation — factory, accessors, JsonConfig backend.
 */
#include "utils/config/role_config.hpp"
#include "utils/json_config.hpp"
#include "utils/role_directory.hpp"

#include <any>
#include <cassert>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace pylabhub::config
{

// ============================================================================
// Impl
// ============================================================================

struct RoleConfig::Impl
{
    // JsonConfig backend — thread-safe, process-safe file I/O.
    utils::JsonConfig jcfg;

    // Metadata.
    std::string           role_tag;
    std::filesystem::path base_dir;

    // Raw JSON snapshot (for raw() accessor and role_parser callback).
    nlohmann::json raw_json;

    // ── Non-directional categories ───────────────────────────────────
    IdentityConfig   identity;
    AuthConfig       auth;
    ScriptConfig     script;
    TimingConfig     timing;
    InboxConfig      inbox;
    ValidationConfig validation;
    StartupConfig    startup;
    MonitoringConfig monitoring;

    // ── Directional categories (two slots each) ──────────────────────
    HubConfig                    in_hub;
    HubConfig                    out_hub;
    TransportConfig              in_transport;
    TransportConfig              out_transport;
    ShmConfig                    in_shm;
    ShmConfig                    out_shm;
    DirectionalValidationConfig  in_validation;
    DirectionalValidationConfig  out_validation;
    std::string                  in_channel;
    std::string                  out_channel;

    // ── Role-specific extension (type-erased) ────────────────────────
    std::any role_data;

    /// Parse all common categories from JSON.
    void load_common(const nlohmann::json &j);
};

// ============================================================================
// Special members
// ============================================================================

RoleConfig::RoleConfig() = default;
RoleConfig::~RoleConfig() = default;
RoleConfig::RoleConfig(RoleConfig &&) noexcept = default;
RoleConfig &RoleConfig::operator=(RoleConfig &&) noexcept = default;

// ============================================================================
// Impl::load_common
// ============================================================================

void RoleConfig::Impl::load_common(const nlohmann::json &j)
{
    const char *tag = role_tag.c_str();

    // Default period: producer=100ms, others=0.
    const double default_period =
        (role_tag == "producer") ? 100.0 : 0.0;

    // ── Non-directional categories ───────────────────────────────────
    identity   = parse_identity_config(j, role_tag);
    auth       = parse_auth_config(j, role_tag);
    script     = parse_script_config(j, base_dir, tag);
    timing     = parse_timing_config(j, tag, default_period);
    inbox      = parse_inbox_config(j, tag);
    validation = parse_validation_config(j);
    startup    = parse_startup_config(j, tag);
    monitoring = parse_monitoring_config(j);

    // ── Directional categories (always load both slots) ──────────────
    in_hub        = parse_hub_config(j, base_dir, "in");
    out_hub       = parse_hub_config(j, base_dir, "out");
    in_transport  = parse_transport_config(j, "in",  tag);
    out_transport = parse_transport_config(j, "out", tag);
    in_shm        = parse_shm_config(j, "in",  tag);
    out_shm       = parse_shm_config(j, "out", tag);
    in_validation  = parse_directional_validation(j, "in");
    out_validation = parse_directional_validation(j, "out");
    in_channel    = j.value("in_channel", std::string{});
    out_channel   = j.value("out_channel", std::string{});
}

// ============================================================================
// Factory methods
// ============================================================================

RoleConfig RoleConfig::load(const std::string &path,
                             const char *role_tag,
                             RoleParser role_parser)
{
    namespace fs = std::filesystem;

    RoleConfig cfg;
    cfg.impl_ = std::make_unique<Impl>();
    auto &s = *cfg.impl_;

    s.role_tag = role_tag;
    s.base_dir = fs::path(path).parent_path();

    // JsonConfig as backend.
    std::error_code ec;
    s.jcfg = utils::JsonConfig(fs::path(path), /*createIfMissing=*/false, &ec);
    if (ec)
        throw std::runtime_error(
            "RoleConfig: cannot open config file '" + path + "': " + ec.message());

    // Step 1: Read from JsonConfig's in-memory cache (no file I/O —
    // the constructor already loaded the file).
    {
        std::error_code lock_ec;
        auto rlock = s.jcfg.lock_for_read(&lock_ec);
        if (!rlock)
            throw std::runtime_error(
                "RoleConfig: cannot read config '" + path + "': " + lock_ec.message());
        s.raw_json = rlock->json();
    }

    // Step 2: Parse outside the transaction — exceptions propagate normally.
    // Clean separation: JsonConfig does file I/O, we do config validation.
    s.load_common(s.raw_json);

    // Security check.
    utils::RoleDirectory::warn_if_keyfile_in_role_dir(s.base_dir, s.auth.keyfile);

    // Role-specific extension.
    if (role_parser)
        s.role_data = role_parser(s.raw_json, cfg);

    return cfg;
}

RoleConfig RoleConfig::load_from_directory(const std::string &dir,
                                            const char *role_tag,
                                            RoleParser role_parser)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::weakly_canonical(fs::path(dir));
    const fs::path cfg_path = base / (std::string(role_tag) + ".json");
    return load(cfg_path.string(), role_tag, std::move(role_parser));
}

// ============================================================================
// Non-directional accessors
// ============================================================================

const IdentityConfig   &RoleConfig::identity()   const { assert(impl_); return impl_->identity; }
const AuthConfig       &RoleConfig::auth()       const { assert(impl_); return impl_->auth; }
const ScriptConfig     &RoleConfig::script()     const { assert(impl_); return impl_->script; }
const TimingConfig     &RoleConfig::timing()     const { assert(impl_); return impl_->timing; }
const InboxConfig      &RoleConfig::inbox()      const { assert(impl_); return impl_->inbox; }
const ValidationConfig &RoleConfig::validation() const { assert(impl_); return impl_->validation; }
const StartupConfig    &RoleConfig::startup()    const { assert(impl_); return impl_->startup; }
const MonitoringConfig &RoleConfig::monitoring() const { assert(impl_); return impl_->monitoring; }

// ============================================================================
// Directional accessors
// ============================================================================

const HubConfig                   &RoleConfig::in_hub()        const { assert(impl_); return impl_->in_hub; }
const HubConfig                   &RoleConfig::out_hub()       const { assert(impl_); return impl_->out_hub; }
const TransportConfig             &RoleConfig::in_transport()  const { assert(impl_); return impl_->in_transport; }
const TransportConfig             &RoleConfig::out_transport() const { assert(impl_); return impl_->out_transport; }
const ShmConfig                   &RoleConfig::in_shm()        const { assert(impl_); return impl_->in_shm; }
const ShmConfig                   &RoleConfig::out_shm()       const { assert(impl_); return impl_->out_shm; }
const DirectionalValidationConfig &RoleConfig::in_validation() const { assert(impl_); return impl_->in_validation; }
const DirectionalValidationConfig &RoleConfig::out_validation() const { assert(impl_); return impl_->out_validation; }
const std::string                 &RoleConfig::in_channel()    const { assert(impl_); return impl_->in_channel; }
const std::string                 &RoleConfig::out_channel()   const { assert(impl_); return impl_->out_channel; }

// ============================================================================
// Mutable auth
// ============================================================================

AuthConfig &RoleConfig::mutable_auth()
{
    assert(impl_);
    return impl_->auth;
}

// ============================================================================
// Raw JSON / reload
// ============================================================================

const nlohmann::json &RoleConfig::raw() const
{
    assert(impl_);
    return impl_->raw_json;
}

bool RoleConfig::reload_if_changed()
{
    assert(impl_);
    bool updated = false;
    impl_->jcfg.transaction(utils::JsonConfig::AccessFlags::ReloadFirst)
        .read([&](const nlohmann::json &j)
    {
        if (j != impl_->raw_json)
        {
            impl_->raw_json = j;
            impl_->load_common(j);
            updated = true;
        }
    });
    return updated;
}

// ============================================================================
// Role-specific data
// ============================================================================

bool RoleConfig::has_role_data() const
{
    assert(impl_);
    return impl_->role_data.has_value();
}

const std::any &RoleConfig::role_data_any_() const
{
    assert(impl_);
    return impl_->role_data;
}

std::any &RoleConfig::mutable_role_data_any_()
{
    assert(impl_);
    return impl_->role_data;
}

// ============================================================================
// Metadata
// ============================================================================

const std::string           &RoleConfig::role_tag() const { assert(impl_); return impl_->role_tag; }
const std::filesystem::path &RoleConfig::base_dir() const { assert(impl_); return impl_->base_dir; }

} // namespace pylabhub::config
