/**
 * @file vault_crypto.cpp
 * @brief Shared Argon2id + XSalsa20-Poly1305 vault crypto implementation.
 */
#include "vault_crypto.hpp"
#include "plh_platform.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/security/key_file_acl.hpp" // write_keyfile — HEP-0035 §4.6.1 recipe

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pylabhub::utils::detail
{

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace
{

void write_secure_file(const fs::path &path, const std::vector<uint8_t> &data)
{
    // HEP-CORE-0035 §4.6.1's write recipe lives in exactly one place now
    // (`security::write_keyfile`).  This function used to spell it out
    // by hand, and the hand-written copy had drifted in two ways: it
    // never called `fsync`, so the vault — the file whose loss costs an
    // identity keypair — was the least durable of the three writers in
    // the tree; and its Windows branch was `std::ofstream(trunc)`, which
    // silently overwrote, so the "refuses to overwrite" guarantee the
    // POSIX branch enforced with O_EXCL did not exist there at all.
    //
    // `Refuse` preserves the property this function has always promised
    // on POSIX: a vault is never silently clobbered.
    namespace sec = pylabhub::utils::security;
    sec::write_keyfile(path,
                       std::string_view(reinterpret_cast<const char *>(data.data()), data.size()),
                       sec::KeyFileRole::VaultFile, sec::ExistingFilePolicy::Refuse);
}

std::vector<uint8_t> read_file(const fs::path &path)
{
#ifndef _WIN32
    // O_NOFOLLOW on the READ path too (review S-2).  The write path has
    // refused to traverse a symlink at the final component since it was
    // written (`O_CREAT|O_EXCL|O_NOFOLLOW` above), but the read did not —
    // so a link planted at the vault path would be followed here, and
    // `verify_keyfile_acl` could not report it either because it stats the
    // TARGET.  The asymmetry had no rationale; it was simply never closed.
    {
        const int probe = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (probe == -1)
        {
            const int err = errno;
            if (err == ELOOP)
            {
                throw std::runtime_error("vault: '" + path.string() +
                                         "' is a symlink — refusing to follow "
                                         "(O_NOFOLLOW guard, HEP-CORE-0035 §4.6.1)");
            }
            throw std::runtime_error("vault: cannot open: " + path.string() + ": " +
                                     std::strerror(err));
        }
        ::close(probe);
    }
#endif
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
    {
        throw std::runtime_error("vault: cannot open: " + path.string());
    }
    const auto size = static_cast<std::size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    if (!ifs)
    {
        throw std::runtime_error("vault: read failed: " + path.string());
    }
    return buf;
}

} // anonymous namespace

// ── Public implementations ────────────────────────────────────────────────────

void vault_require_sodium()
{
    // No-op — retained as a stable API name during the SEC-Fold-2
    // rollout.  All vault ops now route through `secure()` /
    // `secure()`'s gate, which PANICs if SMS is not
    // `Initialized`.  Delete this shim once callers stop referencing it.
}

std::array<uint8_t, kVaultKeyBytes> vault_derive_key(const std::string &password,
                                                     const std::string &uid)
{
    namespace sec = pylabhub::utils::security;
    // Salt = BLAKE2b-16(uid): deterministic, per-vault domain separation.
    // Same password on two vaults with different uids produces different keys.
    // Salt derivation via the purpose-specific SMS method
    // `derive_pwhash_salt`.  Encapsulates the "16 bytes because
    // Argon2id" reasoning inside the security module — this call
    // site just names the operation.
    static_assert(kVaultSaltBytes == pylabhub::utils::security::SecureSubsystem::kPwhashSaltBytes,
                  "vault salt size must match SMS's Argon2id salt size");
    uint8_t salt[kVaultSaltBytes]{};
    if (!sec::secure().derive_pwhash_salt(salt, uid))
    {
        throw std::runtime_error("vault: salt derivation failed");
    }

    std::array<uint8_t, kVaultKeyBytes> key{};
    // Pass the vault's OWN cost parameters.  Until 2026-08-09 this call
    // omitted them and `pwhash_argon2id` hardcoded INTERACTIVE, so the
    // three compile-time profiles above selected constants that reached
    // nothing: `-DPYLABHUB_VAULT_HIGH_SECURITY` produced ordinary
    // INTERACTIVE vaults while reporting success, and the CI fast-KDF
    // build paid the full ~100 ms it was added to avoid.  The default
    // profile matched INTERACTIVE by coincidence, which is why it went
    // unnoticed.
    if (!sec::secure().pwhash_argon2id(key.data(), key.size(), password.data(), password.size(),
                                       salt, kVaultOpsLimit, kVaultMemLimit))
    {
        throw std::runtime_error("vault: Argon2id key derivation failed (insufficient memory?)");
    }
    return key;
}

void vault_write(const fs::path &path, const std::string &json_payload, const std::string &password,
                 const std::string &uid)
{
    namespace sec = pylabhub::utils::security;
    vault_require_sodium();

    auto key = vault_derive_key(password, uid);
    struct KeyGuard
    {
        std::array<uint8_t, kVaultKeyBytes> &k;
        ~KeyGuard()
        {
            pylabhub::utils::security::secure().memzero(
                std::span<std::uint8_t>(k.data(), k.size()));
        }
    } key_guard{key};

    // Random nonce.
    uint8_t nonce[kVaultNonceBytes]{};
    sec::secure().random_bytes(nonce, kVaultNonceBytes);

    // Encrypt: [MAC(16) || ciphertext].
    const std::size_t clen = json_payload.size() + kVaultMacBytes;
    std::vector<uint8_t> ciphertext(clen);
    const std::size_t written = sec::secure().secretbox_encrypt(
        ciphertext.data(), ciphertext.size(),
        reinterpret_cast<const std::uint8_t *>(json_payload.data()), json_payload.size(),
        std::span<const std::uint8_t, 24>(nonce, kVaultNonceBytes),
        std::span<const std::uint8_t, 32>(key.data(), kVaultKeyBytes));
    if (written == 0)
    {
        throw std::runtime_error("vault: secretbox_encrypt failed");
    }

    // Write [nonce(24) || MAC+ciphertext] to path at mode 0600.
    std::vector<uint8_t> vault_bytes;
    vault_bytes.reserve(kVaultNonceBytes + clen);
    vault_bytes.insert(vault_bytes.end(), nonce, nonce + kVaultNonceBytes);
    vault_bytes.insert(vault_bytes.end(), ciphertext.begin(), ciphertext.end());
    write_secure_file(path, vault_bytes);
}

std::size_t vault_read_secure(const fs::path &path, const std::string &password,
                              const std::string &uid, std::span<std::byte> out_buf)
{
    namespace sec = pylabhub::utils::security;
    vault_require_sodium();

    const auto vault_bytes = read_file(path);

    constexpr std::size_t kMinSize = kVaultNonceBytes + kVaultMacBytes + 1;
    if (vault_bytes.size() < kMinSize)
    {
        throw std::runtime_error("vault: file too small or corrupted: " + path.string());
    }

    const uint8_t *nonce = vault_bytes.data();
    const uint8_t *ciphertext = vault_bytes.data() + kVaultNonceBytes;
    const std::size_t clen = vault_bytes.size() - kVaultNonceBytes;
    const std::size_t plain_len = clen - kVaultMacBytes;

    auto span_as_u8 = std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(out_buf.data()),
                                              out_buf.size_bytes());
    if (plain_len > out_buf.size_bytes())
    {
        sec::secure().memzero(span_as_u8);
        throw std::runtime_error("vault_read_secure: out_buf too small (need " +
                                 std::to_string(plain_len) + " bytes, got " +
                                 std::to_string(out_buf.size_bytes()) + "): " + path.string());
    }

    auto key = vault_derive_key(password, uid);
    struct KeyGuard
    {
        std::array<uint8_t, kVaultKeyBytes> &k;
        ~KeyGuard()
        {
            pylabhub::utils::security::secure().memzero(
                std::span<std::uint8_t>(k.data(), k.size()));
        }
    } key_guard{key};

    // Decrypt into the caller's span via SMS Category 1c
    // (HEP-CORE-0043 §5).
    const std::size_t decoded = sec::secure().secretbox_decrypt(
        span_as_u8.data(), span_as_u8.size(), ciphertext, clen,
        std::span<const std::uint8_t, 24>(nonce, kVaultNonceBytes),
        std::span<const std::uint8_t, 32>(key.data(), kVaultKeyBytes));
    if (decoded == 0)
    {
        sec::secure().memzero(span_as_u8);
        throw std::runtime_error("vault: decryption failed — wrong password or corrupted file: " +
                                 path.string());
    }

    return decoded;
}

} // namespace pylabhub::utils::detail
