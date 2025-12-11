// test_filelock.cpp
// Unit test for pylabhub::utils::FileLock.
//
// Usage:
//   ./test_filelock          <-- master: runs all tests
//   ./test_filelock worker <path>  <-- child worker mode (for multi-process test)

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

#include <fmt/core.h>

#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "utils/FileLock.hpp"
#include "utils/Logger.hpp" // For logging inside tests

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

// --- Worker Process Logic ---

#if defined(PLATFORM_WIN64)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &lockpath)
{
    std::string cmdline = fmt::format("\"{}\" worker \"{}\"", exe, lockpath);
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
static pid_t spawn_worker_process(const std::string &exe, const std::string &lockpath)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        execl(exe.c_str(), exe.c_str(), "worker", lockpath.c_str(), nullptr);
        _exit(127); // Should not be reached if execl is successful
    }
    return pid;
}
#endif

// Worker mode: attempt NonBlocking FileLock on provided lock path.
// Returns 0 on success, non-zero on failure.
static int worker_main(const std::string &lockpath)
{
    // Keep the logger quiet in worker processes unless there's an error.
    Logger::instance().set_level(Logger::Level::L_ERROR);

    FileLock lock(fs::path(lockpath), LockMode::NonBlocking);
    if (!lock.valid())
    {
        // On failure, print the error to stderr for easier debugging in CI.
        // For a competing worker, the expected error is "resource unavailable try again".
        if (lock.error_code())
        {
            fmt::print(stderr, "worker: failed to acquire lock: code={} msg='{}'\n",
                       lock.error_code().value(), lock.error_code().message());
        }
        return 1;
    }

    // Successfully acquired the lock. Hold it for a moment to ensure
    // other processes will see it as locked.
    std::this_thread::sleep_for(100ms);

    // Return 0 to signal success.
    return 0;
}

// --- Test Cases ---

