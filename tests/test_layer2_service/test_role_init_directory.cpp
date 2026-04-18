/**
 * @file test_role_init_directory.cpp
 * @brief L2 tests for RoleDirectory::init_directory with LogInitOverrides.
 *
 * Each test creates a fresh temp dir, invokes init_directory for one of
 * the three registered roles (producer/consumer/processor), and inspects
 * the filesystem layout + generated JSON + script-entry content.
 *
 * The registry of role init entries is process-global. SetUpTestSuite
 * registers all three roles exactly once per test binary — guarded
 * against re-registration.
 */
#include "consumer_init.hpp"
#include "processor_init.hpp"
#include "producer_init.hpp"
#include "utils/role_directory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <unistd.h>  // getpid

namespace fs = std::filesystem;
using pylabhub::utils::RoleDirectory;

namespace
{

std::string read_file(const fs::path &p)
{
    std::ifstream f(p);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

nlohmann::json load_json(const fs::path &p)
{
    std::ifstream f(p);
    return nlohmann::json::parse(f);
}

} // namespace

// ──────────────────────────────────────────────────────────────────────────────
// Fixture — registers all three roles once, cleans up temp dirs per-test
// ──────────────────────────────────────────────────────────────────────────────

class RoleInitDirectoryTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        static bool registered = false;
        if (registered)
            return;
        pylabhub::producer::register_producer_init();
        pylabhub::consumer::register_consumer_init();
        pylabhub::processor::register_processor_init();
        registered = true;
    }

  protected:
    fs::path unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_init_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p;
    }

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

  private:
    std::vector<fs::path> paths_to_clean_;
};

// ──────────────────────────────────────────────────────────────────────────────
// Standard-layout creation (per role)
// ──────────────────────────────────────────────────────────────────────────────

struct RoleSpec
{
    const char *role_tag;       // "producer" / "consumer" / "processor"
    const char *config_filename;// "producer.json" / ...
    const char *uid_prefix;     // "PROD-" / "CONS-" / "PROC-"
    const char *role_label;     // "Producer" / ...
};

constexpr RoleSpec kAll[] = {
    {"producer",  "producer.json",  "PROD-", "Producer"},
    {"consumer",  "consumer.json",  "CONS-", "Consumer"},
    {"processor", "processor.json", "PROC-", "Processor"},
};

TEST_F(RoleInitDirectoryTest, Producer_StandardLayout)
{
    const auto dir = unique_dir("prod_layout");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "TestProd"), 0);
    EXPECT_TRUE(fs::exists(dir / "producer.json"));
    EXPECT_TRUE(fs::is_directory(dir / "logs"));
    EXPECT_TRUE(fs::is_directory(dir / "vault"));
    EXPECT_TRUE(fs::is_directory(dir / "run"));
    EXPECT_TRUE(fs::is_directory(dir / "script" / "python"));
    EXPECT_TRUE(fs::exists(dir / "script" / "python" / "__init__.py"));
}

TEST_F(RoleInitDirectoryTest, Consumer_StandardLayout)
{
    const auto dir = unique_dir("cons_layout");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "TestCons"), 0);
    EXPECT_TRUE(fs::exists(dir / "consumer.json"));
    EXPECT_TRUE(fs::is_directory(dir / "logs"));
    EXPECT_TRUE(fs::is_directory(dir / "vault"));
    EXPECT_TRUE(fs::is_directory(dir / "run"));
    EXPECT_TRUE(fs::exists(dir / "script" / "python" / "__init__.py"));
}

TEST_F(RoleInitDirectoryTest, Processor_StandardLayout)
{
    const auto dir = unique_dir("proc_layout");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "TestProc"), 0);
    EXPECT_TRUE(fs::exists(dir / "processor.json"));
    EXPECT_TRUE(fs::is_directory(dir / "logs"));
    EXPECT_TRUE(fs::is_directory(dir / "vault"));
    EXPECT_TRUE(fs::is_directory(dir / "run"));
    EXPECT_TRUE(fs::exists(dir / "script" / "python" / "__init__.py"));
}

// ──────────────────────────────────────────────────────────────────────────────
// UID prefix per role
// ──────────────────────────────────────────────────────────────────────────────

