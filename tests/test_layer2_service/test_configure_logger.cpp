/**
 * @file test_configure_logger.cpp
 * @brief L2 tests for scripting::configure_logger_from_config.
 *
 * Verifies path composition + rotation parameter flow from a parsed
 * RoleConfig into Logger::set_rotating_logfile. The logger worker
 * thread is live in each test (LifecycleGuard). Because
 * set_rotating_logfile is async, we flush() after the call and then
 * stat the expected file path on disk.
 */
#include "producer_fields.hpp"
#include "producer_init.hpp"

#include "plh_datahub.hpp"
#include "utils/config/logging_config.hpp"
#include "utils/config/role_config.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_main_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <regex>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::Logger;
using pylabhub::utils::RoleDirectory;
using pylabhub::config::LoggingConfig;
using pylabhub::config::RoleConfig;

namespace
{

class ConfigureLoggerTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        static bool registered = false;
        if (!registered)
        {
            pylabhub::producer::register_producer_init();
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
        // Return logger to console sink so subsequent tests aren't writing
        // into our soon-to-be-deleted temp dir.
        (void)Logger::instance().set_console();
        Logger::instance().flush();
        guard_.reset();

        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    fs::path make_role_dir(const char *label)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_cfl_" + std::string(label) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p;
    }

    RoleConfig init_and_load(const fs::path &dir,
                              const RoleDirectory::LogInitOverrides &ov = {})
    {
        EXPECT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
        return RoleConfig::load_from_directory(
            dir.string(), "producer",
            pylabhub::producer::parse_producer_fields);
    }

    std::unique_ptr<LifecycleGuard> guard_;
    std::vector<fs::path>           paths_to_clean_;
};

} // namespace

// ─── Auto-composed path ──────────────────────────────────────────────────────

TEST_F(ConfigureLoggerTest, AutoComposedPath_FileAppearsUnderLogsDir)
{
    const auto dir = make_role_dir("autopath");
    auto cfg = init_and_load(dir);
    const std::string uid = cfg.identity().uid;

    std::error_code ec;
    const bool ok = pylabhub::scripting::configure_logger_from_config(
        cfg, ec, "[test]");
    ASSERT_TRUE(ok) << "configure_logger_from_config failed: " << ec.message();

    // Emit a log line so the sink creates the file, then flush.
    LOGGER_INFO("[test] auto-path probe");
    Logger::instance().flush();

    // timestamped mode (the default) writes to <logs>/<uid>-<ts>.log —
    // find the first file whose name matches <uid>-*.log.
    const fs::path logs_dir = dir / "logs";
    ASSERT_TRUE(fs::is_directory(logs_dir));

    bool found = false;
    const std::regex re("^" + uid + R"(-\d{4}-\d{2}-\d{2}-\d{2}-\d{2}-\d{2}\.\d+\.log$)");
    for (const auto &ent : fs::directory_iterator(logs_dir))
    {
        if (std::regex_match(ent.path().filename().string(), re))
        {
            found = true;
            EXPECT_GT(fs::file_size(ent.path()), 0u);
            break;
        }
    }
    EXPECT_TRUE(found)
        << "No file matching <uid>-<ts>.log appeared under " << logs_dir.string();
}

// ─── Rotation-param flow (max_size + backups from LoggingConfig) ─────────────

TEST_F(ConfigureLoggerTest, RotationParams_FlowFromConfig)
{
    const auto dir = make_role_dir("rotparams");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 1.0;   // small for quick rotation in later tests
    ov.backups     = 3;
    auto cfg = init_and_load(dir, ov);

    EXPECT_EQ(cfg.logging().max_size_bytes, 1ULL * 1024 * 1024);
    EXPECT_EQ(cfg.logging().max_backup_files, 3u);
    EXPECT_TRUE(cfg.logging().timestamped);

    std::error_code ec;
    EXPECT_TRUE(pylabhub::scripting::configure_logger_from_config(
        cfg, ec, "[test]"));
}

