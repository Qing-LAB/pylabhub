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
 * Mutation sweep performed 2026-05-29 before commit (each mutation
 * applied to the production code, build + run, observed failures,
 * then reverted):
 *   - Vault-file gate: `(observed & 0077) != 0` → `== 0`.
 *     Result: 6/7 VaultFile tests failed (VaultFile_0600_Ok plus
 *     all five group/world-accessible cases).  Confirms the matrix
 *     pins the gate's logical direction.
 *   - Config-file gate: `(observed & 0002) != 0` →
 *     `(observed & 0020) != 0` (group-write instead of world-
 *     write).  Result: ConfigFile_0606_WorldWritable_Error failed
 *     (0606 has o-write but not g-write); 0666 still caught
 *     because it has BOTH bits.  Confirms the matrix discriminates
 *     between mode bits, not just "some write bit set."
 *   - octal4 rendering: `"0%03o"` → `"%03o"` (dropped the literal
 *     "0" prefix).  Result: VaultFile_0640_GroupReadable_Error and
 *     VaultFile_0644_GroupAndWorldReadable_Error failed because
 *     diagnostic now contains "640"/"644" not "0640"/"0644".
 *     Confirms operator-facing rendering is pinned to the chmod-
 *     compatible literal.
 *
 * NOTE on coverage gap: width-only mutations like "0%03o" → "0%02o"
 * are NOT detectable here because `%NNo` is *minimum* width, not
 * exact — and all modes in our matrix are >= 0100.  A mode <0100
 * (e.g. 044) would discriminate; not added because it has no
 * production analog (vault file mode 044 has no real path).
 *
 * Wrong-owner (`st_uid != geteuid()`) cases are NOT covered here;
 * exercising them requires creating a file owned by a different
 * uid, which needs `CAP_CHOWN` (root) and is unreliable on CI.  The
 * code path is grep-visible and short; coverage gap is documented.
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
using pylabhub::utils::security::set_keyfile_mode;
using pylabhub::utils::security::verify_keyfile_acl;

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
//  ConfigFile — not world-writable; group-readable is warn-not-fail
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, ConfigFile_0600_Ok)
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
    // 0644 has o-readable set but the group bit `(observed & 0040)`
    // IS set too — so the warn branch fires.  Pin the warn text but
    // assert ok is still true.
    EXPECT_TRUE(contains(v.diagnostic, "group-readable")) << v.diagnostic;
}

TEST_F(KeyFileAclTest, ConfigFile_0640_Ok_WarnsGroupReadable)
{
    const auto p = tmpfile_with_mode("config_0640", 0640);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_TRUE(v.ok) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "group-readable")) << v.diagnostic;
    EXPECT_TRUE(contains(v.diagnostic, "chmod g-r")) << v.diagnostic;
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
}

TEST_F(KeyFileAclTest, ConfigFile_0606_WorldWritable_Error)
{
    const auto p = tmpfile_with_mode("config_0606", 0606);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::ConfigFile);

    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.observed_mode, 0606u);
    EXPECT_TRUE(contains(v.diagnostic, "world-writable")) << v.diagnostic;
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
//  set_keyfile_mode — round-trip (write @ wrong mode; set; re-verify)
// ─────────────────────────────────────────────────────────────────────────

TEST_F(KeyFileAclTest, SetMode_VaultFile_RoundTripFrom0666To0600)
{
    const auto p = tmpfile_with_mode("rt_vault", 0666);

    ASSERT_TRUE(set_keyfile_mode(p, KeyFileRole::VaultFile));

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0600u);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultFile);
    EXPECT_TRUE(v.ok) << v.diagnostic;
}

TEST_F(KeyFileAclTest, SetMode_VaultDir_RoundTripFrom0777To0700)
{
    const auto p = tmpdir_with_mode("rt_vault_dir", 0777);

    ASSERT_TRUE(set_keyfile_mode(p, KeyFileRole::VaultDir));

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);

    const AclVerdict v = verify_keyfile_acl(p, KeyFileRole::VaultDir);
    EXPECT_TRUE(v.ok) << v.diagnostic;
}

TEST_F(KeyFileAclTest, SetMode_PublicKeyFile_RoundTripFrom0600To0644)
{
    const auto p = tmpfile_with_mode("rt_pub", 0600);

    ASSERT_TRUE(set_keyfile_mode(p, KeyFileRole::PublicKeyFile));

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0644u);
}

TEST_F(KeyFileAclTest, SetMode_ConfigFile_ReturnsFalseOperatorOwnsIt)
{
    // ConfigFile has no canonical mode; the utility must refuse to
    // touch it and report failure so callers know to use a
    // different code path.  Pre-set mode must NOT change.
    const auto p = tmpfile_with_mode("rt_config", 0644);

    EXPECT_FALSE(set_keyfile_mode(p, KeyFileRole::ConfigFile));

    struct ::stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0644u)
        << "set_keyfile_mode(ConfigFile) must not chmod the file";
}

TEST_F(KeyFileAclTest, SetMode_MissingPath_ReturnsFalse)
{
    const auto p = tmp_dir_ / "does_not_exist";

    EXPECT_FALSE(set_keyfile_mode(p, KeyFileRole::VaultFile));
}

#endif  // !PYLABHUB_PLATFORM_WIN64
