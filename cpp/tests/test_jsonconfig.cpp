// test_jsonconfig.cpp
// Unit test for JsonConfig: function tests, multithread and multiprocess tests.
//
// Usage:
//  ./test_jsonconfig          <- run all tests (master)
//  ./test_jsonconfig worker <path> <id> <- worker mode used by multiprocess test
//
// Exit code 0 = success. Non-zero indicates failure in one of the test cases.

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

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"

using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// --- Test Harness ---
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\n", #condition, __FILE__, __LINE__);   \
            throw std::runtime_error("Test case failed");                                          \
        }                                                                                          \
    } while (0)

void TEST_CASE(const std::string &name, std::function<void()> test_func)
{
    fmt::print("\n=== {} ===\n", name);
    try
    {
        test_func();
        tests_passed++;
        fmt::print("  --- PASSED ---\n");
    }
    catch (const std::exception &e)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: {} ---\n", e.what());
    }
    catch (...)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED with unknown exception ---\n");
    }
}

// --- Test Globals & Helpers ---
static fs::path g_temp_dir;

// Helper to read a file's content
static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open())
        return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// --- Worker Process Logic ---

#if defined(PLATFORM_WIN64)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &cfgpath,
                                   const std::string &worker_id)
{
    std::string cmdline = fmt::format("\"{}\" worker \"{}\" \"{}\"", exe, cfgpath, worker_id);
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    int wide = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0);
    std::wstring wcmd(wide, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, &wcmd[0], wide);

    if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
static pid_t spawn_worker_process(const std::string &exe, const std::string &cfgpath,
                                  const std::string &worker_id)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child
        execl(exe.c_str(), exe.c_str(), "worker", cfgpath.c_str(), worker_id.c_str(), nullptr);
        _exit(127); // exec failed
    }
    return pid;
}
#endif

// Worker mode: each worker will attempt to write a unique marker into the config.
// Returns 0 on success.
static int worker_main(const std::string &cfgpath, const std::string &worker_id)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    JsonConfig cfg;
    if (!cfg.init(cfgpath, false))
        return 1;

    // Attempt to write our ID and save. This will use a non-blocking FileLock internally.
    // Only one process should succeed.
    bool ok = cfg.with_json_write([&](json &j) { j["worker"] = worker_id; });

    return ok ? 0 : 2;
}

// --- Test Cases ---

void test_init_and_create()
{
    auto cfg_path = g_temp_dir / "init_create.json";
    fs::remove(cfg_path);

    JsonConfig config;
    CHECK(!fs::exists(cfg_path));

    // Init with createIfMissing = true should create the file.
    CHECK(config.init(cfg_path, true));
    CHECK(fs::exists(cfg_path));
    CHECK(config.as_json().is_object());
    CHECK(config.as_json().empty());

    // Init again on existing file should succeed.
    JsonConfig config2;
    CHECK(config2.init(cfg_path, false));
    CHECK(config2.as_json().is_object());
}

void test_uninitialized_behavior()
{
    JsonConfig config; // Default-constructed, not initialized with a path.

    // All modification attempts should fail gracefully.
    CHECK(!config.set("key", "value"));
    CHECK(!config.erase("key"));
    CHECK(!config.update("key", [](json &j) { j = 1; }));
    CHECK(!config.save());
    CHECK(!config.replace(json::object()));
    CHECK(!config.with_json_write([](json &j) { j["a"] = 1; }));

    // Read operations should be safe but indicate no data.
    CHECK(!config.has("key"));
    CHECK(config.get_or<int>("key", 42) == 42);
    CHECK(config.as_json().is_object());
    CHECK(config.as_json().empty());
}

