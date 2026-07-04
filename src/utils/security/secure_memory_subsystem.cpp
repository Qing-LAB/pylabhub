/**
 * @file secure_memory_subsystem.cpp
 * @brief Implementation of HEP-CORE-0040 §4 SecureMemorySubsystem.
 *
 * See `src/include/utils/security/secure_memory_subsystem.hpp` for the
 * public surface and HEP-CORE-0040 §4 for the design.
 */
#include "utils/security/secure_memory_subsystem.hpp"

#include "utils/debug_info.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"

#include <sodium.h>

#ifndef _WIN32
#  include <unistd.h>  // getpid — used in the SodiumInit event log line
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#  include <sys/resource.h>          // setrlimit / getrlimit / RLIMIT_*
#  ifdef __linux__
#    include <sys/prctl.h>           // prctl / PR_SET_DUMPABLE
#  endif
#else
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>               // SetErrorMode
#  include <werapi.h>                // WerAddExcludedApplication
#  pragma comment(lib, "wer.lib")
#endif

namespace pylabhub::utils::security
{

// ============================================================================
// File-scope singleton state (HEP-CORE-0043 §1.2 + §1.3)
// ============================================================================
//
// Three-state atomic lifecycle flag, same shape as Logger's
// `g_logger_state` (logger.cpp:59) — the codebase's canonical pattern
// for a static global module's init-visibility contract:
//
//   Uninitialized  → default; no ctor has run yet.
//   InitCalled     → ctor entered, work in progress, not yet safe.
//                    Any consumer that catches this state is looking
//                    at a half-constructed subsystem — must not
//                    proceed.
//   Initialized    → ctor complete, all invariants hold, `g_sms`
//                    published under release fence.  This is the ONLY
//                    state where consumer API calls may proceed.
//   ShuttingDown   → dtor entered, teardown in progress.
//   Shutdown       → dtor complete.
//
// Idempotency: the CAS from Uninitialized→InitCalled in the ctor
// guarantees exactly one initialization sequence per process.  Any
// concurrent or subsequent construction attempt sees a non-
// Uninitialized state and PANICs (HEP-CORE-0043 §1.3 singularity).
//
// Memory ordering: writes prior to the release store of Initialized
// happen-before any consumer's acquire load of Initialized — so
// `g_sms` (plain pointer, written before the release) is visible to
// any thread that observes the Initialized state.  Matches Logger's
// acquire/release discipline.

enum class SmsState : std::uint8_t
{
    Uninitialized,
    InitCalled,
    Initialized,
    ShuttingDown,
    Shutdown
};

namespace
{
std::atomic<SmsState>  g_sms_state{SmsState::Uninitialized};
SecureMemorySubsystem *g_sms = nullptr;  // Published under Initialized release fence.

// Module name registered with LifecycleManager.  Singleton — no
// scope/uid suffix because exactly one SecureMemorySubsystem exists
// per OS process (HEP-CORE-0043 §1.3).
constexpr const char *kModuleName = "SecureMemory";

// RLIMIT_MEMLOCK threshold below which we WARN.  256 KiB chosen so a
// process that needs to lock a few 32-byte CURVE keypairs has ample
// headroom; sandboxed environments below this bar typically cannot
// host pylabhub anyway.
constexpr unsigned long kMemlockWarnThresholdBytes = 256UL * 1024UL;
} // namespace

// ============================================================================
// Impl (pImpl)
// ============================================================================

struct SecureMemorySubsystem::Impl
{
    bool lifecycle_registered = false;
    /// The rc returned by `sodium_init()`; 0 = first init, 1 = already
    /// initialized (both success).  Kept for diagnostic use only —
    /// the load-bearing "is init successful" flag is the global
    /// `g_sms_state == Initialized` (HEP-CORE-0043 §1.2), NOT this
    /// per-instance field.
    int sodium_init_rc = -1;
};

// ============================================================================
// Lifecycle bridge — static thunks called by LifecycleManager
// ============================================================================

namespace
{

/// Validator runs from LifecycleManager's async shutdown thread.  Read
/// the atomic state under acquire ordering; g_sms is safe to observe
/// only while state == Initialized (release/acquire fence).
bool sms_impl_validate(void * /*userdata*/, uint64_t /*key*/) noexcept
{
    return g_sms_state.load(std::memory_order_acquire) == SmsState::Initialized;
}

/// Startup thunk: no-op.  Real work runs in the SecureMemorySubsystem
/// ctor (HEP-CORE-0040 §4.2); the lifecycle module exists to
/// participate in startup ORDERING (HEP-CORE-0001 dependency graph),
/// not to perform work.
void sms_startup(const char * /*arg*/, void * /*userdata*/)
{
}

/// Shutdown thunk: no-op.  Core dumps stay disabled until process
/// exit by design (HEP-CORE-0040 §4.4); there is nothing to unwind.
void sms_shutdown(const char * /*arg*/, void * /*userdata*/)
{
}

// ----------------------------------------------------------------------------
// Platform startup primitives (HEP-CORE-0040 §4.2)
// ----------------------------------------------------------------------------

/// Step 3 platform hardening: disable core dumps.  PANICs on fatal
/// platform failure — static global module contract, no recovery
/// path (matches sodium_init failure discipline; HEP-CORE-0043 §1.2).
void disable_core_dumps_or_panic()
{
#ifndef _WIN32
    // POSIX: setrlimit(RLIMIT_CORE, 0).  Forbids the kernel from
    // writing a core file on this process's behalf.
    struct ::rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    if (::setrlimit(RLIMIT_CORE, &rl) != 0)
    {
        const int saved_errno = errno;
        PLH_PANIC(
            "FATAL: SecureMemorySubsystem: setrlimit(RLIMIT_CORE, 0) failed "
            "(errno={}: {}).  Aborting. (HEP-CORE-0043 §1.2)",
            saved_errno, std::strerror(saved_errno));
    }

#  ifdef __linux__
    // Linux defence-in-depth: prctl(PR_SET_DUMPABLE, 0) also blocks
    // ptrace from non-root attackers and is honoured by the kernel
    // even if some other code path attempts setrlimit(RLIMIT_CORE)
    // back up.
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
    {
        const int saved_errno = errno;
        PLH_PANIC(
            "FATAL: SecureMemorySubsystem: prctl(PR_SET_DUMPABLE, 0) failed "
            "(errno={}: {}).  Aborting. (HEP-CORE-0043 §1.2)",
            saved_errno, std::strerror(saved_errno));
    }
#  endif // __linux__
#else  // _WIN32
    // Windows: suppress the Windows Error Reporting crash dialog +
    // minidump generation for this process.
    UINT old_mode = ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    ::SetErrorMode(old_mode | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    // Belt-and-braces: explicitly tell WER not to collect dumps for
    // this binary.  Best-effort — failure here is logged elsewhere.
    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0)
    {
        (void)::WerAddExcludedApplication(exe_path, FALSE);
    }
#endif // _WIN32
}

/// Step 2: inspect mlock capability.  Returns the current
/// RLIMIT_MEMLOCK soft limit (or 0 on Windows / on unknown).  WARN
/// emission is the caller's responsibility.
unsigned long inspect_memlock_capability() noexcept
{
#ifndef _WIN32
    struct ::rlimit rl{};
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
    {
        return static_cast<unsigned long>(rl.rlim_cur);
    }
    return 0UL;
#else
    // Windows: SeLockMemoryPrivilege adjustment is non-trivial and
    // operator-driven (Local Security Policy).  Probing it correctly
    // requires AdjustTokenPrivileges + LookupPrivilegeValue.  For
    // this initial impl we WARN unconditionally on Windows; refined
    // probe is a HEP-CORE-0040 follow-on.
    return 0UL;
#endif
}

} // namespace

// ============================================================================
// SecureMemorySubsystem
// ============================================================================

SecureMemorySubsystem::SecureMemorySubsystem()
    : pImpl(std::make_unique<Impl>())
{
    // ── Step 1: singularity claim + mid-init marker ──────────────────
    //
    // CAS from Uninitialized → InitCalled.  Exactly one thread can
    // succeed (HEP-CORE-0043 §1.3).  Any other observed state
    // (InitCalled / Initialized / ShuttingDown / Shutdown) means
    // either a concurrent ctor is running or the singleton was
    // already constructed — both are broken-contract programmer
    // errors, PANIC and abort.
    //
    // Happens FIRST so a second-construction attempt bails out
    // immediately, before doing any expensive platform work.
    {
        SmsState expected = SmsState::Uninitialized;
        if (!g_sms_state.compare_exchange_strong(
                expected,
                SmsState::InitCalled,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            PLH_PANIC(
                "FATAL: SecureMemorySubsystem ctor invoked while another "
                "instance is present or in-flight (observed state={}). "
                "Exactly one instance per process (HEP-CORE-0043 §1.3). "
                "Aborting.",
                static_cast<int>(expected));
        }
    }

    // Step 2: initialize libsodium (2026-07-04 CI-triaged fix).
    //
    // Every subsequent libsodium call (KeyStore's `sodium_malloc`,
    // AttachProtocol's `crypto_box_*`, `sodium_memzero` etc.) requires
    // `sodium_init()` to have run first — it seeds the CSPRNG and
    // sets `page_size` used by the guarded allocator's pointer math.
    //
    // Historical: `sodium_init` was ONLY called from AttachProtocol's
    // `ensure_sodium_init` (attach_protocol.cpp:310).  KeyStore-only
    // test workers (e.g. `KeyStoreTest.AddRaw_LookupRawRoundtrip`)
    // never touched AttachProtocol, so init never ran; guard-page
    // arithmetic in `_sodium_malloc` ran against zero `page_size` and
    // fired the internal assertion
    // `_unprotected_ptr_from_user_ptr(user_ptr) == unprotected_ptr`.
    // Locally the test happened to work because LTO/linker order in
    // the local build pulled AttachProtocol's static init in as a
    // side effect; CI's linker order didn't.  Undefined-behaviour
    // class visible only under different build flags — thanks to the
    // 2026-07-04 reviewer for the exit-code-134 = abort() catch.
    //
    // Idempotent: `sodium_init()` returns 0 on first call, 1 if
    // already initialized (both non-error).  Only < 0 is fatal.
    const int sodium_rc = ::sodium_init();
    LOGGER_INFO(
        "[SMS] event=SodiumInit rc={} libsodium_ver={}.{} pid={}",
        sodium_rc,
        ::sodium_library_version_major(),
        ::sodium_library_version_minor(),
        static_cast<int>(::getpid()));
    if (sodium_rc < 0)
    {
        // Static-global-module contract failure: CSPRNG unavailable →
        // process cannot proceed.  Same discipline as FileLock / Logger:
        // PANIC, do not throw.  The g_sms_state remains at InitCalled
        // for postmortem visibility.
        PLH_PANIC(
            "FATAL: SecureMemorySubsystem: sodium_init() returned {}. "
            "CSPRNG unavailable (check /dev/urandom / getrandom on POSIX; "
            "system entropy on Windows).  Aborting. (HEP-CORE-0043 §1.2)",
            sodium_rc);
    }
    pImpl->sodium_init_rc = sodium_rc;

    // Step 3: disable core dumps.  PANICs on fatal platform failure
    // (same discipline as sodium_init above — static global module
    // contract failure).
    disable_core_dumps_or_panic();

    // Step 4: inspect mlock capability + WARN if low.
    const unsigned long memlock_limit = inspect_memlock_capability();
    if (memlock_limit > 0 && memlock_limit < kMemlockWarnThresholdBytes)
    {
        LOGGER_WARN(
            "[SecureMemorySubsystem] RLIMIT_MEMLOCK soft limit is {} bytes — "
            "below the {} byte advisory threshold.  KeyStore allocations "
            "may fail with ENOMEM.  Raise the limit (ulimit -l on POSIX) "
            "for hardened deployments.",
            memlock_limit, kMemlockWarnThresholdBytes);
    }
#ifdef _WIN32
    // See inspect_memlock_capability — refined Windows probe pending.
    LOGGER_WARN(
        "[SecureMemorySubsystem] Windows SeLockMemoryPrivilege probe is not "
        "yet implemented; if KeyStore allocations fail, grant "
        "SeLockMemoryPrivilege via secpol.msc -> Local Policies -> User "
        "Rights Assignment.");
#endif

    // Step 5: publish the instance pointer + transition to Initialized.
    //
    // Order matters: the plain write to `g_sms` happens BEFORE the
    // release store on `g_sms_state`.  Any thread that observes
    // Initialized via an acquire load will therefore see the
    // published pointer (C++ memory model synchronizes-with edge).
    //
    // From this point on, consumers reading `sodium_ready()` /
    // `secure()` are guaranteed a fully constructed subsystem.
    g_sms = this;
    g_sms_state.store(SmsState::Initialized, std::memory_order_release);

    // Step 6: register lifecycle module (best-effort — ordering only,
    // not required for the module to be usable).
    try
    {
        ModuleDef mod(kModuleName, pImpl.get(), sms_impl_validate);
        mod.add_dependency("pylabhub::utils::Logger");
        mod.set_startup(sms_startup);
        mod.set_shutdown(sms_shutdown, std::chrono::milliseconds{100});
        mod.set_owner_managed_teardown(true);
        if (LifecycleManager::instance().register_dynamic_module(std::move(mod)))
        {
            (void)LoadModule(kModuleName);
            pImpl->lifecycle_registered = true;
        }
        else
        {
            LOGGER_WARN(
                "[SecureMemorySubsystem] lifecycle module registration "
                "returned false — continuing without lifecycle integration");
        }
    }
    catch (const std::exception &e)
    {
        // Non-fatal: SecureMemorySubsystem is still functional without
        // lifecycle integration (KeyStore's runtime probe in
        // secure_memory_subsystem_ready() will still succeed).  Match
        // ThreadManager's tolerance for early-startup registration
        // failures.
        LOGGER_WARN(
            "[SecureMemorySubsystem] lifecycle registration threw: {} — "
            "continuing without lifecycle integration", e.what());
    }
}

SecureMemorySubsystem::~SecureMemorySubsystem()
{
    // Step 1: publish the ShuttingDown transition FIRST.  Any concurrent
    // consumer's acquire load of Initialized happens-before this
    // release store; any subsequent load sees ShuttingDown and the
    // gate (`sodium_ready()` → false) closes.  Callers can NO
    // LONGER reach into g_sms after this point.
    g_sms_state.store(SmsState::ShuttingDown, std::memory_order_release);

    // Step 2: clear pointer — safe now that the gate is closed.
    // If a stale caller races past the state check on an older core's
    // cache, they'd see either the still-valid `this` (before dtor
    // runs member deinit) or the nullptr.  Neither races with dtor
    // internals because the state gate on the accessor already
    // rejected them.
    if (g_sms == this)
    {
        g_sms = nullptr;
    }

    // Step 3: deregister lifecycle module (best-effort — dtors must
    // not throw, so swallow lifecycle-layer exceptions).
    if (pImpl && pImpl->lifecycle_registered)
    {
        try
        {
            (void)LifecycleManager::instance().unload_module(
                kModuleName, std::source_location::current());
        }
        catch (...)
        {
        }
    }

    // Step 4: terminal state.  Any subsequent gate check sees Shutdown
    // and PANICs (via the accessor / wrapper panic path).
    g_sms_state.store(SmsState::Shutdown, std::memory_order_release);
}

// ============================================================================
// Namespace accessors
// ============================================================================

SecureMemorySubsystem &secure_memory_subsystem()
{
    // Gate: only Initialized is a passable state.  Any other observed
    // state (Uninitialized / InitCalled / ShuttingDown / Shutdown) is
    // a broken-contract programmer error — PANIC, matches FileLock +
    // Logger discipline (HEP-CORE-0043 §1.2).
    const auto state = g_sms_state.load(std::memory_order_acquire);
    if (state != SmsState::Initialized)
    {
        PLH_PANIC(
            "FATAL: secure_memory_subsystem() called with SMS in state={} "
            "(expected Initialized).  Construct SecureMemorySubsystem in "
            "main() via LifecycleGuard BEFORE any consumer.  Aborting. "
            "(HEP-CORE-0043 §1.2)",
            static_cast<int>(state));
    }
    return *g_sms;
}

bool secure_memory_subsystem_ready() noexcept
{
    return g_sms_state.load(std::memory_order_acquire) == SmsState::Initialized;
}

bool SecureMemorySubsystem::sodium_initialized() const noexcept
{
    // Global atomic is the source of truth; the per-instance rc is
    // diagnostic-only.  This method matches Logger's
    // `lifecycle_initialized()`: reads the atomic, no PANIC (probe).
    return g_sms_state.load(std::memory_order_acquire) == SmsState::Initialized;
}

bool sodium_ready() noexcept
{
    return g_sms_state.load(std::memory_order_acquire) == SmsState::Initialized;
}

SecureSubsystem &secure()
{
    // Delegates to the panic-gated accessor; a broken contract
    // aborts here just as if the caller had used the long name.
    return secure_memory_subsystem();
}

// ─────────────────────────────────────────────────────────────────────
// SEC-Fold-2 wrapper API implementations (HEP-CORE-0043 §2.1)
//
// Every wrapper gates on `sodium_ready()` first, then delegates to
// libsodium.  A wrapper called before SecureMemorySubsystem is
// constructed is a PROGRAMMER ERROR — the caller violated the
// module's singularity+init contract.  Same pattern as FileLock and
// Logger: `PLH_PANIC` aborts the process.  We do NOT throw an
// exception here — this is not a recoverable failure, it's a broken
// static-module contract and the program cannot proceed safely.
// ─────────────────────────────────────────────────────────────────────

namespace
{
// Shared gate: passes only when g_sms_state == Initialized (acquire
// load).  Any other state — Uninitialized, InitCalled (ctor in
// flight), ShuttingDown, Shutdown — is a broken-contract programmer
// error, PANIC.  Mirror of Logger's `logger_is_loggable`
// (logger.cpp:62) adapted to SMS's three-state lifecycle.
inline void panic_if_not_ready(const char *method_name)
{
    const auto state = g_sms_state.load(std::memory_order_acquire);
    if (state != SmsState::Initialized)
    {
        PLH_PANIC("FATAL: SecureSubsystem::{}() called with SMS in state={} "
                  "(expected Initialized).  Aborting. (HEP-CORE-0043 §1.2)",
                  method_name, static_cast<int>(state));
    }
}
} // anonymous namespace

void SecureSubsystem::random_bytes(std::span<std::byte> out)
{
    panic_if_not_ready("random_bytes");
    if (out.empty()) return;
    ::randombytes_buf(out.data(), out.size());
}

bool SecureSubsystem::memcmp_ct(std::span<const std::byte> a,
                                 std::span<const std::byte> b)
{
    panic_if_not_ready("memcmp_ct");
    if (a.size() != b.size()) return false;
    if (a.empty())            return true;
    return ::sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

void SecureSubsystem::memzero(std::span<std::byte> region)
{
    panic_if_not_ready("memzero");
    if (region.empty()) return;
    ::sodium_memzero(region.data(), region.size());
}

} // namespace pylabhub::utils::security
