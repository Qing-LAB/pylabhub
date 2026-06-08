/**
 * @file key_store.cpp
 * @brief Implementation of HEP-CORE-0040 §5 KeyStore + §6 LockedKey.
 *
 * See `src/include/utils/security/key_store.hpp` for the public surface
 * and HEP-CORE-0040 §5–§6 for the design.  Round-5 (2026-06-06) replaced
 * the original `lookup() → const CurveKeypair&` API with use-not-export
 * (`pubkey()` + `with_seckey(name, callback)`) so the seckey never
 * materializes as a std::string outside the LockedKey region.
 */
#include "utils/security/key_store.hpp"

#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"
#include "utils/security/secure_buffer.hpp"
#include "utils/security/secure_memory_subsystem.hpp"

#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <sodium.h>

#ifdef __linux__
#  include <sys/mman.h>   // madvise / MADV_DONTDUMP
#endif

namespace pylabhub::utils::security
{

// ============================================================================
// LockedKey (HEP-CORE-0040 §6) — RAII wrapper around a sodium_malloc'd
// buffer.  Not exposed in the public header: KeyStore is the only owner.
// ============================================================================

namespace
{

class LockedKey
{
public:
    /// Allocates `len` bytes via `sodium_malloc` (mlock + guard pages +
    /// canary) and copies `plaintext_src` in; then `sodium_memzero`'s
    /// the source.  Throws `std::runtime_error` if allocation fails
    /// (typically RLIMIT_MEMLOCK denial).
    explicit LockedKey(std::span<std::byte> plaintext_src)
        : buf_(static_cast<std::byte *>(::sodium_malloc(plaintext_src.size_bytes()))),
          len_(plaintext_src.size_bytes())
    {
        if (buf_ == nullptr)
        {
            throw std::runtime_error(
                "LockedKey: sodium_malloc failed — RLIMIT_MEMLOCK likely "
                "exhausted (HEP-CORE-0040 §6.1).");
        }
        std::memcpy(buf_, plaintext_src.data(), plaintext_src.size_bytes());
        ::sodium_memzero(plaintext_src.data(), plaintext_src.size_bytes());

#ifdef __linux__
        // Page-granular defence-in-depth (HEP-CORE-0040 §4.2 + §7) —
        // in addition to the process-wide PR_SET_DUMPABLE=0 set by
        // SecureMemorySubsystem.  Best-effort: failure here is logged,
        // not fatal, since the process-wide setting is the primary
        // line of defence.
        if (::madvise(buf_, len_, MADV_DONTDUMP) != 0)
        {
            // Cannot LOGGER_* from this header-free anonymous namespace
            // without dragging logger into the LockedKey TU.  Lifecycle
            // logging would be ideal but is overkill for a defence-in-
            // depth advisory.  Silent best-effort accepted.
        }
#endif
    }

    ~LockedKey() noexcept
    {
        if (buf_ != nullptr)
        {
            ::sodium_memzero(buf_, len_);
            ::sodium_free(buf_);
            buf_ = nullptr;
            len_ = 0;
        }
    }

    LockedKey(const LockedKey &)            = delete;
    LockedKey &operator=(const LockedKey &) = delete;
    LockedKey(LockedKey &&)                 = delete;
    LockedKey &operator=(LockedKey &&)      = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return std::span<const std::byte>(buf_, len_);
    }

private:
    std::byte  *buf_;
    std::size_t len_;
};

} // namespace

// ============================================================================
// File-scope singleton accessor state (HEP-CORE-0040 §5.6)
// ============================================================================

namespace
{
std::mutex   g_keystore_mu;
KeyStore    *g_keystore = nullptr;

// Module name registered with LifecycleManager.  Singleton — exactly
// one KeyStore exists per OS process (HEP-CORE-0040 §5.1).
constexpr const char *kModuleName = "KeyStore";

// Identity-key shape: pub_z85 (40 bytes) + sec_z85 (40 bytes) packed
// contiguously — see HEP-CORE-0040 §8.1.
constexpr std::size_t kIdentityKeypairBytes = 80;
constexpr std::size_t kZ85HalfBytes         = 40;
} // namespace

