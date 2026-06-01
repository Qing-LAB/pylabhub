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
    /// Required: mode 0600, owner == current euid (POSIX).  The
    /// verify also stats the file's parent directory and emits a
    /// soft WARN into `diagnostic` (without flipping `ok` to false)
    /// if `(parent.st_mode & 0077) != 0`, per HEP-CORE-0035 §4.6.2.
    /// The WARN concatenates onto any other diagnostic text using
    /// `; ` as separator.
    VaultFile,

    /// Vault parent directory (`<hub_dir>/vault`, `<role_dir>/vault`).
    /// Required: mode 0700, owner == current euid (POSIX).
    VaultDir,

    /// Hub/role configuration file (`hub.json` and role config) that
    /// does NOT reference a vault keyfile path.  Required: not
    /// world-writable.  Group-readable is permitted silently — the
    /// file has no operator-secret content.
    ConfigFile,

    /// Hub/role configuration file (`hub.json` and role config) that
    /// references a vault keyfile path (e.g. `auth.keyfile` is set).
    /// Required: not world-writable.  Per HEP-CORE-0035 §4.6.2, a
    /// soft WARN is appended to `diagnostic` (without flipping `ok`)
    /// when `(st_mode & 0040) != 0` — group can see the path string.
    /// Callers parse the config first to choose between this role
    /// and the plain `ConfigFile` role.
    ConfigFileReferencingVault,

    /// Distributable public-key file (`hub.pubkey`, locally-cached
    /// role pubkeys).  Required at write: 0644.  Per HEP-CORE-0035
    /// §4.6.2, no startup mode-verification — pubkeys are
    /// intentionally distributable and may legitimately be group-
    /// or world-readable.  Listed here so the API is symmetric and
    /// so the write-time setter has a canonical mode.
    PublicKeyFile,
};

