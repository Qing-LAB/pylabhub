/**
 * @file test_key_file_acl.cpp
 * @brief L2 unit tests for `pylabhub::utils::security::key_file_acl`.
 *
 * Subject: the shared file-ACL utility (HEP-CORE-0035 §4.6) that
 * enforces mode + ownership discipline on the vault file, vault
 * directory, config file, and public-key file before any secret is
 * read.  Pure function-level utility: stateless POSIX `stat` +
 * `chmod` + `geteuid` calls; no LOGGER_*, no lifecycle module, no
 * static state.
 *
 * Pattern 1 (plain `::testing::Test`) — no `LifecycleGuard`, no
 * `LogCaptureFixture`, no subprocess workers.  The verdict matrix
 * below is the single authoritative source for "what mode + owner
 * combinations the utility accepts and rejects."  Sibling vault /
 * directory tests delegate to this utility (1C refactor) rather than
 * duplicating inline `fs::permissions` checks.
 *
 * Diagnostic-message pinning: per
 * `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design — silent-
 * failure prevention", every Error verdict assertion pins:
 *   (a) `verdict.ok == false`,
 *   (b) `verdict.observed_mode` is the actual mode set,
 *   (c) `verdict.diagnostic` contains the path, the observed mode
 *       rendered as 0NNN, and (for mode violations) the exact
 *       `chmod NNN` command the operator must run.
 * This prevents a regression that emits the wrong error message
 * from passing an outcome-only check.
 *
 * Mutation sweep performed before commit (each mutation applied to
 * the production code, build + run, observed failures, then
 * reverted):
 *
 *   (a) Vault-file gate: `(observed & 0077) != 0` → `== 0`.
 *       Result: 6/7 VaultFile mode tests fail (the Ok-path AND
 *       all five Error-path cases).  Pins the gate's logical
 *       direction.
 *
 *   (b) Config-file gate: `(observed & 0002) != 0` →
 *       `(observed & 0020) != 0` (group-write instead of world-
 *       write).  Result: ConfigFile_0606_WorldWritable_Error
 *       fails (0606 has o-write but not g-write); 0666 still
 *       caught because it has BOTH bits.  Pins per-bit
 *       discrimination, not just "some write bit set."
 *
 *   (c) octal4 rendering: `"0%03o"` → `"%03o"` (drop the literal
 *       "0" prefix).  Result: VaultFile_0640/0644 Error tests fail
 *       because diagnostic now contains "640"/"644" not
 *       "0640"/"0644".  Pins operator-facing chmod-compatible
 *       rendering.
 *
 *   (d) Diagnostic content: drop the offending `<< path` clause
 *       from the vault-file world-accessible diagnostic emit.
 *       Result: `VaultFile_0640_GroupReadable_Error` fails because
 *       it pins `contains(v.diagnostic, p.string())`.  Coverage
 *       could be widened by adding path-substring assertions to
 *       every Error-path test; today the path-presence pin is
 *       sample-rate (present on `VaultFile_0640`, `ConfigFile_0666`,
 *       `ConfigFile_0606`, `ConfigFileReferencingVault_0644`,
 *       parent-dir warns, and ownership tests), which catches the
 *       documented mutation but not every theoretical path-drop
 *       edit.  Trade-off: assertion noise vs. mutation surface.
 *
 *   (e) Parent-dir warning: drop the parent-dir-stat block in
 *       `verify_vault_file` (per HEP-CORE-0035 §4.6.2 WARN
 *       requirement, H1 fix).  Result:
 *       VaultFile_ParentDir_0750_WarnsParentLeak and
 *       _0755_WarnsParentLeak fail because no "parent directory"
 *       substring is present in the diagnostic.
 *
 *   (f) Ownership helper: change `observed_uid == expected_uid`
 *       to `!=` in `verify_ownership`.  Result: every
 *       `VerifyOwnership_Matching*_Ok` test fails (ok flipped);
 *       every `*_Mismatched*_Error` test fails (ok flipped the
 *       other way).  Pins the uid comparison direction without
 *       needing CAP_CHOWN.  Function is exposed via
 *       `PYLABHUB_UTILS_TEST_EXPORT` so this mutation is catchable
 *       in test builds.
 *
 * NOTE on coverage gap: width-only mutations like "0%03o" →
 * "0%02o" are NOT detectable here because `%NNo` is *minimum*
 * width, not exact — and all modes in our matrix are >= 0100.  A
 * mode <0100 would discriminate; not added because it has no
 * production analog (vault file mode 044 has no real path).
 *
 * Wrong-owner (`st_uid != geteuid()`) cases are covered via the
 * `verify_ownership` primitive, which `verify_vault_file` and
 * `verify_vault_dir` call with `geteuid()` for the expected uid.
 * The primitive is exposed via `PYLABHUB_UTILS_TEST_EXPORT`
 * (HEP-CORE-0032 §3.2 — visible to tests when `BUILD_TESTS=ON`,
 * hidden in production builds), letting tests pass synthetic uid
 * pairs directly without needing `CAP_CHOWN` to chown real files in
 * CI.  The diagnostic emitted by `verify_ownership` is role-aware
 * (vault file / vault directory / config file / public-key file),
 * so the primitive can serve any future caller without the
 * misleading hardcoded "vault" wording.  Tests pin both
 * matching-uid and mismatched-uid branches with explicit role
 * coverage including a non-vault role (ConfigFile) to verify the
 * role-aware diagnostic.
 *
 * @see HEP-CORE-0035 §4.6 (the discipline this enforces)
 * @see HEP-CORE-0036 §3 I1 (the higher-level gate this protects)
 */

