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
#include "utils/security/key_store.hpp"
#include "utils/security/known_roles.hpp"
#include "utils/security/secure_buffer.hpp"

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
    utils::JsonConfig jcfg;
    std::filesystem::path base_dir;
    nlohmann::json raw_json;

    HubIdentityConfig identity;
    AuthConfig auth;
    ScriptConfig script;
    TimingConfig timing;
    LoggingConfig logging;
    HubNetworkConfig network;
    HubAdminConfig admin;
    HubBrokerConfig broker;
    HubFederationConfig federation;
    HubStateConfig state;

    /// Operator-managed allowlist extracted from the encrypted vault by
    /// load_keypair() (HEP-CORE-0035 §4.8).  Empty = deny-all bootstrap.
    std::vector<::pylabhub::broker::KnownRole> known_roles;

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
    "script",
    "stop_on_script_error",
    "python_venv",
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
    "loop_timing",
    "target_period_ms",
    "target_rate_hz",
    "logging",
    "network",
    "admin",
    "broker",
    "federation",
    "state",
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

    identity = parse_hub_identity_config(j);
    auth = parse_auth_config(j, "hub"); // reads j["hub"]["auth"]
    script = parse_script_config(j, base_dir, "hub", /*script_optional=*/true);
    timing = parse_timing_config(j, "hub");
    logging = parse_logging_config(j, "hub");
    network = parse_hub_network_config(j);
    admin = parse_hub_admin_config(j);
    broker = parse_hub_broker_config(j);
    federation = parse_hub_federation_config(j);
    state = parse_hub_state_config(j);
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
        throw std::runtime_error("HubConfig: cannot open config file '" + path +
                                 "': " + ec.message());

    {
        std::error_code lock_ec;
        auto rlock = s.jcfg.lock_for_read(&lock_ec);
        if (!rlock)
            throw std::runtime_error("HubConfig: cannot read config '" + path +
                                     "': " + lock_ec.message());
        s.raw_json = rlock->json();
    }

    s.load_all(s.raw_json);

    // Security diagnostic — symmetric with role-side
    // RoleConfig::load (HEP-CORE-0033 §7.1 + HEP-CORE-0024 §3.4.1).
    // Warn if the operator's auth.keyfile path resolves inside the
    // hub directory; this is the load-bearing reminder of the
    // script-write attack surface trade-off.
    utils::HubDirectory::warn_if_keyfile_in_hub_dir(s.base_dir, s.auth.keyfile);

    // HEP-CORE-0035 §4.6 Tier-1: config file is non-secret but
    // references a vault.  verify_keyfile_acl can populate a
    // diagnostic in TWO non-fatal cases for ConfigFileReferencingVault:
    //   1. World-writable (v.ok=false, v.diagnostic set) — config-
    //      injection vector; operator MUST fix.
    //   2. Group-readable (v.ok=true, v.diagnostic set) — side-
    //      channel leak of the vault path to a wider audience.
    // Both deserve a stderr WARN, so we gate on `!v.diagnostic.empty()`
    // rather than `!v.ok` — otherwise the group-readable advisory
    // surface is dead.  Non-fatal in both cases: hub.json is
    // operator-managed and overly strict enforcement here would
    // break existing tooling.  Distinct from §4.6.2 which IS fatal
    // for the vault file itself.
    if (auto v = utils::security::verify_keyfile_acl(
            fs::path(path), utils::security::KeyFileRole::ConfigFileReferencingVault);
        !v.diagnostic.empty())
    {
        std::fprintf(stderr,
                     "[plh_hub] WARN: hub.json ACL advisory "
                     "(HEP-CORE-0035 §4.6.1):\n%s\n",
                     v.diagnostic.c_str());
    }

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

