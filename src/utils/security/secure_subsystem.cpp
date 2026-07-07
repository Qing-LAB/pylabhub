/**
 * @file secure_subsystem.cpp
 * @brief Implementation of HEP-CORE-0043 §1 + §2 SecureSubsystem.
 *
 * See `src/include/utils/security/secure_subsystem.hpp` for the
 * public surface and HEP-CORE-0043 §1 (contract) + §2 (module
 * shape) for the design.
 */
#include "utils/security/secure_subsystem.hpp"

#include "utils/debug_info.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"
#include "utils/security/key_store.hpp"

#include <sodium.h>

// ── ABI static asserts: sodium constants we've hardcoded in headers
// (secure_subsystem.hpp `kSecretbox*Bytes` + `kPwhashSaltBytes`;
// vault_crypto.hpp `kVault*` KDF params) must match libsodium's
// actual values.  libsodium has held these stable for over a decade;
// if they ever drift, this build breaks loudly with a clear message.
static_assert(crypto_secretbox_KEYBYTES   == 32, "sodium ABI drift: KEYBYTES");
static_assert(crypto_secretbox_NONCEBYTES == 24, "sodium ABI drift: NONCEBYTES");
static_assert(crypto_secretbox_MACBYTES   == 16, "sodium ABI drift: MACBYTES");
static_assert(crypto_pwhash_SALTBYTES     == 16, "sodium ABI drift: SALTBYTES");
static_assert(crypto_box_PUBLICKEYBYTES   == 32, "sodium ABI drift: BOX PK");
static_assert(crypto_box_SECRETKEYBYTES   == 32, "sodium ABI drift: BOX SK");
static_assert(crypto_box_NONCEBYTES       == 24, "sodium ABI drift: BOX NONCE");
static_assert(crypto_box_MACBYTES         == 16, "sodium ABI drift: BOX MAC");
static_assert(
    pylabhub::utils::security::SecureSubsystem::kPwhashSaltBytes
        == crypto_pwhash_SALTBYTES,
    "SMS kPwhashSaltBytes must equal sodium's crypto_pwhash_SALTBYTES");
static_assert(
    pylabhub::utils::security::SecureSubsystem::kBoxPubkeyBytes
        == crypto_box_PUBLICKEYBYTES,
    "SMS kBoxPubkeyBytes must equal sodium's crypto_box_PUBLICKEYBYTES");
static_assert(
    pylabhub::utils::security::SecureSubsystem::kBoxSeckeyBytes
        == crypto_box_SECRETKEYBYTES,
    "SMS kBoxSeckeyBytes must equal sodium's crypto_box_SECRETKEYBYTES");
static_assert(
    pylabhub::utils::security::SecureSubsystem::kBoxNonceBytes
        == crypto_box_NONCEBYTES,
    "SMS kBoxNonceBytes must equal sodium's crypto_box_NONCEBYTES");
static_assert(
    pylabhub::utils::security::SecureSubsystem::kBoxMacBytes
        == crypto_box_MACBYTES,
    "SMS kBoxMacBytes must equal sodium's crypto_box_MACBYTES");

#ifndef _WIN32
#  include <unistd.h>  // getpid — used in the SodiumInit event log line
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>

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
// File-scope module state (HEP-CORE-0043 §1.2 + §1.3)
// ============================================================================
//
// Three-state atomic lifecycle flag, same shape as Logger's
// `g_logger_state` (logger.cpp:59).  Singleton object itself lives
// as a function-local static in `instance()` — no file-scope
// pointer rendezvous.
//
// Memory model — release-acquire discipline (F12):
//   - `bringup()` (below) publishes `Initialized` with release order:
//     every write performed during bringup (sodium_init effects,
//     RLIMIT changes, KeyStore + Crypto ctor-time allocations)
//     happens-before any consumer's acquire load of `Initialized`.
//   - Every accessor (`secure()`, `keys()`, `crypto()`, wrapper
//     methods, `sodium_ready()`, `lifecycle_initialized()`) loads
//     the atomic with acquire order.  Reading `Initialized` means
//     the reader observes all of bringup's writes.
//   - `shutdown_module()` publishes `ShuttingDown` then `Shutdown`
//     with release order.  Callers observing `Shutdown` see the
//     dtor's effects; callers observing `ShuttingDown` see bringup
//     effects (still safe to read pImpl state; not safe to start
//     new operations).

enum class SubsystemState : std::uint8_t
{
    Uninitialized,
    InitCalled,
    Initialized,
    ShuttingDown,
    Shutdown
};

namespace
{
std::atomic<SubsystemState> g_state{SubsystemState::Uninitialized};

// RLIMIT_MEMLOCK threshold below which we WARN.
constexpr unsigned long kMemlockWarnThresholdBytes = 256UL * 1024UL;

// ── Unified state-gate helper (F6) ───────────────────────────────
// One PANIC message template for the "SMS not Initialized" case.
// Every accessor (`secure()`, `keys()`, `crypto()`, wrapper methods)
// routes through this — one code path, one wording, no drift.
[[nodiscard]] SubsystemState load_state() noexcept
{
    return g_state.load(std::memory_order_acquire);
}

void panic_if_not_ready(const char *context)
{
    const auto state = load_state();
    if (state != SubsystemState::Initialized)
    {
        PLH_PANIC(
            "FATAL: SecureSubsystem::{} called with state={} "
            "(expected Initialized).  Add "
            "SecureSubsystem::GetLifecycleModule() to your "
            "LifecycleGuard mods pack BEFORE any consumer.  Aborting. "
            "(HEP-CORE-0043 §1.2)",
            context, static_cast<int>(state));
    }
}

// ── Gate policy (revised 2026-07-07) ────────────────────────────
// The gate protects OUR state (the KeyStore inside `Impl`) — it
// does NOT try to police libsodium's own state.  Only `keys()`
// requires SMS to be `Initialized`.
//
// Every other method is a stateless wrapper on libsodium.  libsodium
// self-initializes on first call for all primitives we wrap
// (verified against libsodium 1.0.18+ behaviour: `randombytes_buf`,
// `crypto_generichash`, `crypto_pwhash`, `crypto_secretbox_easy`,
// `sodium_memzero`, `sodium_memcmp`, `sodium_bin2hex` all handle
// implicit init).  Gating them on SMS state was over-defensive:
// - broke Layer 0 tests that legitimately call `generate_uuid4()`
//   without any LifecycleGuard scope;
// - broke `HubVault::create()` / `RoleVault::create()` from unit
//   tests that construct vaults without full lifecycle bringup;
// - added a nonsensical failure mode ("SMS not up" for a primitive
//   that libsodium runs happily anyway).
//
// Consumers that WANT full hardening (RLIMIT_CORE=0, PR_SET_DUMPABLE,
// etc.) still get it by putting `SecureSubsystem::GetLifecycleModule()`
// in the process mod pack — the hardening runs at SMS bringup, not
// at each call.  Ungating individual primitives doesn't change
// production security posture: production processes always run
// under the mod pack.
//
// Gated: `keys()`.
// Ungated: everything else on `SecureSubsystem`, plus the free
// functions `secure()` and `sodium_ready()`.
} // namespace

// ============================================================================
// Impl (pImpl) — HEP-CORE-0043 §2.1
// ============================================================================

struct SecureSubsystem::Impl
{
    /// KeyStore lives INSIDE `SecureSubsystem::Impl` post-SEC-Fold-2
    /// (HEP-CORE-0043 §2.2).  Default-constructed with Impl; safe to
    /// access from the moment SMS reaches Initialized state.  Sodium
    /// primitives it calls (sodium_malloc etc.) are guaranteed to see
    /// initialized libsodium because the `keys()` accessor gate blocks
    /// entry before state is Initialized.
    KeyStore keys_;

    /// HEP-CORE-0043 §1.2 / §1.3 startup sequence, called by the
    /// startup thunk via `SecureSubsystem::instance().pImpl->bringup()`:
    ///   1. Singularity CAS on g_state (Uninitialized → InitCalled).
    ///   2. sodium_init() — PANIC on rc < 0.
    ///   3. Platform hardening: disable core dumps.
    ///   4. Inspect mlock capability, WARN if low.
    ///   5. Publish g_state = Initialized under release fence.
    void bringup();

    /// Publishes `Initialized → ShuttingDown → Shutdown`.  Sodium
    /// is stateless and core-dump disable is irreversible
    /// (HEP-CORE-0043 §1.5) — nothing to unwind.  Idempotent —
    /// safe to call multiple times.
    void shutdown_module() noexcept;
};

