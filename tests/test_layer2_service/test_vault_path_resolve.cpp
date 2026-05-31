/**
 * @file test_vault_path_resolve.cpp
 * @brief L2 unit tests for `pylabhub::utils::security::vault_path_resolve`.
 *
 * Subject: the canonical vault-location resolver (HEP-CORE-0024 §3.4.1 +
 * HEP-CORE-0033 §7.2) that maps `--vault-mode <option>` into the string
 * written to `auth.keyfile` during `--init`.  Pure pair of functions —
 * `parse_vault_mode()` (CLI string → enum + custom path) and
 * `resolve_vault_keyfile()` (enum + uid → final keyfile string) — with
 * a `vault_mode_usage_summary()` helper for `--help` text.
 *
 * Pattern 1 (plain `::testing::Test`) — no LifecycleGuard, no
 * subprocess workers; the resolver reads at most one env var
 * (`HOME` on POSIX, `LOCALAPPDATA` on Windows) and otherwise does
 * string arithmetic.
 *
 * Tests cover the five vault modes (User / System / Inline /
 * Ephemeral / Custom), the `$HOME` failure path, the absolute-path
 * detection rules, and the rejection cases parser-callers depend on.
 */
#include "utils/security/vault_path_resolve.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

using pylabhub::utils::security::parse_vault_mode;
using pylabhub::utils::security::resolve_vault_keyfile;
using pylabhub::utils::security::ParsedVaultMode;
using pylabhub::utils::security::VaultMode;
using pylabhub::utils::security::vault_mode_usage_summary;

namespace
{

// The resolver takes the FILENAME, not the UID — callers (role and
// hub init_directory) compose the filename per their convention
// (role: `<role_uid>.vault`; hub: fixed `hub.vault`).  Tests
// exercise both conventions below.
constexpr const char *kRoleFilename = "prod.lab.uid01234567.vault";
constexpr const char *kHubFilename  = "hub.vault";

/// Set + restore $HOME (POSIX) or %LOCALAPPDATA% (Windows) so tests
/// that assert the User-mode path are deterministic.  Each test owns
/// its own RAII guard — tests cannot leak env state to each other.
class EnvGuard
{
public:
    EnvGuard(const char *name, const std::string &value) : name_(name)
    {
        if (const char *prior = std::getenv(name); prior)
            saved_ = prior;
#ifdef _WIN32
        ::_putenv_s(name, value.c_str());
#else
        ::setenv(name, value.c_str(), 1);
#endif
    }
    ~EnvGuard()
    {
        if (saved_)
#ifdef _WIN32
            ::_putenv_s(name_, saved_->c_str());
        else
            ::_putenv_s(name_, "");
#else
            ::setenv(name_, saved_->c_str(), 1);
        else
            ::unsetenv(name_);
#endif
    }
    EnvGuard(const EnvGuard &)            = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;
private:
    const char                *name_;
    std::optional<std::string> saved_{};
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  parse_vault_mode
// ─────────────────────────────────────────────────────────────────────────

TEST(VaultPathParse, KnownMode_User)
{
    const auto r = parse_vault_mode("user");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::User);
    EXPECT_TRUE(r->custom_path.empty());
}

TEST(VaultPathParse, KnownMode_System)
{
    const auto r = parse_vault_mode("system");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::System);
    EXPECT_TRUE(r->custom_path.empty());
}

TEST(VaultPathParse, KnownMode_Inline)
{
    const auto r = parse_vault_mode("inline");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Inline);
    EXPECT_TRUE(r->custom_path.empty());
}

TEST(VaultPathParse, KnownMode_Ephemeral)
{
    const auto r = parse_vault_mode("ephemeral");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Ephemeral);
    EXPECT_TRUE(r->custom_path.empty());
}

TEST(VaultPathParse, ModeNames_AreCaseSensitive)
{
    // We deliberately require lowercase to match the published CLI
    // surface in HEP-CORE-0024 §3.4.1 + the binary's --help text.
    // Operators copying from docs get the exact form; typos surface
    // as "not recognized" rather than silently picking a mode.
    EXPECT_FALSE(parse_vault_mode("User").has_value());
    EXPECT_FALSE(parse_vault_mode("SYSTEM").has_value());
    EXPECT_FALSE(parse_vault_mode("Inline").has_value());
}

