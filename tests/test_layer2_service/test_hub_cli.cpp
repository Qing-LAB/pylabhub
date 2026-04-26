/**
 * @file test_hub_cli.cpp
 * @brief L2 tests for `pylabhub::hub_cli::parse_hub_args` + ParseResult.
 *
 * Mirror of `test_role_cli.cpp` (HEP-CORE-0024) for the hub-side parser
 * (HEP-CORE-0033 §15 Phase 2).  In-process tests — `parse_hub_args` is
 * invoked with synthetic argv and `std::ostringstream` streams that
 * capture usage / error output.  No subprocess spawning, no
 * `std::exit` (the parser returns `ParseResult{args, exit_code}`).
 *
 * Coverage: every mode, every flag, mode-exclusion, init-only
 * enforcement, log-flag parse errors, unknown-flag rejection,
 * positional-vs-flag interaction, ordering insensitivity, help output.
 *
 * `--role` is intentionally absent from the hub binary (single-kind);
 * a test case verifies it is rejected as an unknown flag.
 */
#include "utils/hub_cli.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using pylabhub::hub_cli::HubArgs;
using pylabhub::hub_cli::ParseResult;
using pylabhub::hub_cli::parse_hub_args;

namespace
{

// Helper: build an (argc, argv) pair from a string list, keeping the
// underlying storage alive for the call's duration.
struct ArgVec
{
    std::vector<std::string> owned;
    std::vector<char *>      ptrs;

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

ParseResult run(std::initializer_list<const char *> args,
                std::ostringstream                  &out,
                std::ostringstream                  &err)
{
    ArgVec av(args);
    return parse_hub_args(av.argc(), av.argv(), out, err);
}

ParseResult run(std::initializer_list<const char *> args)
{
    std::ostringstream out, err;
    return run(args, out, err);
}

} // namespace

// ─── --help / -h ─────────────────────────────────────────────────────────────

TEST(HubCliTest, Help_ExitsZeroAndPrintsUsageToStdout)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--help"}, out, err);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(err.str().empty()) << "stderr: " << err.str();
    const auto text = out.str();
    EXPECT_NE(text.find("Usage:"),        std::string::npos);
    EXPECT_NE(text.find("--init"),        std::string::npos);
    EXPECT_NE(text.find("--validate"),    std::string::npos);
    EXPECT_NE(text.find("--keygen"),      std::string::npos);
    EXPECT_NE(text.find("--log-maxsize"), std::string::npos);
    EXPECT_NE(text.find("--log-backups"), std::string::npos);
    EXPECT_NE(text.find("<hub_dir>"),     std::string::npos);
    // Hub binary must NOT advertise --role (single-kind binary).
    EXPECT_EQ(text.find("--role"),        std::string::npos);
}

TEST(HubCliTest, Help_ShortFormMinusH)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "-h"}, out, err);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(out.str().find("Usage:"), std::string::npos);
}

// ─── Init mode ────────────────────────────────────────────────────────────────

