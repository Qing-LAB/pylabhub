/**
 * @file vault_path_resolve.cpp
 * @brief Implementation of canonical vault-location resolver.
 *
 * @see HEP-CORE-0024 §3.4 + §3.4.1
 * @see HEP-CORE-0033 §7.1 + §7.2
 */
#include "utils/security/vault_path_resolve.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace pylabhub::utils::security
{

namespace
{

/// POSIX `/`-rooted absolute path: starts with `/`.
/// Windows drive-letter:           starts with `<letter>:\` or `<letter>:/`.
/// Windows UNC:                    starts with `\\` or `//`.
/// We accept both POSIX-style and Windows-style absolutes regardless
/// of the host platform so cross-platform configs / tests work.
bool looks_absolute(std::string_view s) noexcept
{
    if (s.empty())
        return false;
    if (s.front() == '/' || s.front() == '\\')
        return true;  // POSIX root or UNC root
    if (s.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(s.front())) &&
        s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
        return true;  // Windows drive-letter
    return false;
}

#ifdef _WIN32
constexpr char        kSep            = '\\';
constexpr const char *kUserEnv        = "LOCALAPPDATA";
constexpr const char *kAppSubdir      = "pylabhub\\vault";
constexpr const char *kSystemEnv      = "ProgramData";
constexpr const char *kSystemFallback = "C:\\ProgramData\\pylabhub\\vault";
#else
constexpr char        kSep            = '/';
constexpr const char *kUserEnv        = "HOME";
constexpr const char *kAppSubdir      = ".pylabhub/vault";
constexpr const char *kSystemEnv      = nullptr;  // /etc is a fixed path
constexpr const char *kSystemFallback = "/etc/pylabhub/vault";
#endif

/// Read an env var that must be present; throw with an actionable
/// message if it is not.  Used for `$HOME` / `%LOCALAPPDATA%` in
/// `User` mode.
std::string require_env(const char *name)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        throw std::runtime_error(
            std::string("--vault-mode user requires the '") + name +
            "' environment variable to be set.  Set it, or specify "
            "an explicit `--vault-mode <absolute-path>` instead.");
    return v;
}

std::string join_path(std::string lhs, std::string_view rhs)
{
    if (lhs.empty())
        return std::string(rhs);
    if (lhs.back() != '/' && lhs.back() != '\\')
        lhs.push_back(kSep);
    lhs.append(rhs);
    return lhs;
}

} // namespace

// ── parse_vault_mode ────────────────────────────────────────────────────────

std::optional<ParsedVaultMode>
parse_vault_mode(std::string_view arg) noexcept
{
    if (arg == "user")
        return ParsedVaultMode{VaultMode::User, {}};
    if (arg == "system")
        return ParsedVaultMode{VaultMode::System, {}};
    if (arg == "inline")
        return ParsedVaultMode{VaultMode::Inline, {}};
    if (arg == "ephemeral")
        return ParsedVaultMode{VaultMode::Ephemeral, {}};
    if (looks_absolute(arg))
        return ParsedVaultMode{VaultMode::Custom, std::string(arg)};
    return std::nullopt;
}

// ── resolve_vault_keyfile ───────────────────────────────────────────────────

std::string resolve_vault_keyfile(const ParsedVaultMode &mode,
                                   std::string_view       filename)
{
    const std::string fname(filename);

    switch (mode.mode)
    {
    case VaultMode::User: {
        std::string root = require_env(kUserEnv);
        std::string dir  = join_path(std::move(root), kAppSubdir);
        return join_path(std::move(dir), fname);
    }
    case VaultMode::System: {
        std::string dir;
#ifdef _WIN32
        // `%ProgramData%` is typically `C:\ProgramData`; fall back to
        // the canonical literal if the env var is missing.
        if (const char *v = std::getenv(kSystemEnv); v && *v)
            dir = join_path(std::string(v), "pylabhub\\vault");
        else
            dir = kSystemFallback;
#else
        dir = kSystemFallback;
#endif
        return join_path(std::move(dir), fname);
    }
    case VaultMode::Inline:
        // Relative path; the role/hub config parser joins it with
        // role_dir / hub_dir at load time (see resolve_keyfile_path
        // in key_file_acl.hpp).  Use forward-slash because the JSON
        // config is portable across platforms and the parser
        // tolerates both.
        return std::string("vault/") + fname;
    case VaultMode::Ephemeral:
        return {};
    case VaultMode::Custom:
        return mode.custom_path;
    }
    // Unreachable; switch is exhaustive over the enum.
    return {};
}

// ── vault_mode_usage_summary ────────────────────────────────────────────────

std::string vault_mode_usage_summary()
{
    return
        "user|system|inline|ephemeral|<absolute-path> "
        "(default: user; see HEP-CORE-0024 §3.4.1)";
}

} // namespace pylabhub::utils::security
