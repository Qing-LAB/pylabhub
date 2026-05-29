/**
 * @file key_file_acl.cpp
 * @brief Implementation of file-mode and ownership discipline (HEP-CORE-0035 §4.6).
 *
 * POSIX path uses `<sys/stat.h>` for `stat` + `chmod` and `<unistd.h>`
 * for `geteuid`.  Windows is a no-op success path; the C++ standard
 * filesystem model exposes no equivalent of POSIX group/world bits.
 *
 * The diagnostic strings follow the OpenSSH operator-message
 * convention: name the offending path, observed mode (octal),
 * required mode (octal), and the exact `chmod` command to fix.  The
 * caller surfaces the diagnostic — this TU never logs (so the
 * utility stays callable before LifecycleGuard construction at
 * binary startup).
 */
#include "utils/security/key_file_acl.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#  include <io.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace pylabhub::utils::security
{

namespace
{

namespace fs = std::filesystem;

#ifndef _WIN32

constexpr uint32_t kVaultFileMode     = 0600;
constexpr uint32_t kVaultDirMode      = 0700;
constexpr uint32_t kPublicKeyFileMode = 0644;

/// Format `mode` as a four-character zero-padded octal literal (e.g.
/// `0600`).  Used in operator-facing diagnostics so the rendered text
/// matches the `chmod` argument the operator must use.
std::string octal4(uint32_t mode)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0%03o", mode & 07777);
    return std::string(buf);
}

uint32_t required_mode_for(KeyFileRole role)
{
    switch (role)
    {
        case KeyFileRole::VaultFile:     return kVaultFileMode;
        case KeyFileRole::VaultDir:      return kVaultDirMode;
        case KeyFileRole::PublicKeyFile: return kPublicKeyFileMode;
        case KeyFileRole::ConfigFile:    return 0;
    }
    return 0;
}

const char *role_label(KeyFileRole role)
{
    switch (role)
    {
        case KeyFileRole::VaultFile:     return "vault file";
        case KeyFileRole::VaultDir:      return "vault directory";
        case KeyFileRole::ConfigFile:    return "config file";
        case KeyFileRole::PublicKeyFile: return "public-key file";
    }
    return "file";
}

AclVerdict stat_failure_verdict(const fs::path &path,
                                KeyFileRole     role,
                                int             errno_value)
{
    AclVerdict v;
    v.ok            = false;
    v.path          = path;
    v.role          = role;
    v.observed_mode = 0;
    v.required_mode = required_mode_for(role);
    std::ostringstream oss;
    oss << role_label(role) << ' ' << path
        << " cannot be stat()'d: "
        << std::generic_category().message(errno_value)
        << " (errno " << errno_value << ").  Check that the path "
           "exists and the current user has search permission on "
           "every parent directory.";
    v.diagnostic = oss.str();
    return v;
}

AclVerdict verify_vault_file(const fs::path &path)
{
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, KeyFileRole::VaultFile, errno);
    }

    AclVerdict v;
    v.path          = path;
    v.role          = KeyFileRole::VaultFile;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = kVaultFileMode;

    if ((v.observed_mode & 0077) != 0)
    {
        std::ostringstream oss;
        oss << "vault file " << path
            << " is group/world-accessible (mode " << octal4(v.observed_mode)
            << ").  Run: chmod " << octal4(kVaultFileMode) << ' ' << path;
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    const uid_t euid = ::geteuid();
    if (st.st_uid != euid)
    {
        std::ostringstream oss;
        oss << "vault file " << path
            << " owned by uid " << st.st_uid
            << "; expected uid " << euid
            << ".  Check file ownership.";
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    v.ok = true;
    return v;
}

AclVerdict verify_vault_dir(const fs::path &path)
{
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, KeyFileRole::VaultDir, errno);
    }

    AclVerdict v;
    v.path          = path;
    v.role          = KeyFileRole::VaultDir;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = kVaultDirMode;

    if (!S_ISDIR(st.st_mode))
    {
        std::ostringstream oss;
        oss << "vault directory " << path
            << " is not a directory (mode " << octal4(v.observed_mode)
            << ").  Run: rm-and-recreate via plh_hub --init / plh_role --init.";
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    if ((v.observed_mode & 0077) != 0)
    {
        std::ostringstream oss;
        oss << "vault directory " << path
            << " is group/world-accessible (mode " << octal4(v.observed_mode)
            << ").  Run: chmod " << octal4(kVaultDirMode) << ' ' << path;
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    const uid_t euid = ::geteuid();
    if (st.st_uid != euid)
    {
        std::ostringstream oss;
        oss << "vault directory " << path
            << " owned by uid " << st.st_uid
            << "; expected uid " << euid
            << ".  Check directory ownership.";
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    v.ok = true;
    return v;
}

AclVerdict verify_config_file(const fs::path &path)
{
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, KeyFileRole::ConfigFile, errno);
    }

    AclVerdict v;
    v.path          = path;
    v.role          = KeyFileRole::ConfigFile;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = 0;

    if ((v.observed_mode & 0002) != 0)
    {
        std::ostringstream oss;
        oss << "config file " << path
            << " is world-writable (mode " << octal4(v.observed_mode)
            << ") — a writable config file is a config-injection "
               "vector.  Run: chmod o-w " << path;
        v.ok         = false;
        v.diagnostic = oss.str();
        return v;
    }

    // Group-readable when referencing a vault is a warning, not a
    // hard failure (HEP-CORE-0035 §4.6.2: parent-dir-leak warnings
    // are recoverable; same principle applies here).  Reflect it in
    // the diagnostic but keep `ok = true`.
    if ((v.observed_mode & 0040) != 0)
    {
        std::ostringstream oss;
        oss << "config file " << path
            << " is group-readable (mode " << octal4(v.observed_mode)
            << ") — recoverable, but if this file references a vault "
               "path the group can see the path string.  Consider: "
               "chmod g-r " << path;
        v.diagnostic = oss.str();
    }

    v.ok = true;
    return v;
}