#include "utils/security/key_file_acl.hpp"
#include "plh_platform.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#if !defined(PYLABHUB_PLATFORM_WIN64)
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#include <thread>

namespace fs = std::filesystem;
using pylabhub::utils::security::AclVerdict;
using pylabhub::utils::security::KeyFileRole;
using pylabhub::utils::security::resolve_keyfile_path;
using pylabhub::utils::security::SetModeResult;
using pylabhub::utils::security::set_keyfile_mode;
using pylabhub::utils::security::verify_keyfile_acl;
using pylabhub::utils::security::verify_ownership;

#if defined(PYLABHUB_PLATFORM_WIN64)

// Windows: the utility's `#ifdef _WIN32` branch is a deliberate
// no-op-success per HEP-CORE-0035 §4.6 ("UNIX mode bits only").
// Vault contents remain encrypted-at-rest via libsodium regardless.
// A single skip-test reports the platform breadcrumb so CI shows
// the suite ran but didn't enforce.  A future Windows DACL
// enforcement path (see key_file_acl.cpp Windows branch FUTURE
// EXTENSION note) would add real cross-platform tests here.

TEST(KeyFileAclTest, Windows_PosixModeChecksSkipped)
{
    const AclVerdict v = verify_keyfile_acl(
        std::filesystem::temp_directory_path(), KeyFileRole::VaultDir);
    EXPECT_TRUE(v.ok)
        << "Windows: utility must return ok=true per documented "
           "platform skip.";
    EXPECT_NE(v.diagnostic.find("Windows"), std::string::npos)
        << "Windows: diagnostic must name the platform explicitly.";
    GTEST_SKIP() << "POSIX mode + ownership matrix is not applicable "
                    "on Windows; vault encryption-at-rest (libsodium) "
                    "remains in effect.  See HEP-CORE-0035 §4.6.";
}

#else  // POSIX

namespace
{

// Render a mode as the 4-character zero-padded octal form used in
// diagnostics (e.g. 0600).  Mirrors the production-code rendering;
// tests pin against the same string the operator sees.
std::string octal4(uint32_t mode)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0%03o", mode & 07777);
    return std::string(buf);
}

bool contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

} // namespace

class KeyFileAclTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Per-test unique subdir so parallel ctest runs don't collide.
        const auto now =
            std::chrono::steady_clock::now().time_since_epoch().count();
        tmp_dir_ = fs::temp_directory_path() /
                   ("plh_l2_kfacl_" +
                    std::to_string(::getpid()) + "_" +
                    std::to_string(now));
        fs::create_directories(tmp_dir_);
        ::chmod(tmp_dir_.c_str(), 0700);
    }

    void TearDown() override
    {
        std::error_code ec;
        // Restore writable bits on anything we set 0500/0400 etc., so
        // remove_all can traverse.
        for (const auto &entry : fs::recursive_directory_iterator(tmp_dir_, ec))
        {
            std::error_code chmod_ec;
            ::chmod(entry.path().c_str(), 0700);
            (void) chmod_ec;
        }
        fs::remove_all(tmp_dir_, ec);
    }

    fs::path tmpfile_with_mode(const char *name, uint32_t mode)
    {
        const auto p = tmp_dir_ / name;
        {
            std::ofstream out(p);
            out << "test\n";
        }
        ::chmod(p.c_str(), static_cast<mode_t>(mode));
        return p;
    }

    fs::path tmpdir_with_mode(const char *name, uint32_t mode)
    {
        const auto p = tmp_dir_ / name;
        fs::create_directories(p);
        ::chmod(p.c_str(), static_cast<mode_t>(mode));
        return p;
    }

    fs::path tmp_dir_;
};

// ─────────────────────────────────────────────────────────────────────────
//  resolve_keyfile_path — pure path arithmetic.
//  Per HEP-CORE-0033 §7.1 + HEP-CORE-0024 §3.4 unified semantic
//  (clarified 2026-05-30): empty → empty; absolute → as-is; relative
//  → joined with base_dir.  No `~` expansion, no `..` normalization,
//  no `stat()` call — existence is the caller's concern.
// ─────────────────────────────────────────────────────────────────────────

TEST(KeyFileAclResolveTest, Empty_ReturnsEmpty)
{
    const auto r = resolve_keyfile_path("", "/etc/pylabhub/hub");
    EXPECT_TRUE(r.empty())
        << "Empty input must return empty path so callers can use it "
           "as the ephemeral-CURVE sentinel.  Got: " << r;
}

TEST(KeyFileAclResolveTest, Empty_EmptyBase_ReturnsEmpty)
{
    const auto r = resolve_keyfile_path("", "");
    EXPECT_TRUE(r.empty());
}

TEST(KeyFileAclResolveTest, AbsolutePath_ReturnedUnchanged)
{
    const auto r = resolve_keyfile_path("/srv/secrets/hub.vault",
                                        "/etc/pylabhub/hub");
    EXPECT_EQ(r, fs::path("/srv/secrets/hub.vault"));
}

TEST(KeyFileAclResolveTest, AbsolutePath_BaseIgnored)
{
    // Pins that the base_dir parameter is NOT prepended when the
    // input is absolute (a regression here would silently rewrite
    // operator-configured absolute paths).
    const auto r = resolve_keyfile_path("/srv/secrets/hub.vault",
                                        "/completely/unrelated/base");
    EXPECT_EQ(r, fs::path("/srv/secrets/hub.vault"));
    EXPECT_TRUE(r.is_absolute());
}

TEST(KeyFileAclResolveTest, RelativePath_JoinedWithBase)
{
    const auto r = resolve_keyfile_path("vault/hub.vault",
                                        "/etc/pylabhub/hub");
    EXPECT_EQ(r, fs::path("/etc/pylabhub/hub/vault/hub.vault"));
}

TEST(KeyFileAclResolveTest, RelativePath_TrailingSlashOnBase)
{
    // std::filesystem::path / operator handles trailing slashes
    // transparently; pin that behavior so a hub_dir like
    // "/etc/pylabhub/hub/" (operator typo or convention) still
    // produces the right result.
    const auto r = resolve_keyfile_path("vault/hub.vault",
                                        "/etc/pylabhub/hub/");
    EXPECT_EQ(r, fs::path("/etc/pylabhub/hub/vault/hub.vault"));
}

TEST(KeyFileAclResolveTest, RelativePath_DotPrefix)
{
    // Some operators may write "./vault/hub.vault" — should resolve
    // identically to "vault/hub.vault" under the same base.
    const auto r = resolve_keyfile_path("./vault/hub.vault",
                                        "/etc/pylabhub/hub");
    EXPECT_EQ(r.lexically_normal(),
              fs::path("/etc/pylabhub/hub/vault/hub.vault"));
}

