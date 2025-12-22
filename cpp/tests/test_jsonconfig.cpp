// tests/test_jsonconfig.cpp
//
// Unit test for JsonConfig, converted to GoogleTest and corrected to match
// original coverage (POSIX/Windows symlink tests, worker entrypoint name,
// fixed string literal and other issues).

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

#include "test_main.h" // provides extern std::string g_self_exe_path

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

// --- Worker Process Logic (helpers for spawning children) ---

#if defined(PLATFORM_WIN64)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &cfgpath, const std::string &worker_id)
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
static pid_t spawn_worker_process(const std::string &exe, const std::string &cfgpath, const std::string &worker_id)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        execl(exe.c_str(), exe.c_str(), "worker", cfgpath.c_str(), worker_id.c_str(), nullptr);
        _exit(127);
    }
    return pid;
}
#endif

// Test fixture to initialize/finalize lifecycle once per suite.
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

// Worker function with external linkage so test_main.cpp can call it in "worker" mode.
int jsonconfig_worker_main(const std::string &cfgpath, const std::string &worker_id)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    JsonConfig cfg;
    if (!cfg.init(cfgpath, false))
        return 1;
    bool ok = cfg.with_json_write([&](json &j) { j["worker"] = worker_id; });
    return ok ? 0 : 2;
}

// --- Tests ---

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

    int value = 0;
    ASSERT_TRUE(cfg.get("value", value));
    ASSERT_EQ(value, 1);
    ASSERT_FALSE(cfg.has("new_key"));

    ASSERT_TRUE(cfg.reload());
    ASSERT_TRUE(cfg.get("value", value));
    ASSERT_EQ(value, 2);
    std::string new_key;
    ASSERT_TRUE(cfg.get("new_key", new_key));
    ASSERT_EQ(new_key, "external");
}

TEST_F(JsonConfigTest, RecursionGuard)
{
    auto cfg_path = g_temp_dir / "recursion.json";
    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));
    cfg.set("key", 123);
    ASSERT_TRUE(cfg.save());

    bool read_ok = cfg.with_json_read(
        [&]([[maybe_unused]] const json &data)
        {
            int val;
            ASSERT_FALSE(cfg.get("key", val));
        });
    ASSERT_TRUE(read_ok);

    bool write_ok = cfg.with_json_write(
        [&](json &data)
        {
            data["a"] = 1;
            bool nested_set_ok = cfg.set("b", 2);
            ASSERT_FALSE(nested_set_ok);
        });
    ASSERT_TRUE(write_ok);
    int a_val = 0;
    ASSERT_TRUE(cfg.get("a", a_val));
    ASSERT_EQ(a_val, 1);
    ASSERT_FALSE(cfg.has("b"));
}

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
                std::srand(static_cast<unsigned int>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) + i));
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
                for (int j = 0; j < ITERS; ++j)
                {
                    if (j % 10 == 0)
                    {
                        cfg.set(fmt::format("t{}_j{}", i, j), true);
                    }
                    else
                    {
                        (void)cfg.get_or<int>("nonexistent", 0);
                    }
                    if (std::rand() % 50 == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 50));
                    }
                }
            });
    }

    for (auto &t : threads) t.join();

    ASSERT_TRUE(cfg.save());

    JsonConfig verifier;
    ASSERT_TRUE(verifier.init(cfg_path, false));
    ASSERT_TRUE(verifier.has("t0_j0"));
    ASSERT_TRUE(verifier.has("t1_j90"));
}

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
        HANDLE h = spawn_worker_process(g_self_exe_path, cfg_path.string(), fmt::format("win-{}", i));
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
        pid_t pid = spawn_worker_process(g_self_exe_path, cfg_path.string(), fmt::format("posix-{}", i));
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
    JsonConfig verifier;
    ASSERT_TRUE(verifier.init(cfg_path, false));
    ASSERT_TRUE(verifier.has("worker"));
}

#if PYLABHUB_IS_POSIX
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
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

#if defined(PLATFORM_WIN64)
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

    std::wstring symlink_w = pylabhub::utils::win32_to_long_path(symlink_path);
    std::wstring real_w = pylabhub::utils::win32_to_long_path(real_file);

    if (!CreateSymbolicLinkW(symlink_w.c_str(), real_w.c_str(), 0))
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
