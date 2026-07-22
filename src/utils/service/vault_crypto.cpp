/**
 * @file vault_crypto.cpp
 * @brief Shared Argon2id + XSalsa20-Poly1305 vault crypto implementation.
 */
#include "vault_crypto.hpp"
#include "plh_platform.hpp"
#include "utils/security/secure_subsystem.hpp"

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

/// Set file to owner-only access (equivalent to chmod 0600).
/// On POSIX this uses std::filesystem::permissions; on Windows it sets a DACL
/// granting GENERIC_ALL only to the current user.
void set_owner_only_permissions(const fs::path &path)
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Build SDDL: Owner=current user, DACL=only owner has full access.
    // "D:P(A;;GA;;;OW)" = DACL Protected, Allow Generic All to Owner.
    // We use the SID of the current process token for precision.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;

    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> buf(len);
    if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len))
    {
        CloseHandle(token);
        return;
    }
    CloseHandle(token);

    auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());
    PSID sid = user->User.Sid;

    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &acl) == ERROR_SUCCESS)
    {
        SetNamedSecurityInfoW(const_cast<wchar_t *>(path.wstring().c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
    }
#else
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif
}

void write_secure_file(const fs::path &path, const std::vector<uint8_t> &data)
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows: retain the existing pattern (ofstream + DACL set via
    // SetNamedSecurityInfoW).  O_NOFOLLOW + atomic mode-at-create are
    // POSIX-specific contracts; on Windows the threat model defers to
    // OS-level ACLs managed by an operator-installed service account.
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
        {
            throw std::runtime_error("vault: cannot write: " + path.string());
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ofs.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs)
        {
            throw std::runtime_error("vault: write failed: " + path.string());
        }
    } // flush + close before chmod
    set_owner_only_permissions(path);
#else
    // POSIX atomic-secure write (HEP-CORE-0035 §4.6.1).  Single open(2)
    // call carries the three security contracts as kernel-enforced
    // atomic guards:
    //   O_CREAT  — create iff absent.
    //   O_EXCL   — fail with EEXIST if a file is already at path (the
    //              no-overwrite contract; closes the TOCTOU window the
    //              prior fs::exists() check left open).
    //   O_NOFOLLOW — refuse to traverse a symlink at the final path
    //              component (closes the symlink-redirect attack
    //              where the parent dir is briefly writable and the
    //              attacker plants a symlink to a target they read).
    //   O_CLOEXEC — don't leak the fd across exec(2).
    //   mode=0600 — owner-only at create time (subject to umask;
    //              followed by an explicit fchmod below to neutralize
    //              any pathological umask that would mask owner bits).
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        const int err = errno;
        if (err == EEXIST)
            throw std::runtime_error("vault: file already exists at '" + path.string() +
                                     "' — refusing to overwrite (atomic O_EXCL guard, "
                                     "HEP-CORE-0035 §4.6.1)");
        if (err == ELOOP)
            throw std::runtime_error("vault: '" + path.string() +
                                     "' is a symbolic link — "
                                     "refusing to follow (atomic O_NOFOLLOW guard, "
                                     "HEP-CORE-0035 §4.6.1)");
        throw std::runtime_error("vault: cannot create '" + path.string() +
                                 "': " + std::strerror(err));
    }
    // Belt-and-braces: enforce mode 0600 regardless of umask.  Under
    // a normal umask (0022, 0077) the open above already produces
    // 0600; a pathological umask (e.g. 0177) would mask out OWNER_WRITE
    // and the subsequent write(2) would EBADF.  fchmod normalizes.
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
    {
        const int err = errno;
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error("vault: fchmod 0600 failed for '" + path.string() +
                                 "': " + std::strerror(err));
    }
    // Single write(2) — short-write loop is unnecessary for our
    // payload sizes (<4KB typical, <64KB max) but we still guard.
    const auto *buf = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0)
    {
        const ssize_t n = ::write(fd, buf, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            const int err = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error("vault: write failed for '" + path.string() +
                                     "': " + std::strerror(err));
        }
        buf += n;
        remaining -= static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0)
    {
        const int err = errno;
        // File is on disk with the data we wrote; do NOT unlink on
        // close failure — that could destroy a successful write whose
        // failure was a benign EINTR-during-close (rare but legal).
        throw std::runtime_error("vault: close failed for '" + path.string() +
                                 "': " + std::strerror(err));
    }
#endif
}

std::vector<uint8_t> read_file(const fs::path &path)
{
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
    if (!sec::secure().pwhash_argon2id(key.data(), key.size(), password.data(), password.size(),
                                       salt))
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
