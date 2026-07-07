/**
 * @file hub_vault.cpp
 * @brief HubVault — encrypted hub secrets store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with RoleVault).
 * This file handles only hub-specific payload structure (broker keypair + admin token).
 */
#include "utils/hub_vault.hpp"
#include "plh_platform.hpp"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/security/secure_buffer.hpp"
#include "utils/security/secure_subsystem.hpp"

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
#include <zmq.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

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
using json   = nlohmann::json;

namespace pylabhub::utils
{

// ============================================================================
// Pimpl
// ============================================================================

struct HubVault::Impl
{
    /// HEP-CORE-0040 §175: secrets stored in fixed-size buffers + zeroed
    /// in dtor via `sodium_memzero`.  Replaces the pre-#175
    /// `std::string` members whose destructors are not guaranteed to
    /// zero heap memory.
    std::array<char, 40> broker_secret_z85{};
    std::array<char, 40> broker_public_z85{};
    std::array<char, 64> admin_token_hex{};

    /// View sizes (`size()` of each accessor's returned `std::string_view`).
    /// All three secrets are fixed-length in this vault, so the views are
    /// always full-size after a successful `open()` or `create()`.
    static constexpr std::size_t kSecretLen = 40;
    static constexpr std::size_t kPublicLen = 40;
    static constexpr std::size_t kAdminLen  = 64;

    ~Impl() noexcept
    {
        // Wipe via SMS (HEP-CORE-0043 §2.1 category 1) — the security
        // module owns every sodium primitive.  Cast to uint8_t* is safe
        // (byte-addressable char array).
        namespace sec = pylabhub::utils::security;
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(broker_secret_z85.data()),
            broker_secret_z85.size()));
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(broker_public_z85.data()),
            broker_public_z85.size()));
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(admin_token_hex.data()),
            admin_token_hex.size()));
    }
};

// ============================================================================
// Helpers
// ============================================================================

namespace
{

constexpr std::size_t kAdminRawBytes = 32; // → 64 hex chars

std::string generate_admin_token()
{
    namespace sec = pylabhub::utils::security;
    uint8_t raw[kAdminRawBytes]{};
    sec::secure().random_bytes(raw, sizeof(raw));
    char hex[kAdminRawBytes * 2 + 1]{};
    sec::secure().bin2hex(hex, sizeof(hex), raw, sizeof(raw));
    std::string token(hex, kAdminRawBytes * 2);
    sec::secure().memzero(std::span<std::uint8_t>(raw, sizeof(raw)));
    sec::secure().memzero(std::span<std::uint8_t>(
        reinterpret_cast<std::uint8_t *>(hex), sizeof(hex)));
    return token;
}

} // anonymous namespace

// ============================================================================
// Constructors / destructor
// ============================================================================

HubVault::HubVault() : pImpl(std::make_unique<Impl>()) {}
HubVault::~HubVault()                       = default;
HubVault::HubVault(HubVault &&) noexcept    = default;
HubVault &HubVault::operator=(HubVault &&) noexcept = default;

// ============================================================================
// HubVault::create
// ============================================================================

