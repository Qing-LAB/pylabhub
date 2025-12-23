// tests/test_jsonconfig.cpp
//
// Unit tests for pylabhub::utils::JsonConfig.

#include "helpers/test_process_utils.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "platform.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"

#include "helpers/test_entrypoint.h" // provides extern std::string g_self_exe_path
#include "helpers/workers.h"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
using namespace test_utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// TODO: Add tests for handling corrupted/unparseable JSON files on init() and reload().

namespace {

// --- Test Globals & Helpers ---
static fs::path g_temp_dir;

static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open()) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Test fixture to manage the temp directory and utils lifecycle.
class JsonConfigTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
        fs::create_directories(g_temp_dir);
        fmt::print("Using temporary directory: {}\n", g_temp_dir.string());
    }

    static void TearDownTestSuite() {
        try { fs::remove_all(g_temp_dir); } catch (...) {}
    }
};

} // anonymous namespace

// What: Verifies the basic initialization of a JsonConfig object.
// How: It calls `init()` with `create_if_not_found=true` on a non-existent file
//      and asserts that the file is created. It then locks the config, which
//      loads the file, and verifies it contains an empty JSON object.
TEST_F(JsonConfigTest, InitAndCreate)
{
    auto cfg_path = g_temp_dir / "init_create.json";
    fs::remove(cfg_path);

    JsonConfig config;
    ASSERT_FALSE(fs::exists(cfg_path));

    ASSERT_TRUE(config.init(cfg_path, true));
    ASSERT_TRUE(fs::exists(cfg_path));

    // Lock to load the newly created empty file
    ASSERT_TRUE(config.lock());
    ASSERT_TRUE(config.as_json().is_object());
    ASSERT_TRUE(config.as_json().empty());
    config.unlock();

    // Re-init from existing file
    JsonConfig config2(cfg_path);
    ASSERT_TRUE(config2.lock()); // loads the file
    ASSERT_TRUE(config2.as_json().is_object());
    ASSERT_TRUE(config2.as_json().empty());
    config2.unlock();
}

// What: Verifies that all API calls on a default-constructed (uninitialized)
//       JsonConfig object are safe no-ops that return `false`.
// How: It calls all major API functions (`set`, `get`, `erase`, `save`, etc.) on
//      an uninitialized object and asserts that each call returns `false` or a
//      default value, ensuring no crashes or undefined behavior.
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    JsonConfig config;

    ASSERT_FALSE(config.lock());
    ASSERT_FALSE(config.set("key", "value"));
    ASSERT_FALSE(config.erase("key"));
    ASSERT_FALSE(config.update("key", [](json &j) { j = 1; }));
    ASSERT_FALSE(config.save());
    ASSERT_FALSE(config.replace(json::object()));
    ASSERT_FALSE(config.with_json_write([](json &j) { j["a"] = 1; }));

    ASSERT_FALSE(config.has("key"));
    ASSERT_EQ(config.get_or<int>("key", 42), 42);
    ASSERT_TRUE(config.as_json().is_object());
    ASSERT_TRUE(config.as_json().empty());
}

// What: Tests the core getters and setters for various data types.
// How: It initializes a config, locks it, and uses `set`, `get`, `get_or`, `has`,
//      `erase`, and `update` to manipulate values. It asserts that the data is
//      correctly written to and read from the in-memory JSON object.
TEST_F(JsonConfigTest, BasicAccessors)
{
    auto cfg_path = g_temp_dir / "accessors.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Writers need a lock
    ASSERT_TRUE(cfg.lock());
    ASSERT_TRUE(cfg.set("int_val", 42));
    ASSERT_TRUE(cfg.set("str_val", "hello"));
    ASSERT_TRUE(cfg.update("obj",
                           [](json &j) {
                               j["x"] = 100;
                               j["s"] = "world";
                           }));
    // still holding lock for reads
    int int_val = 0;
    ASSERT_TRUE(cfg.get("int_val", int_val));
    ASSERT_EQ(int_val, 42);
    ASSERT_EQ(cfg.get_or<int>("int_val", 0), 42);
    ASSERT_EQ(cfg.get_or<int>("nonexistent", 99), 99);

    ASSERT_TRUE(cfg.has("int_val"));
    ASSERT_FALSE(cfg.has("nonexistent"));

    std::string str_val;
    ASSERT_TRUE(cfg.get("str_val", str_val));
    ASSERT_EQ(str_val, "hello");

    json j;
    ASSERT_TRUE(cfg.get("obj", j));
    ASSERT_EQ(j["x"], 100);
    ASSERT_EQ(j["s"], "world");

    ASSERT_TRUE(cfg.erase("str_val"));
    ASSERT_FALSE(cfg.has("str_val"));

    cfg.unlock();
}

