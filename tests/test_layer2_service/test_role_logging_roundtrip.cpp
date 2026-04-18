/**
 * @file test_role_logging_roundtrip.cpp
 * @brief L2 tests verifying CLI log flags → JSON → LoggingConfig round-trip.
 *
 * The flow under test:
 *
 *   RoleDirectory::init_directory(dir, tag, name, LogInitOverrides{…})
 *        ↓ writes logging section to producer/consumer/processor.json
 *   RoleConfig::load_from_directory(dir, tag, role_parser)
 *        ↓ parses the logging section
 *   cfg.logging().max_size_bytes / max_backup_files / timestamped
 *
 * Each test fills the overrides, runs init, reloads the generated JSON
 * through the real RoleConfig loader, and asserts the LoggingConfig
 * fields match. This verifies the parse_logging_config code path
 * end-to-end (with sentinel handling for backups == -1).
 */
#include "consumer_fields.hpp"
#include "consumer_init.hpp"
#include "processor_fields.hpp"
#include "processor_init.hpp"
#include "producer_fields.hpp"
#include "producer_init.hpp"

#include "plh_datahub.hpp"              // LifecycleGuard, JsonConfig module
#include "utils/config/logging_config.hpp"
#include "utils/config/role_config.hpp"
#include "utils/role_directory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::Logger;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::RoleDirectory;
using pylabhub::config::RoleConfig;
using pylabhub::config::LoggingConfig;

namespace
{

class RoleLoggingRoundtripTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        static bool registered = false;
        if (!registered)
        {
            pylabhub::producer::register_producer_init();
            pylabhub::consumer::register_consumer_init();
            pylabhub::processor::register_processor_init();
            registered = true;
        }
    }

  protected:
    void SetUp() override
    {
        guard_ = std::make_unique<LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                Logger::GetLifecycleModule(),
                FileLock::GetLifecycleModule(),
                JsonConfig::GetLifecycleModule()));
    }

    void TearDown() override
    {
        guard_.reset();
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    fs::path unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_lrt_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p;
    }

    std::unique_ptr<LifecycleGuard> guard_;
    std::vector<fs::path>           paths_to_clean_;
};

} // namespace

// ── Defaults preserved through roundtrip (no CLI overrides) ─────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_DefaultsPreserved)
{
    // With no overrides, the JSON has no "logging" section → parser
    // applies LoggingConfig defaults.
    const auto dir = unique_dir("prod_def");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    const auto &lc = cfg.logging();
    EXPECT_EQ(lc.max_size_bytes, 10ULL * 1024 * 1024);  // LoggingConfig default
    EXPECT_EQ(lc.max_backup_files, 5u);                 // LoggingConfig default
    EXPECT_TRUE(lc.timestamped);                        // LoggingConfig default
    EXPECT_TRUE(lc.file_path.empty());
}

// ── --log-maxsize round-trip ─────────────────────────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_MaxSize_Roundtrip)
{
    const auto dir = unique_dir("prod_ms");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 25.0;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    EXPECT_EQ(cfg.logging().max_size_bytes, 25ULL * 1024 * 1024);
}

TEST_F(RoleLoggingRoundtripTest, Consumer_MaxSize_FractionalRoundtrip)
{
    const auto dir = unique_dir("cons_ms");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 0.5;       // 0.5 MiB = 512 KiB
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "consumer", pylabhub::consumer::parse_consumer_fields);
    EXPECT_EQ(cfg.logging().max_size_bytes, 512ULL * 1024);
}

TEST_F(RoleLoggingRoundtripTest, Processor_MaxSize_Roundtrip)
{
    const auto dir = unique_dir("proc_ms");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 100.0;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "processor", pylabhub::processor::parse_processor_fields);
    EXPECT_EQ(cfg.logging().max_size_bytes, 100ULL * 1024 * 1024);
}

// ── --log-backups round-trip (positive count) ───────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_Backups_Roundtrip)
{
    const auto dir = unique_dir("prod_bk");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = 7;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    EXPECT_EQ(cfg.logging().max_backup_files, 7u);
}

TEST_F(RoleLoggingRoundtripTest, Consumer_Backups_One)
{
    const auto dir = unique_dir("cons_bk1");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = 1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "consumer", pylabhub::consumer::parse_consumer_fields);
    EXPECT_EQ(cfg.logging().max_backup_files, 1u);
}

// ── --log-backups -1 sentinel → kKeepAllBackups ─────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_Backups_MinusOne_Sentinel)
{
    const auto dir = unique_dir("prod_bkm1");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = -1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    EXPECT_EQ(cfg.logging().max_backup_files, LoggingConfig::kKeepAllBackups);
}

TEST_F(RoleLoggingRoundtripTest, Processor_Backups_MinusOne_Sentinel)
{
    const auto dir = unique_dir("proc_bkm1");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = -1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "processor", pylabhub::processor::parse_processor_fields);
    EXPECT_EQ(cfg.logging().max_backup_files, LoggingConfig::kKeepAllBackups);
}

// ── Both overrides together ──────────────────────────────────────────────────

TEST_F(RoleLoggingRoundtripTest, Producer_BothOverrides_Roundtrip)
{
    const auto dir = unique_dir("prod_both");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 50.0;
    ov.backups     = 10;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    EXPECT_EQ(cfg.logging().max_size_bytes,  50ULL * 1024 * 1024);
    EXPECT_EQ(cfg.logging().max_backup_files, 10u);
    EXPECT_TRUE(cfg.logging().timestamped);  // default preserved
}

TEST_F(RoleLoggingRoundtripTest, Consumer_BothOverrides_Roundtrip)
{
    const auto dir = unique_dir("cons_both");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 12.5;
    ov.backups     = -1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X", ov), 0);

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "consumer", pylabhub::consumer::parse_consumer_fields);
    EXPECT_EQ(cfg.logging().max_size_bytes,
              static_cast<size_t>(12.5 * 1024 * 1024));
    EXPECT_EQ(cfg.logging().max_backup_files, LoggingConfig::kKeepAllBackups);
}

// ── Invalid override values surface at config load (parser rejection) ───────

TEST_F(RoleLoggingRoundtripTest, Error_BackupsZero_RejectedAtLoad)
{
    // parse_logging_config rejects backups=0 (H-1 fix) — verifies that
    // bogus overrides passed through init don't silently succeed.
    const auto dir = unique_dir("prod_bk0");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = 0;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
    EXPECT_THROW((void)RoleConfig::load_from_directory(
                     dir.string(), "producer",
                     pylabhub::producer::parse_producer_fields),
                 std::runtime_error);
}

TEST_F(RoleLoggingRoundtripTest, Error_MaxSizeZero_RejectedAtLoad)
{
    const auto dir = unique_dir("prod_ms0");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 0.0;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
    EXPECT_THROW((void)RoleConfig::load_from_directory(
                     dir.string(), "producer",
                     pylabhub::producer::parse_producer_fields),
                 std::runtime_error);
}
