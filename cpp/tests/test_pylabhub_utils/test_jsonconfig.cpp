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
#include <gmock/gmock.h> // Added for expect_worker_ok and matchers
#include <gtest/gtest.h>
#include <memory>
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
 * Sets up a temporary directory for test artifacts. The application lifecycle,
 * including the `JsonConfig` module, is managed by a global `LifecycleGuard`
 * in the main test entry point.
 */
class JsonConfigTest : public ::testing::Test
{
  protected:
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
        &ec, true);
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
        &ec, true);
    ASSERT_TRUE(read2_ok);
}

/**
 * @brief Tests that constructing a JsonConfig object without initializing its
 *        lifecycle module results in a fatal error.
 *
 * This test runs the "uninitialized_behavior" worker in a separate process.
 * That worker attempts to construct a `JsonConfig` object without having a
 * `LifecycleGuard` in place. The expected behavior is for the constructor to
 * call `PLH_PANIC`, which aborts the child process.
 *
 * This test verifies that this protection mechanism is working correctly by
 * checking that the worker process terminates with a non-zero exit code and
 * that the stderr stream contains the expected panic message.
 */
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    WorkerProcess worker(g_self_exe_path, "jsonconfig.uninitialized_behavior", {});
    ASSERT_TRUE(worker.valid());
    const int exit_code = worker.wait_for_exit();

    // The worker process is expected to be terminated by a panic (abort),
    // which results in a non-zero exit code. On POSIX, this is often reported
    // as -1 by our helper, or a value > 128. On Windows, it's an arbitrary code.
    // The key is that it's not 0.
    ASSERT_NE(exit_code, 0);

    // Verify that the stderr output contains the expected panic message.
    const std::string& stderr_output = worker.get_stderr();
    EXPECT_THAT(stderr_output,
                ::testing::HasSubstr("JsonConfig created before its module was initialized"));
    EXPECT_THAT(stderr_output, ::testing::HasSubstr("Aborting"));
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
        &ec, /*commit_after_write=*/false));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j)
        {
            ASSERT_EQ(j.value("int_val", -1), 42);
            ASSERT_EQ(j.value("str_val", std::string{}), "hello");
        },
        &ec, /*reload_before_read=*/false)); // Read from memory, don't reload
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

    ASSERT_TRUE(cfg.with_json_write([&](json &j) { j["value"] = 1; }, &ec,
                                    /*commit_after_write=*/true));
    ASSERT_FALSE(ec);

    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // Explicitly reload to pick up the changes from disk.
    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j)
        {
            ASSERT_EQ(j.value("value", -1), 2);
            ASSERT_EQ(j.value("new_key", std::string{}), "external");
        },
        &ec,
        /*reload_before_read=*/false)); // Already reloaded, so read from memory
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

    // Make a change in memory, but don't commit it yet.
    ASSERT_TRUE(cfg.with_json_write([&](json &j) { j["value"] = "in-memory"; }, &ec,
                                    /*commit_after_write=*/false));
    ASSERT_FALSE(ec);

    // Now, overwrite the file on disk with the in-memory state.
    ASSERT_TRUE(cfg.overwrite(&ec));
    ASSERT_FALSE(ec);

    json j_disk = json::parse(read_file_contents(cfg_path));
    ASSERT_EQ(j_disk.value("value", ""), "in-memory");

    // Modify the file on disk again from an external source.
    j_disk["value"] = "from-disk";
    j_disk["new_val"] = true;
    std::ofstream out(cfg_path);
    out << j_disk.dump();
    out.close();

    // Reload the changes from disk.
    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    // Verify the reloaded state.
    ASSERT_TRUE(cfg.with_json_read(
        [&](const json &j_mem)
        {
            EXPECT_EQ(j_mem.value("value", ""), "from-disk");
            EXPECT_EQ(j_mem.value("new_val", false), true);
        },
        &ec, /*reload_before_read=*/false)); // Already reloaded
    ASSERT_FALSE(ec);
}

TEST_F(JsonConfigTest, SimplifiedApiOverloads)
{
    auto cfg_path = g_temp_dir / "simplified_api.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Write and commit to disk (default behavior)
    bool write_ok = cfg.with_json_write([&](json &j) { j["key"] = "value1"; });
    ASSERT_TRUE(write_ok);

    std::string read_value;
    // Reload from disk and read (default behavior)
    bool read_ok = cfg.with_json_read([&](const json &j) { read_value = j.value("key", ""); });
    ASSERT_TRUE(read_ok);
    ASSERT_EQ(read_value, "value1");
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
                &inner_ec, /*reload_before_read=*/false);
            ASSERT_FALSE(nested_ok);
            ASSERT_NE(inner_ec.value(), 0);
        },
        &ec, false);
    ASSERT_TRUE(outer_ok);
}

/**
 * @brief Tests error handling when loading a malformed JSON file.
 */
TEST_F(JsonConfigTest, LoadMalformedFile)
{
    auto cfg_path = g_temp_dir / "malformed.json";
    fs::remove(cfg_path);

    // Create a malformed JSON file
    {
        std::ofstream out(cfg_path);
        out << "{ \"key\": \"value\""; // Missing closing brace
    }

    JsonConfig cfg;
    std::error_code ec;

    // init should fail because reload fails on a malformed file.
    // The implementation catches the parse error and returns `io_error`.
    ASSERT_FALSE(cfg.init(cfg_path, false, &ec));
    ASSERT_EQ(ec, std::errc::io_error);

    // Test reload() directly as well.
    // First, init with a valid file.
    fs::remove(cfg_path);
    {
        std::ofstream out(cfg_path);
        out << "{}";
    }
    JsonConfig cfg2(cfg_path, false, &ec);
    ASSERT_TRUE(cfg2.is_initialized());
    ASSERT_FALSE(ec);

    // Now, corrupt the file on disk.
    {
        std::ofstream out(cfg_path);
        out << "this is not json";
    }

    // reload() should now fail with an io_error.
    ASSERT_FALSE(cfg2.reload(&ec));
    ASSERT_EQ(ec, std::errc::io_error);
}

