/**
 * @file hub_config.cpp
 * @brief HubConfig composite — pImpl + JsonConfig backend, strict-key parsing.
 *
 * Mirrors `RoleConfig` in shape (HEP-CORE-0033 §6.1).  Owns the identity /
 * auth / script / logging / network / admin / broker / federation / state
 * sub-configs that together form a single `hub.json`.  No directional
 * `in_`/`out_` slots — the hub is single-sided.
 *
 * The legacy `pylabhub::HubConfig` lifecycle singleton (and its self-test)
 * was retired 2026-04-29 along with the never-built `pylabhub-hubshell`
 * binary.
 */
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_vault.hpp"
#include "utils/json_config.hpp"
#include "utils/security/key_file_acl.hpp"

#include <cassert>
#include <cstdio>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace pylabhub::config
{

// ============================================================================
// Impl
// ============================================================================

struct HubConfig::Impl
{
    utils::JsonConfig          jcfg;
    std::filesystem::path      base_dir;
    nlohmann::json             raw_json;

    HubIdentityConfig   identity;
    AuthConfig          auth;
    ScriptConfig        script;
    TimingConfig        timing;
    LoggingConfig       logging;
    HubNetworkConfig    network;
    HubAdminConfig      admin;
    HubBrokerConfig     broker;
    HubFederationConfig federation;
    HubStateConfig      state;

    void load_all(const nlohmann::json &j);
};

HubConfig::HubConfig() = default;
HubConfig::~HubConfig() = default;
HubConfig::HubConfig(HubConfig &&) noexcept = default;
HubConfig &HubConfig::operator=(HubConfig &&) noexcept = default;

// ============================================================================
// Strict-key whitelist (HEP-0033 §6.3)
// ============================================================================

/// Allowed top-level keys in hub.json.  Unknown keys throw a hard error so
/// operators see typos immediately.  Sub-objects (network, admin, broker,
/// federation, state, hub.*, script.*, logging.*) are validated by their
/// respective sub-parsers.
static const std::unordered_set<std::string> kAllowedTopLevelKeys = {
    "hub",
    "script", "stop_on_script_error", "python_venv",
    // Timing keys for the hub script tick (HEP-CORE-0033 Phase 7 Commit B).
    // Same parser + same enum + same JSON shape as RoleConfig — see
    // `src/include/utils/config/timing_config.hpp`.  `loop_timing`
    // is REQUIRED; `target_period_ms` / `target_rate_hz` exactly one
    // when policy is `fixed_rate`/`fixed_rate_with_compensation`.
    // The other TimingConfig fields (`queue_io_wait_timeout_ratio`,
    // `heartbeat_interval_ms`) are intentionally NOT whitelisted at the
    // hub top level — they are queue / role-broker concepts that have
    // no meaning for the hub script tick.  parse_timing_config defaults
    // them to 0 / kDefault on the hub side.
    "loop_timing", "target_period_ms", "target_rate_hz",
    "logging",
    "network", "admin", "broker", "federation", "state",
};

static void validate_top_level_keys(const nlohmann::json &j)
{
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (kAllowedTopLevelKeys.find(it.key()) == kAllowedTopLevelKeys.end())
            throw std::runtime_error("hub: unknown config key '" + it.key() + "'");
    }
}

// ============================================================================
// Impl::load_all
// ============================================================================

void HubConfig::Impl::load_all(const nlohmann::json &j)
{
    validate_top_level_keys(j);

    identity   = parse_hub_identity_config(j);
    auth       = parse_auth_config(j, "hub");  // reads j["hub"]["auth"]
    script     = parse_script_config(j, base_dir, "hub");
    timing     = parse_timing_config(j, "hub");
    logging    = parse_logging_config(j, "hub");
    network    = parse_hub_network_config(j);
    admin      = parse_hub_admin_config(j);
    broker     = parse_hub_broker_config(j);
    federation = parse_hub_federation_config(j);
    state      = parse_hub_state_config(j);
}

// ============================================================================
// Factory methods
// ============================================================================