HubVault HubVault::create(const fs::path    &vault_path,
                           const std::string &hub_uid,
                           const std::string &password)
{
    detail::vault_require_sodium();

    // Generate broker CurveZMQ keypair (Z85).
    auto kp = pylabhub::utils::security::generate_curve_keypair();
    const std::string broker_public = std::move(kp.public_z85);
    const std::string broker_secret = std::move(kp.secret_z85);

    // Generate admin token.
    const std::string admin_tok = generate_admin_token();

    // Serialize payload and encrypt at the operator-supplied path
    // (HEP-CORE-0033 §7.1 — caller resolved via resolve_keyfile_path).
    // Ensure the parent directory exists; --init creates the
    // canonical `vault/` dir but direct callers (tests, custom
    // deployment locations) may not have.
    const json payload = {
        {"broker", {{"curve_secret_key", broker_secret}, {"curve_public_key", broker_public}}},
        {"admin",  {{"token", admin_tok}}}
    };
    if (vault_path.has_parent_path())
    {
        fs::create_directories(vault_path.parent_path());
        // HEP-CORE-0035 §4.6.1: keystore directory MUST be 0700.
        // fs::create_directories applies process umask (typically
        // 0755 result); enforce explicit 0700 here so the directory
        // is not briefly world-readable before vault_write lays
        // down the 0600 secret inside.
        namespace sec = pylabhub::utils::security;
        int chmod_err = 0;
        const auto rc = sec::set_keyfile_mode(
            vault_path.parent_path(), sec::KeyFileRole::VaultDir, &chmod_err);
        // set_keyfile_mode always populates out_errno on ChmodFailed
        // (chmod failure → errno; bad_alloc fallback → ENOMEM).
        if (rc == sec::SetModeResult::ChmodFailed)
            throw std::runtime_error(
                "HubVault: chmod 0700 failed on vault parent dir '" +
                vault_path.parent_path().string() + "': " +
                std::strerror(chmod_err));
    }
    detail::vault_write(vault_path, payload.dump(), password, hub_uid);

    HubVault v;
    if (broker_secret.size() != HubVault::Impl::kSecretLen
     || broker_public.size() != HubVault::Impl::kPublicLen
     || admin_tok.size()     != HubVault::Impl::kAdminLen)
    {
        throw std::runtime_error(
            "HubVault::create: generated material has unexpected length");
    }
    std::memcpy(v.pImpl->broker_secret_z85.data(), broker_secret.data(),
                HubVault::Impl::kSecretLen);
    std::memcpy(v.pImpl->broker_public_z85.data(), broker_public.data(),
                HubVault::Impl::kPublicLen);
    std::memcpy(v.pImpl->admin_token_hex.data(),   admin_tok.data(),
                HubVault::Impl::kAdminLen);
    return v;
}

// ============================================================================
// HubVault::open
// ============================================================================

HubVault HubVault::open(const fs::path    &vault_path,
                         const std::string &hub_uid,
                         const std::string &password)
{
    detail::vault_require_sodium();

    // HEP-CORE-0040 §175: decrypt directly into a stack buffer whose
    // destructor zeroes the plaintext when this scope exits.  Sized
    // generously for the small JSON payload (broker keys + admin
    // token + framing); fits comfortably under 4 KiB.
    pylabhub::utils::security::SecureBuffer<4096> json_buf;
    const std::size_t n =
        detail::vault_read_secure(vault_path, password, hub_uid, json_buf.span());

    HubVault v;
    try
    {
        const auto bytes = json_buf.span().first(n);
        const json j = json::parse(
            reinterpret_cast<const char *>(bytes.data()),
            reinterpret_cast<const char *>(bytes.data() + bytes.size()));

        auto copy_into = [](auto &dst, std::size_t expected_len,
                            const std::string &src, const char *field) {
            if (src.size() != expected_len)
            {
                throw std::runtime_error(
                    std::string("HubVault: ") + field + " has unexpected length ("
                    + std::to_string(src.size()) + ", expected "
                    + std::to_string(expected_len) + ")");
            }
            std::memcpy(dst.data(), src.data(), expected_len);
        };

        // Extract via temporary std::strings (json's internal storage
        // is std::string-backed).  The temporaries live only as long
        // as these statements; the json object's own internal copies
        // live until `j` destructs at end of scope (microsecond
        // window of unlocked exposure — bounded because identity
        // bytes live in the process KeyStore (locked memory) from
        // `HubConfig::load_keypair` onward per HEP-CORE-0040 §172).
        copy_into(v.pImpl->broker_secret_z85, HubVault::Impl::kSecretLen,
                  j.at("broker").at("curve_secret_key").get<std::string>(),
                  "broker.curve_secret_key");
        copy_into(v.pImpl->broker_public_z85, HubVault::Impl::kPublicLen,
                  j.at("broker").at("curve_public_key").get<std::string>(),
                  "broker.curve_public_key");
        copy_into(v.pImpl->admin_token_hex,   HubVault::Impl::kAdminLen,
                  j.at("admin").at("token").get<std::string>(),
                  "admin.token");
    }
    catch (const json::exception &e)
    {
        throw std::runtime_error(std::string("HubVault: vault payload invalid: ") + e.what());
    }
    return v;
}

// ============================================================================
// Accessors
// ============================================================================

std::string_view HubVault::broker_curve_secret_key() const noexcept
{
    return std::string_view(pImpl->broker_secret_z85.data(),
                            Impl::kSecretLen);
}