void test_basic_nonblocking()
{
    auto lock_path = g_temp_dir / "basic.lock";
    fs::remove(lock_path);

    {
        FileLock lock(lock_path, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(!lock.error_code());

        // Try to lock it again in the same process (should fail).
        FileLock lock2(lock_path, LockMode::NonBlocking);
        CHECK(!lock2.valid());
    } // lock is released here

    // Now it should be lockable again
    FileLock lock3(lock_path, LockMode::NonBlocking);
    CHECK(lock3.valid());
}

void test_blocking_lock()
{
    auto lock_path = g_temp_dir / "blocking.lock";
    fs::remove(lock_path);

    std::atomic<bool> thread_has_lock = false;
    std::atomic<bool> main_released_lock = false;

    // 1. Main thread acquires the lock
    auto main_lock = std::make_unique<FileLock>(lock_path, LockMode::Blocking);
    CHECK(main_lock->valid());

    // 2. Spawn a thread that will try to acquire the same lock (and block)
    std::thread t1(
        [&]()
        {
            auto start = std::chrono::steady_clock::now();
            FileLock thread_lock(lock_path, LockMode::Blocking); // This should block
            auto end = std::chrono::steady_clock::now();

            // This thread should have blocked until the main thread released the lock.
            CHECK(thread_lock.valid());
            CHECK(main_released_lock.load());
            CHECK(end - start > 100ms);
            thread_has_lock = true;
        });

    // 3. Give the thread time to start and block on the lock
    std::this_thread::sleep_for(200ms);
    CHECK(!thread_has_lock.load()); // Thread should still be blocked

    // 4. Main thread releases the lock
    main_released_lock = true;
    main_lock.reset(); // This destroys the FileLock object and releases the lock

    // 5. Join the thread and check that it completed
    t1.join();
    CHECK(thread_has_lock.load());
}

void test_move_semantics()
{
    auto lock_path = g_temp_dir / "move.lock";
    fs::remove(lock_path);

    // Test move construction
    {
        FileLock lock1(lock_path, LockMode::NonBlocking);
        CHECK(lock1.valid());

        FileLock lock2(std::move(lock1));
        CHECK(lock2.valid());
        CHECK(!lock1.valid()); // lock1 should be invalid after move
    } // lock2 is destroyed, releasing the lock

    // Test move assignment
    {
        FileLock lock3(lock_path, LockMode::NonBlocking);
        CHECK(lock3.valid());

        FileLock lock4(g_temp_dir / "another.lock", LockMode::NonBlocking);
        CHECK(lock4.valid()); // Holds a different lock

        lock4 = std::move(lock3); // lock4 releases its old lock and takes lock3's
        CHECK(lock4.valid());
        CHECK(!lock3.valid());
    } // lock4 is destroyed, releasing the lock
}

void test_directory_creation()
{
    // This test verifies that if the parent directory for a lock file does not
    // exist, FileLock will create it automatically.
    auto new_dir = g_temp_dir / "new_dir_for_lock";
    auto resource_to_lock = new_dir / "resource.txt";
    auto actual_lock_file = new_dir / "resource.txt.lock";

    fs::remove_all(new_dir); // Ensure the parent directory doesn't exist.
    CHECK(!fs::exists(new_dir));

    {
        // Create a lock for a resource inside the non-existent directory.
        FileLock lock(resource_to_lock, LockMode::NonBlocking);
        CHECK(lock.valid());

        // FileLock should have created the directory for the lock file.
        CHECK(fs::exists(new_dir));

        // It should also have created the lock file itself.
        CHECK(fs::exists(actual_lock_file));
    }

    fs::remove_all(new_dir); // Cleanup
}

void test_multithread_nonblocking()
{
    auto lock_path = g_temp_dir / "multithread.lock";
    fs::remove(lock_path);

    const int THREADS = 16;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                // Each thread makes one attempt.
                FileLock lock(lock_path, LockMode::NonBlocking);
                if (lock.valid())
                {
                    success_count++;
                    // Hold the lock for a moment to increase contention
                    std::this_thread::sleep_for(50ms);
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    // Exactly one thread should have succeeded in acquiring the non-blocking lock.
    CHECK(success_count.load() == 1);
}

void test_multiprocess_nonblocking(const std::string &self_exe)
{
    auto lock_path = g_temp_dir / "multiprocess.lock";
    fs::remove(lock_path);

    const int PROCS = 8;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(self_exe, lock_path.string());
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
        pid_t pid = spawn_worker_process(self_exe, lock_path.string());
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

    // Exactly one worker process should have succeeded.
    CHECK(success_count == 1);
}

int main(int argc, char **argv)
{
    // Worker process entry point
    if (argc > 1 && std::string(argv[1]) == "worker")
    {
        if (argc < 3)
        {
            fmt::print(stderr, "Worker mode requires a lock path argument.\n");
            return 2;
        }
        return worker_main(argv[2]);
    }

    // Main test runner
    fmt::print("--- FileLock Test Suite ---\n");
    g_temp_dir = fs::temp_directory_path() / "pylabhub_filelock_tests";
    fs::create_directories(g_temp_dir);
    fmt::print("Using temporary directory: {}\n", g_temp_dir.string());

    TEST_CASE("Basic Non-Blocking Lock", test_basic_nonblocking);
    TEST_CASE("Blocking Lock Behavior", test_blocking_lock);
    TEST_CASE("Move Semantics", test_move_semantics);
    TEST_CASE("Automatic Directory Creation", test_directory_creation);
    TEST_CASE("Multi-Threaded Non-Blocking Lock", test_multithread_nonblocking);

    std::string self_exe = argv[0];
    TEST_CASE("Multi-Process Non-Blocking Lock",
              [&]() { test_multiprocess_nonblocking(self_exe); });

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    // Final cleanup
    fs::remove_all(g_temp_dir);

    return tests_failed == 0 ? 0 : 1;
}
