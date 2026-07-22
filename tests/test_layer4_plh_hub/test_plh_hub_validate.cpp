/**
 * @file test_plh_hub_validate.cpp
 * @brief plh_hub --validate CLI tests.
 *
 * What --validate does (HEP-CORE-0033 §15):
 *   - Parses hub.json (`HubConfig::load_from_directory` /
 *     `HubConfig::load`).  Schema errors surface as non-zero exit
 *     with "Config error" in stderr.
 *   - Builds HubHost(cfg).  When `script.path` is non-empty, HubHost
 *     constructs HubScriptRunner; the engine itself is built inside
 *     the runner's worker thread via `scripting::create_engine`
 *     (HEP-CORE-0011 §"Engine Construction Lifecycle").
 *   - Runs `host.startup()` followed immediately by `host.shutdown()`.
 *     This exercises broker bind, admin bind, engine init +
 *     load_script (when scripts are enabled), and the orderly
 *     shutdown sequence.
 *   - Prints "Validation passed" on success.
 *
 * The startup→shutdown round-trip is the strongest validation we can
 * do without entering the run loop.  A misconfigured script or a
 * broker port collision would fail here, not at first-run time.
 */

#include "plh_hub_fixture.h"

#include <chrono>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// ── Success paths ────────────────────────────────────────────────────────────

TEST_F(PlhHubCliTest, MinimalConfigPasses)
{
    const auto dir = tmp("val_ok");
    const auto cfg_path = dir / "hub.json";

    write_minimal_script(dir);
    write_minimal_config(cfg_path, dir);

    // HEP-CORE-0035 §2 + HEP-CORE-0033 §6.5: --validate is a
    // clearance check on a provisioned hub (vault must exist).
    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    const auto t0 = std::chrono::steady_clock::now();
    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // Bound prevents a regression where validate accidentally enters
    // the run loop (would hang until SIGTERM).
    EXPECT_LT(elapsed, std::chrono::seconds(15))
        << "--validate took >15s — likely entered run loop";
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos)
        << "stdout did not contain 'Validation passed'; got:\n"
        << p.get_stdout();
}

/// --validate works with the directory positional form (matches the
/// `plh_hub <hub_dir>` run shape) in addition to `--config <file>`.
TEST_F(PlhHubCliTest, DirectoryFlavorPasses)
{
    const auto dir = tmp("val_dir");

    write_minimal_script(dir);
    write_minimal_config(dir / "hub.json", dir);

    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(dir / "hub.json");

    WorkerProcess p(plh_hub_binary(), dir.string(), {"--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos);
}

/// --validate works without a script (script.path empty) — the hub
/// can run script-disabled, and validate must accept that.
TEST_F(PlhHubCliTest, NoScriptPasses)
{
    const auto dir = tmp("val_noscript");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["script"]["path"] = ""; // explicitly disabled
    write_minimal_config(cfg_path, dir, overrides);

    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos);
}

// ── L6 wire — config-ACL advisory (HEP-CORE-0035 §4.6.1) ────────────────────

#if !defined(_WIN32) && !defined(_WIN64)
/// `HubConfig::load` runs `verify_keyfile_acl(<config_path>,
/// ConfigFileReferencingVault)` and emits a stderr WARN — gated on
/// `!v.diagnostic.empty()` per the AclVerdict contract — when the
/// config file's mode is group-readable.  Round-2 fix B5 changed
/// the gate from `!v.ok` to `!v.diagnostic.empty()` specifically to
/// surface this advisory case (v.ok=true, diagnostic populated).
/// This test PINS the wire — a regression that inverts the gate
/// back to `!v.ok` would silently drop the advisory and pass every
/// other test.
TEST_F(PlhHubCliTest, ConfigAclAdvisory_GroupReadable_EmitsWarn)
{
    const auto dir = tmp("val_acl_advisory");
    const auto cfg_path = dir / "hub.json";

    write_minimal_script(dir);
    write_minimal_config(cfg_path, dir);

    // Pre-keygen the vault with normal mode; the chmod below makes
    // hub.json group-readable AFTER keygen so --validate sees the
    // advisory at load time.
    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    // Group-readable mode (0640) — group can see the vault path that
    // hub.auth.keyfile embeds.  Non-fatal but worth flagging.
    ::chmod(cfg_path.c_str(), 0640);

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    // Non-fatal: validate must still exit 0 — the advisory is a WARN.
    EXPECT_EQ(p.wait_for_exit(), 0) << "validate must remain non-fatal on group-readable config; "
                                       "stderr:\n"
                                    << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("hub.json ACL advisory"), std::string::npos)
        << "stderr should carry the L6-wire advisory; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("group-readable"), std::string::npos)
        << "advisory should name the suspicious-mode reason; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0035"), std::string::npos)
        << "advisory should cite HEP-CORE-0035 §4.6.1; got:\n"
        << p.get_stderr();
}
#endif

// ── Security warning (HEP-CORE-0033 §7.1 + §7.2) ────────────────────────────