TEST(VaultPathParse, AbsolutePath_PosixRoot)
{
    const auto r = parse_vault_mode("/etc/secrets/vault.bin");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Custom);
    EXPECT_EQ(r->custom_path, "/etc/secrets/vault.bin");
}

TEST(VaultPathParse, AbsolutePath_WindowsDriveLetter)
{
    const auto r = parse_vault_mode("C:\\secrets\\vault.bin");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Custom);
    EXPECT_EQ(r->custom_path, "C:\\secrets\\vault.bin");
}

TEST(VaultPathParse, AbsolutePath_WindowsForwardSlashAfterDrive)
{
    const auto r = parse_vault_mode("D:/secrets/vault.bin");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Custom);
    EXPECT_EQ(r->custom_path, "D:/secrets/vault.bin");
}

TEST(VaultPathParse, AbsolutePath_UNC)
{
    const auto r = parse_vault_mode("\\\\share\\secrets\\vault.bin");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, VaultMode::Custom);
    EXPECT_EQ(r->custom_path, "\\\\share\\secrets\\vault.bin");
}

TEST(VaultPathParse, RelativePath_Rejected)
{
    // Relative paths reject — callers must opt explicitly into a
    // known mode or supply an absolute path.  Silently treating
    // "vault/foo.vault" as Custom would conflict with `inline` mode
    // semantics, where the relative path is supplied by the
    // resolver, not the operator.
    EXPECT_FALSE(parse_vault_mode("vault/foo.vault").has_value());
    EXPECT_FALSE(parse_vault_mode("./foo.vault").has_value());
    EXPECT_FALSE(parse_vault_mode("foo.vault").has_value());
}

TEST(VaultPathParse, EmptyString_Rejected)
{
    EXPECT_FALSE(parse_vault_mode("").has_value());
}

TEST(VaultPathParse, UnknownArbitraryString_Rejected)
{
    EXPECT_FALSE(parse_vault_mode("garbage").has_value());
    EXPECT_FALSE(parse_vault_mode("usre").has_value());        // typo
    EXPECT_FALSE(parse_vault_mode("system_v2").has_value());   // close-but-no
}

// ─────────────────────────────────────────────────────────────────────────
//  resolve_vault_keyfile
// ─────────────────────────────────────────────────────────────────────────

TEST(VaultPathResolve, User_RoleFilename_UsesEnvVar)
{
#ifdef _WIN32
    EnvGuard g("LOCALAPPDATA", "C:\\Users\\test");
#else
    EnvGuard g("HOME", "/home/test");
#endif
    const auto out = resolve_vault_keyfile({VaultMode::User, {}}, kRoleFilename);

#ifdef _WIN32
    EXPECT_EQ(out, "C:\\Users\\test\\pylabhub\\vault\\prod.lab.uid01234567.vault");
#else
    EXPECT_EQ(out, "/home/test/.pylabhub/vault/prod.lab.uid01234567.vault");
#endif
}

TEST(VaultPathResolve, User_HubFilename_UsesEnvVar)
{
    // Hub-side filename is the fixed `hub.vault` (HEP-CORE-0033 §6.5).
#ifdef _WIN32
    EnvGuard g("LOCALAPPDATA", "C:\\Users\\test");
#else
    EnvGuard g("HOME", "/home/test");
#endif
    const auto out = resolve_vault_keyfile({VaultMode::User, {}}, kHubFilename);

#ifdef _WIN32
    EXPECT_EQ(out, "C:\\Users\\test\\pylabhub\\vault\\hub.vault");
#else
    EXPECT_EQ(out, "/home/test/.pylabhub/vault/hub.vault");
#endif
}