TEST_F(RoleInitDirectoryTest, Producer_UidHasProdPrefix)
{
    const auto dir = unique_dir("prod_uid");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "SampleProd"), 0);
    auto j = load_json(dir / "producer.json");
    const auto uid = j["producer"]["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind("PROD-", 0), 0u)
        << "uid should start with PROD-; got: " << uid;
}

TEST_F(RoleInitDirectoryTest, Consumer_UidHasConsPrefix)
{
    const auto dir = unique_dir("cons_uid");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "SampleCons"), 0);
    auto j = load_json(dir / "consumer.json");
    const auto uid = j["consumer"]["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind("CONS-", 0), 0u)
        << "uid should start with CONS-; got: " << uid;
}

TEST_F(RoleInitDirectoryTest, Processor_UidHasProcPrefix)
{
    const auto dir = unique_dir("proc_uid");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "SampleProc"), 0);
    auto j = load_json(dir / "processor.json");
    const auto uid = j["processor"]["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind("PROC-", 0), 0u)
        << "uid should start with PROC-; got: " << uid;
}

// ──────────────────────────────────────────────────────────────────────────────
// Log overrides flow into JSON
// ──────────────────────────────────────────────────────────────────────────────

TEST_F(RoleInitDirectoryTest, LogOverrides_MaxSize_Producer)
{
    const auto dir = unique_dir("prod_maxsize");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 25.0;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
    auto j = load_json(dir / "producer.json");
    ASSERT_TRUE(j.contains("logging"));
    ASSERT_TRUE(j["logging"].contains("max_size_mb"));
    EXPECT_DOUBLE_EQ(j["logging"]["max_size_mb"].get<double>(), 25.0);
}

TEST_F(RoleInitDirectoryTest, LogOverrides_Backups_Consumer)
{
    const auto dir = unique_dir("cons_backups");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = 7;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X", ov), 0);
    auto j = load_json(dir / "consumer.json");
    ASSERT_TRUE(j.contains("logging"));
    ASSERT_TRUE(j["logging"].contains("backups"));
    EXPECT_EQ(j["logging"]["backups"].get<int>(), 7);
}

TEST_F(RoleInitDirectoryTest, LogOverrides_BackupsMinusOne_Processor)
{
    const auto dir = unique_dir("proc_backups_m1");
    RoleDirectory::LogInitOverrides ov;
    ov.backups = -1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "X", ov), 0);
    auto j = load_json(dir / "processor.json");
    EXPECT_EQ(j["logging"]["backups"].get<int>(), -1);
}

TEST_F(RoleInitDirectoryTest, LogOverrides_Both_Producer)
{
    const auto dir = unique_dir("prod_both");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 50.0;
    ov.backups     = 3;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
    auto j = load_json(dir / "producer.json");
    ASSERT_TRUE(j.contains("logging"));
    EXPECT_DOUBLE_EQ(j["logging"]["max_size_mb"].get<double>(), 50.0);
    EXPECT_EQ(j["logging"]["backups"].get<int>(), 3);
}

TEST_F(RoleInitDirectoryTest, NoOverrides_NoLoggingKeysAdded)
{
    // Without overrides, init_directory should not inject logging keys the
    // template itself didn't set. Current templates don't populate
    // "logging" at all → the JSON should lack the section entirely (or
    // have only template-defined keys).
    const auto dir = unique_dir("prod_nov");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);
    auto j = load_json(dir / "producer.json");
    // Current templates don't emit a "logging" key. If this changes in the
    // future, this test catches the silent default injection.
    EXPECT_FALSE(j.contains("logging"))
        << "Template should not emit logging without CLI overrides; "
           "found: " << j["logging"].dump();
}

TEST_F(RoleInitDirectoryTest, LogOverrides_Consumer_Same)
{
    const auto dir = unique_dir("cons_both");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 15.0;
    ov.backups     = -1;
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X", ov), 0);
    auto j = load_json(dir / "consumer.json");
    EXPECT_DOUBLE_EQ(j["logging"]["max_size_mb"].get<double>(), 15.0);
    EXPECT_EQ(j["logging"]["backups"].get<int>(), -1);
}

