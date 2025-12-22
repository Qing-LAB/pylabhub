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
        pylabhub::utils::Initialize();
    }

    static void TearDownTestSuite() {
        pylabhub::utils::Finalize();
        try { fs::remove_all(g_temp_dir); } catch (...) {}
    }
};

} // anonymous namespace

// What: Verifies the basic initialization of a JsonConfig object.
// How: It calls `init()` with `create_if_not_found=true` on a non-existent file
//      and asserts that the file is created and contains an empty JSON object.
//      It then confirms that initializing a second object from the created
//      file succeeds.
TEST_F(JsonConfigTest, InitAndCreate)
{
    auto cfg_path = g_temp_dir / "init_create.json";
    fs::remove(cfg_path);

    JsonConfig config;
    ASSERT_FALSE(fs::exists(cfg_path));

    ASSERT_TRUE(config.init(cfg_path, true));
    ASSERT_TRUE(fs::exists(cfg_path));
    ASSERT_TRUE(config.as_json().is_object());
    ASSERT_TRUE(config.as_json().empty());

    JsonConfig config2;
    ASSERT_TRUE(config2.init(cfg_path, false));
    ASSERT_TRUE(config2.as_json().is_object());
}

// What: Verifies that all API calls on a default-constructed (uninitialized)
//       JsonConfig object are safe no-ops that return `false`.
// How: It calls all major API functions (`set`, `get`, `erase`, `save`, etc.) on
//      an uninitialized object and asserts that each call returns `false` or a
//      default value, ensuring no crashes or undefined behavior.
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    JsonConfig config;

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
// How: It initializes a config and uses `set`, `get`, `get_or`, `has`, `erase`,
//      and `update` to manipulate values (int, string, object). It asserts that
//      the data is correctly written to and read from the in-memory JSON object.
TEST_F(JsonConfigTest, BasicAccessors)
{
    auto cfg_path = g_temp_dir / "accessors.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    ASSERT_TRUE(cfg.set("int_val", 42));
    int int_val = 0;
    ASSERT_TRUE(cfg.get("int_val", int_val));
    ASSERT_EQ(int_val, 42);
    ASSERT_EQ(cfg.get_or<int>("int_val", 0), 42);
    ASSERT_EQ(cfg.get_or<int>("nonexistent", 99), 99);

    ASSERT_TRUE(cfg.has("int_val"));
    ASSERT_FALSE(cfg.has("nonexistent"));

    ASSERT_TRUE(cfg.set("str_val", "hello"));
    std::string str_val;
    ASSERT_TRUE(cfg.get("str_val", str_val));
    ASSERT_EQ(str_val, "hello");

    ASSERT_TRUE(cfg.erase("str_val"));
    ASSERT_FALSE(cfg.has("str_val"));

    ASSERT_TRUE(cfg.update("obj",
                     [](json &j)
                     {
                         j["x"] = 100;
                         j["s"] = "world";
                     }));
    json j;
    ASSERT_TRUE(cfg.get("obj", j));
    ASSERT_EQ(j["x"], 100);
    ASSERT_EQ(j["s"], "world");
}

// What: Verifies that the `reload()` function correctly updates the in-memory
//       cache from the underlying file.
// How: It initializes a config and saves it. It then modifies the file on disk
//      externally. It asserts that the in-memory data is initially unchanged.
//      After calling `reload()`, it asserts that the in-memory data now matches
//      the externally-modified file content.
TEST_F(JsonConfigTest, Reload)
{
    auto cfg_path = g_temp_dir / "reload.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    cfg.set("value", 1);
    ASSERT_TRUE(cfg.save());

    // Modify the file externally.
    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // Check that the in-memory cache is still the old value.
    int value = 0;
    ASSERT_TRUE(cfg.get("value", value));
    ASSERT_EQ(value, 1);
    ASSERT_FALSE(cfg.has("new_key"));

    // Reload and verify the cache is updated.
    ASSERT_TRUE(cfg.reload());
    ASSERT_TRUE(cfg.get("value", value));
    ASSERT_EQ(value, 2);
    std::string new_key;
    ASSERT_TRUE(cfg.get("new_key", new_key));
    ASSERT_EQ(new_key, "external");
}