TEST(KeyFileAclResolveTest, RelativePath_DotDotTraversal_NotNormalized)
{
    // Pins that `..` is NOT normalized by the helper — the §4.6.2
    // ACL check runs on the resulting path, so privilege escalation
    // via `..` still requires owning the target.  But the helper
    // itself does not silently rewrite the operator's intent.
    const auto r = resolve_keyfile_path("../shared/hub.vault",
                                        "/etc/pylabhub/hub");
    // The result is the literal join; lexically_normal would collapse
    // /etc/pylabhub/hub/../shared/hub.vault → /etc/pylabhub/shared/hub.vault.
    // We do NOT normalize, so the literal path retains `..`.
    EXPECT_NE(r.string().find(".."), std::string::npos)
        << "helper must preserve `..` for the ACL check to see "
           "what the operator actually wrote: " << r;
}

TEST(KeyFileAclResolveTest, TildeNotExpanded)
{
    // L1 reviewer finding: JSON values are read literally; `~` is
    // not shell-expanded.  Pin that the helper treats `~/...` as a
    // relative path (since `~` is not a path-is_absolute starter
    // under POSIX std::filesystem::path::is_absolute).
    const auto r = resolve_keyfile_path("~/.pylabhub/vault/x.vault",
                                        "/etc/pylabhub/role");
    // Joined as relative → `/etc/pylabhub/role/~/.pylabhub/vault/x.vault`
    // (which the ACL check will then fail at `stat()`, producing an
    // operator-facing error — exactly the desired behavior).
    EXPECT_TRUE(r.string().find("~") != std::string::npos);
    EXPECT_TRUE(r.string().find("/etc/pylabhub/role") == 0)
        << "tilde-bearing relative paths are joined with base, NOT "
           "expanded to $HOME.  Got: " << r;
}

TEST(KeyFileAclResolveTest, ResultLength_DoesNotCorrupt)
{
    // Defensive sanity: a moderately long but legal input must round-trip
    // through path construction without truncation.
    std::string deep_path = "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z/vault.bin";
    const auto r = resolve_keyfile_path(deep_path, "/etc/p");
    EXPECT_TRUE(r.string().size() > deep_path.size());
    EXPECT_NE(r.string().find("vault.bin"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────
//  VaultFile — 0600 strict
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, VaultFile_0600_Ok)
{
    const auto p = tmpfile_with_mode("vault_0600", 0600);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_TRUE(v.ok) << "diagnostic: " << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0600u);
    EXPECT_EQ(v.required_mode, 0600u);
    EXPECT_EQ(v.role, KeyFileRole::VaultFile);
    EXPECT_EQ(v.path, p);
    EXPECT_TRUE(v.diagnostic.empty());
}

TEST_F(KeyFileAclTest, VaultFile_0640_GroupReadable_Error)
{
    const auto p = tmpfile_with_mode("vault_0640", 0640);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0640u);
    EXPECT_TRUE(contains(v.diagnostic, "vault file"))
        << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "group/world-accessible"))
        << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, octal4(0640)))
        << "diagnostic must show observed mode 0640: " << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod 0600"))
        << "diagnostic must give the chmod fix command: " << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string()))
        << "diagnostic must include offending path: " << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_0644_GroupAndWorldReadable_Error)
{
    const auto p = tmpfile_with_mode("vault_0644", 0644);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0644u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0644))) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod 0600")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_0660_GroupWritable_Error)
{
    const auto p = tmpfile_with_mode("vault_0660", 0660);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0660u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0660))) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_0606_WorldWritable_Error)
{
    const auto p = tmpfile_with_mode("vault_0606", 0606);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0606u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0606))) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_0666_FullyOpen_Error)
{
    const auto p = tmpfile_with_mode("vault_0666", 0666);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0666u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0666))) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_Missing_Error)
{
    const auto p = tmp_dir_ / "does_not_exist.vault";

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0u);
    EXPECT_TRUE(contains(v.diagnostic, "vault file")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "cannot be stat()'d")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  VaultDir — 0700 strict, must be a directory
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, VaultDir_0700_Ok)
{
    const auto p = tmpdir_with_mode("vault_0700", 0700);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0700u);
    EXPECT_EQ(v.required_mode, 0700u);
    EXPECT_TRUE(v.diagnostic.empty());
}