// ============================================================================
// Impl (pImpl)
// ============================================================================

struct KeyStore::Impl
{
    std::string scope_tag;
    std::string owner_id;
    bool        lifecycle_registered = false;

    /// Map entry — owns one LockedKey + remembers whether it's an
    /// identity keypair (80 bytes = pub_z85 || sec_z85) or a raw
    /// HEP-0038 secret (arbitrary bytes).  No cached `CurveKeypair`
    /// view: under the use-not-export design (round-5) the bytes
    /// never leave the LockedKey region as data.
    struct Entry
    {
        std::unique_ptr<LockedKey> key;
        bool                       is_identity = false;
    };

    mutable std::shared_mutex                  mu;
    std::unordered_map<std::string, Entry>     store;
};

// ============================================================================
// Lifecycle bridge — static thunks called by LifecycleManager
// ============================================================================

namespace
{

/// Validator runs from LifecycleManager's async shutdown thread.  We
/// check the file-scope singleton pointer (the lifetime guarantee
/// `key_store()` provides) — userdata is unused.
bool ks_impl_validate(void * /*userdata*/, uint64_t /*key*/) noexcept
{
    std::lock_guard<std::mutex> lk(g_keystore_mu);
    return g_keystore != nullptr;
}

void ks_startup(const char * /*arg*/, void * /*userdata*/)
{
    // No-op — the actual work runs in KeyStore::add_identity/add_raw
    // as scopes call them during their own startup.
}

void ks_shutdown(const char * /*arg*/, void * /*userdata*/)
{
    // No-op — the destructor owns teardown (sodium_memzero +
    // sodium_free for each entry).  This thunk exists only to
    // participate in the LifecycleGuard ordering graph.
}

} // namespace

// ============================================================================
// KeyStore
// ============================================================================

