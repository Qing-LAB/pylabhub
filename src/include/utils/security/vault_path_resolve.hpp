#pragma once
/**
 * @file vault_path_resolve.hpp
 * @brief Compute the canonical vault file location for a deployment.
 *
 * Per HEP-CORE-0024 §3.4 + HEP-CORE-0033 §7.1 (clarified 2026-05-30),
 * the vault is recommended to live OUTSIDE the role / hub directory:
 * scripts running in the role process share the binary's euid and can
 * write any file the binary can write.  Encryption-at-rest protects
 * the vault's secret content from reads (script gets ciphertext) but
 * does NOT protect against malicious or accidental truncation /
 * replacement of the vault file.  Locating the vault outside the
 * role_dir reduces the script's surface for that attack class.
 *
 * This module is the single source of truth for the canonical vault
 * location across the codebase: `--init` template writers (role +
 * hub) call it to compute the string that goes into `auth.keyfile`;
 * tests reach for it through the same surface so they pin the actual
 * operator-facing default rather than a parallel test-only path.
 *
 * Five vault modes (HEP-CORE-0024 §3.4.1):
 *
 *   - `user`        — POSIX: `$HOME/.pylabhub/vault/<uid>.vault`
 *                   — Windows: `%LOCALAPPDATA%\pylabhub\vault\<uid>.vault`
 *                   Single-user desktop / laptop deployments.  Default
 *                   if the operator does not specify `--vault-mode`.
 *
 *   - `system`      — POSIX: `/etc/pylabhub/vault/<uid>.vault`
 *                   — Windows: `%ProgramData%\pylabhub\vault\<uid>.vault`
 *                   System-managed service deployments (sysv / systemd /
 *                   Windows service).  Requires write access at
 *                   keygen time (typically root / Administrator).
 *
 *   - `inline`      — `vault/<uid>.vault` (relative to role_dir)
 *                   Tarball-portable / self-contained workspace.
 *                   Operator explicitly accepts the script-write
 *                   attack-surface trade-off; pylabhub continues to
 *                   emit the "*** PYLABHUB SECURITY WARNING ***" at
 *                   startup as the load-bearing reminder.
 *
 *   - `ephemeral`   — `""` (empty string)
 *                   No on-disk vault; CURVE keypair generated in
 *                   memory each run.  Loopback / dev / CI only.
 *
 *   - `<absolute-path>` — operator-supplied absolute path used verbatim
 *                   (full vault file path, not just a directory).
 *                   Custom storage (encrypted volume mount, shared
 *                   NAS, secrets manager).
 *
 * @see HEP-CORE-0024 §3.4 (vault file convention) + §3.4.1 (placement)
 * @see HEP-CORE-0033 §7.1 (hub auth.keyfile) + §7.2 (placement)
 */
#include "pylabhub_utils_export.h"

#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::utils::security
{

/// Vault placement modes recognized by `--vault-mode`.
enum class VaultMode : int
{
    User,       ///< POSIX `$HOME/.pylabhub/...`; Windows `%LOCALAPPDATA%\pylabhub\...`.
    System,     ///< POSIX `/etc/pylabhub/...`; Windows `%ProgramData%\pylabhub\...`.
    Inline,     ///< Relative `vault/<uid>.vault` (lives inside role_dir).
    Ephemeral,  ///< Empty keyfile string; CURVE keys generated in memory.
    Custom,     ///< Operator-supplied absolute path (full vault file, not dir).
};

/// Parsed result of `--vault-mode <arg>`.  For `Custom`, `custom_path`
/// holds the absolute path the operator supplied.  For all other
/// modes `custom_path` is empty.
struct ParsedVaultMode
{
    VaultMode   mode{VaultMode::User};
    std::string custom_path{};
};

/// Parse a CLI `--vault-mode` argument.
///
/// Accepted shapes (case-sensitive):
///   - `"user"`     → {User, ""}
///   - `"system"`   → {System, ""}
///   - `"inline"`   → {Inline, ""}
///   - `"ephemeral"` → {Ephemeral, ""}
///   - any absolute path (leading `/` POSIX, drive-letter or `\\?\` /
///     `\\` UNC on Windows)
///                  → {Custom, <path>}
///
/// Returns `std::nullopt` on unrecognized input (neither a known mode
/// nor an absolute path).  Pure parser — no filesystem access; the
/// resolver below is what reads env vars and produces the path string.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<ParsedVaultMode>
parse_vault_mode(std::string_view arg) noexcept;

/// Resolve a parsed mode + vault filename into the string that goes
/// into the JSON `auth.keyfile` field.
///
/// Per-mode behavior:
///   - User      — reads `$HOME` (POSIX) / `%LOCALAPPDATA%` (Windows);
///                 throws `std::runtime_error` if the env var is unset.
///                 Result: absolute `<home>/.pylabhub/vault/<filename>`.
///   - System    — fixed absolute path
///                 (`/etc/pylabhub/vault/<filename>` POSIX;
///                 `%ProgramData%\pylabhub\vault\<filename>` Windows).
///   - Inline    — relative `vault/<filename>` (the role / hub config
///                 parser joins it with `<role_dir>` / `<hub_dir>` at
///                 load time via `resolve_keyfile_path`).
///   - Ephemeral — empty string (filename ignored).
///   - Custom    — `mode.custom_path` verbatim (filename ignored —
///                 the operator-supplied path is the full vault file
///                 path, not just a directory).
///
/// `filename` is the final filename the role / hub side wants for
/// the vault.  By convention:
///   - Hub:  `"hub.vault"` (fixed; HEP-CORE-0033 §6.5 — hub is
///           single-instance per `hub_dir`).
///   - Role: `<role_uid>.vault` (UID-based per HEP-CORE-0024 §3.4 —
///           supports a single shared vault directory holding
///           multiple roles' files).
///
/// The function does NOT validate the filename — callers have
/// already done so.
///
/// Cross-platform: on Windows the slash convention is the native
/// backslash; on POSIX it's the forward slash.  Operators can edit
/// the generated `auth.keyfile` later if they need to override.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
resolve_vault_keyfile(const ParsedVaultMode &mode,
                      std::string_view       filename);

/// One-line summary of the accepted `--vault-mode` values, suitable
/// for use in `--help` output.  Centralized here so role and hub
/// usage texts stay in sync.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
vault_mode_usage_summary();

} // namespace pylabhub::utils::security
