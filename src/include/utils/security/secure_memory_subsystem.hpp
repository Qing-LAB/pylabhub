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

#include <memory>

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

private:
    std::unique_ptr<Impl> pImpl;
};

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

} // namespace pylabhub::utils::security
