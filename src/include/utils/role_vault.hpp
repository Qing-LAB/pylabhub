/**
 * @file role_vault.hpp
 * @brief RoleVault: encrypted CurveZMQ keypair store for a role instance.
 *
 * Used by all three role types (producer, consumer, processor) to store their
 * CurveZMQ keypair encrypted at rest, in the same file format as HubVault
 * (HEP-CORE-0035 §4.6.6, which is authoritative).
 *
 * The secret key is NOT a field in a document.  It is raw bytes at a
 * fixed offset inside the ciphertext, and it travels locked-memory to
 * locked-memory in both directions — so it never becomes a `std::string`
 * that gets freed without being wiped.  Only the metadata is JSON:
 *
 * @code{.json}
 * {
 *   "role_uid":   "prod.sensor1.uid3a7f2b1c",
 *   "public_key": "<Z85 40-char>"
 * }
 * @endcode
 *
 * The "role_uid" key stores the role UID (PROD-/CONS-/PROC- prefix) and
 * is used as the per-vault KDF domain separator so two roles using the same
 * password produce independent encryption keys.
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(role_uid)) at the
 * cost profile RECORDED IN THE FILE, so a vault written by one build
 * opens under any other.
 *
 * Password sources (checked in order by caller):
 *   1. PYLABHUB_ROLE_PASSWORD environment variable (service / CI)
 *   2. Interactive terminal prompt via getpass()
 *      XPLAT: getpass() is POSIX-only. Windows interactive mode needs
 *      ReadConsoleW() or equivalent. CI uses PYLABHUB_ROLE_PASSWORD env var
 *      (cross-platform).
 *
 * No password is stored in any file or config field.
 */
#pragma once

#include "pylabhub_utils_export.h"

#include "utils/security/key_store.hpp" // kRoleIdentityName

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::utils
{

/**
 * @brief Encrypted role keypair store.
 *
 * Construct via RoleVault::create() (--keygen) or RoleVault::open() (runtime).
 * Both factory functions throw std::runtime_error on failure (wrong password,
 * corrupted file, I/O error).
 *
 * The vault holds only the role's CurveZMQ keypair (Z85).
 * Nothing secret is ever written to role config files.
 */
/// Default KeyStore name for the key that decrypts the role vault.
/// The name is a parameter rather than an internal detail so a caller
/// can cite the key afterwards — a later save needs no password, and a
/// script holding two vaults open can tell them apart.
inline constexpr std::string_view kRoleVaultKeyName = "role.vault.key";

class PYLABHUB_UTILS_EXPORT RoleVault
{
  public:
    /**
     * @brief Create a new vault file at vault_path.
     *
     * - Generates a CurveZMQ keypair with zmq_curve_keypair().
     * - Derives a 256-bit key via Argon2id (salt = BLAKE2b-16(role_uid)).
     * - Encrypts the JSON payload with XSalsa20-Poly1305.
     * - Writes the vault file with mode 0600.
     *
     * @param vault_path  Destination file path (e.g. role_dir / "role.key").
     * @param role_uid    Role UID string — determines the KDF salt.
     * @param password    Master password.  An empty password is accepted
     *                    and still encrypts — it simply derives the key
     *                    from an empty string, which is weak.  There is
     *                    no unencrypted mode.
     * @param key_name    KeyStore name the derived key is filed under, so
     *                    the caller can cite it later.  Defaults to
     *                    `kRoleVaultKeyName`; pass your own to hold more
     *                    than one vault open at a time.
     * @throws std::runtime_error on crypto or I/O failure.
     */
    static RoleVault create(const std::filesystem::path &vault_path, const std::string &role_uid,
                            const std::string &password,
                            std::string_view identity_name = security::kRoleIdentityName,
                            std::string_view key_name = kRoleVaultKeyName);

    /**
     * @brief Open an existing vault file at vault_path.
     *
     * Derives the key from role_uid + password and decrypts.
     * The Poly1305 MAC authenticates the ciphertext — a wrong password or
     * corrupted file throws rather than returning garbage.
     *
     * On success the derived key stays in the process KeyStore under
     * `key_name`, so later work needs neither the password nor a second
     * ~100 ms derivation.
     *
     * @throws std::runtime_error on MAC failure, I/O error, or malformed JSON.
     */
    static RoleVault open(const std::filesystem::path &vault_path, const std::string &role_uid,
                          const std::string &password,
                          std::string_view identity_name = security::kRoleIdentityName,
                          std::string_view key_name = kRoleVaultKeyName);

    /// CurveZMQ public key (Z85, 40 chars).  View points into the
    /// vault's internal zero-on-destruct storage (HEP-CORE-0040 §175);
    /// valid until this RoleVault is destroyed.  Safe to distribute.
    std::string_view public_key() const noexcept;

    /// Role UID stored in the vault payload (matches the role_uid used
    /// at create time).  Same view-lifetime contract.
    std::string_view role_uid() const noexcept;

    ~RoleVault();
    RoleVault(RoleVault &&) noexcept;
    RoleVault &operator=(RoleVault &&) noexcept;
    RoleVault(const RoleVault &) = delete;
    RoleVault &operator=(const RoleVault &) = delete;

  private:
    RoleVault();
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
