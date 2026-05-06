/**
 * @file test_plh_hub_keygen.cpp
 * @brief plh_hub --keygen CLI tests.
 *
 * What --keygen does (HEP-CORE-0033 §6.5):
 *   - Reads hub.json, requires `hub.auth.keyfile` to be set.
 *   - Prompts for a vault password (or reads PYLABHUB_HUB_PASSWORD).
 *   - Writes <vault_path> with an encrypted CURVE keypair.
 *   - Prints the public key + vault path on stdout.
 *
 * Tests pin: vault file creation, public-key emission on stdout, error
 * when keyfile is unset.
 */

#include "plh_hub_fixture.h"

#include <cstdlib>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// Helper — set / unset PYLABHUB_HUB_PASSWORD around a test scope so
// the binary doesn't block on the interactive password prompt.
struct ScopedHubPassword
{
    explicit ScopedHubPassword(const std::string &pw)
    {
        ::setenv("PYLABHUB_HUB_PASSWORD", pw.c_str(), /*overwrite=*/1);
    }
    ~ScopedHubPassword() { ::unsetenv("PYLABHUB_HUB_PASSWORD"); }
};

// ── Success paths ────────────────────────────────────────────────────────────

TEST_F(PlhHubCliTest, GeneratesVaultFileAndEmitsPubkey)
{
    const auto dir = tmp("keygen_ok");
    const auto cfg_path = dir / "hub.json";

    // Vault file path is fixed at `<hub_dir>/vault/hub.vault`
    // (HEP-CORE-0033 §7).  HubVault::{create,open} ignore the
    // `auth.keyfile` value at the moment — the field acts as a
    // non-empty/empty toggle for "vault auth on/off" and the path
    // is hard-coded.  Its documented value matches the actual
    // location.
    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.vault";
    write_minimal_config(cfg_path, dir, overrides);
    const fs::path vault_actual = dir / "vault" / "hub.vault";

    ScopedHubPassword pw("test-password");
    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--keygen"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);

    // (a) Vault file written.
    EXPECT_TRUE(fs::exists(vault_actual))
        << "hub.vault not created at " << vault_actual;
    EXPECT_GT(fs::file_size(vault_actual), 0u)
        << "vault file is empty";

    // (b) stdout contains hub_uid + public_key (the operator-facing
    //     summary the hub binary prints after a successful keygen).
    EXPECT_NE(p.get_stdout().find("hub_uid"), std::string::npos)
        << "stdout missing hub_uid:\n" << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("public_key"), std::string::npos)
        << "stdout missing public_key:\n" << p.get_stdout();
}

// ── Error paths ──────────────────────────────────────────────────────────────

/// --keygen with an unset auth.keyfile is a config error.  Pins the
/// "we don't pick a default vault path" contract.
TEST_F(PlhHubCliTest, FailsWhenKeyfileUnset)
{
    const auto dir = tmp("keygen_no_keyfile");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "";   // explicitly empty
    write_minimal_config(cfg_path, dir, overrides);

    ScopedHubPassword pw("test-password");
    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--keygen"});
    const int rc = p.wait_for_exit();
    EXPECT_NE(rc, 0) << "expected non-zero exit; stdout:\n"
                     << p.get_stdout() << "\nstderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("auth.keyfile"), std::string::npos)
        << "stderr should mention auth.keyfile; got:\n" << p.get_stderr();
}

} // namespace
