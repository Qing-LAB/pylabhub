/**
 * @file test_plh_hub_init.cpp
 * @brief plh_hub --init CLI tests.
 *
 * What --init does (HEP-CORE-0033 §7 + §15):
 *   - Creates the canonical hub directory layout
 *     (hub.json, logs/, run/, vault/, schemas/, script/python/).
 *   - Generates a hub uid via `uid::generate_hub_uid(name)`.
 *   - Writes a hub.json template with operator-overridable defaults.
 *   - Vault directory mode 0700 on POSIX.
 *
 * Tests pin: directory layout, hub.json shape, identity fields, and
 * --log-* override propagation.
 */

#include "plh_hub_fixture.h"

#include <chrono>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// ── Success paths ────────────────────────────────────────────────────────────

/// --init creates the canonical directory structure AND a non-empty
/// hub.json with the expected identity fields.  Pins both layout and
/// initial content so a regression that dropped the template generation
/// would be caught.
TEST_F(PlhHubCliTest, CreatesDirectoryStructure)
{
    const auto dir = tmp("init_layout");

    const auto t0 = std::chrono::steady_clock::now();
    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--name", "L4TestHub"});
    EXPECT_EQ(p.wait_for_exit(), 0)
        << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(10))
        << "--init took >10s — likely hung waiting for stdin "
           "(should be non-interactive when --name is provided)";

    // (a) Layout: required subdirs all present.
    EXPECT_TRUE(fs::exists(dir / "hub.json"))   << "hub.json missing";
    EXPECT_TRUE(fs::is_directory(dir / "logs"))   << "logs/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "run"))    << "run/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "vault"))  << "vault/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "schemas"))<< "schemas/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "script" / "python"))
        << "script/python/ missing";

    // (b) hub.json identity wiring — name we passed flows through, uid
    //     uses the hub. prefix per the naming grammar (HEP-0033 G2.2.0b).
    auto j = read_json(dir / "hub.json");
    ASSERT_FALSE(j.is_null()) << "hub.json failed to parse";
    ASSERT_TRUE(j.contains("hub")) << "hub.json missing 'hub' block";
    EXPECT_EQ(j["hub"].value("name", ""), "L4TestHub");
    const std::string uid = j["hub"].value("uid", "");
    EXPECT_FALSE(uid.empty()) << "hub.uid missing or empty";
    EXPECT_EQ(uid.substr(0, 4), std::string("hub."))
        << "hub.uid should start with 'hub.'; got: " << uid;
}

/// --log-maxsize / --log-backups overrides propagate into the generated
/// logging block.  Pins the CLI→config wiring for the two
/// init-only flags.
TEST_F(PlhHubCliTest, LogOverridesPropagateToConfig)
{
    const auto dir = tmp("init_log_overrides");

    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--name", "LogTestHub",
         "--log-maxsize", "42", "--log-backups", "7"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);

    auto j = read_json(dir / "hub.json");
    ASSERT_FALSE(j.is_null());
    ASSERT_TRUE(j.contains("logging"));
    EXPECT_EQ(j["logging"].value("max_size_mb", 0.0), 42.0);
    EXPECT_EQ(j["logging"].value("backups", 0),       7);
}

/// --log-backups -1 → "keep all" (LoggingConfig::kKeepAllBackups
/// sentinel; written to JSON as -1 per §6.4 schema).
TEST_F(PlhHubCliTest, LogBackupsMinusOneMeansKeepAll)
{
    const auto dir = tmp("init_keep_all");

    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--name", "KeepAllHub", "--log-backups", "-1"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    auto j = read_json(dir / "hub.json");
    EXPECT_EQ(j["logging"].value("backups", 0), -1);
}

/// --init refuses to overwrite an existing hub.json — protects against
/// accidental config wipes when re-running --init.
TEST_F(PlhHubCliTest, RefusesToOverwriteExistingConfig)
{
    const auto dir = tmp("init_no_clobber");
    write_file(dir / "hub.json", R"({"hub": {"name": "preserved"}})");

    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--name", "Wouldoverwrite"});
    const int rc = p.wait_for_exit();
    EXPECT_NE(rc, 0) << "expected non-zero exit when hub.json exists; "
                        "got 0.  stdout:\n" << p.get_stdout()
                     << "\nstderr:\n" << p.get_stderr();

    // Original content preserved (no clobber).
    auto j = read_json(dir / "hub.json");
    EXPECT_EQ(j["hub"].value("name", ""), "preserved");
}

} // namespace