// ============================================================================
// Platform startup primitives (HEP-CORE-0043 §1.6)
// ============================================================================

namespace
{

void disable_core_dumps_or_panic()
{
#ifndef _WIN32
    struct ::rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    if (::setrlimit(RLIMIT_CORE, &rl) != 0)
    {
        const int saved_errno = errno;
        PLH_PANIC(
            "FATAL: SecureSubsystem: setrlimit(RLIMIT_CORE, 0) failed "
            "(errno={}: {}).  Aborting. (HEP-CORE-0043 §1.2)",
            saved_errno, std::strerror(saved_errno));
    }

#  ifdef __linux__
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
    {
        const int saved_errno = errno;
        PLH_PANIC(
            "FATAL: SecureSubsystem: prctl(PR_SET_DUMPABLE, 0) failed "
            "(errno={}: {}).  Aborting. (HEP-CORE-0043 §1.2)",
            saved_errno, std::strerror(saved_errno));
    }
#  endif // __linux__
#else  // _WIN32
    UINT old_mode = ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    ::SetErrorMode(old_mode | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0)
    {
        (void)::WerAddExcludedApplication(exe_path, FALSE);
    }
#endif // _WIN32
}

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
    return 0UL;
#endif
}

} // namespace

// ============================================================================
// Impl methods — bringup + shutdown
// ============================================================================

