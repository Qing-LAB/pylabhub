// tests/test_pylabhub_utils/test_jsonconfig.cpp
/**
 * @file test_jsonconfig.cpp
 * @brief Unit tests for the JsonConfig class.
 *
 * This file contains tests for the `pylabhub::utils::JsonConfig` class, covering
 * initialization, read/write access, thread and process safety, and security
 * features like symlink protection.
 */
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "format_tools.hpp"
#include "nlohmann/json.hpp"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "utils/JsonConfig.hpp"
#include <fmt/core.h>
#include <fmt/format.h>

using namespace nlohmann;
using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;
namespace fs = std::filesystem;

namespace
{

// Path for temporary files created during tests.
static fs::path g_temp_dir;

// Helper to read file contents for verification.
static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open())
        return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @class JsonConfigTest
 * @brief Test fixture for JsonConfig tests.
 *
 * Manages the lifecycle of required modules (JsonConfig, FileLock, Logger)
 * and sets up a temporary directory for test artifacts.
 */
class JsonConfigTest : public ::testing::Test
{
  protected:
    // Ensures that the required modules are initialized before each test
    // in this suite runs, and finalized after.
    pylabhub::utils::LifecycleGuard guard;

    JsonConfigTest()
        : guard({pylabhub::utils::JsonConfig::GetLifecycleModule(),
                 pylabhub::utils::FileLock::GetLifecycleModule(),
                 pylabhub::utils::Logger::GetLifecycleModule()})
    {
    }

    static void SetUpTestSuite()
    {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
        fs::create_directories(g_temp_dir);
    }

    static void TearDownTestSuite()
    {
        try
        {
            fs::remove_all(g_temp_dir);
        }
        catch (...)
        {
        }
    }
};

} // anonymous namespace

/**
 * @brief Tests initialization and file creation.
 * Verifies that a `JsonConfig` object can be initialized, and that it creates
 * the configuration file on disk if requested. Also tests re-initialization
 * via the constructor.
 */
TEST_F(JsonConfigTest, InitAndCreate)
{
    auto cfg_path = g_temp_dir / "init_create.json";
    fs::remove(cfg_path);

    JsonConfig config;
    ASSERT_FALSE(fs::exists(cfg_path));

    std::error_code ec;
    ASSERT_TRUE(config.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);
    ASSERT_TRUE(fs::exists(cfg_path));

    bool read_ok = config.with_json_read(
        [&](const json &j)
        {
            ASSERT_TRUE(j.is_object());
            ASSERT_TRUE(j.empty());
        },
        &ec);
    ASSERT_TRUE(read_ok);
    ASSERT_FALSE(ec);

    JsonConfig config2(cfg_path, false, &ec);
    ASSERT_FALSE(ec);
    bool read2_ok = config2.with_json_read(
        [&](const json &j)
        {
            ASSERT_TRUE(j.is_object());
            ASSERT_TRUE(j.empty());
        },
        &ec);
    ASSERT_TRUE(read2_ok);
}

/**
 * @brief Tests the behavior of a default-constructed, uninitialized object.
 * All operations on an uninitialized `JsonConfig` object should fail gracefully
 * and return an appropriate error code.
 */
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    JsonConfig config;
    std::error_code ec;

    ASSERT_FALSE(config.is_initialized());

    // Overwrite should fail.
    ASSERT_FALSE(config.overwrite(&ec));
    ASSERT_NE(ec.value(), 0);

    // Read should fail.
    ec.clear();
    ASSERT_FALSE(config.with_json_read([&](const json &) {}, &ec));
    ASSERT_NE(ec.value(), 0);

    // Write should also fail.
    ec.clear();
    ASSERT_FALSE(config.with_json_write([&](json &) {}, &ec));
    ASSERT_NE(ec.value(), 0);
}

/**
 * @brief Tests basic read and write operations using the callback-based API.
 */
TEST_F(JsonConfigTest, BasicAccessors)
{
    auto cfg_path = g_temp_dir / "accessors.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_write(
        [&](json &j)
        {
            j["int_val"] = 42;
            j["str_val"] = "hello";
        },
        &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j)
        {
            ASSERT_EQ(j.value("int_val", -1), 42);
            ASSERT_EQ(j.value("str_val", std::string{}), "hello");
        },
        &ec));
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that `reload()` correctly picks up external file modifications.
 */
