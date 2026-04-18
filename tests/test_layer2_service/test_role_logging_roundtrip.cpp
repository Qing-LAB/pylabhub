/**
 * @file test_role_logging_roundtrip.cpp
 * @brief Pattern 3 driver: CLI log flags → JSON → LoggingConfig round-trip.
 *
 * Each TEST_F spawns a dedicated worker subprocess via IsolatedProcessTest.
 * The worker owns its lifecycle (Logger + FileLock + JsonConfig) through
 * run_gtest_worker() and asserts:
 *
 *   RoleDirectory::init_directory(dir, tag, name, LogInitOverrides{…})
 *        ↓ writes logging section to producer/consumer/processor.json
 *   RoleConfig::load_from_directory(dir, tag, role_parser)
 *        ↓ parses the logging section
 *   cfg.logging().{max_size_bytes,max_backup_files,timestamped}
 *
 * The parent only creates a unique temp dir, invokes the worker with (dir,
 * role[, overrides]), and cleans up the dir after wait_for_exit(). No
 * lifecycle is initialised in the parent — the subprocess is the only
 * owner.
 *
 * See docs/tech_draft/test_compliance_audit.md for the framework contract
 * this file now follows after correction from the earlier V1 violation.
 */
#include "test_patterns.h"
#include "utils/config/logging_config.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::config::LoggingConfig;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class RoleLoggingRoundtripTest : public IsolatedProcessTest
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

    /// Each call returns a fresh unique path and registers it for teardown.
    std::string unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_lrt_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ── Defaults preserved through roundtrip ─────────────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_DefaultsPreserved)
{
    auto dir = unique_dir("prod_def");
    auto w = SpawnWorker("role_logging.roundtrip_defaults_preserved",
                         {dir, "producer"});
    ExpectWorkerOk(w);
}

// ── --log-maxsize round-trip ─────────────────────────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_MaxSize_Roundtrip)
{
    auto dir = unique_dir("prod_ms");
    auto w   = SpawnWorker("role_logging.roundtrip_max_size",
                           {dir, "producer", "25.0",
                            std::to_string(25ULL * 1024 * 1024)});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Consumer_MaxSize_FractionalRoundtrip)
{
    auto dir = unique_dir("cons_ms");
    auto w   = SpawnWorker("role_logging.roundtrip_max_size",
                           {dir, "consumer", "0.5",
                            std::to_string(512ULL * 1024)});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Processor_MaxSize_Roundtrip)
{
    auto dir = unique_dir("proc_ms");
    auto w   = SpawnWorker("role_logging.roundtrip_max_size",
                           {dir, "processor", "100.0",
                            std::to_string(100ULL * 1024 * 1024)});
    ExpectWorkerOk(w);
}

// ── --log-backups round-trip (positive count) ───────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_Backups_Roundtrip)
{
    auto dir = unique_dir("prod_bk");
    auto w   = SpawnWorker("role_logging.roundtrip_backups",
                           {dir, "producer", "7", "7"});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Consumer_Backups_One)
{
    auto dir = unique_dir("cons_bk1");
    auto w   = SpawnWorker("role_logging.roundtrip_backups",
                           {dir, "consumer", "1", "1"});
    ExpectWorkerOk(w);
}

// ── --log-backups -1 sentinel → kKeepAllBackups ─────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_Backups_MinusOne_Sentinel)
{
    auto dir = unique_dir("prod_bkm1");
    auto w   = SpawnWorker("role_logging.roundtrip_backups",
                           {dir, "producer", "-1",
                            std::to_string(LoggingConfig::kKeepAllBackups)});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Processor_Backups_MinusOne_Sentinel)
{
    auto dir = unique_dir("proc_bkm1");
    auto w   = SpawnWorker("role_logging.roundtrip_backups",
                           {dir, "processor", "-1",
                            std::to_string(LoggingConfig::kKeepAllBackups)});
    ExpectWorkerOk(w);
}

// ── Both overrides together ──────────────────────────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_BothOverrides_Roundtrip)
{
    auto dir = unique_dir("prod_both");
    auto w   = SpawnWorker("role_logging.roundtrip_both",
                           {dir, "producer", "50.0", "10",
                            std::to_string(50ULL * 1024 * 1024), "10"});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Consumer_BothOverrides_Roundtrip)
{
    auto dir = unique_dir("cons_both");
    const std::size_t expected_bytes =
        static_cast<std::size_t>(12.5 * 1024 * 1024);
    auto w = SpawnWorker("role_logging.roundtrip_both",
                         {dir, "consumer", "12.5", "-1",
                          std::to_string(expected_bytes),
                          std::to_string(LoggingConfig::kKeepAllBackups)});
    ExpectWorkerOk(w);
}

// ── Invalid override values surface at config load ──────────────────────────

TEST_F(RoleLoggingRoundtripTest, Error_BackupsZero_RejectedAtLoad)
{
    auto dir = unique_dir("prod_bk0");
    auto w   = SpawnWorker("role_logging.roundtrip_error_backups_zero",
                           {dir, "producer"});
    ExpectWorkerOk(w);
}

TEST_F(RoleLoggingRoundtripTest, Error_MaxSizeZero_RejectedAtLoad)
{
    auto dir = unique_dir("prod_ms0");
    auto w   = SpawnWorker("role_logging.roundtrip_error_maxsize_zero",
                           {dir, "producer"});
    ExpectWorkerOk(w);
}
