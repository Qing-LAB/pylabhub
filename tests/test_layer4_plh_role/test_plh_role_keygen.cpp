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

#include <cstdlib>   // setenv / _putenv_s

using namespace pylabhub::tests::plh_role_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

struct RoleSpec { std::string_view role; };

class PlhRoleKeygenTest : public PlhRoleCliTest,
                          public ::testing::WithParamInterface<RoleSpec>
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

INSTANTIATE_TEST_SUITE_P(
    Roles, PlhRoleKeygenTest,
    ::testing::Values(
        RoleSpec{"producer"}, RoleSpec{"consumer"}, RoleSpec{"processor"}),
    [](const auto &info) { return std::string(info.param.role); });

// ── Success paths ───────────────────────────────────────────────────────────

/// --keygen writes the vault file at the path specified in auth.keyfile
/// and prints the public key on stdout.  Pins the "on success, file
/// appears + pubkey emitted" contract.
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
    overrides[std::string(s.role)]["auth"]["keyfile"] =
        vault_path.generic_string();
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--keygen"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    EXPECT_TRUE(fs::exists(vault_path))
        << "vault file not created at " << vault_path;

    // Public key is printed as "public_key : <hex>" in stdout (see
    // plh_role_main.cpp: "role_uid" and "public_key" labels).
    EXPECT_NE(p.get_stdout().find("public_key"), std::string::npos)
        << "stdout missing 'public_key' label; got:\n" << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("role_uid"), std::string::npos)
        << "stdout missing 'role_uid' label; got:\n" << p.get_stdout();
}

// ── Error paths ─────────────────────────────────────────────────────────────

/// --keygen against a config WITHOUT auth.keyfile set → exit non-zero
/// with a diagnostic.  Pins that the "no keyfile configured" guard
/// fires before any crypto work.
TEST_P(PlhRoleKeygenTest, NoKeyfileConfiguredFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("kg_nokf");
    const auto cfg = dir / (std::string(s.role) + ".json");

    // Minimal config WITHOUT auth block → auth.keyfile is empty.
    write_minimal_config(cfg, std::string(s.role), dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--keygen"});
    EXPECT_NE(p.wait_for_exit(), 0) << "keygen without keyfile must fail";
    EXPECT_NE(p.get_stderr().find("keyfile"), std::string::npos)
        << "stderr should mention 'keyfile'; got:\n" << p.get_stderr();
}

/// --keygen without --role → fails with the dispatch-level "--role is
/// required" diagnostic from plh_role_main.cpp::register_and_lookup.
TEST_F(PlhRoleCliTest, KeygenWithoutRoleFails)
{
    const auto dir = tmp("kg_norole");
    const auto cfg = dir / "producer.json";
    write_minimal_config(cfg, "producer", dir);

    WorkerProcess p(plh_role_binary(), "--config",
        {cfg.string(), "--keygen"});
    EXPECT_NE(p.wait_for_exit(), 0);
    const std::string &err = p.get_stderr();
    EXPECT_NE(err.find("--role"), std::string::npos)
        << "stderr should mention '--role'; got:\n" << err;
    EXPECT_NE(err.find("Available roles"), std::string::npos)
        << "stderr should list available roles; got:\n" << err;
}

} // namespace
