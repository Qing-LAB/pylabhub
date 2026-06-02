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
 *
 * Vault-file verification (HEP-CORE-0035 §4.6.2):
 *   - mode `(st & 0077) != 0` is an ERROR (group/world-accessible).
 *   - uid mismatch is an ERROR.
 *   - parent dir `(parent.st & 0077) != 0` is a WARN appended to
 *     `diagnostic` with a `; ` separator, NOT a flip of `ok` — some
 *     operators want group-readable parents for shared-host setups.
 *
 * Config-file verification splits into two roles per §4.6.2's
 * "AND file references a keyfile path" qualifier:
 *   - `ConfigFile`: world-writable is ERROR; group-readable is
 *     SILENT (no warn) because the file has no operator-secret
 *     content from the utility's perspective.
 *   - `ConfigFileReferencingVault`: world-writable is ERROR;
 *     group-readable is WARN (appended to diagnostic, ok stays true)
 *     because the path string itself becomes group-visible.
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
        case KeyFileRole::VaultFile:                  return kVaultFileMode;
        case KeyFileRole::VaultDir:                   return kVaultDirMode;
        case KeyFileRole::PublicKeyFile:              return kPublicKeyFileMode;
        case KeyFileRole::ConfigFile:                 return 0;
        case KeyFileRole::ConfigFileReferencingVault: return 0;
    }
    return 0;
}

