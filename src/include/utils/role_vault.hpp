/**
 * @file role_vault.hpp
 * @brief RoleVault: encrypted CurveZMQ keypair store for a role instance.
 *
 * Used by all three role types (producer, consumer, processor) to store their
 * CurveZMQ keypair encrypted at rest, using the same Argon2id KDF +
 * XSalsa20-Poly1305 scheme as HubVault.
 *
 * Vault file format (binary):
 *   [nonce (24 bytes)] [MAC (16 bytes) || ciphertext]
 *
 * Decrypted payload is UTF-8 JSON:
 * @code{.json}
 * {
 *   "role_uid":   "PROD-SENSOR1-3A7F2B1C",
 *   "public_key": "<Z85 40-char>",
 *   "secret_key": "<Z85 40-char>"
 * }
 * @endcode
 *
 * The "role_uid" key stores the role UID (PROD-/CONS-/PROC- prefix) and
 * is used as the per-vault KDF domain separator so two roles using the same
 * password produce independent encryption keys.
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(role_uid),
 *                           kVaultOpsLimit, kVaultMemLimit)
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

#include <filesystem>
#include <memory>
#include <string>

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
     * @param password    Master password. Empty string = no encryption (dev mode).
     * @throws std::runtime_error on crypto or I/O failure.
     */
    static RoleVault create(const std::filesystem::path &vault_path,
                             const std::string           &role_uid,
                             const std::string           &password);

    /**
     * @brief Open an existing vault file at vault_path.
     *
     * Derives the key from role_uid + password and decrypts.
     * The Poly1305 MAC authenticates the ciphertext — a wrong password or
     * corrupted file throws rather than returning garbage.
     *
     * @throws std::runtime_error on MAC failure, I/O error, or malformed JSON.
     */
    static RoleVault open(const std::filesystem::path &vault_path,
                           const std::string           &role_uid,
                           const std::string           &password);

    /// CurveZMQ public key (Z85, 40 chars). Safe to distribute.
    const std::string &public_key() const noexcept;

    /// CurveZMQ secret key (Z85, 40 chars). Never write this to disk in plaintext.
    const std::string &secret_key() const noexcept;

    /// Role UID stored in the vault payload (matches the role_uid used at create time).
    const std::string &role_uid() const noexcept;

    ~RoleVault();
    RoleVault(RoleVault &&) noexcept;
    RoleVault &operator=(RoleVault &&) noexcept;
    RoleVault(const RoleVault &)            = delete;
    RoleVault &operator=(const RoleVault &) = delete;

private:
    RoleVault();
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