HubConfig HubConfig::load(const std::string &path)
{
    namespace fs = std::filesystem;

    HubConfig cfg;
    cfg.impl_ = std::make_unique<Impl>();
    auto &s = *cfg.impl_;

    s.base_dir = fs::path(path).parent_path();

    std::error_code ec;
    s.jcfg = utils::JsonConfig(fs::path(path), /*createIfMissing=*/false, &ec);
    if (ec)
        throw std::runtime_error(
            "HubConfig: cannot open config file '" + path + "': " + ec.message());

    {
        std::error_code lock_ec;
        auto rlock = s.jcfg.lock_for_read(&lock_ec);
        if (!rlock)
            throw std::runtime_error(
                "HubConfig: cannot read config '" + path + "': " + lock_ec.message());
        s.raw_json = rlock->json();
    }

    s.load_all(s.raw_json);

    // Security diagnostic — symmetric with role-side
    // RoleConfig::load (HEP-CORE-0033 §7.1 + HEP-CORE-0024 §3.4.1).
    // Warn if the operator's auth.keyfile path resolves inside the
    // hub directory; this is the load-bearing reminder of the
    // script-write attack surface trade-off.
    utils::HubDirectory::warn_if_keyfile_in_hub_dir(
        s.base_dir, s.auth.keyfile);

    return cfg;
}

HubConfig HubConfig::load_from_directory(const std::string &dir)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::weakly_canonical(fs::path(dir));
    return load((base / "hub.json").string());
}

// ============================================================================
// Accessors
// ============================================================================

const HubIdentityConfig   &HubConfig::identity()   const { assert(impl_); return impl_->identity; }
const AuthConfig          &HubConfig::auth()       const { assert(impl_); return impl_->auth; }
const ScriptConfig        &HubConfig::script()     const { assert(impl_); return impl_->script; }
const TimingConfig        &HubConfig::timing()     const { assert(impl_); return impl_->timing; }
const LoggingConfig       &HubConfig::logging()    const { assert(impl_); return impl_->logging; }
const HubNetworkConfig    &HubConfig::network()    const { assert(impl_); return impl_->network; }
const HubAdminConfig      &HubConfig::admin()      const { assert(impl_); return impl_->admin; }
const HubBrokerConfig     &HubConfig::broker()     const { assert(impl_); return impl_->broker; }
const HubFederationConfig &HubConfig::federation() const { assert(impl_); return impl_->federation; }
const HubStateConfig      &HubConfig::state()      const { assert(impl_); return impl_->state; }

// ============================================================================
// Vault operations
// ============================================================================

// `hub.auth.keyfile` is the source of truth for the vault file
// location at runtime per HEP-CORE-0033 §7.1 (finalized 2026-05-31):
//   - Non-empty (relative) → resolved against hub_dir.
//   - Non-empty (absolute) → used as-is.
//   - Non-empty + file absent at resolved path → throw (operator
//     configured a vault but the file is not there; no silent
//     fallback).
//
// The empty case is unreachable here — `parse_auth_config` in
// auth_config.hpp rejects empty `auth.keyfile` at config-load
// (HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1, finalized 2026-05-31:
// pylabhub is a vault; no in-memory CURVE mode exists).
//
// Path resolution uses pylabhub::utils::security::resolve_keyfile_path()
// — single source of truth shared with role_config.cpp.
bool HubConfig::load_keypair(const std::string &password)
{
    assert(impl_);
    auto &auth = impl_->auth;
    // auth.keyfile guaranteed non-empty by parse_auth_config; an empty
    // value would have thrown at config-load.

    const auto &hub_dir = impl_->base_dir;
    const auto &uid     = impl_->identity.uid;

    namespace fs       = std::filesystem;
    namespace security = pylabhub::utils::security;
    const fs::path vault_path =
        security::resolve_keyfile_path(auth.keyfile, hub_dir);
    if (!fs::exists(vault_path))
    {
        throw std::runtime_error(
            "[plh_hub] Error: hub.auth.keyfile = '" + auth.keyfile +
            "' resolves to '" + vault_path.string() +
            "' which does not exist.  Run `plh_hub --keygen` to "
            "create the vault (HEP-CORE-0033 §7.1).");
    }

    // HEP-CORE-0035 §4.6.2: verify ACL contract BEFORE reading the
    // secret.  File must be 0600 + owner-uid; parent dir must be 0700
    // + owner-uid.  Failure produces an OpenSSH-style actionable
    // diagnostic naming the path, observed mode, required mode, and
    // the exact `chmod` command to fix.
    if (auto v = security::verify_keyfile_acl(
            vault_path, security::KeyFileRole::VaultFile);
        !v.ok)
        throw std::runtime_error(
            "[plh_hub] Refusing to load vault — ACL check failed (HEP-"
            "CORE-0035 §4.6.2):\n" + v.diagnostic);
    if (auto v = security::verify_keyfile_acl(
            vault_path.parent_path(), security::KeyFileRole::VaultDir);
        !v.ok)
        throw std::runtime_error(
            "[plh_hub] Refusing to load vault — parent dir ACL check "
            "failed (HEP-CORE-0035 §4.6.2):\n" + v.diagnostic);

    const auto vault = utils::HubVault::open(vault_path, uid, password);
    auth.client_pubkey = vault.broker_curve_public_key();
    auth.client_seckey = vault.broker_curve_secret_key();
    impl_->admin.admin_token = vault.admin_token();
    std::fprintf(stderr, "[plh_hub] Loaded vault from '%s' (pubkey: %.8s...)\n",
                 vault_path.string().c_str(),
                 vault.broker_curve_public_key().c_str());
    return true;
}