TEST(HubCliTest, InitMode_WithPositionalDir)
{
    auto r = run({"plh_hub", "--init", "/tmp/h"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_EQ(r.args.hub_dir, "/tmp/h");
    EXPECT_FALSE(r.args.validate_only);
    EXPECT_FALSE(r.args.keygen_only);
}

TEST(HubCliTest, InitMode_NoPositionalDir_EmptyHubDir)
{
    auto r = run({"plh_hub", "--init"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_TRUE(r.args.hub_dir.empty());
}

TEST(HubCliTest, InitMode_WithName)
{
    auto r = run({"plh_hub", "--init", "/tmp/h", "--name", "MainHub"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.init_only);
    EXPECT_EQ(r.args.init_name, "MainHub");
}

TEST(HubCliTest, InitMode_WithLogOverrides_BothSet)
{
    auto r = run({"plh_hub", "--init", "/tmp/h",
                  "--log-maxsize", "25", "--log-backups", "7"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_max_size_mb.has_value());
    EXPECT_DOUBLE_EQ(*r.args.log_max_size_mb, 25.0);
    ASSERT_TRUE(r.args.log_backups.has_value());
    EXPECT_EQ(*r.args.log_backups, 7);
}

TEST(HubCliTest, InitMode_LogBackupsMinusOneSentinel)
{
    auto r = run({"plh_hub", "--init", "/tmp/h", "--log-backups", "-1"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_backups.has_value());
    EXPECT_EQ(*r.args.log_backups, -1);
}

TEST(HubCliTest, InitMode_LogMaxsizeFractional)
{
    auto r = run({"plh_hub", "--init", "/tmp/h", "--log-maxsize", "0.5"});
    ASSERT_EQ(r.exit_code, -1);
    ASSERT_TRUE(r.args.log_max_size_mb.has_value());
    EXPECT_DOUBLE_EQ(*r.args.log_max_size_mb, 0.5);
}

// ─── Validate mode ───────────────────────────────────────────────────────────

TEST(HubCliTest, ValidateMode_WithConfigFlag)
{
    auto r = run({"plh_hub", "--config", "/tmp/h.json", "--validate"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.validate_only);
    EXPECT_EQ(r.args.config_path, "/tmp/h.json");
}

TEST(HubCliTest, ValidateMode_WithPositionalDir)
{
    auto r = run({"plh_hub", "/tmp/h", "--validate"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.validate_only);
    EXPECT_EQ(r.args.hub_dir, "/tmp/h");
}

// ─── Keygen mode ─────────────────────────────────────────────────────────────

TEST(HubCliTest, KeygenMode_WithConfigFlag)
{
    auto r = run({"plh_hub", "--config", "/tmp/h.json", "--keygen"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.keygen_only);
}

TEST(HubCliTest, KeygenMode_WithPositionalDir)
{
    auto r = run({"plh_hub", "/tmp/h", "--keygen"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_TRUE(r.args.keygen_only);
}

// ─── Run mode (no mode flag) ─────────────────────────────────────────────────

TEST(HubCliTest, RunMode_PositionalHubDir)
{
    auto r = run({"plh_hub", "/tmp/h"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_FALSE(r.args.init_only);
    EXPECT_FALSE(r.args.validate_only);
    EXPECT_FALSE(r.args.keygen_only);
    EXPECT_EQ(r.args.hub_dir, "/tmp/h");
}

TEST(HubCliTest, RunMode_ConfigFlag)
{
    auto r = run({"plh_hub", "--config", "/tmp/h.json"});
    ASSERT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.args.config_path, "/tmp/h.json");
    EXPECT_TRUE(r.args.hub_dir.empty());
}

// ─── Mode exclusion ──────────────────────────────────────────────────────────

TEST(HubCliTest, ModeExclusion_InitAndValidate_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "/tmp/h", "--validate"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually"), std::string::npos);
}

TEST(HubCliTest, ModeExclusion_InitAndKeygen_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "/tmp/h", "--keygen"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually"), std::string::npos);
}

TEST(HubCliTest, ModeExclusion_ValidateAndKeygen_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--validate", "--keygen"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually"), std::string::npos);
}

TEST(HubCliTest, ModeExclusion_AllThree_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "/tmp/h", "--validate", "--keygen"},
                 out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually"), std::string::npos);
}

// ─── Init-only flag enforcement ──────────────────────────────────────────────

TEST(HubCliTest, NameFlagOutsideInit_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--name", "X"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(HubCliTest, LogMaxsizeOutsideInit_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--log-maxsize", "50"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(HubCliTest, LogBackupsOutsideInit_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--log-backups", "5"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

TEST(HubCliTest, NameWithKeygen_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--keygen", "--name", "X"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("only valid with --init"), std::string::npos);
}

// ─── Log-flag parse errors ───────────────────────────────────────────────────

TEST(HubCliTest, LogMaxsizeNonNumeric_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "/tmp/h", "--log-maxsize", "abc"},
                 out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("--log-maxsize"), std::string::npos);
    EXPECT_NE(err.str().find("number"),        std::string::npos);
}

TEST(HubCliTest, LogBackupsNonInteger_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "/tmp/h", "--log-backups", "xyz"},
                 out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("--log-backups"), std::string::npos);
    EXPECT_NE(err.str().find("integer"),       std::string::npos);
}

// ─── Unknown flags ───────────────────────────────────────────────────────────

TEST(HubCliTest, UnknownFlag_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--bogus"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("Unknown argument"), std::string::npos);
    EXPECT_NE(err.str().find("--bogus"),          std::string::npos);
}

TEST(HubCliTest, RoleFlagRejected_HubIsSingleKind)
{
    // --role is a role_cli flag; the hub binary does not accept it.
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h", "--role", "producer"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("Unknown argument"), std::string::npos);
    EXPECT_NE(err.str().find("--role"),           std::string::npos);
}

// ─── Positional / flag interaction ───────────────────────────────────────────

TEST(HubCliTest, MultiplePositionalArgs_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub", "/tmp/h1", "/tmp/h2"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("multiple positional"), std::string::npos);
}

TEST(HubCliTest, InitFollowedByDashArg_NoPositional)
{
    // --init is followed by --validate (a flag, not a positional);
    // hub_dir should remain empty, and mode-exclusion catches the
    // double-mode error.
    std::ostringstream out, err;
    auto r = run({"plh_hub", "--init", "--validate"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("mutually"), std::string::npos);
}

// ─── No required arguments missing ───────────────────────────────────────────

TEST(HubCliTest, NoArgs_Rejected)
{
    std::ostringstream out, err;
    auto r = run({"plh_hub"}, out, err);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(err.str().find("specify a hub directory"), std::string::npos);
}

// ─── Ordering insensitivity ──────────────────────────────────────────────────

TEST(HubCliTest, FlagOrderDoesNotMatter)
{
    auto r1 = run({"plh_hub", "--init", "/tmp/h", "--name", "X",
                    "--log-maxsize", "20"});
    auto r2 = run({"plh_hub", "--name", "X", "--log-maxsize", "20",
                    "--init", "/tmp/h"});
    auto r3 = run({"plh_hub", "--log-maxsize", "20", "--init", "/tmp/h",
                    "--name", "X"});
    ASSERT_EQ(r1.exit_code, -1);
    ASSERT_EQ(r2.exit_code, -1);
    ASSERT_EQ(r3.exit_code, -1);
    EXPECT_EQ(r1.args.hub_dir,    "/tmp/h");
    EXPECT_EQ(r2.args.hub_dir,    "/tmp/h");
    EXPECT_EQ(r3.args.hub_dir,    "/tmp/h");
    EXPECT_EQ(r1.args.init_name,  "X");
    EXPECT_EQ(r2.args.init_name,  "X");
    EXPECT_EQ(r3.args.init_name,  "X");
}