TEST(VaultPathResolve, User_EnvVarUnset_Throws)
{
#ifdef _WIN32
    // Force-clear LOCALAPPDATA.
    ::_putenv_s("LOCALAPPDATA", "");
#else
    ::unsetenv("HOME");
#endif
    try {
        resolve_vault_keyfile({VaultMode::User, {}}, kRoleFilename);
        FAIL() << "expected runtime_error when User-mode env var is unset";
    } catch (const std::runtime_error &ex) {
        // Actionable message names the env var the operator must set
        // AND points at the --vault-mode <absolute-path> escape hatch.
        std::string msg = ex.what();
#ifdef _WIN32
        EXPECT_NE(msg.find("LOCALAPPDATA"), std::string::npos)
            << "missing env var name in: " << msg;
#else
        EXPECT_NE(msg.find("HOME"), std::string::npos)
            << "missing env var name in: " << msg;
#endif
        EXPECT_NE(msg.find("--vault-mode"), std::string::npos)
            << "missing escape hatch in: " << msg;
    }
}

TEST(VaultPathResolve, System_FixedPath)
{
    const auto out = resolve_vault_keyfile({VaultMode::System, {}}, kRoleFilename);
#ifdef _WIN32
    // Either %ProgramData% if set, or the C:\ProgramData fallback —
    // both end in pylabhub\vault\<filename>.
    EXPECT_NE(out.find("pylabhub\\vault\\prod.lab.uid01234567.vault"),
              std::string::npos)
        << "got: " << out;
#else
    EXPECT_EQ(out, "/etc/pylabhub/vault/prod.lab.uid01234567.vault");
#endif
}

TEST(VaultPathResolve, Inline_RelativePath)
{
    const auto role_out = resolve_vault_keyfile({VaultMode::Inline, {}}, kRoleFilename);
    const auto hub_out  = resolve_vault_keyfile({VaultMode::Inline, {}}, kHubFilename);
    // Forward-slash deliberately — the JSON config is portable across
    // platforms and the parser tolerates both.
    EXPECT_EQ(role_out, "vault/prod.lab.uid01234567.vault");
    EXPECT_EQ(hub_out,  "vault/hub.vault");
}

TEST(VaultPathResolve, Ephemeral_EmptyString)
{
    const auto out = resolve_vault_keyfile({VaultMode::Ephemeral, {}}, kRoleFilename);
    EXPECT_TRUE(out.empty())
        << "Ephemeral mode must produce empty string — the explicit "
           "no-vault opt-in per HEP-CORE-0024 §3.4.  Got: " << out;
}

TEST(VaultPathResolve, Custom_VerbatimUnchanged)
{
    const ParsedVaultMode m{VaultMode::Custom, "/opt/secrets/my.vault"};
    EXPECT_EQ(resolve_vault_keyfile(m, kRoleFilename), "/opt/secrets/my.vault");
}

TEST(VaultPathResolve, Custom_FilenameNotAppended)
{
    // Custom mode treats `custom_path` as the FULL vault file path —
    // resolver does NOT append the filename to it.  A regression that
    // re-appended would write `/opt/secrets/my.vault/<filename>`
    // which would never round-trip with operator expectations.
    const ParsedVaultMode m{VaultMode::Custom, "/opt/secrets/my.vault"};
    const auto out = resolve_vault_keyfile(m, kRoleFilename);
    EXPECT_EQ(out.find("prod.lab.uid"), std::string::npos)
        << "Custom-mode resolver must not append filename to the operator's "
           "verbatim path.  Got: " << out;
}

// ─────────────────────────────────────────────────────────────────────────
//  vault_mode_usage_summary
// ─────────────────────────────────────────────────────────────────────────

TEST(VaultPathUsage, SummaryListsAllModes)
{
    const auto s = vault_mode_usage_summary();
    EXPECT_NE(s.find("user"),      std::string::npos);
    EXPECT_NE(s.find("system"),    std::string::npos);
    EXPECT_NE(s.find("inline"),    std::string::npos);
    EXPECT_NE(s.find("ephemeral"), std::string::npos);
    EXPECT_NE(s.find("absolute"),  std::string::npos);
}

TEST(VaultPathUsage, SummaryCitesHEP)
{
    // The summary appears in --help; citing the HEP gives operators
    // who want context a direct doc pointer.
    const auto s = vault_mode_usage_summary();
    EXPECT_NE(s.find("HEP-CORE-0024"), std::string::npos)
        << "summary should cite HEP-CORE-0024 §3.4.1 — operators "
           "looking up the rationale need the section number.  Got: " << s;
}
