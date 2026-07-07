#pragma once
/**
 * @file key_store.hpp
 * @brief Process-wide locked-memory secret store — the ONE holder of
 *        long-term identities and ephemeral runtime keys.
 *
 * @author  pyLabHub team
 * @date    2026-06-06 (use-not-export ship) /
 *          2026-07-06 (KeyStore-as-member of SMS::Impl)
 * @copyright  MIT
 * @see  HEP-CORE-0043 §2.2 + §7  (KeyStore contract)
 * @see  HEP-CORE-0040 §5, §6, §8.5.2  (LockedKey RAII, raw-64 layout)
 * @see  secure_subsystem.hpp  (facade + `secure().keys()` accessor)
 *
 * # Ownership + lifecycle
 *
 * KeyStore is a MEMBER of `SecureSubsystem::Impl` (field `keys_`).
 * There is EXACTLY ONE KeyStore per process, brought up automatically
 * when `SecureSubsystem::GetLifecycleModule()` fires its startup
 * thunk.  KeyStore has:
 * - **Private ctor** (`friend struct SecureSubsystem::Impl`) — no
 *   external construction site can compile.
 * - **No independent lifecycle module** — its lifetime is bound to
 *   SMS's.  LifecycleManager sees ONE module (`"SecureSubsystem"`),
 *   not two.
 * - **Access via `secure().keys()`** — the SMS gate ensures sodium
 *   is initialized before any KeyStore method runs.
 *
 * # Use-not-export contract
 *
 * The security module exposes OPERATIONS on secret material, not
 * byte exports (HEP-CORE-0040 §5.2 + §8.2, round-5 2026-06-06):
 *
 * - **Public-half keys (`pubkey`)** — returned as `std::string_view`
 *   into non-secret cache bytes owned by the Entry.  Non-secret;
 *   safe to copy, log, pass across boundaries.
 * - **Secret-half keys (`with_seckey`)** — accessed only via a
 *   callback.  The seckey `std::string_view` handed to the callback
 *   is valid ONLY for callback scope; bytes NEVER leave the mlocked
 *   `LockedKey` region as data.
 * - **Raw HEP-0038 secrets (`lookup_raw`)** — returned as
 *   `std::span<const std::byte>` into LockedKey-owned bytes.
 *   Script bindings must materialize into a script-owned buffer
 *   before returning to the script layer.
 *
 * # Storage layout
 *
 * Internally owns a name → `LockedKey` map under a shared mutex.
 * Each `LockedKey`:
 * - Allocated via `sodium_malloc` (mlock + guard pages + canary).
 * - Destructed via `sodium_memzero` + `sodium_free`.
 * - Full definition lives in `key_store.cpp` (anonymous namespace);
 *   not exposed at the public surface (HEP-CORE-0043 §2.1 R7).
 *
 * Identity keypairs are stored as RAW 64 bytes: `pub_raw[32]` +
 * `sec_raw[32]` (HEP-CORE-0040 §8.5.2, #291 flip 2026-06-26).  Z85
 * conversion happens at the wire/file/display boundary via
 * `add_identity_from_z85` / `pubkey()` / `with_seckey_z85`.
 *
 * # Concurrency
 *
 * - **Read-mostly** (shared lock, parallel):
 *   `pubkey`, `with_seckey`, `with_seckey_z85`, `with_keypair_z85`,
 *   `lookup_raw`, `has`, `size`.
 * - **Write** (exclusive lock):
 *   `add_identity`, `add_identity_from_z85`,
 *   `generate_and_add_identity`, `add_raw`, `remove`.
 *
 * The `with_seckey` callback runs under the SHARED lock.  Callback
 * MUST be prompt (microseconds — no blocking I/O, no syscalls beyond
 * consuming the bytes).  A concurrent `remove(name)` blocks until
 * every in-flight `with_seckey` callback for that name returns —
 * this is the security guarantee that "bytes become unreachable for
 * every caller as soon as `remove()` returns."
 *
 * Full thread-safety contract: HEP-CORE-0040 §5.5.
 *
 * # Canonical entry names
 *
 * - `kHubIdentityName` = `"hub_identity"` — the hub's CURVE identity.
 * - `kRoleIdentityName` = `"role_identity"` — the role's CURVE identity.
 *
 * At most one of each per process (HEP-CORE-0040 §5.3).  Federation
 * testing requires separate processes; Pattern-3 subprocess isolation
 * is the test-side pattern.  Test conventions (e.g.
 * `role_keystore_name(uid)`) live in
 * `tests/test_framework/curve_test_setup.h` — NOT production names,
 * MUST NOT be used by library code.
 */

