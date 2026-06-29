#pragma once
/**
 * @file key_store.hpp
 * @brief Process-wide locked-memory secret store (HEP-CORE-0040 §5).
 *
 * Public class + guarded global accessor.  Exactly one instance per OS
 * process; consumer access is via the namespace accessor below, NOT via
 * LifecycleManager (which has no instance-retrieval API — see
 * HEP-CORE-0001 + HEP-CORE-0040 §5.6).
 *
 * Lifecycle: ctor calls `secure_memory_subsystem_ready()` and throws if
 * false (HEP-CORE-0040 §4.5 ordering invariant), then registers a
 * dynamic lifecycle module named `"KeyStore"` with dependencies on
 * `"SecureMemory"` + `"pylabhub::utils::Logger"`; startup/shutdown
 * thunks are no-ops.  Mirrors HEP-CORE-0031 §3 ThreadManager pattern.
 *
 * Use-not-export design (HEP-CORE-0040 §5.2 + §8.2, round-5 2026-06-06):
 * the security module exposes OPERATIONS on secret material, not byte
 * exports.  Public-half keys (pubkey) return as `std::string_view`
 * into LockedKey-owned bytes.  Secret-half keys (seckey) are accessed
 * only via `with_seckey(name, callback)` — bytes never leave the
 * LockedKey region; the callback's view parameter is valid only for
 * callback scope.
 *
 * Internally owns a name → `LockedKey` map under a shared mutex.  All
 * `LockedKey` instances are constructed via `sodium_malloc` (mlock +
 * guard pages + canary + auto-wipe); destructed via `sodium_memzero` +
 * `sodium_free`.  See `key_store.cpp` for `LockedKey` definition.
 *
 * Concurrency: `pubkey` / `with_seckey` / `lookup_raw` / `has` / `size`
 * take a shared lock (parallel reads OK); `add_identity` / `add_raw` /
 * `remove` take an exclusive lock.  `with_seckey`'s callback runs
 * under the shared lock — callback MUST be prompt (microseconds, no
 * blocking I/O) — concurrent `remove(name)` waits for the callback to
 * return.  See HEP-CORE-0040 §5.5 for the full thread-safety contract.
 */

#include "pylabhub_utils_export.h"

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
    /// Construct the process's KeyStore.
    ///
    /// @param scope_tag  Owner kind: `"hub"` (in plh_hub) or `"role"`
    ///                   (in plh_role).  Non-empty.
    /// @param owner_id   Owner uid (e.g. `"hub.lab1.uid00000001"`).
    ///                   Non-empty.
    ///
    /// Throws `std::logic_error` if another KeyStore already exists in
    /// this process (HEP-CORE-0040 §5.1 — exactly one per process) or
    /// if `SecureMemorySubsystem` has not been constructed
    /// (HEP-CORE-0040 §4.5).
    /// Throws `std::invalid_argument` if either identity string is empty.
    KeyStore(std::string scope_tag, std::string owner_id);

    /// Destroys all stored LockedKey instances (each `sodium_memzero` +
    /// `sodium_free`) and deregisters the lifecycle module.
    /// Defined in `.cpp` so `Impl` is complete at the dtor site
    /// (canonical pImpl per `IMPLEMENTATION_GUIDANCE §"pImpl Idiom"`).
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

    /// Implementation state.  Declared public so the free-function
    /// lifecycle thunks (in `key_store.cpp`) can dispatch against it.
    /// Still opaque — struct definition lives in the `.cpp`.  Same
    /// pattern as `pylabhub::utils::ThreadManager::Impl`.
    struct Impl;

private:
    std::unique_ptr<Impl> pImpl;
};

/// Global access — guarded singleton accessor (HEP-CORE-0040 §5.6).
/// Returns a reference to the process's `KeyStore` instance.  Throws
/// `std::runtime_error` if not yet constructed.
[[nodiscard]] PYLABHUB_UTILS_EXPORT KeyStore &key_store();

/// Non-throwing probe — true once a KeyStore has been constructed
/// (and not yet destructed).  Used by tests + by code paths that
/// want to dispatch on KeyStore availability without exception
/// machinery.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool key_store_ready() noexcept;

} // namespace pylabhub::utils::security
