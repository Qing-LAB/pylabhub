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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pylabhub::utils::security
{

namespace
{

namespace fs = std::filesystem;

#ifndef _WIN32

constexpr uint32_t kVaultFileMode = 0600;
constexpr uint32_t kVaultDirMode = 0700;
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
    case KeyFileRole::VaultFile:
        return kVaultFileMode;
    case KeyFileRole::VaultDir:
        return kVaultDirMode;
    case KeyFileRole::PublicKeyFile:
        return kPublicKeyFileMode;
    case KeyFileRole::ConfigFile:
        return 0;
    case KeyFileRole::ConfigFileReferencingVault:
        return 0;
    }
    return 0;
}

const char *role_label(KeyFileRole role)
{
    switch (role)
    {
    case KeyFileRole::VaultFile:
        return "vault file";
    case KeyFileRole::VaultDir:
        return "vault directory";
    case KeyFileRole::ConfigFile:
        return "config file";
    case KeyFileRole::ConfigFileReferencingVault:
        return "config file";
    case KeyFileRole::PublicKeyFile:
        return "public-key file";
    }
    return "file";
}

AclVerdict stat_failure_verdict(const fs::path &path, KeyFileRole role, int errno_value)
{
    AclVerdict v;
    v.ok = false;
    v.path = path;
    v.role = role;
    v.observed_mode = 0;
    v.required_mode = required_mode_for(role);
    std::ostringstream oss;
    oss << role_label(role) << ' ' << path
        << " cannot be stat()'d: " << std::generic_category().message(errno_value) << " (errno "
        << errno_value
        << ").  Check that the path "
           "exists and the current user has search permission on "
           "every parent directory.";
    v.diagnostic = oss.str();
    return v;
}