void SecureSubsystem::Impl::bringup()
{
    // Step 1: singularity CAS — HEP-CORE-0043 §1.3.  acq_rel on
    // success (release side pairs with a would-be concurrent
    // bringup's acquire load); acquire on failure so the PANIC
    // reads a coherent `expected`.
    {
        SubsystemState expected = SubsystemState::Uninitialized;
        if (!g_state.compare_exchange_strong(
                expected,
                SubsystemState::InitCalled,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            PLH_PANIC(
                "FATAL: SecureSubsystem bringup invoked while another "
                "instance is present or in-flight (observed state={}). "
                "Exactly one instance per process (HEP-CORE-0043 §1.3). "
                "Aborting.",
                static_cast<int>(expected));
        }
    }

    // Step 2: sodium_init.
    const int sodium_rc = ::sodium_init();
    LOGGER_INFO(
        "[SecureSubsystem] event=SodiumInit rc={} libsodium_ver={}.{} pid={}",
        sodium_rc,
        ::sodium_library_version_major(),
        ::sodium_library_version_minor(),
        static_cast<int>(::getpid()));
    if (sodium_rc < 0)
    {
        PLH_PANIC(
            "FATAL: SecureSubsystem: sodium_init() returned {}. "
            "CSPRNG unavailable (check /dev/urandom / getrandom on POSIX; "
            "system entropy on Windows).  Aborting. (HEP-CORE-0043 §1.2)",
            sodium_rc);
    }

    // Step 3: disable core dumps.
    disable_core_dumps_or_panic();

    // Step 4: inspect mlock capability + WARN if low.
    const unsigned long memlock_limit = inspect_memlock_capability();
    if (memlock_limit > 0 && memlock_limit < kMemlockWarnThresholdBytes)
    {
        LOGGER_WARN(
            "[SecureSubsystem] RLIMIT_MEMLOCK soft limit is {} bytes — "
            "below the {} byte advisory threshold.  KeyStore allocations "
            "may fail with ENOMEM.  Raise the limit (ulimit -l on POSIX) "
            "for hardened deployments.",
            memlock_limit, kMemlockWarnThresholdBytes);
    }
#ifdef _WIN32
    LOGGER_WARN(
        "[SecureSubsystem] Windows SeLockMemoryPrivilege probe is not "
        "yet implemented; if KeyStore allocations fail, grant "
        "SeLockMemoryPrivilege via secpol.msc -> Local Policies -> User "
        "Rights Assignment.");
#endif

    // Step 5: publish Initialized with release order.  All writes
    // above (sodium_init effects, RLIMIT changes, KeyStore + Crypto
    // ctor-time allocations) happen-before any consumer's acquire
    // load observing `Initialized` (F12).
    g_state.store(SubsystemState::Initialized, std::memory_order_release);
}

void SecureSubsystem::Impl::shutdown_module() noexcept
{
    // Idempotent — publish Shutdown even if state has already moved.
    // Callers that observe ShuttingDown briefly see a coherent world
    // (bringup effects are visible); callers observing Shutdown must
    // not touch pImpl state.
    g_state.store(SubsystemState::ShuttingDown, std::memory_order_release);
    g_state.store(SubsystemState::Shutdown, std::memory_order_release);
}

// ============================================================================
// SecureSubsystem — trivial ctor / dtor (Logger-pattern shape)
// ============================================================================

SecureSubsystem::SecureSubsystem()
    : pImpl(std::make_unique<Impl>())
{}

SecureSubsystem::~SecureSubsystem()
{
    // F11 — defensive shutdown, R4.6-guarded.  Only publish the
    // shutdown transition when bringup actually ran (state is one of
    // InitCalled / Initialized).  If SMS was never brought up (state
    // still Uninitialized), leave the atomic alone — transitioning
    // directly from Uninitialized to Shutdown would mean "bringup ran
    // + shutdown ran" which is misleading, and the pImpl fields we
    // guard don't hold any state that needs closing anyway.
    if (pImpl)
    {
        const auto state = load_state();
        if (state == SubsystemState::InitCalled ||
            state == SubsystemState::Initialized)
        {
            pImpl->shutdown_module();
        }
    }
}

// ============================================================================
// Singleton accessor + lifecycle-module surface
// ============================================================================

SecureSubsystem &SecureSubsystem::instance()
{
    // C++11 thread-safe function-local static.  Matches
    // `Logger::instance()` (logger.cpp:883-890).
    static SecureSubsystem sole;
    return sole;
}

bool SecureSubsystem::lifecycle_initialized() noexcept
{
    return load_state() != SubsystemState::Uninitialized;
}

// ============================================================================
// Free-function lifecycle thunks (friends of SecureSubsystem)
// ============================================================================

void do_secure_subsystem_startup(const char * /*arg*/, void * /*userdata*/)
{
    // Trigger ctor of the function-local static `sole`, then drive
    // bringup.  Matches Logger's `do_logger_startup` (logger.cpp:1272).
    SecureSubsystem::instance().pImpl->bringup();
}

void do_secure_subsystem_shutdown(const char * /*arg*/, void * /*userdata*/)
{
    SubsystemState expected = SubsystemState::Initialized;
    if (g_state.compare_exchange_strong(
            expected,
            SubsystemState::ShuttingDown,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        SecureSubsystem::instance().pImpl->shutdown_module();
    }
}

pylabhub::utils::ModuleDef SecureSubsystem::GetLifecycleModule()
{
    pylabhub::utils::ModuleDef mod("SecureSubsystem");
    mod.add_dependency("pylabhub::utils::Logger");
    mod.set_startup(&do_secure_subsystem_startup);
    mod.set_shutdown(&do_secure_subsystem_shutdown, std::chrono::milliseconds{100});
    return mod;
}

// ============================================================================
// Sub-container accessors (categories 2 + 3)
// ============================================================================

KeyStore &SecureSubsystem::keys()
{
    panic_if_not_ready("keys()");
    return pImpl->keys_;
}

// ============================================================================
// Namespace-scope free functions
// ============================================================================

bool sodium_ready() noexcept
{
    return load_state() == SubsystemState::Initialized;
}

SecureSubsystem &secure()
{
    return SecureSubsystem::instance();
}

// ─────────────────────────────────────────────────────────────────
// Wrapper API implementations (HEP-CORE-0043 §2.1 category 1)
// uint8_t typing (F9) — matches sodium's unsigned char * convention.
// ─────────────────────────────────────────────────────────────────

void SecureSubsystem::random_bytes(std::span<std::uint8_t> out)
{
    if (out.empty()) return;
    ::randombytes_buf(out.data(), out.size());
}

bool SecureSubsystem::memcmp_ct(std::span<const std::uint8_t> a,
                                 std::span<const std::uint8_t> b)
{
    if (a.size() != b.size()) return false;
    if (a.empty())            return true;
    return ::sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

void SecureSubsystem::memzero(std::span<std::uint8_t> region)
{
    if (region.empty()) return;
    ::sodium_memzero(region.data(), region.size());
}

// ─────────────────────────────────────────────────────────────────
// Retired `pylabhub::crypto::*` API — folded here 2026-07-06 to
// close HEP-CORE-0043 §1.2 mechanism 4 (sodium.h boundary).
// ─────────────────────────────────────────────────────────────────

void SecureSubsystem::random_bytes(std::uint8_t *out, std::size_t len)
{
    if (out == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] random_bytes: null output pointer");
        return;
    }
    ::randombytes_buf(out, len);
}

std::uint64_t SecureSubsystem::random_u64()
{
    std::uint64_t value = 0;
    ::randombytes_buf(&value, sizeof(value));
    return value;
}

std::array<std::uint8_t, 64> SecureSubsystem::generate_shared_secret()
{
    std::array<std::uint8_t, 64> secret{};
    ::randombytes_buf(secret.data(), secret.size());
    return secret;
}

bool SecureSubsystem::compute_blake2b(std::uint8_t *out, const void *data,
                                       std::size_t len)
{
    if (out == nullptr || data == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] compute_blake2b: null pointer argument");
        return false;
    }
    const int rc = ::crypto_generichash(
        out, BLAKE2B_HASH_BYTES,
        static_cast<const unsigned char *>(data), len,
        nullptr, 0);
    if (rc != 0)
    {
        LOGGER_ERROR("[SecureSubsystem] crypto_generichash failed rc={}", rc);
        return false;
    }
    return true;
}

bool SecureSubsystem::derive_pwhash_salt(std::uint8_t *salt_out,
                                          std::string_view domain)
{
    if (salt_out == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] derive_pwhash_salt: null output pointer");
        return false;
    }
    // Genuine BLAKE2b-16 via outlen=16 (parameter-block personalization
    // per RFC 7693 §3.1; NOT a truncation of BLAKE2b-256).  This is
    // the same salt-derivation shape production has used since vault
    // format v1 — changing the outlen here would silently invalidate
    // every existing vault.
    const int rc = ::crypto_generichash(
        salt_out, kPwhashSaltBytes,
        reinterpret_cast<const unsigned char *>(domain.data()),
        domain.size(),
        nullptr, 0);
    if (rc != 0)
    {
        LOGGER_ERROR("[SecureSubsystem] derive_pwhash_salt: crypto_generichash rc={}", rc);
        return false;
    }
    return true;
}