/// Outcome of `set_keyfile_mode`.  Discriminates three cases that
/// previously collapsed into a single `bool false`: the canonical
/// mode was applied successfully (`Applied`), the role has no
/// canonical mode and nothing was done (`NoCanonicalMode` — currently
/// the `ConfigFile*` roles, whose modes are operator-managed), and
/// the POSIX `chmod` syscall failed (`ChmodFailed` — usually because
/// the path does not exist or the current process lacks permission).
/// Callers should treat `NoCanonicalMode` as expected (skip), not as
/// an error.
enum class SetModeResult : int
{
    Applied,
    NoCanonicalMode,
    ChmodFailed,
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

/// Resolve a config-supplied `auth.keyfile` string to a filesystem
/// path, per the unified semantic in HEP-CORE-0033 §7.1 + HEP-CORE-
/// 0024 §3.4 (finalized 2026-05-31):
///
///   - Absolute input → returned as-is (no normalization).  JSON
///     values are read literally — `~` is NOT shell-expanded here.
///     Operators wanting the home directory must write the
///     fully-resolved `/home/<user>/...` form in their config.
///   - Relative input → joined with `base_dir` (hub_dir or role_dir
///     depending on caller).  No `..` normalization is performed;
///     the result may resolve outside `base_dir` if the operator
///     writes a `..`-bearing path.  The §4.6.2 ACL check applies to
///     whatever the resolved path is, so privilege escalation via
///     `..` still requires the attacker to also control file
///     ownership at the target.
///   - Empty input → empty result (defensive only — `auth.keyfile`
///     is REQUIRED non-empty per HEP-CORE-0024 §3.4 / HEP-CORE-0033
///     §7.1 and is rejected at parse_auth_config; this branch is
///     unreachable from normal callers but kept so the helper is
///     well-defined for any future caller that supplies untrusted
///     input).
///
/// Never throws.  Pure path arithmetic — no `stat()` call, no
/// existence check.  Callers verify existence separately (the
/// distinction between "operator configured a vault" and "vault
/// file is currently present at that path" is part of the §4.6.2
/// runtime contract — non-empty + file absent is a hard error,
/// not a silent fallback).
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::filesystem::path
resolve_keyfile_path(const std::string           &keyfile,
                     const std::filesystem::path &base_dir) noexcept;

/// Verify that `path` satisfies the ACL contract for `role`.
///
/// On POSIX: stats the file; compares the low 12 bits of `st_mode`
/// and `st_uid` to the role's contract.  Never throws; filesystem
/// errors (missing path, EACCES) land in `verdict.diagnostic` with
/// `ok = false`.
///
/// On Windows: returns `ok = true` with `diagnostic` describing the
/// platform skip (the diagnostic is non-empty even on success).
/// Callers logging diagnostics MUST consult `verdict.ok` first and
/// emit the message only on `ok = false`, OR they will produce a
/// noisy log line on every Windows verify.
///
/// Ownership semantics: the uid comparison uses `geteuid()` (the
/// effective uid).  Under a setuid binary this differs from the real
/// uid by design — `open` and `chmod` use the effective uid, so the
/// verify must match.
///
/// Known limitation (TOCTOU): the verdict is computed from a `stat()`
/// whose result the caller uses to decide whether to read the file
/// later.  A racing process could change the mode between
/// `verify_keyfile_acl` and the read.  Per HEP-CORE-0035 §4.6 the
/// file-mode floor is defence in depth on top of the vault's
/// encryption-at-rest layer (libsodium AEAD via `vault_crypto.cpp`);
/// the encryption is the primary protection and is not subject to
/// the TOCTOU window.
[[nodiscard]] PYLABHUB_UTILS_EXPORT AclVerdict
verify_keyfile_acl(const std::filesystem::path &path,
                   KeyFileRole                   role) noexcept;

/// Compare `observed_uid` to `expected_uid` for `path` under `role`
/// and return a verdict.  This is the ownership-check primitive that
/// `verify_keyfile_acl` calls internally with `geteuid()` for the
/// expected uid; exposed via `PYLABHUB_UTILS_TEST_EXPORT` (HEP-
/// CORE-0032 §3.2) so unit tests can exercise the uid-mismatch
/// branch with synthetic uids without needing `CAP_CHOWN` to chown
/// real files in CI.  In production builds the symbol is hidden;
/// in `BUILD_TESTS=ON` builds it expands to `PYLABHUB_UTILS_EXPORT`.
///
/// Returns `ok = true` with empty diagnostic when the uids match.
/// Returns `ok = false` with an operator-facing diagnostic (path,
/// observed uid, expected uid, ownership-check guidance) when they
/// differ.  The diagnostic uses the role's canonical noun (vault
/// file / vault directory / config file / public-key file) so the
/// message stays accurate for any caller-supplied role.
/// `observed_mode` / `required_mode` are 0 on both paths — this
/// helper does NOT check modes, only ownership.
[[nodiscard]] PYLABHUB_UTILS_TEST_EXPORT AclVerdict
verify_ownership(const std::filesystem::path &path,
                 KeyFileRole                   role,
                 uint32_t                      observed_uid,
                 uint32_t                      expected_uid) noexcept;

/// Set the mode of `path` to the canonical value for `role` per
/// HEP-CORE-0035 §4.6.1.
///
/// POSIX mapping:
///   - VaultFile                  → 0600  (Applied)
///   - VaultDir                   → 0700  (Applied)
///   - PublicKeyFile              → 0644  (Applied)
///   - ConfigFile                 → no canonical mode  (NoCanonicalMode)
///   - ConfigFileReferencingVault → no canonical mode  (NoCanonicalMode)
///
/// On Windows: no-op returning `Applied`.
///
/// Does not log; the caller decides how to surface.  Treat
/// `NoCanonicalMode` as expected, not as an error.
[[nodiscard]] PYLABHUB_UTILS_EXPORT SetModeResult
set_keyfile_mode(const std::filesystem::path &path,
                 KeyFileRole                   role) noexcept;

} // namespace pylabhub::utils::security
