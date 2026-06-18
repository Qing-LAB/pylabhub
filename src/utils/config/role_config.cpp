/**
 * @file role_config.cpp
 * @brief RoleConfig implementation — factory, accessors, JsonConfig backend.
 */
#include "utils/config/role_config.hpp"
#include "utils/json_config.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_vault.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_buffer.hpp"

#include <any>
#include <cassert>
#include <unordered_set>
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
    ChecksumConfig   checksum;
    LoggingConfig    logging;
    InboxConfig      inbox;
    StartupConfig    startup;
    MonitoringConfig monitoring;

    // ── Directional categories (two slots each) ──────────────────────
    HubRefConfig                    in_hub;
    HubRefConfig                    out_hub;
    TransportConfig              in_transport;
    TransportConfig              out_transport;
    ShmConfig                    in_shm;
    ShmConfig                    out_shm;
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

/// Allowed top-level JSON keys. Unknown keys cause a hard error.
/// Nested objects (producer.uid, script.type, etc.) are validated by their parsers.
static const std::unordered_set<std::string> kAllowedKeys = {
    // Identity (nested object — role_tag is the key: "producer", "consumer", "processor")
    "producer", "consumer", "processor",
    // Script
    "script", "stop_on_script_error", "python_venv",
    // Timing
    "loop_timing", "target_period_ms", "target_rate_hz",
    "queue_io_wait_timeout_ratio", "heartbeat_interval_ms",
    // Checksum
    "checksum", "flexzone_checksum",
    // Logging
    "logging",
    // Inbox
    "inbox_schema", "inbox_endpoint", "inbox_buffer_depth",
    "inbox_overflow_policy",
    // Startup
    "startup",
    // Monitoring
    "ctrl_queue_max_depth", "peer_dead_timeout_ms",
    // Per-direction: hub
    "in_hub_dir", "out_hub_dir",
    // Per-direction: channel
    "in_channel", "out_channel",
    // Per-direction: transport
    "in_transport", "out_transport",
    "in_zmq_endpoint", "out_zmq_endpoint",
    "in_zmq_bind", "out_zmq_bind",
    "in_zmq_buffer_depth", "out_zmq_buffer_depth",
    "in_zmq_overflow_policy", "out_zmq_overflow_policy",
    // NOTE: "in_zmq_packing" / "out_zmq_packing" removed 2026-04-20 —
    // packing is schema-level only (SchemaSpec::packing, set via the
    // schema JSON's "packing" field).  Having it also transport-level
    // allowed divergence with no conversion layer → silent corruption.
    // Per-direction: SHM
    "in_shm_enabled", "out_shm_enabled",
    // HEP-CORE-0041 §7 substep 1h (#255) — `in_shm_secret` /
    // `out_shm_secret` retired.  The legacy `shm_secret` field was a
    // header-stored guard secret on SHM channels that the design
    // determined to be not actually an auth gate (it never gated
    // ATTACH, only LOOKUP — see HEP-CORE-0041 §1 + §3).  The
    // capability-transport replacement (substeps 1a-1g) carries no
    // wire equivalent.  Configs containing this field are rejected
    // explicitly by `reject_retired_keys` below with a clear message
    // pointing operators at the migration; the runtime machinery
    // (`shm_config.hpp:41`, `hub_shm_queue.cpp:379-389`) is now dead
    // code per the rejection — substep 1i (#256) deletes the dead
    // runtime path along with `ChannelAccessEntry::shm_secret` and
    // related fields.
    "in_shm_slot_count", "out_shm_slot_count",
    "in_shm_sync_policy", "out_shm_sync_policy",
    // Role-specific (schemas — validated by role parser, not here)
    "in_slot_schema", "out_slot_schema",
    "in_flexzone_schema", "out_flexzone_schema",
};

/// HEP-CORE-0041 §7 substep 1h (#255) — explicit retirement of config
/// keys that were removed from the whitelist for a specific reason
/// (not "we forgot to add it" but "this is a deliberately retired
/// surface").  Fires BEFORE `validate_known_keys` so the operator sees
/// the HEP-referenced migration message instead of a generic
/// "unknown config key" error that would obscure the actual root
/// cause.  Each entry includes the substep / commit that retired the
/// field so a future maintainer can trace the decision.
static void reject_retired_keys(const nlohmann::json &j, const char *tag)
{
    struct RetiredKey
    {
        const char *name;
        const char *hep_ref;
        const char *migration;
    };
    static constexpr RetiredKey kRetiredKeys[] = {
        // HEP-CORE-0041 substep 1h (#255) — `*_shm_secret` retired.
        // The header-stored guard secret was determined to not be an
        // auth gate (it never gated ATTACH, only LOOKUP).  The
        // capability-transport replacement (substeps 1a-1g) carries
        // no wire equivalent.
        {"in_shm_secret",  "HEP-CORE-0041 §7 substep 1h (#255)",
         "remove the field; auth is now via SCM_RIGHTS capability "
         "transport (HEP-CORE-0041 §5.1) and requires no config knob"},
        {"out_shm_secret", "HEP-CORE-0041 §7 substep 1h (#255)",
         "remove the field; auth is now via SCM_RIGHTS capability "
         "transport (HEP-CORE-0041 §5.1) and requires no config knob"},
    };

    for (const auto &rk : kRetiredKeys)
    {
        if (j.contains(rk.name))
        {
            throw std::runtime_error(
                std::string(tag) + ": config key '" + rk.name +
                "' was RETIRED by " + rk.hep_ref +
                ".  Migration: " + rk.migration + ".");
        }
    }
}

