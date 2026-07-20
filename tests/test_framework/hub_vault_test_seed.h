#pragma once
/**
 * @file hub_vault_test_seed.h
 * @brief Seed a REAL encrypted hub vault with the known_roles allowlist
 *        for in-process HubHost tests (HEP-CORE-0035 §4.8).
 *
 * The allowlist lives INSIDE the encrypted hub vault, not a plaintext
 * `known_roles.json` sidecar (the hub refuses to start if that file
 * exists — §4.8.7 hard cutover).  In-process L3 harnesses that boot a
 * `HubHost` use this helper to create the real vault (Argon2id +
 * XSalsa20-Poly1305) holding the allowlist, then load it back into the
 * `HubConfig` — so the allowlist round-trips through the real vault
 * exactly as production does, no test-only seam.
 *
 * The hub CURVE identity is a separate concern: these harnesses seed it
 * directly into the process `KeyStore` via `seed_curve_identities`, so
 * the vault's own freshly generated keypair is unused here (we call
 * `load_known_roles_from_vault`, NOT `load_keypair`, to avoid a
 * duplicate `"hub_identity"` seed).
 */

#include "curve_test_setup.h" // CurveSetup, make_known_role

#include "utils/config/hub_config.hpp"
#include "utils/hub_vault.hpp"
#include "utils/security/key_file_acl.hpp" // resolve_keyfile_path
#include "utils/security/known_roles.hpp"

#include <filesystem>
#include <string>

namespace pylabhub::tests
{

/// Create the hub vault at @p cfg's resolved `auth.keyfile` path, store
/// the `known_roles` built from `setup.role_keys` inside it (encrypted),
/// then decrypt it back into `cfg` (populating `cfg.known_roles()`).
/// `password` defaults to empty (test/dev vault).  Mirrors production:
/// the allowlist is really encrypted and really decrypted through the
/// vault crypto — there is no plaintext file and no in-memory shortcut.
inline void seed_vault_known_roles(pylabhub::config::HubConfig &cfg,
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

    cfg.load_known_roles_from_vault(password);
}

} // namespace pylabhub::tests
