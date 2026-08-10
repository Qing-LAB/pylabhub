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
    static_assert(kVaultKeyBytes == pylabhub::utils::security::SecureSubsystem::kSymmetricKeyBytes,
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

void vault_write(const fs::path &path, VaultKind kind, std::string_view metadata_json,
                 std::string_view key_name,
                 const std::function<void(std::span<std::uint8_t>)> &fill_secret)
{
    namespace sec = pylabhub::utils::security;

    const std::size_t secret_len = vault_secret_section_bytes(kind);
    if (secret_len == 0)
    {
        throw std::runtime_error("vault_write: unknown vault kind");
    }
    const std::size_t plain_len = secret_len + metadata_json.size();

    //   [ header 12 ][ nonce 24 ][ ciphertext = plain_len ][ tag 16 ]
    std::vector<uint8_t> file_bytes(kVaultHeaderBytes + kVaultNonceBytes + plain_len +
                                    kVaultMacBytes);

    // Anything that leaves this function early leaves a buffer that has
    // held the secret in the clear.  Zero it on every path rather than
    // at each `throw` — there are four of them, and the one that gets
    // forgotten is the one that matters.
    struct WipeOnExit
    {
        std::vector<uint8_t> &b;
        bool armed = true;
        ~WipeOnExit() noexcept
        {
            if (armed)
                pylabhub::utils::security::secure().memzero(std::span<std::uint8_t>(b));
        }
    } wipe{file_bytes};

    std::uint8_t *const hdr = file_bytes.data();
    std::memcpy(hdr + kVaultHdrMagicOffset, kVaultMagic, kVaultHdrMagicBytes);
    hdr[kVaultHdrVersionOffset] = kVaultFormatVersion;
    hdr[kVaultHdrKdfProfileOffset] = static_cast<std::uint8_t>(kVaultWriteProfile);
    hdr[kVaultHdrKindOffset] = static_cast<std::uint8_t>(kind);
    hdr[kVaultHdrReservedOffset] = 0U;

    // Assemble the plaintext WHERE THE CIPHERTEXT WILL GO, so the AEAD
    // encrypts over it in place and the secret is destroyed by the very
    // operation that protects it.  Nothing has to remember to wipe the
    // secret afterwards, because after the call those bytes ARE the
    // ciphertext.  Ordering is fixed by §4.6.6: secret at offset 0 so no
    // variable-length field can move it, metadata filling the rest.
    const std::size_t pt_off = kVaultHeaderBytes + kVaultNonceBytes;
    if (!metadata_json.empty())
    {
        std::memcpy(file_bytes.data() + pt_off + secret_len, metadata_json.data(),
                    metadata_json.size());
    }
    fill_secret(std::span<std::uint8_t>(file_bytes.data() + pt_off, secret_len));

    // `aead_encrypt_using` writes the nonce at the head of the span it
    // is given and the ciphertext immediately after — which is exactly
    // where the plaintext already sits, so `c == m` and libsodium
    // encrypts in place.  The header goes in as associated data, so
    // editing it fails the open (VF-3).
    const std::size_t written = sec::secure().aead_encrypt_using(
        key_name, std::span<const std::uint8_t>(file_bytes.data() + pt_off, plain_len),
        std::span<std::uint8_t>(file_bytes.data() + kVaultHeaderBytes,
                                file_bytes.size() - kVaultHeaderBytes),
        std::span<const std::uint8_t>(file_bytes.data(), kVaultHeaderBytes));
    if (written != kVaultNonceBytes + plain_len + kVaultMacBytes)
    {
        throw std::runtime_error("vault: encryption failed for key '" + std::string(key_name) +
                                 "'");
    }

    // Past this point the buffer holds ciphertext, not the secret.
    wipe.armed = false;
    write_secure_file(path, file_bytes);
}

void vault_read(const fs::path &path, VaultKind expected_kind, const std::string &uid,
                const std::string &password, std::string_view key_name,
                const std::function<void(std::span<const std::uint8_t> secret,
                                         std::string_view metadata_json)> &on_plaintext)
{
    namespace sec = pylabhub::utils::security;

    const std::size_t secret_len = vault_secret_section_bytes(expected_kind);
    if (secret_len == 0)
    {
        throw std::runtime_error("vault_read: unknown vault kind");
    }

    auto file_bytes = read_file(path);

    const std::size_t min_size =
        kVaultHeaderBytes + kVaultNonceBytes + secret_len + kVaultMacBytes;
    if (file_bytes.size() < min_size)
    {
        throw std::runtime_error("vault: file too small to be a v" +
                                 std::to_string(static_cast<int>(kVaultFormatVersion)) +
                                 " vault of this kind (" + std::to_string(file_bytes.size()) +
                                 " bytes, need at least " + std::to_string(min_size) +
                                 "): " + path.string());
    }

    // ── Header first, before a key is derived (VF-1, VF-5) ───────────
    //
    // Each of these is a DIFFERENT failure from a wrong password, and
    // says so.  Collapsing them into "could not open" would tell an
    // operator holding a correct password to go looking for a typo.
    const std::uint8_t *const hdr = file_bytes.data();
    if (std::memcmp(hdr + kVaultHdrMagicOffset, kVaultMagic, kVaultHdrMagicBytes) != 0)
    {
        throw std::runtime_error("vault: '" + path.string() +
                                 "' is not a vault file (bad magic)");
    }
    if (hdr[kVaultHdrVersionOffset] != kVaultFormatVersion)
    {
        throw std::runtime_error(
            "vault: '" + path.string() + "' is format version " +
            std::to_string(static_cast<int>(hdr[kVaultHdrVersionOffset])) + ", this build reads v" +
            std::to_string(static_cast<int>(kVaultFormatVersion)));
    }
    if (hdr[kVaultHdrKindOffset] != static_cast<std::uint8_t>(expected_kind))
    {
        throw std::runtime_error("vault: '" + path.string() + "' is a " +
                                 (hdr[kVaultHdrKindOffset] ==
                                          static_cast<std::uint8_t>(VaultKind::Hub)
                                      ? "hub"
                                      : "role") +
                                 " vault, opened as the other kind");
    }
    // VF-5: a non-zero reserved byte means a writer this reader does not
    // understand.  Refusing costs nothing; guessing forfeits the whole
    // point of having a version field.
    if (hdr[kVaultHdrReservedOffset] != 0U)
    {
        throw std::runtime_error("vault: '" + path.string() +
                                 "' has a non-zero reserved header byte — written by a newer "
                                 "format than this build understands");
    }

    // VF-2: derive at the cost THE FILE records, never this build's.
    VaultKdfCost cost{};
    const auto profile = static_cast<VaultKdfProfile>(hdr[kVaultHdrKdfProfileOffset]);
    if (!vault_kdf_cost(profile, cost))
    {
        throw std::runtime_error("vault: '" + path.string() + "' records KDF profile " +
                                 std::to_string(static_cast<int>(hdr[kVaultHdrKdfProfileOffset])) +
                                 ", which this build does not recognise");
    }
    sec::secure().keys().replace_key_from_password(key_name, password, uid, kVaultKeyBytes,
                                                   cost.opslimit, cost.memlimit);

    // Decrypt in place: the plaintext lands where the ciphertext was.
    // The secret is unavoidably in the clear here — decryption has to
    // produce it somewhere — so the buffer is zeroed on every exit,
    // including one taken by a throwing callback.
    struct WipeOnExit
    {
        std::vector<uint8_t> &b;
        ~WipeOnExit() noexcept
        {
            pylabhub::utils::security::secure().memzero(std::span<std::uint8_t>(b));
        }
    } wipe{file_bytes};

    const std::size_t sealed_len = file_bytes.size() - kVaultHeaderBytes;
    const std::size_t decoded = sec::secure().aead_decrypt_using(
        key_name, std::span<const std::uint8_t>(file_bytes.data() + kVaultHeaderBytes, sealed_len),
        std::span<std::uint8_t>(file_bytes.data() + kVaultHeaderBytes + kVaultNonceBytes,
                                sealed_len - kVaultNonceBytes),
        std::span<const std::uint8_t>(file_bytes.data(), kVaultHeaderBytes));
    if (decoded == 0)
    {
        throw std::runtime_error("vault: decryption failed — wrong password, tampered header, or "
                                 "corrupted file: " +
                                 path.string());
    }

    // VF-4: the plaintext must cover the secret section before anything
    // is sliced out of it.  A shorter plaintext is a corrupt file, not a
    // parse to attempt.
    if (decoded < secret_len)
    {
        throw std::runtime_error("vault: '" + path.string() + "' decrypted to " +
                                 std::to_string(decoded) + " bytes, too short for its " +
                                 std::to_string(secret_len) + "-byte secret section");
    }

    const std::uint8_t *const plain = file_bytes.data() + kVaultHeaderBytes + kVaultNonceBytes;
    on_plaintext(std::span<const std::uint8_t>(plain, secret_len),
                 std::string_view(reinterpret_cast<const char *>(plain + secret_len),
                                  decoded - secret_len));
}

} // namespace pylabhub::utils::detail
