// tests/test_filelock.cpp
//
// Unit test for pylabhub::utils::FileLock, converted to GoogleTest and
// expanded to fully match the original coverage (timed locks, parent-child worker, etc.).

#include <gtest/gtest.h>
#include "workers.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include <fmt/core.h>

#include "platform.hpp"
#include "utils/FileLock.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "test_main.h" // provides extern std::string g_self_exe_path

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helper spawn implementation (anonymous namespace - helper only)
// ---------------------------------------------------------------------------
namespace {

#if defined(PLATFORM_WIN64)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &mode,
                                   const std::vector<std::string> &args)
{
    // Build a quoted commandline: "<exe>" <mode> "arg1" "arg2" ...
    std::string cmdline = fmt::format("\"{}\" {}", exe, mode);
    for (const auto &arg : args)
    {
        cmdline += fmt::format(" \"{}\"", arg);
    }

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
static pid_t spawn_worker_process(const std::string &exe, const std::string &mode,
                                  const std::vector<std::string> &args)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process: execv expects char* const*
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(exe.c_str(), argv.data());
        _exit(127); // execv failed
    }
    return pid;
}
#endif

} // namespace (helpers)

// ---------------------------------------------------------------------------
// Worker entrypoints (non-anonymous so test_main can call these in worker mode)
// ---------------------------------------------------------------------------

// Worker mode: try to acquire a non-blocking lock. Return 0 on success, 1 on failure.
int worker_main_nonblocking_test(const std::string &resource_path_str)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);

    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
    if (!lock.valid())
    {
        // If error_code is present we can print it for debugging; keep quiet otherwise.
        if (lock.error_code())
        {
#if defined(PLATFORM_WIN64)
            fmt::print(stderr, "worker: failed to acquire lock: {} - {}\n",
                       lock.error_code().value(), lock.error_code().message());
#else
            std::string err_msg = fmt::format("worker: failed to acquire lock: {} - {}\n",
                                              lock.error_code().value(), lock.error_code().message());
            ::write(STDERR_FILENO, err_msg.c_str(), err_msg.length());
#endif
        }
        return 1;
    }

    // Hold lock for a while to let parent/spawned processes contend and fail.
    std::this_thread::sleep_for(3s);
    return 0;
}

// Worker mode: blocking contention incrementing a counter file.
// Returns 0 on success, 1 on lock failure.
int worker_main_blocking_contention(const std::string &counter_path_str, int num_iterations)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path counter_path(counter_path_str);

    // Seed with a mix of thread id and time to jitter.
    std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                         std::chrono::system_clock::now().time_since_epoch().count()));

    for (int i = 0; i < num_iterations; ++i)
    {
        if (std::rand() % 2 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
        }

        FileLock lock(counter_path, ResourceType::File, LockMode::Blocking);
        if (!lock.valid()) return 1;

        if (std::rand() % 10 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
        }

        // Read, increment, write back.
        int current_value = 0;
        {
            std::ifstream ifs(counter_path);
            if (ifs.is_open()) { ifs >> current_value; }
        }
        {
            std::ofstream ofs(counter_path);
            ofs << (current_value + 1);
        }
    }
    return 0;
}

// Worker for parent-child blocking test: measure duration waiting for the lock.
// Return 0 on success, 1 on lock error, 2 if it didn't block long enough.
int worker_main_parent_child(const std::string &resource_path_str)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);

    auto start = std::chrono::steady_clock::now();
    FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
    auto end = std::chrono::steady_clock::now();

    if (!lock.valid()) return 1;

    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    if (dur.count() < 100) return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class FileLockTest : public ::testing::Test {
protected:
    static fs::path g_temp_dir_;

    static void SetUpTestSuite() {
        g_temp_dir_ = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir_);
        fmt::print("Using temporary directory: {}\n", g_temp_dir_.string());
        pylabhub::utils::Initialize();
    }

    static void TearDownTestSuite() {
        pylabhub::utils::Finalize();
        // Best-effort cleanup
        try {
            fs::remove_all(g_temp_dir_);
        } catch (...) {
            // ignore
        }
    }

    // Short helper to get the fixture temp dir
    fs::path temp_dir() const { return g_temp_dir_; }
};