// What: Verifies that calling `lock()` reloads data from disk.
// How: It initializes a config, locks it, saves data, and unlocks. It then
//      modifies the file externally. After calling `lock()` again, it asserts
//      that the in-memory data now matches the external changes.
TEST_F(JsonConfigTest, ReloadOnLock)
{
    auto cfg_path = g_temp_dir / "reload_on_lock.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Lock, modify, save, unlock
    ASSERT_TRUE(cfg.lock());
    cfg.set("value", 1);
    ASSERT_TRUE(cfg.save());
    cfg.unlock();

    // Modify the file externally.
    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // Lock again, which should trigger a reload.
    ASSERT_TRUE(cfg.lock());

    // Verify the cache is updated.
    int value = 0;
    ASSERT_TRUE(cfg.get("value", value));
    ASSERT_EQ(value, 2);
    std::string new_key;
    ASSERT_TRUE(cfg.get("new_key", new_key));
    ASSERT_EQ(new_key, "external");

    cfg.unlock();
}


// What: Tests the deadlock prevention mechanism for nested read calls.
// How: It attempts to call `get` from inside a `with_json_read` lambda and
//      asserts that the nested call fails, proving the recursion guard works.
TEST_F(JsonConfigTest, RecursionGuardForReads)
{
    auto cfg_path = g_temp_dir / "recursion_reads.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Test nested read-in-read attempt.
    bool read_ok = cfg.with_json_read(
        [&]([[maybe_unused]] const json &data) {
            int val;
            ASSERT_FALSE(cfg.get("key", val)); // This should fail due to recursion.
        });
    ASSERT_TRUE(read_ok);
}

TEST_F(JsonConfigTest, MultiThreadContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";

    // Pre-populate with initial data using a dedicated instance.
    {
        JsonConfig setup_cfg;
        ASSERT_TRUE(setup_cfg.init(cfg_path, true));
        ASSERT_TRUE(setup_cfg.lock());
        setup_cfg.set("counter", 0);
        setup_cfg.set("write_log", json::array());
        ASSERT_TRUE(setup_cfg.save());
        setup_cfg.unlock();
    }

    const int THREADS = 16;
    const int ITERS = 100;
    std::vector<std::thread> threads;
    std::atomic<int> save_failures = 0;
    std::atomic<int> read_failures = 0;
    std::atomic<int> successful_writes = 0;

    for (int i = 0; i < THREADS; ++i)
    {
        // Each thread gets its own JsonConfig instance, but they all point to the
        // same file path. The underlying FileLock will correctly arbitrate access
        // between these separate objects. This is the intended use pattern.
        //
        // ANTI-PATTERN: Do NOT share a single JsonConfig object across threads that
        // perform lock/unlock cycles. This can lead to a race condition where
        // Thread A holds a lock, but Thread B (on the same object) calls unlock()
        // before Thread A is finished, causing Thread A's subsequent `save()` to fail.
        threads.emplace_back(
            [=, &save_failures, &read_failures, &successful_writes]
            {
                JsonConfig cfg(cfg_path);
                std::srand(static_cast<unsigned int>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) + i));
                int last_read_value = -1;

                for (int j = 0; j < ITERS; ++j)
                {
                    // 1 in 4 chance to be a writer
                    if (std::rand() % 4 == 0)
                    {
                        if (cfg.lock_for(100ms))
                        {
                            std::string my_id = fmt::format("T{}-{}", i, j);
                            int val = -1;
                            // NOTE: We MUST use get() here, not get_or(), because we need to
                            // know if the read failed. A failed read after a lock indicates
                            // a serious problem.
                            if (!cfg.get("counter", val))
                            {
                                save_failures++;
                                cfg.unlock();
                                continue;
                            }

                            cfg.set("counter", val + 1);
                            cfg.update("write_log",
                                       [&](json &log) { log.push_back(my_id); });

                            if (cfg.save())
                            {
                                successful_writes++;
                            }
                            else
                            {
                                save_failures++;
                            }
                            cfg.unlock();
                        }
                        // If lock fails, it's just contention, not an error.
                    }
                    else
                    {
                        // Reader: uses `with_json_read` for a consistent snapshot.
                        bool read_ok = cfg.with_json_read(
                            [&](const json &data)
                            {
                                int current_value = data.value("counter", -1);
                                if (current_value < last_read_value)
                                {
                                    read_failures++;
                                }
                                last_read_value = current_value;
                            });

                        if (!read_ok)
                        {
                            // A read should not fail unless there's a deadlock (which the
                            // recursion guard prevents) or some other unexpected issue.
                            read_failures++;
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(std::rand() % 200));
                }
            });
    }

    for (auto &t : threads) t.join();

    ASSERT_EQ(save_failures.load(), 0);
    ASSERT_EQ(read_failures.load(), 0);

    // Final verification using a separate instance.
    JsonConfig verifier_cfg(cfg_path);
    ASSERT_TRUE(verifier_cfg.lock());
    int final_counter = verifier_cfg.get_or<int>("counter", -1);
    json final_log = verifier_cfg.get_or<json>("write_log", json::array());

    EXPECT_EQ(final_counter, successful_writes.load());
    EXPECT_EQ(final_log.size(), successful_writes.load());

    // Also sanity-check that some writes actually happened.
    ASSERT_GT(successful_writes.load(), 0);

    verifier_cfg.unlock();
}