/// auth.keyfile resolves INSIDE hub_dir → hub binary emits the
/// "*** PYLABHUB SECURITY WARNING ***" stderr block.  The warning is
/// load-bearing under the script-write attack-surface model
/// (HEP-CORE-0033 §7.2) and must fire on every config load.  This
/// test verifies the emission contract — the binary identifies the
/// risky placement AND tells the operator what to fix.
TEST_F(PlhHubCliTest, WarnsWhenKeyfileInsideHubDir)
{
    const auto dir = tmp("val_warn_in_hub");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    // Inside hub_dir — keygen creates the vault inside hub_dir so the
    // §7.2 security warning fires on the subsequent --validate load.
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4test.uid00000001.vault";
    write_minimal_script(dir);
    write_minimal_config(cfg_path, dir, overrides);

    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    // (a) Warning block is present.
    const std::string &err = p.get_stderr();
    EXPECT_NE(err.find("PYLABHUB SECURITY WARNING"), std::string::npos)
        << "stderr missing the PYLABHUB SECURITY WARNING banner; got:\n"
        << err;
    // (b) Names the failing field with the exact path the operator wrote.
    EXPECT_NE(err.find("hub.auth.keyfile"), std::string::npos)
        << "warning should identify 'hub.auth.keyfile'; got:\n"
        << err;
    EXPECT_NE(err.find("vault/hub.l4test.uid00000001.vault"), std::string::npos)
        << "warning should quote the operator's keyfile value; got:\n"
        << err;
    // (c) Tells the operator what to do — recommends moving outside hub_dir.
    EXPECT_NE(err.find("RECOMMENDED"), std::string::npos)
        << "warning should include a RECOMMENDED action; got:\n"
        << err;
    EXPECT_NE(err.find("/etc/pylabhub/vault"), std::string::npos)
        << "warning should suggest /etc/pylabhub/vault for system-managed; got:\n"
        << err;
    EXPECT_NE(err.find(".pylabhub/vault"), std::string::npos)
        << "warning should suggest ~/.pylabhub/vault for single-user; got:\n"
        << err;
}

/// auth.keyfile resolves OUTSIDE hub_dir → NO security warning.
/// Symmetric negative-path check — the warning must NOT fire spuriously
/// for the recommended deployment pattern (vault outside hub_dir).
TEST_F(PlhHubCliTest, NoWarningWhenKeyfileOutsideHubDir)
{
    const auto dir = tmp("val_no_warn_out");
    const auto vault_outside = tmp("val_no_warn_vault_outside"); // sibling dir
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    // Absolute path OUTSIDE hub_dir.  Keygen creates the vault at
    // this absolute path; --validate unlocks it; no security
    // warning should fire because the placement is the recommended
    // outside-hub_dir form.
    overrides["hub"]["auth"]["keyfile"] =
        (vault_outside / "hub.l4test.uid00000001.vault").generic_string();
    write_minimal_script(dir);
    write_minimal_config(cfg_path, dir, overrides);

    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    const std::string &err = p.get_stderr();
    EXPECT_EQ(err.find("PYLABHUB SECURITY WARNING"), std::string::npos)
        << "stderr should NOT contain the security warning when "
           "keyfile is outside hub_dir; got:\n"
        << err;
}

// ── Error paths ──────────────────────────────────────────────────────────────

/// Malformed JSON → non-zero exit + "Config error" in stderr.  Pins
/// the "config parse failure surfaces as rc!=0 with a recognizable
/// diagnostic" contract — same shape plh_role uses.
TEST_F(PlhHubCliTest, MalformedJsonFails)
{
    const auto dir = tmp("val_badjson");
    const auto cfg_path = dir / "hub.json";

    write_file(cfg_path, R"({"hub": { broken json )");

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    const int rc = p.wait_for_exit();
    EXPECT_NE(rc, 0);
    EXPECT_NE(p.get_stderr().find("Config error"), std::string::npos)
        << "stderr should contain 'Config error'; got:\n"
        << p.get_stderr();
}

/// Missing config path → non-zero exit, recognizable diagnostic.
TEST_F(PlhHubCliTest, MissingConfigFails)
{
    const auto dir = tmp("val_missing");
    const auto cfg_path = dir / "does_not_exist.json";

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
}

/// Script path points at a non-existent module → engine.load_script
/// fails inside HubHost::startup; --validate surfaces it as non-zero
/// exit with "Validation failed" in stderr.
TEST_F(PlhHubCliTest, BrokenScriptFails)
{
    const auto dir = tmp("val_broken_script");
    const auto cfg_path = dir / "hub.json";

    // Write config pointing at a script dir, but DON'T create the
    // script file — load_script will fail.
    nlohmann::json overrides;
    overrides["script"]["type"] = "python";
    overrides["script"]["path"] = dir.generic_string();
    write_minimal_config(cfg_path, dir, overrides);

    // Pre-keygen the vault so --validate reaches the engine load
    // step.  Without this, validate fails earlier at vault unlock
    // ("vault not found") which is a different diagnostic than the
    // "Validation failed" path this test pins.
    ScopedHubPassword pw("test-password");
    keygen_minimal_hub(cfg_path);

    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("Validation failed"), std::string::npos)
        << "stderr should contain 'Validation failed'; got:\n"
        << p.get_stderr();
}

} // namespace
