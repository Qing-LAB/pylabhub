/**
 * @file test_platform_core.cpp
 * @brief Layer 0 tests for core platform APIs (PID, thread ID, time, process detection)
 *
 * Tests cover:
 * - Process and thread identification
 * - Time measurement with clock skew protection
 * - Process liveness detection (including zombie/permission scenarios)
 * - Cross-platform behavior verification
 */
#include "plh_platform.hpp"
#include "shared_test_helpers.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>

#if PYLABHUB_IS_POSIX
#include <unistd.h>
#include <sys/wait.h>
#elif defined(PYLABHUB_PLATFORM_WIN64)
#include <cstdlib>
#endif

using namespace pylabhub::platform;
using namespace ::testing;
using namespace std::chrono_literals;

// ============================================================================
// Basic Platform API Tests
// ============================================================================

/**
 * Test get_pid() returns a valid, non-zero process ID
 */
TEST(PlatformCoreTest, GetPID_ReturnsValidID)
{
    uint64_t pid = get_pid();
    EXPECT_GT(pid, 0u) << "PID should be greater than zero";
}

/**
 * Test get_pid() is stable across multiple calls in the same process
 */
TEST(PlatformCoreTest, GetPID_IsStable)
{
    uint64_t pid1 = get_pid();
    uint64_t pid2 = get_pid();
    EXPECT_EQ(pid1, pid2) << "PID should be stable within the same process";
}

/**
 * Test get_native_thread_id() returns a valid, non-zero thread ID
 */
TEST(PlatformCoreTest, GetThreadID_ReturnsValidID)
{
    uint64_t tid = get_native_thread_id();
    EXPECT_GT(tid, 0u) << "Thread ID should be greater than zero";
}

/**
 * Test get_native_thread_id() is stable for the same thread
 */
TEST(PlatformCoreTest, GetThreadID_IsStableForSameThread)
{
    uint64_t tid1 = get_native_thread_id();
    uint64_t tid2 = get_native_thread_id();
    EXPECT_EQ(tid1, tid2) << "Thread ID should be stable for the same thread";
}

/**
 * Test get_native_thread_id() returns different IDs for different threads
 */
TEST(PlatformCoreTest, GetThreadID_DifferentForDifferentThreads)
{
    uint64_t main_tid = get_native_thread_id();

    std::atomic<uint64_t> worker_tid{0};
    std::thread worker([&worker_tid]() { worker_tid.store(get_native_thread_id()); });
    worker.join();

    EXPECT_GT(worker_tid.load(), 0u) << "Worker thread ID should be valid";
    EXPECT_NE(main_tid, worker_tid.load()) << "Different threads should have different thread IDs";
}

// ============================================================================
// Time API Tests
// ============================================================================

/**
 * Test monotonic_time_ns() returns increasing values
 */
TEST(PlatformCoreTest, MonotonicTime_IsIncreasing)
{
    uint64_t t1 = monotonic_time_ns();
    std::this_thread::sleep_for(1ms);
    uint64_t t2 = monotonic_time_ns();

    EXPECT_GT(t2, t1) << "Monotonic time should increase";
    EXPECT_GT(t2 - t1, 0u) << "Time delta should be positive";
}

/**
 * Test monotonic_time_ns() has reasonable resolution (nanoseconds)
 */
TEST(PlatformCoreTest, MonotonicTime_HasNanosecondResolution)
{
    // Take multiple samples
    const int samples = 100;
    uint64_t min_delta = UINT64_MAX;

    for (int i = 0; i < samples; ++i)
    {
        uint64_t t1 = monotonic_time_ns();
        uint64_t t2 = monotonic_time_ns();
        if (t2 > t1)
        {
            uint64_t delta = t2 - t1;
            min_delta = std::min(min_delta, delta);
        }
    }

    // Expect at least some deltas in the microsecond range or better
    EXPECT_LT(min_delta, 1'000'000u)
        << "Minimum observed delta should be less than 1ms (indicating sub-millisecond resolution)";
}

/**
 * Test elapsed_time_ns() calculates correct deltas
 */
TEST(PlatformCoreTest, ElapsedTime_CalculatesCorrectDelta)
{
    uint64_t start = monotonic_time_ns();
    std::this_thread::sleep_for(10ms);
    uint64_t elapsed = elapsed_time_ns(start);

    // Should be approximately 10ms (allow some variance)
    EXPECT_GT(elapsed, 5'000'000u) << "Elapsed time should be > 5ms";
    EXPECT_LT(elapsed, 50'000'000u) << "Elapsed time should be < 50ms";
}

/**
 * CRITICAL: Test elapsed_time_ns() protects against clock skew
 *
 * If the clock goes backwards (start > now), elapsed_time_ns() should return 0
 * instead of wrapping around to a huge value.
 */
TEST(PlatformCoreTest, ElapsedTime_ProtectsAgainstClockSkew)
{
    // Simulate clock skew by passing a future timestamp
    uint64_t now = monotonic_time_ns();
    uint64_t future = now + 1'000'000'000u; // 1 second in the future

    uint64_t elapsed = elapsed_time_ns(future);

    // Should return 0 instead of wrapping to a huge negative value
    EXPECT_EQ(elapsed, 0u)
        << "elapsed_time_ns() should return 0 when start > now (clock skew protection)";
}

// ============================================================================
// Process Liveness Detection Tests
// ============================================================================

/**
 * Test is_process_alive() returns true for the current process
 */
TEST(PlatformCoreTest, IsProcessAlive_CurrentProcess)
{
    uint64_t my_pid = get_pid();
    EXPECT_TRUE(is_process_alive(my_pid)) << "Current process should be alive";
}