KeyStore::KeyStore(std::string scope_tag, std::string owner_id)
    : pImpl(std::make_unique<Impl>())
{
    if (scope_tag.empty())
        throw std::invalid_argument("KeyStore: scope_tag must be non-empty");
    if (owner_id.empty())
        throw std::invalid_argument("KeyStore: owner_id must be non-empty");

    if (!secure_memory_subsystem_ready())
    {
        throw std::logic_error(
            "KeyStore: SecureMemorySubsystem not initialized (HEP-CORE-0040 "
            "§4.5).  Construct SecureMemorySubsystem in main() BEFORE the "
            "first KeyStore construction.");
    }

    pImpl->scope_tag = std::move(scope_tag);
    pImpl->owner_id  = std::move(owner_id);

    // Singleton claim — fails fast on second construction without
    // polluting the LifecycleManager registry.
    {
        std::lock_guard<std::mutex> lk(g_keystore_mu);
        if (g_keystore != nullptr)
        {
            throw std::logic_error(
                "KeyStore: already constructed (HEP-CORE-0040 §5.1 — "
                "exactly one instance per process)");
        }
        g_keystore = this;
    }

    try
    {
        ModuleDef mod(kModuleName, pImpl.get(), ks_impl_validate);
        mod.add_dependency("SecureMemory");
        mod.add_dependency("pylabhub::utils::Logger");
        mod.set_startup(ks_startup);
        mod.set_shutdown(ks_shutdown, std::chrono::milliseconds{100});
        mod.set_owner_managed_teardown(true);
        if (LifecycleManager::instance().register_dynamic_module(std::move(mod)))
        {
            (void)LoadModule(kModuleName);
            pImpl->lifecycle_registered = true;
        }
        else
        {
            LOGGER_WARN(
                "[KeyStore:{}:{}] lifecycle module registration returned "
                "false — continuing without lifecycle integration",
                pImpl->scope_tag, pImpl->owner_id);
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN(
            "[KeyStore:{}:{}] lifecycle registration threw: {} — continuing "
            "without lifecycle integration",
            pImpl->scope_tag, pImpl->owner_id, e.what());
    }
}

KeyStore::~KeyStore()
{
    // 1. Block NEW callers — any subsequent `key_store()` throws.
    {
        std::lock_guard<std::mutex> lk(g_keystore_mu);
        if (g_keystore == this)
            g_keystore = nullptr;
    }

    // 2. Drain in-flight readers (HEP-CORE-0040 §5.5).  Acquiring the
    // exclusive lock blocks until every active `with_seckey` callback
    // / `pubkey` / `lookup_raw` call has released its shared lock.
    // The lock is released as `drain_lk` goes out of scope; after this
    // point, no consumer holds any reference into `pImpl` (well-
    // disciplined consumers don't store views past method return; the
    // lifecycle ordering guarantee — broker / BRC stop before scope
    // dtor — combined with this drain closes the UAF window the
    // earlier impl had).
    if (pImpl)
    {
        std::unique_lock<std::shared_mutex> drain_lk(pImpl->mu);
        // drain_lk released as it goes out of scope.
    }

    // 3. Store destruction is automatic — std::unordered_map<Entry>
    // walks entries; each Entry's unique_ptr<LockedKey> dtor runs
    // sodium_memzero + sodium_free.

    if (pImpl && pImpl->lifecycle_registered)
    {
        try
        {
            (void)LifecycleManager::instance().unload_module(
                kModuleName, std::source_location::current());
        }
        catch (...)
        {
            // Dtors must not throw.
        }
    }
}

void KeyStore::add_identity(std::string_view name, std::span<std::byte> packed_pub_sec)
{
    if (packed_pub_sec.size_bytes() != kIdentityKeypairBytes)
    {
        throw std::runtime_error(
            "KeyStore::add_identity: packed_pub_sec must be exactly "
            + std::to_string(kIdentityKeypairBytes)
            + " bytes (pub_z85 || sec_z85), got "
            + std::to_string(packed_pub_sec.size_bytes()));
    }

    std::string name_key(name);

    std::unique_lock<std::shared_mutex> wlk(pImpl->mu);

    if (pImpl->store.find(name_key) != pImpl->store.end())
    {
        throw std::runtime_error(
            "KeyStore::add_identity: name already present: '"
            + std::string(name) + "'");
    }

    Impl::Entry entry;
    entry.key         = std::make_unique<LockedKey>(packed_pub_sec);
    entry.is_identity = true;

    pImpl->store.emplace(std::move(name_key), std::move(entry));
}

void KeyStore::add_identity_from_z85(std::string_view name,
                                      std::string_view pub_z85,
                                      std::string_view sec_z85)
{
    if (pub_z85.size() != kZ85HalfBytes || sec_z85.size() != kZ85HalfBytes)
    {
        throw std::runtime_error(
            "KeyStore::add_identity_from_z85: pub_z85 and sec_z85 must "
            "each be exactly "
            + std::to_string(kZ85HalfBytes)
            + " chars (Z85 of 32 raw bytes).  got pub="
            + std::to_string(pub_z85.size())
            + ", sec=" + std::to_string(sec_z85.size()));
    }

    // SINGLE SOURCE OF TRUTH for the (pub_z85 || sec_z85) layout.
    // SecureBuffer<80> zero-on-destructs the pack scope; add_identity
    // also wipes the source before return.  Production callers
    // (HubConfig / RoleConfig load_keypair) and tests both reach
    // KeyStore through this method — no parallel packing code.
    SecureBuffer<kIdentityKeypairBytes> packed;
    auto buf = packed.span();
    std::memcpy(buf.data(),                 pub_z85.data(), kZ85HalfBytes);
    std::memcpy(buf.data() + kZ85HalfBytes, sec_z85.data(), kZ85HalfBytes);
    add_identity(name, buf);
}

void KeyStore::add_raw(std::string_view name, std::span<std::byte> plaintext)
{
    std::string name_key(name);

    std::unique_lock<std::shared_mutex> wlk(pImpl->mu);

    if (pImpl->store.find(name_key) != pImpl->store.end())
    {
        throw std::runtime_error(
            "KeyStore::add_raw: name already present: '"
            + std::string(name) + "'");
    }

    Impl::Entry entry;
    entry.key         = std::make_unique<LockedKey>(plaintext);
    entry.is_identity = false;

    pImpl->store.emplace(std::move(name_key), std::move(entry));
}

std::string_view KeyStore::pubkey(std::string_view name) const
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);

    const auto it = pImpl->store.find(std::string(name));
    if (it == pImpl->store.end())
    {
        throw std::out_of_range(
            "KeyStore::pubkey: name not present: '" + std::string(name) + "'");
    }
    if (!it->second.is_identity)
    {
        throw std::out_of_range(
            "KeyStore::pubkey: '" + std::string(name)
            + "' is a raw secret, not an identity keypair — use lookup_raw");
    }
    // First 40 bytes of the 80-byte packed pub||sec buffer (HEP-CORE-0040 §8.1).
    const auto bytes = it->second.key->bytes();
    return std::string_view(reinterpret_cast<const char *>(bytes.data()),
                            kZ85HalfBytes);
}

