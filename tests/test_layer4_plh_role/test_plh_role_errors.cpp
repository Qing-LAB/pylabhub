/**
 * @file test_plh_role_errors.cpp
 * @brief plh_role cross-cutting CLI error paths.
 *
 * These tests pin binary-level behaviour for parser / dispatch errors
 * that precede any specific mode.  The binary must exit non-zero with a
 * diagnostic whose content matches the source-defined error string, and
 * must never hang or segfault.
 *
 * The exact error strings are defined in:
 *   - src/include/utils/role_cli.hpp (parser)
 *   - src/plh_role/plh_role_main.cpp (dispatch + mode handlers)
 *
 * Stderr-content pins are mandatory: an exit-code-only check would let a
 * segfault (exit 128+N) silently satisfy EXPECT_NE(exit, 0).
 */

#include "plh_role_fixture.h"

using namespace pylabhub::tests::plh_role_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// ── --role dispatch errors ──────────────────────────────────────────────────

/// Run-mode attempted without --role → error with "available roles" hint.
/// Pins the register_and_lookup guard in plh_role_main.cpp that refuses
/// to dispatch without a role tag.  The diagnostic text comes from
/// `"Error: --role <tag> is required"` + `print_available_roles`.
TEST_F(PlhRoleCliTest, RunModeWithoutRoleFails)
{
    const auto dir = tmp("norole");

    WorkerProcess p(plh_role_binary(), dir.string(), {});
    EXPECT_NE(p.wait_for_exit(10), 0);
    const std::string &err = p.get_stderr();
    EXPECT_NE(err.find("--role"), std::string::npos)
        << "stderr should mention '--role'; got:\n" << err;
    EXPECT_NE(err.find("Available roles"), std::string::npos)
        << "stderr should list available roles; got:\n" << err;
}

/// Unknown role → error listing available roles.  Pins the RoleRegistry
/// lookup failure diagnostic path in plh_role_main.cpp::register_and_lookup.
TEST_F(PlhRoleCliTest, UnknownRoleFails)
{
    const auto dir = tmp("badrole");

    WorkerProcess p(plh_role_binary(), "--role",
        {"some_future_role", dir.string()});
    EXPECT_NE(p.wait_for_exit(10), 0);
    const std::string &err = p.get_stderr();
    EXPECT_NE(err.find("unknown role"), std::string::npos)
        << "stderr should identify the error class; got:\n" << err;
    // Diagnostic should help the user recover — list available roles.
    EXPECT_NE(err.find("producer"),  std::string::npos)
        << "stderr should list 'producer' as an available role; got:\n" << err;
    EXPECT_NE(err.find("consumer"),  std::string::npos);
    EXPECT_NE(err.find("processor"), std::string::npos);
}

// ── Help / usage ────────────────────────────────────────────────────────────

/// --help prints usage to STDOUT (not stderr) and exits 0.  Pins the
/// `out_stream = std::cout` default in parse_role_args — errors go to
/// stderr, help goes to stdout (UX contract for shell piping).
TEST_F(PlhRoleCliTest, HelpLongFormExitsZero)
{
    WorkerProcess p(plh_role_binary(), "--help", {});
    EXPECT_EQ(p.wait_for_exit(10), 0);
    const std::string &out = p.get_stdout();
    // "Usage:" is the literal header from print_role_usage.
    EXPECT_NE(out.find("Usage:"), std::string::npos)
        << "help output should contain 'Usage:'; got stdout:\n" << out;
    // Stderr should be empty for --help (no errors reported).
    EXPECT_TRUE(p.get_stderr().empty())
        << "help should write nothing to stderr; got:\n" << p.get_stderr();
}

/// -h is equivalent to --help — same exit code, same stream, same content.
TEST_F(PlhRoleCliTest, HelpShortFormExitsZero)
{
    WorkerProcess p(plh_role_binary(), "-h", {});
    EXPECT_EQ(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stdout().find("Usage:"), std::string::npos)
        << "'-h' should print usage to stdout; got:\n" << p.get_stdout();
    EXPECT_TRUE(p.get_stderr().empty());
}

/// --help wins over any other flag, regardless of order.  Pins the
/// "--help returns immediately from the parse loop" contract so users
/// can always discover CLI shape, even with malformed commands.
TEST_F(PlhRoleCliTest, HelpWinsOverOtherFlags)
{
    // --help after other args — parser still returns 0 on first encounter.
    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", "producer", "--validate", "--help"});
    EXPECT_EQ(p.wait_for_exit(10), 0)
        << "--help must win over --init+--validate; got exit="
        << p.wait_for_exit(10) << "\nstderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("Usage:"), std::string::npos);
}

// ── Unknown flag ─────────────────────────────────────────────────────────────

/// Unrecognized flag → "Unknown argument:" to stderr + exit 1.  Pins the
/// fallback branch in parse_role_args for args starting with '-'.
TEST_F(PlhRoleCliTest, UnknownFlagFails)
{
    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", "--no-such-flag"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("Unknown argument"), std::string::npos)
        << "stderr should contain 'Unknown argument'; got:\n" << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("--no-such-flag"), std::string::npos)
        << "stderr should echo the offending flag; got:\n" << p.get_stderr();
}

// ── Config path errors ──────────────────────────────────────────────────────

/// --config pointing at a non-existent file → error.
TEST_F(PlhRoleCliTest, ConfigFileNotFoundFails)
{
    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", "--config", "/nonexistent/path/to/config.json",
         "--validate"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("Config error"), std::string::npos)
        << "stderr should contain 'Config error'; got:\n" << p.get_stderr();
}

/// <dir> positional pointing at a plain file (not a directory) → error.
/// Diagnostic must identify the path-related failure so the user knows
/// what to fix.
TEST_F(PlhRoleCliTest, DirPositionalPointsAtFileFails)
{
    const auto dir = tmp("not_a_dir");
    const auto file = dir / "some_file.txt";
    write_file(file, "not a role dir");

    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", file.string(), "--validate"});
    ASSERT_NE(p.wait_for_exit(10), 0);
    // Stderr should mention "Config error" (the binary's standard
    // failure prefix for path/parse issues).  Prevents "any error
    // counts" — pins the specific failure class.
    EXPECT_NE(p.get_stderr().find("Config error"), std::string::npos)
        << "stderr should contain 'Config error'; got:\n" << p.get_stderr();
}

/// Multiple positional arguments → parser refuses.  Pins that only one
/// <role_dir> is accepted.
TEST_F(PlhRoleCliTest, MultiplePositionalsFails)
{
    const auto d1 = tmp("multi_a");
    const auto d2 = tmp("multi_b");

    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", d1.string(), d2.string()});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("multiple positional"), std::string::npos)
        << "stderr should identify the duplicate-positional error; got:\n"
        << p.get_stderr();
}

// ── Mutually exclusive flag combinations ────────────────────────────────────

/// --init + --validate are mutually exclusive.  The parser enforces this
/// AFTER the parsing loop via mode_count > 1.  Diagnostic is
/// "Error: --init, --validate, and --keygen are mutually exclusive".
TEST_F(PlhRoleCliTest, InitPlusValidateFails)
{
    const auto dir = tmp("init_val");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", "producer", dir.string(), "--name", "X", "--validate"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("mutually exclusive"), std::string::npos)
        << "stderr should identify the exclusive-mode violation; got:\n"
        << p.get_stderr();
}

/// --init + --keygen are mutually exclusive.
TEST_F(PlhRoleCliTest, InitPlusKeygenFails)
{
    const auto dir = tmp("init_kg");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", "producer", dir.string(), "--name", "X", "--keygen"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("mutually exclusive"), std::string::npos)
        << "stderr should identify the exclusive-mode violation; got:\n"
        << p.get_stderr();
}

