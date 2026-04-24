/**
 * @file test_plh_role_init.cpp
 * @brief plh_role --init CLI tests across all 3 roles.
 *
 * What --init does: creates the canonical role directory layout
 * (<role>.json, script/, vault/, logs/, run/) with role-appropriate
 * defaults and overrides from --name / --log-maxsize / --log-backups.
 *
 * Parametrized over role ("producer", "consumer", "processor") so each
 * concern is exercised across all three without code duplication.
 */

#include "plh_role_fixture.h"

#include <algorithm>
#include <cctype>

using namespace pylabhub::tests::plh_role_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

struct RoleSpec
{
    std::string_view role;              // "producer" / "consumer" / "processor"
    std::string_view uid_prefix;        // "prod." / "cons." / "proc."
    std::string_view role_json_key;     // "producer" / "consumer" / "processor"
    std::string_view default_loop_timing; // producer=fixed_rate; cons/proc=max_rate
    bool             expects_target_period_ms;
};

// Parametrized fixture: each PlhRoleInitTest.<name> runs 3x, one per role.
class PlhRoleInitTest : public PlhRoleCliTest,
                        public ::testing::WithParamInterface<RoleSpec>
{
};

// Per-role init-template defaults.  Producer default is fixed_rate
// (the "heartbeat data source" shape); consumer/processor default to
// max_rate (they react to upstream data, no intrinsic cadence).
// target_period_ms is only emitted for fixed_rate templates.
INSTANTIATE_TEST_SUITE_P(
    Roles, PlhRoleInitTest,
    ::testing::Values(
        RoleSpec{"producer",  "prod.", "producer",
                 "fixed_rate", /*expects_target_period_ms=*/true},
        RoleSpec{"consumer",  "cons.", "consumer",
                 "max_rate",   /*expects_target_period_ms=*/false},
        RoleSpec{"processor", "proc.", "processor",
                 "max_rate",   /*expects_target_period_ms=*/false}),
    [](const auto &info) {
        return std::string(info.param.role);
    });

// ── Success paths ───────────────────────────────────────────────────────────

/// --init creates the canonical directory structure AND the script
/// files are non-empty (not just placeholder touches).  Pins both
/// layout and initial content so a regression that dropped the
/// callbacks.py generation would be caught.
TEST_P(PlhRoleInitTest, CreatesDirectoryStructure)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_layout");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(), "--name", "TestRole"});
    EXPECT_EQ(p.wait_for_exit(), 0)
        << "stderr:\n" << p.get_stderr();

    // (a) Config file exists.
    const fs::path cfg_path = dir / (std::string(s.role) + ".json");
    EXPECT_TRUE(fs::exists(cfg_path))
        << s.role << ".json missing";

    // (b) Script package — BOTH files (template uses __init__.py →
    // from .callbacks import ...).  A regression that skipped
    // callbacks.py would leave __init__.py referencing a missing
    // module; validate would fail.
    const fs::path init_py = dir / "script" / "python" / "__init__.py";
    const fs::path cb_py   = dir / "script" / "python" / "callbacks.py";
    EXPECT_TRUE(fs::exists(init_py))  << "__init__.py missing";
    EXPECT_TRUE(fs::exists(cb_py))    << "callbacks.py missing";
    EXPECT_GT(fs::file_size(init_py), 0u) << "__init__.py is empty";
    EXPECT_GT(fs::file_size(cb_py),   0u) << "callbacks.py is empty";

    // (c) Runtime directories exist and are directories (not files).
    EXPECT_TRUE(fs::is_directory(dir / "vault"))  << "vault/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "logs"))   << "logs/ missing";
    EXPECT_TRUE(fs::is_directory(dir / "run"))    << "run/ missing";

    // (d) Config parses as valid JSON.  A regression that wrote a
    // truncated/corrupt config would pass (a)+(c) trivially.
    const auto j = read_json(cfg_path);
    EXPECT_FALSE(j.is_null()) << "generated config is not valid JSON";
    EXPECT_TRUE(j.is_object())
        << "generated config must be a JSON object at top level";
}