#include "pylabhub_utils_export.h"
// Transitive include contract (F19, 2026-07-06):
//   Every consumer of `KeyStore` reaches the process's instance via
//   `secure().keys()`.  `secure()` is declared in
//   `secure_subsystem.hpp`; the friend declaration below also names
//   `SecureSubsystem::Impl`.  Both of those require the SMS header
//   to be visible when `key_store.hpp` is #included.  We pull SMS's
//   header here so consumers only need one #include for the
//   `secure().keys().X()` idiom.  Consumers that touch other SMS
//   surfaces (`secure().secretbox_encrypt()`, `secure().random_bytes()`,
//   `sodium_ready()`) may include `secure_subsystem.hpp` directly
//   without relying on this transitive pull — it's a convenience,
//   not a substitute for explicit includes at the SMS use site.
#include "utils/security/secure_subsystem.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace pylabhub::utils::security
{

/// Canonical KeyStore entry names — HEP-CORE-0040 §5.3.  Exactly one
/// entry per process per role kind (production constraint inherited
/// from `HubConfig` / `RoleConfig`: at most one hub identity and at
/// most one role identity per process).  Federation testing requires
/// separate processes; Pattern-3 subprocess isolation is the test-side
/// pattern for multi-role scenarios that share a single-process
/// production constraint.
///
/// Defined as `inline constexpr` so callers can use them as
/// `std::string_view` arguments to `KeyStore::add_identity_from_z85`,
/// `KeyStore::pubkey`, `KeyStore::with_seckey`, and `KeyStore::has`
/// without per-translation-unit storage duplication.
///
/// Test-side conventions for multi-role-in-one-process scenarios live
/// in `tests/test_framework/curve_test_setup.h` (e.g.
/// `role_keystore_name(uid)`); they are NOT production names and MUST
/// NOT be used by lib code.
inline constexpr std::string_view kHubIdentityName  = "hub_identity";
inline constexpr std::string_view kRoleIdentityName = "role_identity";

class PYLABHUB_UTILS_EXPORT KeyStore
{
public:
    /// Destroys all stored LockedKey instances (each `sodium_memzero` +
    /// `sodium_free`).  Defined in `.cpp` so `Impl` is complete at the
    /// dtor site (canonical pImpl per `IMPLEMENTATION_GUIDANCE §"pImpl
    /// Idiom"`).
    ~KeyStore();

    KeyStore(const KeyStore &)            = delete;
    KeyStore &operator=(const KeyStore &) = delete;
    KeyStore(KeyStore &&)                 = delete;
    KeyStore &operator=(KeyStore &&)      = delete;

    // ── Writes (exclusive lock) ──────────────────────────────────────

    /// Insert an identity keypair packed as pub_raw (32 bytes) +
    /// sec_raw (32 bytes) — 64 bytes total raw binary
    /// (HEP-CORE-0040 §8.5.2).  Source buffer zeroed before return.
    /// Throws `std::runtime_error` if `name` already present, if the
    /// span is not exactly 64 bytes, or if `sodium_malloc` fails
    /// (RLIMIT_MEMLOCK denial).
    ///
    /// **Pre-#291 historical note:** packed shape was 80 bytes Z85
    /// (pub_z85 || sec_z85).  HEP-CORE-0040 §8.5.2 flipped storage to
    /// raw 64 bytes so every libsodium primitive (and AttachProtocol)
    /// consumes the canonical form directly; Z85 is computed on-demand
    /// at the wire / file / display boundary via
    /// `add_identity_from_z85` / `pubkey()` / `with_seckey_z85`.
    void add_identity(std::string_view name, std::span<std::byte> packed_pub_sec);

    /// Generate a fresh CURVE keypair in-memory and register it under
    /// `name`.  Returns the Z85 pubkey; the seckey is accessible only
    /// via `with_seckey(name, ...)`, per the HEP-CORE-0040 §5.2
    /// use-not-export contract.
    ///
    /// The keypair lives for the KeyStore's lifetime (typically the
    /// process lifetime).  No disk write anywhere; libsodium primitives
    /// operate on mlocked LockedKey memory.
    ///
    /// **Naming.**  Callers pick their own names; storage enforces no
    /// naming rules.  When the script crypto API lands (task #247),
    /// script bindings will TRANSLATE script-provided names into
    /// sandboxed storage names before calling this method (e.g.,
    /// `"script." + role_uid + "." + user_name`).  Storage need not
    /// know about the sandbox distinction; it just stores whatever
    /// name the caller passes.
    ///
    /// Design record: `docs/tech_draft/DRAFT_keystore_ephemeral_and_
    /// script_crypto_2026-07.md`; will promote to HEP-CORE-0043 (or
    /// HEP-CORE-0040 amendment) once script bindings land.  First
    /// consumer: broker observer identity per HEP-CORE-0041 §D1(d).
    ///
    /// Throws `std::runtime_error` if `name` already present, if
    /// libzmq CSPRNG init fails, or if `sodium_malloc` fails.
    [[nodiscard]] std::string
    generate_and_add_identity(std::string_view name);

    /// Convenience: insert an identity keypair given the two Z85
    /// halves separately.  Internally Z85-decodes both halves into a
    /// `SecureBuffer<64>` (zero-on-destruct) of raw bytes and
    /// delegates to `add_identity`.
    ///
    /// This is the SINGLE site (across production + tests) where the
    /// Z85→raw decode at the security-module boundary lives
    /// (HEP-CORE-0040 §8.5.2).  Production callers
    /// (`HubConfig::load_keypair`, `RoleConfig::load_keypair`) and
    /// test fixtures both go through this method, so the encoding
    /// boundary is in exactly one place.
    ///
    /// Throws `std::runtime_error` if `name` already present, if
    /// either half is not exactly 40 chars (Z85 of 32 raw bytes), if
    /// `zmq_z85_decode` fails (malformed Z85), or if `sodium_malloc`
    /// fails.
    void add_identity_from_z85(std::string_view name,
                                std::string_view pub_z85,
                                std::string_view sec_z85);

    /// Insert raw secret bytes (HEP-0038 script vault_save).
    /// `plaintext` zeroed before return.  Caller-side name validation
    /// enforces reserved-prefix rules; KeyStore stores opaque bytes.
    /// Throws `std::runtime_error` if `name` already present or if
    /// `sodium_malloc` fails.
    void add_raw(std::string_view name, std::span<std::byte> plaintext);

    /// Remove a stored secret.  No-op if absent.  Blocks until any
    /// in-flight `with_seckey` callback for the same name returns —
    /// correct security semantic: bytes become unreachable for every
    /// caller as soon as `remove()` returns.
    void remove(std::string_view name);

    // ── Reads (shared lock; parallel across consumers) ──────────────

    /// Return the Z85 PUBLIC key (40 chars) for an identity entry.
    /// View points into a non-secret cache stored on the Entry next
    /// to the LockedKey; lifetime is until `remove()` or KeyStore
    /// dtor.  Pubkeys are non-secret — fine to pass / log / copy
    /// (HEP-CORE-0036 §I10 + HEP-CORE-0040 §8.5.2).
    /// Throws `std::out_of_range` if `name` is absent or refers to a
    /// raw entry (use `lookup_raw` for HEP-0038 secrets).
    [[nodiscard]] std::string_view pubkey(std::string_view name) const;

    /// Invoke `use` with the **RAW 32-byte SECRET key**
    /// (`crypto_box_SECRETKEYBYTES`) for an identity entry, per
    /// HEP-CORE-0040 §8.5.2 canonical contract.  View is valid ONLY
    /// inside `use`; storing it past return is undefined behaviour.
    /// Bytes live in the LockedKey region; the security module never
    /// materializes a std::string copy.
    ///
    /// **Use this for every libsodium / AttachProtocol consumer.**
    /// If your downstream API requires Z85 (vault round-trip, log
    /// display, libzmq Z85 socket option set-as-string), use
    /// `with_seckey_z85` instead — it encodes raw → Z85 on the fly
    /// into a stack buffer that is sodium_memzero'd before return.
    ///
    /// Shared lock is held for the callback's duration — concurrent
    /// `with_seckey` / `pubkey` / `lookup_raw` calls run in parallel,
    /// but a concurrent `remove(name)` waits.  Callback MUST be
    /// prompt (microseconds): no blocking I/O, no syscalls beyond
    /// what's needed to consume the bytes (typically a single
    /// `crypto_box_easy(...)` call).
    ///
    /// Throws `std::out_of_range` if `name` is absent or refers to a
    /// raw entry.  Rethrows anything thrown by `use` after releasing
    /// the shared lock.
    void with_seckey(std::string_view                          name,
                     std::function<void(std::string_view)>     use) const;

    /// Invoke `use` with the **Z85 SECRET key (40 ASCII chars)** for
    /// an identity entry — the on-disk / on-the-wire form per
    /// HEP-CORE-0040 §8.5.2.  Encoded on-the-fly from the raw bytes
    /// in LockedKey into a stack buffer; the buffer is
    /// `sodium_memzero`'d before this function returns (both on
    /// normal exit and on exceptions from `use`).
    ///
    /// **Prefer `with_seckey` (raw) and encode to Z85 at the wire
    /// boundary.**  This accessor exists only for the rare downstream
    /// API that REQUIRES Z85 input (e.g., vault re-serialize, libzmq
    /// `curve_secretkey` Z85-string set, human-readable log lines).
    ///
    /// Same threading semantics as `with_seckey`.
    void with_seckey_z85(std::string_view                          name,
                          std::function<void(std::string_view)>     use) const;

    /// Combined keypair accessor — invokes `use(pubkey_z85, seckey_z85)`
    /// with BOTH halves of an identity entry, encoded as Z85.  One
    /// entry lookup, one lock acquisition — replaces the common
    /// `pubkey(name)` + `with_seckey_z85(name, ...)` pair (test +
    /// production sites that need both halves of a CURVE keypair for a
    /// ZMQ socket setup or AttachProtocol challenge).  Same lifetime +
    /// threading semantics as `with_seckey_z85`: both views are valid
    /// only inside `use`; the seckey buffer is sodium_memzero'd before
    /// this function returns.  Throws `std::out_of_range` if `name` is
    /// absent or refers to a raw entry.
    void with_keypair_z85(
        std::string_view name,
        std::function<void(std::string_view /*pubkey*/,
                           std::string_view /*seckey*/)> use) const;

    /// HEP-0038 raw-secret access (`api.vault_load`).  Span lifetime
    /// is until `remove()` or KeyStore dtor; script bindings MUST
    /// materialize the bytes into a script-owned buffer before
    /// returning to the script, not pass the span to script code.
    /// Throws `std::out_of_range` if `name` is absent.
    [[nodiscard]] std::span<const std::byte>
    lookup_raw(std::string_view name) const;

    /// Existence check (tests; production uses `pubkey()` /
    /// `with_seckey()` and lets the throw signal).
    [[nodiscard]] bool has(std::string_view name) const noexcept;

    /// Number of stored secrets.  Snapshot under shared lock.
    [[nodiscard]] std::size_t size() const noexcept;

private:
    /// Private default ctor — F8 discipline.  KeyStore is a MEMBER
    /// of `SecureSubsystem::Impl` (HEP-CORE-0043 §2.2); the ONLY
    /// construction path is `SecureSubsystem::Impl`'s member-init
    /// list.  External code MUST access the process's instance via
    /// `secure().keys()`.  Making the ctor private enforces this
    /// at compile time.
    KeyStore();

    /// `SecureSubsystem::Impl` needs access to the private ctor
    /// for its `KeyStore keys_;` member-init.  Sole friend.
    friend struct SecureSubsystem::Impl;

    /// Implementation state — opaque pImpl.  Struct definition
    /// lives in the `.cpp`.  Same pattern as
    /// `pylabhub::utils::ThreadManager::Impl`.
    struct Impl;

    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils::security
