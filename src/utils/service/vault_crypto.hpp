/**
 * @file vault_crypto.hpp
 * @brief Internal shared Argon2id + XSalsa20-Poly1305 vault crypto helpers.
 *
 * Used by HubVault and RoleVault. NOT a public header — do not include from
 * outside src/utils/service/.
 *
 * Vault binary format — specified in HEP-CORE-0035 §4.6.6, which is
 * authoritative.  Written by `vault_write`, read by `vault_read`:
 *
 *   [ header 12 ][ nonce 24 ][ ciphertext ][ tag 16 ]
 *
 * The header is cleartext because a reader must act on it before it has
 * a key, and it is passed to the AEAD as associated data so that editing
 * it is still detected (VF-3).  Inside the ciphertext the secret comes
 * first at a fixed offset, with the metadata JSON filling the remainder
 * (VF-4) — no length field, because the secret's size is fixed by the
 * vault kind and a stored length could disagree with reality.
 *
 * NOTE the tag is at the END: the AEAD's combined mode appends it,
 * unlike `crypto_secretbox_easy` which prepended its MAC.  A reader
 * built to the old shape will not open these files.
 *
 * Key derivation: Argon2id(password, salt=BLAKE2b-16(uid),
 *                           kVaultOpsLimit, kVaultMemLimit)
 *
 * The derived key is held in the process KeyStore under a caller-chosen
 * name and never appears in this file: `vault_add_key_from_password`
 * puts it there, and `vault_write` / `vault_read` cite it by
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
 * The profile above is a WRITE-time choice only.  The file records
 * which one it was written under, and a reader derives at the file's
 * profile rather than its own (HEP-CORE-0035 §4.6.6 VF-2), so any
 * binary opens any vault.  This header used to warn against pointing a
 * test-mode binary at a production vault; that hazard was real when the
 * cost was implied by the build, and does not exist now that it is
 * recorded in the file.
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
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace pylabhub::utils::detail
{

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

// ── KDF cost this build WRITES with ──────────────────────────────────────────
//
// The build chooses only what it writes.  What it reads comes from the
// file (VF-2), so there is no such thing as a binary that can open only
// its own vaults — the warning that used to sit at the top of this
// header, telling operators not to mix test-mode and production
// binaries, described a hazard the format no longer has.
//
// The ops/mem numbers are DERIVED from the profile rather than being a
// second `#if` ladder beside it.  Two ladders can disagree; one cannot.
#if defined(PYLABHUB_VAULT_TEST_KDF)
constexpr VaultKdfProfile kVaultWriteProfile = VaultKdfProfile::Minimal;
#elif defined(PYLABHUB_VAULT_HIGH_SECURITY)
constexpr VaultKdfProfile kVaultWriteProfile = VaultKdfProfile::Sensitive;
#else
constexpr VaultKdfProfile kVaultWriteProfile = VaultKdfProfile::Interactive;
#endif

/// Cost for `kVaultWriteProfile`, resolved at compile time.
constexpr VaultKdfCost vault_write_cost() noexcept
{
    VaultKdfCost c{0ULL, 0U};
    // The profile is one of the three enumerators by construction
    // above, so this never returns the zero-initialised value.
    (void)vault_kdf_cost(kVaultWriteProfile, c);
    return c;
}

constexpr unsigned long long kVaultOpsLimit = vault_write_cost().opslimit;
constexpr std::size_t kVaultMemLimit = vault_write_cost().memlimit;

static_assert(kVaultOpsLimit != 0ULL && kVaultMemLimit != 0U,
              "vault write profile must map to a known Argon2id cost");

constexpr std::size_t kVaultKeyBytes = 32U;   // symmetric key
constexpr std::size_t kVaultNonceBytes = 24U; // AEAD nonce
constexpr std::size_t kVaultMacBytes = 16U;   // AEAD tag (APPENDED, not prefixed)
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

/// Write a vault file at `path` in the §4.6.6 v1 format.
///
/// `fill_secret` is invoked with a span of exactly
/// `vault_secret_section_bytes(kind)` bytes and MUST fill all of it.
/// It is a callback rather than a parameter because the secret must
/// come straight out of the key store (VF-7): the vault kind knows
/// which named keys make up its secret section, and this layer does
/// not need to.
///
/// The span it receives points INTO the output buffer, at the offset
/// where the ciphertext will be written.  Encryption is in place, so
/// the secret is overwritten by its own ciphertext rather than left
/// behind for someone to remember to wipe.  On any failure path the
/// buffer is zeroed before the throw.
///
/// The header is authenticated as associated data (VF-3) and the nonce
/// is generated inside the security module, so no caller can reuse one.
/// The file is written 0600 and refuses to clobber an existing path.
///
/// Throws `std::runtime_error` on crypto or I/O failure, and
/// `std::out_of_range` if `key_name` is not in the KeyStore.
void vault_write(const std::filesystem::path &path, VaultKind kind,
                 std::string_view metadata_json, std::string_view key_name,
                 const std::function<void(std::span<std::uint8_t>)> &fill_secret);

/// Open a vault file written by `vault_write`.
///
/// Verifies the header before deriving anything (VF-1, VF-5), derives
/// at the profile the FILE records rather than this build's (VF-2),
/// decrypts with the header as associated data (VF-3), and checks the
/// plaintext covers the secret section before slicing it (VF-4).  On
/// success the derived vault key is left in the KeyStore under
/// `key_name`, so a later save needs neither the password nor a second
/// derivation.
///
/// `on_plaintext` receives the raw secret section and the JSON
/// remainder TOGETHER, while the plaintext is still live.  One callback
/// rather than two, because depositing the secret can require a value
/// from the metadata — a role identity is filed as a keypair, and only
/// the secret half is in the secret section — and two callbacks would
/// impose an ordering the caller has to know about.
///
/// The plaintext buffer is zeroed before this function returns, on
/// every path including one taken by a throwing callback — so the
/// callback must copy what it needs (into the key store, for a secret)
/// rather than retaining the span.
///
/// A wrong password, a tampered header, and a tampered ciphertext are
/// deliberately one outcome: the tag verifying IS the password check.
/// A file that is not a vault at all, or is a version or kind this
/// reader does not handle, is a DIFFERENT and distinguishable failure —
/// that distinction is the point of the header.
///
/// Throws `std::runtime_error` on any of the above.
void vault_read(const std::filesystem::path &path, VaultKind expected_kind,
                const std::string &uid, const std::string &password,
                std::string_view key_name,
                const std::function<void(std::span<const std::uint8_t> secret,
                                         std::string_view metadata_json)> &on_plaintext);

} // namespace pylabhub::utils::detail
