/**
 * @file hub_vault.hpp
 * @brief HubVault: encrypted secrets store for a hub instance.
 *
 * Stores the broker CurveZMQ keypair and admin token, encrypted at rest
 * with Argon2id KDF + XSalsa20-Poly1305 (libsodium secretbox).
 *
 * Vault file format (binary):
 *   [nonce (24 bytes)] [MAC (16 bytes) || ciphertext]
 *
 * Decrypted payload is UTF-8 JSON:
 * @code{.json}
 * {
 *   "broker": { "curve_secret_key": "<Z85 40-char>",
 *               "curve_public_key":  "<Z85 40-char>" },
 *   "admin":  { "token": "<64-char hex>" }
 * }
 * @endcode
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-128(hub_uid),
 *                           OPSLIMIT_INTERACTIVE, MEMLIMIT_INTERACTIVE).
 *
 * No dependency on JSON or ZMQ at the header level.
 * Safe to include without lifecycle startup.
 */
#pragma once

#include "pylabhub_utils_export.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::utils
{

/**
 * @brief Encrypted hub secrets store.
 *
 * Construct via HubVault::create() or HubVault::open().
 * Both factory functions throw std::runtime_error on failure.
 *
 * The vault holds the broker CurveZMQ keypair (Z85) and the admin
 * authentication token. Nothing secret is written to hub.json or hub.pubkey.
 */
class PYLABHUB_UTILS_EXPORT HubVault
{
public:
    /**
     * @brief Create a new vault file at the operator-supplied path.
     *
     * Per HEP-CORE-0033 §7.1 (clarified 2026-05-30), the vault file
     * location is determined by `hub.auth.keyfile`; callers resolve
     * the path via `pylabhub::utils::security::resolve_keyfile_path()`
     * and pass it here.  The canonical default is `<hub_dir>/vault/
     * hub.vault` (what `--init` writes into the template) but
     * operators may point the vault elsewhere for the
     * system-managed-config + user-owned-vault deployment model.
     *
     * - Generates a broker CurveZMQ keypair with zmq_curve_keypair().
     * - Generates a 64-char hex admin token from 32 random bytes.
     * - Derives a 256-bit key via Argon2id (salt = BLAKE2b-128(hub_uid)).
     * - Encrypts the JSON payload with XSalsa20-Poly1305.
     * - Creates the parent directory if it does not exist.
     * - Writes the vault file with mode 0600.
     *
     * @param vault_path  Filesystem path where the vault file is created.
     * @param hub_uid     Hub UUID4 string — determines the KDF salt.
     * @param password    Master password. Empty string is allowed (dev mode).
     * @throws std::runtime_error on crypto or I/O failure.
     */
    static HubVault create(const std::filesystem::path &vault_path,
                           const std::string &hub_uid,
                           const std::string &password);

    /**
     * @brief Open an existing vault file at the operator-supplied path.
     *
     * Path-resolution rules: see `create()`.
     *
     * Derives the key from hub_uid + password and decrypts.
     * The Poly1305 MAC authenticates the ciphertext — a wrong password or
     * corrupted file throws rather than returning garbage.
     *
     * @throws std::runtime_error on MAC failure, I/O error, or malformed JSON.
     */
    static HubVault open(const std::filesystem::path &vault_path,
                         const std::string &hub_uid,
                         const std::string &password);

    /// Broker CurveZMQ secret key (Z85, 40 chars).  View points into
    /// the vault's internal zero-on-destruct storage (HEP-CORE-0040
    /// §175); valid until this HubVault is destroyed.  Callers MUST
    /// NOT retain the view past the vault's lifetime.  Pre-HEP-0040
    /// callers that copied to `std::string` continue to work via
    /// implicit construction.  Never write this to disk in plaintext.
    std::string_view broker_curve_secret_key() const noexcept;

    /// Broker CurveZMQ public key (Z85, 40 chars). Safe to distribute —
    /// see publish_public_key().  Same view-lifetime contract as
    /// `broker_curve_secret_key()`.
    std::string_view broker_curve_public_key() const noexcept;

    /// Admin authentication token (64-char hex string).  Same
    /// view-lifetime contract as `broker_curve_secret_key()`.
    std::string_view admin_token() const noexcept;

    /**
     * @brief Write the broker public key to <hub_dir>/hub.pubkey.
     *
     * Writes exactly 40 characters (no newline). Permissions: 0644.
     * Roles read this file to configure CurveZMQ server-key before connecting.
     */
    void publish_public_key(const std::filesystem::path &hub_dir) const;

    ~HubVault();
    HubVault(HubVault &&) noexcept;
    HubVault &operator=(HubVault &&) noexcept;
    HubVault(const HubVault &) = delete;
    HubVault &operator=(const HubVault &) = delete;

private:
    HubVault();
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
