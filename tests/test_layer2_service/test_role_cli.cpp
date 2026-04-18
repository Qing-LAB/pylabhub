/**
 * @file test_role_cli.cpp
 * @brief L2 tests for role_cli::parse_role_args + ParseResult.
 *
 * In-process tests — parse_role_args is invoked with synthetic argv and
 * std::ostringstream streams that capture usage / error output. No
 * subprocess spawning, no std::exit (the Phase 21.0 refactor made the
 * parser return ParseResult{args, exit_code} instead).
 *
 * Coverage: every mode, every flag, mode-exclusion, init-only
 * enforcement, log-flag parse errors, unknown-flag rejection, removed
 * `--log-file` flag rejection, ordering insensitivity, help output.
 */
#include "utils/role_cli.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using pylabhub::role_cli::ParseResult;
using pylabhub::role_cli::RoleArgs;
using pylabhub::role_cli::parse_role_args;

namespace
{

// Helper: build an (argc, argv) pair from a string list, keeping the
// underlying storage alive for the call's duration.
struct ArgVec
{
    std::vector<std::string> owned;
    std::vector<char *>       ptrs;

    explicit ArgVec(std::initializer_list<const char *> args)
    {
        owned.reserve(args.size());
        for (const char *a : args) owned.emplace_back(a);
        ptrs.reserve(owned.size());
        for (auto &s : owned) ptrs.push_back(s.data());
    }
    int    argc() const { return static_cast<int>(ptrs.size()); }
    char **argv()       { return ptrs.data(); }
};

// Run parse_role_args with in-memory streams.
ParseResult run(std::initializer_list<const char *> args,
                std::ostringstream &out,
                std::ostringstream &err,
                const char *role_name = "producer")
{
    ArgVec av(args);
    return parse_role_args(av.argc(), av.argv(), role_name, out, err);
}

// Convenience overload that discards streams (for happy-path tests).
ParseResult run(std::initializer_list<const char *> args,
                const char *role_name = "producer")
{
    std::ostringstream out, err;
    return run(args, out, err, role_name);
}

} // namespace

// ─── --help / -h ─────────────────────────────────────────────────────────────

TEST(RoleCliTest, Help_ExitsZeroAndPrintsUsageToStdout)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--help"}, out, err);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(err.str().empty()) << "stderr: " << err.str();
    const auto text = out.str();
    EXPECT_NE(text.find("Usage:"),        std::string::npos);
    EXPECT_NE(text.find("--role"),        std::string::npos);
    EXPECT_NE(text.find("--init"),        std::string::npos);
    EXPECT_NE(text.find("--validate"),    std::string::npos);
    EXPECT_NE(text.find("--keygen"),      std::string::npos);
    EXPECT_NE(text.find("--log-maxsize"), std::string::npos);
    EXPECT_NE(text.find("--log-backups"), std::string::npos);
}

TEST(RoleCliTest, Help_ShortFormMinusH)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "-h"}, out, err);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(out.str().find("Usage:"), std::string::npos);
}

TEST(RoleCliTest, Help_RoleNameAppearsInUsage_Producer)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--help"}, out, err, "producer");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(out.str().find("<producer_dir>"), std::string::npos);
}

TEST(RoleCliTest, Help_RoleNameAppearsInUsage_Generic)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--help"}, out, err, "role");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(out.str().find("<role_dir>"), std::string::npos);
}

// ─── Init mode ────────────────────────────────────────────────────────────────

