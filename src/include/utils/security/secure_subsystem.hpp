#pragma once
/**
 * @file secure_subsystem.hpp
 * @brief Process-wide security facade — the ONE C++ subsystem that
 *        owns libsodium and mediates every access to it.
 *
 * @author  pyLabHub team
 * @date    2026-07-04 (created) / 2026-07-06 (Crypto sub-container
 *          collapsed; two categories shipped) / 2026-07-07 (Category
 *          1c `box_encrypt_using` / `box_decrypt_using` shipped with
 *          name-based key citation; gate softened to `keys()` only)
 * @copyright  MIT
 * @see     HEP-CORE-0043 (Security Subsystem) — authoritative design
 * @see     HEP-CORE-0040 §5.2 (KeyStore API surface)
 * @see     `docs/IMPLEMENTATION_GUIDANCE.md` (pImpl idiom, lifecycle)
 *
 * # Purpose
 *
 * `SecureSubsystem` (informally "SMS") is a facade over the pyLabHub
 * security surface.  Every production caller that would previously
 * `#include <sodium.h>` and call `randombytes_buf`,
 * `crypto_generichash`, `crypto_secretbox_easy`, etc. now routes
 * through this module.  Nothing outside `src/utils/security/` (the
 * security-module directory) includes `<sodium.h>` — HEP-CORE-0043
 * §1.2 mechanism 4.
 *
 * # Two service categories
 *
 * The public surface has TWO categories:
 *
 * - **Category 1 — Sodium operations.**  Flat methods on
 *   `SecureSubsystem` itself.  Sub-grouped for documentation:
 *     - **1a. Byte primitives** — `random_bytes`, `random_u64`,
 *       `memcmp_ct`, `memzero`, `bin2hex`, `generate_shared_secret`.
 *     - **1b. Hash + KDF** — `compute_blake2b`, `verify_blake2b`,
 *       `pwhash_argon2id`.
 *     - **1c. Encryption / decryption** — `secretbox_encrypt`,
 *       `secretbox_decrypt`, `secretbox_encrypt_using`,
 *       `secretbox_decrypt_using`, `box_encrypt_using`,
 *       `box_decrypt_using` (all shipped).  The `_using` forms name
 *       their key instead of taking it; prefer them.  Future:
 *       `aead_encrypt/_decrypt`, `sealed_box_*`.
 *   Category 1 has NO instance state — every method is stateless.
 *
 * - **Category 2 — Key management.**  Nested sub-container
 *   `KeyStore`, accessed via `secure().keys()`.  Storage of
 *   long-term identity keypairs + ephemeral keys under the
 *   use-not-export contract (HEP-CORE-0040 §5.2).  KeyStore stays
 *   nested because it encapsulates real state (map + mutex +
 *   `LockedKey` machinery).
 *
 * # Singularity + lifecycle
 *
 * Exactly ONE `SecureSubsystem` instance exists per OS process
 * (HEP-CORE-0043 §1.3 — five singularity mechanisms).  The class is:
 *
 * - **Non-copyable, non-movable** (`= delete` on all four).
 * - **Private ctor** — the ONLY construction path is
 *   `SecureSubsystem::instance()`, a function-local `static
 *   SecureSubsystem sole;`.  C++11 guarantees thread-safe once-only
 *   initialization.
 * - **Registered as a STATIC lifecycle module** via
 *   `GetLifecycleModule()` in a `LifecycleGuard` mods pack.
 *   Same shape as `Logger::GetLifecycleModule()` and
 *   `FileLock::GetLifecycleModule()`.
 * - **CAS-guarded bringup** — first line of `Impl::bringup()` does
 *   `compare_exchange` from `Uninitialized` → `InitCalled`; any
 *   double-init PANICs.
 *
 * The singleton's `sole` lives for program lifetime and is
 * atexit-destructed.  The shutdown thunk publishes state
 * transitions but does NOT delete the outer object (no
 * `new`/`delete` crosses the shared-lib boundary).
 *
 * # Gate discipline
 *
 * The state gate protects OUR state (KeyStore) — NOT libsodium's.
 * Only `keys()` requires SMS to be `Initialized`; every other method
 * on `SecureSubsystem` is a stateless wrapper on libsodium primitives
 * that libsodium self-initializes on first use.  This softened gate
 * (revised 2026-07-07) matches the actual failure surface: gating a
 * primitive like `random_bytes` on SMS state added an artificial
 * failure mode without buying real security — production processes
 * always run under the mod pack, and libsodium's implicit init makes
 * `random_bytes()` work correctly even without SMS bringup.
 *
 * - **Gated (PANICs on non-`Initialized`):** `keys()` — bypass path
 *   via `SecureSubsystem::instance().keys()` also PANICs (double gate).
 * - **Ungated (safe pre-bringup):** `secure()`, `sodium_ready()`,
 *   `lifecycle_initialized()`, and every Category 1 method
 *   (`random_bytes`, `memcmp_ct`, `memzero`, `bin2hex`,
 *   `compute_blake2b`, `verify_blake2b`, `derive_pwhash_salt`,
 *   `pwhash_argon2id`, `secretbox_encrypt`, `secretbox_decrypt`).
 *
 * Note: the four `*_using` methods (`secretbox_encrypt_using`,
 * `secretbox_decrypt_using`, `box_encrypt_using`, `box_decrypt_using`)
 * reach through `keys()` internally to resolve the key by name — they
 * inherit the `keys()` gate transitively.  Callers get the PANIC if they
 * invoke these methods without SMS being up.
 *
 * See `secure_subsystem.cpp` "Gate policy" block for the full
 * rationale.  Reaching `keys()` before SMS is up is a PROGRAMMER
 * ERROR, not a recoverable exception — same discipline as
 * `FileLock` and `Logger`.
 *
 * # Standard usage
 *
 * @code
 * // main.cpp (production role binary):
 * pylabhub::utils::LifecycleGuard lifecycle(
 *     pylabhub::utils::MakeModDefList(
 *         pylabhub::utils::Logger::GetLifecycleModule(),
 *         pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
 *         // ... other modules ...
 *     ));
 * // At this point SMS is Initialized; secure() is safe to call.
 *
 * // Anywhere downstream:
 * namespace sec = pylabhub::utils::security;
 * std::array<std::uint8_t, 32> hash{};
 * sec::secure().compute_blake2b(hash.data(), buf.data(), buf.size());
 *
 * sec::secure().keys().add_identity_from_z85(
 *     sec::kHubIdentityName, pub_z85, sec_z85);
 * @endcode
 *
 * # Platform hardening
 *
 * `Impl::bringup()` (see `.cpp`) also performs per-OS hardening:
 * - Linux/BSD/macOS: `setrlimit(RLIMIT_CORE, 0)` + Linux
 *   `prctl(PR_SET_DUMPABLE, 0)`.
 * - Windows: `SetErrorMode` + `WerAddExcludedApplication`.
 * - `RLIMIT_MEMLOCK` capability probe with a WARN log below the
 *   256 KiB threshold.
 *
 * These steps are IRREVERSIBLE — core dumps stay disabled for the
 * process lifetime (HEP-CORE-0043 §1.5).
 */