/// --init generates a config whose identity fields follow the role-tagged
/// UID convention, carries the CLI-provided name, and has the expected
/// default values for required operational fields (loop_timing,
/// stop_on_script_error, target_period_ms, log_level, auth.keyfile).
/// Also pins that no OBSOLETE fields slip back in.
TEST_P(PlhRoleInitTest, DefaultValues)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_defaults");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(),
         "--name", "DefaultTest"});
    ASSERT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    const auto j = read_json(dir / (std::string(s.role) + ".json"));
    ASSERT_FALSE(j.is_null()) << "config parse failed";
    ASSERT_TRUE(j.contains(std::string(s.role_json_key)))
        << "config missing '" << s.role_json_key << "' identity block";
    const auto &id = j[std::string(s.role_json_key)];

    // (a) UID has role-tagged prefix.
    ASSERT_TRUE(id.contains("uid")) << "identity missing 'uid'";
    const std::string uid = id["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind(std::string(s.uid_prefix), 0), 0u)
        << "uid missing " << s.uid_prefix << " prefix: " << uid;

    // (b) Name round-trips from CLI --name.
    ASSERT_TRUE(id.contains("name")) << "identity missing 'name'";
    EXPECT_EQ(id["name"].get<std::string>(), "DefaultTest");

    // (c) log_level default is "info" (plh_role convention; see
    // README_Deployment.md "log levels" section).
    ASSERT_TRUE(id.contains("log_level")) << "identity missing 'log_level'";
    EXPECT_EQ(id["log_level"].get<std::string>(), "info");

    // (d) auth.keyfile default is "" — users opt in to vault via
    // --keygen.  Empty means no vault; validate and run mode both
    // skip the vault-unlock step.
    ASSERT_TRUE(id.contains("auth")) << "auth block missing from identity";
    ASSERT_TRUE(id["auth"].contains("keyfile")) << "auth missing 'keyfile'";
    EXPECT_EQ(id["auth"]["keyfile"].get<std::string>(), "")
        << "auth.keyfile default must be empty; user opts in via --keygen";

    // (e) Script defaults.
    ASSERT_TRUE(j.contains("script") && j["script"].is_object())
        << "config missing 'script' section";
    ASSERT_TRUE(j["script"].contains("path"));
    ASSERT_TRUE(j["script"].contains("type"));
    EXPECT_EQ(j["script"]["path"].get<std::string>(), ".");
    EXPECT_EQ(j["script"]["type"].get<std::string>(), "python");

    // (f) Loop timing default is role-dependent:
    //   producer          → "fixed_rate" (+ target_period_ms)
    //   consumer/processor → "max_rate"   (no target_period_ms)
    ASSERT_TRUE(j.contains("loop_timing")) << "config missing 'loop_timing'";
    EXPECT_EQ(j["loop_timing"].get<std::string>(),
              std::string(s.default_loop_timing))
        << "loop_timing default mismatch for " << s.role;
    EXPECT_EQ(j.contains("target_period_ms"),
              s.expects_target_period_ms)
        << "target_period_ms presence mismatch for " << s.role
        << " — expected "
        << (s.expects_target_period_ms ? "present" : "absent");

    // (g) stop_on_script_error defaults to false — user opts in
    // explicitly for strict error handling.
    ASSERT_TRUE(j.contains("stop_on_script_error"))
        << "config missing 'stop_on_script_error'";
    EXPECT_FALSE(j["stop_on_script_error"].get<bool>())
        << "stop_on_script_error must default to false";

    // (h) Obsolete field must NOT be present.  Catches silent
    // reintroduction of the pre-HEP-0011 field.
    EXPECT_FALSE(j.contains("interval_ms"))
        << "init template regressed — obsolete 'interval_ms' field present";
}

/// --init produces a config whose auth block is present with an EMPTY
/// keyfile — users opt in to vault via --keygen later.  Pins the
/// default-empty contract and the location of the auth block (under
/// the role identity block, NOT at top level).  A regression that
/// silently pre-populated keyfile OR moved the auth block would fail.
TEST_P(PlhRoleInitTest, DefaultAuthKeyfileEmpty)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_auth");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(), "--name", "AuthTest"});
    ASSERT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    const auto j = read_json(dir / (std::string(s.role) + ".json"));
    ASSERT_FALSE(j.is_null());

    // auth is INSIDE the identity block (RoleConfig::auth() reads from
    // there), not at top level.
    EXPECT_FALSE(j.contains("auth"))
        << "auth block must NOT be at top level — it lives under the "
        << s.role_json_key << " identity block";
    ASSERT_TRUE(j.contains(std::string(s.role_json_key)));
    ASSERT_TRUE(j[std::string(s.role_json_key)].contains("auth"))
        << "identity block missing auth";
    EXPECT_EQ(j[std::string(s.role_json_key)]["auth"]["keyfile"].get<std::string>(), "")
        << "default keyfile must be empty string (user opts in via --keygen)";
}

