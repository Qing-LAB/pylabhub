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
#include <mutex> // Required for std::mutex and std::lock_guard
#include <condition_variable> // Required for std::condition_variable

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
#include "utils/Lifecycle.hpp"
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
static HANDLE spawn_worker_process(const std::string &exe, const std::string &mode,
                                   const std::vector<std::string> &args)
{
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
        // Child process
        // Prepare arguments for execv
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(exe.c_str(), argv.data());
        _exit(127); // Should not be reached if execv is successful
    }
    return pid;
}
#endif

// Worker mode for the multi-process non-blocking test.
// This worker attempts to acquire a non-blocking lock on the given resource.
// It exits with code 0 on success and 1 on failure.
static int worker_main_nonblocking_test(const std::string &resource_path_str)
{
    // Keep the logger quiet in worker processes unless there's an error.
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);

    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
    if (!lock.valid())
    {
        if (lock.error_code())
        {
            std::string err_msg =
                fmt::format("worker: failed to acquire lock: code={} msg='{}'\n",
                            lock.error_code().value(), lock.error_code().message());
#if defined(PLATFORM_WIN64)
            fmt::print(stderr, "{}", err_msg);
            DWORD last = GetLastError();
            fmt::print(stderr, "worker: last error: {}\n", last);
#else
            ::write(STDERR_FILENO, err_msg.c_str(), err_msg.length());
#endif
        }
        return 1;
    }

    // Successfully acquired the lock. Hold it for a long time to ensure
    // all other processes have a chance to attempt their lock and fail.
    // This is crucial for the correctness of the non-blocking multi-process test.
    std::this_thread::sleep_for(3s);
    return 0;
}

// Worker mode for the multi-process blocking contention test.
// This worker acquires a blocking lock in a loop, reads a number from a shared
// file, increments it, and writes it back. This simulates a real-world critical
// section and tests the lock's ability to prevent race conditions under load.
// It returns 0 on success, 1 on any failure.
static int worker_main_blocking_contention(const std::string &counter_path_str, int num_iterations)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path counter_path(counter_path_str);

    // Seed random number generator for introducing jitter.
    std::srand(
        static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                  std::chrono::system_clock::now().time_since_epoch().count()));

    for (int i = 0; i < num_iterations; ++i)
    {
        // Add a random pre-lock delay to stagger attempts and increase contention.
        if (std::rand() % 2 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
        }

        FileLock lock(counter_path, ResourceType::File, LockMode::Blocking);
        if (!lock.valid())
        {
            // This should not happen in a blocking lock test unless a serious error occurs.
            return 1;
        }

        // Add a random delay within the critical section to simulate work.
        if (std::rand() % 10 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
        }

        std::ifstream ifs(counter_path);
        int current_value = 0;
        if (ifs.is_open())
        {
            ifs >> current_value;
            ifs.close();
        }

        std::ofstream ofs(counter_path);
        ofs << (current_value + 1);
        ofs.close();
    }
    return 0;
}

// Worker for the parent-child blocking test.
// This worker attempts to acquire a blocking lock that its parent process is
// holding. It checks that it was forced to wait for a significant amount of
// time before succeeding, thus verifying the "blocking" aspect of the lock.
// Returns 0 on success, 1 on lock failure, 2 if it didn't block as expected.
static int worker_main_parent_child(const std::string &resource_path_str)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);

    auto start = std::chrono::steady_clock::now();
    FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
    auto end = std::chrono::steady_clock::now();

    if (!lock.valid())
    {
        return 1;
    }

    // Check that we actually blocked for a significant time.
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    if (duration.count() < 100)
    {
        fmt::print(stderr, "Child did not block as expected. Wait time: {}ms\n", duration.count());
        return 2;
    }

    return 0;
}

// --- Test Cases ---

// Tests the fundamental non-blocking lock behavior.
// 1. Acquires a lock and verifies it is valid.
// 2. Tries to acquire the *same lock* again from the same thread and verifies
//    that this second attempt fails (intra-process locking).
// 3. Releases the first lock and verifies that a new lock can be acquired.
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

// Tests the blocking lock behavior between two threads.
// 1. The main thread acquires a blocking lock.
// 2. A second thread is spawned and attempts to acquire the same blocking lock.
// 3. Verifies that the second thread is waiting (blocked).
// 4. The main thread releases the lock.
// 5. Verifies that the second thread successfully acquires the lock and finishes.
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

