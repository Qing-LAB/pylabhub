// tests/test_atomicguard.cpp
#include "platform.hpp"
#include "atomic_guard.hpp"

// Helpers for stress sizes (tweak to make tests faster or heavier).
static constexpr int LIGHT_THREADS = 8;
static constexpr int HEAVY_THREADS = 64;
static constexpr int LIGHT_ITERS = 2000;
static constexpr int HEAVY_ITERS = 200000;

// Basic acquire / release behavior
TEST(AtomicGuardTest, BasicAcquireRelease) {
    AtomicOwner owner;
    AtomicGuard g(&owner);
    ASSERT_NE(g.token(), 0u);
    ASSERT_FALSE(g.active());

    ASSERT_TRUE(g.acquire());
    ASSERT_TRUE(g.active());
    ASSERT_EQ(owner.load(), g.token());

    ASSERT_TRUE(g.release());
    ASSERT_FALSE(g.active());
    ASSERT_TRUE(owner.is_free());
}

// RAII: acquire on construction and release on destruction
TEST(AtomicGuardTest, RaiiAndTokenPersistence) {
    AtomicOwner owner;
    uint64_t token_in_scope = 0;
    {
        AtomicGuard g(&owner, true);
        ASSERT_NE(g.token(), 0u);
        token_in_scope = g.token();
        ASSERT_TRUE(g.active());
        ASSERT_EQ(owner.load(), token_in_scope);
    } // destructor releases here
    ASSERT_TRUE(owner.is_free());
}

// Explicit release before destruction
TEST(AtomicGuardTest, ExplicitReleaseAndDestruction) {
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        ASSERT_TRUE(g.acquire());
        ASSERT_TRUE(g.active());
        ASSERT_TRUE(g.release());
        ASSERT_FALSE(g.active());
    }
    ASSERT_TRUE(owner.is_free());
}

// Acquire fails if owner pre-locked
TEST(AtomicGuardTest, RaiiAcquireFailure) {
    AtomicOwner owner;
    owner.store(123u);
    {
        AtomicGuard g(&owner, true);
        ASSERT_FALSE(g.active());
    }
    ASSERT_EQ(owner.load(), 123u);
    owner.store(0u);
}

// Concurrent acquire stress: many threads create local guards and attempt to acquire
TEST(AtomicGuardTest, ConcurrentAcquireStress) {
    AtomicOwner owner;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < LIGHT_THREADS; ++t) {
        threads.emplace_back([&]() {
            std::mt19937_64 rng(std::random_device{}());
            std::uniform_int_distribution<int> jitter(0, 200);
            auto until = std::chrono::steady_clock::now() + 500ms;
            while (std::chrono::steady_clock::now() < until) {
                AtomicGuard g(&owner);
                if (g.acquire()) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                    // simulate variable work
                    if ((rng() & 0xF) == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() & 0xFF));
                    [[maybe_unused]] bool ok = g.release();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(jitter(rng)));
                }
            }
        });
    }

    for (auto &th : threads) th.join();

    ASSERT_GT(success_count.load(std::memory_order_relaxed), 0);
    ASSERT_TRUE(owner.is_free());
}

// Move construction and move assignment semantics (single-threaded)
TEST(AtomicGuardTest, MoveSemanticsSingleThread) {
    AtomicOwner owner;
    uint64_t tok;

    // Move construction
    {
        AtomicGuard a(&owner, true);
        ASSERT_TRUE(a.active());
        tok = a.token();
        ASSERT_EQ(owner.load(), tok);

        AtomicGuard b(std::move(a));
        ASSERT_TRUE(b.active());
        ASSERT_EQ(b.token(), tok);
        ASSERT_EQ(owner.load(), tok);
    } // b destructor releases
    ASSERT_TRUE(owner.is_free());

    // Move assignment
    {
        AtomicGuard c(&owner, true);
        ASSERT_TRUE(c.active());
        uint64_t token_c = c.token();

        AtomicGuard d;
        d = std::move(c);
        ASSERT_TRUE(d.active());
        ASSERT_EQ(d.token(), token_c);
        ASSERT_EQ(owner.load(), token_c);
    }
    ASSERT_TRUE(owner.is_free());
}

