/**
 * @file test_plh_role_keygen.cpp
 * @brief plh_role --keygen CLI tests across all 3 roles.
 *
 * What --keygen does: generates a libsodium Ed25519 keypair, stores the
 * private half in the vault file specified by auth.keyfile (optionally
 * encrypted with PYLABHUB_ROLE_PASSWORD), and prints the public key on
 * stdout.  The role_uid is also printed for copy-into-hub-allowlist flows.
 *
 * L2 tests cover the vault-encryption semantics exhaustively.  These
 * L4 tests only pin the binary-level side effects: keyfile created,
 * pubkey printed, exit code correct.
 */

#include "plh_role_fixture.h"

#include <cstdlib> // setenv / _putenv_s

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

using namespace pylabhub::tests::plh_role_l4;
using pylabhub::tests::helper::ExpectNoVaultArtifactsUnder;
using pylabhub::tests::helper::ExpectVaultDirSecured;
using pylabhub::tests::helper::ExpectVaultFileSecured;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

struct RoleSpec
{
    std::string_view role;
};

// See PrintTo rationale in test_plh_role_init.cpp — keeps CTest test
// names as `Roles/<Case>/producer` instead of a raw 16-byte dump.
inline void PrintTo(const RoleSpec &s, std::ostream *os)
{
    *os << s.role;
}

class PlhRoleKeygenTest : public PlhRoleCliTest, public ::testing::WithParamInterface<RoleSpec>
{
  protected:
    void SetUp() override
    {
        PlhRoleCliTest::SetUp();
        // Provide vault password via env so the binary doesn't prompt.
#if defined(_WIN32) || defined(_WIN64)
        _putenv_s("PYLABHUB_ROLE_PASSWORD", "l4-test-password");
#else
        ::setenv("PYLABHUB_ROLE_PASSWORD", "l4-test-password", 1);
#endif
    }

    void TearDown() override
    {
#if defined(_WIN32) || defined(_WIN64)
        _putenv_s("PYLABHUB_ROLE_PASSWORD", "");
#else
        ::unsetenv("PYLABHUB_ROLE_PASSWORD");
#endif
        PlhRoleCliTest::TearDown();
    }
};

INSTANTIATE_TEST_SUITE_P(Roles, PlhRoleKeygenTest,
                         ::testing::Values(RoleSpec{"producer"}, RoleSpec{"consumer"},
                                           RoleSpec{"processor"}),
                         [](const auto &info) { return std::string(info.param.role); });

// ── Success paths ───────────────────────────────────────────────────────────

/// --keygen writes the vault file at the path specified in auth.keyfile
/// and prints the public key on stdout.  Pins the "on success, file
/// appears at the right path + with the right mode + pubkey emitted"
/// contract.  Exit code is necessary but not sufficient — the
/// post-keygen artifact verification catches regressions that the
/// binary "succeeds" but leaves the file unwritten, zero-sized, or
/// at wrong permissions.
TEST_P(PlhRoleKeygenTest, WritesVaultFile)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "test.vault";

    // Config with absolute vault path (keyfile created at that path).
    // auth lives INSIDE the role-tagged block — top-level "auth" is
    // rejected by the strict config key whitelist.
    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    WorkerProcess p(plh_role_binary(), "--role",
                    {std::string(s.role), "--config", cfg.string(), "--keygen"});
    // Keygen drives Argon2id (INTERACTIVE: ~100ms @ 64MiB locally).
    // PYLABHUB_TEST_CRYPTO_TIMEOUT_S is set by tests/test_framework/
    // CMakeLists.txt at configure time: 60s local, 120s under CI
    // (picks up memory pressure + scheduler jitter without hiding hangs).
    EXPECT_EQ(p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);

    // (a) Vault file at the exact configured path, mode 0600, non-zero.
    //     A regression that writes to the wrong path, leaves the file
    //     0-sized, or doesn't chmod is caught here.
    ExpectVaultFileSecured(vault_path);

    // (b) Vault parent dir MUST be 0700 (HEP-CORE-0035 §4.6.1).
    //     Enforced at write time by RoleVault::create calling
    //     set_keyfile_mode(parent, VaultDir).  A regression that
    //     drops the explicit chmod and falls back to umask-derived
    //     mode (typically 0755) is caught here.
    ExpectVaultDirSecured(vault_path.parent_path());

    // (c) Public key is printed as "public_key : <hex>" in stdout (see
    // plh_role_main.cpp: "role_uid" and "public_key" labels).  Pins
    // the operator-facing summary.
    EXPECT_NE(p.get_stdout().find("public_key"), std::string::npos)
        << "stdout missing 'public_key' label; got:\n"
        << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("role_uid"), std::string::npos)
        << "stdout missing 'role_uid' label; got:\n"
        << p.get_stdout();
}

