#pragma once
/**
 * @file secure_memory_subsystem.hpp
 * @brief Process-wide platform setup for locked-memory secret storage
 *        (HEP-CORE-0040 §4).
 *
 * Public class + guarded global accessor.  Exactly one instance per OS
 * process; consumer access is the class's own concern via the namespace
 * accessor below, NOT via LifecycleManager (which has no instance-
 * retrieval API — see HEP-CORE-0001 + HEP-CORE-0040 §4.6).
 *
 * Lifecycle: ctor registers a dynamic lifecycle module named
 * `"SecureMemory"` with dependency on `"pylabhub::utils::Logger"`;
 * startup/shutdown thunks are no-ops (the real work runs in ctor/dtor
 * — LifecycleGuard provides ORDERING ONLY).  Mirrors the ThreadManager
 * pattern (HEP-CORE-0031 §3).
 *
 * Construction must happen before any `KeyStore` is constructed
 * (HEP-CORE-0040 §4.5).  Standard call site: very early in
 * `plh_role` / `plh_hub` `main()`, immediately after the Logger module
 * is up, before vault open.
 */

#include "pylabhub_utils_export.h"

#include <cstddef>
#include <memory>
#include <span>

namespace pylabhub::utils::security
{

class PYLABHUB_UTILS_EXPORT SecureMemorySubsystem
{
public:
    /// Runs the HEP-CORE-0040 §4.2 startup steps in ctor:
    ///   1. Disable core dumps (setrlimit(RLIMIT_CORE, 0) + prctl
    ///      PR_SET_DUMPABLE on Linux; SetErrorMode +
    ///      WerAddExcludedApplication on Windows).  Fatal on failure.
    ///   2. Inspect mlock capability (getrlimit(RLIMIT_MEMLOCK) on
    ///      POSIX; SeLockMemoryPrivilege on Windows).  WARN on low.
    ///   3. Register dynamic lifecycle module `"SecureMemory"` with
    ///      dependency `"pylabhub::utils::Logger"`; thunks no-op.
    ///
    /// Throws `std::logic_error` if a SecureMemorySubsystem has
    /// already been constructed in this process — exactly one
    /// instance per OS process.
    /// Throws `std::runtime_error` if disable_core_dumps fails
    /// (errno included in the message).
    SecureMemorySubsystem();

    /// Deregisters the lifecycle module.  Does NOT re-enable core
    /// dumps — irreversible by design (HEP-CORE-0040 §4.4).
    /// Defined in `.cpp` so `Impl` is complete at the dtor site
    /// (canonical pImpl per `docs/IMPLEMENTATION_GUIDANCE.md
    /// §"pImpl Idiom"`).
    ~SecureMemorySubsystem();

    SecureMemorySubsystem(const SecureMemorySubsystem &)            = delete;
    SecureMemorySubsystem &operator=(const SecureMemorySubsystem &) = delete;
    SecureMemorySubsystem(SecureMemorySubsystem &&)                 = delete;
    SecureMemorySubsystem &operator=(SecureMemorySubsystem &&)      = delete;

    /// Implementation state. Declared public so the free-function lifecycle
    /// thunks (in `secure_memory_subsystem.cpp`) can dispatch against it.
    /// Still opaque — struct definition lives in the `.cpp` — so callers
    /// cannot access fields directly.  Same pattern as
    /// `pylabhub::utils::ThreadManager::Impl` (HEP-CORE-0031).
    struct Impl;

    /// Non-throwing accessor — returns true iff `sodium_init()` was
    /// called and returned >= 0 during this SMS's construction.  Used
    /// by the free-function `sodium_ready()` probe.
    [[nodiscard]] bool sodium_initialized() const noexcept;

    // ─────────────────────────────────────────────────────────────────
    // SEC-Fold-2 wrapper API (HEP-CORE-0043 §2.1) — libsodium boundary.
    // Every direct sodium primitive in the codebase migrates to these
    // wrappers.  Nothing outside this module `#include <sodium.h>`
    // once migration completes.
    //
    // Contract: every method gates on `sodium_ready()` and PANICS
    // (`PLH_PANIC` → process abort) if false.  Calling any of these
    // before SecureMemorySubsystem is constructed is a programmer
    // error — a violation of the module's singularity+init contract.
    // Same pattern as FileLock and Logger; not a recoverable
    // exception.
    // ─────────────────────────────────────────────────────────────────

    /// Fill `out` with cryptographically-secure random bytes.
    /// Wrapper for libsodium `randombytes_buf`.  Panics if
    /// `sodium_ready()` is false.
    void random_bytes(std::span<std::byte> out);

    /// Constant-time memory compare — replaces `sodium_memcmp`.
    /// Returns true iff spans are equal length AND byte-equal.
    /// Panics if `sodium_ready()` is false.
    [[nodiscard]] bool memcmp_ct(std::span<const std::byte> a,
                                  std::span<const std::byte> b);

    /// Zero a memory region such that the compiler cannot optimize it
    /// away — replaces `sodium_memzero`.  Panics if `sodium_ready()`
    /// is false.
    void memzero(std::span<std::byte> region);

private:
    std::unique_ptr<Impl> pImpl;
};

/// HEP-CORE-0043 §2.1 — public class name for SEC-Fold-2.  For the
/// transition window we keep both names in play; `SecureSubsystem` is
/// the target name (see HEP-0043 §1.1 Nature), `SecureMemorySubsystem`
/// is the historical name (HEP-CORE-0040 §4.1).  Full rename in a
/// follow-up commit once all callers migrate.
using SecureSubsystem = SecureMemorySubsystem;

/// Global access — guarded singleton accessor (HEP-CORE-0040 §4.6).
/// Returns a reference to the process's `SecureMemorySubsystem`
/// instance.  Throws `std::runtime_error` if not yet constructed.
[[nodiscard]] PYLABHUB_UTILS_EXPORT SecureMemorySubsystem &
secure_memory_subsystem();

/// Non-throwing probe — true once a SecureMemorySubsystem has been
/// constructed (and not yet destructed).  Used by `KeyStore`'s ctor
/// to fail fast on ordering violations without exception machinery.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool
secure_memory_subsystem_ready() noexcept;

/// Non-throwing probe — true iff SMS is constructed AND `sodium_init()`
/// has completed successfully.  This is the gate every consumer of
/// libsodium (`KeyStore`, `AttachProtocol`, `vault_crypto`, `uuid_utils`,
/// ...) MUST check before calling any libsodium function.  No consumer
/// may call `sodium_init()` on its own — it's the SMS module's job,
/// and only its job.  If this returns false, libsodium calls have
/// undefined behavior; throw a clear runtime_error or PANIC.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool
sodium_ready() noexcept;

/// HEP-CORE-0043 §2.1 — canonical accessor for the module's public
/// API.  Alias for `secure_memory_subsystem()` during the SEC-Fold-2
/// transition; will BECOME the primary accessor after all consumers
/// migrate off `secure_memory_subsystem()`.  Throws if not
/// constructed (same contract).
[[nodiscard]] PYLABHUB_UTILS_EXPORT SecureSubsystem &
secure();

} // namespace pylabhub::utils::security