/// HEP-CORE-0035 §4.6.2 parent-directory check.  The mask is SPLIT by
/// consequence, because the two halves are not the same kind of problem:
///
///   - WRITE bits (0022) WITHOUT the sticky bit → ERROR.  Permission to
///     replace a directory entry comes from the DIRECTORY, not the file, so
///     any such user can `rename(2)` or `unlink`+recreate the vault and
///     substitute the hub's identity key — the file's own 0600 does not
///     enter into it.
///   - WRITE bits WITH the sticky bit (S_ISVTX, 01000) → NOT an error.  The
///     sticky bit exists for exactly this: on such a directory only the
///     file's owner, the directory's owner, or root may rename or unlink a
///     file.  `/tmp` is mode 01777 and is a correct, safe location — a rule
///     that rejected it would be a false positive, which is how this was
///     first written and what the L2 suite caught.
///   - READ/EXEC bits (0055) → WARN, `ok` unchanged.  These leak only the
///     file's existence and name; per the spec note, "some operators want
///     group-readable parents for shared host setups".
///
/// Amended 2026-07-30 (review S-1).  This was one `(pmode & 0077)` test
/// producing a WARN for every bit, with the group-readable rationale quoted
/// as justification — a reason about VISIBILITY used to downgrade a
/// REPLACEMENT vector.  It also disagreed with `verify_vault_dir`, which
/// already hard-fails on the same mask.
///
/// Appends onto existing diagnostic text via "; ".
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

    // Writable parent WITHOUT sticky — an ERROR, overriding any prior verdict.
    // With sticky set the replacement vector is closed by the kernel, so a
    // world-writable /tmp (01777) is fine and must not be rejected.
    const bool sticky = (pst.st_mode & S_ISVTX) != 0;
    if ((pmode & 0022) != 0 && !sticky)
    {
        std::ostringstream oss;
        if (!v.diagnostic.empty())
        {
            oss << v.diagnostic << "; ";
        }
        oss << "parent directory " << parent << " is group/world-WRITABLE (mode " << octal4(pmode)
            << ") and NOT sticky — any such user can rename(2) or unlink the "
               "vault file and substitute their own; the vault's own 0600 does "
               "not prevent it (HEP-CORE-0035 §4.6.2).  Run: chmod go-w "
            << parent;
        v.ok = false;
        v.diagnostic = oss.str();
        return;
    }

    // Readable/searchable parent — advisory only.
    if ((pmode & 0055) == 0)
    {
        return;
    }
    std::ostringstream oss;
    if (!v.diagnostic.empty())
    {
        oss << v.diagnostic << "; ";
    }
    oss << "parent directory " << parent << " is group/world-readable (mode " << octal4(pmode)
        << ") — leaks the vault's existence and name only; recoverable per "
           "HEP-CORE-0035 §4.6.2 (some operators want group-readable parents "
           "for shared host setups), but consider: chmod 0700 "
        << parent;
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
    v.path = path;
    v.role = KeyFileRole::VaultFile;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = kVaultFileMode;

    if ((v.observed_mode & 0077) != 0)
    {
        std::ostringstream oss;
        oss << "vault file " << path << " is group/world-accessible (mode "
            << octal4(v.observed_mode) << ").  Run: chmod " << octal4(kVaultFileMode) << ' '
            << path;
        v.ok = false;
        v.diagnostic = oss.str();
        return v;
    }

    // Ownership check via the extracted helper; uses `geteuid()` for
    // the expected uid.  Setuid binaries: `chmod` and `open` use the
    // effective uid, so this is the correct comparison.
    const auto own_v =
        verify_ownership(path, KeyFileRole::VaultFile, static_cast<uint32_t>(st.st_uid),
                         static_cast<uint32_t>(::geteuid()));
    if (!own_v.ok)
    {
        v.ok = false;
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
    v.path = path;
    v.role = KeyFileRole::VaultDir;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = kVaultDirMode;

    if (!S_ISDIR(st.st_mode))
    {
        std::ostringstream oss;
        oss << "vault directory " << path << " is not a directory (mode " << octal4(v.observed_mode)
            << ").  Run: rm-and-recreate via plh_hub --init / plh_role --init.";
        v.ok = false;
        v.diagnostic = oss.str();
        return v;
    }

    if ((v.observed_mode & 0077) != 0)
    {
        std::ostringstream oss;
        oss << "vault directory " << path << " is group/world-accessible (mode "
            << octal4(v.observed_mode) << ").  Run: chmod " << octal4(kVaultDirMode) << ' ' << path;
        v.ok = false;
        v.diagnostic = oss.str();
        return v;
    }

    const auto own_v =
        verify_ownership(path, KeyFileRole::VaultDir, static_cast<uint32_t>(st.st_uid),
                         static_cast<uint32_t>(::geteuid()));
    if (!own_v.ok)
    {
        v.ok = false;
        v.diagnostic = own_v.diagnostic;
        return v;
    }

    v.ok = true;
    return v;
}

AclVerdict verify_config_file(const fs::path &path, bool references_vault)
{
    const KeyFileRole role =
        references_vault ? KeyFileRole::ConfigFileReferencingVault : KeyFileRole::ConfigFile;

    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return stat_failure_verdict(path, role, errno);
    }

    AclVerdict v;
    v.path = path;
    v.role = role;
    v.observed_mode = static_cast<uint32_t>(st.st_mode) & 07777;
    v.required_mode = 0;

    if ((v.observed_mode & 0002) != 0)
    {
        std::ostringstream oss;
        oss << "config file " << path << " is world-writable (mode " << octal4(v.observed_mode)
            << ") — a writable config file is a config-injection "
               "vector.  Run: chmod o-w "
            << path;
        v.ok = false;
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
        oss << "config file " << path << " is group-readable (mode " << octal4(v.observed_mode)
            << ") — the group can see the referenced vault path.  "
               "Consider: chmod g-r "
            << path;
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
    v.ok = true;
    v.path = path;
    v.role = KeyFileRole::PublicKeyFile;
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
    case KeyFileRole::VaultFile:
        return "vault file";
    case KeyFileRole::VaultDir:
        return "vault directory";
    case KeyFileRole::ConfigFile:
        return "config file";
    case KeyFileRole::ConfigFileReferencingVault:
        return "config file";
    case KeyFileRole::PublicKeyFile:
        return "public-key file";
    }
    return "file";
}
} // namespace