void KeyStore::with_seckey(std::string_view                          name,
                            std::function<void(std::string_view)>     use) const
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);

    const auto it = pImpl->store.find(std::string(name));
    if (it == pImpl->store.end())
    {
        throw std::out_of_range(
            "KeyStore::with_seckey: name not present: '" + std::string(name) + "'");
    }
    if (!it->second.is_identity)
    {
        throw std::out_of_range(
            "KeyStore::with_seckey: '" + std::string(name)
            + "' is a raw secret, not an identity keypair — use lookup_raw");
    }

    // Last 40 bytes of the 80-byte packed pub||sec buffer.  Shared lock
    // is held for the callback's duration — concurrent remove(name)
    // waits.  Callback MUST be prompt (HEP-CORE-0040 §5.5 contract).
    const auto bytes = it->second.key->bytes();
    const std::string_view sec(
        reinterpret_cast<const char *>(bytes.data() + kZ85HalfBytes),
        kZ85HalfBytes);
    use(sec);
}

std::span<const std::byte> KeyStore::lookup_raw(std::string_view name) const
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);

    const auto it = pImpl->store.find(std::string(name));
    if (it == pImpl->store.end())
    {
        throw std::out_of_range(
            "KeyStore::lookup_raw: name not present: '" + std::string(name) + "'");
    }
    return it->second.key->bytes();
}

void KeyStore::remove(std::string_view name)
{
    std::unique_lock<std::shared_mutex> wlk(pImpl->mu);
    pImpl->store.erase(std::string(name));
}

bool KeyStore::has(std::string_view name) const noexcept
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);
    return pImpl->store.find(std::string(name)) != pImpl->store.end();
}

std::size_t KeyStore::size() const noexcept
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);
    return pImpl->store.size();
}

// ============================================================================
// Namespace accessors
// ============================================================================

KeyStore &key_store()
{
    std::lock_guard<std::mutex> lk(g_keystore_mu);
    if (g_keystore == nullptr)
    {
        throw std::runtime_error(
            "key_store(): KeyStore has not been constructed "
            "(HEP-CORE-0040 §5.6).  Construct it in the HubHost / "
            "RoleHostFrame ctor BEFORE any code path that reads identity "
            "keys.");
    }
    return *g_keystore;
}

bool key_store_ready() noexcept
{
    std::lock_guard<std::mutex> lk(g_keystore_mu);
    return g_keystore != nullptr;
}

} // namespace pylabhub::utils::security