void test_basic_accessors()
{
    auto cfg_path = g_temp_dir / "accessors.json";
    JsonConfig cfg;
    cfg.init(cfg_path, true);

    CHECK(cfg.set("int_val", 42));
    CHECK(cfg.get<int>("int_val") == 42);
    CHECK(cfg.get_or<int>("int_val", 0) == 42);
    CHECK(cfg.get_or<int>("nonexistent", 99) == 99);

    CHECK(cfg.has("int_val"));
    CHECK(!cfg.has("nonexistent"));

    CHECK(cfg.set("str_val", "hello"));
    CHECK(cfg.get<std::string>("str_val") == "hello");

    CHECK(cfg.erase("str_val"));
    CHECK(!cfg.has("str_val"));

    CHECK(cfg.update("obj",
                     [](json &j)
                     {
                         j["x"] = 100;
                         j["s"] = "world";
                     }));
    auto j = cfg.get<json>("obj");
    CHECK(j["x"] == 100);
    CHECK(j["s"] == "world");
}

void test_reload()
{
    auto cfg_path = g_temp_dir / "reload.json";
    JsonConfig cfg;
    cfg.init(cfg_path, true);

    cfg.set("value", 1);
    CHECK(cfg.save());

    // Modify the file externally.
    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // The in-memory value should still be the old one.
    CHECK(cfg.get<int>("value") == 1);
    CHECK(!cfg.has("new_key"));

    // Reload should update the in-memory state.
    CHECK(cfg.reload());
    CHECK(cfg.get<int>("value") == 2);
    CHECK(cfg.get<std::string>("new_key") == "external");
}

void test_recursion_guard()
{
    auto cfg_path = g_temp_dir / "recursion.json";
    JsonConfig cfg;
    cfg.init(cfg_path, true);
    cfg.set("key", 123);
    cfg.save();

    // Test nested call from with_json_read -> get()
    bool read_ok = cfg.with_json_read(
        [&]([[maybe_unused]] const json &data)
        {
            // This nested call should be rejected by the recursion guard.
            // get() throws on failure, so we expect an exception.
            bool caught = false;
            try
            {
                (void)cfg.get<int>("key");
            }
            catch (const std::runtime_error &)
            {
                caught = true;
            }
            CHECK(caught);
        });
    // The outer call should still succeed.
    CHECK(read_ok);

    // Test nested call from with_json_write -> set()
    bool write_ok = cfg.with_json_write(
        [&](json &data)
        {
            data["a"] = 1;
            // This nested call should be rejected and return false.
            bool nested_set_ok = cfg.set("b", 2);
            CHECK(!nested_set_ok);
        });
    // The outer call should succeed and save the change from `data["a"] = 1;`
    CHECK(write_ok);
    CHECK(cfg.get<int>("a") == 1);
    CHECK(!cfg.has("b")); // The nested set should not have taken effect.
}

void test_move_semantics()
{
    auto cfg_path = g_temp_dir / "move.json";
    fs::remove(cfg_path);

    // Test move construction
    {
        JsonConfig cfg1;
        cfg1.init(cfg_path, true);
        cfg1.set("val", 1);
        CHECK(cfg1.has("val"));

        JsonConfig cfg2(std::move(cfg1));
        CHECK(cfg2.has("val"));
        CHECK(cfg2.get<int>("val") == 1);
        // cfg1 is now in a moved-from state, operations should fail gracefully.
        CHECK(!cfg1.has("val"));
        CHECK(!cfg1.save());
    } // cfg2 is destroyed, its Pimpl is valid. cfg1's destructor is a no-op.

    // Test move assignment
    {
        JsonConfig cfg3;
        cfg3.init(cfg_path, true);
        cfg3.set("val", 3);

        JsonConfig cfg4; // Default constructed
        cfg4 = std::move(cfg3);
        CHECK(cfg4.get<int>("val") == 3);
        CHECK(!cfg3.has("val"));
    }
}