AclVerdict verify_ownership(const fs::path &path, KeyFileRole role, uint32_t observed_uid,
                            uint32_t expected_uid) noexcept
{
    AclVerdict v;
    v.path = path;
    v.role = role;
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
        const char *category = (role == KeyFileRole::VaultDir) ? "directory" : "file";
        std::ostringstream oss;
        oss << noun << ' ' << path << " owned by uid " << observed_uid << "; expected uid "
            << expected_uid << ".  Check " << category << " ownership.";
        v.diagnostic = oss.str();
    }
    catch (...)
    {
        v.diagnostic = "ownership mismatch (uid comparison failed; "
                       "operator-facing diagnostic could not be formatted).";
    }
    return v;
}

fs::path resolve_keyfile_path(const std::string &keyfile, const fs::path &base_dir) noexcept
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

bool keyfile_inside_base_dir(const std::string &keyfile, const fs::path &base_dir,
                             std::string *out_canonicalize_error) noexcept
{
    if (out_canonicalize_error != nullptr)
        out_canonicalize_error->clear();
    if (keyfile.empty())
        return false;

    std::error_code ec;
    const fs::path kf_raw(keyfile);
    const fs::path kf =
        fs::weakly_canonical(kf_raw.is_absolute() ? kf_raw : (base_dir / kf_raw), ec);
    if (ec)
    {
        if (out_canonicalize_error != nullptr)
            *out_canonicalize_error = "weakly_canonical(keyfile) failed: " + ec.message();
        return false;
    }
    const fs::path base = fs::weakly_canonical(base_dir, ec);
    if (ec)
    {
        if (out_canonicalize_error != nullptr)
            *out_canonicalize_error = "weakly_canonical(base_dir) failed: " + ec.message();
        return false;
    }

    // Component-by-component prefix check.  An empty `base` would make
    // the algorithm vacuously match anything; refuse to claim
    // containment in that degenerate case.
    if (base.empty())
        return false;
    auto [base_end, kf_it] = std::mismatch(base.begin(), base.end(), kf.begin(), kf.end());
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
    (void)path;
    (void)role;
    AclVerdict v;
    v.ok = true;
    v.path = path;
    v.role = role;
    v.observed_mode = 0;
    v.required_mode = 0;
    v.diagnostic = "Windows: POSIX mode checks are not applicable; ACL "
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
        v.ok = false;
        v.path = path;
        v.role = role;
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
        v.ok = false;
        v.path = path;
        v.role = role;
        v.observed_mode = 0;
        v.required_mode = 0;
        v.diagnostic = "internal error during ACL verification (likely OOM or "
                       "unexpected exception while formatting the diagnostic).";
        return v;
    }
#endif
}

SetModeResult set_keyfile_mode(const fs::path &path, KeyFileRole role, int *out_errno) noexcept
{
    if (out_errno != nullptr)
        *out_errno = 0;
#ifdef _WIN32
    (void)path;
    (void)role;
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
        // fchmod on an O_NOFOLLOW fd, not chmod on the path (review S-2).
        // `chmod(path)` follows symlinks, so a link planted at `path` would
        // have had the mode applied to its TARGET — tightening someone else's
        // file while leaving the vault path itself unprotected.  Opening with
        // O_NOFOLLOW first means we can only ever chmod the thing we looked
        // at.  This is the same fd-based discipline `vault_crypto.cpp`
        // already uses to defeat a pathological umask.
        const int fd = ::open(path.c_str(),
                              (role == KeyFileRole::VaultDir ? O_RDONLY | O_DIRECTORY : O_RDONLY) |
                                  O_NOFOLLOW | O_CLOEXEC);
        if (fd == -1)
        {
            if (out_errno != nullptr)
                *out_errno = errno;
            return SetModeResult::ChmodFailed;
        }
        if (::fchmod(fd, static_cast<mode_t>(mode)) != 0)
        {
            if (out_errno != nullptr)
                *out_errno = errno;
            ::close(fd);
            return SetModeResult::ChmodFailed;
        }
        ::close(fd);
        return SetModeResult::Applied;
    }
    catch (...)
    {
        // Defensive only — the try block constructs no paths; the only
        // allocations are libc-internal under chmod or compiler-inserted
        // RAII, neither expected to throw in practice.  Every syscall
        // path above already returned its own ChmodFailed + errno.
        // Populate ENOMEM here so callers don't see strerror(0) →
        // "Success" in the operator message.
        if (out_errno != nullptr)
            *out_errno = ENOMEM;
        return SetModeResult::ChmodFailed;
    }
