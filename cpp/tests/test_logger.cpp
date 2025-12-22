// tests/test_logger_gtest.cpp
//
// GoogleTest conversions of the original tests/test_logger.cpp.
// All original tests have been ported to TEST_F methods on LoggerTest.

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

// Globals visible across test/main and multiproc child
extern std::string g_self_exe_path;           // set in test_main.cpp
fs::path g_multiproc_log_path;                // accessible to multiproc child

namespace {

// Helper utilities (same semantics as original)
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
        if (c == '\n') ++count;
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
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Count only "child-msg" lines (preserves original test semantics)
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

// Utility to let CI scale heavy tests down. Default uses original values.
// Set env PYLAB_TEST_SCALE=small to reduce multiprocess/multithread stress for CI.
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

} // namespace

// --- Test Fixture ---
class LoggerTest : public ::testing::Test {
protected:
    std::vector<fs::path> paths_to_clean_;

    void SetUp() override {
        pylabhub::utils::Initialize();
    }

    void TearDown() override {
        // Ensure logger is using console and file handles released before cleanup.
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
        // best-effort cleanup
        try { if (fs::exists(p)) fs::remove(p); } catch (...) {}
        return p;
    }
};

// --- Multiprocess child entry used by spawners ---
// Called by test_main.cpp when the process is invoked with --multiproc-child args.
void multiproc_child_main(int msg_count)
{
    pylabhub::utils::Initialize();
    Logger &L = Logger::instance();
    L.set_logfile(g_multiproc_log_path.string(), true);
    L.set_level(Logger::Level::L_TRACE);

#if defined(PLATFORM_WIN64)
    std::srand(static_cast<unsigned int>(GetCurrentProcessId() + std::chrono::system_clock::now().time_since_epoch().count()));
#else
    std::srand(static_cast<unsigned int>(getpid() + std::chrono::system_clock::now().time_since_epoch().count()));
#endif

    for (int i = 0; i < msg_count; ++i)
    {
        if (std::rand() % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
        }
#if defined(PLATFORM_WIN64)
        LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
        LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
#endif
    }
    L.flush();
    pylabhub::utils::Finalize();
}

// --- Spawn helpers for multiprocess test ---
#if defined(PLATFORM_WIN64)
static HANDLE spawn_multiproc_child(const std::string &exe, const fs::path& log_path, int count)
{
    std::string cmdline = fmt::format("\"{}\" --multiproc-child \"{}\" {}", exe, log_path.string(), count);
    std::wstring wcmd(cmdline.begin(), cmdline.end());
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    // CreateProcessW expects writable buffer; std::wstring & provides it.
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

// Run one multiprocess iteration (child spawning + verifying)
bool run_multiproc_iteration(const std::string& self_exe, const fs::path& log_path, int num_children, int msgs_per_child)
{
    fmt::print(stdout, "  Multiprocess iteration: {} children, {} msgs/child...\n", num_children, msgs_per_child);

    // ensure a clean slate
    try { fs::remove(log_path); } catch(...) {}

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < num_children; ++i)
    {
        HANDLE h = spawn_multiproc_child(self_exe, log_path, msgs_per_child);
        if (!h) { fmt::print(stderr, "Failed to spawn child (win)\n"); return false; }
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
        pid_t pid = spawn_multiproc_child(self_exe, log_path, msgs_per_child);
        if (pid == -1) { fmt::print(stderr, "Failed to spawn child (posix)\n"); return false; }
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
    if (!read_file_contents(log_path.string(), contents)) return false;

    size_t found = count_child_msgs(contents);
    size_t expected = static_cast<size_t>(num_children) * msgs_per_child;
    fmt::print("  [Stress: {} procs * {} msgs] Found: {} / Expected: {}\n",
               num_children, msgs_per_child, found, expected);

    return found == expected;
}

// ------------------ Tests ported ------------------

// test_basic_logging
TEST_F(LoggerTest, BasicLogging)
{
    auto log_path = GetUniqueLogPath("basic_logging");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);

    ASSERT_TRUE(wait_for_string_in_file(log_path, "Switched log to file"));
    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, 3u);

    EXPECT_NE(contents_after.find("unit-test: ascii message 42"), std::string::npos);
    EXPECT_NE(contents_after.find("unit-test: debug 3.14"), std::string::npos);
    EXPECT_NE(contents_after.find("☃"), std::string::npos);
    EXPECT_NE(contents_after.find("日本語"), std::string::npos);
}