TEST_F(RoleInitDirectoryTest, LogOverrides_Processor_OnlyMaxSize)
{
    const auto dir = unique_dir("proc_maxonly");
    RoleDirectory::LogInitOverrides ov;
    ov.max_size_mb = 100.0;
    // backups deliberately unset.
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "X", ov), 0);
    auto j = load_json(dir / "processor.json");
    EXPECT_TRUE(j["logging"].contains("max_size_mb"));
    EXPECT_FALSE(j["logging"].contains("backups"))
        << "unset override should not appear in JSON";
}

// ──────────────────────────────────────────────────────────────────────────────
// Error paths
// ──────────────────────────────────────────────────────────────────────────────

TEST_F(RoleInitDirectoryTest, Error_UnknownRole)
{
    const auto dir = unique_dir("unknown");
    EXPECT_NE(RoleDirectory::init_directory(dir, "not_a_role", "X"), 0);
    EXPECT_FALSE(fs::exists(dir / "not_a_role.json"));
}

TEST_F(RoleInitDirectoryTest, Error_EmptyName)
{
    const auto dir = unique_dir("emptyname");
    EXPECT_NE(RoleDirectory::init_directory(dir, "producer", ""), 0);
}

TEST_F(RoleInitDirectoryTest, Error_ConfigAlreadyExists_NoOverwrite)
{
    const auto dir = unique_dir("exists");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "First"), 0);
    const auto first_mtime = fs::last_write_time(dir / "producer.json");

    // Second call must fail and not touch existing file.
    EXPECT_NE(RoleDirectory::init_directory(dir, "producer", "Second"), 0);
    EXPECT_EQ(fs::last_write_time(dir / "producer.json"), first_mtime)
        << "Existing producer.json was modified by a failed re-init";
}

// ──────────────────────────────────────────────────────────────────────────────
// Script file content
// ──────────────────────────────────────────────────────────────────────────────

TEST_F(RoleInitDirectoryTest, Producer_ScriptFile_HasOnProduceCallback)
{
    const auto dir = unique_dir("prod_script");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);
    const auto content = read_file(dir / "script" / "python" / "__init__.py");
    EXPECT_NE(content.find("on_produce"), std::string::npos);
    EXPECT_NE(content.find("on_init"),    std::string::npos);
    EXPECT_NE(content.find("on_stop"),    std::string::npos);
}

TEST_F(RoleInitDirectoryTest, Consumer_ScriptFile_HasOnConsumeCallback)
{
    const auto dir = unique_dir("cons_script");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "X"), 0);
    const auto content = read_file(dir / "script" / "python" / "__init__.py");
    EXPECT_NE(content.find("on_consume"), std::string::npos);
}

TEST_F(RoleInitDirectoryTest, Processor_ScriptFile_HasOnProcessCallback)
{
    const auto dir = unique_dir("proc_script");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "X"), 0);
    const auto content = read_file(dir / "script" / "python" / "__init__.py");
    EXPECT_NE(content.find("on_process"), std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────────────
// Name is written into the JSON identity section
// ──────────────────────────────────────────────────────────────────────────────

TEST_F(RoleInitDirectoryTest, Producer_NameInIdentity)
{
    const auto dir = unique_dir("prod_name");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "MySensor42"), 0);
    auto j = load_json(dir / "producer.json");
    EXPECT_EQ(j["producer"]["name"].get<std::string>(), "MySensor42");
}

TEST_F(RoleInitDirectoryTest, Consumer_NameInIdentity)
{
    const auto dir = unique_dir("cons_name");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "consumer", "MyLogger99"), 0);
    auto j = load_json(dir / "consumer.json");
    EXPECT_EQ(j["consumer"]["name"].get<std::string>(), "MyLogger99");
}

TEST_F(RoleInitDirectoryTest, Processor_NameInIdentity)
{
    const auto dir = unique_dir("proc_name");
    ASSERT_EQ(RoleDirectory::init_directory(dir, "processor", "MyDoubler1"), 0);
    auto j = load_json(dir / "processor.json");
    EXPECT_EQ(j["processor"]["name"].get<std::string>(), "MyDoubler1");
}