// What: Solves the "lost update" problem with process-safe locking.
// How: Multiple child processes contend to write to the same config file.
//      Each worker now uses the `lock/read-modify-write/save/unlock` pattern.
//      The test asserts that all workers complete and that the final state
//      of the file reflects the accumulated changes from all processes,
//      proving that no updates were lost.
TEST_F(JsonConfigTest, MultiProcessContention)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    // Create the file with an empty JSON object before starting workers.
    JsonConfig creator;
    ASSERT_TRUE(creator.init(cfg_path, true));

    const int PROCS = 16;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("win-{}", i)});
        ASSERT_TRUE(h != nullptr);
        procs.push_back(h);
    }
    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 0) success_count++;
        CloseHandle(h);
    }
#else
    std::vector<ProcessHandle> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("posix-{}", i)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }
    for (auto pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success_count++;
    }
#endif

    ASSERT_EQ(success_count, PROCS);

    JsonConfig verifier(cfg_path);
    ASSERT_TRUE(verifier.lock());

    for (int i = 0; i < PROCS; ++i)
    {
#if defined(PLATFORM_WIN64)
        ASSERT_TRUE(verifier.has(fmt::format("win-{}", i)))
            << "Worker win-" << i << " failed to write.";
#else
        ASSERT_TRUE(verifier.has(fmt::format("posix-{}", i)))
            << "Worker posix-" << i << " failed to write.";
#endif
    }

        int total_attempts = verifier.get_or<int>("total_attempts", -1);
        ASSERT_GE(total_attempts, PROCS)
            << "Expected the total number of attempts to be at least the number of processes.";
        
        verifier.unlock();
    }

#if PYLABHUB_IS_POSIX
// What: Verifies that the atomic save mechanism is not vulnerable to a
//       symlink attack on POSIX systems.
// How: It creates a "sensitive" real file and makes the config path a symlink
//      to it. It then locks and modifies the config. When `save()` is called,
//      it asserts that the save fails and the original file is NOT overwritten.
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    // Create a "sensitive" file with valid JSON.
    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    fs::create_symlink(real_file, symlink_path);
    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(symlink_path, false));

    ASSERT_TRUE(cfg.lock()); // This loads from the symlinked file.
    std::string original;
    ASSERT_TRUE(cfg.get("original", original));
    ASSERT_EQ(original, "data");

    // Attempt to save, which should be blocked by atomic_write_json.
    cfg.set("malicious", "data");
    ASSERT_FALSE(cfg.save());

    cfg.unlock();

    // Verify the original file was not touched.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

#if defined(PLATFORM_WIN64)
// What: Verifies that the atomic save mechanism is not vulnerable to a
//       symlink attack on Windows.
// How: The same logic as the POSIX test is applied, but using Windows-specific
//      APIs. Skips if user does not have symlink creation privileges.
TEST_F(JsonConfigTest, SymlinkAttackPreventionWindows)
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config_win.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Creating symlinks on Windows can require special privileges.
    if (!CreateSymbolicLinkW(symlink_path.c_str(), real_file.c_str(), 0))
    {
        GTEST_SKIP() << "Skipping Windows symlink test: Requires SeCreateSymbolicLinkPrivilege "
                        "or Developer Mode.";
    }

    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(symlink_path, false));

    ASSERT_TRUE(cfg.lock()); // Loads from symlink
    std::string original;
    ASSERT_TRUE(cfg.get("original", original));
    ASSERT_EQ(original, "data");

    cfg.set("malicious", "data");
    ASSERT_FALSE(cfg.save()); // Should fail

    cfg.unlock();

    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

// End of file