const HubIdentityConfig &HubConfig::identity() const
{
    assert(impl_);
    return impl_->identity;
}
const AuthConfig &HubConfig::auth() const
{
    assert(impl_);
    return impl_->auth;
}
const ScriptConfig &HubConfig::script() const
{
    assert(impl_);
    return impl_->script;
}
const TimingConfig &HubConfig::timing() const
{
    assert(impl_);
    return impl_->timing;
}
const LoggingConfig &HubConfig::logging() const
{
    assert(impl_);
    return impl_->logging;
}
const HubNetworkConfig &HubConfig::network() const
{
    assert(impl_);
    return impl_->network;
}
const HubAdminConfig &HubConfig::admin() const
{
    assert(impl_);
    return impl_->admin;
}
const HubBrokerConfig &HubConfig::broker() const
{
    assert(impl_);
    return impl_->broker;
}
const HubFederationConfig &HubConfig::federation() const
{
    assert(impl_);
    return impl_->federation;
}
const HubStateConfig &HubConfig::state() const
{
    assert(impl_);
    return impl_->state;
}
const std::vector<::pylabhub::broker::KnownRole> &HubConfig::known_roles() const
{
    assert(impl_);
    return impl_->known_roles;
}

// ============================================================================
// Vault operations
// ============================================================================