#include "pylabhub_utils_export.h"
#include "utils/module_def.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pylabhub::utils::security
{

/// BLAKE2b-256 hash output size in bytes (32).
inline constexpr std::size_t BLAKE2B_HASH_BYTES = 32;

// Forward-declare KeyStore — full definition in `key_store.hpp`.
// SecureSubsystem holds one as a member of `Impl` per HEP-CORE-0043 §2.
class KeyStore;

// Forward-declare startup/shutdown thunks in the namespace so the
// class can befriend them (Logger pattern — thunks need `pImpl`
// access to drive `Impl::bringup()` / `Impl::shutdown_module()`).
void do_secure_subsystem_startup(const char *, void *);
void do_secure_subsystem_shutdown(const char *, void *);

class PYLABHUB_UTILS_EXPORT SecureSubsystem
{
    // Startup / shutdown thunks drive the singleton via `pImpl`.
    friend void do_secure_subsystem_startup(const char *, void *);
    friend void do_secure_subsystem_shutdown(const char *, void *);

  public:
    // ═════════════════════════════════════════════════════════════
    // Lifecycle
    // ═════════════════════════════════════════════════════════════

    /// Singleton accessor — function-local static.  Thread-safe per
    /// C++11 function-local-static-initialization.  First call
    /// constructs `sole` (ctor is trivial: allocates `pImpl` only).
    /// Actual bringup work runs inside `pImpl->bringup()` driven by
    /// the startup thunk.  Both the startup thunk and the free-
    /// function `secure()` accessor route through this method.
    static SecureSubsystem &instance();

    /// HEP-CORE-0043 §2.3 — the STATIC lifecycle module.  Registered
    /// via a `LifecycleGuard` mods pack.  Dependency:
    /// `"pylabhub::utils::Logger"`.
    static pylabhub::utils::ModuleDef GetLifecycleModule();

    /// Non-throwing probe — `true` iff the state gate is NOT
    /// `Uninitialized`.
    static bool lifecycle_initialized() noexcept;

    ~SecureSubsystem();

    SecureSubsystem(const SecureSubsystem &) = delete;
    SecureSubsystem &operator=(const SecureSubsystem &) = delete;
    SecureSubsystem(SecureSubsystem &&) = delete;
    SecureSubsystem &operator=(SecureSubsystem &&) = delete;

    // ═════════════════════════════════════════════════════════════
    // Category 2 — Key management (sub-container)
    // ═════════════════════════════════════════════════════════════

    /// Access the process-wide KeyStore (HEP-CORE-0043 §2.2).
    /// KeyStore is a MEMBER of `SecureSubsystem::Impl` — no external
    /// construction site exists (KeyStore ctor is private, only
    /// `Impl` befriends it).  PANIC if SMS is not `Initialized`.
    ///
    /// The gate is defensive: `secure()` also gates, but a caller
    /// that reaches `SecureSubsystem::instance().keys()` directly
    /// bypasses `secure()`'s gate; this second check catches that
    /// bypass path.
    KeyStore &keys();

    // ═════════════════════════════════════════════════════════════
    // Category 1a — Byte primitives (stateless)
    // ═════════════════════════════════════════════════════════════
    //
    // Contract: every method gates on the state atomic and PANICs
    // (`PLH_PANIC` → process abort) if not `Initialized`.  Calling
    // any of these before SecureSubsystem is up is a programmer
    // error — not a recoverable exception.  Type discipline:
    // `uint8_t` throughout (matches sodium's `unsigned char *`).

    /// Fill `out` with cryptographically-secure random bytes.
    /// Wrapper for `randombytes_buf`.
    void random_bytes(std::span<std::uint8_t> out);

    /// Pointer/size overload of `random_bytes`.
    void random_bytes(std::uint8_t *out, std::size_t len);

    /// Random 64-bit unsigned integer.
    [[nodiscard]] std::uint64_t random_u64();

    /// Random 64-byte shared secret (matches the retired
    /// `pylabhub::crypto::generate_shared_secret`).
    [[nodiscard]] std::array<std::uint8_t, 64> generate_shared_secret();

    /// Constant-time memory compare (replaces `sodium_memcmp`).
    /// Returns true iff spans are equal length AND byte-equal.
    [[nodiscard]] bool memcmp_ct(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);

    /// Zero a memory region — compiler-optimizer-proof (replaces
    /// `sodium_memzero`).
    void memzero(std::span<std::uint8_t> region);

    /// Encode raw bytes to lowercase hex (wrapper for
    /// `sodium_bin2hex`).  `hex_max_len` must be at least
    /// `bin_len * 2 + 1`.
    ///
    /// Failure signal: LOGS + no-ops on null-pointer arguments.
    /// Callers are expected to pass non-null buffers with the
    /// correct size — the failure mode is a programmer error, and
    /// the log line is the diagnostic.  If the caller cannot
    /// guarantee non-null upstream, they must check the arguments
    /// themselves before calling.
    void bin2hex(char *hex, std::size_t hex_max_len, const std::uint8_t *bin, std::size_t bin_len);

    // ═════════════════════════════════════════════════════════════
    // Category 1b — Hash + KDF (stateless)
    // ═════════════════════════════════════════════════════════════

    /// BLAKE2b-256 hash — the canonical content-addressing hash used
    /// throughout pyLabHub (data-block slot checksums, schema hashes,
    /// content integrity).  Wrapper for `crypto_generichash` with
    /// output length fixed at `BLAKE2B_HASH_BYTES` (32).
    ///
    /// @param out   Output buffer, at least 32 bytes.  Must be non-null.
    /// @param data  Input pointer.  Must be non-null (0-length input
    ///              is legal — pass any non-null pointer + `len=0`).
    /// @param len   Input length in bytes.
    /// @return true on success, false on null pointer or sodium failure.
    bool compute_blake2b(std::uint8_t *out, const void *data, std::size_t len);

    /// BLAKE2b-256 as std::array.  Failure signal: returns all-zeros.
    ///
    /// The failure paths (`data == nullptr` and internal
    /// `crypto_generichash` non-zero rc) are both essentially
    /// impossible in production — the former is caught by input
    /// validation upstream, the latter has never been observed in
    /// libsodium 1.0.18+ for the constant-size 32-byte output.
    /// Callers that want a hard-failure signal should use
    /// `compute_blake2b(out, data, len) → bool` instead.
    ///
    /// **Understand which way the risk runs.**  An earlier revision of this
    /// comment justified the weak marker by saying all-zeros "can also be a
    /// LEGITIMATE hash output for a specific input".  That is the harmless
    /// direction and it is not reachable — producing 32 zero bytes from
    /// BLAKE2b-256 is a preimage attack (~2^256).  Nobody will ever hit it.
    ///
    /// The direction that matters is the opposite one: on failure this
    /// returns a value that compares EQUAL for every input.  Callers that
    /// compare two of these — envelope tamper binding, schema tags — would
    /// then agree unconditionally, so an integrity check built on it fails
    /// OPEN, not closed.  That is why the callers below document why their
    /// trigger is unreachable rather than simply not checking.
    ///
    /// The alternative — returning `std::optional<std::array<...>>` — would
    /// churn ~15 caller sites; the chosen mitigation is instead that every
    /// caller either checks, or states at the call site why it cannot fire.
    [[nodiscard]] std::array<std::uint8_t, 32> compute_blake2b_array(const void *data,
                                                                     std::size_t len);

    /// Verify a stored BLAKE2b-256 hash against `data[0..len)` using
    /// constant-time comparison.
    [[nodiscard]] bool verify_blake2b(const std::uint8_t *stored, const void *data,
                                      std::size_t len);

    /// std::array overload of `verify_blake2b`.
    [[nodiscard]] bool verify_blake2b(const std::array<std::uint8_t, 32> &stored, const void *data,
                                      std::size_t len);

    /// Derive an Argon2id salt from a domain-separator string.
    ///
    /// Output is exactly `kPwhashSaltBytes` (16) — the length
    /// libsodium's `crypto_pwhash` requires as its salt input.
    /// Internally computes BLAKE2b in `outlen=16` mode (a genuine
    /// 16-byte BLAKE2b, NOT a truncation of BLAKE2b-256 — the digest
    /// length participates in the algorithm's initial state per RFC
    /// 7693 §3.1).
    ///
    /// Deterministic: same `domain` produces the same salt.  That is
    /// what lets a vault be reopened without storing its salt in the
    /// file: the uid reproduces it.  The live caller is
    /// `KeyStore::add_key_from_password`, which hashes the caller's
    /// `scope` into the salt.
    ///
    /// @param salt_out  16-byte output buffer.  Must be non-null.
    /// @param domain    Non-secret domain separator (typically a uid).
    /// @return true on success, false on null pointer or sodium
    ///         failure.
    bool derive_pwhash_salt(std::uint8_t *salt_out, std::string_view domain);

    /// Argon2id password-based KDF (wrapper for `crypto_pwhash`
    /// with `crypto_pwhash_ALG_ARGON2ID13`).  Uses INTERACTIVE
    /// ops/mem-limit constants — appropriate for vault-file unlock;
    /// not for password-storage KDF.
    ///
    /// `salt` MUST point at a `kPwhashSaltBytes` (16) byte buffer.
    /// Typically produced by `derive_pwhash_salt(salt, domain)`.
    ///
    /// @param out            Derived-key output buffer.
    /// @param out_len        Derived-key size in bytes (e.g. 32 for
    ///                       an XSalsa20-Poly1305 key).
    /// @param password       Password bytes.  Not required to be
    ///                       null-terminated.
    /// @param password_len   Password length in bytes.
    /// @param salt           16-byte salt (`kPwhashSaltBytes`).
    /// @return true on success, false on null pointer or Argon2
    ///         failure (typically insufficient memory).
    /// Argon2id password hashing.
    ///
    /// **`opslimit` / `memlimit` are the caller's policy, not this
    /// module's.**  How expensive a derivation should be depends on
    /// what is being protected and where it runs, so the owner of the
    /// data chooses; the defaults below are libsodium's INTERACTIVE
    /// pair and suit a caller with no opinion.
    ///
    /// This used to hardcode INTERACTIVE with no way to override it,
    /// which silently disabled every caller-side strength setting —
    /// `vault_crypto`'s three compile-time profiles selected constants
    /// that then reached nothing.  A cost knob that cannot be read is
    /// worse than none: the build reports success and the strength
    /// never changes.
    ///
    /// **Changing these changes what can be decrypted.**  A key derived
    /// at one setting will not match one derived at another, and the
    /// failure surfaces as a MAC mismatch — indistinguishable from a
    /// wrong password.
    [[nodiscard]] bool pwhash_argon2id(std::uint8_t *out, std::size_t out_len, const char *password,
                                       std::size_t password_len, const std::uint8_t *salt,
                                       unsigned long long opslimit = kPwhashOpsLimitInteractive,
                                       std::size_t memlimit = kPwhashMemLimitInteractive);

    /// `crypto_pwhash_SALTBYTES` (16) — the exact salt size Argon2id
    /// requires as input.  Exposed as a constant so callers can size
    /// their salt buffers without pulling `<sodium.h>` in.
    static constexpr std::size_t kPwhashSaltBytes = 16;

    /// libsodium's INTERACTIVE cost pair — the default for callers with
    /// no policy of their own.  Mirrored here so callers can name the
    /// values without including `<sodium.h>`; static-asserted against
    /// the real constants in the implementation, so they cannot drift.
    static constexpr unsigned long long kPwhashOpsLimitInteractive = 2ULL;
    static constexpr std::size_t kPwhashMemLimitInteractive = 67108864U; // 64 MiB

    // ═════════════════════════════════════════════════════════════
    // Category 1c — Encryption / decryption (protocol operations)
    // ═════════════════════════════════════════════════════════════
    //
    // These methods make PROTOCOL DECISIONS: nonce discipline, MAC
    // verification, key material handling.  Callers MUST:
    //   - Supply unique nonces per key (nonce reuse under the same
    //     key is a catastrophic failure of XSalsa20).
    //   - Check the return value (0 = failure — MAC mismatch, bad
    //     input, or wrong key).
    //   - Ensure keys are exactly the byte length the primitive
    //     requires (`kSecretbox*Bytes` constants below).

    /// XSalsa20-Poly1305 authenticated symmetric encryption
    /// (wrapper for `crypto_secretbox_easy`).  Encrypts `plaintext`
    /// under 32-byte `key` and 24-byte `nonce`.  Ciphertext contains
    /// the MAC as the first 16 bytes (combined mode) — do NOT
    /// interpret bytes 0..15 as data.  Writes
    /// `plaintext.size() + kSecretboxMacBytes` bytes to `out`.
    /// Returns the number of bytes written, 0 on failure.
    ///
    /// Nonce and key are statically-sized spans — the compiler
    /// enforces exact 24-byte / 32-byte inputs at each call site
    /// (Priority 7 of the SMS review — replaces the previous
    /// unsafe `const std::uint8_t *nonce, const std::uint8_t *key`
    /// signature that would silently read past a shorter buffer).
    ///
    /// **Prefer `secretbox_encrypt_using` below.**  This form makes the
    /// caller hold the key, which is the courier problem HEP-CORE-0043
    /// §2.5.1 exists to remove; it also makes the caller pick a nonce,
    /// and a repeated nonce breaks XSalsa20 catastrophically.  The
    /// vault used to be the caller here and no longer is.  Retained for
    /// callers that must control the nonce because a peer parses the
    /// frame — the same reason `box_*_using` keeps its explicit nonce.
    [[nodiscard]] std::size_t secretbox_encrypt(std::uint8_t *out, std::size_t out_max_len,
                                                const std::uint8_t *plaintext,
                                                std::size_t plaintext_len,
                                                std::span<const std::uint8_t, 24> nonce,
                                                std::span<const std::uint8_t, 32> key);

    /// XSalsa20-Poly1305 authenticated symmetric decryption
    /// (wrapper for `crypto_secretbox_open_easy`).  Verifies the MAC
    /// and decrypts into `out`.  Returns
    /// `ciphertext_len - kSecretboxMacBytes` on success, 0 on MAC
    /// failure or bad input.  A 0 return means the ciphertext was
    /// tampered with or the key is wrong — CALLERS MUST CHECK.
    ///
    /// Nonce and key are statically-sized spans — same rationale as
    /// `secretbox_encrypt` above.
    [[nodiscard]] std::size_t secretbox_decrypt(std::uint8_t *out, std::size_t out_max_len,
                                                const std::uint8_t *ciphertext,
                                                std::size_t ciphertext_len,
                                                std::span<const std::uint8_t, 24> nonce,
                                                std::span<const std::uint8_t, 32> key);

    /// Encrypt `plaintext` under the **named** symmetric key, writing
    /// a self-contained sealed blob to `out`.
    ///
    /// This is the symmetric twin of `box_encrypt_using`, and the
    /// reason to prefer it over `secretbox_encrypt` above: the key is
    /// named, not passed, so the caller never holds key bytes.
    ///
    /// **The nonce is generated inside and written to the front of
    /// `out`** — `[nonce(24) ‖ MAC(16) ‖ ciphertext]`.  Callers do not
    /// supply one, which is the point: a nonce must never repeat for a
    /// given key, and a caller that cannot supply one cannot repeat
    /// one.  `box_*_using` keeps its explicit nonce because there the
    /// bytes are a protocol frame another implementation must parse;
    /// here they are an opaque blob only we ever open.
    ///
    /// `out` needs `plaintext.size() + kSealedOverheadBytes` bytes.
    /// Returns bytes written, or 0 on failure (short `out`, missing
    /// key, or a key that is not `kSecretboxKeyBytes` long).
    ///
    /// Throws `std::out_of_range` if `key_name` is absent — a missing
    /// key is a wiring error, distinct from the 0 return that means
    /// "the crypto did not work".
    [[nodiscard]] std::size_t secretbox_encrypt_using(std::string_view key_name,
                                                      std::span<const std::uint8_t> plaintext,
                                                      std::span<std::uint8_t> out);

    /// Reverse of `secretbox_encrypt_using`.  `sealed` is the whole
    /// blob including its leading nonce.  Writes
    /// `sealed.size() - kSealedOverheadBytes` bytes to `out`.
    ///
    /// Returns 0 if the MAC does not verify — meaning the data was
    /// tampered with, or the key is wrong.  **Callers must check.**
    /// For a password-derived key this return IS the password check:
    /// the tag verifies or it does not.
    ///
    /// Throws `std::out_of_range` if `key_name` is absent.
    [[nodiscard]] std::size_t secretbox_decrypt_using(std::string_view key_name,
                                                      std::span<const std::uint8_t> sealed,
                                                      std::span<std::uint8_t> out);

    /// Bytes that `secretbox_encrypt_using` adds to the plaintext
    /// length: the leading nonce plus the MAC.  Sizing constant for
    /// callers allocating an output buffer.
    static constexpr std::size_t kSealedOverheadBytes = 24 + 16;

    /// `crypto_secretbox_KEYBYTES` (32) — the exact secretbox key
    /// size.  Exposed so callers can size their key buffers without
    /// pulling `<sodium.h>` in.
    static constexpr std::size_t kSecretboxKeyBytes = 32;
    /// `crypto_secretbox_NONCEBYTES` (24).
    static constexpr std::size_t kSecretboxNonceBytes = 24;
    /// `crypto_secretbox_MACBYTES` (16) — the MAC prefix length
    /// that `secretbox_encrypt`'s output contains.
    static constexpr std::size_t kSecretboxMacBytes = 16;

    // ── Category 1c — Asymmetric box (crypto_box) ────────────────
    // Two-party authenticated encryption using Curve25519 +
    // XSalsa20-Poly1305 keypairs.  Sender proves possession of
    // `own_seckey_name`'s secret key AND recipient owns the
    // matching pubkey — mutual authentication built into every
    // ciphertext (unlike `secretbox` which is single-party
    // symmetric).
    //
    // The sender's seckey is cited by NAME — SMS resolves it via
    // KeyStore's `with_seckey`, uses it inside a scoped callback,
    // and NEVER exposes the raw bytes at the API boundary
    // (HEP-CORE-0043 §1.4 use-not-export).  Callers pass the
    // KeyStore entry name (e.g. `kHubIdentityName`,
    // `kRoleIdentityName`, or `"broker.observer"`), NOT the seckey
    // bytes.
    //
    // The peer's pubkey travels as raw 32 bytes.  It's non-secret,
    // known from a trusted source (broker allowlist, REG_ACK, etc.),
    // and doesn't participate in the use-not-export contract.  If
    // the caller has Z85, decode with `zmq_z85_decode` at the wire
    // boundary before calling.

    /// Encrypt `plaintext` under (`own_seckey_name`, `peer_pubkey`).
    /// Wrapper for `crypto_box_easy` (Curve25519 + XSalsa20-Poly1305).
    /// Ciphertext contains the MAC as the first 16 bytes (combined
    /// mode) — bytes 0..15 are NOT plaintext data.  Writes
    /// `plaintext.size() + kBoxMacBytes` bytes into `out`.
    ///
    /// @param own_seckey_name  KeyStore entry name for the sender's
    ///                          identity keypair.  MUST be present in
    ///                          `secure().keys()` (SMS must be
    ///                          `Initialized` — this method's `keys()`
    ///                          access gates on state).  The seckey
    ///                          bytes never cross the API boundary.
    /// @param peer_pubkey_raw   Recipient's raw 32-byte Curve25519
    ///                          pubkey.  Non-secret; typically from
    ///                          the broker allowlist or REG_ACK.
    /// @param nonce             Unique 24-byte nonce per (key, message).
    ///                          Reusing a nonce under the same key is
    ///                          a catastrophic failure of XSalsa20.
    /// @param plaintext         Plaintext to encrypt (may be empty).
    /// @param out               Output buffer, at least
    ///                          `plaintext.size() + kBoxMacBytes` bytes.
    /// @return Bytes written on success (== `plaintext.size() +
    ///         kBoxMacBytes`), 0 on failure (key lookup failure,
    ///         buffer too small, sodium error).
    [[nodiscard]] std::size_t box_encrypt_using(std::string_view own_seckey_name,
                                                std::span<const std::uint8_t, 32> peer_pubkey_raw,
                                                std::span<const std::uint8_t, 24> nonce,
                                                std::span<const std::uint8_t> plaintext,
                                                std::span<std::uint8_t> out);

    /// Decrypt `ciphertext` under (`own_seckey_name`, `peer_pubkey`).
    /// Wrapper for `crypto_box_open_easy`.  Verifies the MAC and
    /// decrypts into `out`.
    ///
    /// Callers MUST check the return value — 0 means the ciphertext
    /// was tampered with, produced under a different (key, nonce)
    /// pair, or `peer_pubkey_raw` is wrong.  This is the SOLE
    /// authentication signal — do not proceed on `0` return.
    ///
    /// @param own_seckey_name  KeyStore entry name for the recipient's
    ///                          identity keypair.  Same semantics as
    ///                          `box_encrypt_using`.
    /// @param peer_pubkey_raw   Sender's raw 32-byte Curve25519 pubkey.
    /// @param nonce             The 24-byte nonce the sender used.
    /// @param ciphertext        Ciphertext with MAC prefix (at least
    ///                          `kBoxMacBytes` bytes).
    /// @param out               Output buffer, at least
    ///                          `ciphertext.size() - kBoxMacBytes` bytes.
    /// @return Bytes written on success (== `ciphertext.size() -
    ///         kBoxMacBytes`), 0 on MAC failure, key lookup failure,
    ///         buffer too small, or bad input.
    [[nodiscard]] std::size_t box_decrypt_using(std::string_view own_seckey_name,
                                                std::span<const std::uint8_t, 32> peer_pubkey_raw,
                                                std::span<const std::uint8_t, 24> nonce,
                                                std::span<const std::uint8_t> ciphertext,
                                                std::span<std::uint8_t> out);

    /// `crypto_box_PUBLICKEYBYTES` (32).
    static constexpr std::size_t kBoxPubkeyBytes = 32;
    /// `crypto_box_SECRETKEYBYTES` (32).
    static constexpr std::size_t kBoxSeckeyBytes = 32;
    /// `crypto_box_NONCEBYTES` (24).
    static constexpr std::size_t kBoxNonceBytes = 24;
    /// `crypto_box_MACBYTES` (16) — MAC prefix length in
    /// `box_encrypt_using`'s output.
    static constexpr std::size_t kBoxMacBytes = 16;

    // Future encryption verbs land here as more protocols absorb
    // (aead_encrypt / _decrypt, sealed_box_seal / _open).  All flat
    // on `SecureSubsystem` — no separate class.

    /// Implementation state — forward-declared public so `KeyStore`
    /// can befriend `SecureSubsystem::Impl` to grant Impl's member-
    /// init access to KeyStore's private ctor (HEP-CORE-0043 §2.2).
    struct Impl;

  private:
    /// Private ctor — singleton discipline.  Only construction path
    /// is the function-local static in `instance()`, driven by the
    /// startup thunk.
    SecureSubsystem();

    std::unique_ptr<Impl> pImpl;
};

/// HEP-CORE-0043 §2.1 canonical accessor.  Returns a reference to
/// the process's SecureSubsystem instance.  PANICs if the state gate
/// is not `Initialized`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT SecureSubsystem &secure();

/// Non-throwing probe — true iff SecureSubsystem is constructed AND
/// `sodium_init()` has completed successfully.  This is the gate
/// every consumer of libsodium uses before touching libsodium.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool sodium_ready() noexcept;

} // namespace pylabhub::utils::security
