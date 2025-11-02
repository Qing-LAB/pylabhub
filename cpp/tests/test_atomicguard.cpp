// test_atomicguard_token.cpp
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "util/AtomicGuard.hpp"

using namespace pylabhub::util;

int main()
{
    std::cout << "test_atomicguard_token: start\n";

    // Simple acquire/release test
    {
        AtomicOwner owner;
        AtomicGuard g(&owner);
        assert(!g.active());
        bool ok = g.acquire();
        assert(ok);
        assert(g.active());
        // release explicitly
        bool rel = g.release();
        assert(rel);
        assert(!g.active());
        // release again should be false
        assert(!g.release());
    }

    // Destructor releases: acquire and let destructor release
    {
        AtomicOwner owner;
        {
            AtomicGuard g(&owner);
            assert(g.acquire());
            assert(g.active());
        } // g destroyed -> should release
        assert(owner.is_free());
    }

    // Move semantics: move-construct
    {
        AtomicOwner owner;
        AtomicGuard g(&owner);
        assert(g.acquire());
        assert(g.active());
        uint64_t token_before = g.token();
        AtomicGuard g2 = std::move(g);
        // moved-from g should be inert:
        assert(!g.active());
        assert(!g.token());
        // moved-to g2 should be active and hold the same token
        assert(g2.active());
        assert(g2.token() == token_before);
        // g2 destructor will release
    }
    // Confirm the owner released
    {
        AtomicOwner owner;
        AtomicGuard a(&owner);
        assert(a.acquire());
        AtomicGuard b;
        b = std::move(a); // move assignment
        assert(!a.active());
        assert(b.active());
        // release via destructor of b
    }

    // attach/detach semantics
    {
        AtomicOwner owner1, owner2;
        AtomicGuard g(&owner1);
        assert(g.acquire());
        assert(g.active());
        // detach -> should release and become detached
        g.detach();
        assert(!g.active());
        assert(owner1.is_free());
        // attach to different owner (no acquire)
        g.attach(&owner2);
        assert(!g.active());
        // acquire on new owner
        assert(g.acquire());
        assert(g.active());
        // detach will release
        g.detach();
        assert(owner2.is_free());
    }

    // Concurrent contention: only one thread may acquire
    {
        AtomicOwner owner;
        const int N = 16;
        std::atomic<int> success_count{0};
        std::vector<std::thread> threads;
        threads.reserve(N);

        for (int i = 0; i < N; ++i)
        {
            threads.emplace_back(
                [&owner, &success_count, i]()
                {
                    // each thread uses its own guard attached to the same owner
                    AtomicGuard g(&owner);
                    if (g.acquire())
                    {
                        // we got it
                        success_count.fetch_add(1, std::memory_order_relaxed);
                        // hold it a bit to make the race visible
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                        // destructor or release will clear it
                    }
                    else
                    {
                        // didn't get it
                    }
                });
        }

        for (auto &t : threads)
            t.join();

        int succeeded = success_count.load(std::memory_order_relaxed);
        if (succeeded != 1)
        {
            std::cerr << "FAILED: expected 1 success, got " << succeeded << "\n";
            return 2;
        }
        else
        {
            std::cout << "concurrency: exactly one thread acquired as expected\n";
        }
        assert(owner.is_free()); // should be released now
    }

    // Stress test: many repeated acquires/releases concurrently
    {
        AtomicOwner owner;
        const int THREADS = 8;
        const int ROUNDS = 1000;
        std::atomic<int> global_acquires{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t)
        {
            threads.emplace_back(
                [&owner, &global_acquires, ROUNDS]()
                {
                    for (int r = 0; r < ROUNDS; ++r)
                    {
                        AtomicGuard g(&owner);
                        // spin a bit trying to acquire
                        for (int attempts = 0; attempts < 10; ++attempts)
                        {
                            if (g.acquire())
                            {
                                global_acquires.fetch_add(1, std::memory_order_relaxed);
                                // short hold
                                std::this_thread::sleep_for(std::chrono::microseconds(10));
                                break;
                            }
                            // small backoff
                            std::this_thread::yield();
                        }
                    }
                });
        }
        for (auto &th : threads)
            th.join();

        std::cout << "stress: total successful acquires (some may be repeated): "
                  << global_acquires.load() << "\n";

        // invariant: owner must be free at end
        assert(owner.is_free());
    }

    std::cout << "test_atomicguard_token: OK\n";
    return 0;
}