/// --init --log-maxsize N --log-backups M threads CLI overrides into
/// the generated config's logging section.  Pins the CLI-to-template
/// plumbing (HEP-0024 Phase 18).
///
/// Config shape (from actual init output):
///   "logging": { "max_size_mb": <double>, "backups": <int> }
/// max_size_mb is stored as a double (allows fractional MB in config).
TEST_P(PlhRoleInitTest, LogOverridesThreadThrough)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_logs");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(), "--name", "LogTest",
         "--log-maxsize", "25",
         "--log-backups", "7"});
    ASSERT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    const auto j = read_json(dir / (std::string(s.role) + ".json"));
    ASSERT_FALSE(j.is_null());
    ASSERT_TRUE(j.contains("logging"))
        << "config missing 'logging' section — override plumbing broken";

    // max_size_mb is stored as numeric (double); compare against 25.0.
    EXPECT_DOUBLE_EQ(j["logging"]["max_size_mb"].get<double>(), 25.0);
    // backups is an int (not "max_backup_files"; the key name matches
    // RotatingLogConfig's internal field).
    EXPECT_EQ(j["logging"]["backups"].get<int>(), 7);
}

/// --init with --log-backups -1 selects the kKeepAllBackups sentinel
/// (no deletion).  Pins that -1 survives as the "unlimited" marker;
/// other negative values are rejected at L2 parse time.
TEST_P(PlhRoleInitTest, LogBackupsUnlimitedSentinel)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_logs_unlim");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(), "--name", "LogTest",
         "--log-backups", "-1"});
    ASSERT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    const auto j = read_json(dir / (std::string(s.role) + ".json"));
    ASSERT_FALSE(j.is_null());
    EXPECT_EQ(j["logging"]["backups"].get<int>(), -1)
        << "kKeepAllBackups sentinel (-1) must round-trip as-is";
}

// ── Cross-mode consistency: init → validate ─────────────────────────────────

/// Round-trip: --init generates a config + script package, and --validate
/// on that SAME directory must accept it.  This is a PROPERTY of the
/// init-template design — if init's output doesn't validate, one of the
/// two modes has a bug.  Catches drift between the two modes' specs
/// that the individual DefaultValues/etc. assertions would not.
TEST_P(PlhRoleInitTest, InitOutputValidates)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_validate_rt");

    // Step 1: --init populates dir with config + script package.
    WorkerProcess init_p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string(), "--name", "RTTest"});
    ASSERT_EQ(init_p.wait_for_exit(), 0)
        << "init failed; stderr:\n" << init_p.get_stderr();

    // Step 2: --validate against the directory (positional form) —
    // exercises the run-directory-style entry path that production
    // deployments use.
    WorkerProcess val_p(plh_role_binary(), "--role",
        {std::string(s.role), dir.string(), "--validate"});
    ASSERT_EQ(val_p.wait_for_exit(), 0)
        << "validate of init-produced config failed; stderr:\n"
        << val_p.get_stderr();
    EXPECT_NE(val_p.get_stdout().find("Validation passed"), std::string::npos)
        << "validate should print 'Validation passed' on success; got stdout:\n"
        << val_p.get_stdout();
}

// ── Error paths ─────────────────────────────────────────────────────────────

/// --init without a name AND without a TTY cannot prompt for one —
/// binary must exit non-zero with a diagnostic that mentions "name",
/// NOT hang or segfault.  The stderr pin prevents "any error counts
/// as success" — the test catches only the specific missing-name path.
TEST_P(PlhRoleInitTest, NoNameNonInteractiveExitsWithError)
{
    const auto &s = GetParam();
    const auto dir = tmp("init_noname");

    // No --name flag; stdin is the test's pipe (non-interactive).
    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", std::string(s.role), dir.string()});
    const int rc = p.wait_for_exit(10);  // 10s cap — must not hang.
    ASSERT_NE(rc, 0)
        << "non-interactive --init without --name must fail, got rc=" << rc
        << "\nstderr:\n" << p.get_stderr();

    // Diagnostic must mention "name" so the user knows what to fix.
    // Case-insensitive search to tolerate "Name required" vs "name".
    const std::string &err = p.get_stderr();
    std::string err_lc = err;
    std::transform(err_lc.begin(), err_lc.end(), err_lc.begin(),
                    [](char c){ return std::tolower(c); });
    EXPECT_NE(err_lc.find("name"), std::string::npos)
        << "stderr should mention 'name' to help the user; got:\n" << err;
}

} // namespace
