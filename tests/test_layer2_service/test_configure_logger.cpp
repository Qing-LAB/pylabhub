/**
 * @file test_configure_logger.cpp
 * @brief Pattern 3 driver: scripting::configure_logger_from_config tests.
 *
 * Each TEST_F spawns a worker subprocess (IsolatedProcessTest) that owns
 * its own lifecycle (Logger + FileLock + JsonConfig) via run_gtest_worker.
 * The worker invokes configure_logger_from_config against a RoleConfig
 * loaded from a fresh role directory, asserts the resulting file layout +
 * rotation parameters, and restores the console sink before teardown so
 * LifecycleGuard doesn't write into the dir the parent is about to
 * remove.
 *
 * See docs/tech_draft/test_compliance_audit.md for the framework contract
 * this file now follows after correction from the earlier V1 violation.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class ConfigureLoggerTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string make_role_dir(const char *label)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_cfl_" + std::string(label) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ─── Auto-composed path ──────────────────────────────────────────────────────

TEST_F(ConfigureLoggerTest, AutoComposedPath_FileAppearsUnderLogsDir)
{
    auto dir = make_role_dir("autopath");
    auto w   = SpawnWorker("role_logging.configure_auto_composed_path", {dir});
    ExpectWorkerOk(w);
}

// ─── Rotation-param flow (max_size + backups from LoggingConfig) ─────────────

TEST_F(ConfigureLoggerTest, RotationParams_FlowFromConfig)
{
    auto dir = make_role_dir("rotparams");
    auto w   = SpawnWorker("role_logging.configure_rotation_params", {dir});
    ExpectWorkerOk(w);
}

TEST_F(ConfigureLoggerTest, RotationParams_KeepAllSentinel)
{
    auto dir = make_role_dir("keepall");
    auto w   = SpawnWorker("role_logging.configure_keep_all_sentinel", {dir});
    ExpectWorkerOk(w);
}

// ─── Unwritable path surfaces error via std::error_code (POSIX only) ─────────

TEST_F(ConfigureLoggerTest, Error_UnwritableDir_ErrorCodeSet)
{
    auto dir = make_role_dir("unwritable");
    auto w   = SpawnWorker("role_logging.configure_unwritable_dir", {dir});
    // Logger emits ONE expected [ERROR ] line when the rotating sink cannot
    // be attached — it's the behaviour under test. Declare it so the harness
    // does not fail the worker on "stderr contains ERROR".
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Failed to attach rotating log sink"});
}

// ─── Explicit file_path in LoggingConfig overrides auto-compose ─────────────

TEST_F(ConfigureLoggerTest, ExplicitFilePath_UsedInsteadOfAutoCompose)
{
    auto dir = make_role_dir("explicit_path");
    auto w   = SpawnWorker("role_logging.configure_explicit_file_path", {dir});
    ExpectWorkerOk(w);
}
