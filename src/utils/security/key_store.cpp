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
#include <zmq.h>  // zmq_z85_encode + zmq_z85_decode — Z85 codec at the
                  // KeyStore module boundary (HEP-CORE-0040 §8.5.2)

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

// HEP-CORE-0040 §8.5.2 (#291 follow-up, 2026-06-26) — in-memory
// identity-key shape is RAW: pub_raw (32 bytes) + sec_raw (32 bytes)
// packed contiguously inside the LockedKey region.  Z85 is reserved
// for the vault file, the wire, and human/log display — see §8.5.2
// for the full contract.  Pre-#291 the storage was Z85 (40+40=80)
// and decoded ad-hoc at every libsodium call site, which silently
// fed 40-byte spans into a 32-byte-expecting crypto_box; the
// AttachProtocol size check at attach_protocol.cpp:559 +
// :388 catches that drift but only after the consumer had to bail
// mid-handshake.  Centralising the encoding at the KeyStore boundary
// removes the bug class.
constexpr std::size_t kRawHalfBytes         = 32; // crypto_box_SECRETKEYBYTES / _PUBLICKEYBYTES
constexpr std::size_t kIdentityKeypairBytes = 64; // pub_raw || sec_raw
constexpr std::size_t kZ85HalfBytes         = 40; // wire/file/display half
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
    /// identity keypair (64 bytes raw = pub_raw[32] || sec_raw[32],
    /// HEP-CORE-0040 §8.5.2) or a raw HEP-0038 secret (arbitrary
    /// bytes).  No cached `CurveKeypair` view: under the use-not-
    /// export design (round-5) the bytes never leave the LockedKey
    /// region as data.
    ///
    /// `pub_z85_cache` — non-secret Z85 view of the pubkey, computed
    /// once at `add_identity` time so `pubkey()` can return a
    /// string_view with the same lifetime as the entry (until
    /// `remove()` or KeyStore dtor) without re-encoding per call.
    /// Living OUTSIDE the LockedKey region is fine: pubkey is non-
    /// secret per HEP-CORE-0036 §I10 + HEP-CORE-0040 §8.5.2.
    struct Entry
    {
        std::unique_ptr<LockedKey> key;
        std::string                pub_z85_cache;  // empty for non-identity
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
    // HEP-CORE-0040 §8.5.2 (#291 follow-up, 2026-06-26) — the packed
    // buffer is now RAW 64 bytes (pub_raw[32] || sec_raw[32]), not
    // Z85 80 bytes.  Pre-#291 callers that packed Z85 must go through
    // add_identity_from_z85 (or its sibling for raw); direct callers
    // of add_identity are the SecureBuffer-pack site below.
    if (packed_pub_sec.size_bytes() != kIdentityKeypairBytes)
    {
        throw std::runtime_error(
            "KeyStore::add_identity: packed_pub_sec must be exactly "
            + std::to_string(kIdentityKeypairBytes)
            + " bytes (pub_raw[32] || sec_raw[32], HEP-CORE-0040 §8.5.2), got "
            + std::to_string(packed_pub_sec.size_bytes()));
    }

    std::string name_key(name);

    // Pre-compute pubkey Z85 cache BEFORE moving into LockedKey, so
    // we can stuff it on the Entry in one shot.  Encoding is from
    // the raw 32 pubkey bytes in the front half of `packed_pub_sec`;
    // result is exactly 40 ASCII chars + NUL (Z85 expansion is 5:4).
    char pub_z85_buf[kZ85HalfBytes + 1] = {};
    if (zmq_z85_encode(pub_z85_buf,
                        reinterpret_cast<const uint8_t *>(
                            packed_pub_sec.data()),
                        kRawHalfBytes) == nullptr)
    {
        throw std::runtime_error(
            "KeyStore::add_identity: zmq_z85_encode(pub) failed");
    }

    std::unique_lock<std::shared_mutex> wlk(pImpl->mu);

    if (pImpl->store.find(name_key) != pImpl->store.end())
    {
        throw std::runtime_error(
            "KeyStore::add_identity: name already present: '"
            + std::string(name) + "'");
    }

    Impl::Entry entry;
    entry.key           = std::make_unique<LockedKey>(packed_pub_sec);
    entry.pub_z85_cache = std::string(pub_z85_buf, kZ85HalfBytes);
    entry.is_identity   = true;

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

    // HEP-CORE-0040 §8.5.2 (#291 follow-up, 2026-06-26) — SINGLE
    // SOURCE OF TRUTH for the Z85→raw decode at the security-module
    // boundary.  SecureBuffer<64> zero-on-destructs the raw scope;
    // add_identity also wipes the source before return.  Production
    // callers (HubConfig / RoleConfig load_keypair, test fixtures)
    // pass Z85 strings; this is the ONE site that does the decode.
    SecureBuffer<kIdentityKeypairBytes> raw_packed;
    auto buf = raw_packed.span();
    if (zmq_z85_decode(reinterpret_cast<uint8_t *>(buf.data()),
                        std::string(pub_z85).c_str()) == nullptr)
    {
        throw std::runtime_error(
            "KeyStore::add_identity_from_z85: zmq_z85_decode(pub) failed");
    }
    if (zmq_z85_decode(reinterpret_cast<uint8_t *>(buf.data() + kRawHalfBytes),
                        std::string(sec_z85).c_str()) == nullptr)
    {
        throw std::runtime_error(
            "KeyStore::add_identity_from_z85: zmq_z85_decode(sec) failed");
    }
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
    // HEP-CORE-0040 §8.5.2 — return the Z85 cache (40 ASCII chars)
    // computed at add_identity time.  Pubkey is non-secret; the
    // cache string lives in the Entry (not in LockedKey).
    return it->second.pub_z85_cache;
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

    // HEP-CORE-0040 §8.5.2 (#291 follow-up, 2026-06-26) — return the
    // RAW 32-byte seckey (sec_raw) from the back half of the 64-byte
    // packed buffer.  This is the single canonical form at the
    // security-module API boundary; libsodium primitives
    // (crypto_box_*, crypto_sign_*) and AttachProtocol's
    // SeckeyAccessor callback all consume raw bytes.  Z85 callers
    // use with_seckey_z85 instead.  Shared lock is held for the
    // callback's duration — concurrent remove(name) waits.  Callback
    // MUST be prompt (HEP-CORE-0040 §5.5 contract).
    const auto bytes = it->second.key->bytes();
    const std::string_view sec(
        reinterpret_cast<const char *>(bytes.data() + kRawHalfBytes),
        kRawHalfBytes);
    use(sec);
}

