// test_filelock.cpp
// Unit test for pylabhub::utils::FileLock.
//
// Usage:
//   ./test_filelock          <-- master: runs all tests
//   ./test_filelock worker <resource_path>  <-- child worker mode (for multi-process test)

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
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{} \n", #condition, __FILE__, __LINE__);  \
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
        fmt::print(stderr, "  --- FAILED: {} \n", e.what());
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
static HANDLE spawn_worker_process(const std::string &exe, const std::string &resource_path)
{
    std::string cmdline = fmt::format("\"{}\" worker \"{}\"", exe, resource_path);
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
static pid_t spawn_worker_process(const std::string &exe, const std::string &resource_path)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        execl(exe.c_str(), exe.c_str(), "worker", resource_path.c_str(), nullptr);
        _exit(127); // Should not be reached if execl is successful
    }
    return pid;
}
#endif

// Worker mode: attempt NonBlocking FileLock on provided resource path.
// Returns 0 on success, non-zero on failure.
static int worker_main(const std::string &resource_path_str)
{
    // Keep the logger quiet in worker processes unless there's an error.
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);

    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
    if (!lock.valid())
    {
        if (lock.error_code())
        {
            // On failure, print the error to stderr for easier debugging in CI.
            std::string err_msg =
                fmt::format("worker: failed to acquire lock: code={} msg='{}'\n",
                            lock.error_code().value(), lock.error_code().message());
#if defined(PLATFORM_WIN64)
            // On Windows, CreateProcess is used, so fmt::print to stderr is generally safe.
            fmt::print(stderr, "{}", err_msg);
            DWORD last = GetLastError();
            fmt::print(stderr, "worker: last error: {}\n", last);
#else
            // On POSIX, use write() for async-signal-safety.
            ::write(STDERR_FILENO, err_msg.c_str(), err_msg.length());
#endif
        }
        return 1;
    }

    // Successfully acquired the lock. Hold it for a moment.
    std::this_thread::sleep_for(100ms);
    return 0;
}

// --- Test Cases ---

void test_basic_nonblocking()
{
    auto resource_path = g_temp_dir / "basic_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(!lock.error_code());

        // Try to lock it again in the same process (should fail).
        FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
        CHECK(!lock2.valid());
    } // lock is released here

    // Now it should be lockable again
    FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
    CHECK(lock3.valid());
}

void test_blocking_lock()
{
    auto resource_path = g_temp_dir / "blocking_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    std::atomic<bool> thread_has_lock = false;
    std::atomic<bool> main_released_lock = false;

    auto main_lock =
        std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
    CHECK(main_lock->valid());

    std::thread t1(
        [&]()
        {
            auto start = std::chrono::steady_clock::now();
            FileLock thread_lock(resource_path, ResourceType::File,
                                 LockMode::Blocking); // This should block
            auto end = std::chrono::steady_clock::now();

            CHECK(thread_lock.valid());
            CHECK(main_released_lock.load());
            CHECK(end - start > 100ms);
            thread_has_lock = true;
        });

    std::this_thread::sleep_for(200ms);
    CHECK(!thread_has_lock.load());

    main_released_lock = true;
    main_lock.reset();

    t1.join();
    CHECK(thread_has_lock.load());
}

void test_move_semantics()
{
    auto resource1 = g_temp_dir / "move1.txt";
    auto resource2 = g_temp_dir / "move2.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource1, ResourceType::File));
    fs::remove(FileLock::get_expected_lock_fullname_for(resource2, ResourceType::File));

    fmt::print("  - Testing move construction\n");
    {
        FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock1.valid());
        FileLock lock2(std::move(lock1));
        CHECK(lock2.valid());
        CHECK(!lock1.valid());
    }

    {
        FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock1_again.valid());
    }

    fmt::print("  - Testing move assignment to valid target\n");
    {
        FileLock lock_A(resource1, ResourceType::File, LockMode::NonBlocking);
        FileLock lock_B(resource2, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_A.valid());
        CHECK(lock_B.valid());

        lock_B = std::move(lock_A);
        CHECK(lock_B.valid());
        CHECK(!lock_A.valid());

        FileLock lock_res2_again(resource2, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_res2_again.valid());
    }

    {
        FileLock lock_res1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_res1_again.valid());
    }

    fmt::print("  - Testing self-move assignment\n");
    {
        FileLock lock_self(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_self.valid());

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

        CHECK(lock_self.valid());
    }

    {
        FileLock lock_after_self(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_after_self.valid());
    }
}

void test_directory_creation()
{
    auto new_dir = g_temp_dir / "new_dir_for_lock";
    auto resource_to_lock = new_dir / "resource.txt";
    auto actual_lock_file =
        FileLock::get_expected_lock_fullname_for(resource_to_lock, ResourceType::File);

    fs::remove_all(new_dir);
    CHECK(!fs::exists(new_dir));

    {
        FileLock lock(resource_to_lock, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(fs::exists(new_dir));
        CHECK(fs::exists(actual_lock_file));
    }

    fs::remove_all(new_dir);
}

void test_multithread_nonblocking()
{
    auto resource_path = g_temp_dir / "multithread.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int THREADS = 32;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                if (lock.valid())
                {
                    success_count++;
                    std::this_thread::sleep_for(50ms);
                }
            });
    }

    for (auto &t : threads)
        t.join();

    CHECK(success_count.load() == 1);
}

void test_multiprocess_nonblocking(const std::string &self_exe)
{
    auto resource_path = g_temp_dir / "multiprocess.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int PROCS = 16;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(self_exe, resource_path.string());
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
        pid_t pid = spawn_worker_process(self_exe, resource_path.string());
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

    CHECK(success_count == 1);
}

void test_timed_lock()
{
    auto resource_path = g_temp_dir / "timed.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
        CHECK(main_lock.valid());

        auto start = std::chrono::steady_clock::now();
        FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
        auto end = std::chrono::steady_clock::now();

        CHECK(!timed_lock_fail.valid());
        CHECK(timed_lock_fail.error_code() == std::errc::timed_out);

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        CHECK(duration.count() >= 100);
        CHECK(duration.count() < 300);
    }

    FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
    if (!timed_lock_succeed.valid())
    {
        fmt::print(stderr, "  timed_lock_succeed failed with error: {}\n",
                   timed_lock_succeed.error_code().message());
    }
    CHECK(timed_lock_succeed.valid());
    CHECK(!timed_lock_succeed.error_code());
}

void test_directory_path_locking()
{
    fmt::print("  - Testing standard directory\n");
    {
        auto dir_to_lock = g_temp_dir / "dir_to_lock";
        fs::create_directory(dir_to_lock);

        auto expected_lock_file =
            FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
        auto regular_file_lock_path =
            FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::File);
        fs::remove(expected_lock_file);
        fs::remove(regular_file_lock_path);

        FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(fs::exists(expected_lock_file));
        CHECK(!fs::exists(regular_file_lock_path));

        FileLock non_conflicting_lock(g_temp_dir / "dir_to_lock", ResourceType::File,
                                      LockMode::NonBlocking);
        CHECK(non_conflicting_lock.valid());
    }

    fmt::print("  - Testing current directory (.)\n");
    {
        fs::path cwd = fs::current_path();
        auto expected_lock_file =
            FileLock::get_expected_lock_fullname_for(".", ResourceType::Directory);
        fmt::print("  - CWD: '{}', expecting lock: '{}'\n", cwd.string(),
                   expected_lock_file.string());

        FileLock lock(".", ResourceType::Directory, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(fs::exists(expected_lock_file));
        lock = FileLock("non_existence_removal", ResourceType::File,
                        LockMode::NonBlocking); // Release lock
        fs::remove(expected_lock_file);         // Clean up for next test
    }

#if !defined(PLATFORM_WIN64)
    // Note: Creating files in the root directory may fail due to permissions.
    // This test focuses on ensuring that paths resolving to root are correctly
    // handled by the path generation logic.
    fmt::print("  - Testing path resolving to root (e.g., '.._.._..')\n");
    {
        // Construct a path that will resolve to root from the current directory.
        fs::path path_to_root = ".";
        for (const auto &part : fs::current_path())
        {
            // Avoid adding ".." for the root slash itself.
            if (part.string() != "/")
            {
                path_to_root /= "..";
            }
        }

        // The primary goal is to test the path generation logic.
        auto generated_lock_file =
            FileLock::get_expected_lock_fullname_for(path_to_root, ResourceType::Directory);
        fs::path correct_root_lock_file = "/pylabhub_root.dir.lock";

        fmt::print("  - Path to root: '{}', expecting lock file: '{}', generated: '{}'\n",
                   path_to_root.string(), correct_root_lock_file.string(),
                   generated_lock_file.string());

        CHECK(generated_lock_file == correct_root_lock_file);

        // We can also try to acquire the lock, but we won't fail the test if
        // it fails due to permissions. This part just verifies the locking
        // mechanism itself in permissive environments.
        try
        {
            fs::remove(generated_lock_file); // Attempt cleanup before locking
            FileLock lock(path_to_root, ResourceType::Directory, LockMode::NonBlocking);
            if (lock.valid())
            {
                fmt::print("  - NOTE: Successfully acquired lock on resolving-to-root path.\n");
                CHECK(fs::exists(generated_lock_file));
                fs::remove(generated_lock_file); // Cleanup after locking
            }
            else
            {
                fmt::print(stderr,
                           "  NOTE: Could not acquire lock on root dir, likely due to "
                           "permissions (Error: {}).\n",
                           lock.error_code().message());
            }
        }
        catch (const fs::filesystem_error &e)
        {
            // This is an acceptable outcome if we don't have permissions.
            fmt::print(stderr,
                       "  NOTE: Could not test root dir locking, likely due to permissions: {}\n",
                       e.what());
        }
    }
#endif
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "worker")
    {
        if (argc < 3)
        {
            fmt::print(stderr, "Worker mode requires a resource path argument.\n");
            return 2;
        }
        return worker_main(argv[2]);
    }

    fmt::print("--- FileLock Test Suite ---\n");
    g_temp_dir = fs::temp_directory_path() / "pylabhub_filelock_tests";
    fs::create_directories(g_temp_dir);
    fmt::print("Using temporary directory: {}\n", g_temp_dir.string());

    TEST_CASE("Basic Non-Blocking Lock", test_basic_nonblocking);
    TEST_CASE("Blocking Lock Behavior", test_blocking_lock);
    TEST_CASE("Timed Lock Behavior", test_timed_lock);
    TEST_CASE("Move Semantics", test_move_semantics);
    TEST_CASE("Automatic Directory Creation", test_directory_creation);
    TEST_CASE("Directory Path Locking", test_directory_path_locking);
    TEST_CASE("Multi-Threaded Non-Blocking Lock", test_multithread_nonblocking);

    std::string self_exe = argv[0];
    TEST_CASE("Multi-Process Non-Blocking Lock",
              [&]() { test_multiprocess_nonblocking(self_exe); });

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    fs::remove_all(g_temp_dir);

    return tests_failed == 0 ? 0 : 1;
}