// test_atomicguard.cpp
//
// Build:
//   g++ -std=c++17 -O2 -pthread test_atomicguard.cpp -o test_atomicguard
//
// Run:
//   ./test_atomicguard

#include <cstdlib> // For std::abort()

// Ensure that for this test, PANIC() results in a standard abort,
// regardless of any custom definition provided during the build.
// This is critical for the correctness of test_destructor_abort_on_invariant_violation.
#ifdef PANIC
#undef PANIC
#endif
#define PANIC(msg) std::abort()

#include "utils/AtomicGuard.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// --- Minimal Test Harness ---
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (condition)                                                                             \
        {                                                                                          \
            /* Test passed, do nothing */                                                          \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            std::cerr << "  CHECK FAILED: " << #condition << " at " << __FILE__ << ":" << __LINE__ \
                      << std::endl;                                                                \
            throw std::runtime_error("Test case failed");                                          \
        }                                                                                          \
    } while (0)

void TEST_CASE(const std::string &name, std::function<void()> test_func)
{
    std::cout << "\n=== " << name << " ===\n";
    try
    {
        test_func();
        tests_passed++;
        std::cout << "  --- PASSED ---\n";
    }
    catch (const std::exception &e)
    {
        tests_failed++;
        std::cerr << "  --- FAILED: " << e.what() << " ---\n";
    }
    catch (...)
    {
        tests_failed++;
        std::cerr << "  --- FAILED with unknown exception ---\n";
    }
}

// --- Test Cases ---

void test_basic_acquire_release()
{
    AtomicOwner owner;
    AtomicGuard guard(&owner);
    CHECK(guard.token() != 0);
    CHECK(!guard.active());

    CHECK(guard.acquire());
    CHECK(guard.active());
    CHECK(owner.load() == guard.token());

    CHECK(guard.release());
    CHECK(!guard.active());
    CHECK(owner.is_free());
}

void test_raii_and_token_persistence()
{
    AtomicOwner owner;
    uint64_t token_in_scope = 0;
    {
        AtomicGuard g(&owner, true); // acquire on construction
        CHECK(g.token() != 0);
        token_in_scope = g.token();
        CHECK(g.active());
        CHECK(owner.load() == token_in_scope);
    } // destructor releases
    CHECK(owner.is_free());
}

void test_concurrent_acquire()
{
    AtomicOwner owner;
    constexpr int THREADS = 8;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                auto until = std::chrono::steady_clock::now() + 200ms;
                while (std::chrono::steady_clock::now() < until)
                {
                    // Create a new guard for each attempt, testing the RAII case.
                    AtomicGuard g(&owner);
                    if (g.acquire())
                    {
                        success_count++;
                        CHECK(g.active());
                        std::this_thread::sleep_for(1ms);
                    } // g's destructor is called here, releasing the lock.
                }
            });
    }

    for (auto &t : threads)
        t.join();

    CHECK(success_count.load() > 0);
    CHECK(owner.is_free());
}

void test_transfer_single_thread()
{
    AtomicOwner owner;
    AtomicGuard a(&owner);
    AtomicGuard b(&owner);

    CHECK(a.acquire());
    CHECK(a.active());

    CHECK(a.transfer_to(b));
    CHECK(!a.active());
    CHECK(b.active());
    CHECK(owner.load() == b.token());

    CHECK(b.release());
    CHECK(owner.is_free());
}

void test_transfer_between_threads()
{
    AtomicOwner owner;
    AtomicGuard guard_A(&owner);
    AtomicGuard guard_B; // Unattached initially

    std::atomic<bool> transfer_done{false};

    CHECK(guard_A.acquire());

    std::thread t2(
        [&]()
        {
            guard_B.attach(&owner);
            while (!guard_B.active())
            {
                std::this_thread::sleep_for(1ms);
            }
            // Now we are the owner
            CHECK(owner.load() == guard_B.token());
            CHECK(guard_B.release());
            transfer_done = true;
        });

    std::this_thread::sleep_for(10ms); // give t2 time to start waiting
    CHECK(guard_A.transfer_to(guard_B));

    t2.join();
    CHECK(transfer_done.load());
    CHECK(owner.is_free());
}

void test_transfer_rejects_different_owners()
{
    AtomicOwner o1, o2;
    AtomicGuard a(&o1);
    AtomicGuard b(&o2);

    CHECK(a.acquire());
    CHECK(!a.transfer_to(b)); // must reject
    CHECK(a.active());       // still active
    CHECK(a.release());
}

void test_destructor_with_transfer()
{
    AtomicOwner owner;
    {
        AtomicGuard a(&owner, true);
        CHECK(a.active());

        AtomicGuard b(&owner);
        CHECK(a.transfer_to(b));

        // `a` is no longer owner, its destructor should not release
        // `b` is owner, its destructor will release
    } // a is destroyed first, then b
    CHECK(owner.is_free());
}