std::string HubConfig::create_keypair(const std::string &password)
{
    assert(impl_);
    const auto &auth = impl_->auth;
    // auth.keyfile guaranteed non-empty by parse_auth_config
    // (HEP-CORE-0033 §7.1, finalized 2026-05-31); empty is rejected
    // at config-load before --keygen dispatch.

    const auto &hub_dir = impl_->base_dir;
    const auto &uid     = impl_->identity.uid;
    const std::filesystem::path vault_path =
        pylabhub::utils::security::resolve_keyfile_path(
            auth.keyfile, hub_dir);

    // No-silent-overwrite (HEP-CORE-0033 §7.1, added 2026-05-31): refuse
    // to clobber an existing vault file.  --keygen produces fresh CURVE
    // material AND a fresh admin token; overwriting destroys both,
    // invalidating any federation peer that pinned the old pubkey and
    // every admin-channel session bound to the old token.  Operator
    // must remove the file explicitly to re-keygen.
    if (std::filesystem::exists(vault_path))
        throw std::runtime_error(
            "[plh_hub] Error: vault already exists at '" +
            vault_path.string() +
            "'. Refusing to overwrite — that would destroy the existing "
            "keypair (and any ChaCha20-Poly1305 admin token / CURVE "
            "secret bound to it).  If you really want a new keypair, "
            "remove the file first:\n    rm '" + vault_path.string() +
            "'\nthen re-run --keygen (HEP-CORE-0033 §7.1).");

    const auto vault = utils::HubVault::create(vault_path, uid, password);
    // Publish the broker's CURVE public key at <hub_dir>/hub.pubkey
    // (still hub_dir-relative — the public-key file location is part
    // of the hub-directory layout per HEP-CORE-0033 §7, independent
    // of where the operator placed the vault).  Without this the
    // role's `broker_pubkey` stays empty and the producer→hub→
    // consumer pipeline either bypasses CURVE auth (if both sides
    // allow it) or fails at REG_REQ time
    // (audit REVIEW_HEP_0033_PostP9_2026-05-05.md F1).
    vault.publish_public_key(hub_dir);
    return vault.broker_curve_public_key();
}

// ============================================================================
// Raw JSON / JsonConfig operations
// ============================================================================

const nlohmann::json &HubConfig::raw() const
{
    assert(impl_);
    return impl_->raw_json;
}

bool HubConfig::reload_if_changed()
{
    assert(impl_);
    bool updated = false;
    impl_->jcfg.transaction(utils::JsonConfig::AccessFlags::ReloadFirst)
        .read([&](const nlohmann::json &j)
    {
        if (j != impl_->raw_json)
        {
            impl_->raw_json = j;
            impl_->load_all(j);
            updated = true;
        }
    });
    return updated;
}

const std::filesystem::path &HubConfig::base_dir() const
{
    assert(impl_);
    return impl_->base_dir;
}

} // namespace pylabhub::config