TEST(RoleCliTest, InitMode_WithPositionalDir)
{
    auto r = run({"plh_role", "--init", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_EQ(r.args.role_dir, "/tmp/x");
    EXPECT_FALSE(r.args.validate_only);
    EXPECT_FALSE(r.args.keygen_only);
}

TEST(RoleCliTest, InitMode_NoPositionalDir_EmptyRoleDir)
{
    auto r = run({"plh_role", "--init"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_TRUE(r.args.role_dir.empty());
}

TEST(RoleCliTest, InitMode_WithName)
{
    auto r = run({"plh_role", "--init", "/tmp/x", "--name", "MyRole"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_EQ(r.args.init_name, "MyRole");
}

TEST(RoleCliTest, InitMode_WithLogOverrides_BothSet)
{
    auto r = run({"plh_role", "--init", "/tmp/x",
                  "--log-maxsize", "25", "--log-backups", "7"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_max_size_mb.has_value());
    EXPECT_DOUBLE_EQ(*r.args.log_max_size_mb, 25.0);
    ASSERT_TRUE(r.args.log_backups.has_value());
    EXPECT_EQ(*r.args.log_backups, 7);
}

TEST(RoleCliTest, InitMode_LogBackupsMinusOneSentinel)
{
    auto r = run({"plh_role", "--init", "/tmp/x", "--log-backups", "-1"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_backups.has_value());
    EXPECT_EQ(*r.args.log_backups, -1);
}

TEST(RoleCliTest, InitMode_LogMaxsizeFractional)
{
    auto r = run({"plh_role", "--init", "/tmp/x", "--log-maxsize", "0.5"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_max_size_mb.has_value());
    EXPECT_DOUBLE_EQ(*r.args.log_max_size_mb, 0.5);
}

TEST(RoleCliTest, InitMode_DefaultLogOverridesAbsent)
{
    auto r = run({"plh_role", "--init", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_FALSE(r.args.log_max_size_mb.has_value());
    EXPECT_FALSE(r.args.log_backups.has_value());
}

// ─── Validate / Keygen / Run (default) ────────────────────────────────────────

TEST(RoleCliTest, ValidateMode)
{
    auto r = run({"plh_role", "--config", "/tmp/x.json", "--validate"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.validate_only);
    EXPECT_FALSE(r.args.init_only);
    EXPECT_FALSE(r.args.keygen_only);
}

TEST(RoleCliTest, KeygenMode)
{
    auto r = run({"plh_role", "--config", "/tmp/x.json", "--keygen"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.keygen_only);
}

TEST(RoleCliTest, RunMode_PositionalDir)
{
    auto r = run({"plh_role", "/tmp/mydir"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.role_dir, "/tmp/mydir");
    EXPECT_FALSE(r.args.init_only);
    EXPECT_FALSE(r.args.validate_only);
    EXPECT_FALSE(r.args.keygen_only);
}

TEST(RoleCliTest, RunMode_ConfigPath)
{
    auto r = run({"plh_role", "--config", "/tmp/x.json"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.config_path, "/tmp/x.json");
}

// ─── --role flag ─────────────────────────────────────────────────────────────

TEST(RoleCliTest, Role_Specified)
{
    auto r = run({"plh_role", "--role", "producer", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.role, "producer");
}

TEST(RoleCliTest, Role_NotSpecified_Empty)
{
    auto r = run({"plh_role", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.role.empty());
}

TEST(RoleCliTest, Role_ValueIsOpaque_AcceptsAnyString)
{
    // Parser doesn't validate role values — that's the binary's job.
    auto r = run({"plh_role", "--role", "some_future_role", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.role, "some_future_role");
}

// ─── Mode exclusion ──────────────────────────────────────────────────────────

TEST(RoleCliTest, Error_InitAndValidate)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x", "--validate"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually exclusive"), std::string::npos);
}

TEST(RoleCliTest, Error_InitAndKeygen)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x", "--keygen"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually exclusive"), std::string::npos);
}

TEST(RoleCliTest, Error_ValidateAndKeygen)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--config", "/tmp/x.json",
                  "--validate", "--keygen"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually exclusive"), std::string::npos);
}

TEST(RoleCliTest, Error_AllThreeModes)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x", "--validate", "--keygen"},
                 out, err);
    EXPECT_EQ(r.exit_code, 1);
}

// ─── Init-only flag enforcement ──────────────────────────────────────────────

TEST(RoleCliTest, Error_LogMaxsizeWithoutInit_RunMode)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "/tmp/x", "--log-maxsize", "10"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(RoleCliTest, Error_LogMaxsizeWithoutInit_ValidateMode)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--config", "/tmp/x.json", "--validate",
                  "--log-maxsize", "10"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
}

TEST(RoleCliTest, Error_LogBackupsWithoutInit_RunMode)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "/tmp/x", "--log-backups", "5"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(RoleCliTest, Error_NameWithoutInit_RunMode)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "/tmp/x", "--name", "Foo"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(RoleCliTest, Error_NameWithValidate)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--config", "/tmp/x.json", "--validate",
                  "--name", "Foo"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
}

TEST(RoleCliTest, Error_NameWithKeygen)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--config", "/tmp/x.json", "--keygen",
                  "--name", "Foo"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
}

// ─── Log flag parse errors ────────────────────────────────────────────────────

TEST(RoleCliTest, Error_LogMaxsize_NotANumber)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x",
                  "--log-maxsize", "abc"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("--log-maxsize expects a number"),
              std::string::npos);
}

TEST(RoleCliTest, Error_LogBackups_NotAnInteger)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x",
                  "--log-backups", "abc"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("--log-backups expects an integer"),
              std::string::npos);
}

// ─── Removed flag (Phase 18) — --log-file rejected as unknown ────────────────

TEST(RoleCliTest, Error_LogFileFlagRemoved)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "/tmp/x", "--log-file", "/tmp/app.log"},
                 out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("Unknown argument"),      std::string::npos);
    EXPECT_NE(err.str().find("--log-file"),            std::string::npos);
}