// Verifies the correctness of the move constructor and move assignment operator.
// Ensures that lock ownership is properly transferred and that the moved-from
// object becomes invalid and releases no resources.
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
        FileLock lock2(std::move(lock1)); // Move constructor
        CHECK(lock2.valid());
        CHECK(!lock1.valid()); // Original is now invalid
    } // lock2 is released here

    // The resource should be free now.
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

        lock_B = std::move(lock_A); // Move assignment
        CHECK(lock_B.valid());       // B now owns the lock on resource1
        CHECK(!lock_A.valid());      // A is now invalid

        // lock_B's original lock on resource2 should have been released.
        FileLock lock_res2_again(resource2, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_res2_again.valid());
    }

    // lock_B (owning lock on resource1) is now out of scope.
    {
        FileLock lock_res1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_res1_again.valid());
    }

    fmt::print("  - Testing self-move assignment\n");
    {
        FileLock lock_self(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_self.valid());

// Suppress compiler warnings about self-move.
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

        CHECK(lock_self.valid()); // Object should remain valid after self-move.
    }

    // The lock should have been released.
    {
        FileLock lock_after_self(resource1, ResourceType::File, LockMode::NonBlocking);
        CHECK(lock_after_self.valid());
    }
}

// Tests that the FileLock constructor can create the necessary parent
// directories for the lock file if they do not already exist.
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
        CHECK(fs::exists(new_dir));           // The directory should have been created.
        CHECK(fs::exists(actual_lock_file)); // The lock file itself should exist.
    }

    fs::remove_all(new_dir);
}

// --- High Contention Tests ---

// Spawns a large number of threads that all try to acquire the same
// non-blocking lock at the same time. Verifies that exactly one thread succeeds.
void test_multithread_nonblocking()
{
    auto resource_path = g_temp_dir / "multithread.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int THREADS = 64;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                // Add a small, varied delay to increase the chance of simultaneous execution.
                std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
                FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                if (lock.valid())
                {
                    success_count++;
                    // Hold the lock for a moment to ensure others see it as locked.
                    std::this_thread::sleep_for(50ms);
                }
            });
    }

    for (auto &t : threads)
        t.join();

    CHECK(success_count.load() == 1);
}

// Spawns a large number of *processes* that all try to acquire the same
// non-blocking lock. This is the core test for cross-process exclusion.
// It verifies that exactly one process succeeds.
void test_multiprocess_nonblocking(const std::string &self_exe)
{
    auto resource_path = g_temp_dir / "multiprocess.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int PROCS = 32;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<HANDLE> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(self_exe, "nonblocking_worker", {resource_path.string()});
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
        pid_t pid = spawn_worker_process(self_exe, "nonblocking_worker", {resource_path.string()});
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

// Spawns many processes that repeatedly contend for a *blocking* lock to
// increment a counter in a shared file. This tests the lock's ability to
// protect a critical section from race conditions under heavy load.
// The test passes if the final counter value is correct.
void test_multiprocess_blocking_contention(const std::string &self_exe)
{
    auto counter_path = g_temp_dir / "counter.txt";
    fs::remove(counter_path);
    fs::remove(FileLock::get_expected_lock_fullname_for(counter_path, ResourceType::File));


    // Initialize counter file to 0.
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
        HANDLE h = spawn_worker_process(self_exe, "blocking_worker",
                                        {counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        CHECK(h != nullptr);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        CloseHandle(h);
        CHECK(exit_code == 0); // Each worker must succeed.
    }
#else
    std::vector<pid_t> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(self_exe, "blocking_worker",
                                         {counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        CHECK(pid > 0);
        pids.push_back(pid);
    }

    for (pid_t pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); // Each worker must succeed.
    }
#endif

    // Verify the final counter value.
    std::ifstream ifs(counter_path);
    int final_value = 0;
    if (ifs.is_open())
    {
        ifs >> final_value;
        ifs.close();
    }
    CHECK(final_value == PROCS * ITERS_PER_WORKER);
}

// Tests a simple parent/child blocking scenario.
// 1. The parent process acquires a lock.
// 2. The parent spawns a child process.
// 3. The child attempts to acquire the same lock (and should block).
// 4. The parent sleeps, then releases the lock.
// 5. The parent waits for the child, which should now be able to finish.
//    The child internally checks that it was actually forced to wait.
void test_multiprocess_parent_child_blocking(const std::string &self_exe)
{
    auto resource_path = g_temp_dir / "parent_child_block.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    // 1. Parent acquires the lock.
    FileLock parent_lock(resource_path, ResourceType::File, LockMode::Blocking);
    CHECK(parent_lock.valid());

#if defined(PLATFORM_WIN64)
    // 2. Spawn child, which will block.
    HANDLE child_proc =
        spawn_worker_process(self_exe, "parent_child_worker", {resource_path.string()});
    CHECK(child_proc != nullptr);

    // 3. Parent sleeps to ensure child has time to try and block on the lock.
    std::this_thread::sleep_for(200ms);

    // 4. Parent releases lock.
    parent_lock = FileLock({}, ResourceType::File, LockMode::NonBlocking);

    // 5. Child should now be able to acquire the lock and exit successfully.
    WaitForSingleObject(child_proc, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(child_proc, &exit_code);
    CloseHandle(child_proc);
    CHECK(exit_code == 0);
#else
    pid_t pid = spawn_worker_process(self_exe, "parent_child_worker", {resource_path.string()});
    CHECK(pid > 0);
    std::this_thread::sleep_for(200ms);
    parent_lock = FileLock({}, ResourceType::File, LockMode::NonBlocking);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

// Tests the timed lock constructor.
// 1. Acquires a lock.
// 2. Verifies that a second, timed lock attempt on the same resource fails
//    with a 'timed_out' error and takes the expected amount of time.
// 3. Verifies that a timed lock attempt on a free resource succeeds.
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
        CHECK(duration.count() < 1000); // Check it didn't wait excessively.
    }

    // Now that the main_lock is released, a timed lock should succeed.
    FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
    if (!timed_lock_succeed.valid())
    {
        fmt::print(stderr, "  timed_lock_succeed failed with error: {}\n",
                   timed_lock_succeed.error_code().message());
    }
    CHECK(timed_lock_succeed.valid());
    CHECK(!timed_lock_succeed.error_code());
}

// Tests the lock file naming convention for directory paths.
// Ensures that locking a directory creates a `.dir.lock` file in the parent,
// preventing name collisions with locks on files of the same name.
// Also tests edge cases like locking the current directory (".").
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

        // Lock the path as a directory.
        FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
        CHECK(lock.valid());
        CHECK(fs::exists(expected_lock_file));
        CHECK(!fs::exists(regular_file_lock_path));

        // A lock on a file with the same name should not conflict.
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

namespace
{
struct TestLifecycleManager
{
    TestLifecycleManager() { pylabhub::utils::Initialize(); }
    ~TestLifecycleManager() { pylabhub::utils::Finalize(); }
};
} // namespace

int main(int argc, char **argv)
{
    TestLifecycleManager lifecycle_manager;
    if (argc > 1)
    {
        std::string mode = argv[1];
        if (mode == "nonblocking_worker")
        {
            if (argc < 3)
            {
                fmt::print(stderr, "Worker mode requires a resource path argument.\n");
                return 2;
            }
            return worker_main_nonblocking_test(argv[2]);
        }
        else if (mode == "blocking_worker")
        {
            if (argc < 4)
            {
                fmt::print(stderr, "Blocking worker mode requires counter path and iterations.\n");
                return 2;
            }
            int iterations = std::stoi(argv[3]);
            return worker_main_blocking_contention(argv[2], iterations);
        }
        else if (mode == "parent_child_worker")
        {
            if (argc < 3)
            {
                fmt::print(stderr, "Parent-child worker requires a resource path argument.\n");
                return 2;
            }
            return worker_main_parent_child(argv[2]);
        }
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
    TEST_CASE("Multi-Process Blocking Contention (Counter)",
              [&]() { test_multiprocess_blocking_contention(self_exe); });
    TEST_CASE("Multi-Process Parent-Child Blocking",
              [&]() { test_multiprocess_parent_child_blocking(self_exe); });

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    fs::remove_all(g_temp_dir);

    return tests_failed == 0 ? 0 : 1;
}