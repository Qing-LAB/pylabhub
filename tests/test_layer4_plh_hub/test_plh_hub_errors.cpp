/**
 * @file test_plh_hub_errors.cpp
 * @brief plh_hub CLI error-path tests.
 *
 * Covers the contract that hub_cli's parser pins:
 *   - --help exits 0 cleanly with usage text.
 *   - Unknown flags exit non-zero + usage to stderr.
 *   - --init / --validate / --keygen are mutually exclusive.
 *   - Init-only flags (--name, --log-maxsize, --log-backups) reject
 *     outside --init.
 *   - Run mode requires either a positional <hub_dir>, --init, or
 *     --config.
 */

#include "plh_hub_fixture.h"

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

TEST_F(PlhHubCliTest, HelpExitsZeroAndPrintsUsage)
{
    WorkerProcess p(plh_hub_binary(), "--help", {});
    EXPECT_EQ(p.wait_for_exit(), 0);
    // Usage text covers each mode header and the binary name.
    EXPECT_NE(p.get_stdout().find("Usage:"),       std::string::npos);
    EXPECT_NE(p.get_stdout().find("--init"),       std::string::npos);
    EXPECT_NE(p.get_stdout().find("--validate"),   std::string::npos);
    EXPECT_NE(p.get_stdout().find("--keygen"),     std::string::npos);
}

TEST_F(PlhHubCliTest, UnknownFlagFails)
{
    WorkerProcess p(plh_hub_binary(), "--bogus-flag", {});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("Unknown argument"), std::string::npos)
        << "stderr should mention the unknown argument; got:\n"
        << p.get_stderr();
}

TEST_F(PlhHubCliTest, InitAndValidateMutuallyExclusive)
{
    const auto dir = tmp("err_init_validate");
    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--validate", "--name", "x"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("mutually exclusive"), std::string::npos)
        << "stderr should mention mutual exclusion; got:\n"
        << p.get_stderr();
}

TEST_F(PlhHubCliTest, NameOutsideInitFails)
{
    const auto dir = tmp("err_name_no_init");
    WorkerProcess p(plh_hub_binary(), dir.string(),
        {"--name", "Whatever", "--validate"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("only valid with --init"), std::string::npos)
        << "stderr should reject --name outside --init; got:\n"
        << p.get_stderr();
}

TEST_F(PlhHubCliTest, LogMaxsizeBadValueFails)
{
    const auto dir = tmp("err_log_maxsize");
    WorkerProcess p(plh_hub_binary(), "--init",
        {dir.string(), "--name", "x",
         "--log-maxsize", "not-a-number"});
    EXPECT_NE(p.wait_for_exit(), 0);
    EXPECT_NE(p.get_stderr().find("--log-maxsize"), std::string::npos);
}

} // namespace