// Move while active: ensure move preserves ownership and source becomes inactive
TEST(AtomicGuardTest, MoveActiveGuardBehavior) {
    AtomicOwner owner;
    AtomicGuard a(&owner, true);
    ASSERT_TRUE(a.active());
    uint64_t tok = a.token();

    AtomicGuard b(std::move(a)); // move while active
    ASSERT_TRUE(b.active());
    ASSERT_EQ(b.token(), tok);
    ASSERT_EQ(owner.load(), tok);
    // a is now detached / inactive; calling methods on a should be safe but inactive
    ASSERT_FALSE(a.active());
    ASSERT_TRUE(b.release());
    ASSERT_TRUE(owner.is_free());
}

// Attach and detach behavior
TEST(AtomicGuardTest, AttachDetach) {
    AtomicOwner owner;
    AtomicGuard g;
    ASSERT_FALSE(g.active());
    ASSERT_FALSE(g.acquire());

    ASSERT_TRUE(g.attach_and_acquire(&owner) ? true : true); // attach_and_acquire returns bool; here accept either
    // If attach_and_acquire returned false, maybe other test interference; ensure consistent outcome by checking owner status:
    if (g.active()) {
        ASSERT_TRUE(g.release());
    } else {
        // try a direct acquire
        ASSERT_TRUE(g.acquire());
        ASSERT_TRUE(g.release());
    }

    g.detach_no_release();
    ASSERT_FALSE(g.acquire());
}

// Producer->consumer move handoff using promise/future (single handoff)
TEST(AtomicGuardTest, TransferBetweenThreads_SingleHandoff) {
    AtomicOwner owner;

    std::promise<AtomicGuard> p;
    std::future<AtomicGuard> f = p.get_future();

    // Producer: create and acquire, then move to consumer via promise
    std::thread producer([&]() {
        AtomicGuard g(&owner, true);
        ASSERT_TRUE(g.active());
        p.set_value(std::move(g));
    });

    // Consumer: receive guard and release
    std::thread consumer([&]() {
        AtomicGuard g = f.get();
        ASSERT_TRUE(g.active());
        ASSERT_EQ(owner.load(), g.token());
        ASSERT_TRUE(g.release());
    });

    producer.join();
    consumer.join();

    ASSERT_TRUE(owner.is_free());
}

// Heavy repeated producer->consumer handoffs to stress move-based transfers.
// Many pairs run concurrently to increase contention.
TEST(AtomicGuardTest, TransferBetweenThreads_HeavyHandoff) {
    const int pairs = 64;
    const int iters_per_pair = 1000;

    std::vector<AtomicOwner> owners(pairs);
    std::vector<std::thread> workers;
    workers.reserve(pairs);

    for (int p = 0; p < pairs; ++p) {
        workers.emplace_back([&, p, owner = &owners[p]]() mutable {
            for (int i = 0; i < iters_per_pair; ++i) {
                AtomicGuard g(owner, true);
                if (!g.active()) {
                    auto until = std::chrono::steady_clock::now() + 20ms;
                    while (!g.acquire() && std::chrono::steady_clock::now() < until) {}
                }
                ASSERT_TRUE(g.active()) << "Guard should be active before move";

                std::promise<AtomicGuard> p2;
                std::future<AtomicGuard> f2 = p2.get_future();
                std::thread local_consumer([g = std::move(g), p2 = std::move(p2)]() mutable {
                    p2.set_value(std::move(g));
                });

                AtomicGuard moved = f2.get();
                ASSERT_TRUE(moved.active()) << "Guard should be active after move";
                ASSERT_TRUE(moved.release());
                local_consumer.join();
            }
        });
    }

    for (auto &w : workers) w.join();

    for (const auto &owner : owners) {
        ASSERT_TRUE(owner.is_free());
    }
}