/**
 * Test is_process_alive() returns false for an invalid PID (0)
 */
TEST(PlatformCoreTest, IsProcessAlive_InvalidPID_Zero)
{
    EXPECT_FALSE(is_process_alive(0)) << "PID 0 should be considered not alive";
}

/**
 * Test is_process_alive() returns false for a very large, unlikely PID
 *
 * This tests the "definitely dead" case without needing to spawn and kill a process.
 */
TEST(PlatformCoreTest, IsProcessAlive_UnlikelyPID)
{
    // Use a very large PID that is extremely unlikely to exist
    uint64_t unlikely_pid = UINT64_MAX - 1;

    // This should return false (process doesn't exist)
    bool alive = is_process_alive(unlikely_pid);
    EXPECT_FALSE(alive) << "Extremely large PID should be considered not alive";
}

/**
 * Test is_process_alive() correctly detects alive then dead transition.
 *
 * Spawns a child process, verifies it is alive, signals it to exit (POSIX) or
 * terminates it (Windows), waits for it, then verifies it is detected as dead.
 * Uses pipe sync on POSIX and CreateProcess+TerminateProcess on Windows.
 */
TEST(PlatformCoreTest, IsProcessAlive_DetectsAliveThenDeadProcess)
{
#if PYLABHUB_IS_POSIX
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0) << "pipe() failed";

    pid_t child_pid = fork();
    ASSERT_GE(child_pid, 0) << "fork() failed";

    if (child_pid == 0)
    {
        close(pipefd[1]);
        char c;
        (void)read(pipefd[0], &c, 1);
        close(pipefd[0]);
        _exit(0);
    }

    close(pipefd[0]);
    uint64_t pid = static_cast<uint64_t>(child_pid);

    EXPECT_TRUE(is_process_alive(pid)) << "Child process should be alive (blocking on read)";

    close(pipefd[1]);

    int status = 0;
    ASSERT_EQ(waitpid(child_pid, &status, 0), child_pid) << "waitpid failed";
    ASSERT_TRUE(WIFEXITED(status)) << "Child should exit normally";

    EXPECT_FALSE(is_process_alive(pid)) << "Child process should be detected as dead after exit";

#elif defined(PYLABHUB_PLATFORM_WIN64)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    pi.hProcess = INVALID_HANDLE_VALUE;
    pi.hThread = INVALID_HANDLE_VALUE;

    char cmdline[] = "cmd.exe /c ping 127.0.0.1 -n 6 >nul";
    BOOL ok = CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    ASSERT_TRUE(ok) << "CreateProcess failed";

    uint64_t pid = static_cast<uint64_t>(pi.dwProcessId);

    EXPECT_TRUE(is_process_alive(pid)) << "Child process should be alive";

    TerminateProcess(pi.hProcess, 0);
    DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    ASSERT_EQ(wait, WAIT_OBJECT_0) << "WaitForSingleObject failed or timed out";

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    EXPECT_FALSE(is_process_alive(pid)) << "Child process should be detected as dead after exit";

#else
    GTEST_SKIP() << "Platform not supported";
#endif
}

/**
 * Test get_executable_name() with full path
 */
TEST(PlatformCoreTest, GetExecutableName_WithPath)
{
    std::string full_path = get_executable_name(true);

    EXPECT_FALSE(full_path.empty()) << "Executable path should not be empty";
    EXPECT_THAT(full_path, HasSubstr("test_layer0_platform"))
        << "Path should contain test executable name";
}

/**
 * Test get_executable_name() without path (filename only)
 */
TEST(PlatformCoreTest, GetExecutableName_WithoutPath)
{
    std::string filename = get_executable_name(false);

    EXPECT_FALSE(filename.empty()) << "Executable filename should not be empty";

#if defined(PYLABHUB_PLATFORM_WIN64)
    EXPECT_EQ(filename, "test_layer0_platform_core.exe")
        << "Windows executable should have .exe extension";
#else
    EXPECT_EQ(filename, "test_layer0_platform_core")
        << "POSIX executable should not have extension";
#endif
}

// ============================================================================
// Version API Tests
// ============================================================================

TEST(PlatformCoreTest, VersionAPI_MajorIsValid)
{
    int major = get_version_major();
    EXPECT_GE(major, 0) << "Version major should be non-negative";
}

TEST(PlatformCoreTest, VersionAPI_MinorIsValid)
{
    int minor = get_version_minor();
    EXPECT_GE(minor, 0) << "Version minor should be non-negative";
}

TEST(PlatformCoreTest, VersionAPI_RollingIsValid)
{
    int rolling = get_version_rolling();
    EXPECT_GE(rolling, 0) << "Version rolling should be non-negative";
}

TEST(PlatformCoreTest, VersionAPI_StringMatchesComponents)
{
    std::string expected = std::to_string(get_version_major()) + "." +
                           std::to_string(get_version_minor()) + "." +
                           std::to_string(get_version_rolling());

    EXPECT_EQ(get_version_string(), expected)
        << "Version string should match individual components";
}

TEST(PlatformCoreTest, VersionAPI_StringFormat)
{
    const char *ver = get_version_string();
    ASSERT_NE(ver, nullptr) << "Version string should not be null";
    EXPECT_GT(strlen(ver), 0u) << "Version string should not be empty";

    // Format: major.minor.rolling (e.g., "0.1.42")
    EXPECT_THAT(ver, MatchesRegex(R"(^[0-9]+\.[0-9]+\.[0-9]+$)"))
        << "Version string should match major.minor.rolling format";
}