// test_log_level_filtering
TEST_F(LoggerTest, LogLevelFiltering)
{
    auto log_path = GetUniqueLogPath("log_level_filtering");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Switched log to file"));

    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    L.set_level(Logger::Level::L_WARNING);
    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, 2u);

    EXPECT_EQ(contents_after.find("This should NOT be logged."), std::string::npos);
    EXPECT_NE(contents_after.find("This WARNING should be logged."), std::string::npos);
    EXPECT_NE(contents_after.find("This DEBUG should now be logged."), std::string::npos);
}

// test_bad_format_string
TEST_F(LoggerTest, BadFormatString)
{
    auto log_path = GetUniqueLogPath("bad_format_string");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Switched log to file"));

    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt); // Should cause internal "[FORMAT ERROR]" message
    ASSERT_TRUE(wait_for_string_in_file(log_path, "[FORMAT ERROR]"));
}

// test_default_sink_and_switching
TEST_F(LoggerTest, DefaultSinkAndSwitching)
{
    auto log_path = GetUniqueLogPath("default_sink_and_switching");

    Logger &L = Logger::instance();
    L.set_level(Logger::Level::L_INFO);

    // Log to default console (visual check); then switch to file sink.
    LOGGER_INFO("This message should go to the default console sink (stderr).");
    L.flush();

    L.set_logfile(log_path.string());
    LOGGER_INFO("This message should be logged to the file.");
    ASSERT_TRUE(wait_for_string_in_file(log_path, "This message should be logged to the file."));

    std::string contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents));
    EXPECT_EQ(contents.find("This message should go to the default console sink"), std::string::npos);
    EXPECT_NE(contents.find("Switched log to file"), std::string::npos);
}

// test_multithread_stress
TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_DEBUG);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Switched log to file"));

    const int LOG_THREADS = scaled_value(32, 8);
    const int MESSAGES_PER_THREAD = scaled_value(1000, 100);
    const int SINK_SWITCHES = scaled_value(100, 10);

    std::vector<std::thread> threads;
    threads.reserve(LOG_THREADS + 1);

    for (int t = 0; t < LOG_THREADS; ++t)
    {
        threads.emplace_back([t, MESSAGES_PER_THREAD]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                LOGGER_DEBUG("thread {} message {}", t, i);
                if (i % 100 == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    threads.emplace_back([&]() {
        for (int i = 0; i < SINK_SWITCHES; ++i)
        {
            if (i % 2 == 0)
                Logger::instance().set_logfile(log_path.string());
            else
                Logger::instance().set_console();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Logger::instance().set_logfile(log_path.string());
    });

    for (auto &t : threads) t.join();
    L.flush();

    std::string contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents));

    size_t found_threads = 0;
    for (int t = 0; t < LOG_THREADS; ++t)
    {
        if (contents.find(fmt::format("thread {} message", t)) != std::string::npos)
            found_threads++;
    }
    EXPECT_EQ(found_threads, static_cast<size_t>(LOG_THREADS));
    EXPECT_NE(contents.find("Switched log to Console"), std::string::npos);
    EXPECT_NE(contents.find("Switched log to file"), std::string::npos);
}

// test_flush_waits_for_queue
TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Switched log to file"));
    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    const int MESSAGES = 500;
    for (int i = 0; i < MESSAGES; ++i)
    {
        LOGGER_INFO("flush-test: msg={}", i);
    }

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, static_cast<size_t>(MESSAGES));
}

