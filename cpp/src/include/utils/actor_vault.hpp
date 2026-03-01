/**
 * @file actor_vault.hpp
 * @brief ActorVault: encrypted keypair store for an actor instance.
 *
 * Stores the actor's CurveZMQ keypair, encrypted at rest with the same
 * Argon2id KDF + XSalsa20-Poly1305 scheme used by HubVault.
 *
 * Vault file format (binary):
 *   [nonce (24 bytes)] [MAC (16 bytes) || ciphertext]
 *
 * Decrypted payload is UTF-8 JSON:
 * @code{.json}
 * {
 *   "actor_uid":  "ACTOR-...",
 *   "public_key": "<Z85 40-char>",
 *   "secret_key": "<Z85 40-char>"
 * }
 * @endcode
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(actor_uid),
 *                           kVaultOpsLimit, kVaultMemLimit)
 *
 * The actor_uid is used as a per-vault domain separator so two actors using
 * the same password produce independent encryption keys.
 *
 * Password sources (checked in order by caller):
 *   1. PYLABHUB_ACTOR_PASSWORD environment variable (service / CI)
 *   2. Interactive terminal prompt via getpass()
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
 * @brief Encrypted actor keypair store.
 *
 * Construct via ActorVault::create() (--keygen) or ActorVault::open() (runtime).
 * Both factory functions throw std::runtime_error on failure (wrong password,
 * corrupted file, I/O error).
 *
 * The vault holds only the actor's CurveZMQ keypair (Z85).
 * Nothing secret is ever written to actor.json.
 */
class PYLABHUB_UTILS_EXPORT ActorVault
{
public:
    /**
     * @brief Create a new vault file at vault_path.
     *
     * - Generates a CurveZMQ keypair with zmq_curve_keypair().
     * - Derives a 256-bit key via Argon2id (salt = BLAKE2b-16(actor_uid)).
     * - Encrypts the JSON payload with XSalsa20-Poly1305.
     * - Writes the vault file with mode 0600.
     *
     * @param vault_path  Destination file path (e.g. actor_dir / "actor.key").
     * @param actor_uid   Actor UID string — determines the KDF salt.
     * @param password    Master password. Empty string = no encryption (dev mode).
     * @throws std::runtime_error on crypto or I/O failure.
     */
    static ActorVault create(const std::filesystem::path &vault_path,
                             const std::string           &actor_uid,
                             const std::string           &password);

    /**
     * @brief Open an existing vault file at vault_path.
     *
     * Derives the key from actor_uid + password and decrypts.
     * The Poly1305 MAC authenticates the ciphertext — a wrong password or
     * corrupted file throws rather than returning garbage.
     *
     * @throws std::runtime_error on MAC failure, I/O error, or malformed JSON.
     */
    static ActorVault open(const std::filesystem::path &vault_path,
                           const std::string           &actor_uid,
                           const std::string           &password);

    /// CurveZMQ public key (Z85, 40 chars). Safe to distribute.
    const std::string &public_key() const noexcept;

    /// CurveZMQ secret key (Z85, 40 chars). Never write this to disk in plaintext.
    const std::string &secret_key() const noexcept;

    /// Actor UID stored in the vault payload (matches the actor_uid used at create time).
    const std::string &actor_uid() const noexcept;

    ~ActorVault();
    ActorVault(ActorVault &&) noexcept;
    ActorVault &operator=(ActorVault &&) noexcept;
    ActorVault(const ActorVault &)            = delete;
    ActorVault &operator=(const ActorVault &) = delete;

private:
    ActorVault();
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
