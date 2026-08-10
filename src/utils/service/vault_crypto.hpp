/**
 * @file vault_crypto.hpp
 * @brief Internal shared Argon2id + XSalsa20-Poly1305 vault crypto helpers.
 *
 * Used by HubVault and RoleVault. NOT a public header — do not include from
 * outside src/utils/service/.
 *
 * Vault binary format (written by vault_write, read by vault_read_secure):
 *   [nonce (24 bytes)] [MAC (16 bytes) || ciphertext]
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(uid),
 *                           kVaultOpsLimit, kVaultMemLimit)
 *
 * The derived key is held in the process KeyStore under a caller-chosen
 * name and never appears in this file: `vault_add_key_from_password`
 * puts it there, and `vault_write` / `vault_read_secure` cite it by
 * name.  Naming it is what lets the caller reuse it — a save after an
 * open needs no password and no second derivation.
 *
 * KDF parameters are selected at compile time:
 *   Default (INTERACTIVE):                                       64 MB RAM, ~100 ms/hash
 *   High security (SENSITIVE, -DPYLABHUB_VAULT_HIGH_SECURITY):    1 GB RAM, ~5 s/hash
 *   Test mode (MIN,     -DPYLABHUB_VAULT_TEST_KDF):              ~8 KB RAM, ~1 ms/hash
 *
 * PYLABHUB_VAULT_TEST_KDF is set by tests/CMakeLists.txt ONLY when TWO
 * deliberate conditions hold together: the build is configured with
 * BUILD_TESTS=ON, AND `CI` / `GITHUB_ACTIONS` is actually set in the
 * environment.  Neither happens by accident — an ordinary local build,
 * debug or release, does not set `CI`, so it cannot reach this branch.
 * Choosing it means choosing a CI environment, which is by definition a
 * test environment; that pairing IS the guard.  The release path is
 * narrower still: the wheel build sets BUILD_TESTS=OFF outright
 * (pyproject.toml).
 *
 * The reason it exists is a resource limit, not a security tradeoff:
 * Argon2id INTERACTIVE wants 64 MiB per derivation and stretches to 60+
 * seconds on a memory-pressured CI runner.  MIN restores predictable
 * sub-millisecond keygen for the test vaults, which never leave the
 * test directory.  CI's job is to exercise the library, so the flag
 * lands on the library — that is the intent, not a leak.
 *
 * WARNING: Vaults encrypted with one KDF parameter set cannot be opened
 * with a different one.  Do NOT use test-mode-built binaries against a
 * production vault or vice versa.
 */
#pragma once

// HEP-CORE-0043 §1.2 mechanism 4: this header does NOT include
// `<sodium.h>` directly.  Sodium constants
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
#include <string_view>

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

// ── File format — HEP-CORE-0035 §4.6.6 ───────────────────────────────────────
//
// A vault file is:
//
//   [ header 12 ][ nonce 24 ][ ciphertext ][ tag 16 ]
//
// The header is cleartext because a reader must act on it before it has
// a key, and it is passed to the AEAD as ASSOCIATED DATA so that editing
// it is still detected (VF-3).  The tag follows the ciphertext because
// that is where the AEAD's combined mode writes it.
//
// The plaintext inside is:
//
//   [ secret section, fixed length by kind ][ metadata JSON ]
//
// The secret sits at offset 0 so that no variable-length field can move
// it, and there is no stored length: the secret's size is fixed by
// `vault_kind`, so the metadata is simply the remainder (VF-4).

constexpr std::size_t kVaultHeaderBytes = 12U;

/// Byte offsets within the header.  Every field is a single byte or a
/// byte string, so there is no endianness to get wrong.
constexpr std::size_t kVaultHdrMagicOffset = 0U;  ///< 8 bytes
constexpr std::size_t kVaultHdrMagicBytes = 8U;
constexpr std::size_t kVaultHdrVersionOffset = 8U;
constexpr std::size_t kVaultHdrKdfProfileOffset = 9U;
constexpr std::size_t kVaultHdrKindOffset = 10U;
constexpr std::size_t kVaultHdrReservedOffset = 11U;

constexpr char kVaultMagic[kVaultHdrMagicBytes] = {'P', 'L', 'H', 'V', 'A', 'U', 'L', 'T'};

/// Bumped only when the layout changes.  A reader refuses anything it
/// does not recognise rather than guessing (VF-1).
constexpr std::uint8_t kVaultFormatVersion = 1U;

/// Which secrets a file carries, and therefore how long its secret
/// section is.  Also stops a hub vault being parsed as a role vault.
enum class VaultKind : std::uint8_t
{
    Hub = 1,
    Role = 2,
};

/// Argon2id cost, recorded in the file and honoured on read (VF-2).
/// A reader NEVER substitutes its own build-time choice, which is what
/// makes a vault portable between builds.
enum class VaultKdfProfile : std::uint8_t
{
    Interactive = 1, ///< 2 ops, 64 MiB  — the default
    Sensitive = 2,   ///< 4 ops, 1 GiB   — high security
    Minimal = 3,     ///< 1 op,  8 KiB   — throwaway test vaults ONLY
};

