/**
 * @file test_plh_hub_validate.cpp
 * @brief plh_hub --validate CLI tests.
 *
 * What --validate does (HEP-CORE-0033 §15):
 *   - Parses hub.json (`HubConfig::load_from_directory` /
 *     `HubConfig::load`).  Schema errors surface as non-zero exit
 *     with "Config error" in stderr.
 *   - Builds the engine + HubHost(cfg, engine).
 *   - Runs `host.startup()` followed immediately by `host.shutdown()`.
 *     This exercises broker bind, admin bind, engine init +
 *     load_script, and the orderly shutdown sequence.
 *   - Prints "Validation passed" on success.
 *
 * The startup→shutdown round-trip is the strongest validation we can
 * do without entering the run loop.  A misconfigured script or a
 * broker port collision would fail here, not at first-run time.
 */

#include "plh_hub_fixture.h"

#include <chrono>

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

    const auto t0 = std::chrono::steady_clock::now();
    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--validate"});
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
    overrides["script"]["path"] = "";   // explicitly disabled
    write_minimal_config(cfg_path, dir, overrides);

    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos);
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

    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--validate"});
    const int rc = p.wait_for_exit();
    EXPECT_NE(rc, 0);
    EXPECT_NE(p.get_stderr().find("Config error"), std::string::npos)
        << "stderr should contain 'Config error'; got:\n" << p.get_stderr();
}

/// Missing config path → non-zero exit, recognizable diagnostic.
TEST_F(PlhHubCliTest, MissingConfigFails)
{
    const auto dir = tmp("val_missing");
    const auto cfg_path = dir / "does_not_exist.json";

    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--validate"});
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

    WorkerProcess p(plh_hub_binary(), "--config",
        {cfg_path.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("Validation failed"), std::string::npos)
        << "stderr should contain 'Validation failed'; got:\n"
        << p.get_stderr();
}

} // namespace
