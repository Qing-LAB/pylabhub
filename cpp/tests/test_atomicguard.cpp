// test_atomicguard.cpp
//
// This test is part of the project's CMake build system.
// It should be built via the `tests` subdirectory, which links it against
// the `pylabhub-utils` shared library.
//
// Manual build example (from the `build` directory):
//   g++ -std=c++17 -O2 -pthread ../tests/test_atomicguard.cpp
//       -I../include -L./src/utils -lpylabhub-utils -o test_atomicguard
//
// Run:
//   # After building and staging:
//   ./build/stage/bin/test_atomicguard

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

#include "platform.hpp"

#if defined(PLATFORM_WIN64)
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
        if (!(condition))                                                                          \
        {                                                                                          \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\n", #condition, __FILE__, __LINE__);    \
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

void test_explicit_release_and_destruction()
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        CHECK(g.acquire());
        CHECK(g.active());
        CHECK(g.release());
        CHECK(!g.active());
        // g's destructor runs here. It should not PANIC because its internal
        // is_active_ flag was set to false by the successful release().
    }
    CHECK(owner.is_free());
    // If we get here, the test passed (no abort).
}

void test_raii_acquire_failure()
{
    AtomicOwner owner;
    owner.store(123); // Lock is already held by someone else.
    {
        AtomicGuard g(&owner, true); // tryAcquire will fail.
        CHECK(!g.active());
    } // Destructor runs, should be a no-op.
    CHECK(owner.load() == 123);
    owner.store(0); // Clean up.
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

void test_concurrent_transfers()
{
    AtomicOwner owner;
    constexpr int NUM_GUARDS = 4;
    std::vector<AtomicGuard> guards;
    for (int i = 0; i < NUM_GUARDS; ++i)
    {
        guards.emplace_back(&owner);
    }

    CHECK(guards[0].acquire()); // Start with guard 0 as owner.

    constexpr int NUM_THREADS = 8;
    constexpr int TRANSFERS_PER_THREAD = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back([&]() {
            for (int j = 0; j < TRANSFERS_PER_THREAD; ++j)
            {
                // Transfer between pseudo-random pairs to stress the locks.
                int src_idx = (j * 3 + i) % NUM_GUARDS;
                int dest_idx = (j * 5 + i + 1) % NUM_GUARDS;
                if (src_idx == dest_idx) continue;

                // Intentionally ignore the [[nodiscard]] return value. In this
                // concurrent stress test, many transfers are expected to fail.
                // The goal is to ensure atomicity, not to check every transfer.
                (void)guards[src_idx].transfer_to(guards[dest_idx]);
            }
        });
    }

    for (auto &t : threads) { t.join(); }

    // After all the transfers, exactly one guard should be active.
    int active_count = 0;
    for (const auto &g : guards) { if (g.active()) { active_count++; } }
    CHECK(active_count == 1);
    CHECK(owner.load() != 0);

    // Clean up by releasing the final owner.
    for (auto &g : guards) { if (g.active()) { CHECK(g.release()); } }
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

void test_detach_and_destruction()
{
    AtomicOwner owner;
    uint64_t leaked_token = 0;
    {
        AtomicGuard g(&owner, true);
        CHECK(g.active());
        leaked_token = g.token();

        // Detach while holding the lock. This is a deliberate leak.
        g.detach_no_release();

        // The guard is no longer attached. Its destructor should be a no-op
        // regarding the lock because its owner pointer is null.
    } // Destructor runs here. Should not PANIC.

    CHECK(owner.load() == leaked_token); // The lock is leaked, as expected.
    owner.store(0);                      // Manually clean up for subsequent tests.
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
    { // The internal `is_active_` flag will be false.
        AtomicGuard g(&owner);
        CHECK(!g.active());
    } // Destructor runs here.
    CHECK(owner.is_free());
}

void test_atomicowner_move_semantics()
{
    uint64_t initial_state = 999;
    // Test move construction
    {
        AtomicOwner o1(initial_state);
        CHECK(o1.load() == initial_state);

        AtomicOwner o2(std::move(o1));
        CHECK(o2.load() == initial_state);
        // o1 is now in a valid but unspecified (moved-from) state.
        // Its destructor will run, but we should not call other methods on it.
    }

    // Test move assignment
    {
        AtomicOwner o3(initial_state);
        CHECK(o3.load() == initial_state);

        AtomicOwner o4;
        o4 = std::move(o3);
        CHECK(o4.load() == initial_state);
        // o3 is now in a moved-from state.
    }
}

void test_atomicguard_move_semantics()
{
    AtomicOwner owner;
    uint64_t token_a = 0;

    // Test move construction
    {
        AtomicGuard a(&owner, true);
        CHECK(a.active());
        token_a = a.token();
        CHECK(owner.load() == token_a);

        AtomicGuard b(std::move(a)); // Move constructor
        CHECK(b.active());
        CHECK(b.token() == token_a);
        CHECK(owner.load() == token_a);
    }
    // `b` goes out of scope, releasing the lock.
    CHECK(owner.is_free());

    // Test move assignment
    {
        AtomicGuard c(&owner, true);
        CHECK(c.active());
        uint64_t token_c = c.token();

        AtomicGuard d;      // Default constructed
        d = std::move(c); // Move assignment

        CHECK(d.active());
        CHECK(d.token() == token_c);
        CHECK(owner.load() == token_c);
    }
    // `d` goes out of scope, releasing the lock.
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
            fmt::print(stderr, "ABORT_TEST: Failed to acquire lock initially.\n");
            exit(5);
        }

        // Simulate another entity "stealing" the lock by directly manipulating the owner state.
        owner.store(12345); // Some other non-zero token

    } // g's destructor runs here. It will find:
      // 1. It believes it is active (`is_active_` is true).
      // 2. It cannot release the lock because the owner state is not its token.
      // This should trigger the std::abort().
}

