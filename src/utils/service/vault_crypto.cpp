/**
 * @file vault_crypto.cpp
 * @brief Shared Argon2id + XSalsa20-Poly1305 vault crypto implementation.
 */
#include "vault_crypto.hpp"
#include "plh_platform.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sodium.h>
#include <stdexcept>
#include <vector>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
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
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif
}

void write_secure_file(const fs::path &path, const std::vector<uint8_t> &data)
{
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("vault: cannot write: " + path.string());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ofs.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs)
            throw std::runtime_error("vault: write failed: " + path.string());
    } // flush + close before chmod
    set_owner_only_permissions(path);
}

std::vector<uint8_t> read_file(const fs::path &path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        throw std::runtime_error("vault: cannot open: " + path.string());
    const auto size = static_cast<std::size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    if (!ifs)
        throw std::runtime_error("vault: read failed: " + path.string());
    return buf;
}

} // anonymous namespace

// ── Public implementations ────────────────────────────────────────────────────

void vault_require_sodium()
{
    if (sodium_init() == -1)
        throw std::runtime_error("vault: sodium_init() failed");
}

std::array<uint8_t, kVaultKeyBytes> vault_derive_key(const std::string &password,
                                                      const std::string &uid)
{
    // Salt = BLAKE2b-16(uid): deterministic, per-vault domain separation.
    // Same password on two vaults with different uids produces different keys.
    uint8_t salt[kVaultSaltBytes]{};
    crypto_generichash(salt, kVaultSaltBytes,
                       reinterpret_cast<const uint8_t *>(uid.data()), uid.size(),
                       nullptr, 0);

    std::array<uint8_t, kVaultKeyBytes> key{};
    if (crypto_pwhash(key.data(), key.size(),
                      password.data(), password.size(),
                      salt,
                      kVaultOpsLimit,
                      kVaultMemLimit,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw std::runtime_error("vault: Argon2id key derivation failed (insufficient memory?)");
    return key;
}

void vault_write(const fs::path &path,
                 const std::string &json_payload,
                 const std::string &password,
                 const std::string &uid)
{
    vault_require_sodium();

    auto key = vault_derive_key(password, uid);
    struct KeyGuard
    {
        std::array<uint8_t, kVaultKeyBytes> &k;
        ~KeyGuard() { sodium_memzero(k.data(), k.size()); }
    } key_guard{key};

    // Random nonce.
    uint8_t nonce[kVaultNonceBytes]{};
    randombytes_buf(nonce, kVaultNonceBytes);

    // Encrypt: [MAC(16) || ciphertext].
    const std::size_t clen = json_payload.size() + kVaultMacBytes;
    std::vector<uint8_t> ciphertext(clen);
    crypto_secretbox_easy(ciphertext.data(),
                          reinterpret_cast<const uint8_t *>(json_payload.data()),
                          json_payload.size(),
                          nonce, key.data());

    // Write [nonce(24) || MAC+ciphertext] to path at mode 0600.
    std::vector<uint8_t> vault_bytes;
    vault_bytes.reserve(kVaultNonceBytes + clen);
    vault_bytes.insert(vault_bytes.end(), nonce, nonce + kVaultNonceBytes);
    vault_bytes.insert(vault_bytes.end(), ciphertext.begin(), ciphertext.end());
    write_secure_file(path, vault_bytes);
}

std::string vault_read(const fs::path &path,
                       const std::string &password,
                       const std::string &uid)
{
    vault_require_sodium();

    const auto vault_bytes = read_file(path);

    constexpr std::size_t kMinSize = kVaultNonceBytes + kVaultMacBytes + 1;
    if (vault_bytes.size() < kMinSize)
        throw std::runtime_error("vault: file too small or corrupted: " + path.string());

    const uint8_t *nonce      = vault_bytes.data();
    const uint8_t *ciphertext = vault_bytes.data() + kVaultNonceBytes;
    const std::size_t clen    = vault_bytes.size() - kVaultNonceBytes;

    auto key = vault_derive_key(password, uid);
    struct KeyGuard
    {
        std::array<uint8_t, kVaultKeyBytes> &k;
        ~KeyGuard() { sodium_memzero(k.data(), k.size()); }
    } key_guard{key};

    std::vector<uint8_t> plaintext(clen - kVaultMacBytes);
    if (crypto_secretbox_open_easy(plaintext.data(), ciphertext, clen, nonce, key.data()) != 0)
        throw std::runtime_error(
            "vault: decryption failed — wrong password or corrupted file: " + path.string());

    return std::string(plaintext.begin(), plaintext.end());
}

} // namespace pylabhub::utils::detail