void test_multithread_contention()
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    JsonConfig cfg;
    cfg.init(cfg_path, true);

    const int THREADS = 8;
    const int ITERS = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&, i]
            {
                for (int j = 0; j < ITERS; ++j)
                {
                    // Mix of reads and writes to stress the locks.
                    if (j % 10 == 0)
                    {
                        cfg.set(fmt::format("t{}_j{}", i, j), true);
                    }
                    else
                    {
                        (void)cfg.get_or<int>("nonexistent", 0);
                    }
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    CHECK(cfg.save());

    // Verify some of the data was written.
    JsonConfig verifier;
    verifier.init(cfg_path, false);
    CHECK(verifier.has("t0_j0"));
    CHECK(verifier.has("t1_j90"));
}

void test_multiprocess_contention(const std::string &self_exe)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    // Create the initial file.
    {
        JsonConfig creator;
        creator.init(cfg_path, true);
        CHECK(creator.save());
    }

    const int PROCS = 8;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(self_exe, cfg_path.string(), fmt::format("win-{}", i));
        CHECK(h != nullptr);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 0)
        {
            success_count++;
        }
        CloseHandle(h);
    }
#else
    std::vector<pid_t> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(self_exe, cfg_path.string(), fmt::format("posix-{}", i));
        CHECK(pid > 0);
        pids.push_back(pid);
    }

    for (pid_t pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            success_count++;
        }
    }
#endif

    // In a non-blocking scenario, it's possible for more than one to succeed if they
    // don't overlap perfectly, but we expect at least one to have succeeded.
    CHECK(success_count > 0);

    // Verify that the file contains a "worker" key.
    JsonConfig verifier;
    verifier.init(cfg_path, false);
    CHECK(verifier.has("worker"));
}

#if PYLABHUB_IS_POSIX
void test_symlink_attack_prevention()
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    // Create a "sensitive" file with VALID JSON content.
    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Create a symlink pointing to it.
    fs::create_symlink(real_file, symlink_path);
    CHECK(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    // init() will now succeed because reload() can parse the JSON.
    CHECK(cfg.init(symlink_path, false));
    CHECK(cfg.get<std::string>("original") == "data");

    // Now, attempt to save. This should fail because atomic_write_json
    // detects the symlink path and throws an exception, which save()
    // catches and converts to a `false` return value.
    cfg.set("malicious", "data");
    CHECK(!cfg.save());

    // Verify the sensitive file was not overwritten with malicious data.
    // It should still contain the original valid JSON.
    json j = json::parse(read_file_contents(real_file));
    CHECK(j["original"] == "data");
    CHECK(j.find("malicious") == j.end());
}
#endif

int main(int argc, char **argv)
{
    // Worker mode: invoked by master to test multiprocess behavior
    if (argc >= 2 && std::strcmp(argv[1], "worker") == 0)
    {
        if (argc < 4)
        {
            fmt::print(stderr, "Worker mode requires a config path and worker ID.\n");
            return 2;
        }
        return worker_main(argv[2], argv[3]);
    }

    // Main test runner
    fmt::print("--- JsonConfig Test Suite ---\n");
    g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
    fs::create_directories(g_temp_dir);
    fmt::print("Using temporary directory: {}\n", g_temp_dir.string());

    TEST_CASE("Initialization and Creation", test_init_and_create);
    TEST_CASE("Uninitialized Object Behavior", test_uninitialized_behavior);
    TEST_CASE("Basic Accessors (get/set/has/erase/update)", test_basic_accessors);
    TEST_CASE("Reload from External Change", test_reload);
    TEST_CASE("Recursion Guard Deadlock Prevention", test_recursion_guard);
    TEST_CASE("Move Semantics", test_move_semantics);
    TEST_CASE("Multi-Threaded Contention", test_multithread_contention);

    std::string exe_path = argv[0];
    TEST_CASE("Multi-Process Contention", [&] { test_multiprocess_contention(exe_path); });

#if PYLABHUB_IS_POSIX
    TEST_CASE("Symlink Attack Prevention (POSIX-only)", test_symlink_attack_prevention);
#endif

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    // Final cleanup
    fs::remove_all(g_temp_dir);

    return tests_failed == 0 ? 0 : 1;
}