TEST_F(JsonConfigTest, ReloadOnDiskChange)
{
    auto cfg_path = g_temp_dir / "reload_on_disk.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_write([&](json &j) { j["value"] = 1; }, &ec));
    ASSERT_FALSE(ec);

    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j)
        {
            ASSERT_EQ(j.value("value", -1), 2);
            ASSERT_EQ(j.value("new_key", std::string{}), "external");
        },
        &ec));
    ASSERT_FALSE(ec);
}

TEST_F(JsonConfigTest, OverwriteAndReload)
{
    auto cfg_path = g_temp_dir / "overwrite_reload.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_write([&](json &j) { j["value"] = "in-memory"; }, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.overwrite(&ec));
    ASSERT_FALSE(ec);

    json j_disk = json::parse(read_file_contents(cfg_path));
    ASSERT_EQ(j_disk.value("value", ""), "in-memory");

    j_disk["value"] = "from-disk";
    j_disk["new_val"] = true;
    std::ofstream out(cfg_path);
    out << j_disk.dump();
    out.close();

    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j_mem)
        {
            EXPECT_EQ(j_mem.value("value", ""), "from-disk");
            EXPECT_EQ(j_mem.value("new_val", false), true);
        },
        &ec));
    ASSERT_FALSE(ec);
}

TEST_F(JsonConfigTest, SimplifiedApiOverloads)
{
    auto cfg_path = g_temp_dir / "simplified_api.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    bool write_ok = cfg.with_json_write([&](json &j) { j["key"] = "value1"; });
    ASSERT_TRUE(write_ok);

    std::string read_value;
    bool read_ok = cfg.with_json_read([&](const json &j) { read_value = j.value("key", ""); });
    ASSERT_TRUE(read_ok);
    ASSERT_EQ(read_value, "value1");

    bool timed_write_ok =
        cfg.with_json_write([&](json &j) { j["key"] = "value2"; }, std::chrono::milliseconds(100));
    ASSERT_TRUE(timed_write_ok);

    read_ok = cfg.with_json_read([&](const json &j) { read_value = j.value("key", ""); });
    ASSERT_TRUE(read_ok);
    ASSERT_EQ(read_value, "value2");
}

/**
 * @brief Verifies that the internal recursion guard prevents nested read callbacks.
 */
TEST_F(JsonConfigTest, RecursionGuardForReads)
{
    auto cfg_path = g_temp_dir / "recursion_reads.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // An attempt to call with_json_read from within another with_json_read should fail.
    bool outer_ok = cfg.with_json_read(
        [&]([[maybe_unused]] const json &j)
        {
            std::error_code inner_ec;
            bool nested_ok = cfg.with_json_read(
                [&](const json &)
                {
                    // This lambda should not be executed.
                },
                &inner_ec);
            ASSERT_FALSE(nested_ok);
            ASSERT_NE(inner_ec.value(), 0);
        },
        &ec);
    ASSERT_TRUE(outer_ok);
}

/**
 * @brief Stress-tests read and write operations from multiple threads on the same object.
 */
TEST_F(JsonConfigTest, MultiThreadContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    fs::remove(cfg_path);

    // Pre-populate with initial data.
    {
        JsonConfig setup_cfg;
        std::error_code ec;
        ASSERT_TRUE(setup_cfg.init(cfg_path, true, &ec));
        ASSERT_FALSE(ec);
        ASSERT_TRUE(setup_cfg.with_json_write(
            [&](json &data)
            {
                data["counter"] = 0;
                data["write_log"] = json::array();
            },
            &ec));
        ASSERT_FALSE(ec);
    }

    const int THREADS = 16;
    const int ITERS = 100;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;
    std::atomic<int> successful_writes = 0;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [=, &read_failures, &successful_writes]()
            {
                JsonConfig cfg(cfg_path); // Each thread gets its own object instance.
                int last_read_value = -1;

                for (int j = 0; j < ITERS; ++j)
                {
                    if (std::rand() % 4 == 0) // ~25% chance of being a writer
                    {
                        std::error_code ec;
                        // Use a 0ms timeout for a non-blocking try-lock pattern
                        bool ok = cfg.with_json_write(
                            [&](json &data)
                            {
                                int v = data.value("counter", 0);
                                data["counter"] = v + 1;
                                data["write_log"].push_back(fmt::format("T{}-{}", i, j));
                            },
                            &ec, std::chrono::milliseconds(0));

                        if (ok && ec.value() == 0)
                            successful_writes++;
                    }
                    else // ~75% chance of being a reader
                    {
                        std::error_code ec;
                        bool ok = cfg.with_json_read(
                            [&](const json &data)
                            {
                                int cur = data.value("counter", -1);
                                if (cur < last_read_value)
                                    read_failures++;
                                last_read_value = cur;
                            },
                            &ec);
                        if (!ok)
                            read_failures++;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
                }
            });
    }

    for (auto &t : threads)
        t.join();

    ASSERT_EQ(read_failures.load(), 0);

    // Verify the final state of the file.
    JsonConfig verifier(cfg_path);
    ASSERT_TRUE(verifier.with_json_read(
        [&](const json &data)
        {
            int final_counter = data.value("counter", -1);
            json final_log = data.value("write_log", json::array());
            EXPECT_EQ(final_counter, static_cast<int>(final_log.size()));
            EXPECT_GT(final_counter, 0);
        }));
}

