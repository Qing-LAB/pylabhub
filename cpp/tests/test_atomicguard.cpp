// test_atomicguard.cpp
// Simple tests for AtomicGuard functionalities: RAII increment/decrement, move semantics,
// try_acquire_if_zero

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

#include "util/AtomicGuard.hpp"

using namespace pylabhub::util;

int main()
{
    std::atomic<int> counter{0};

    // RAII increment
    {
        AtomicGuard<int> g(&counter);
        assert(counter.load() == 1);
        // move semantics
        AtomicGuard<int> g2 = std::move(g);
        assert(g2.active());
    } // destructor should decrement
    assert(counter.load() == 0);

    // Manual increment/decrement
    {
        AtomicGuard<int> g(&counter, /*do_increment*/ false);
        g.increment();
        assert(counter.load() == 1);
        g.decrement();
        assert(counter.load() == 0);
    }

    // try_acquire_if_zero concurrency
    std::atomic<int> c2{0};
    AtomicGuard<int> guard_template(&c2, /*do_increment*/ false);

    // spawn N threads where each thread tries to acquire exclusive (only one should succeed)
    const int N = 8;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < N; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                AtomicGuard<int> g(&c2, /*do_increment*/ false);
                int prev = -1;
                if (g.try_acquire_if_zero(prev))
                {
                    // got it; simulate work
                    ++success_count;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    // guard releases when it goes out of scope
                }
                else
                {
                    // didn't get it
                }
            });
    }

    for (auto &t : threads)
        t.join();

    // Only one thread should have succeeded
    if (success_count.load() != 1)
    {
        std::cerr << "AtomicGuard concurrency test failed: success_count=" << success_count.load()
                  << std::endl;
        return 2;
    }

    std::cout << "test_atomicguard: OK\n";
    return 0;
}