fs::path FileLockTest::g_temp_dir_;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = temp_dir() / "basic_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_FALSE(lock.error_code());

        // Second non-blocking lock in same process should fail.
        FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock2.valid());
    }

    // After scope, it should be re-lockable.
    FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_TRUE(lock3.valid());
}

TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = temp_dir() / "blocking_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    std::atomic<bool> thread_valid{false};
    std::atomic<bool> thread_saw_block{false};

    auto main_lock = std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
    ASSERT_TRUE(main_lock->valid());

    std::thread t1([&]() {
        auto start = std::chrono::steady_clock::now();
        FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
        auto end = std::chrono::steady_clock::now();

        if (thread_lock.valid()) thread_valid.store(true, std::memory_order_relaxed);
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (dur > 100ms) thread_saw_block.store(true, std::memory_order_relaxed);
    });

    // Give the thread time to attempt to lock (and block).
    std::this_thread::sleep_for(200ms);
    // Still holding main_lock means thread should be blocked.
    // Now release main_lock so thread can proceed.
    main_lock.reset();

    t1.join();

    ASSERT_TRUE(thread_valid.load());
    ASSERT_TRUE(thread_saw_block.load());
}

TEST_F(FileLockTest, TimedLock)
{
    auto resource_path = temp_dir() / "timed.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
        ASSERT_TRUE(main_lock.valid());

        auto start = std::chrono::steady_clock::now();
        FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
        auto end = std::chrono::steady_clock::now();

        ASSERT_FALSE(timed_lock_fail.valid());
        ASSERT_TRUE(timed_lock_fail.error_code());
        // Compare against std::errc::timed_out (value comparison for portability)
        ASSERT_EQ(timed_lock_fail.error_code().value(), static_cast<int>(std::errc::timed_out));

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        ASSERT_GE(duration.count(), 100);
        ASSERT_LT(duration.count(), 1000);
    }

    // After main lock released, timed lock should succeed.
    FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
    ASSERT_TRUE(timed_lock_succeed.valid());
    ASSERT_FALSE(timed_lock_succeed.error_code());
}

TEST_F(FileLockTest, MoveSemanticsFull)
{
    auto resource1 = temp_dir() / "move1.txt";
    auto resource2 = temp_dir() / "move2.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource1, ResourceType::File));
    fs::remove(FileLock::get_expected_lock_fullname_for(resource2, ResourceType::File));

    // Move constructor
    {
        FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1.valid());
        FileLock lock2(std::move(lock1));
        ASSERT_TRUE(lock2.valid());
        ASSERT_FALSE(lock1.valid());
    }

    // lock_res1 should be free now
    {
        FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1_again.valid());
    }

    // Move assignment to valid target: lock_B should release old resource and take new one.
    {
        FileLock lock_A(resource1, ResourceType::File, LockMode::NonBlocking);
        FileLock lock_B(resource2, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_A.valid());
        ASSERT_TRUE(lock_B.valid());

        lock_B = std::move(lock_A);
        ASSERT_TRUE(lock_B.valid());
        ASSERT_FALSE(lock_A.valid());

        // resource2 should now be free again
        FileLock lock_res2_again(resource2, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_res2_again.valid());
    }

    // Self-move assignment should keep the object valid.
    {
        FileLock lock_self(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_self.valid());

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
        lock_self = std::move(lock_self);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

        ASSERT_TRUE(lock_self.valid());
    }
}

TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = temp_dir() / "new_dir_for_lock";
    auto resource_to_lock = new_dir / "resource.txt";
    auto actual_lock_file =
        FileLock::get_expected_lock_fullname_for(resource_to_lock, ResourceType::File);

    fs::remove_all(new_dir);
    ASSERT_FALSE(fs::exists(new_dir));

    {
        FileLock lock(resource_to_lock, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(new_dir));
        ASSERT_TRUE(fs::exists(actual_lock_file));
    }

    fs::remove_all(new_dir);
}