/**
 * @brief Stress-tests file contention from multiple threads using separate objects.
 */
TEST_F(JsonConfigTest, MultiThreadFileContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    fs::remove(cfg_path);

    // Pre-populate with initial data and write it to disk.
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
            &ec, /*commit_after_write=*/true));
        ASSERT_FALSE(ec);
    }

    const int THREADS = 16;
    const int ITERS = 25;
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
                        // Atomically reload, modify, and commit.
                        bool ok = cfg.with_json_write(
                            [&](json &data)
                            {
                                int v = data.value("counter", 0);
                                data["counter"] = v + 1;
                                data["write_log"].push_back(fmt::format("T{}-வைக்", i, j));
                            },
                            &ec, /*commit_after_write=*/true);

                        if (ok && !ec)
                        {
                            successful_writes++;
                        }
                    }
                    else // ~75% chance of being a reader
                    {
                        std::error_code ec;
                        // Reload from disk to get the latest version.
                        bool ok = cfg.with_json_read(
                            [&](const json &data)
                            {
                                int cur = data.value("counter", -1);
                                if (cur < last_read_value)
                                {
                                    read_failures++;
                                }
                                last_read_value = cur;
                            },
                            &ec, /*reload_before_read=*/true);
                        if (!ok || ec)
                            read_failures++;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
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
        },
        nullptr, /*reload_before_read=*/true));
}

#if defined(PLATFORM_WIN64)
constexpr std::string prefix_info_fmt = "win-{}";
#else
constexpr std::string prefix_info_fmt = "posix-{}";
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

    // Spawn multiple worker processes that all try to write to the same file.
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "jsonconfig.write_id",
            std::vector<std::string>{cfg_path.string(),
                                     fmt::to_string(pylabhub::format_tools::make_buffer(prefix_info_fmt, i))}));
        ASSERT_TRUE(procs.back()->valid());
    }

    int success_count = 0;
    for (auto &p : procs)
    {
        p->wait_for_exit();
        if (p->exit_code() == 0)
        {
            success_count++;
            expect_worker_ok(*p);
        }
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
        },
        nullptr, true));
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
    // The object is not initialized, so we expect a 'not_connected' error.
    bool ok = cfg.with_json_write([&](json &j) { j["malicious"] = "data"; }, &ec);
    ASSERT_FALSE(ok);
    ASSERT_EQ(ec, std::errc::not_connected);

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
    // On Windows, init may not fail on a symlink, the check happens at write time.
    ASSERT_TRUE(cfg.init(symlink_path, false, &ec));
    ASSERT_FALSE(ec);

    // Write should fail because atomic_write_json refuses to operate on a symlink.
    bool ok = cfg.with_json_write([&](json &j) { j["malicious"] = "data"; }, &ec, true);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(ec); // Expect an error from the failed write

    // Verify the original file is untouched.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

/**
 * @brief Verifies the in-memory thread-safety of a SINGLE shared JsonConfig object.
 *
 * This test creates one JsonConfig object and shares it by reference with
 * multiple threads that perform concurrent in-memory reads and writes. It is
 * designed to fail if the internal `std::shared_mutex` is not working correctly
 * to prevent data races on the in-memory `nlohmann::json` object.
 *
 * All operations are in-memory only to isolate thread-safety from file I/O.
 */
TEST_F(JsonConfigTest, MultiThreadSharedObjectContention)
{
    auto cfg_path = g_temp_dir / "multithread_shared_object.json";
    fs::remove(cfg_path);

    // 1. Create a SINGLE JsonConfig object to be shared by all threads.
    JsonConfig shared_cfg(cfg_path, true);
    ASSERT_TRUE(shared_cfg.is_initialized());

    // 2. Pre-populate with initial data (in-memory only).
    shared_cfg.with_json_write([&](json &data) { data["counter"] = 0; }, nullptr, false);

    const int WRITER_THREADS = 4;
    const int READER_THREADS = 8;
    const int ITERS_PER_WRITER = 50;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;

    // 3. Spawn writer threads that increment a counter (in-memory only).
    for (int i = 0; i < WRITER_THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                for (int j = 0; j < ITERS_PER_WRITER; ++j)
                {
                    shared_cfg.with_json_write(
                        [&](json &data)
                        {
                            int v = data.value("counter", 0);
                            data["counter"] = v + 1;
                        },
                        nullptr, false); // In-memory only
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
                }
            });
    }

    // 4. Spawn reader threads that verify the counter is always increasing (in-memory only).
    for (int i = 0; i < READER_THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                int last_read_value = -1;
                auto start_time = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(1))
                {
                    shared_cfg.with_json_read(
                        [&](const json &data)
                        {
                            int cur = data.value("counter", -1);
                            if (cur < last_read_value)
                            {
                                read_failures++;
                            }
                            last_read_value = cur;
                        },
                        nullptr, false); // In-memory only
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    // 5. Final verification.
    ASSERT_EQ(read_failures.load(), 0) << "Reader threads detected non-monotonic counter changes.";

    int final_counter = 0;
    shared_cfg.with_json_read([&](const json &data) { final_counter = data.value("counter", -1); },
                                nullptr, false);

    EXPECT_EQ(final_counter, WRITER_THREADS * ITERS_PER_WRITER);
}