namespace
{
/// Shared extraction: turn a vault's opaque `known_roles` document into
/// the validated in-memory vector (HEP-CORE-0035 §4.8).  An empty object
/// is the §4.8.4 deny-all bootstrap → empty list.  Used by `load_keypair`
/// (identity + allowlist in one vault open).
std::vector<::pylabhub::broker::KnownRole> extract_known_roles(const nlohmann::json &kr)
{
    namespace security = pylabhub::utils::security;
    if (kr.is_object() && !kr.empty())
        return security::KnownRolesStore::from_json(kr, "hub vault known_roles").list();
    return {};
}
} // namespace

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
    const auto &uid = impl_->identity.uid;

    namespace fs = std::filesystem;
    namespace security = pylabhub::utils::security;
    const fs::path vault_path = security::resolve_keyfile_path(auth.keyfile, hub_dir);
    if (!fs::exists(vault_path))
    {
        throw std::runtime_error("[plh_hub] Error: hub.auth.keyfile = '" + auth.keyfile +
                                 "' resolves to '" + vault_path.string() +
                                 "' which does not exist.  Run `plh_hub --keygen` to "
                                 "create the vault (HEP-CORE-0033 §7.1).");
    }

    // HEP-CORE-0035 §4.6.2: verify ACL contract BEFORE reading the
    // secret.  File must be 0600 + owner-uid; parent dir must be 0700
    // + owner-uid.  Failure produces an OpenSSH-style actionable
    // diagnostic naming the path, observed mode, required mode, and
    // the exact `chmod` command to fix.
    if (auto v = security::verify_keyfile_acl(vault_path, security::KeyFileRole::VaultFile); !v.ok)
        throw std::runtime_error("[plh_hub] Refusing to load vault — ACL check failed (HEP-"
                                 "CORE-0035 §4.6.2):\n" +
                                 v.diagnostic);
    // Edge: an absolute keyfile with no parent (bare-filename like
    // "/x.vault") has parent_path() = "/" which is a valid dir to
    // check.  Operator-relative paths joined with base_dir always
    // produce a non-empty parent.  Defensive skip if somehow empty.
    if (vault_path.has_parent_path())
    {
        if (auto v = security::verify_keyfile_acl(vault_path.parent_path(),
                                                  security::KeyFileRole::VaultDir);
            !v.ok)
            throw std::runtime_error("[plh_hub] Refusing to load vault — parent dir ACL check "
                                     "failed (HEP-CORE-0035 §4.6.2):\n" +
                                     v.diagnostic);
    }

    const auto vault = utils::HubVault::open(vault_path, uid, password);
    {
        const auto pub = vault.broker_curve_public_key(); // string_view
        const auto sec = vault.broker_curve_secret_key(); // string_view
        const auto adm = vault.admin_token();             // string_view

        // HEP-CORE-0040 §171: identity keypair lives in
        // `pylabhub::utils::security::secure().keys()` (LockedKey storage,
        // mlock'd + zero-on-destruct).  `add_identity_from_z85` is the
        // single site (production + tests) where the (pub_z85 ||
        // sec_z85) layout is defined; it packs into a SecureBuffer<80>
        // and hands off, then KeyStore + SecureBuffer dtor both zero
        // the source.  No `std::string` copy survives.
        pylabhub::utils::security::secure().keys().add_identity_from_z85(
            pylabhub::utils::security::kHubIdentityName, pub, sec);

        // Admin token is a separate secret; for now it continues as a
        // std::string on AdminConfig (HEP-CORE-0040 §175 deferred —
        // admin-token hardening tracked as a follow-on).
        impl_->admin.admin_token = std::string(adm);

        // known_roles allowlist (HEP-CORE-0035 §4.8) rides the SAME
        // encrypted vault — extracted here from the already-decrypted
        // payload (no second open, no second password prompt).  An
        // empty object is the §4.8.4 deny-all bootstrap; a populated
        // document is parsed + validated by the SAME strict codec the
        // legacy file path used (KnownRolesStore::from_json).  This is
        // the isolation contract: the allowlist lives inside the single
        // system-level vault file, never a script-reachable sidecar.
        impl_->known_roles = extract_known_roles(vault.known_roles());

        // §4.8.7 hard cutover — refuse to run with a stale plaintext
        // allowlist present.  It is no longer read; running would admit
        // roles from the vault while an operator who believes the file is
        // authoritative sees a divergent set.  Only the run/validate path
        // reaches load_keypair; the known-role CLI ops open the vault
        // directly (not here), so `--migrate-known-roles` can still read
        // and remove the file.
        {
            const std::filesystem::path legacy = impl_->base_dir / "vault" / "known_roles.json";
            std::error_code ec;
            if (std::filesystem::exists(legacy, ec))
                throw std::runtime_error(
                    "[plh_hub] Refusing to start: a plaintext allowlist still "
                    "exists at '" +
                    legacy.string() +
                    "'.  known_roles now lives inside the encrypted vault "
                    "(HEP-CORE-0035 §4.8) and this file is no longer read.  "
                    "Import it once (it is then removed):\n    plh_hub --config "
                    "<hub.json> --migrate-known-roles\n(or, if already "
                    "migrated, delete it: rm '" +
                    legacy.string() + "').");
        }

        std::fprintf(stderr,
                     "[plh_hub] Loaded vault from '%s' (pubkey: %.8s..., "
                     "%zu known_role(s))\n",
                     vault_path.string().c_str(), pub.data(), impl_->known_roles.size());
    }
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
    const auto &uid = impl_->identity.uid;
    const std::filesystem::path vault_path =
        pylabhub::utils::security::resolve_keyfile_path(auth.keyfile, hub_dir);

    // No-silent-overwrite (HEP-CORE-0033 §7.1, added 2026-05-31): refuse
    // to clobber an existing vault file.  --keygen produces fresh CURVE
    // material AND a fresh admin token; overwriting destroys both,
    // invalidating any federation peer that pinned the old pubkey and
    // every admin-channel session bound to the old token.  Operator
    // must remove the file explicitly to re-keygen.
    if (std::filesystem::exists(vault_path))
        throw std::runtime_error(
            "[plh_hub] Error: vault already exists at '" + vault_path.string() +
            "'. Refusing to overwrite — that would destroy the existing "
            "keypair (and any ChaCha20-Poly1305 admin token / CURVE "
            "secret bound to it).  If you really want a new keypair, "
            "remove the file first:\n    rm '" +
            vault_path.string() + "'\nthen re-run --keygen (HEP-CORE-0033 §7.1).");

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
    return std::string(vault.broker_curve_public_key());
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
        .read(
            [&](const nlohmann::json &j)
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
