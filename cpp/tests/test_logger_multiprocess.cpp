#include "helpers/test_process_utils.h"
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
#include "helpers/test_entrypoint.h" // provides extern std::string g_self_exe_path

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
using namespace test_utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// TODO: Refactor the test fixture and helper utilities into a shared header
// to eliminate code duplication between this file and test_logger.cpp.

namespace {

// Helper utility to read the entire content of a file into a string.
static bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

// Counts only lines containing "child-msg".
static size_t count_child_msgs(const std::string &contents)
{
    size_t found = 0;
    std::stringstream ss(contents);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.find("child-msg") != std::string::npos) ++found;
    }
    return found;
}

// Helper to scale down test intensity for CI environments.
static std::string test_scale()
{
    const char *v = std::getenv("PYLAB_TEST_SCALE");
    return v ? std::string(v) : std::string();
}

static int scaled_value(int original, int small_value)
{
    if (test_scale() == "small") return small_value;
    return original;
}

// --- Test Fixture ---
// NOTE: This is duplicated from test_logger.cpp.
class LoggerTest : public ::testing::Test {
protected:
    std::vector<fs::path> paths_to_clean_;

    void SetUp() override {
        pylabhub::utils::Initialize();
    }

    void TearDown() override {
        Logger::instance().set_console();
        Logger::instance().flush();
        pylabhub::utils::Finalize();

        for (const auto& p : paths_to_clean_) {
            try {
                if (fs::exists(p)) fs::remove(p);
            } catch (const fs::filesystem_error& e) {
                fmt::print(stderr, "Warning: failed to clean up '{}': {}\n", p.string(), e.what());
            }
        }
    }

    fs::path GetUniqueLogPath(const std::string& test_name) {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        try { if (fs::exists(p)) fs::remove(p); } catch (...) {}
        return p;
    }
};

// Helper to run one full iteration of the multi-process stress test.
bool run_multiproc_iteration(const std::string& self_exe, const fs::path& log_path, int num_children, int msgs_per_child)
{
    fmt::print(stdout, "  Multiprocess iteration: {} children, {} msgs/child...\n", num_children, msgs_per_child);

    try { fs::remove(log_path); } catch(...) {}

#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < num_children; ++i)
    {
        ProcessHandle h = spawn_worker_process(self_exe, "logger.stress_log", {log_path.string(), std::to_string(msgs_per_child)});
        if (!h) { fmt::print(stderr, "Failed to spawn child (win)\n"); return false; }
        procs.push_back(h);
    }
    for (auto &h : procs)
    {
        WaitForSingleObject(h, 60000); // 60s timeout
        CloseHandle(h);
    }
#else
    std::vector<ProcessHandle> child_pids;
    for (int i = 0; i < num_children; ++i)
    {
        ProcessHandle pid = spawn_worker_process(self_exe, "logger.stress_log", {log_path.string(), std::to_string(msgs_per_child)});
        if (pid == -1) { fmt::print(stderr, "Failed to spawn child (posix)\n"); return false; }
        child_pids.push_back(pid);
    }
    for (auto pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    }
#endif

    std::string contents;
    if (!read_file_contents(log_path.string(), contents)) return false;

    size_t found = count_child_msgs(contents);
    size_t expected = static_cast<size_t>(num_children) * msgs_per_child;
    fmt::print("  [Stress: {} procs * {} msgs] Found: {} / Expected: {}\n",
               num_children, msgs_per_child, found, expected);

    return found == expected;
}

} // namespace

// What: A high-stress test to verify the logger's ability to handle concurrent
//       writes from many separate processes to a single log file without data loss
//       or corruption.
// How: The test runs in a "ramp-up" loop. In each iteration, it spawns an
//      increasing number of child processes. Each child logs a large number of
//      messages to a shared log file via the `logger.stress_log` worker. After
//      all children in an iteration complete, the parent process reads the log
//      file and asserts that the total number of messages logged is exactly equal
//      to the expected amount, proving no messages were dropped.
TEST_F(LoggerTest, MultiprocessLogging)
{
    const int start_children = 10;
    const int max_children = 50;
    const int step_children = 10;
    const int msgs = scaled_value(1000, 200);

    fmt::print("Starting high-stress multiprocess ramp-up (msgs/child={})...\n", msgs);
    auto log_path = GetUniqueLogPath("multiprocess_high_stress");

    for (int n = start_children; n <= max_children; n += step_children)
    {
        ASSERT_TRUE(run_multiproc_iteration(g_self_exe_path, log_path, n, msgs))
            << "Multiprocess logging FAILED at " << n << " children.";
    }
}