// ─── Unknown flags / positional errors ───────────────────────────────────────

TEST(RoleCliTest, Error_UnknownFlag)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--totally-not-a-flag"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("Unknown argument"), std::string::npos);
}

TEST(RoleCliTest, Error_MultiplePositionals)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "/tmp/a", "/tmp/b"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("multiple positional"), std::string::npos);
}

// ─── Missing required input ───────────────────────────────────────────────────

TEST(RoleCliTest, Error_NoDirNoConfigNoInit)
{
    std::ostringstream out, err;
    auto r = run({"plh_role"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("role directory, --init, or --config"),
              std::string::npos);
}

TEST(RoleCliTest, Error_ValidateWithoutDirOrConfig)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--validate"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
}

TEST(RoleCliTest, Error_KeygenWithoutDirOrConfig)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--keygen"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
}

// ─── Flag ordering insensitivity ──────────────────────────────────────────────

TEST(RoleCliTest, Ordering_RoleBeforeDir)
{
    auto r = run({"plh_role", "--role", "consumer", "/tmp/x"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.role,     "consumer");
    EXPECT_EQ(r.args.role_dir, "/tmp/x");
}

TEST(RoleCliTest, Ordering_DirBeforeRole)
{
    auto r = run({"plh_role", "/tmp/x", "--role", "consumer"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.role,     "consumer");
    EXPECT_EQ(r.args.role_dir, "/tmp/x");
}

TEST(RoleCliTest, Ordering_InitFlagsReordered)
{
    auto r = run({"plh_role", "--log-backups", "3",
                  "--init", "/tmp/x",
                  "--log-maxsize", "5",
                  "--name", "N"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_EQ(r.args.role_dir,    "/tmp/x");
    EXPECT_EQ(r.args.init_name,   "N");
    ASSERT_TRUE(r.args.log_max_size_mb.has_value());
    EXPECT_DOUBLE_EQ(*r.args.log_max_size_mb, 5.0);
    ASSERT_TRUE(r.args.log_backups.has_value());
    EXPECT_EQ(*r.args.log_backups, 3);
}

TEST(RoleCliTest, Ordering_ConfigAfterMode)
{
    auto r = run({"plh_role", "--validate", "--config", "/tmp/x.json"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.validate_only);
    EXPECT_EQ(r.args.config_path, "/tmp/x.json");
}

// ─── Error output includes usage ─────────────────────────────────────────────

TEST(RoleCliTest, Error_OutputIncludesUsageText)
{
    std::ostringstream out, err;
    auto r = run({"plh_role", "--init", "/tmp/x", "--validate"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    // Error message precedes usage text on stderr.
    EXPECT_NE(err.str().find("Usage:"), std::string::npos);
}