std::array<std::uint8_t, 32>
SecureSubsystem::compute_blake2b_array(const void *data, std::size_t len)
{
    std::array<std::uint8_t, 32> hash{};
    if (!compute_blake2b(hash.data(), data, len))
    {
        hash.fill(0);
    }
    return hash;
}

bool SecureSubsystem::verify_blake2b(const std::uint8_t *stored,
                                      const void *data, std::size_t len)
{
    if (stored == nullptr || data == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] verify_blake2b: null pointer argument");
        return false;
    }
    std::array<std::uint8_t, 32> computed{};
    if (!compute_blake2b(computed.data(), data, len))
    {
        return false;
    }
    return ::sodium_memcmp(stored, computed.data(), BLAKE2B_HASH_BYTES) == 0;
}

bool SecureSubsystem::verify_blake2b(
    const std::array<std::uint8_t, 32> &stored,
    const void *data, std::size_t len)
{
    return verify_blake2b(stored.data(), data, len);
}

// ─────────────────────────────────────────────────────────────────
// Category 1c — Encryption / decryption (protocol operations)
// Collapsed from the retired `Crypto` sub-container 2026-07-06.
// ─────────────────────────────────────────────────────────────────

std::size_t SecureSubsystem::secretbox_encrypt(
    std::uint8_t *out, std::size_t out_max_len,
    const std::uint8_t *plaintext, std::size_t plaintext_len,
    const std::uint8_t *nonce,
    const std::uint8_t *key)
{
    if (out == nullptr || nonce == nullptr || key == nullptr) return 0;
    if (plaintext == nullptr && plaintext_len > 0) return 0;
    const std::size_t need = plaintext_len + kSecretboxMacBytes;
    if (out_max_len < need) return 0;
    if (::crypto_secretbox_easy(out, plaintext, plaintext_len, nonce, key) != 0)
        return 0;
    return need;
}

