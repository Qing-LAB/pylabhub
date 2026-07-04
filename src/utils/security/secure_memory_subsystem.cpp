/**
 * @file secure_memory_subsystem.cpp
 * @brief Implementation of HEP-CORE-0040 §4 SecureMemorySubsystem.
 *
 * See `src/include/utils/security/secure_memory_subsystem.hpp` for the
 * public surface and HEP-CORE-0040 §4 for the design.
 */
#include "utils/security/secure_memory_subsystem.hpp"

#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"

#include <sodium.h>

#ifndef _WIN32
#  include <unistd.h>  // getpid — used in the SodiumInit event log line
#endif

#include <cerrno>
#include <cstring>
#include <mutex>
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
// File-scope singleton accessor state (HEP-CORE-0040 §4.6)
// ============================================================================

namespace
{
std::mutex             g_sms_mu;
SecureMemorySubsystem *g_sms = nullptr;

// Module name registered with LifecycleManager.  Singleton — no
// scope/uid suffix because exactly one SecureMemorySubsystem exists
// per OS process (HEP-CORE-0040 §4.1).
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
    /// True iff `sodium_init()` was called AND returned >= 0 during
    /// this SMS's construction.  Gated by `sodium_ready()`; every
    /// libsodium consumer MUST check.
    bool sodium_initialized = false;
};

// ============================================================================
// Lifecycle bridge — static thunks called by LifecycleManager
// ============================================================================

namespace
{

/// Validator runs from LifecycleManager's async shutdown thread.  We
/// check the file-scope singleton pointer (the lifetime guarantee
/// `secure_memory_subsystem()` provides) — userdata is unused.
bool sms_impl_validate(void * /*userdata*/, uint64_t /*key*/) noexcept
{
    std::lock_guard<std::mutex> lk(g_sms_mu);
    return g_sms != nullptr;
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

/// Step 1: disable core dumps.  Throws std::runtime_error on fatal
/// platform failure (POSIX setrlimit, Linux prctl, Windows SetErrorMode).
void disable_core_dumps_or_throw()
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
        throw std::runtime_error(
            "SecureMemorySubsystem: setrlimit(RLIMIT_CORE, 0) failed (errno="
            + std::to_string(saved_errno) + ": "
            + std::strerror(saved_errno) + ")");
    }

#  ifdef __linux__
    // Linux defence-in-depth: prctl(PR_SET_DUMPABLE, 0) also blocks
    // ptrace from non-root attackers and is honoured by the kernel
    // even if some other code path attempts setrlimit(RLIMIT_CORE)
    // back up.
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
    {
        const int saved_errno = errno;
        throw std::runtime_error(
            "SecureMemorySubsystem: prctl(PR_SET_DUMPABLE, 0) failed (errno="
            + std::to_string(saved_errno) + ": "
            + std::strerror(saved_errno) + ")");
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
    // Step 0: initialize libsodium (2026-07-04 CI-triaged fix).
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
        throw std::runtime_error(
            "SecureMemorySubsystem: sodium_init() failed — libsodium "
            "will not be usable.  Check CSPRNG availability "
            "(/dev/urandom / getrandom); HEP-CORE-0040 §4.0.");
    }
    pImpl->sodium_initialized = true;

    // Step 1: disable core dumps.  Throws on fatal platform failure.
    disable_core_dumps_or_throw();

    // Step 2: inspect mlock capability + WARN if low.
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

    // Step 3: claim singleton + register lifecycle module.
    // Singleton claim happens BEFORE lifecycle registration so a second
    // SecureMemorySubsystem ctor fails fast without polluting the
    // LifecycleManager registry.
    {
        std::lock_guard<std::mutex> lk(g_sms_mu);
        if (g_sms != nullptr)
        {
            throw std::logic_error(
                "SecureMemorySubsystem: already constructed (HEP-CORE-0040 "
                "§4.1 — exactly one instance per process)");
        }
        g_sms = this;
    }

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
    // Clear singleton pointer FIRST — any subsequent
    // secure_memory_subsystem_ready() returns false.
    {
        std::lock_guard<std::mutex> lk(g_sms_mu);
        if (g_sms == this)
            g_sms = nullptr;
    }

    // Deregister lifecycle module (best-effort).
    if (pImpl && pImpl->lifecycle_registered)
    {
        try
        {
            (void)LifecycleManager::instance().unload_module(
                kModuleName, std::source_location::current());
        }
        catch (...)
        {
            // Dtors must not throw; lifecycle layer logs internally.
        }
    }
}

// ============================================================================
// Namespace accessors
// ============================================================================

SecureMemorySubsystem &secure_memory_subsystem()
{
    std::lock_guard<std::mutex> lk(g_sms_mu);
    if (g_sms == nullptr)
    {
        throw std::runtime_error(
            "secure_memory_subsystem(): SecureMemorySubsystem has not been "
            "constructed (HEP-CORE-0040 §4.6).  Construct it in main() "
            "BEFORE the first vault open / KeyStore construction.");
    }
    return *g_sms;
}

bool secure_memory_subsystem_ready() noexcept
{
    std::lock_guard<std::mutex> lk(g_sms_mu);
    return g_sms != nullptr;
}

bool SecureMemorySubsystem::sodium_initialized() const noexcept
{
    return pImpl != nullptr && pImpl->sodium_initialized;
}

bool sodium_ready() noexcept
{
    std::lock_guard<std::mutex> lk(g_sms_mu);
    return g_sms != nullptr && g_sms->sodium_initialized();
}

SecureSubsystem &secure()
{
    return secure_memory_subsystem();
}

// ─────────────────────────────────────────────────────────────────────
// SEC-Fold-2 wrapper API implementations (HEP-CORE-0043 §2.1)
//
// Each wrapper asserts sodium_initialized() first, then delegates to
// the raw libsodium primitive.  Consumers do not check the gate —
// they call `secure().X()` and get a runtime_error if SMS hasn't been
// constructed, matching the "gate at the module boundary" contract
// (HEP-0043 §1.2).
// ─────────────────────────────────────────────────────────────────────

namespace
{
// Shared gate check used by every wrapper.  Throws with a specific
// message naming the wrapper method so the failure trace is
// unambiguous.
inline void check_sodium_ready_or_throw(const char *method_name)
{
    if (!sodium_ready())
    {
        throw std::runtime_error(
            std::string{"SecureSubsystem::"} + method_name +
            ": sodium not ready — SecureMemorySubsystem must be "
            "constructed before use (HEP-CORE-0043 §1.2).");
    }
}
} // anonymous namespace

void SecureSubsystem::random_bytes(std::span<std::byte> out)
{
    check_sodium_ready_or_throw("random_bytes");
    ::randombytes_buf(out.data(), out.size());
}

bool SecureSubsystem::memcmp_ct(std::span<const std::byte> a,
                                 std::span<const std::byte> b) noexcept
{
    if (a.size() != b.size()) return false;
    if (a.empty())            return true;
    // sodium_memcmp is constant-time.  Both spans same size at this
    // point; safe to compare.
    return ::sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

void SecureSubsystem::memzero(std::span<std::byte> region) noexcept
{
    if (region.empty()) return;
    ::sodium_memzero(region.data(), region.size());
}

} // namespace pylabhub::utils::security