// test_shutdown_idempotency
TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("Message before shutdown.");
    L.flush();

    std::string content_before_shutdown;
    ASSERT_TRUE(read_file_contents(log_path.string(), content_before_shutdown));
    EXPECT_NE(content_before_shutdown.find("Message before shutdown"), std::string::npos);

    const int THREADS = 16;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([]() { Logger::instance().shutdown(); });
    }
    for (auto &t : threads) t.join();

    // This message should be silently dropped.
    LOGGER_INFO("This message should NOT be logged.");

    // flush should be no-op; wait a short while.
    L.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string content_after_shutdown;
    ASSERT_TRUE(read_file_contents(log_path.string(), content_after_shutdown));

    EXPECT_EQ(content_after_shutdown.find("This message should NOT be logged."), std::string::npos);
    EXPECT_EQ(content_before_shutdown, content_after_shutdown);
}

// test_reentrant_error_callback
TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto initial_log_path = GetUniqueLogPath("reentrant_initial");

    Logger &L = Logger::instance();
    L.set_logfile(initial_log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(initial_log_path, "Switched log to file"));

    std::atomic<bool> callback_invoked(false);
    L.set_write_error_callback([&](const std::string &msg) {
        // Re-entrant log from callback should not deadlock and should write to previous sink.
        LOGGER_SYSTEM("Re-entrant log from error callback: {}", msg);
        callback_invoked = true;
    });

    // Produce a locked/unwritable target path depending on platform.
#if defined(PLATFORM_WIN64)
    fs::path locked_log_path = fs::temp_directory_path() / "pylab_reentrant_locked.log";
    std::remove(locked_log_path.string().c_str());
    HANDLE h = CreateFileA(locked_log_path.string().c_str(), GENERIC_READ, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
#else
    fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test_reentrant";
    fs::remove_all(readonly_dir);
    fs::create_directory(readonly_dir);
    chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // 0500
    fs::path locked_log_path = readonly_dir / "test.log";
#endif

    L.set_logfile(locked_log_path.string());
    LOGGER_INFO("This message will be dropped and should trigger an error.");
    L.flush();

    ASSERT_TRUE(wait_for_string_in_file(initial_log_path, "Re-entrant log from error callback"));
    EXPECT_TRUE(callback_invoked.load());

    // cleanup
#if defined(PLATFORM_WIN64)
    CloseHandle(h);
    fs::remove(locked_log_path);
#else
    chmod(readonly_dir.string().c_str(), S_IRWXU);
    fs::remove_all(readonly_dir);
#endif
}

// test_write_error_callback_async
TEST_F(LoggerTest, WriteErrorCallbackAsync)
{
#if defined(PLATFORM_WIN64)
    fs::path test_log_path = fs::temp_directory_path() / "pylab_write_err_async.log";
    fs::remove(test_log_path);

    HANDLE h = CreateFileA(test_log_path.string().c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);

    Logger &L = Logger::instance();
    std::atomic<bool> callback_invoked(false);
    L.set_write_error_callback([&](const std::string &msg) {
        callback_invoked = true;
        EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
    });

    L.set_logfile(test_log_path.string());
    LOGGER_INFO("This write should be dropped as sink creation failed.");
    L.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_TRUE(callback_invoked.load());
    CloseHandle(h);
    fs::remove(test_log_path);
#else
    // POSIX Test 1: read-only directory
    {
        fmt::print("  - Testing sink failure: read-only directory (POSIX)\n");
        fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test";
        fs::remove_all(readonly_dir);
        fs::create_directory(readonly_dir);
        chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // 0500
        fs::path locked_log_path = readonly_dir / "test.log";

        Logger &L = Logger::instance();
        std::atomic<bool> callback_invoked(false);
        L.set_write_error_callback([&](const std::string &msg) {
            callback_invoked = true;
            EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
        });

        L.set_logfile(locked_log_path.string());
        LOGGER_INFO("This message will be dropped.");
        L.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        EXPECT_TRUE(callback_invoked.load());

        chmod(readonly_dir.string().c_str(), S_IRWXU);
        fs::remove_all(readonly_dir);
    }

    // POSIX Test 2: read-only file
    {
        fmt::print("  - Testing sink failure: read-only file (POSIX)\n");
        fs::path readonly_file = fs::temp_directory_path() / "pylab_readonly_file.log";
        {
            std::ofstream touch(readonly_file);
        }
        chmod(readonly_file.string().c_str(), S_IRUSR); // 0400

        Logger &L = Logger::instance();
        std::atomic<bool> callback_invoked(false);
        L.set_write_error_callback([&](const std::string &msg) {
            callback_invoked = true;
            EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
        });

        L.set_logfile(readonly_file.string());
        LOGGER_INFO("This message will also be dropped.");
        L.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        EXPECT_TRUE(callback_invoked.load());

        chmod(readonly_file.string().c_str(), S_IRWXU);
        fs::remove(readonly_file);
    }
#endif
}