TEST_F(FileLockTest, DirectoryPathLocking)
{
    // Standard directory locking
    {
        auto dir_to_lock = temp_dir() / "dir_to_lock";
        fs::create_directory(dir_to_lock);

        auto expected_lock_file =
            FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
        auto regular_file_lock_path =
            FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::File);
        fs::remove(expected_lock_file);
        fs::remove(regular_file_lock_path);

        FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(expected_lock_file));
        ASSERT_FALSE(fs::exists(regular_file_lock_path));

        FileLock non_conflicting_lock(temp_dir() / "dir_to_lock", ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(non_conflicting_lock.valid());
    }

    // Current directory (".")
    {
        auto expected_lock_file = FileLock::get_expected_lock_fullname_for(".", ResourceType::Directory);
        FileLock lock(".", ResourceType::Directory, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(expected_lock_file));
        // Cleanup
        fs::remove(expected_lock_file);
    }

#if !defined(PLATFORM_WIN64)
    // Path resolving to root: ensure generated lock filename is the expected root lock on POSIX.
    {
        fs::path path_to_root = ".";
        for (const auto &part : fs::current_path()) {
            if (part.string() != "/") path_to_root /= "..";
        }
        auto generated = FileLock::get_expected_lock_fullname_for(path_to_root, ResourceType::Directory);
        fs::path expected_root = "/pylabhub_root.dir.lock";
        ASSERT_EQ(generated, expected_root);

        try {
            fs::remove(generated);
            FileLock maybe_root_lock(path_to_root, ResourceType::Directory, LockMode::NonBlocking);
            if (maybe_root_lock.valid()) {
                ASSERT_TRUE(fs::exists(generated));
                fs::remove(generated);
            }
        } catch (const fs::filesystem_error &) {
            // Acceptable if lacking permissions.
        }
    }
#endif
}

TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto resource_path = temp_dir() / "multithread.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int THREADS = 64;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([&, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
            FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
            if (lock.valid())
            {
                success_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(50ms);
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_EQ(success_count.load(), 1);
}

TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    auto resource_path = temp_dir() / "multiprocess.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int PROCS = 32;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(g_self_exe_path, "nonblocking_worker", {resource_path.string()});
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
        pid_t pid = spawn_worker_process(g_self_exe_path, "nonblocking_worker", {resource_path.string()});
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

    ASSERT_EQ(success_count, 1);
}

TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto counter_path = temp_dir() / "counter.txt";
    fs::remove(counter_path);
    fs::remove(FileLock::get_expected_lock_fullname_for(counter_path, ResourceType::File));

    // initialize counter
    {
        std::ofstream ofs(counter_path);
        ofs << 0;
    }

    const int PROCS = 16;
    const int ITERS_PER_WORKER = 100;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(g_self_exe_path, "blocking_worker",
                                        {counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        ASSERT_TRUE(h != nullptr);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        CloseHandle(h);
        ASSERT_EQ(exit_code, 0);
    }
#else
    std::vector<pid_t> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(g_self_exe_path, "blocking_worker",
                                         {counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }

    for (pid_t pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#endif

    // Verify final counter value
    int final_value = 0;
    {
        std::ifstream ifs(counter_path);
        if (ifs.is_open()) { ifs >> final_value; }
    }
    ASSERT_EQ(final_value, PROCS * ITERS_PER_WORKER);
}

TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    auto resource_path = temp_dir() / "parent_child_block.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    // Parent acquires the lock
    FileLock parent_lock(resource_path, ResourceType::File, LockMode::Blocking);
    ASSERT_TRUE(parent_lock.valid());

#if defined(PLATFORM_WIN64)
    HANDLE child_proc = spawn_worker_process(g_self_exe_path, "parent_child_worker", {resource_path.string()});
    ASSERT_TRUE(child_proc != nullptr);

    // Give child time to attempt to block
    std::this_thread::sleep_for(200ms);

    // release parent lock
    parent_lock = FileLock({}, ResourceType::File, LockMode::NonBlocking);

    WaitForSingleObject(child_proc, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(child_proc, &exit_code);
    CloseHandle(child_proc);
    ASSERT_EQ(exit_code, 0);
#else
    pid_t pid = spawn_worker_process(g_self_exe_path, "parent_child_worker", {resource_path.string()});
    ASSERT_GT(pid, 0);

    std::this_thread::sleep_for(200ms);
    parent_lock = FileLock({}, ResourceType::File, LockMode::NonBlocking);

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

// ---------------------------------------------------------------------------
// End of file
// ---------------------------------------------------------------------------