// ── No-silent-overwrite contract (HEP-CORE-0024 §3.4) ───────────────────────

/// --keygen refuses to overwrite an existing role vault file.  Pins
/// that the existing CURVE keypair is not silently destroyed by a
/// re-run of --keygen.  Verifies content unchanged, not just rc!=0.
TEST_P(PlhRoleKeygenTest, KeygenRefusesToOverwriteExistingVault)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg_no_overwrite");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "test.vault";

    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    // Pre-create the vault parent dir + a sentinel file with known
    // content.  The binary's --keygen must NOT touch this file.
    fs::create_directories(vault_path.parent_path());
    const std::string sentinel = "PRE-EXISTING ROLE VAULT — must not be overwritten by --keygen.\n";
    {
        std::ofstream out(vault_path, std::ios::binary);
        out << sentinel;
    }
    ASSERT_EQ(fs::file_size(vault_path), sentinel.size());

    WorkerProcess p(plh_role_binary(), "--role",
                    {std::string(s.role), "--config", cfg.string(), "--keygen"});
    const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);

    // (a) Binary refused.
    EXPECT_NE(rc, 0) << "expected non-zero exit; stderr:\n" << p.get_stderr();

    // (b) Diagnostic identifies the violated rule + path + HEP cite.
    EXPECT_NE(p.get_stderr().find("already exists"), std::string::npos)
        << "stderr should say the vault already exists; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find(vault_path.string()), std::string::npos)
        << "stderr should name the offending path; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0024"), std::string::npos)
        << "stderr should cite HEP-CORE-0024; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("rm"), std::string::npos)
        << "stderr should tell operator how to remove the file; got:\n"
        << p.get_stderr();
    // Role-side specificity: the diagnostic explains that the OLD pubkey
    // is still pinned on the HUB ALLOWLIST, so silently re-keying would
    // strand this role.  Catches a regression that genericizes the
    // message and stops telling the operator why re-keygen is destructive
    // on the role side.
    EXPECT_NE(p.get_stderr().find("allowlist"), std::string::npos)
        << "role stderr should mention 'allowlist' (role-specific impact); "
           "got:\n"
        << p.get_stderr();

    // (c) THE LOAD-BEARING CHECK: file content IS UNCHANGED.
    std::string actual_content;
    {
        std::ifstream in(vault_path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        actual_content = ss.str();
    }
    EXPECT_EQ(actual_content, sentinel)
        << "no-overwrite contract VIOLATED — vault file content was "
           "modified by --keygen despite the refusal.";
    EXPECT_EQ(fs::file_size(vault_path), sentinel.size())
        << "no-overwrite contract VIOLATED — vault file size changed";
}

/// After the operator removes the existing vault file, --keygen
/// MUST succeed AND produce DIFFERENT key material than the previous
/// run.  Mutation-sweep of the refusal contract: the refusal must be
/// rooted in file existence, not in some sticky in-binary state; and
/// the rekey must derive a NEW CURVE keypair (deterministic
/// regeneration would silently re-pin the same allowlist entry).
TEST_P(PlhRoleKeygenTest, ReKeygenAfterRemovalSucceeds)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg_rekey");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "test.vault";

    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    auto run_keygen = [&]()
    {
        WorkerProcess p(plh_role_binary(), "--role",
                        {std::string(s.role), "--config", cfg.string(), "--keygen"});
        EXPECT_EQ(p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S), 0) << "stderr:\n"
                                                                      << p.get_stderr();
        expect_no_unexpected_errors(p);
    };
    auto read_vault = [&]()
    {
        std::ifstream in(vault_path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };

    // First keygen.
    run_keygen();
    ExpectVaultFileSecured(vault_path);
    const std::string first = read_vault();
    ASSERT_FALSE(first.empty());

    // Remove the vault file and re-keygen.
    std::error_code ec;
    fs::remove(vault_path, ec);
    ASSERT_FALSE(ec) << "rm vault failed: " << ec.message();
    ASSERT_FALSE(fs::exists(vault_path));

    run_keygen();
    ExpectVaultFileSecured(vault_path);
    const std::string second = read_vault();
    ASSERT_FALSE(second.empty());

    // The second run MUST produce different bytes — a fresh CURVE
    // keypair has a fresh secret + fresh public key.  Equal bytes
    // would indicate a deterministic regeneration regression.
    EXPECT_NE(first, second) << "re-keygen produced byte-identical vault — fresh CURVE "
                                "material was expected (first="
                             << first.size() << " bytes, second=" << second.size() << " bytes)";
}