TEST_F(KeyFileAclTest, VaultDir_0750_GroupReadable_Error)
{
    const auto p = tmpdir_with_mode("vault_0750", 0750);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0750u);
    EXPECT_TRUE(contains(v.diagnostic, "vault directory")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, octal4(0750))) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod 0700")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultDir_0755_Error)
{
    const auto p = tmpdir_with_mode("vault_0755", 0755);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0755u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0755))) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod 0700")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultDir_0777_Error)
{
    const auto p = tmpdir_with_mode("vault_0777", 0777);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0777u);
    EXPECT_TRUE(contains(v.diagnostic, octal4(0777))) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultDir_PathIsAFile_Error)
{
    const auto p = tmpfile_with_mode("not_a_dir", 0700);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "not a directory")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultDir_Missing_Error)
{
    const auto p = tmp_dir_ / "does_not_exist_dir";

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "vault directory")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "cannot be stat()'d")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  ConfigFile (does NOT reference a vault path) — world-writable is
//  Error; group-readable is SILENT (no warn) per HEP-CORE-0035 §4.6.2.
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, ConfigFile_0600_Ok_NoWarn)
{
    const auto p = tmpfile_with_mode("config_0600", 0600);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0600u);
    EXPECT_TRUE(v.diagnostic.empty());
}

TEST_F(KeyFileAclTest, ConfigFile_0644_Ok_NoWarn)
{
    const auto p = tmpfile_with_mode("config_0644", 0644);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0644u);
    EXPECT_TRUE(v.diagnostic.empty())
        << "plain ConfigFile must NOT warn on group-readable; only "
           "ConfigFileReferencingVault warns.  Got: " << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFile_0640_Ok_NoWarn)
{
    const auto p = tmpfile_with_mode("config_0640", 0640);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(v.diagnostic.empty())
        << "plain ConfigFile must NOT warn on group-readable.  "
           "Got: " << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFile_0666_WorldWritable_Error)
{
    const auto p = tmpfile_with_mode("config_0666", 0666);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0666u);
    EXPECT_TRUE(contains(v.diagnostic, "world-writable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "config-injection")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod o-w")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFile_0606_WorldWritable_Error)
{
    const auto p = tmpfile_with_mode("config_0606", 0606);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0606u);
    EXPECT_TRUE(contains(v.diagnostic, "world-writable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod o-w")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFile_Missing_Error)
{
    const auto p = tmp_dir_ / "no_config.json";

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "config file")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "cannot be stat()'d")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  ConfigFileReferencingVault — same world-writable Error gate, plus
//  the group-readable WARN that HEP-CORE-0035 §4.6.2 qualifies with
//  "AND file references a keyfile path".  Diagnostic appended without
//  flipping ok.
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, ConfigFileReferencingVault_0600_Ok_NoWarn)
{
    const auto p = tmpfile_with_mode("config_rv_0600", 0600);

    const AclVerdict v = verify_keyfile_acl(
        p, KeyFileRole::ConfigFileReferencingVault);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(v.diagnostic.empty());
}

TEST_F(KeyFileAclTest, ConfigFileReferencingVault_0644_Ok_WarnsGroupReadable)
{
    const auto p = tmpfile_with_mode("config_rv_0644", 0644);

    const AclVerdict v = verify_keyfile_acl(
        p, KeyFileRole::ConfigFileReferencingVault);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "group-readable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod g-r")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFileReferencingVault_0640_Ok_WarnsGroupReadable)
{
    const auto p = tmpfile_with_mode("config_rv_0640", 0640);

    const AclVerdict v = verify_keyfile_acl(
        p, KeyFileRole::ConfigFileReferencingVault);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "group-readable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod g-r")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFileReferencingVault_0666_WorldWritable_Error)
{
    const auto p = tmpfile_with_mode("config_rv_0666", 0666);

    const AclVerdict v = verify_keyfile_acl(
        p, KeyFileRole::ConfigFileReferencingVault);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0666u);
    EXPECT_TRUE(contains(v.diagnostic, "world-writable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod o-w")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFileReferencingVault_Missing_Error)
{
    const auto p = tmp_dir_ / "no_config_rv.json";

    const AclVerdict v = verify_keyfile_acl(
        p, KeyFileRole::ConfigFileReferencingVault);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "config file")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "cannot be stat()'d")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  PublicKeyFile — no mode enforcement per §4.6.2; only existence
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, PublicKeyFile_0644_Ok)
{
    const auto p = tmpfile_with_mode("pub_0644", 0644);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::PublicKeyFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0644u);
    EXPECT_EQ(v.required_mode, 0644u);
}