std::string_view HubVault::broker_curve_public_key() const noexcept
{
    return std::string_view(pImpl->broker_public_z85.data(),
                            Impl::kPublicLen);
}

std::string_view HubVault::admin_token() const noexcept
{
    return std::string_view(pImpl->admin_token_hex.data(),
                            Impl::kAdminLen);
}

// ============================================================================
// publish_public_key
// ============================================================================

void HubVault::publish_public_key(const fs::path &hub_dir) const
{
    const fs::path pubkey_path = hub_dir / "hub.pubkey";
#if defined(PYLABHUB_PLATFORM_WIN64)
    {
        std::ofstream ofs(pubkey_path, std::ios::trunc);
        if (!ofs)
        {
            throw std::runtime_error("HubVault: cannot write hub.pubkey: " +
                                     pubkey_path.string());
        }
        ofs << broker_curve_public_key();
        if (!ofs)
        {
            throw std::runtime_error("HubVault: write failed: " + pubkey_path.string());
        }
    }
#else
    // POSIX integrity-hardened write (HEP-CORE-0035 §4.6.4 expansion
    // 2026-06-01): the pubkey file is intentionally world-readable
    // (0644) — confidentiality is not the goal — but INTEGRITY is.
    // An attacker who can briefly write inside hub_dir could plant a
    // symlink at `hub.pubkey` redirecting writes to an attacker-chosen
    // target (federation-trust hijack: `cp <hub-dir>/hub.pubkey
    // <role-dir>/` would then propagate the wrong material).
    //
    // Atomicity guarantee, exact flags as written below:
    //   - O_CREAT + O_EXCL   : open(2) fails with EEXIST if any path
    //                          component exists at pubkey_path.  This
    //                          is the lock that prevents writing onto
    //                          a path an attacker has under their
    //                          control between our pre-clean unlink
    //                          and our create.  NB: the lock is only
    //                          as strong as the parent dir's mode —
    //                          an attacker without write-on-parent
    //                          cannot plant anything to race against.
    //                          The hub-dir parent is operator-managed,
    //                          not 0700-enforced like VaultDir; if
    //                          you're adding a new auth surface that
    //                          uses this pattern under stricter
    //                          attacker assumptions, pair it with
    //                          `verify_keyfile_acl(parent, VaultDir)`
    //                          per HEP-CORE-0035 §4.6.4 precondition.
    //   - O_NOFOLLOW         : refuses to traverse a symlink at the
    //                          FINAL component.  Closes the redirect
    //                          attack even if O_EXCL would not (we
    //                          unlink first, but O_NOFOLLOW is the
    //                          defense-in-depth catch).
    //   - O_WRONLY + O_CLOEXEC : write-only fd, not inherited by exec.
    //   - mode 0644 : atomic at create + normalized by fchmod below
    //                 (matches HEP-CORE-0035 §4.6.1 PublicKeyFile).
    //
    // O_TRUNC is INTENTIONALLY ABSENT.  We do NOT overwrite in place.
    // Re-keygen replace works via the explicit unlink-then-O_EXCL
    // pattern below, NOT via O_TRUNC.  A future "harmonization" that
    // adds O_TRUNC and drops O_EXCL would silently undo the redirect
    // defense — the lock is O_EXCL, not the unlink.
    {
        // Pre-clean: unlink any existing path at pubkey_path so the
        // atomic O_EXCL create below succeeds on a fresh inode.
        // remove() without follow_symlink semantics deletes the
        // symlink itself if one is present (filesystem::remove is
        // specified as `::unlink(2)` on POSIX).  ENOENT is fine
        // (first publish).
        //
        // SECURITY OBSERVABILITY (HEP-CORE-0035 §4.6.4): if a path
        // actually existed at pubkey_path before --keygen, that's a
        // signal — either a prior keygen left it (expected) or an
        // attacker planted it (unexpected).  Emit a one-line stderr
        // note so the operator at least has audit-trail evidence
        // that a removal happened.
        std::error_code ec;
        const bool removed = std::filesystem::remove(pubkey_path, ec);
        if (removed)
            // Wording: "removed (publish attempt follows)" — NOT
            // "was removed before publish".  If the subsequent
            // open(O_EXCL) fails (e.g., racing writer plants a new
            // file → EEXIST), the operator would otherwise see a
            // contradiction: this note claims publish happened, but
            // the subsequent error claims it didn't.  "Attempt
            // follows" keeps the note honest about ordering.
            std::fprintf(stderr,
                         "[plh_hub] note: pre-existing hub.pubkey at '%s' "
                         "was removed (publish attempt follows; expected "
                         "on re-keygen; investigate if unexpected — "
                         "HEP-CORE-0035 §4.6.4)\n",
                         pubkey_path.string().c_str());
        // We don't check ec here — if the unlink failed for any reason
        // other than ENOENT, the open(O_EXCL) below will surface a
        // precise errno (EEXIST for a leftover file, ELOOP for an
        // unreadable symlink chain).
    }
    const int fd = ::open(pubkey_path.c_str(),
                          O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0)
    {
        const int err = errno;
        if (err == ELOOP)
            throw std::runtime_error(
                "HubVault: '" + pubkey_path.string() +
                "' is a symbolic link — refusing to follow "
                "(atomic O_NOFOLLOW guard, HEP-CORE-0035 §4.6.1)");
        if (err == EEXIST)
            throw std::runtime_error(
                "HubVault: '" + pubkey_path.string() +
                "' could not be cleared before publish "
                "(racing writer?)");
        throw std::runtime_error(
            "HubVault: cannot create '" + pubkey_path.string() +
            "': " + std::strerror(err));
    }
    // Normalize mode against pathological umask (subset of write_secure_file
    // hardening — matches the PublicKeyFile canonical mode).
    if (::fchmod(fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
    {
        const int err = errno;
        ::close(fd);
        ::unlink(pubkey_path.c_str());
        throw std::runtime_error(
            "HubVault: fchmod 0644 failed for '" + pubkey_path.string() +
            "': " + std::strerror(err));
    }
    const auto key = broker_curve_public_key();
    const char *buf = key.data();
    std::size_t remaining = key.size();
    while (remaining > 0)
    {
        const ssize_t n = ::write(fd, buf, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            const int err = errno;
            ::close(fd);
            ::unlink(pubkey_path.c_str());
            throw std::runtime_error(
                "HubVault: write failed for '" + pubkey_path.string() +
                "': " + std::strerror(err));
        }
        buf       += n;
        remaining -= static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0)
        throw std::runtime_error(
            "HubVault: close failed for '" + pubkey_path.string() +
            "': " + std::strerror(errno));
#endif
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows: set DACL granting owner full access + everyone read.
    {
        WELL_KNOWN_SID_TYPE everyone_type = WinWorldSid;
        BYTE everyone_sid[SECURITY_MAX_SID_SIZE];
        DWORD sid_size = sizeof(everyone_sid);
        CreateWellKnownSid(everyone_type, nullptr, everyone_sid, &sid_size);

        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            DWORD len = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &len);
            std::vector<uint8_t> buf(len);
            if (GetTokenInformation(token, TokenUser, buf.data(), len, &len))
            {
                auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());

                EXPLICIT_ACCESS_W ea[2]{};
                // Owner: full access
                ea[0].grfAccessPermissions = GENERIC_ALL;
                ea[0].grfAccessMode = SET_ACCESS;
                ea[0].grfInheritance = NO_INHERITANCE;
                ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
                // Everyone: read
                ea[1].grfAccessPermissions = GENERIC_READ;
                ea[1].grfAccessMode = SET_ACCESS;
                ea[1].grfInheritance = NO_INHERITANCE;
                ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone_sid);

                PACL acl = nullptr;
                if (SetEntriesInAclW(2, ea, nullptr, &acl) == ERROR_SUCCESS)
                {
                    SetNamedSecurityInfoW(
                        const_cast<wchar_t *>(pubkey_path.wstring().c_str()), SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, acl, nullptr);
                    LocalFree(acl);
                }
            }
            CloseHandle(token);
        }
    }
#endif
    // POSIX: 0644 is applied atomically by fchmod inside the open
    // block — guaranteed once we reach this point.  Windows: the
    // DACL block above is best-effort (each Win32 call returns void
    // success on failure; we proceed without retry).  Tightening the
    // Windows path is tracked under task #120.  No additional
    // fs::permissions call is issued on either branch.
}

} // namespace pylabhub::utils