/// Validate that all top-level JSON keys are in the whitelist.
static void validate_known_keys(const nlohmann::json &j, const char *tag)
{
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (kAllowedKeys.find(it.key()) == kAllowedKeys.end())
        {
            throw std::runtime_error(
                std::string(tag) + ": unknown config key '" + it.key() + "'");
        }
    }
}

void RoleConfig::Impl::load_common(const nlohmann::json &j)
{
    const char *tag = role_tag.c_str();

    // ── Reject retired keys with a specific HEP-referenced message ──
    // Fires BEFORE the generic unknown-key check so operators see the
    // migration hint, not "unknown config key" (HEP-CORE-0041 §7
    // substep 1h #255 + future retirements).
    reject_retired_keys(j, tag);

    // ── Reject unknown keys before parsing ──────────────────────────
    validate_known_keys(j, tag);

    // ── Non-directional categories ───────────────────────────────────
    identity   = parse_identity_config(j, role_tag);
    auth       = parse_auth_config(j, role_tag);
    script     = parse_script_config(j, base_dir, tag);
    timing     = parse_timing_config(j, tag);
    checksum   = parse_checksum_config(j, tag);
    logging    = parse_logging_config(j, tag);
    inbox      = parse_inbox_config(j, tag);
    startup    = parse_startup_config(j, tag);
    monitoring = parse_monitoring_config(j);

    // ── Directional categories (always load both slots) ──────────────
    in_hub        = parse_hub_ref_config(j, base_dir, "in");
    out_hub       = parse_hub_ref_config(j, base_dir, "out");
    in_transport  = parse_transport_config(j, "in",  tag);
    out_transport = parse_transport_config(j, "out", tag);
    in_shm        = parse_shm_config(j, "in",  tag);
    out_shm       = parse_shm_config(j, "out", tag);
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

    // HEP-CORE-0035 §4.6 Tier-1: <role>.json is non-secret but
    // references a vault.  Non-fatal WARN on suspicious mode —
    // gate on diagnostic-non-empty so the group-readable advisory
    // surface (v.ok=true, v.diagnostic set) doesn't go dead.
    if (auto v = utils::security::verify_keyfile_acl(
            std::filesystem::path(path),
            utils::security::KeyFileRole::ConfigFileReferencingVault);
        !v.diagnostic.empty())
    {
        std::fprintf(stderr,
                     "[%s] WARN: role config ACL advisory "
                     "(HEP-CORE-0035 §4.6.1):\n%s\n",
                     s.role_tag.c_str(), v.diagnostic.c_str());
    }

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
const StartupConfig    &RoleConfig::startup()    const { assert(impl_); return impl_->startup; }
const MonitoringConfig &RoleConfig::monitoring() const { assert(impl_); return impl_->monitoring; }
const ChecksumConfig   &RoleConfig::checksum()   const { assert(impl_); return impl_->checksum; }
const LoggingConfig    &RoleConfig::logging()    const { assert(impl_); return impl_->logging; }

// ============================================================================
// Directional accessors
// ============================================================================

const HubRefConfig                   &RoleConfig::in_hub()        const { assert(impl_); return impl_->in_hub; }
const HubRefConfig                   &RoleConfig::out_hub()       const { assert(impl_); return impl_->out_hub; }
const TransportConfig             &RoleConfig::in_transport()  const { assert(impl_); return impl_->in_transport; }
const TransportConfig             &RoleConfig::out_transport() const { assert(impl_); return impl_->out_transport; }
const ShmConfig                   &RoleConfig::in_shm()        const { assert(impl_); return impl_->in_shm; }
const ShmConfig                   &RoleConfig::out_shm()       const { assert(impl_); return impl_->out_shm; }
const std::string                 &RoleConfig::in_channel()    const { assert(impl_); return impl_->in_channel; }
const std::string                 &RoleConfig::out_channel()   const { assert(impl_); return impl_->out_channel; }

// ============================================================================
// Vault operations
// ============================================================================

// `role.auth.keyfile` is the source of truth for the vault file
// location at runtime per HEP-CORE-0024 §3.4 (finalized 2026-05-31):
//   - Non-empty (relative) → resolved against role base_dir.
//   - Non-empty (absolute) → used as-is.
//   - Non-empty + file absent at resolved path → throw (operator
//     configured a vault but the file is not there; no silent
//     fallback).
//
// The empty case is unreachable here — `parse_auth_config` in
// auth_config.hpp rejects empty `auth.keyfile` at config-load
// (HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1, finalized 2026-05-31:
// pylabhub is a vault; no in-memory CURVE mode exists).
bool RoleConfig::load_keypair(const std::string &password)
{
    assert(impl_);
    auto &auth = impl_->auth;
    const char *tag = impl_->role_tag.c_str();
    // auth.keyfile guaranteed non-empty by parse_auth_config.

    const auto &uid = impl_->identity.uid;
    namespace security = pylabhub::utils::security;
    const std::filesystem::path vault_path =
        security::resolve_keyfile_path(auth.keyfile, impl_->base_dir);

    if (!std::filesystem::exists(vault_path))
    {
        throw std::runtime_error(std::string("[") + tag +
            "] Error: auth.keyfile = '" + auth.keyfile +
            "' resolves to '" + vault_path.string() +
            "' which does not exist.  Run `plh_role --role " + tag +
            " --keygen` to create the vault (HEP-CORE-0024 §3.4).");
    }

    // HEP-CORE-0035 §4.6.2: verify ACL contract BEFORE reading the
    // secret.  File must be 0600 + owner-uid; parent dir must be 0700
    // + owner-uid.  Failure produces an OpenSSH-style actionable
    // diagnostic naming the path, observed mode, required mode, and
    // the exact `chmod` command to fix.
    if (auto v = security::verify_keyfile_acl(
            vault_path, security::KeyFileRole::VaultFile);
        !v.ok)
        throw std::runtime_error(std::string("[") + tag +
            "] Refusing to load vault — ACL check failed (HEP-CORE-"
            "0035 §4.6.2):\n" + v.diagnostic);
    // Defensive skip if parent_path is empty (operator passed a
    // bare-filename absolute keyfile + empty base_dir).
    if (vault_path.has_parent_path())
    {
        if (auto v = security::verify_keyfile_acl(
                vault_path.parent_path(), security::KeyFileRole::VaultDir);
            !v.ok)
            throw std::runtime_error(std::string("[") + tag +
                "] Refusing to load vault — parent dir ACL check failed "
                "(HEP-CORE-0035 §4.6.2):\n" + v.diagnostic);
    }

    {
        const auto vault = utils::RoleVault::open(vault_path, uid, password);
        const auto pub = vault.public_key();   // string_view
        const auto sec = vault.secret_key();   // string_view

        // HEP-CORE-0040 §171: identity keypair lives in `key_store()`
        // (LockedKey storage).  `add_identity_from_z85` is the single
        // site (production + tests) where the (pub_z85 || sec_z85)
        // layout is defined.
        pylabhub::utils::security::key_store().add_identity_from_z85(
            pylabhub::utils::security::kRoleIdentityName, pub, sec);

        std::fprintf(stderr, "[%s] Loaded vault from '%s' (pubkey: %.8s...)\n",
                     tag, vault_path.string().c_str(), pub.data());
    }
    return true;
}

std::string RoleConfig::create_keypair(const std::string &password)
{
    assert(impl_);
    const auto &auth = impl_->auth;
    const char *tag = impl_->role_tag.c_str();
    // auth.keyfile guaranteed non-empty by parse_auth_config
    // (HEP-CORE-0024 §3.4, finalized 2026-05-31).

    const auto &uid = impl_->identity.uid;
    const std::filesystem::path vault_path =
        pylabhub::utils::security::resolve_keyfile_path(
            auth.keyfile, impl_->base_dir);

    // No-silent-overwrite (HEP-CORE-0024 §3.4, added 2026-05-31): refuse
    // to clobber an existing vault file.  --keygen produces a fresh
    // CURVE keypair; overwriting destroys the existing one, invalidating
    // any hub-side allowlist entry pinned to the old pubkey.  Operator
    // must remove the file explicitly to re-keygen.
    if (std::filesystem::exists(vault_path))
        throw std::runtime_error(std::string("[") + tag +
            "] Error: vault already exists at '" +
            vault_path.string() +
            "'. Refusing to overwrite — that would destroy the existing "
            "CURVE keypair (the hub-side allowlist still pins the OLD "
            "pubkey).  If you really want a new keypair, remove the "
            "file first:\n    rm '" + vault_path.string() +
            "'\nthen re-run --keygen (HEP-CORE-0024 §3.4).");

    const auto vault = utils::RoleVault::create(vault_path, uid, password);
    return std::string(vault.public_key());
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