TEST_F(KeyFileAclTest, PublicKeyFile_0600_StillOk_NoModeEnforcement)
{
    const auto p = tmpfile_with_mode("pub_0600", 0600);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::PublicKeyFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0600u);
}

TEST_F(KeyFileAclTest, PublicKeyFile_0666_StillOk_NoModeEnforcement)
{
    const auto p = tmpfile_with_mode("pub_0666", 0666);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::PublicKeyFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_EQ(v.observed_mode, 0666u);
}

TEST_F(KeyFileAclTest, PublicKeyFile_Missing_Error)
{
    const auto p = tmp_dir_ / "no_pubkey";

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::PublicKeyFile);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "public-key file")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "cannot be stat()'d")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  Parent-directory WARN (HEP-CORE-0035 §4.6.2): stat the vault file's
//  parent.  Group/world-accessible parent emits a soft warning
//  appended to `diagnostic`, NOT a flip of `ok`.
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, VaultFile_ParentDir_0700_NoWarn)
{
    const auto parent = tmpdir_with_mode("strict_parent", 0700);
    const auto file_p = parent / "vault_in_strict";
    {
        std::ofstream out(file_p);
        out << "x\n";
    }
    ::chmod(file_p.c_str(), 0600);

    const AclVerdict v = verify_keyfile_acl(file_p, KeyFileRole::VaultFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(v.diagnostic.empty())
        << "strict 0700 parent must produce empty diagnostic.  Got: "
        << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_ParentDir_0750_WarnsParentLeak)
{
    const auto parent = tmpdir_with_mode("group_parent", 0750);
    const auto file_p = parent / "vault_in_group_parent";
    {
        std::ofstream out(file_p);
        out << "x\n";
    }
    ::chmod(file_p.c_str(), 0600);

    const AclVerdict v = verify_keyfile_acl(file_p, KeyFileRole::VaultFile);

    EXPECT_TRUE(v.ok)
        << "parent-dir leak is a WARN (recoverable), not an ERROR.  "
           "ok must stay true.  Got diagnostic: " << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "parent directory"))
        << "diagnostic must name the parent dir explicitly: "
        << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "group/world-accessible"))
        << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "0750")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, parent.string()))
        << "diagnostic must include the parent path: " << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod 0700"))
        << "diagnostic must give the parent-dir fix command: "
        << v.diagnostic;
}

TEST_F(KeyFileAclTest, VaultFile_ParentDir_0755_WarnsParentLeak)
{
    const auto parent = tmpdir_with_mode("loose_parent", 0755);
    const auto file_p = parent / "vault_in_loose_parent";
    {
        std::ofstream out(file_p);
        out << "x\n";
    }
    ::chmod(file_p.c_str(), 0600);

    const AclVerdict v = verify_keyfile_acl(file_p, KeyFileRole::VaultFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "parent directory")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "0755")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  verify_ownership — the ownership-check primitive that
