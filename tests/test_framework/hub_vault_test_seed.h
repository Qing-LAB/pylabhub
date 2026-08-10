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
/// This ONLY writes the vault file; it leaves the process `KeyStore`
/// and the in-memory config as it found them.  The caller reads it back
/// with the production `cfg.load_keypair(password)` — mirroring a real
/// hub boot exactly: `--keygen` writes the vault, `load_keypair` reads
/// it.
///
/// **Keeping that true now takes explicit work.**  `HubVault::create`
/// mints the broker keypair and the admin token INTO the key store —
/// it has to, because under HEP-CORE-0035 §4.6.6 the secrets go to disk
/// straight from locked memory and never exist as values it could hold.
/// In production that is invisible: `--keygen` is its own process and
/// exits.  Here it is the same process, so the entries would still be
/// sitting there when `load_keypair` ran, and both `add_identity` and
/// `add_raw` refuse to replace — correctly, since silently overwriting
/// an identity is a security event.
///
/// So this helper evicts what `create` deposited, which is precisely
/// what makes it behave like the separate process it is standing in
/// for.  `load_keypair` remains the sole seeder of `"hub_identity"`,
/// exactly as the paragraph above promises.
inline void provision_hub_vault(pylabhub::config::HubConfig &cfg, const CurveSetup &setup,
                                const std::string &password = "")
{
    namespace security = pylabhub::utils::security;

    security::KnownRolesStore store;
    for (const auto &[uid, kp] : setup.role_keys)
        store.add(make_known_role(uid, kp.public_z85));

    const std::filesystem::path vault_path =
        security::resolve_keyfile_path(cfg.auth().keyfile, cfg.base_dir());

    {
        auto vault = pylabhub::utils::HubVault::create(vault_path, cfg.identity().uid, password);
        vault.set_known_roles(store.to_json());
        vault.save(vault_path);
        // `save` reads both secrets back out of the key store, so the
        // eviction below must happen after it, not before.
    }
    for (const auto name : {security::kHubIdentityName, security::kHubAdminTokenName})
    {
        if (security::secure().keys().has(name))
            security::secure().keys().remove(name);
    }
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
                                   const std::string &password = "")
{
    namespace sec = pylabhub::utils::security;
    // BOTH hub secrets have to be evicted, not just the identity.
    //
    // The vault's secret section is broker seckey ‖ admin token
    // (HEP-CORE-0035 §4.6.6), so opening it deposits two named keys, and
    // both `add_identity` and `add_raw` throw on a duplicate — by design,
    // since silently replacing either is a security event.  This helper
    // evicted only `hub_identity`, which was complete when the token was
    // still a plain string on the config; it is not any more.  A fixture
    // that provisions a vault and then loads it in the same subprocess
    // hits the token first.
    for (const auto name : {sec::kHubIdentityName, sec::kHubAdminTokenName})
    {
        if (sec::secure().keys().has(name))
            sec::secure().keys().remove(name);
    }
    cfg.load_keypair(password);
}

} // namespace pylabhub::tests