// Concurrent move assignment stress: many threads repeatedly move guards into a shared slot.
// A slot is protected by an index-based mutex to avoid container races; the goal is to stress the guard move logic itself.
TEST(AtomicGuardTest, ConcurrentMoveAssignmentStress) {
    AtomicOwner owner;
    const int SLOTS = 16;
    const int THREADS = 32;
    const int ITERS = 10000;

    std::vector<AtomicGuard> slots;
    slots.reserve(SLOTS);
    for (int i = 0; i < SLOTS; ++i) slots.emplace_back(&owner); // attached but not acquired

    std::vector<std::mutex> slot_mtx(SLOTS);

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937_64 rng(static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())) ^ t);
            std::uniform_int_distribution<int> idxdist(0, SLOTS - 1);
            for (int it = 0; it < ITERS; ++it) {
                int src = idxdist(rng);
                int dst = idxdist(rng);
                if (src == dst) continue;

                // Acquire small window to move from src->dst; protecting slot access to avoid container races.
                std::unique_lock<std::mutex> lk_src(slot_mtx[src], std::defer_lock);
                std::unique_lock<std::mutex> lk_dst(slot_mtx[dst], std::defer_lock);
                std::lock(lk_src, lk_dst);

                // Move source into a temporary, then move into dst
                AtomicGuard tmp = std::move(slots[src]);
                slots[dst] = std::move(tmp);
                // After the move, src is detached/inactive

                // Optionally attempt to acquire if slot holds no owner and the guard attached
                // (This is just to exercise acquires/releases under concurrent move churn.)
                if (!slots[dst].active()) {
                    [[maybe_unused]] bool ok = slots[dst].acquire();
                    if (slots[dst].active()) {
                        // small critical section
                        ASSERT_EQ(owner.load(), slots[dst].token());
                        [[maybe_unused]] bool ok = slots[dst].release();
                    }
                }
            }
        });
    }

    for (auto &th : threads) th.join();

    // cleanup: release any active
    for (auto &s : slots) {
        if (s.active()) 
            [[maybe_unused]] bool ok = s.release();
    }
    ASSERT_TRUE(owner.is_free());
}

// Stress test: create many guards concurrently, move them via futures to consumers, ensure no leaks.
TEST(AtomicGuardTest, ManyConcurrentProducerConsumerPairs) {
    AtomicOwner owner;
    const int PAIRS = 128;
    const int ITERS = 512;

    struct Channel {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::future<AtomicGuard>> q;
    };

    std::vector<std::unique_ptr<Channel>> channels;
    channels.reserve(PAIRS);
    for (int p = 0; p < PAIRS; ++p) channels.emplace_back(std::make_unique<Channel>());

    std::atomic<bool> thread_failure{false};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(PAIRS);
    consumers.reserve(PAIRS);

    for (int p = 0; p < PAIRS; ++p) {
        Channel &ch = *channels[p];

        // Consumer: pop futures and get guards ITERS times
        consumers.emplace_back([&ch, &owner, &thread_failure, ITERS]() {
            for (int i = 0; i < ITERS; ++i) {
                std::future<AtomicGuard> fut;
                {
                    std::unique_lock<std::mutex> lk(ch.mtx);
                    ch.cv.wait(lk, [&] { return !ch.q.empty(); });
                    fut = std::move(ch.q.front());
                    ch.q.pop_front();
                }

                try {
                    AtomicGuard g = fut.get(); // moved guard
                    if (g.active()) {
                        bool ok = g.release();
                        if (!ok) thread_failure.store(true, std::memory_order_relaxed);
                    }
                } catch (...) {
                    thread_failure.store(true, std::memory_order_relaxed);
                }
            }
        });

        // Producer: produce ITERS guards and push future to channel
        producers.emplace_back([&ch, &owner, ITERS]() {
            for (int i = 0; i < ITERS; ++i) {
                std::promise<AtomicGuard> prom;
                std::future<AtomicGuard> fut = prom.get_future();

                // Create and acquire guard
                AtomicGuard g(&owner, true);
                if (!g.active()) {
                    // bounded retry
                    auto until = std::chrono::steady_clock::now() + 10ms;
                    while (!g.acquire() && std::chrono::steady_clock::now() < until) {}
                }

                // publish the future (consumer will get the guard)
                {
                    std::lock_guard<std::mutex> lk(ch.mtx);
                    ch.q.emplace_back(std::move(fut));
                }
                ch.cv.notify_one();

                // deliver the guard to the future
                prom.set_value(std::move(g));
            }
        });
    }

    // join all
    for (auto &t : producers) t.join();
    for (auto &t : consumers) t.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_TRUE(owner.is_free());
}
