/**
 * @file test_plh_role_validate.cpp
 * @brief plh_role --validate CLI tests across all 3 roles.
 *
 * What --validate does: parses config, loads script, registers slot
 * types, calls build_api.  Does NOT open the control-plane (no broker
 * connection).  Does NOT invoke on_init or enter the data loop.  Exits
 * 0 with "Validation passed" on success, non-zero on any failure.
 *
 * See producer_role_host.cpp:150 and similar for the explicit
 * `is_validate_only()` short-circuits that keep this path hub-free.
 */

#include "plh_role_fixture.h"

using namespace pylabhub::tests::plh_role_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

struct RoleSpec { std::string_view role; };

// See PrintTo rationale in test_plh_role_init.cpp — keeps CTest test
// names as `Roles/<Case>/producer` instead of a raw 16-byte dump.
inline void PrintTo(const RoleSpec &s, std::ostream *os)
{
    *os << s.role;
}

class PlhRoleValidateTest : public PlhRoleCliTest,
                            public ::testing::WithParamInterface<RoleSpec>
{
};

INSTANTIATE_TEST_SUITE_P(
    Roles, PlhRoleValidateTest,
    ::testing::Values(
        RoleSpec{"producer"}, RoleSpec{"consumer"}, RoleSpec{"processor"}),
    [](const auto &info) { return std::string(info.param.role); });

// ── Success paths ───────────────────────────────────────────────────────────

/// Minimal valid config + trivial script → exit 0 + "Validation passed"
/// on stdout.  This is the shape every other validate test diverges from.
TEST_P(PlhRoleValidateTest, MinimalConfigPasses)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_ok");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    write_minimal_script(script_dir);
    write_minimal_config(cfg, std::string(s.role), dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos)
        << "stdout did not contain 'Validation passed'; got:\n"
        << p.get_stdout();
}

/// --validate works against a role directory (the <dir> positional form)
/// in addition to the --config <path> form.  Pins the directory-flavor
/// alternative entry point.
TEST_P(PlhRoleValidateTest, DirectoryFlavorPasses)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_dir");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    write_minimal_script(script_dir);
    write_minimal_config(cfg, std::string(s.role), dir);

    // Positional <dir> instead of --config <file>.
    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), dir.string(), "--validate"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);
    EXPECT_NE(p.get_stdout().find("Validation passed"), std::string::npos);
}

// ── Error paths ─────────────────────────────────────────────────────────────

/// Malformed JSON → non-zero exit + "Config error" in stderr.  Pins the
/// "config parse failure surfaces as rc!=0 with a recognizable
/// diagnostic" contract.
TEST_P(PlhRoleValidateTest, MalformedJsonFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_badjson");
    const auto cfg = dir / (std::string(s.role) + ".json");

    write_file(cfg, R"({"producer": { broken json )");

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0) << "malformed JSON must fail";
    EXPECT_NE(p.get_stderr().find("Config error"), std::string::npos)
        << "stderr missing 'Config error' diagnostic; got:\n" << p.get_stderr();
}

/// Config missing the role-specific identity block → non-zero exit.
/// Pins that required-field validation runs before the engine step.
TEST_P(PlhRoleValidateTest, MissingIdentityBlockFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_noident");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    write_minimal_script(script_dir);
    // Write a config WITHOUT the top-level "producer"/"consumer"/"processor"
    // identity block — still valid JSON but semantically wrong.
    write_file(cfg, R"({"script": {"type": "python", "path": "."}})");

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0) << "config without identity block must fail";
}

/// Config pointing at a script directory that doesn't exist → non-zero
/// exit.  Pins that the script load step fails loudly instead of
/// silently continuing with no script.
TEST_P(PlhRoleValidateTest, MissingScriptDirFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_noscript");
    const auto cfg = dir / (std::string(s.role) + ".json");

    // Config points at a non-existent script path.
    write_minimal_config(cfg, std::string(s.role),
                          dir / "nonexistent_script_dir");

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0) << "missing script dir must fail";
    // Either "Script load failed" or the engine startup exception message
    // — both are reasonable; keep pin loose to "Script" so reworded
    // messages still land.
    EXPECT_TRUE(
        p.get_stderr().find("Script")  != std::string::npos ||
        p.get_stderr().find("script")  != std::string::npos ||
        p.get_stderr().find("Startup") != std::string::npos)
        << "stderr did not mention script / startup failure; got:\n"
        << p.get_stderr();
}

