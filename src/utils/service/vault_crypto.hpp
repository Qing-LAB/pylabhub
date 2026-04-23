/**
 * @file vault_crypto.hpp
 * @brief Internal shared Argon2id + XSalsa20-Poly1305 vault crypto helpers.
 *
 * Used by HubVault and RoleVault. NOT a public header — do not include from
 * outside src/utils/service/.
 *
 * Vault binary format (written/read by vault_write / vault_read):
 *   [nonce (24 bytes)] [MAC (16 bytes) || ciphertext]
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(uid),
 *                           kVaultOpsLimit, kVaultMemLimit)
 *
 * KDF parameters are selected at compile time:
 *   Default (INTERACTIVE):                                       64 MB RAM, ~100 ms/hash
 *   High security (SENSITIVE, -DPYLABHUB_VAULT_HIGH_SECURITY):    1 GB RAM, ~5 s/hash
 *   Test mode (MIN,     -DPYLABHUB_VAULT_TEST_KDF):              ~8 KB RAM, ~1 ms/hash
 *
 * PYLABHUB_VAULT_TEST_KDF is set by tests/CMakeLists.txt ONLY when the
 * build is configured with BUILD_TESTS=ON AND a CI environment is
 * detected.  Production builds (no BUILD_TESTS) cannot reach this
 * branch.  The guard is there because CI runners experience Argon2id
 * INTERACTIVE stretching to 60+ seconds under memory pressure; MIN
 * restores predictable sub-millisecond keygen for CI runs without
 * compromising the production security posture.
 *
 * WARNING: Vaults encrypted with one KDF parameter set cannot be opened
 * with a different one.  Do NOT use test-mode-built binaries against a
 * production vault or vice versa.
 */
#pragma once

#include <sodium.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pylabhub::utils::detail
{

// ── KDF parameters ────────────────────────────────────────────────────────────
// Selected at compile time via -DPYLABHUB_VAULT_HIGH_SECURITY or
// -DPYLABHUB_VAULT_TEST_KDF.  Both create and open sites must be
// compiled with the same setting; a vault written at one level cannot
// be opened at another.
#if defined(PYLABHUB_VAULT_TEST_KDF)
// Test-only fast path.  MIN params yield ~1 ms keygen with trivial
// cracking resistance — acceptable because this define is only set
// for BUILD_TESTS=ON + CI configurations where the vault contents
// never escape the test directory.  See vault_crypto.hpp header doc.
constexpr unsigned long long kVaultOpsLimit = crypto_pwhash_OPSLIMIT_MIN;
constexpr std::size_t        kVaultMemLimit = crypto_pwhash_MEMLIMIT_MIN;
#elif defined(PYLABHUB_VAULT_HIGH_SECURITY)
constexpr unsigned long long kVaultOpsLimit = crypto_pwhash_OPSLIMIT_SENSITIVE;
constexpr std::size_t        kVaultMemLimit = crypto_pwhash_MEMLIMIT_SENSITIVE;
#else
constexpr unsigned long long kVaultOpsLimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
constexpr std::size_t        kVaultMemLimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;
#endif

constexpr std::size_t kVaultKeyBytes   = crypto_secretbox_KEYBYTES;   // 32
constexpr std::size_t kVaultNonceBytes = crypto_secretbox_NONCEBYTES; // 24
constexpr std::size_t kVaultMacBytes   = crypto_secretbox_MACBYTES;   // 16
constexpr std::size_t kVaultSaltBytes  = crypto_pwhash_SALTBYTES;     // 16

// ── Function declarations ─────────────────────────────────────────────────────

/// Ensure libsodium is initialised (idempotent, thread-safe).
void vault_require_sodium();

/// Derive a 256-bit encryption key from password and uid (domain separator).
/// Salt = BLAKE2b-16(uid). Deterministic: same (password, uid) → same key.
/// The uid acts as a per-vault domain separator so different vaults using the
/// same password produce different encryption keys.
std::array<uint8_t, kVaultKeyBytes> vault_derive_key(const std::string &password,
                                                      const std::string &uid);

/// Encrypt json_payload and write to path as [nonce(24)][MAC(16)||ciphertext].
/// File permissions are set to 0600 (owner read/write only).
/// Throws std::runtime_error on crypto or I/O failure.
void vault_write(const std::filesystem::path &path,
                 const std::string           &json_payload,
                 const std::string           &password,
                 const std::string           &uid);

/// Read and decrypt vault at path. Returns the plaintext JSON string.
/// Throws std::runtime_error on MAC failure (wrong password or corrupted file),
/// I/O error, or minimum-size violation.
std::string vault_read(const std::filesystem::path &path,
                       const std::string           &password,
                       const std::string           &uid);

} // namespace pylabhub::utils::detail