struct VaultKdfCost
{
    unsigned long long opslimit;
    std::size_t memlimit;
};

/// Maps the recorded profile byte to its cost.  Returns false for an
/// unrecognised value — refused, not defaulted (VF-2).
[[nodiscard]] constexpr bool vault_kdf_cost(VaultKdfProfile profile, VaultKdfCost &out) noexcept
{
    switch (profile)
    {
    case VaultKdfProfile::Interactive:
        out = {2ULL, 67108864U};
        return true;
    case VaultKdfProfile::Sensitive:
        out = {4ULL, 1073741824U};
        return true;
    case VaultKdfProfile::Minimal:
        out = {1ULL, 8192U};
        return true;
    }
    return false;
}

constexpr std::size_t kVaultKeyBytes = 32U;   // crypto_secretbox_KEYBYTES
constexpr std::size_t kVaultNonceBytes = 24U; // crypto_secretbox_NONCEBYTES
constexpr std::size_t kVaultMacBytes = 16U;   // crypto_secretbox_MACBYTES
constexpr std::size_t kVaultSaltBytes = 16U;  // crypto_pwhash_SALTBYTES

/// Secret-section length per kind (VF-8).  A role carries its CurveZMQ
/// secret key; a hub carries the broker secret key followed by the
/// admin token.  Both are RAW bytes — the form the KeyStore holds
/// (HEP-CORE-0040 §8.5.2) — so the secret crosses the file boundary
/// without an encoding step that would materialise it as text.
constexpr std::size_t kVaultRoleSecretBytes = 32U;      ///< seckey
constexpr std::size_t kVaultHubSecretBytes = 32U + 32U; ///< seckey ‖ admin token

/// Length of the secret section for a kind, or 0 for an unknown kind.
[[nodiscard]] constexpr std::size_t vault_secret_section_bytes(VaultKind kind) noexcept
{
    switch (kind)
    {
    case VaultKind::Role:
        return kVaultRoleSecretBytes;
    case VaultKind::Hub:
        return kVaultHubSecretBytes;
    }
    return 0U;
}

// ── Function declarations ─────────────────────────────────────────────────────

/// Derive the vault's 256-bit key from `password` and file it in the
/// process KeyStore under `key_name`.  `uid` is the domain separator
/// (salt = BLAKE2b-16(uid)), so the same password on two vaults with
/// different uids gives different keys.
///
/// The key is derived straight into locked memory and never leaves the
/// security module — which is why the write / read functions below take
/// a NAME and not a key.  Before 2026-08-09 this file derived into a
/// plain stack array and wiped it by hand at two sites; the bytes were
/// pageable for the whole operation.
///
/// Replaces any existing entry under `key_name` — re-opening a vault
/// re-derives the same key, and a remove-then-add would leave a window
/// where the name resolves to nothing.
///
/// Throws `std::runtime_error` if derivation fails (which, for these
/// inputs, means Argon2id could not get `kVaultMemLimit` bytes).
void vault_add_key_from_password(std::string_view key_name, const std::string &password,
                                 const std::string &uid);

/// Encrypt json_payload under the KeyStore key `key_name` and write to
/// path as [nonce(24)][MAC(16)||ciphertext].  The nonce is generated
/// inside the security module, so no caller can reuse one.
/// File permissions are set to 0600 (owner read/write only).
/// Throws std::runtime_error on crypto or I/O failure, and
/// `std::out_of_range` if `key_name` is not in the KeyStore.
void vault_write(const std::filesystem::path &path, const std::string &json_payload,
                 std::string_view key_name);

/// Decrypt the vault at `path` and write the plaintext JSON bytes
/// directly into `out_buf`.  Returns the number of bytes written.
///
/// `out_buf` MUST be large enough to hold the plaintext; throws
/// `std::runtime_error` if the plaintext does not fit (the caller's
/// span is zeroed before the throw to avoid leaving a partial leak).
/// Also throws `std::runtime_error` on MAC failure, I/O error, or a
/// file too short to hold a nonce and a MAC.
///
/// No `std::string` materializes on this path — the plaintext never
/// lives in a heap-allocated container whose destructor cannot be
/// trusted to zero (HEP-CORE-0040 §175).  Callers typically pair this
/// with `pylabhub::utils::security::SecureBuffer<N>` whose destructor
/// `sodium_memzero`'s the bytes:
///
///     vault_add_key_from_password(kName, pw, uid);
///     SecureBuffer<4096> json_buf;
///     auto n = vault_read_secure(path, kName, json_buf.span());
///     // parse JSON from json_buf.span().first(n) ...
///     // json_buf dtor zeros the plaintext when this scope exits.
///
/// A wrong password is not distinguishable from a corrupted file, and
/// deliberately so: the Poly1305 tag verifying IS the password check.
/// Throws `std::out_of_range` if `key_name` is not in the KeyStore.
std::size_t vault_read_secure(const std::filesystem::path &path, std::string_view key_name,
                              std::span<std::byte> out_buf);

} // namespace pylabhub::utils::detail