#if defined(PLATFORM_WIN64)
constexpr std::string prefix_info_fmt = "win-{}";
#define INVALID_PID_TYPE NULL_PROC_HANDLE
#else
constexpr std::string prefix_info_fmt = "posix-{}";
#define INVALID_PID_TYPE 0
#endif
/**
 * @brief Stress-tests write contention between multiple processes.
 */
TEST_F(JsonConfigTest, MultiProcessContention)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    // Create the initial empty file.
    JsonConfig creator;
    std::error_code ec;
    ASSERT_TRUE(creator.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);
    const int PROCS = 8;
    int success_count = 0;

    // Spawn multiple worker processes that all try to write to the same file.

    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        auto pid = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(),
             fmt::to_string(pylabhub::format_tools::make_buffer(prefix_info_fmt, i))});
        EXPECT_NE(pid, INVALID_PID_TYPE);
        if (pid != INVALID_PID_TYPE)
            procs.push_back(pid);
    }
    for (auto p : procs)
    {
        if (wait_for_worker_and_get_exit_code(p) == 0)
            success_count++;
    }

    // All workers should have succeeded.
    ASSERT_EQ(success_count, PROCS);

    // Verify that the file contains entries from all workers.
    JsonConfig verifier(cfg_path);
    ASSERT_TRUE(verifier.with_json_read(
        [&](const json &data)
        {
            for (int i = 0; i < PROCS; ++i)
            {
                std::string key =
                    fmt::to_string(pylabhub::format_tools::make_buffer(prefix_info_fmt, i));
                ASSERT_TRUE(data.contains(key)) << "Worker " << key << " failed to write.";
            }
        }));
}

/**
 * @brief Tests that JsonConfig refuses to write to a path that is a symbolic link.
 * This is a security measure to prevent a malicious actor from replacing the config
 * file with a symlink to overwrite a sensitive system file.
 */
#if PYLABHUB_IS_POSIX
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config_symlink.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    // Create a dummy "sensitive" file.
    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Create a symlink pointing from the config path to the sensitive file.
    fs::create_symlink(real_file, symlink_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_FALSE(cfg.init(symlink_path, false, &ec));
    // This should fail now, since init checks for symlinks
    ASSERT_TRUE(ec);
    ASSERT_EQ(ec.value(), static_cast<int>(std::errc::operation_not_permitted));

    // Attempting to write should also fail, as a double check.
    bool ok = cfg.with_json_write([&](json &j) { j["malicious"] = "data"; }, &ec);
    ASSERT_FALSE(ok);
    ASSERT_NE(ec.value(), 0);

    // Confirm the original file was not modified.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

#if defined(PLATFORM_WIN64)
TEST_F(JsonConfigTest, SymlinkAttackPreventionWindows)
{
    auto real_file = g_temp_dir / "real_file_win.txt";
    auto symlink_path = g_temp_dir / "config_win.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Creating symlinks on Windows may require administrative privileges.
    if (!CreateSymbolicLinkW(symlink_path.wstring().c_str(), real_file.wstring().c_str(), 0))
    {
        GTEST_SKIP() << "Skipping Windows symlink test: insufficient privileges.";
    }

    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(symlink_path, false, &ec));
    ASSERT_FALSE(ec);
    // Write should fail.
    bool ok = cfg.with_json_write([&](json &j) { j["malicious"] = "data"; }, &ec);

    ASSERT_FALSE(ok);
    ASSERT_NE(ec.value(), 0);

    // Verify the original file is untouched.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif
