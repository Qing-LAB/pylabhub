#pragma once
/**
 * @file key_file_acl.hpp
 * @brief File-mode and ownership discipline for secret-bearing paths.
 *
 * Per HEP-CORE-0035 §4.6, the cryptographic strength of pylabhub's
 * layered authentication (Layer-1 ZAP allowlist, Layer-2 federation
 * trust, Layer-3 data-plane CURVE — HEP-CORE-0036) rests on two
 * operational trust anchors:
 *
 *   (1) Secret-key file confidentiality — the encrypted vault file
 *       MUST NOT be readable by any process other than the owner.
 *   (2) Allowlist integrity — the vault directory MUST NOT be
 *       writable by anyone other than the owner.  (HEP-CORE-0035 §4.8
 *       further protects the known-roles allowlist by storing it
 *       inside the encrypted vault payload; this file-mode check is
 *       the file-system floor.)
 *
 * This module is the shared utility both binaries (`plh_hub`,
 * `plh_role`) call at startup and at write time.  It has two
 * responsibilities:
 *
 *   - `set_keyfile_mode(path, role)` — applied at write time (per
 *     `--keygen` / `--init`) so the mode does not depend on the
 *     operator's umask (HEP-CORE-0035 §4.6.1).
 *   - `verify_keyfile_acl(path, role)` — applied at every binary
 *     startup before any secret is read (HEP-CORE-0035 §4.6.2).  On
 *     violation, returns an OpenSSH-style actionable error in
 *     `AclVerdict::diagnostic` (path, observed mode, required mode,
 *     exact `chmod` command to fix).
 *
 * Both functions are `noexcept` and do not log.  Callers decide how
 * to surface failures (stderr vs LOGGER_ERROR vs propagated error
 * value) — this keeps the utility callable from very early binary
 * startup where the Logger lifecycle module may not be initialized.
 *
 * Cross-platform: POSIX (Linux, macOS) performs real mode and owner
 * checks.  Windows has no equivalent of POSIX group/world bits in the
 * standard file model; `verify_keyfile_acl` returns `ok = true` with
 * a platform-skip diagnostic, and `set_keyfile_mode` is a no-op
 * returning `true`.
 *
 * @see HEP-CORE-0035 §4.6 (file ACL discipline) + §4.8 (known-roles
 *      in vault, the higher-level integrity layer)
 * @see HEP-CORE-0036 §3 I1 (the two-condition gate this floor protects)
 */
#include "pylabhub_utils_export.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace pylabhub::utils::security
{

/// Roles that the ACL utility recognizes.  Each role implies the
/// required mode (POSIX) and ownership contract per HEP-CORE-0035
/// §4.6.1.  Callers pass a role tag so a single utility can serve
/// every secret-bearing path the codebase emits without re-stating
/// the mode at each site.
enum class KeyFileRole : int
{
    /// Encrypted vault file (`hub.vault`, `<role_uid>.vault`).
    /// Required: mode 0600, owner == current euid (POSIX).
    VaultFile,

    /// Vault parent directory (`<hub_dir>/vault`, `<role_dir>/vault`).
    /// Required: mode 0700, owner == current euid (POSIX).
    VaultDir,

    /// Hub/role configuration file (`hub.json` and role config).
    /// Required: not world-writable.  HEP-CORE-0035 §4.6.2 also
    /// warns on group-readable when the file references a vault
    /// path; the warning is reflected in `diagnostic` but does not
    /// flip `ok` to false.
    ConfigFile,

    /// Distributable public-key file (`hub.pubkey`, locally-cached
    /// role pubkeys).  Required at write: 0644.  Per HEP-CORE-0035
    /// §4.6.2, no startup verification — pubkeys are intentionally
    /// distributable and may legitimately be group- or world-
    /// readable.  Listed here so the API is symmetric and so the
    /// write-time setter has a canonical mode.
    PublicKeyFile,
};

/// Result of `verify_keyfile_acl`.
///
/// `diagnostic` is empty when `ok` is true.  When `ok` is false, it
/// holds an OpenSSH-style operator-facing message: it names the
/// offending path, the observed mode, the required mode, and the
/// exact `chmod` command that would fix the violation (HEP-CORE-0035
/// §4.6.2).
struct AclVerdict
{
    bool                  ok{false};
    std::filesystem::path path{};
    KeyFileRole           role{KeyFileRole::VaultFile};
    uint32_t              observed_mode{0};
    uint32_t              required_mode{0};
    std::string           diagnostic{};
};

/// Verify that `path` satisfies the ACL contract for `role`.
///
/// On POSIX: stats the file; compares the low 12 bits of `st_mode`
/// and `st_uid` to the role's contract.  Never throws; filesystem
/// errors (missing path, EACCES) land in `verdict.diagnostic` with
/// `ok = false`.
///
/// On Windows: returns `ok = true` with `diagnostic` noting the
/// platform skip.
[[nodiscard]] PYLABHUB_UTILS_EXPORT AclVerdict
verify_keyfile_acl(const std::filesystem::path &path,
                   KeyFileRole                   role) noexcept;

/// Set the mode of `path` to the canonical value for `role` per
/// HEP-CORE-0035 §4.6.1.
///
/// POSIX mapping:
///   - VaultFile     → 0600
///   - VaultDir      → 0700
///   - PublicKeyFile → 0644
///   - ConfigFile    → no canonical mode (operator-managed); returns
///                     false without touching the file.
///
/// On Windows: no-op returning `true`.
///
/// Returns false on POSIX `chmod` failure or when called with
/// `ConfigFile`.  Does not log; the caller decides how to surface.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool
set_keyfile_mode(const std::filesystem::path &path,
                 KeyFileRole                   role) noexcept;

} // namespace pylabhub::utils::security