const char *role_label(KeyFileRole role)
{
    switch (role)
    {
        case KeyFileRole::VaultFile:                  return "vault file";
        case KeyFileRole::VaultDir:                   return "vault directory";
        case KeyFileRole::ConfigFile:                 return "config file";
        case KeyFileRole::ConfigFileReferencingVault: return "config file";
        case KeyFileRole::PublicKeyFile:              return "public-key file";
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

/// HEP-CORE-0035 §4.6.2 parent-directory WARN: when checking a
/// vault FILE, also stat its parent and append a soft warning to
/// `diagnostic` (without flipping `ok` to false) if the parent has
/// group/world bits set.  Per the spec note, "parent dir leak is
/// recoverable; some operators want group-readable parents for
/// shared host setups" — so this is an advisory, not a contract
/// failure.  Concatenates onto existing diagnostic text via "; ".
void append_parent_dir_warning(const fs::path &file_path, AclVerdict &v)
{
    const fs::path parent = file_path.parent_path();
    if (parent.empty())
    {
        return;
    }
    struct ::stat pst{};
    if (::stat(parent.c_str(), &pst) != 0)
    {
        // Parent stat failed; we already passed the file check, so
        // this is a soft signal at most.  Stay silent — surfacing
        // parent-stat errors here would create false positives for
        // legitimate setups where the file is owned-and-readable
        // but the parent dir is search-only (e.g., 0710 parent).
        return;
    }
    const uint32_t pmode = static_cast<uint32_t>(pst.st_mode) & 07777;
    if ((pmode & 0077) == 0)
    {
        return;
    }
    std::ostringstream oss;
    if (!v.diagnostic.empty())
    {
        oss << v.diagnostic << "; ";
    }
    oss << "parent directory " << parent
        << " is group/world-accessible (mode " << octal4(pmode)
        << ") — recoverable per HEP-CORE-0035 §4.6.2 "
           "(some operators want group-readable parents for shared "
           "host setups), but consider: chmod 0700 " << parent;
    v.diagnostic = oss.str();
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

    // Ownership check via the extracted helper; uses `geteuid()` for
    // the expected uid.  Setuid binaries: `chmod` and `open` use the
    // effective uid, so this is the correct comparison.
    const auto own_v = verify_ownership(
        path, KeyFileRole::VaultFile,
        static_cast<uint32_t>(st.st_uid),
        static_cast<uint32_t>(::geteuid()));
    if (!own_v.ok)
    {
        v.ok         = false;
        v.diagnostic = own_v.diagnostic;
        return v;
    }

    v.ok = true;
    append_parent_dir_warning(path, v);
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

    const auto own_v = verify_ownership(
        path, KeyFileRole::VaultDir,
        static_cast<uint32_t>(st.st_uid),
        static_cast<uint32_t>(::geteuid()));
    if (!own_v.ok)
    {
        v.ok         = false;
        v.diagnostic = own_v.diagnostic;
        return v;
    }

    v.ok = true;
    return v;
}

AclVerdict verify_config_file(const fs::path &path, bool references_vault)
{
    const KeyFileRole role = references_vault
        ? KeyFileRole::ConfigFileReferencingVault
        : KeyFileRole::ConfigFile;

    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, role, errno);
    }

    AclVerdict v;
    v.path          = path;
    v.role          = role;
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

    // Group-readable warning fires only when the config file is known
    // to reference a vault path.  HEP-CORE-0035 §4.6.2 qualifies the
    // WARN with "AND file references a keyfile path"; the utility
    // cannot inspect config content, so the caller picks the role.
    if (references_vault && (v.observed_mode & 0040) != 0)
    {
        std::ostringstream oss;
        oss << "config file " << path
            << " is group-readable (mode " << octal4(v.observed_mode)
            << ") — the group can see the referenced vault path.  "
               "Consider: chmod g-r " << path;
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

/// Operator-facing noun for the role.  Inlined here (not the
/// anonymous-namespace `role_label` helper) because this function
/// lives outside the namespace.  Keep in sync with `role_label`.
namespace
{
const char *public_role_noun(KeyFileRole role)
{
    switch (role)
    {
        case KeyFileRole::VaultFile:                  return "vault file";
        case KeyFileRole::VaultDir:                   return "vault directory";
        case KeyFileRole::ConfigFile:                 return "config file";
        case KeyFileRole::ConfigFileReferencingVault: return "config file";
        case KeyFileRole::PublicKeyFile:              return "public-key file";
    }
    return "file";
}
} // namespace

AclVerdict verify_ownership(const fs::path &path,
                            KeyFileRole     role,
                            uint32_t        observed_uid,
                            uint32_t        expected_uid) noexcept
{
    AclVerdict v;
    v.path          = path;
    v.role          = role;
    v.observed_mode = 0;
    v.required_mode = 0;
    if (observed_uid == expected_uid)
    {
        v.ok = true;
        return v;
    }
    v.ok = false;
    try
    {
        const char *noun = public_role_noun(role);
        const char *category =
            (role == KeyFileRole::VaultDir) ? "directory" : "file";
        std::ostringstream oss;
        oss << noun << ' ' << path
            << " owned by uid " << observed_uid
            << "; expected uid " << expected_uid
            << ".  Check " << category << " ownership.";
        v.diagnostic = oss.str();
    }
    catch (...)
    {
        v.diagnostic =
            "ownership mismatch (uid comparison failed; "
            "operator-facing diagnostic could not be formatted).";
    }
    return v;
}

fs::path resolve_keyfile_path(const std::string &keyfile,
                              const fs::path    &base_dir) noexcept
{
    try
    {
        if (keyfile.empty())
        {
            return {};
        }
        fs::path p(keyfile);
        if (p.is_absolute())
        {
            return p;
        }
        return base_dir / p;
    }
    catch (...)
    {
        // path construction can throw bad_alloc on extremely long
        // strings; return empty as a defensive fallback.  Empty
        // results never reach the runtime under the normal contract
        // (`auth.keyfile` is REQUIRED non-empty per HEP-CORE-0024
        // §3.4 / HEP-CORE-0033 §7.1, rejected at config-load), so
        // this branch is reached only in an OOM scenario; the
        // binary is going to die anyway.
        return {};
    }
}

bool keyfile_inside_base_dir(const std::string &keyfile,
                              const fs::path    &base_dir) noexcept
{
    if (keyfile.empty())
        return false;

    std::error_code ec;
    const fs::path kf_raw(keyfile);
    const fs::path kf = fs::weakly_canonical(
        kf_raw.is_absolute() ? kf_raw : (base_dir / kf_raw), ec);
    if (ec)
        return false;
    const fs::path base = fs::weakly_canonical(base_dir, ec);
    if (ec)
        return false;

    // Component-by-component prefix check.  An empty `base` would make
    // the algorithm vacuously match anything; refuse to claim
    // containment in that degenerate case.
    if (base.empty())
        return false;
    auto [base_end, kf_it] =
        std::mismatch(base.begin(), base.end(), kf.begin(), kf.end());
    return base_end == base.end();
}

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
            case KeyFileRole::VaultFile:
                return verify_vault_file(path);
            case KeyFileRole::VaultDir:
                return verify_vault_dir(path);
            case KeyFileRole::ConfigFile:
                return verify_config_file(path, /*references_vault=*/false);
            case KeyFileRole::ConfigFileReferencingVault:
                return verify_config_file(path, /*references_vault=*/true);
            case KeyFileRole::PublicKeyFile:
                return verify_public_key_file(path);
        }
        AclVerdict v;
        v.ok            = false;
        v.path          = path;
        v.role          = role;
        v.observed_mode = 0;
        v.required_mode = 0;
        v.diagnostic = "internal error: unhandled KeyFileRole enumerator.";
        return v;
    }
    catch (...)
    {
        // Defensive fallback: stream/path operations theoretically can
        // throw under OOM or path-format errors.  Do not claim the
        // role is unrecognized — be specific about what happened so
        // operator diagnosis isn't misled.
        AclVerdict v;
        v.ok            = false;
        v.path          = path;
        v.role          = role;
        v.observed_mode = 0;
        v.required_mode = 0;
        v.diagnostic =
            "internal error during ACL verification (likely OOM or "
            "unexpected exception while formatting the diagnostic).";
        return v;
    }
#endif
}

SetModeResult set_keyfile_mode(const fs::path &path, KeyFileRole role) noexcept
{
#ifdef _WIN32
    (void) path;
    (void) role;
    return SetModeResult::Applied;
#else
    try
    {
        const uint32_t mode = required_mode_for(role);
        if (mode == 0)
        {
            // ConfigFile and ConfigFileReferencingVault have no
            // canonical mode; operator owns them.
            return SetModeResult::NoCanonicalMode;
        }
        if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0)
        {
            return SetModeResult::ChmodFailed;
        }
        return SetModeResult::Applied;
    }
    catch (...)
    {
        return SetModeResult::ChmodFailed;
    }
#endif
}

} // namespace pylabhub::utils::security
