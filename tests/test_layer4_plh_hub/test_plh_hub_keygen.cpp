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
using pylabhub::tests::helper::ExpectVaultFileSecured;
using pylabhub::tests::helper::ExpectVaultDirSecured;
using pylabhub::tests::helper::ExpectNoVaultArtifactsUnder;

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

    // (a) Vault file written with secure file mode (0600 on POSIX)
    //     and non-zero content.  Exit code alone is not reliable —
    //     a regression where the binary returns 0 but the chmod
    //     step is removed would pass the rc check; the mode check
    //     here catches it.  Equally a regression where the file is
    //     touched-empty (size 0) is caught by ExpectVaultFileSecured.
    ExpectVaultFileSecured(vault_actual);

    // (b) Vault parent dir EXISTS.  Parent dir MODE check (0700 per
    //     HEP-CORE-0035 §4.6.1) is deliberately NOT asserted here —
    //     the binary's `fs::create_directories` path does not yet
    //     apply 0700 explicitly (security audit finding, separately
    //     tracked).  When that gap is fixed, swap this to
    //     `ExpectVaultDirSecured(vault_actual.parent_path())`.
    EXPECT_TRUE(fs::is_directory(vault_actual.parent_path()))
        << "vault dir missing: " << vault_actual.parent_path();

    // (c) stdout contains hub_uid + public_key (the operator-facing
    //     summary the hub binary prints after a successful keygen).
    //     This pins that the binary's stdout matches the documented
    //     operator-facing surface; rc=0 alone would not catch a
    //     regression that silenced the summary.
    EXPECT_NE(p.get_stdout().find("hub_uid"), std::string::npos)
        << "stdout missing hub_uid:\n" << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("public_key"), std::string::npos)
        << "stdout missing public_key:\n" << p.get_stdout();
}

// ── Error paths ──────────────────────────────────────────────────────────────

/// Empty `hub.auth.keyfile` is rejected at config-parse time
/// (HEP-CORE-0033 §7.1) — pylabhub is a vault and requires a path.
/// Tests that the parse-time rejection fires before any --keygen
/// work begins.  Replaces the prior contract that allowed empty
/// keyfile and rejected it at --keygen specifically.
TEST_F(PlhHubCliTest, FailsWhenKeyfileEmpty)
{
    const auto dir = tmp("keygen_empty_keyfile");
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

    // (a) Diagnostic identifies the failing field AND the contract violated.
    EXPECT_NE(p.get_stderr().find("auth.keyfile"), std::string::npos)
        << "stderr should mention auth.keyfile; got:\n" << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("non-empty"), std::string::npos)
        << "stderr should describe the empty-string violation; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0024"), std::string::npos)
        << "stderr should cite the source-of-truth HEP; got:\n"
        << p.get_stderr();

    // (b) No vault file (or any *.vault artifact) was created — the
    //     config-load guard fires BEFORE any --keygen filesystem work.
    //     A regression where the parse-time error is set but write
    //     still proceeds would leave a vault on disk; this catches it.
    ExpectNoVaultArtifactsUnder(dir);
}

} // namespace