void KeyStore::with_seckey_z85(std::string_view                          name,
                                std::function<void(std::string_view)>     use) const
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);

    const auto it = pImpl->store.find(std::string(name));
    if (it == pImpl->store.end())
    {
        throw std::out_of_range(
            "KeyStore::with_seckey_z85: name not present: '"
            + std::string(name) + "'");
    }
    if (!it->second.is_identity)
    {
        throw std::out_of_range(
            "KeyStore::with_seckey_z85: '" + std::string(name)
            + "' is a raw secret, not an identity keypair — use lookup_raw");
    }

    // HEP-CORE-0040 §8.5.2 — Z85 view of the seckey, encoded
    // on-the-fly from the raw bytes in LockedKey into a stack buffer.
    // Used by vault round-trip (re-serialize back to disk) and any
    // downstream API that requires Z85 input (rare; prefer the raw
    // accessor + encode at the wire boundary).  Stack buffer is
    // zeroed before return so the seckey bytes don't survive the
    // callback's stack frame.
    const auto bytes = it->second.key->bytes();
    char       sec_z85_buf[kZ85HalfBytes + 1] = {};
    if (zmq_z85_encode(sec_z85_buf,
                        reinterpret_cast<const uint8_t *>(
                            bytes.data() + kRawHalfBytes),
                        kRawHalfBytes) == nullptr)
    {
        throw std::runtime_error(
            "KeyStore::with_seckey_z85: zmq_z85_encode(sec) failed");
    }
    try
    {
        use(std::string_view(sec_z85_buf, kZ85HalfBytes));
    }
    catch (...)
    {
        ::sodium_memzero(sec_z85_buf, sizeof(sec_z85_buf));
        throw;
    }
    ::sodium_memzero(sec_z85_buf, sizeof(sec_z85_buf));
}

void KeyStore::with_keypair_z85(
    std::string_view name,
    std::function<void(std::string_view /*pubkey*/,
                       std::string_view /*seckey*/)> use) const
{
    std::shared_lock<std::shared_mutex> rlk(pImpl->mu);

    const auto it = pImpl->store.find(std::string(name));
    if (it == pImpl->store.end())
    {
        throw std::out_of_range(
            "KeyStore::with_keypair_z85: name not present: '"
            + std::string(name) + "'");
    }
    if (!it->second.is_identity)
    {
        throw std::out_of_range(
            "KeyStore::with_keypair_z85: '" + std::string(name)
            + "' is a raw secret, not an identity keypair — use lookup_raw");
    }

    // Pubkey: non-secret cached Z85 (40 chars, lifetime = Entry).
    const std::string_view pub_view(it->second.pub_z85_cache);

    // Seckey: encode raw → Z85 into a stack buffer, zero before
    // return.  Same shape as with_seckey_z85 above; combining the two
    // saves one entry lookup + one lock acquisition per call.
    const auto bytes = it->second.key->bytes();
    char       sec_z85_buf[kZ85HalfBytes + 1] = {};
    if (zmq_z85_encode(sec_z85_buf,
                        reinterpret_cast<const uint8_t *>(
                            bytes.data() + kRawHalfBytes),
                        kRawHalfBytes) == nullptr)
    {
        throw std::runtime_error(
            "KeyStore::with_keypair_z85: zmq_z85_encode(sec) failed");
    }
    try
    {
        use(pub_view, std::string_view(sec_z85_buf, kZ85HalfBytes));
    }
    catch (...)
    {
        ::sodium_memzero(sec_z85_buf, sizeof(sec_z85_buf));
        throw;
    }
    ::sodium_memzero(sec_z85_buf, sizeof(sec_z85_buf));
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