/// Config with a script that has a syntax error → non-zero exit.
/// Pins Python syntax errors surfacing up through validate.
TEST_P(PlhRoleValidateTest, ScriptSyntaxErrorFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_syntax");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    // Garbage Python — unclosed def, clearly a SyntaxError.
    write_file(script_dir / "__init__.py", "def on_produce(\n");
    write_minimal_config(cfg, std::string(s.role), dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0) << "script syntax error must fail";
}

/// Config with a script that's MISSING the required callback for this
/// role (e.g., producer config but script has no on_produce) → non-zero
/// exit.  Pins the required-callback check.
TEST_P(PlhRoleValidateTest, MissingRequiredCallbackFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_nocb");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    // Script defines only OTHER callbacks, not the one this role needs.
    // Each role's required callback is unique per role:
    //   producer → on_produce, consumer → on_consume, processor → on_process.
    // So we write a script with a callback that doesn't match.
    std::string unrelated_cb_script;
    if (s.role == "producer")
        unrelated_cb_script = "def on_consume(rx, msgs, api):\n    return True\n";
    else
        unrelated_cb_script = "def on_produce(tx, msgs, api):\n    return False\n";
    write_file(script_dir / "__init__.py", unrelated_cb_script);
    write_minimal_config(cfg, std::string(s.role), dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0)
        << s.role << " config with missing required callback must fail";
}

/// --validate without --role fails — plh_role must know which role to
/// dispatch to before it can validate.
TEST_F(PlhRoleCliTest, ValidateWithoutRoleFails)
{
    const auto dir = tmp("val_norole");
    const auto cfg = dir / "producer.json";
    const auto script_dir = dir / "script" / "python";
    write_minimal_script(script_dir);
    write_minimal_config(cfg, "producer", dir);

    WorkerProcess p(plh_role_binary(), "--config",
        {cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("--role"), std::string::npos)
        << "stderr should mention --role; got:\n" << p.get_stderr();
}

/// --validate without either --config <path> or <dir> positional → error.
/// Pins the parse_role_args post-loop guard:
///   "Error: specify a role directory, --init, or --config <path>".
TEST_F(PlhRoleCliTest, ValidateWithoutConfigOrDirFails)
{
    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("specify a role directory"),
              std::string::npos)
        << "stderr should prompt for input location; got:\n"
        << p.get_stderr();
}

/// Config with an unknown TOP-LEVEL key → validate rejects with
/// "unknown config key".  Pins the strict key-whitelist contract from
/// RoleConfig parsing (introduced with the HEP-0024 config refactor).
///
/// This is the precise failure our test helper's original
/// `{"auth": {"keyfile": "…"}}` override hit — pinning it here prevents
/// that class of bug from reoccurring silently.
TEST_P(PlhRoleValidateTest, UnknownTopLevelKeyFails)
{
    const auto &s = GetParam();
    const auto dir = tmp("val_unk");
    const auto cfg = dir / (std::string(s.role) + ".json");
    const auto script_dir = dir / "script" / "python";

    write_minimal_script(script_dir);
    // Inject a bogus top-level key via merge_patch override.
    nlohmann::json overrides;
    overrides["bogus_top_level_key"] = 42;
    write_minimal_config(cfg, std::string(s.role), dir, overrides);

    WorkerProcess p(plh_role_binary(), "--role",
        {std::string(s.role), "--config", cfg.string(), "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0) << "unknown top-level key must fail";
    EXPECT_NE(p.get_stderr().find("unknown config key"), std::string::npos)
        << "stderr should identify the unknown-key rejection; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("bogus_top_level_key"), std::string::npos)
        << "stderr should echo the offending key; got:\n" << p.get_stderr();
}

} // namespace