// What: Tests the deadlock prevention mechanism provided by RecursionGuard.
// How: It attempts to call `get` from inside a `with_json_read` lambda and `set`
//      from inside a `with_json_write` lambda. In both cases, it asserts that
//      the nested call fails (returns false), proving that the recursion guard
//      is correctly preventing deadlocks.
TEST_F(JsonConfigTest, RecursionGuard)
{
    auto cfg_path = g_temp_dir / "recursion.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Test nested read-in-read attempt.
    bool read_ok = cfg.with_json_read(
        [&]([[maybe_unused]] const json &data)
        {
            int val;
            ASSERT_FALSE(cfg.get("key", val)); // This should fail due to recursion.
        });
    ASSERT_TRUE(read_ok);

    // Test nested write-in-write attempt.
    bool write_ok = cfg.with_json_write(
        [&](json &data)
        {
            data["a"] = 1;
            bool nested_set_ok = cfg.set("b", 2); // This should fail.
            ASSERT_FALSE(nested_set_ok);
        });
    ASSERT_TRUE(write_ok);
    int a_val = 0;
    ASSERT_TRUE(cfg.get("a", a_val));
    ASSERT_EQ(a_val, 1);
    ASSERT_FALSE(cfg.has("b")); // Verify the nested write did not occur.
}

// What: A stress test to verify the thread-safety of the JsonConfig object.
// How: A large number of threads are spawned to concurrently call `set` and `get`
//      on the same JsonConfig instance. This tests the internal `shared_mutex`
//      that allows for concurrent reads and exclusive writes. The test passes
//      if it completes without crashing or deadlocking.
TEST_F(JsonConfigTest, MultiThreadContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    const int THREADS = 32;
    const int ITERS = 1000;
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&, i]
            {
                std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) + i));
                for (int j = 0; j < ITERS; ++j)
                {
                    if (j % 10 == 0) {
                        cfg.set(fmt::format("t{}_j{}", i, j), true); // Write lock
                    } else {
                        (void)cfg.get_or<int>("nonexistent", 0); // Read lock
                    }
                }
            });
    }

    for (auto &t : threads) t.join();

    ASSERT_TRUE(cfg.save());

    JsonConfig verifier;
    ASSERT_TRUE(verifier.init(cfg_path, false));
    ASSERT_TRUE(verifier.has("t0_j0"));
}

// What: A stress test to verify the process-safety of the JsonConfig object.
// How: A config file is created. A large number of child processes are then
//      spawned. Each child process attempts to acquire a file lock and write its
//      unique ID to the file. The test verifies that all children complete
//      successfully and that the final file contains a valid state, proving that
//      the cross-process locking and atomic write-and-rename mechanism works.
TEST_F(JsonConfigTest, MultiProcessContention)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    {
        JsonConfig creator;
        ASSERT_TRUE(creator.init(cfg_path, true));
        ASSERT_TRUE(creator.save());
    }

    const int PROCS = 16;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(g_self_exe_path, "jsonconfig.write_id", {cfg_path.string(), fmt::format("win-{}", i)});
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
    std::vector<pid_t> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(g_self_exe_path, "jsonconfig.write_id", {cfg_path.string(), fmt::format("posix-{}", i)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }
    for (pid_t pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success_count++;
    }
#endif

    ASSERT_GT(success_count, 0);
    ASSERT_EQ(success_count, PROCS);
    JsonConfig verifier;
    ASSERT_TRUE(verifier.init(cfg_path, false));
    ASSERT_TRUE(verifier.has("worker")); // Check that some worker successfully wrote.
}

#if PYLABHUB_IS_POSIX
// What: Verifies that the atomic save mechanism is not vulnerable to a
//       symlink attack on POSIX systems.
// How: It creates a "sensitive" real file and makes the config path a symlink
//      to it. It then initializes the JsonConfig from the symlink and modifies it.
//      When `save()` is called, it asserts that the save fails and that the
//      original, sensitive file was NOT overwritten, proving the protection works.
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
    std::string original;
    ASSERT_TRUE(cfg.get("original", original));
    ASSERT_EQ(original, "data");

    // Attempt to save, which should follow the symlink and be blocked.
    cfg.set("malicious", "data");
    ASSERT_FALSE(cfg.save());

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
//      APIs to create the symbolic link. It skips the test if the user does
//      not have the required privileges to create symlinks.
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
        GTEST_SKIP() << "Skipping Windows symlink test: Requires SeCreateSymbolicLinkPrivilege or Developer Mode.";
    }

    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(symlink_path, false));
    std::string original;
    ASSERT_TRUE(cfg.get("original", original));
    ASSERT_EQ(original, "data");

    cfg.set("malicious", "data");
    ASSERT_FALSE(cfg.save());

    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

// End of file