// ── Error paths ─────────────────────────────────────────────────────────────

/// --keygen against a config with explicitly EMPTY auth.keyfile →
/// exits non-zero at config-parse (HEP-CORE-0024 §3.4: pylabhub is
/// a vault; empty keyfile is rejected).  Pins that the parse-time
/// guard fires BEFORE any --keygen filesystem work AND that no
/// half-written artifact survives.
TEST_P(PlhRoleKeygenTest, EmptyKeyfileFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg_empty");
    const auto cfg = dir / (std::string(s.role) + ".json");

    // Explicitly write empty keyfile.  write_minimal_config inserts a
    // placeholder path by default (HEP-CORE-0024 §3.4 — auth.keyfile
    // is required); the override below makes it empty so we can pin
    // the empty-string rejection contract.
    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = "";
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    WorkerProcess p(plh_role_binary(), "--role",
                    {std::string(s.role), "--config", cfg.string(), "--keygen"});
    EXPECT_NE(p.wait_for_exit(), 0) << "keygen with empty keyfile must fail; stderr:\n"
                                    << p.get_stderr();

    // (a) Diagnostic identifies the failing field AND the contract.
    EXPECT_NE(p.get_stderr().find("auth.keyfile"), std::string::npos)
        << "stderr should mention 'auth.keyfile'; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("non-empty"), std::string::npos)
        << "stderr should describe the empty-string violation; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0024"), std::string::npos)
        << "stderr should cite the source-of-truth HEP; got:\n"
        << p.get_stderr();

    // (b) No vault artifact was created — the parse-time guard fires
    //     before --keygen reaches the write path.
    ExpectNoVaultArtifactsUnder(dir);
}

/// --keygen against a config with NO auth block at all → same outcome
/// as empty keyfile, but via the missing-section parse path (C′-1).
/// Symmetric test to EmptyKeyfileFails — both paths feed the same
/// "binary rejects at config-load" contract, but exercise different
/// branches of parse_auth_config.
TEST_P(PlhRoleKeygenTest, MissingAuthBlockFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg_noauth");
    const auto cfg = dir / (std::string(s.role) + ".json");

    // Build minimal config, then erase the auth block via override
    // semantics (overrides merge-patch into the base; `null` deletes
    // a key per JSON Merge Patch RFC 7396).
    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"] = nullptr; // RFC 7396 delete
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    WorkerProcess p(plh_role_binary(), "--role",
                    {std::string(s.role), "--config", cfg.string(), "--keygen"});
    EXPECT_NE(p.wait_for_exit(), 0) << "keygen with missing auth block must fail; stderr:\n"
                                    << p.get_stderr();

    EXPECT_NE(p.get_stderr().find(std::string(s.role) + ".auth"), std::string::npos)
        << "stderr should mention '<role>.auth'; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0024"), std::string::npos)
        << "stderr should cite the source-of-truth HEP; got:\n"
        << p.get_stderr();

    ExpectNoVaultArtifactsUnder(dir);
}

// ── Runmode vault-presence contract (HEP-CORE-0024 §3.4) ───────────────────

/// Runmode startup requires the configured `auth.keyfile` to exist —
/// no silent fall-through to ephemeral CURVE.  Pins the load_keypair
/// failure path in plh_role_main.cpp: a non-empty keyfile that does
/// NOT exist on disk produces a "Vault unlock failed:" diagnostic
/// and a non-zero exit.  The HEP-CORE-0024 §3.4 "non-empty + file
/// absent" row is otherwise only exercised indirectly.
TEST_P(PlhRoleKeygenTest, RunmodeFailsWhenVaultMissing)
{
    const auto &s = GetParam();
    const auto dir = tmp("rm_no_vault");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "missing.vault";

    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    // Confirm we are testing the missing-file path, not a stale leftover.
    ASSERT_FALSE(fs::exists(vault_path));

    // Runmode invocation — no --keygen, no --validate.  The binary
    // reaches load_keypair, which throws because the vault file is
    // absent.  plh_role_main.cpp catches and prints "Vault unlock
    // failed:" before exiting 1.
    WorkerProcess p(plh_role_binary(), "--role", {std::string(s.role), "--config", cfg.string()});
    const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);
    EXPECT_NE(rc, 0) << "runmode with missing vault must fail; stderr:\n" << p.get_stderr();

    EXPECT_NE(p.get_stderr().find("Vault unlock failed"), std::string::npos)
        << "stderr should carry the load_keypair failure tag; got:\n"
        << p.get_stderr();

    // No vault material was created by the failure path.
    EXPECT_FALSE(fs::exists(vault_path)) << "runmode failure must not synthesize a vault file";
}