/// --validate + --keygen are mutually exclusive.
TEST_F(PlhRoleCliTest, ValidatePlusKeygenFails)
{
    const auto dir = tmp("val_kg");
    const auto cfg = dir / "producer.json";
    write_minimal_config(cfg, "producer", dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", "--config", cfg.string(), "--validate", "--keygen"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("mutually exclusive"), std::string::npos)
        << "stderr should identify the exclusive-mode violation; got:\n"
        << p.get_stderr();
}

// ── Init-only flags outside --init ───────────────────────────────────────────

/// --log-maxsize / --log-backups / --name are --init-only.  Using them
/// in run/validate/keygen mode must fail with a specific diagnostic.
/// Pins the post-loop guard in parse_role_args.
TEST_F(PlhRoleCliTest, LogMaxsizeOutsideInitFails)
{
    const auto dir = tmp("lm_out");
    const auto cfg = dir / "producer.json";
    write_minimal_config(cfg, "producer", dir);

    WorkerProcess p(plh_role_binary(), "--role",
        {"producer", "--config", cfg.string(), "--validate",
         "--log-maxsize", "5"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("only valid with --init"), std::string::npos)
        << "stderr should identify the init-only flag misuse; got:\n"
        << p.get_stderr();
}

/// Missing mode + no dir + no --config → "specify a role directory …".
TEST_F(PlhRoleCliTest, NoModeNoDirFails)
{
    WorkerProcess p(plh_role_binary(), "--role", {"producer"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("specify a role directory"),
              std::string::npos)
        << "stderr should prompt user to specify input; got:\n"
        << p.get_stderr();
}

// ── Malformed CLI flag values ───────────────────────────────────────────────

/// --log-maxsize with a non-numeric value → parse error.  Exact message
/// per role_cli.hpp:338: "Error: --log-maxsize expects a number (MB)".
TEST_F(PlhRoleCliTest, LogMaxsizeNonNumericFails)
{
    const auto dir = tmp("logmax_bad");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", "producer", dir.string(), "--name", "X",
         "--log-maxsize", "not_a_number"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("--log-maxsize expects a number"),
              std::string::npos)
        << "stderr should identify the parse error; got:\n"
        << p.get_stderr();
}

/// --log-backups with a non-numeric value → parse error.  Exact message
/// per role_cli.hpp:348: "Error: --log-backups expects an integer".
TEST_F(PlhRoleCliTest, LogBackupsNonNumericFails)
{
    const auto dir = tmp("logbk_bad_str");

    WorkerProcess p(plh_role_binary(), "--init",
        {"--role", "producer", dir.string(), "--name", "X",
         "--log-backups", "lots"});
    EXPECT_NE(p.wait_for_exit(10), 0);
    EXPECT_NE(p.get_stderr().find("--log-backups expects an integer"),
              std::string::npos)
        << "stderr should identify the parse error; got:\n"
        << p.get_stderr();
}

/// --log-backups with a below-sentinel negative value (< -1) →
/// accepted at init time (written as-is into the config), REJECTED
/// at validate time (the L2 parser enforces the range).  This test
/// pins the end-to-end behaviour: init threads CLI values through
/// without deep validation; validate is where range rejection lands.
///
/// Rationale for split: init is a template generator, so it shouldn't
/// duplicate parser logic.  The rejection surfaces when the user
/// actually tries to RUN or --validate the config.
TEST_F(PlhRoleCliTest, LogBackupsBelowSentinelRejectedByValidate)
{
    const auto dir = tmp("logbk_bad");
    const auto cfg = dir / "producer.json";

    // Step 1: init accepts -2 and writes it into logging.backups.
    WorkerProcess init_p(plh_role_binary(), "--init",
        {"--role", "producer", dir.string(), "--name", "X",
         "--log-backups", "-2"});
    ASSERT_EQ(init_p.wait_for_exit(10), 0)
        << "init must accept -2 at CLI level (threads value through)";

    // Step 2: validate rejects the resulting config.
    WorkerProcess val_p(plh_role_binary(), "--role",
        {"producer", "--config", cfg.string(), "--validate"});
    ASSERT_NE(val_p.wait_for_exit(10), 0)
        << "validate must reject a config with logging.backups=-2";
    EXPECT_NE(val_p.get_stderr().find("Config error"), std::string::npos)
        << "stderr should contain 'Config error'; got:\n"
        << val_p.get_stderr();
}

} // namespace
