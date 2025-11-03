// test_atomic_guard.cpp
// Build:
//   g++ -std=c++17 -O2 -pthread test_atomic_guard.cpp -o test_atomic_guard
//
// Run:
//   ./test_atomic_guard
//
// This test suite uses only assertions and prints progress; it requires that
// AtomicGuard.hpp is the version we discussed (persistent tokens, lock-free
// acquire/release, scoped_lock transfer_to, guard_mutex accessor).

#include "util/AtomicGuard.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace pylabhub::util;
using namespace std::chrono_literals;

static void header(const char *s)
{
    std::cout << "\n=== " << s << " ===\n";
}

// Test 1: Many threads try to acquire the same AtomicOwner concurrently.
void test_concurrent_acquire_release()
{
    header("test_concurrent_acquire_release");
    AtomicOwner owner;
    constexpr int THREADS = 8;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&owner, &success_count]()
            {
                AtomicGuard g(&owner);
                // token should be non-zero
                assert(g.token() != 0);

                auto until = std::chrono::steady_clock::now() + 500ms;
                while (std::chrono::steady_clock::now() < until)
                {
                    if (g.acquire())
                    {
                        ++success_count;
                        assert(g.active());
                        std::this_thread::sleep_for(3ms);
                        bool r = g.release();
                        assert(r);
                    }
                    else
                    {
                        std::this_thread::sleep_for(1ms);
                    }
                }
            });
    }

    for (auto &t : threads)
        t.join();

    std::cout << "success_count: " << success_count.load() << "\n";
    assert(success_count.load() > 0);
    std::cout << "test_concurrent_acquire_release: passed\n";
}

// Test 2: RAII behavior and token persistence
void test_raii_and_token_persistence()
{
    header("test_raii_and_token_persistence");
    AtomicOwner owner;
    uint64_t token_in_scope = 0;
    {
        AtomicGuard g(&owner);
        assert(g.token() != 0);
        token_in_scope = g.token();
        bool ok = g.acquire();
        assert(ok);
        assert(g.active());
        uint64_t cur = owner.load();
        assert(cur == token_in_scope);
    } // destructor releases
    assert(owner.is_free());
    std::cout << "token (persistent per guard): " << token_in_scope << "\n";
    std::cout << "test_raii_and_token_persistence: passed\n";
}

// Test 3: single-thread transfer_to
void test_transfer_single_thread()
{
    header("test_transfer_single_thread");
    AtomicOwner owner;
    AtomicGuard a(&owner);
    AtomicGuard b(&owner);

    assert(a.token() != 0 && b.token() != 0);
    bool ok = a.acquire();
    assert(ok);
    assert(a.active());
    assert(!b.active());

    bool t = a.transfer_to(b);
    assert(t);
    assert(!a.active());
    assert(b.active());

    bool rel = b.release();
    assert(rel);
    assert(owner.is_free());
    std::cout << "test_transfer_single_thread: passed\n";
}

// Test 4: transfer in threads
void test_transfer_between_threads()
{
    header("test_transfer_between_threads");
    AtomicOwner owner;
    AtomicGuard src(&owner);
    AtomicGuard dst(&owner);

    bool ok = src.acquire();
    assert(ok);

    std::thread transferer(
        [&src, &dst]()
        {
            // attempt transfer; allow retries for transient contention
            for (int i = 0; i < 50; ++i)
            {
                if (src.transfer_to(dst))
                    return; // success
                std::this_thread::sleep_for(1ms);
            }
            assert(false && "transfer_to failed in worker");
        });

    transferer.join();

    assert(!src.active());
    assert(dst.active());
    bool rel = dst.release();
    assert(rel);
    assert(owner.is_free());
    std::cout << "test_transfer_between_threads: passed\n";
}

// Test 5: transfer_to rejects different owners
void test_transfer_reject_different_owner()
{
    header("test_transfer_reject_different_owner");
    AtomicOwner o1, o2;
    AtomicGuard a(&o1);
    AtomicGuard b(&o2);

    bool ok = a.acquire();
    assert(ok);
    bool t = a.transfer_to(b);
    assert(!t); // must reject
    assert(a.active());
    bool r = a.release();
    assert(r);
    assert(o1.is_free());
    assert(o2.is_free());
    std::cout << "test_transfer_reject_different_owner: passed\n";
}

// Test 6: demonstrate consistent active() by locking guard_mutex()
// (advanced usage example)
void test_consistent_active_with_mutex()
{
    header("test_consistent_active_with_mutex");
    AtomicOwner owner;
    AtomicGuard a(&owner);
    AtomicGuard b(&owner);

    bool ok = a.acquire();
    assert(ok);

    bool t = a.transfer_to(b);
    assert(t);

    // Without locking, active() might be observed transiently; to get a stable view,
    // lock the guard mutex before checking active().
    {
        std::lock_guard<std::mutex> lk(b.guard_mutex());
        bool active_now = b.active();
        std::cout << "b.active() under guard_mutex = " << active_now << "\n";
        assert(active_now);
    }

    // cleanup
    bool r = b.release();
    assert(r);
    assert(owner.is_free());
    std::cout << "test_consistent_active_with_mutex: passed\n";
}

int main()
{
    std::cout << "AtomicGuard tests starting...\n";

    test_concurrent_acquire_release();
    test_raii_and_token_persistence();
    test_transfer_single_thread();
    test_transfer_between_threads();
    test_transfer_reject_different_owner();
    test_consistent_active_with_mutex();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