// ── Runtime ACL discipline (HEP-CORE-0035 §4.6.2) ───────────────────────────

#if !defined(_WIN32) && !defined(_WIN64)
/// Runmode startup verifies vault file mode == 0600 before reading
/// the secret.  If the file is world-readable (0644), the binary
/// must refuse with an OpenSSH-style actionable diagnostic naming
/// the path + required mode + chmod command.
TEST_P(PlhRoleKeygenTest, RunmodeFailsWhenVaultFileModeIsLoose)
{
    const auto &s = GetParam();
    const auto dir = tmp("rm_loose_mode");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "test.vault";

    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    // Create the vault (mode 0600 + parent 0700 by the new contract).
    {
        WorkerProcess kg(plh_role_binary(), "--role",
                         {std::string(s.role), "--config", cfg.string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S), 0)
            << "setup keygen failed: " << kg.get_stderr();
    }

    // Loosen mode to world-readable.  Runmode must now refuse.
    ::chmod(vault_path.c_str(), 0644);

    WorkerProcess p(plh_role_binary(), "--role", {std::string(s.role), "--config", cfg.string()});
    const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);
    EXPECT_NE(rc, 0) << "runmode against vault at mode 0644 must refuse; stderr:\n"
                     << p.get_stderr();

    EXPECT_NE(p.get_stderr().find("HEP-CORE-0035"), std::string::npos)
        << "stderr should cite HEP-CORE-0035 §4.6.2; got:\n"
        << p.get_stderr();
    // Pin "vault file" (NOT dir) + exact target mode 0600 so the
    // test catches a regression that swaps file/dir diagnostics
    // or emits the wrong target mode.
    EXPECT_NE(p.get_stderr().find("vault file"), std::string::npos)
        << "diagnostic should identify the FILE check fired; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("chmod 0600"), std::string::npos)
        << "stderr should tell operator to chmod 0600 (exact); got:\n"
        << p.get_stderr();
}

/// Symmetric: parent dir at 0755 (group/world readable) → runmode
/// refuses.  Pins HEP-CORE-0035 §4.6.2 directory enforcement.
TEST_P(PlhRoleKeygenTest, RunmodeFailsWhenVaultParentDirModeIsLoose)
{
    const auto &s = GetParam();
    const auto dir = tmp("rm_loose_dir");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto vault_path = dir / "vault" / "test.vault";

    nlohmann::json overrides;
    overrides[std::string(s.role)]["auth"]["keyfile"] = vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    {
        WorkerProcess kg(plh_role_binary(), "--role",
                         {std::string(s.role), "--config", cfg.string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S), 0)
            << "setup keygen failed: " << kg.get_stderr();
    }

    // Loosen parent dir mode to world-readable.  Runmode must refuse.
    ::chmod(vault_path.parent_path().c_str(), 0755);

    WorkerProcess p(plh_role_binary(), "--role", {std::string(s.role), "--config", cfg.string()});
    const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);
    EXPECT_NE(rc, 0) << "runmode against vault dir at mode 0755 must refuse; stderr:\n"
                     << p.get_stderr();

    EXPECT_NE(p.get_stderr().find("HEP-CORE-0035"), std::string::npos)
        << "stderr should cite HEP-CORE-0035 §4.6.2; got:\n"
        << p.get_stderr();
    // Pin "vault directory" (NOT file) + exact target mode 0700.
    EXPECT_NE(p.get_stderr().find("vault directory"), std::string::npos)
        << "diagnostic should identify the DIR check fired; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("chmod 0700"), std::string::npos)
        << "stderr should tell operator to chmod 0700 (exact); got:\n"
        << p.get_stderr();
}
#endif

/// --keygen without --role → fails with the dispatch-level "--role is
/// required" diagnostic from plh_role_main.cpp::register_and_lookup.
TEST_F(PlhRoleCliTest, KeygenWithoutRoleFails)
{
    const auto dir = tmp("kg_norole");
    const auto cfg = dir / "producer.json";
    write_minimal_config(cfg, "producer", dir);

    WorkerProcess p(plh_role_binary(), "--config", {cfg.string(), "--keygen"});
    EXPECT_NE(p.wait_for_exit(), 0);
    const std::string &err = p.get_stderr();
    EXPECT_NE(err.find("--role"), std::string::npos) << "stderr should mention '--role'; got:\n"
                                                     << err;
    EXPECT_NE(err.find("Available roles"), std::string::npos)
        << "stderr should list available roles; got:\n"
        << err;
}

} // namespace
