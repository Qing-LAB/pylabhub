/**
 * @file test_datahub_hub_config_script.cpp
 * @brief Unit tests for HubConfig script-block configuration fields.
 *
 * Tests the getters added for the hub script package feature:
 *   hub_script_dir(), script_type(), tick_interval_ms(), health_log_interval_ms()
 *
 * Two test fixtures cover:
 *   HubConfigScriptConfiguredTest  — hub.json with all script/python fields set
 *   HubConfigScriptDefaultsTest    — hub.json with script/python fields absent
 *
 * Each fixture creates an isolated temp directory, writes a hub.json, starts a
 * minimal lifecycle (Logger + CryptoUtils + JsonConfig + HubConfig), runs tests,
 * then tears down the lifecycle before the next fixture runs.
 *
 * Secret numbers: 90001+ to avoid conflicts with other test suites.
 */
#include "plh_datahub.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GET_PID() _getpid()
#else
#include <unistd.h>
#define GET_PID() getpid()
#endif

namespace fs = std::filesystem;

namespace
{

// Write a string to a file, creating parent directories as needed.
void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot write: " + path.string());
    f << content;
}

// Generate a unique temp directory path per fixture + PID (avoids collisions
// when CI runs multiple instances or a previous run left stale directories).
fs::path make_test_hub_dir(const std::string& fixture_name)
{
    return fs::temp_directory_path() /
           ("pylabhub_test_" + fixture_name + "_" + std::to_string(GET_PID()));
}

} // namespace

// ============================================================================
// Fixture 1: all script/python fields configured
// ============================================================================

/**
 * @class HubConfigScriptConfiguredTest
 * @brief hub.json with explicit script + python block.
 *
 * JSON:
 *   "script": { "path": "./my_script", "type": "python" }
 *   "python": { "tick_interval_ms": 500, "health_log_interval_ms": 30000 }
 */
class HubConfigScriptConfiguredTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_hub_dir_ = make_test_hub_dir("HubConfigScriptConfigured");
        fs::remove_all(s_hub_dir_);  // clean stale state from previous runs
        fs::create_directories(s_hub_dir_);

        const std::string hub_json = R"({
  "hub": {
    "name": "test.hub.cfg",
    "uid":  "HUB-TESTHUB-A0B1C2D3"
  },
  "script": {
    "path": "./my_script",
    "type": "python"
  },
  "python": {
    "tick_interval_ms":      500,
    "health_log_interval_ms": 30000
  }
})";
        write_file(s_hub_dir_ / "hub.json", hub_json);

        pylabhub::HubConfig::set_config_path(s_hub_dir_ / "hub.json");
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::utils::FileLock::GetLifecycleModule(),
                pylabhub::utils::JsonConfig::GetLifecycleModule(),
                pylabhub::HubConfig::GetLifecycleModule()), std::source_location::current());
    }

    static void TearDownTestSuite()
    {
        s_lifecycle_.reset();
        fs::remove_all(s_hub_dir_);
        s_hub_dir_.clear();
    }

  protected:
    static pylabhub::HubConfig& cfg() { return pylabhub::HubConfig::get_instance(); }

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static fs::path s_hub_dir_;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
fs::path HubConfigScriptConfiguredTest::s_hub_dir_;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> HubConfigScriptConfiguredTest::s_lifecycle_;

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(HubConfigScriptConfiguredTest, HubScriptDirResolved)
{
    // hub_script_dir() = hub_dir / "my_script" / "python"
    // Use weakly_canonical to normalize the path (resolves 8.3 short names on Windows).
    const fs::path expected = fs::weakly_canonical(s_hub_dir_ / "my_script" / "python");
    EXPECT_EQ(cfg().hub_script_dir(), expected);
}

TEST_F(HubConfigScriptConfiguredTest, ScriptTypeFromJson)
{
    EXPECT_EQ(cfg().script_type(), "python");
}

TEST_F(HubConfigScriptConfiguredTest, TickIntervalFromJson)
{
    EXPECT_EQ(cfg().tick_interval_ms(), 500);
}

TEST_F(HubConfigScriptConfiguredTest, HealthLogIntervalFromJson)
{
    EXPECT_EQ(cfg().health_log_interval_ms(), 30000);
}

TEST_F(HubConfigScriptConfiguredTest, HubDirMatchesTempDir)
{
    // hub_dir() should be the directory containing hub.json.
    EXPECT_EQ(cfg().hub_dir(), s_hub_dir_);
}

// ============================================================================
// Fixture 2: script/python fields absent — verify defaults
// ============================================================================

/**
 * @class HubConfigScriptDefaultsTest
 * @brief hub.json with no script/python block → all defaults.
 *
 * JSON:
 *   { "hub": { "name": "test.hub.def" } }
 *
 * Expected defaults:
 *   tick_interval_ms()      == 1000
 *   health_log_interval_ms() == 60000
 *   hub_script_dir()        == empty path
 *   script_type()           == ""
 */
class HubConfigScriptDefaultsTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_hub_dir_ = make_test_hub_dir("HubConfigScriptDefaults");
        fs::remove_all(s_hub_dir_);
        fs::create_directories(s_hub_dir_);

        const std::string hub_json = R"({
  "hub": {
    "name": "test.hub.def"
  }
})";
        write_file(s_hub_dir_ / "hub.json", hub_json);

        pylabhub::HubConfig::set_config_path(s_hub_dir_ / "hub.json");
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::utils::FileLock::GetLifecycleModule(),
                pylabhub::utils::JsonConfig::GetLifecycleModule(),
                pylabhub::HubConfig::GetLifecycleModule()), std::source_location::current());
    }

    static void TearDownTestSuite()
    {
        s_lifecycle_.reset();
        fs::remove_all(s_hub_dir_);
        s_hub_dir_.clear();
    }

  protected:
    static pylabhub::HubConfig& cfg() { return pylabhub::HubConfig::get_instance(); }

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static fs::path s_hub_dir_;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
fs::path HubConfigScriptDefaultsTest::s_hub_dir_;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> HubConfigScriptDefaultsTest::s_lifecycle_;

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(HubConfigScriptDefaultsTest, TickIntervalDefaultIs1000)
{
    EXPECT_EQ(cfg().tick_interval_ms(), 1000);
}

TEST_F(HubConfigScriptDefaultsTest, HealthLogIntervalDefaultIs60000)
{
    EXPECT_EQ(cfg().health_log_interval_ms(), 60000);
}

TEST_F(HubConfigScriptDefaultsTest, HubScriptDirEmptyWhenAbsent)
{
    // No "script" block → hub_script_dir() returns empty path.
    EXPECT_TRUE(cfg().hub_script_dir().empty());
}

TEST_F(HubConfigScriptDefaultsTest, ScriptTypeEmptyWhenAbsent)
{
    EXPECT_TRUE(cfg().script_type().empty());
}