// test_platform_sinks (manual verification) -> make DISABLED by default for CI
TEST_F(LoggerTest, DISABLED_PlatformSinks)
{
    Logger &L = Logger::instance();
#if defined(_WIN32)
    L.set_eventlog(L"PyLabHubTestLogger");
    LOGGER_INFO("Testing Windows Event Log sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Windows Event Log:\n");
    fmt::print(stderr, "  1. Open Event Viewer (eventvwr.msc).\n");
    fmt::print(stderr, "  2. Look for an Information-level message from 'PyLabHubTestLogger'.\n\n");
#else
    L.set_syslog("pylab-logger-test");
    LOGGER_INFO("Testing syslog sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Syslog:\n");
    fmt::print(stderr, "  1. Run 'journalctl -r | grep \"pylab-logger-test\"' or\n");
    fmt::print(stderr, "     'cat /var/log/syslog | grep \"pylab-logger-test\"'.\n\n");
#endif
    SUCCEED();
}

// test_multiprocess_logging (ramp-up)
TEST_F(LoggerTest, MultiprocessLogging)
{
    // Original had start 10 -> 50 by 10, msgs=1000. Keep default but allow scaling for CI.
    const int start_children = 10;
    const int max_children = 50;
    const int step_children = 10;
    const int msgs = scaled_value(1000, 200);

    fmt::print("Starting high-stress multiprocess ramp-up (msgs/child={})...\n", msgs);

    auto log_path = GetUniqueLogPath("multiprocess_high_stress");
    // ensure global path for child code
    g_multiproc_log_path = log_path;

    for (int n = start_children; n <= max_children; n += step_children)
    {
        ASSERT_TRUE(run_multiproc_iteration(g_self_exe_path, log_path, n, msgs))
            << "Multiprocess logging FAILED at " << n << " children.";
    }
}

// test_concurrent_lifecycle_chaos
TEST_F(LoggerTest, ConcurrentLifecycleChaos)
{
    auto chaos_log_path = GetUniqueLogPath("lifecycle_chaos");
    std::atomic<bool> stop_flag(false);
    const int DURATION_MS = 2000;

    auto worker = [&](int id, auto fn) {
        while (!stop_flag.load(std::memory_order_relaxed)) fn(id);
    };

    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(worker, i, [](int id) {
            LOGGER_INFO("chaos-log-{}: message", id);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        });
    }

    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(worker, i, [](int) {
            Logger::instance().flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }

    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(worker, i, [&](int id) {
            if (id % 2 == 0) Logger::instance().set_console();
            else Logger::instance().set_logfile(chaos_log_path.string());
            Logger::instance().set_write_error_callback(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    Logger::instance().shutdown();
    stop_flag.store(true);

    for (auto &t : threads) t.join();
    SUCCEED();
}


