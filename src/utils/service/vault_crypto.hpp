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

// Post-SEC-Fold-2 Phase 2 (HEP-CORE-0043 §1.2 mechanism 4): this
// header does NOT include `<sodium.h>` directly.  Sodium constants
// are hardcoded here — their values are stable ABI (libsodium has
// preserved them for a decade); if libsodium ever changes them, the
// static_asserts inside `secure_subsystem.cpp` catch the drift at
// build time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace pylabhub::utils::detail
{

// ── KDF parameters ────────────────────────────────────────────────────────────
// Selected at compile time.  Both create and open sites must be
// compiled with the same setting; a vault written at one level cannot
// be opened at another.
//
// Values hardcoded from libsodium 1.0.18+ constants:
//   INTERACTIVE: opslimit=2, memlimit=64 MiB     (default; ~100 ms/hash)
//   SENSITIVE:   opslimit=4, memlimit=1024 MiB   (high-sec; ~5 s/hash)
//   MIN:         opslimit=1, memlimit=8192       (test; ~1 ms/hash)
#if defined(PYLABHUB_VAULT_TEST_KDF)
constexpr unsigned long long kVaultOpsLimit = 1ULL;
constexpr std::size_t kVaultMemLimit = 8192U;
#elif defined(PYLABHUB_VAULT_HIGH_SECURITY)
constexpr unsigned long long kVaultOpsLimit = 4ULL;
constexpr std::size_t kVaultMemLimit = 1073741824U; // 1 GiB
#else
constexpr unsigned long long kVaultOpsLimit = 2ULL;
constexpr std::size_t kVaultMemLimit = 67108864U; // 64 MiB
#endif

constexpr std::size_t kVaultKeyBytes = 32U;   // crypto_secretbox_KEYBYTES
constexpr std::size_t kVaultNonceBytes = 24U; // crypto_secretbox_NONCEBYTES
constexpr std::size_t kVaultMacBytes = 16U;   // crypto_secretbox_MACBYTES
constexpr std::size_t kVaultSaltBytes = 16U;  // crypto_pwhash_SALTBYTES

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
void vault_write(const std::filesystem::path &path, const std::string &json_payload,
                 const std::string &password, const std::string &uid);

/// Decrypt the vault at `path` and write the plaintext JSON bytes
/// directly into `out_buf`.  Returns the number of bytes written.
///
/// `out_buf` MUST be large enough to hold the plaintext; throws
/// `std::runtime_error` if the plaintext does not fit (the caller's
/// span is zeroed before the throw to avoid leaving a partial leak).
/// Same throws as `vault_read` for MAC failure / I/O error / minimum-
/// size violation.
///
/// Unlike `vault_read`, no `std::string` materializes — the plaintext
/// never lives in a heap-allocated container whose destructor cannot
/// be trusted to zero (HEP-CORE-0040 §175).  Callers typically pair
/// this with `pylabhub::utils::security::SecureBuffer<N>` whose
/// destructor `sodium_memzero`'s the bytes:
///
///     SecureBuffer<4096> json_buf;
///     auto n = vault_read_secure(path, pw, uid, json_buf.span());
///     // parse JSON from json_buf.span().first(n) ...
///     // json_buf dtor zeros the plaintext when this scope exits.
std::size_t vault_read_secure(const std::filesystem::path &path, const std::string &password,
                              const std::string &uid, std::span<std::byte> out_buf);

} // namespace pylabhub::utils::detail