std::size_t SecureSubsystem::secretbox_decrypt(
    std::uint8_t *out, std::size_t out_max_len,
    const std::uint8_t *ciphertext, std::size_t ciphertext_len,
    const std::uint8_t *nonce,
    const std::uint8_t *key)
{
    if (out == nullptr || ciphertext == nullptr ||
        nonce == nullptr || key == nullptr) return 0;
    if (ciphertext_len < kSecretboxMacBytes) return 0;
    const std::size_t need = ciphertext_len - kSecretboxMacBytes;
    if (out_max_len < need) return 0;
    if (::crypto_secretbox_open_easy(out, ciphertext, ciphertext_len, nonce, key) != 0)
        return 0;
    return need;
}

// ─────────────────────────────────────────────────────────────────
// Category 1c — Asymmetric box (crypto_box)
// Seckey cited by KeyStore name (use-not-export) — bytes never
// cross the API boundary.  HEP-CORE-0043 §1.4 + §6.
// ─────────────────────────────────────────────────────────────────

std::size_t SecureSubsystem::box_encrypt_using(
    std::string_view                  own_seckey_name,
    std::span<const std::uint8_t, 32> peer_pubkey_raw,
    std::span<const std::uint8_t, 24> nonce,
    std::span<const std::uint8_t>     plaintext,
    std::span<std::uint8_t>           out)
{
    const std::size_t need = plaintext.size() + kBoxMacBytes;
    if (out.size() < need)                  return 0;
    if (peer_pubkey_raw.data() == nullptr)  return 0;
    if (nonce.data() == nullptr)            return 0;
    if (plaintext.data() == nullptr && !plaintext.empty()) return 0;

    // `keys()` is the SMS gate — panics if SMS not `Initialized`.
    // The KeyStore's `with_seckey` gives us a scoped view; sodium
    // reads the bytes inside the callback and we return.
    std::size_t written = 0;
    keys().with_seckey(own_seckey_name, [&](std::string_view sk_view) {
        if (sk_view.size() != kBoxSeckeyBytes) return;
        const int rc = ::crypto_box_easy(
            out.data(),
            plaintext.data(), plaintext.size(),
            nonce.data(),
            peer_pubkey_raw.data(),
            reinterpret_cast<const std::uint8_t *>(sk_view.data()));
        if (rc == 0) written = need;
    });
    return written;
}

std::size_t SecureSubsystem::box_decrypt_using(
    std::string_view                  own_seckey_name,
    std::span<const std::uint8_t, 32> peer_pubkey_raw,
    std::span<const std::uint8_t, 24> nonce,
    std::span<const std::uint8_t>     ciphertext,
    std::span<std::uint8_t>           out)
{
    if (ciphertext.size() < kBoxMacBytes)   return 0;
    const std::size_t need = ciphertext.size() - kBoxMacBytes;
    if (out.size() < need)                  return 0;
    if (peer_pubkey_raw.data() == nullptr)  return 0;
    if (nonce.data() == nullptr)            return 0;
    if (ciphertext.data() == nullptr)       return 0;

    std::size_t written = 0;
    keys().with_seckey(own_seckey_name, [&](std::string_view sk_view) {
        if (sk_view.size() != kBoxSeckeyBytes) return;
        const int rc = ::crypto_box_open_easy(
            out.data(),
            ciphertext.data(), ciphertext.size(),
            nonce.data(),
            peer_pubkey_raw.data(),
            reinterpret_cast<const std::uint8_t *>(sk_view.data()));
        if (rc == 0) written = need;
    });
    return written;
}

bool SecureSubsystem::pwhash_argon2id(
    std::uint8_t *out, std::size_t out_len,
    const char *password, std::size_t password_len,
    const std::uint8_t *salt)
{
    if (out == nullptr || password == nullptr || salt == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] pwhash_argon2id: null pointer argument");
        return false;
    }
    const int rc = ::crypto_pwhash(
        out, out_len,
        password, password_len,
        salt,
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13);
    if (rc != 0)
    {
        LOGGER_ERROR("[SecureSubsystem] crypto_pwhash returned {}", rc);
        return false;
    }
    return true;
}

void SecureSubsystem::bin2hex(char *hex, std::size_t hex_max_len,
                               const std::uint8_t *bin, std::size_t bin_len)
{
    if (hex == nullptr || bin == nullptr)
    {
        LOGGER_ERROR("[SecureSubsystem] bin2hex: null pointer argument");
        return;
    }
    ::sodium_bin2hex(hex, hex_max_len, bin, bin_len);
}

} // namespace pylabhub::utils::security