void test_attach_and_detach()
{
    AtomicOwner owner;
    AtomicGuard guard;

    CHECK(!guard.active());
    CHECK(!guard.acquire()); // Fails, not attached

    // Attach and acquire
    CHECK(guard.attach_and_acquire(&owner));
    CHECK(guard.active());
    CHECK(guard.release());

    // Detach
    guard.detach_no_release();
    CHECK(!guard.acquire()); // Fails again
}

void test_noop_destructor_scenarios()
{
    // Test 1: A guard that is never attached to an owner.
    // Its destructor should be a complete no-op and not abort.
    {
        AtomicGuard g;
        CHECK(!g.active());
    } // Destructor runs here.
    CHECK(true); // If we get here, it didn't abort.

    // Test 2: A guard that is attached but never acquires.
    // Its destructor should also be a no-op because has_ever_acquired_ is false.
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        CHECK(!g.active());
    } // Destructor runs here.
    CHECK(owner.is_free());
}

// This function contains logic that is expected to cause the program to abort.
// It will be run in a dedicated child process.
void trigger_abort_logic()
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner, true); // g acquires the lock
        if (!g.active())
        {
            // This shouldn't happen in a single-threaded context.
            // If it does, the test is flawed, so exit with a unique code.
            std::cerr << "ABORT_TEST: Failed to acquire lock initially.\n";
            exit(5);
        }

        // Simulate another entity "stealing" the lock by directly manipulating the owner state.
        // This violates the invariants of AtomicGuard.
        owner.store(12345); // Some other non-zero token

    } // g's destructor runs here. It will find:
      // 1. It once owned the lock (has_ever_acquired_ is true).
      // 2. It did not transfer ownership (released_via_transfer_ is false).
      // 3. It cannot release the lock because the owner state is not its token.
      // This should trigger the std::abort().
}

#if defined(_WIN32)
static HANDLE spawn_child_process(const std::string &exe, const std::string &arg)
{
    std::string cmdline = "\"" + exe + "\" " + arg;
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
static pid_t spawn_child_process(const std::string &exe, const std::string &arg)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        execl(exe.c_str(), exe.c_str(), arg.c_str(), nullptr);
        _exit(127); // Should not be reached if execl is successful
    }
    return pid;
}
#endif

void test_destructor_abort_on_invariant_violation(const std::string &self_exe)
{
    std::cout << "  Spawning child process to test abort condition...\n";
#if defined(_WIN32)
    HANDLE hProcess = spawn_child_process(self_exe, "trigger_abort");
    CHECK(hProcess != nullptr);

    WaitForSingleObject(hProcess, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(hProcess, &exit_code);
    CloseHandle(hProcess);

    // On Windows, abort() typically results in exit code 3.
    // We check for a non-zero exit code, as specific codes can vary.
    CHECK(exit_code != 0);
    std::cout << "  Child process exited with code " << exit_code << " (expected non-zero for abort).\n";
#else
    pid_t pid = spawn_child_process(self_exe, "trigger_abort");
    CHECK(pid > 0);
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    std::cout << "  Child process terminated by signal " << WTERMSIG(status) << " (expected SIGABRT).\n";
#endif
}

int main(int argc, char **argv)
{
    // If the executable is called with "trigger_abort", run only that logic.
    // This prevents the child process from re-running the entire test suite.
    if (argc > 1 && std::string(argv[1]) == "trigger_abort")
    {
        trigger_abort_logic();
        // This line should not be reached, as trigger_abort_logic() is expected to abort.
        return 1; // Return non-zero to indicate the abort did not happen.
    }

    std::cout << "--- AtomicGuard Test Suite ---\n";

    TEST_CASE("Basic Acquire/Release", test_basic_acquire_release);
    TEST_CASE("RAII and Token Persistence", test_raii_and_token_persistence);
    TEST_CASE("Concurrent Acquire", test_concurrent_acquire);
    TEST_CASE("Single-Thread Transfer", test_transfer_single_thread);
    TEST_CASE("Cross-Thread Transfer", test_transfer_between_threads);
    TEST_CASE("Transfer Rejects Different Owners", test_transfer_rejects_different_owners);
    TEST_CASE("Destructor Correctly Handles Transferred-From Guard",
              test_destructor_with_transfer);
    TEST_CASE("Attach, Detach, and Attach-Acquire", test_attach_and_detach);
    TEST_CASE("Destructor Correctly Handles No-Op Scenarios",
              test_noop_destructor_scenarios);

    // This test must be handled carefully as it involves process spawning.
    std::string self_exe = argv[0];
    TEST_CASE("Destructor Abort on Invariant Violation",
              [&]() { test_destructor_abort_on_invariant_violation(self_exe); });

    std::cout << "\n--- Test Summary ---\n";
    std::cout << "Passed: " << tests_passed << ", Failed: " << tests_failed << std::endl;

    return tests_failed == 0 ? 0 : 1;
}
