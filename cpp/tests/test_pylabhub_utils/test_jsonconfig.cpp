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

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "utils/JsonConfig.hpp"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "nlohmann/json.hpp"
#include <fmt/core.h>

using namespace nlohmann;
using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;
namespace fs = std::filesystem;

namespace {

// Path for temporary files created during tests.
static fs::path g_temp_dir;

// Helper to read file contents for verification.
static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open()) return "";
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
class JsonConfigTest : public ::testing::Test {
protected:
    // Ensures that the required modules are initialized before each test
    // in this suite runs, and finalized after.
    pylabhub::lifecycle::LifecycleGuard guard;

    JsonConfigTest()
      : guard(
            pylabhub::utils::JsonConfig::GetLifecycleModule(),
            pylabhub::utils::FileLock::GetLifecycleModule(),
            pylabhub::utils::Logger::GetLifecycleModule()
        )
    {}

    static void SetUpTestSuite() {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
        fs::create_directories(g_temp_dir);
    }

    static void TearDownTestSuite() {
        try { fs::remove_all(g_temp_dir); } catch (...) {}
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

    // Initialize and create the file.
    std::error_code ec;
    ASSERT_TRUE(config.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);
    ASSERT_TRUE(fs::exists(cfg_path));

    // Verify the newly created file is an empty JSON object.
    bool read_ok = config.with_json_read([&](const json &j){
        ASSERT_TRUE(j.is_object());
        ASSERT_TRUE(j.empty());
    }, &ec);
    ASSERT_TRUE(read_ok);
    ASSERT_FALSE(ec);

    // Verify that initializing with an existing file works correctly.
    JsonConfig config2(cfg_path, false, &ec);
    ASSERT_FALSE(ec);
    bool read2_ok = config2.with_json_read([&](const json &j){
        ASSERT_TRUE(j.is_object());
        ASSERT_TRUE(j.empty());
    }, &ec);
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
    
    // Save should fail.
    ASSERT_FALSE(config.save(&ec));
    ASSERT_NE(ec.value(), 0);

    // Read should fail.
    ec.clear();
    ASSERT_FALSE(config.with_json_read([&](const json &){}, &ec));
    ASSERT_NE(ec.value(), 0);

    // Write should fail.
    ec.clear();
    ASSERT_FALSE(config.with_json_write([&](json &){}, std::chrono::milliseconds{0}, &ec));
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

    // Write some values using with_json_write.
    ASSERT_TRUE(cfg.with_json_write([&](json &j){
        j["int_val"] = 42;
        j["str_val"] = "hello";
        j["obj"] = json::object();
        j["obj"]["x"] = 100;
        j["obj"]["s"] = "world";
    }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    // Read back and verify the values.
    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("int_val", -1), 42);
        ASSERT_EQ(j.value("str_val", std::string{}), "hello");
        ASSERT_TRUE(j.contains("obj"));
        ASSERT_EQ(j["obj"].value("x", 0), 100);
    }, &ec));
    ASSERT_FALSE(ec);

    // Update a value and verify the change.
    ASSERT_TRUE(cfg.with_json_write([&](json &j){
        j["int_val"] = j.value("int_val", 0) + 1;
    }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("int_val", -1), 43);
    }, &ec));
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

    // Write an initial value.
    ASSERT_TRUE(cfg.with_json_write([&](json &j){ j["value"] = 1; }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    // Modify the file externally.
    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // `reload()` should detect the change and load the new content.
    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    // Verify the reloaded content.
    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("value", -1), 2);
        ASSERT_EQ(j.value("new_key", std::string{}), "external");
    }, &ec));
    ASSERT_FALSE(ec);
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
    bool outer_ok = cfg.with_json_read([&]([[maybe_unused]] const json &j){
        std::error_code inner_ec;
        bool nested_ok = cfg.with_json_read([&](const json &) {
            // This lambda should not be executed.
        }, &inner_ec);
        ASSERT_FALSE(nested_ok);
        ASSERT_NE(inner_ec.value(), 0);
    }, &ec);
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
        ASSERT_TRUE(setup_cfg.with_json_write([&](json &data){
            data["counter"] = 0;
            data["write_log"] = json::array();
        }, std::chrono::milliseconds{0}, &ec));
        ASSERT_FALSE(ec);
    }

    const int THREADS = 16;
    const int ITERS = 100;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;
    std::atomic<int> successful_writes = 0;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([=, &read_failures, &successful_writes]() {
            JsonConfig cfg(cfg_path); // Each thread gets its own object instance.
            int last_read_value = -1;

            for (int j = 0; j < ITERS; ++j)
            {
                if (std::rand() % 4 == 0) // ~25% chance of being a writer
                {
                    std::error_code ec;
                    bool ok = cfg.with_json_write([&](json &data) {
                        int v = data.value("counter", 0);
                        data["counter"] = v + 1;
                        data["write_log"].push_back(fmt::format("T{}-{}", i, j));
                    }, std::chrono::milliseconds{0}, &ec);

                    if (ok && ec.value() == 0) successful_writes++;
                }
                else // ~75% chance of being a reader
                {
                    std::error_code ec;
                    bool ok = cfg.with_json_read([&](const json &data) {
                        int cur = data.value("counter", -1);
                        if (cur < last_read_value) read_failures++;
                        last_read_value = cur;
                    }, &ec);
                    if (!ok) read_failures++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_EQ(read_failures.load(), 0);

    // Verify the final state of the file.
    JsonConfig verifier(cfg_path);
    ASSERT_TRUE(verifier.with_json_read([&](const json &data) {
        int final_counter = data.value("counter", -1);
        json final_log = data.value("write_log", json::array());
        EXPECT_EQ(final_counter, static_cast<int>(final_log.size()));
        EXPECT_GT(final_counter, 0);
    }));
}

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
#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i) {
        HANDLE h = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("win-{}", i)});
        ASSERT_NE(h, NULL_PROC_HANDLE);
        procs.push_back(h);
    }
    for (auto h : procs) {
        if (wait_for_worker_and_get_exit_code(h) == 0) success_count++;
    }
#else
    std::vector<ProcessHandle> pids;
    for (int i = 0; i < PROCS; ++i) {
        pid_t pid = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("posix-{}", i)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }
    for (auto pid : pids) {
        if (wait_for_worker_and_get_exit_code(pid) == 0) success_count++;
    }
#endif

    // All workers should have succeeded.
    ASSERT_EQ(success_count, PROCS);

    // Verify that the file contains entries from all workers.
    JsonConfig verifier(cfg_path);
    ASSERT_TRUE(verifier.with_json_read([&](const json &data) {
        for (int i = 0; i < PROCS; ++i) {
#if defined(PLATFORM_WIN64)
            std::string key = fmt::format("win-{}", i);
#else
            std::string key = fmt::format("posix-{}", i);
#endif
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
        JsonConfig cfg;
        std::error_code ec;
        ASSERT_TRUE(cfg.init(symlink_path, false, &ec));
        ASSERT_FALSE(ec);
    // Attempting to write should fail because the path is a symlink.
    bool ok = cfg.with_json_write([&](json &j){
        j["malicious"] = "data";
    }, std::chrono::milliseconds{0}, &ec);
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
    bool ok = cfg.with_json_write([&](json &j){
        j["malicious"] = "data";
    }, std::chrono::milliseconds{0}, &ec);

    ASSERT_FALSE(ok);
    ASSERT_NE(ec.value(), 0);

    // Verify the original file is untouched.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif
