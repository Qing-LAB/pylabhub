// tests/test_filelock.cpp
//
// Unit test for pylabhub::utils::FileLock, converted to GoogleTest.

#include <gtest/gtest.h>
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

#include "test_main.h"

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

// Removed: static std::string g_self_exe_path; // Now extern from test_main.h

namespace { // Anonymous namespace for helper functions and test fixture only

// --- Test Globals & Helpers ---
static fs::path g_temp_dir; // This can stay local to this test's helpers/fixtures

// --- Worker Process Logic ---

#if defined(PLATFORM_WIN64)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &mode,
                                   const std::vector<std::string> &args)
{
    std::string cmdline = fmt::format("\"{}\" {}\n", exe, mode);
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
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    return pid;
}
#endif

// --- Test Fixture ---
class FileLockTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir);
        fmt::print("Using temporary directory: {}\n", g_temp_dir.string());
        pylabhub::utils::Initialize();
    }

    static void TearDownTestSuite() {
        pylabhub::utils::Finalize();
        fs::remove_all(g_temp_dir);
    }
};

} // anonymous namespace (end of helpers and fixtures)


// Worker function implementations (MOVED OUT OF ANONYMOUS NAMESPACE)
int worker_main_nonblocking_test(const std::string &resource_path_str)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path resource_path(resource_path_str);
    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
    if (!lock.valid())
    {
        return 1;
    }
    std::this_thread::sleep_for(3s);
    return 0;
}

int worker_main_blocking_contention(const std::string &counter_path_str, int num_iterations)
{
    Logger::instance().set_level(Logger::Level::L_ERROR);
    fs::path counter_path(counter_path_str);
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



// --- Test Cases ---
// All test cases (TEST_F) use the g_self_exe_path from test_main.h implicitly via spawn_worker_process.
// No further changes needed for test cases themselves.
TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = g_temp_dir / "basic_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_FALSE(lock.error_code());

        FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock2.valid());
    }

    FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_TRUE(lock3.valid());
}

TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = g_temp_dir / "blocking_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    std::atomic<bool> thread_has_lock = false;
    std::atomic<bool> main_released_lock = false;

    auto main_lock =
        std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
    ASSERT_TRUE(main_lock->valid());

    std::thread t1(
        [&]()
        {
            auto start = std::chrono::steady_clock::now();
            FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
            auto end = std::chrono::steady_clock::now();

            ASSERT_TRUE(thread_lock.valid());
            ASSERT_TRUE(main_released_lock.load());
            ASSERT_GT(end - start, 100ms);
            thread_has_lock = true;
        });

    std::this_thread::sleep_for(200ms);
    ASSERT_FALSE(thread_has_lock.load());

    main_released_lock = true;
    main_lock.reset();

    t1.join();
    ASSERT_TRUE(thread_has_lock.load());
}

TEST_F(FileLockTest, MoveSemantics)
{
    auto resource1 = g_temp_dir / "move1.txt";
    auto resource2 = g_temp_dir / "move2.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource1, ResourceType::File));
    fs::remove(FileLock::get_expected_lock_fullname_for(resource2, ResourceType::File));

    {
        FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1.valid());
        FileLock lock2(std::move(lock1));
        ASSERT_TRUE(lock2.valid());
        ASSERT_FALSE(lock1.valid());
    }

    {
        FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1_again.valid());
    }
}

TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = g_temp_dir / "new_dir_for_lock";
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
}

TEST_F(FileLockTest, MultiThreadedNonBlocking)
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
                std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
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

    ASSERT_EQ(success_count.load(), 1);
}

TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    auto resource_path = g_temp_dir / "multiprocess.txt";
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
        pid_t pid = spawn_worker_process(g_self_exe_path, "nonblocking_worker", {resource_path.string()});
        ASSERT_GT(pid, 0);
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

    ASSERT_EQ(success_count, 1);
}

TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto counter_path = g_temp_dir / "counter.txt";
    fs::remove(counter_path);
    fs::remove(FileLock::get_expected_lock_fullname_for(counter_path, ResourceType::File));

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

    std::ifstream ifs(counter_path);
    int final_value = 0;
    if (ifs.is_open())
    {
        ifs >> final_value;
        ifs.close();
    }
    ASSERT_EQ(final_value, PROCS * ITERS_PER_WORKER);
}