#if defined(PLATFORM_WIN64)
static HANDLE spawn_child_process(const std::string &exe, const std::string &arg)
{
    std::string cmdline = fmt::format("\"{}\" {}", exe, arg);
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
    fmt::print("  Spawning child process to test abort condition...\n");
#if defined(PLATFORM_WIN64)
    HANDLE hProcess = spawn_child_process(self_exe, "trigger_abort");
    CHECK(hProcess != nullptr);

    WaitForSingleObject(hProcess, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(hProcess, &exit_code);
    CloseHandle(hProcess);

    // On Windows, abort() typically results in exit code 3.
    // We check for a non-zero exit code, as specific codes can vary.
    CHECK(exit_code != 0);
    fmt::print("  Child process exited with code {} (expected non-zero for abort).\n", exit_code);
#else
    pid_t pid = spawn_child_process(self_exe, "trigger_abort");
    CHECK(pid > 0);
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    fmt::print("  Child process terminated by signal {} (expected SIGABRT).\n", WTERMSIG(status));
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

    fmt::print("--- AtomicGuard Test Suite ---\n");

    TEST_CASE("Basic Acquire/Release", test_basic_acquire_release);
    TEST_CASE("Explicit Release then Destruction", test_explicit_release_and_destruction);
    TEST_CASE("RAII and Token Persistence", test_raii_and_token_persistence);
    TEST_CASE("RAII Acquire Failure", test_raii_acquire_failure);
    TEST_CASE("Concurrent Acquire", test_concurrent_acquire);
    TEST_CASE("Single-Thread Transfer", test_transfer_single_thread);
    TEST_CASE("Concurrent Transfers", test_concurrent_transfers);
    TEST_CASE("Cross-Thread Transfer", test_transfer_between_threads);
    TEST_CASE("Transfer Rejects Different Owners", test_transfer_rejects_different_owners);
    TEST_CASE("Destructor Correctly Handles Transferred-From Guard",
              test_destructor_with_transfer);
    TEST_CASE("Attach, Detach, and Attach-Acquire", test_attach_and_detach);
    TEST_CASE("Detach while Active and Destruction", test_detach_and_destruction);
    TEST_CASE("Destructor Correctly Handles No-Op Scenarios",
              test_noop_destructor_scenarios);
    TEST_CASE("AtomicOwner Move Semantics", test_atomicowner_move_semantics);
    TEST_CASE("AtomicGuard Move Semantics", test_atomicguard_move_semantics);

    // This test must be handled carefully as it involves process spawning.
    std::string self_exe = argv[0];
    TEST_CASE("Destructor Abort on Invariant Violation",
              [&]() { test_destructor_abort_on_invariant_violation(self_exe); });

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