AclVerdict verify_public_key_file(const fs::path &path)
{
    // HEP-CORE-0035 §4.6.2: pubkeys are intentionally distributable;
    // no mode check.  We still stat the file to surface a
    // "missing file" diagnostic if the path is wrong, which is the
    // most common operator mistake when copying pubkeys between
    // hosts.
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, KeyFileRole::PublicKeyFile, errno);
    }

    AclVerdict v;
    v.ok            = true;
    v.path          = path;
    v.role          = KeyFileRole::PublicKeyFile;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = kPublicKeyFileMode;
    return v;
}

#endif // !_WIN32

} // namespace

AclVerdict verify_keyfile_acl(const fs::path &path, KeyFileRole role) noexcept
{
#ifdef _WIN32
    // FUTURE EXTENSION (HEP-CORE-0035 §4.6 follow-up): Windows DACL
    // check.  Today this is a no-op success — the vault file's
    // encryption-at-rest layer (libsodium Argon2id + XSalsa20-Poly1305
    // via vault_crypto.cpp) is cross-platform and remains the primary
    // protection on Windows; the POSIX mode-bit floor is a defence-
    // in-depth layer that simply does not apply.  An NTFS-aware
    // hardening pass would call GetSecurityInfo() to verify the
    // file's DACL grants access only to the current owner SID.  Not
    // wired today because (a) HEP-0035 §4.6 declares UNIX mode bits
    // as the enforcement scope, (b) Windows operators are typically
    // expected to run the binary under a dedicated service account
    // whose ACLs are managed operator-side.  Worth revisiting if
    // Windows operators report local-read attack surface.
    (void) path;
    (void) role;
    AclVerdict v;
    v.ok            = true;
    v.path          = path;
    v.role          = role;
    v.observed_mode = 0;
    v.required_mode = 0;
    v.diagnostic =
        "Windows: POSIX mode checks are not applicable; ACL "
        "discipline is platform-dependent.  HEP-CORE-0035 §4.6 "
        "enforces UNIX mode bits only.  Vault contents remain "
        "encrypted-at-rest via libsodium (cross-platform).";
    return v;
#else
    try
    {
        switch (role)
        {
            case KeyFileRole::VaultFile:     return verify_vault_file(path);
            case KeyFileRole::VaultDir:      return verify_vault_dir(path);
            case KeyFileRole::ConfigFile:    return verify_config_file(path);
            case KeyFileRole::PublicKeyFile: return verify_public_key_file(path);
        }
    }
    catch (...)
    {
        // path construction can throw; keep verdict shape sane.
    }
    AclVerdict v;
    v.ok            = false;
    v.path          = path;
    v.role          = role;
    v.observed_mode = 0;
    v.required_mode = 0;
    v.diagnostic    = "internal error: unrecognized KeyFileRole.";
    return v;
#endif
}

bool set_keyfile_mode(const fs::path &path, KeyFileRole role) noexcept
{
#ifdef _WIN32
    (void) path;
    (void) role;
    return true;
#else
    try
    {
        const uint32_t mode = required_mode_for(role);
        if (mode == 0)
        {
            // ConfigFile has no canonical mode; the operator owns it.
            return false;
        }
        if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0)
        {
            return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
#endif
}

} // namespace pylabhub::utils::security