//  verify_vault_file / verify_vault_dir call internally with
//  geteuid().  Exposed via PYLABHUB_UTILS_TEST_EXPORT (HEP-CORE-0032
//  §3.2) so this test suite can pin the mismatch branch with
//  synthetic uids without needing CAP_CHOWN.  Diagnostic is role-
//  aware via the internal role-noun helper, so passing any
//  KeyFileRole produces a correct operator-facing message.
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, VerifyOwnership_MatchingUids_VaultFile_Ok)
{
    const auto p = tmpfile_with_mode("own_match_file", 0600);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::VaultFile, 1000u, 1000u);

    EXPECT_TRUE(v.ok);
    EXPECT_TRUE(v.diagnostic.empty()) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VerifyOwnership_MatchingUids_VaultDir_Ok)
{
    const auto p = tmpdir_with_mode("own_match_dir", 0700);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::VaultDir, 42u, 42u);

    EXPECT_TRUE(v.ok);
    EXPECT_TRUE(v.diagnostic.empty()) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VerifyOwnership_MismatchedUids_VaultFile_Error)
{
    const auto p = tmpfile_with_mode("own_mismatch_file", 0600);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::VaultFile,
                         /*observed=*/9999u, /*expected=*/1000u);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "vault file")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "owned by uid 9999")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "expected uid 1000")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "file ownership")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, p.string())) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VerifyOwnership_MismatchedUids_VaultDir_Error)
{
    const auto p = tmpdir_with_mode("own_mismatch_dir", 0700);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::VaultDir,
                         /*observed=*/9999u, /*expected=*/1000u);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "vault directory")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "owned by uid 9999")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "expected uid 1000")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "directory ownership")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, VerifyOwnership_MismatchedUids_ConfigFile_RoleAwareDiagnostic)
{
    // Confirms the role-aware diagnostic: passing ConfigFile must
    // emit "config file" not "vault file" — a sibling helper that
    // hardcoded "vault" would mislead the operator.
    const auto p = tmpfile_with_mode("own_mismatch_cfg", 0600);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::ConfigFile,
                         /*observed=*/9999u, /*expected=*/1000u);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "config file")) << v.diagnostic;
    EXPECT_FALSE(contains(v.diagnostic, "vault"))
        << "diagnostic must not say 'vault' when role is ConfigFile: "
        << v.diagnostic;
}

TEST_F(KeyFileAclTest, VerifyOwnership_RootVsUser_VaultFile_Error)
{
    // Common operator mistake: vault written as root before binary
    // dropped privileges to a service uid.  Pin that the diagnostic
    // surfaces both uids so the operator can `chown <expected> <path>`.
    const auto p = tmpfile_with_mode("own_root_user", 0600);

    const AclVerdict v =
        verify_ownership(p, KeyFileRole::VaultFile,
                         /*observed=*/0u, /*expected=*/1000u);

    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(contains(v.diagnostic, "owned by uid 0")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "expected uid 1000")) << v.diagnostic;
}

// ─────────────────────────────────────────────────────────────────────────
//  set_keyfile_mode — round-trip + tri-state return value
//  (HEP-CORE-0035 §4.6.1 + code-review H3).
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, SetMode_VaultFile_RoundTripFrom0666To0600)
{
    const auto p = tmpfile_with_mode("rt_vault", 0666);

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::VaultFile),
              SetModeResult::Applied);

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0600u);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);
    EXPECT_TRUE(v.ok) << v.diagnostic;
}

TEST_F(KeyFileAclTest, SetMode_VaultDir_RoundTripFrom0777To0700)
{
    const auto p = tmpdir_with_mode("rt_vault_dir", 0777);

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::VaultDir),
              SetModeResult::Applied);

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);
    EXPECT_TRUE(v.ok) << v.diagnostic;
}

TEST_F(KeyFileAclTest, SetMode_PublicKeyFile_RoundTripFrom0600To0644)
{
    const auto p = tmpfile_with_mode("rt_pub", 0600);

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::PublicKeyFile),
              SetModeResult::Applied);

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0644u);
}

TEST_F(KeyFileAclTest, SetMode_ConfigFile_NoCanonicalMode)
{
    // ConfigFile has no canonical mode (operator-managed).  The
    // tri-state return value distinguishes this from a genuine chmod
    // failure — callers must NOT treat NoCanonicalMode as an error.
    // The pre-set mode must NOT change.
    const auto p = tmpfile_with_mode("rt_config", 0644);

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::ConfigFile),
              SetModeResult::NoCanonicalMode);

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0644u)
        << "set_keyfile_mode(ConfigFile) must not chmod the file";
}

TEST_F(KeyFileAclTest, SetMode_ConfigFileReferencingVault_NoCanonicalMode)
{
    const auto p = tmpfile_with_mode("rt_config_rv", 0644);

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::ConfigFileReferencingVault),
              SetModeResult::NoCanonicalMode);
}

TEST_F(KeyFileAclTest, SetMode_MissingPath_ChmodFailed)
{
    const auto p = tmp_dir_ / "does_not_exist";

    EXPECT_EQ(set_keyfile_mode(p, KeyFileRole::VaultFile),
              SetModeResult::ChmodFailed);
}

#endif  // !PYLABHUB_PLATFORM_WIN64