TEST_F(ConfigureLoggerTest, RotationParams_KeepAllSentinel)
{
    const auto dir = make_role_dir("keepall");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = -1;   // sentinel → kKeepAllBackups
    auto cfg = init_and_load(dir, ov);
    EXPECT_EQ(cfg.logging().max_backup_files, LoggingConfig::kKeepAllBackups);

    std::error_code ec;
    EXPECT_TRUE(pylabhub::scripting::configure_logger_from_config(
        cfg, ec, "[test]"));
}

// ─── Unwritable path surfaces error via std::error_code ─────────────────────

TEST_F(ConfigureLoggerTest, Error_UnwritableDir_ErrorCodeSet)
{
    // Use a role dir whose parent is unwritable so Logger's pre-flight fails.
    // On POSIX: create a dir with read-only perms and put the role inside.
    const auto parent = make_role_dir("unwritable_parent");
    fs::create_directories(parent);

    // We still need a RoleConfig. Init into a sibling writable dir, then
    // manually mutate cfg.logging().file_path to point into an unwritable
    // location. Simpler approach: init normally, then invoke
    // set_rotating_logfile with an explicit bad path here too via the
    // logger (since configure_logger_from_config uses cfg).
    //
    // To test configure_logger_from_config's own pre-flight, we
    // override the config's base_dir to an unwritable path by swapping
    // the logging.file_path to an absolute unwritable path.
    const auto role_dir = make_role_dir("unwritable_role");
    auto cfg = init_and_load(role_dir);
    // Re-emit LoggingConfig with a clearly unwritable file_path.
    // We can't mutate cfg directly, so instead emit an override at the
    // std::filesystem layer: delete logs/, make parent read-only,
    // and attempt configure.
#if defined(__unix__) || defined(__APPLE__)
    const auto logs = role_dir / "logs";
    ASSERT_TRUE(fs::remove_all(logs));
    // Make role_dir itself read-only so creating logs/ fails.
    fs::permissions(role_dir, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);

    std::error_code ec;
    const bool ok = pylabhub::scripting::configure_logger_from_config(
        cfg, ec, "[test]");

    // Restore perms so TearDown can rm -r the tree.
    fs::permissions(role_dir, fs::perms::owner_all, fs::perm_options::replace);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(ec) << "expected a non-empty error_code on pre-flight failure";
#else
    GTEST_SKIP() << "Unwritable-path test is POSIX-only (permission bits).";
#endif
}

// ─── Explicit file_path in LoggingConfig overrides auto-compose ─────────────

TEST_F(ConfigureLoggerTest, ExplicitFilePath_UsedInsteadOfAutoCompose)
{
    // LoggingConfig.file_path isn't exposed via CLI overrides — users set
    // it directly in the JSON. We simulate that by writing the dir first,
    // hand-editing producer.json's logging.file_path, then loading.
    const auto dir = make_role_dir("explicit_path");
    RoleDirectory::LogInitOverrides ov;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);

    // Inject explicit file_path into the generated JSON.
    const fs::path explicit_log = dir / "logs" / "custom_name.log";
    {
        std::ifstream in(dir / "producer.json");
        nlohmann::json j = nlohmann::json::parse(in);
        j["logging"]["file_path"]  = explicit_log.string();
        j["logging"]["timestamped"] = false;  // use plain file — easier to check
        in.close();
        std::ofstream out(dir / "producer.json");
        out << j.dump(2);
    }

    auto cfg = RoleConfig::load_from_directory(
        dir.string(), "producer", pylabhub::producer::parse_producer_fields);
    EXPECT_EQ(cfg.logging().file_path, explicit_log.string());

    std::error_code ec;
    ASSERT_TRUE(pylabhub::scripting::configure_logger_from_config(
        cfg, ec, "[test]")) << ec.message();

    LOGGER_INFO("[test] explicit-path probe");
    Logger::instance().flush();

    // With timestamped=false, the file path is used verbatim.
    EXPECT_TRUE(fs::exists(explicit_log))
        << "Expected explicit log at " << explicit_log.string();
}