#endif
}

// ── write_keyfile / atomic_write_owner_only_file ────────────────────────────

namespace
{

#ifdef _WIN32
/// Windows owner-only write: temp file + DACL granting the calling user
/// full access only + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`.
/// Atomicity is best-effort on NTFS.  Deduplicated when #120 hardens the
/// Windows path.
void win32_write_owner_only(const fs::path &path, std::string_view contents)
{
    const fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("write_keyfile: cannot open tmp '" + tmp_path.string() + "'");
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!ofs)
            throw std::runtime_error("write_keyfile: write failed for tmp '" + tmp_path.string() +
                                     "'");
    }
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<uint8_t> buf(len);
        if (GetTokenInformation(token, TokenUser, buf.data(), len, &len))
        {
            auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());
            EXPLICIT_ACCESS_W ea{};
            ea.grfAccessPermissions = GENERIC_ALL;
            ea.grfAccessMode = SET_ACCESS;
            ea.grfInheritance = NO_INHERITANCE;
            ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
            PACL acl = nullptr;
            if (SetEntriesInAclW(1, &ea, nullptr, &acl) == ERROR_SUCCESS)
            {
                SetNamedSecurityInfoW(
                    const_cast<wchar_t *>(tmp_path.wstring().c_str()), SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                    nullptr, acl, nullptr);
                LocalFree(acl);
            }
        }
        CloseHandle(token);
    }
    if (!MoveFileExW(tmp_path.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("write_keyfile: MoveFileExW failed for '" + path.string() + "'");
}
#else
/// Normalize the mode against a hostile `umask`, write every byte, make
/// it durable, and close — unlinking @p cleanup_path on any failure so
/// no partial file is ever left behind.  Takes ownership of @p fd.
///
/// `fsync` before close is what makes the subsequent `rename(2)` a real
/// commit: without it the rename can survive a power cut while the data
/// pages are still only in the page cache, leaving a correctly-named
/// file with garbage in it.  Only one of the three original writers did
/// this; now all of them do.
void write_all_and_close_or_unlink(int fd, const fs::path &target, const fs::path &cleanup_path,
                                   std::string_view contents, int mode_bits, const char *who)
{
    const auto fail = [&](const char *what, int err) -> std::runtime_error
    {
        ::close(fd);
        ::unlink(cleanup_path.c_str());
        return std::runtime_error(std::string{who} + ": " + what + " failed for '" +
                                  target.string() + "': " + std::strerror(err));
    };

    if (::fchmod(fd, static_cast<mode_t>(mode_bits)) != 0)
        throw fail("fchmod", errno);

    const char *buf = contents.data();
    std::size_t remaining = contents.size();
    while (remaining > 0)
    {
        const ssize_t n = ::write(fd, buf, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            throw fail("write", errno);
        }
        buf += n;
        remaining -= static_cast<std::size_t>(n);
    }

    (void)::fsync(fd);

    if (::close(fd) != 0)
    {
        const int err = errno;
        ::unlink(cleanup_path.c_str());
        throw std::runtime_error(std::string{who} + ": close failed for '" + target.string() +
                                 "': " + std::strerror(err));
    }
}
#endif

/// The single implementation of HEP-CORE-0035 §4.6.1's write recipe.
/// `write_keyfile` and `atomic_write_owner_only_file` are both thin
/// callers; nothing else in the tree should open a protected file for
/// writing by hand.
void write_protected_file(const fs::path &path, std::string_view contents, uint32_t mode,
                          ExistingFilePolicy policy)
{
#ifdef _WIN32
    // Refuse is a pre-check here, not an atomic guarantee — see the
    // Windows caveat on `write_keyfile`.  It replaces a branch that
    // truncated unconditionally, so the check is a strict improvement
    // even though it is not race-free.
    if (policy == ExistingFilePolicy::Refuse && fs::exists(path))
    {
        throw std::runtime_error("write_keyfile: file already exists at '" + path.string() +
                                 "' — refusing to overwrite");
    }
    (void)mode; // Windows uses the DACL below, not POSIX mode bits.
    win32_write_owner_only(path, contents);
#else
    const int mode_bits = static_cast<int>(mode);

    if (policy == ExistingFilePolicy::Refuse)
    {
        // Write the target directly under O_EXCL: the kernel's
        // create-or-fail IS the no-clobber guarantee, so there is no
        // check-then-act window for a racing creator to slip through.
        const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
                              static_cast<mode_t>(mode_bits));
        if (fd < 0)
        {
            const int err = errno;
            if (err == EEXIST)
                throw std::runtime_error("write_keyfile: file already exists at '" + path.string() +
                                         "' — refusing to overwrite (atomic O_EXCL guard, "
                                         "HEP-CORE-0035 §4.6.1)");
            if (err == ELOOP)
                throw std::runtime_error("write_keyfile: '" + path.string() +
                                         "' is a symbolic link — refusing to follow "
                                         "(atomic O_NOFOLLOW guard, HEP-CORE-0035 §4.6.1)");
            throw std::runtime_error("write_keyfile: cannot create '" + path.string() +
                                     "': " + std::strerror(err));
        }
        write_all_and_close_or_unlink(fd, path, path, contents, mode_bits, "write_keyfile");
        return;
    }

    // Replace: sibling temp + rename(2).  rename is atomic on POSIX
    // even when the target exists, so a reader sees the old file or the
    // new one — never a partial write, and never a missing file (which
    // an unlink-then-create sequence would expose).
    const fs::path tmp_path = path.string() + ".tmp";
    ::unlink(tmp_path.c_str()); // best-effort; ENOENT is fine

    const int fd =
        ::open(tmp_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_TRUNC,
               static_cast<mode_t>(mode_bits));
    if (fd < 0)
    {
        const int err = errno;
        if (err == ELOOP)
            throw std::runtime_error("write_keyfile: tmp '" + tmp_path.string() +
                                     "' is a symbolic link — refusing to follow (O_NOFOLLOW)");
        throw std::runtime_error("write_keyfile: cannot create tmp '" + tmp_path.string() +
                                 "': " + std::strerror(err));
    }
    write_all_and_close_or_unlink(fd, tmp_path, tmp_path, contents, mode_bits, "write_keyfile");

    if (::rename(tmp_path.c_str(), path.c_str()) != 0)
    {
        const int err = errno;
        ::unlink(tmp_path.c_str());
        throw std::runtime_error("write_keyfile: rename to '" + path.string() +
                                 "' failed: " + std::strerror(err));
    }
#endif
}

} // namespace

void write_keyfile(const fs::path &path, std::string_view contents, KeyFileRole role,
                   ExistingFilePolicy policy)
{
    const uint32_t mode = required_mode_for(role);
    if (mode == 0)
    {
        // ConfigFile / ConfigFileReferencingVault. The operator owns
        // those modes (see `required_mode_for`), so this function has no
        // mode to assert and must not invent one.
        throw std::invalid_argument(
            std::string{"write_keyfile: role '"} + role_label(role) +
            "' has no canonical mode — the operator owns config-file permissions "
            "(HEP-CORE-0035 §4.6.1); use a caller-chosen write path instead");
    }
    write_protected_file(path, contents, mode, policy);
}

void atomic_write_owner_only_file(const fs::path &path, std::string_view contents)
{
    // known_roles.json is an owner-only DATA file, not a KeyFileRole —
    // 0600 is passed directly rather than borrowing `VaultFile` to
    // reach the same number.
    write_protected_file(path, contents, kVaultFileMode, ExistingFilePolicy::Replace);
}


} // namespace pylabhub::utils::security
