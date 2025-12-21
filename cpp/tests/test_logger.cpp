// tests/test_logger.cpp
//
// Unit test for pylabhub::util::Logger, converted to GoogleTest.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "platform.hpp"

#include "test_main.h"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

fs::path g_multiproc_log_path;

namespace {

// --- Test Globals & Helpers (remain in anonymous namespace) ---

static bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

static size_t count_lines(const std::string &s)
{
    size_t count = 0;
    for (char c : s)
    {
        if (c == '\n')
        {
            count++;
        }
    }
    return count;
}

static bool wait_for_string_in_file(const fs::path &path, const std::string &expected,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(15))
{
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout)
    {
        std::string contents;
        if (read_file_contents(path.string(), contents))
        {
            if (contents.find(expected) != std::string::npos)
            {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// --- Test Fixture ---
class LoggerTest : public ::testing::Test {
protected:
    std::vector<fs::path> paths_to_clean_;

    void SetUp() override {
        // Initialization is handled by the fixture for all tests now.
        pylabhub::utils::Initialize();
    }

    void TearDown() override {
        // Centralized cleanup: ensures the logger file handle is released BEFORE
        // any files are removed. This fixes the "file in use" error on Windows.
        Logger::instance().set_console();
        Logger::instance().flush();
        pylabhub::utils::Finalize();

        // Clean up any log files created during the test.
        for (const auto& p : paths_to_clean_) {
            try {
                if (fs::exists(p)) {
                    fs::remove(p);
                }
            } catch (const fs::filesystem_error& e) {
                // Log error but don't fail the test on cleanup issues.
                fmt::print(stderr, "Warning: failed to clean up '{}': {}\\n", p.string(), e.what());
            }
        }
    }

    // Helper to get a unique path for a test and register it for cleanup.
    fs::path GetUniqueLogPath(const std::string& test_name) {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        fs::remove(p); // Clean up before test starts.
        return p;
    }
};

} // anonymous namespace (end of helpers and fixtures)

// --- Multi-Process Test Logic (MOVED OUT OF ANONYMOUS NAMESPACE) ---

void multiproc_child_main(int msg_count)
{
    pylabhub::utils::Initialize();
    Logger &L = Logger::instance();
    // g_multiproc_log_path is now a global accessible to this function.
    L.set_logfile(g_multiproc_log_path.string(), true);
    L.set_level(Logger::Level::L_TRACE);
    std::srand(static_cast<unsigned int>(getpid() + std::chrono::system_clock::now().time_since_epoch().count()));
    for (int i = 0; i < msg_count; ++i)
    {
        if (std::rand() % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
        }
        LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
    }
    L.flush();
    pylabhub::utils::Finalize();
}

#if defined(PLATFORM_WIN64)
static HANDLE spawn_multiproc_child(const std::string &exe, const fs::path& log_path, int count)
{
    std::string cmdline = fmt::format("\"{}\" --multiproc-child \"{}\" {}", exe, log_path.string(), count);
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::wstring wcmd(cmdline.begin(), cmdline.end());

    if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
static pid_t spawn_multiproc_child(const std::string &exe, const fs::path& log_path, int count)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        execl(exe.c_str(), exe.c_str(), "--multiproc-child",
              log_path.c_str(), std::to_string(count).c_str(), (char *)nullptr);
        _exit(127);
    }
    return pid;
}
#endif

bool run_multiproc_iteration(const std::string& self_exe, int num_children, int msgs_per_child)
{
    fmt::print(stdout, "  Multiprocess iteration: {} children, {} msgs/child...\n", num_children,
               msgs_per_child);
    
    g_multiproc_log_path = fs::temp_directory_path() / "pylabhub_multiprocess_test.log";
    fs::remove(g_multiproc_log_path);

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < num_children; ++i)
    {
        HANDLE h = spawn_multiproc_child(self_exe, g_multiproc_log_path, msgs_per_child);
        if (!h) return false;
        procs.push_back(h);
    }
    for (auto &h : procs)
    {
        WaitForSingleObject(h, 60000);
        CloseHandle(h);
    }
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < num_children; ++i)
    {
        pid_t pid = spawn_multiproc_child(self_exe, g_multiproc_log_path, msgs_per_child);
        if (pid == -1) return false;
        child_pids.push_back(pid);
    }
    for (pid_t pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    }
#endif

    std::string contents;
    if (!read_file_contents(g_multiproc_log_path.string(), contents)) return false;
    
    size_t found = count_lines(contents);
    size_t expected = static_cast<size_t>(num_children) * msgs_per_child;
    fmt::print("  [Stress: {} procs * {} msgs] Found: {} / Expected: {}\n", 
               num_children, msgs_per_child, found, expected);
               
    return found == expected;
}

TEST(LoggerMultiProcessTest, HighContention)
{
    const int start_children = 10;
    const int max_children = 30; // Reduced for CI speed
    const int step_children = 10;
    const int msgs = 500; // Reduced for CI speed

    fmt::print("Starting high-stress multiprocess ramp-up (msgs/child={})...\n", msgs);

    for (int n = start_children; n <= max_children; n += step_children)
    {
        ASSERT_TRUE(run_multiproc_iteration(g_self_exe_path, n, msgs)) 
            << "Multiprocess logging FAILED at " << n << " children.";
    }
}


int main(int argc, char **argv) {
    // Handle multi-process worker mode
    if (argc > 1 && std::string(argv[1]) == "--multiproc-child") {
        if (argc < 4) return 3;
        g_multiproc_log_path = argv[2];
        int count = std::stoi(argv[3]);
        multiproc_child_main(count);
        return 0;
    }

    // If not a worker, run all GTest tests
    g_self_exe_path = argv[0];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}