#pragma once
/**
 * @file hub_vault_test_seed.h
 * @brief Provision a REAL encrypted hub vault (keypair + known_roles
 *        allowlist) for in-process HubHost tests (HEP-CORE-0035 §4.8).
 *
 * The allowlist lives INSIDE the encrypted hub vault, not a plaintext
 * `known_roles.json` sidecar (the hub refuses to start if that file
 * exists — §4.8.7 hard cutover).  In-process L3 harnesses that boot a
 * `HubHost` use this helper to create the real vault (Argon2id +
 * XSalsa20-Poly1305) holding a freshly minted CURVE keypair AND the
 * allowlist — exactly what `plh_hub --keygen` produces in production.
 *
 * The harness then calls the PRODUCTION `HubConfig::load_keypair()` to
 * read the vault back: that seeds `"hub_identity"` into the process
 * `KeyStore` from the vault's keypair and extracts the allowlist into
 * the config.  So the fixture must seed ONLY the per-role identities
 * (`seed_role_identities`) — never `"hub_identity"`, which `load_keypair`
 * owns — and `broker_pubkey()` surfaces the vault's minted pubkey for
 * role clients to pin.  There is no test-only vault seam: identity and
 * allowlist round-trip through the same crypto production uses.
 */

#include "curve_test_setup.h" // CurveSetup, make_known_role

#include "utils/config/hub_config.hpp"
#include "utils/hub_vault.hpp"
#include "utils/security/key_file_acl.hpp" // resolve_keyfile_path
#include "utils/security/key_store.hpp"    // secure().keys() re-boot evict
#include "utils/security/known_roles.hpp"
#include "utils/security/secure_subsystem.hpp" // secure(), kHubIdentityName

#include <filesystem>
#include <string>

namespace pylabhub::tests
{

/// Create the hub vault at @p cfg's resolved `auth.keyfile` path — a
/// freshly minted CURVE keypair (via `HubVault::create`) plus the
/// `known_roles` allowlist built from `setup.role_keys`, encrypted and
/// persisted.  `password` defaults to empty (test/dev vault).
///
/// This ONLY writes the vault file; it does not touch the process
/// `KeyStore` or the in-memory config.  The caller reads it back with
/// the production `cfg.load_keypair(password)` — mirroring a real hub
/// boot exactly: `--keygen` writes the vault, `load_keypair` reads it.
inline void provision_hub_vault(pylabhub::config::HubConfig &cfg,
                                const CurveSetup            &setup,
                                const std::string           &password = "")
{
    namespace security = pylabhub::utils::security;

    security::KnownRolesStore store;
    for (const auto &[uid, kp] : setup.role_keys)
        store.add(make_known_role(uid, kp.public_z85));

    const std::filesystem::path vault_path =
        security::resolve_keyfile_path(cfg.auth().keyfile, cfg.base_dir());

    auto vault = pylabhub::utils::HubVault::create(
        vault_path, cfg.identity().uid, password);
    vault.set_known_roles(store.to_json());
    vault.save(vault_path, cfg.identity().uid, password);
}

/// Read the provisioned vault back through the PRODUCTION
/// `HubConfig::load_keypair` — it ACL-checks the vault, seeds
/// `"hub_identity"` into the process KeyStore from the vault's keypair,
/// and extracts the allowlist into @p cfg.  This is the same call a real
/// hub boot makes; pair it with `provision_hub_vault`.
///
/// Re-boot safety: a fixture may boot a second in-process HubHost in the
/// same subprocess (e.g. a `.reset()`/re-emplace idiom).  `load_keypair`
/// always ADDS `"hub_identity"` and the KeyStore throws on a duplicate,
/// while HubHost shutdown does not evict it (the KeyStore is
/// SMS-process-global).  Evict any stale entry first so each boot loads a
/// fresh identity from its own vault — mirroring a real hub restart
/// (new process → fresh KeyStore).  Per-role identities are the caller's
/// concern (`seed_role_identities`) and are untouched here.
inline void load_hub_keypair_fresh(pylabhub::config::HubConfig &cfg,
                                   const std::string           &password = "")
{
    namespace sec = pylabhub::utils::security;
    if (sec::secure().keys().has(sec::kHubIdentityName))
        sec::secure().keys().remove(sec::kHubIdentityName);
    cfg.load_keypair(password);
}

} // namespace pylabhub::tests
