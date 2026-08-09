/**
 * @file vault_crypto.cpp
 * @brief Shared Argon2id + XSalsa20-Poly1305 vault crypto implementation.
 */
#include "vault_crypto.hpp"
#include "plh_platform.hpp"
#include "utils/security/key_store.hpp" // keys().replace_key_from_password
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

void vault_add_key_from_password(std::string_view key_name, const std::string &password,
                                 const std::string &uid)
{
    namespace sec = pylabhub::utils::security;
    static_assert(kVaultSaltBytes == pylabhub::utils::security::SecureSubsystem::kPwhashSaltBytes,
                  "vault salt size must match SMS's Argon2id salt size");
    static_assert(kVaultKeyBytes == pylabhub::utils::security::SecureSubsystem::kSecretboxKeyBytes,
                  "vault key size must match SMS's secretbox key size");

    // `replace_` rather than `add_`: re-opening a vault legitimately
    // re-derives the same key under the same name, and making the
    // caller `remove` first would leave a window where the name
    // resolves to nothing.
    //
    // `uid` is the domain separator — the same password on two vaults
    // with different uids yields different keys.  The vault's own
    // compile-time cost profile is passed explicitly; a wrapper
    // choosing it for us is exactly the bug fixed on 2026-08-09, where
    // all three profiles selected constants that reached nothing.
    sec::secure().keys().replace_key_from_password(key_name, password, uid, kVaultKeyBytes,
                                                   kVaultOpsLimit, kVaultMemLimit);
}

void vault_write(const fs::path &path, const std::string &json_payload, std::string_view key_name)
{
    namespace sec = pylabhub::utils::security;
    // No key appears in this function.  `secretbox_encrypt_using`
    // generates the nonce internally and emits
    // `[nonce(24) || MAC(16) || ciphertext]` — byte-for-byte the vault
    // format this file used to assemble by hand, so the on-disk layout
    // is unchanged and old vaults still open.
    std::vector<uint8_t> vault_bytes(json_payload.size() +
                                     sec::SecureSubsystem::kSealedOverheadBytes);
    const std::size_t written = sec::secure().secretbox_encrypt_using(
        key_name,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(json_payload.data()),
                                      json_payload.size()),
        std::span<std::uint8_t>(vault_bytes.data(), vault_bytes.size()));
    if (written != vault_bytes.size())
    {
        throw std::runtime_error("vault: encryption failed for key '" + std::string(key_name) +
                                 "'");
    }

    write_secure_file(path, vault_bytes);
}

std::size_t vault_read_secure(const fs::path &path, std::string_view key_name,
                              std::span<std::byte> out_buf)
{
    namespace sec = pylabhub::utils::security;
    const auto vault_bytes = read_file(path);

    constexpr std::size_t kMinSize = kVaultNonceBytes + kVaultMacBytes + 1;
    if (vault_bytes.size() < kMinSize)
    {
        throw std::runtime_error("vault: file too small or corrupted: " + path.string());
    }

    const std::size_t plain_len = vault_bytes.size() - sec::SecureSubsystem::kSealedOverheadBytes;

    auto span_as_u8 = std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(out_buf.data()),
                                              out_buf.size_bytes());
    if (plain_len > out_buf.size_bytes())
    {
        sec::secure().memzero(span_as_u8);
        throw std::runtime_error("vault_read_secure: out_buf too small (need " +
                                 std::to_string(plain_len) + " bytes, got " +
                                 std::to_string(out_buf.size_bytes()) + "): " + path.string());
    }

    // No key here either — the whole file, nonce included, goes to the
    // named-key operation.  A 0 return IS the password check: the
    // Poly1305 tag either verifies or it does not.
    const std::size_t decoded = sec::secure().secretbox_decrypt_using(
        key_name, std::span<const std::uint8_t>(vault_bytes.data(), vault_bytes.size()),
        span_as_u8);
    if (decoded == 0)
    {
        sec::secure().memzero(span_as_u8);
        throw std::runtime_error("vault: decryption failed — wrong password or corrupted file: " +
                                 path.string());
    }

    return decoded;
}

} // namespace pylabhub::utils::detail
