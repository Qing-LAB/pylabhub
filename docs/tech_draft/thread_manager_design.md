# Tech Draft: ThreadManager — Service Module for Per-Owner Thread Lifetime

**Status**: DRAFT 2026-04-15 — locked for execution in L3.γ sprint.
**Branch**: `feature/lua-role-support`
**Cross-ref**: `docs/tech_draft/flexzone_api_design.md` §4 Commit A5b (implementation slot).
**Anchor for shutdown ordering**: `src/utils/service/lifecycle*.cpp` (ModuleDef + LifecycleGuard + timedShutdown).
**Companion HEP**: promotes to HEP-CORE after stabilization (post-L3.γ).

---

## 1. Problem

Across the codebase, every component that spawns background threads (`RoleAPIBase`, `BrokerRequestComm`, `BrokerService`, `ZmqQueue`, `InboxQueue`, `InboxClient`, `Logger`, and the per-role hosts) reinvents the same pattern independently:

1. A stop atomic / stop flag / `std::atomic<bool>` inside the component's Impl.
2. One or more `std::thread` fields inside Impl.
3. A `stop()` method that sets the atomic, signals any condition variable, then joins the thread.

Three recurring defects:

- **Unbounded joins.** Every component calls `std::thread::join()` directly. If the thread body doesn't observe the stop atomic (stuck in a ZMQ poll, a condition-variable wait with a lost wakeup, a robust-mutex acquisition on a dead owner, a blocking IO syscall), the join waits forever. The calling thread enters `futex_wait_queue` and never returns.

- **Silent hangs.** The `join()` is silent — no "still waiting after N seconds" log, no ERROR output, no diagnostic. The caller of stop() blocks with no observable signal. Processes under `ctest` hit the external ctest timeout (60s) and get `SIGKILL`'d externally; operators see a hung role host with nothing in the log explaining why.

- **No escape.** There's no fallback: no `pthread_cancel`, no `detach`, no process-exit path. A single stuck background thread can hold the entire process open.

Evidence from today's session: `BrokerSchemaTest.SchemaHash_StoredOnReg` hung 60s with all 3 threads in `futex_wait_queue`. PID-attach via gdb was blocked by `ptrace_scope`. No log output after "thread started". ctest killed the process externally. Pattern observed across A3/A4/A5 ctest runs on different tests each time.

Separately, `LifecycleGuard`'s `timedShutdown` (`src/utils/service/lifecycle_helpers.cpp:46`) already solves this problem for module shutdown: helper thread + `cv.wait_for(timeout, predicate)` + `thread.detach()` on timeout + `debug_info += "TIMEOUT ({}ms)! Thread detached.\n"`. Per-module timeout is configurable (NativeEngine uses 5000ms). **The mechanism exists; it just isn't used for component-owned background threads.**

## 2. Design

### 2.1 What `ThreadManager` is

A **per-owner utility class** composed by value into each component that owns background threads. One instance per owner. Each instance manages only the threads spawned through itself. No global registry, no singleton, no shared state between instances.

Responsibilities:

- **Spawn**: register a named `std::thread` and start it. Record spawn timestamp for diagnostics.
- **Bounded join**: on destruction, join each managed thread with a per-thread timeout. On timeout: log ERROR, detach, continue.
- **Stop-signal composition** (out of scope): `ThreadManager` does NOT own the stop atomic. The owner component owns its stop atomic and captures it into the thread body lambda. `ThreadManager` handles only the join-half of the shutdown.
- **Observability**: `snapshot()` returns a list of active threads (name, alive, elapsed-since-spawn). Used for stuck-process diagnostics.
- **Lifecycle integration**: each `ThreadManager` instance registers itself as a **dynamic module** with `LifecycleManager` so process-global teardown is driven by the existing LifecycleGuard topological-sort shutdown. The module name includes the owner's tag so it's identifiable in lifecycle logs.

### 2.2 Lifecycle module integration

Each `ThreadManager` instance auto-registers as a dynamic lifecycle module at construction:

```
ThreadManager("prod")                      → module: "ThreadManager:prod"
ThreadManager("BRC:PROD-SENSOR-0001")      → module: "ThreadManager:BRC:PROD-SENSOR-0001"
ThreadManager("BrokerService:tcp://*:5555") → module: "ThreadManager:BrokerService:tcp://*:5555"
ThreadManager("ZmqQueue:lab.sensor.raw")   → module: "ThreadManager:ZmqQueue:lab.sensor.raw"
ThreadManager("InboxQueue:PROD-SENSOR-0001")→ module: "ThreadManager:InboxQueue:PROD-SENSOR-0001"
ThreadManager("Logger")                    → module: "ThreadManager:Logger"
```

The tag (the owner identifier) distinguishes instances. The `"ThreadManager:"` prefix **classifies** the module — operators greppping lifecycle logs see all thread-management-related shutdowns together, filtered from other modules.

Registration is `LifecycleManager::instance().register_dynamic_module(ModuleDef)`. The `ModuleDef`:

- **name**: `"ThreadManager:" + owner_tag`
- **dependency**: `"pylabhub::utils::Logger"` (ThreadManager logs during join, so Logger must be up).
- **startup**: no-op (threads spawn lazily via `spawn()` after construction).
- **shutdown**: calls the instance's `join_all()` — bounded join + detach on timeout + ERROR log.
- **shutdown timeout**: configurable at construction (default `5000ms` aggregate per manager); individual threads within the manager have their own per-spawn timeouts that contribute to the aggregate.

This gives four benefits:

1. **Uniform shutdown timeout enforcement** via the already-existing `LifecycleManagerImpl::shutdownModuleWithTimeout` → `timedShutdown` path. If a ThreadManager's `join_all()` itself hangs (pathological case), the lifecycle layer detaches *it* and logs `ShutdownTimeout`. The hang is bounded at two layers.

2. **Dependency ordering**. Roles depend on BrokerRequestComm + ZmqQueue + InboxQueue for their data plane. With each owning a ThreadManager registered as a dynamic module, the Role's ThreadManager declares dependencies on the others, and `LifecycleGuard` shuts them down in reverse topological order at process exit. No manual "make sure I stop BRC before I stop the role" ordering in destructor bodies.

3. **Observability in standard lifecycle logs**. The existing lifecycle log sink (`LifecycleLogSink`) prints module init/teardown with timing. ThreadManagers show up there identified by owner tag. Grep `ThreadManager:` in the log = all thread-subsystem activity.

4. **Central registration for runtime health endpoints** (future). A `/status` / admin-shell command can enumerate all `ThreadManager:*` dynamic modules and call `snapshot()` to list every active thread in the process with its owner and alive duration. Today each component's threads are invisible to the admin plane.

### 2.3 API

```cpp
// src/include/utils/thread_manager.hpp
#pragma once

#include "pylabhub_utils_export.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::utils
{

class PYLABHUB_UTILS_EXPORT ThreadManager
{
  public:
    struct SpawnOptions {
        std::chrono::milliseconds join_timeout{5000};   // per-thread bounded-join deadline
        // future: cpu_affinity, nice level, thread-priority hints
    };

    struct ThreadInfo {
        std::string                          name;
        bool                                 alive;       // joinable() at snapshot time
        std::chrono::steady_clock::duration  elapsed;     // since spawn()
        std::chrono::milliseconds            join_timeout;
    };

    /// @param owner_tag e.g. "prod", "BRC:PROD-SENSOR-0001", "Logger".
    ///   Becomes the dynamic-module name via "ThreadManager:" + owner_tag.
    /// @param module_shutdown_timeout  lifecycle-layer cap on the aggregate
    ///   join_all() duration. Default 10s (caller should provide).
    explicit ThreadManager(std::string owner_tag,
                           std::chrono::milliseconds module_shutdown_timeout
                               = std::chrono::milliseconds{10000});

    ~ThreadManager();   // calls join_all() + deregisters from LifecycleManager

    ThreadManager(const ThreadManager &) = delete;
    ThreadManager &operator=(const ThreadManager &) = delete;
    ThreadManager(ThreadManager &&) noexcept;
    ThreadManager &operator=(ThreadManager &&) noexcept;

    /// Spawn a named thread. body MUST periodically check the caller's stop
    /// condition (captured by the lambda) and return when shutdown is
    /// requested. ThreadManager does NOT signal the thread — only joins it.
    /// @return true if spawned; false if the manager has already started
    ///   tearing down (post-join_all()).
    bool spawn(const std::string &name,
               std::function<void()> body,
               SpawnOptions opts = {});

    /// Bounded join of all managed threads (reverse spawn order). Each thread
    /// that doesn't exit within its SpawnOptions.join_timeout is detached
    /// with an ERROR log. After return, all managed threads are either
    /// joined cleanly or detached + logged. Idempotent.
    void join_all();

    /// Number of active (joinable) threads.
    size_t active_count() const;

    /// Diagnostic snapshot of all tracked threads. Thread-safe. Useful for
    /// admin-shell / health-check endpoints enumerating all ThreadManager:*
    /// modules at runtime.
    std::vector<ThreadInfo> snapshot() const;

    /// Owner tag (for log formatting and external identification).
    const std::string &owner_tag() const noexcept;

    /// Lifecycle module name: "ThreadManager:" + owner_tag.
    std::string module_name() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::utils
```

### 2.4 Interaction with stop signaling

`ThreadManager` deliberately does not own the stop signal. Each component continues to hold its own atomic/flag/cv for stop notification:

```cpp
// BrokerRequestComm::Impl (illustrative post-migration):
struct BrokerRequestComm::Impl {
    std::atomic<bool>         stop{false};
    std::condition_variable   wakeup_cv;
    // ...

    // Owns its ThreadManager. Tag includes BRC identity.
    utils::ThreadManager threads{"BRC:" + uid};
};

// Thread body captures the stop atomic by reference:
void BrokerRequestComm::start(...)
{
    pImpl->threads.spawn("receiver", [this]() {
        while (!pImpl->stop.load(std::memory_order_acquire)) {
            // ZMQ poll with short timeout so we return to the loop head
            // and re-check the atomic.
            auto n = zmq_poller_wait_all(poller, events.data(), ..., /*timeout_ms=*/100);
            // ... handle events
        }
    });
}

void BrokerRequestComm::stop()
{
    pImpl->stop.store(true, std::memory_order_release);
    pImpl->wakeup_cv.notify_all();
    // pImpl->threads.~ThreadManager() calls join_all() which bounds
    // each thread at its SpawnOptions.join_timeout. Stuck threads
    // get ERROR-logged + detached.
}
```

The stop flag is the signal. The ThreadManager handles the join outcome. Clean separation.

### 2.5 Escape path (detach on timeout)

A detached thread that was stuck in a syscall continues running until process exit. This is a documented resource leak — preferable to a silent process hang. The ERROR log identifies exactly which thread leaked:

```
ERROR [ThreadManager:BRC:PROD-SENSOR-0001] thread 'receiver' did NOT exit within
5000ms — detaching. Shutdown continuing; detached thread may still hold ZMQ
socket state. active_count after this thread = 0.
```

Operators see the leak and can investigate root cause in the underlying component's thread body. Long-running processes that detach repeatedly will eventually exhaust file descriptors or thread quota — that's a visible failure signal, not an invisible hang.

Components that can deterministically unblock their threads (e.g., closing a ZMQ socket wakes the poll loop) should do that in their `stop()` before dropping the ThreadManager. `ThreadManager` is the safety net, not the primary signal.

---

## 3. Migration targets

Order of migration (low-coupling first):

| Commit | Owner | Tag | Threads absorbed |
|---|---|---|---|
| **A5b** | `RoleAPIBase::Impl` | `role_tag` ("prod" / "cons" / "proc") | `managed_threads_` (ctrl, inbox, worker) |
| **A5c** | `BrokerRequestComm::Impl` | `"BRC:" + uid` | receiver thread |
| **A5d** | `BrokerService::Impl` | `"BrokerService:" + endpoint` | accept + worker threads |
| **A5e** | `ZmqQueue::Impl` | `"ZmqQueue:" + name` | send/recv thread |
| **A5f** | `InboxQueue::Impl` + `InboxClient::Impl` | `"InboxQueue:" + name`, `"InboxClient:" + target_uid` | router/dealer threads |
| **A5g** | `Logger::Impl` | `"Logger"` | async log writer thread |

Each commit:
1. Replace ad-hoc `std::thread` + stop atomic in the component's Impl with `utils::ThreadManager threads{tag}`.
2. Delete the ad-hoc `join()` call in the component's `stop()` / dtor — `ThreadManager::~ThreadManager` handles it.
3. Confirm the component's stop signal still unblocks the thread body (the stop atomic is now captured by reference in the body lambda passed to `spawn()`).
4. Tests green + one new test per migration: verify that a deliberately-stuck thread body produces the expected ERROR log + detach behavior and that `stop()` returns within the expected timeout.

### Ordering vs. `hub::Producer`/`hub::Consumer` deletion

- **A5b happens before A6** (class deletion) because the role's shutdown path is the one that needs bounded guarantees first. After A5b the RoleAPIBase thread-shutdown contract is bounded; any subsequent migration can lean on that.
- **A5c–A5f can land in any order** (independent components).
- **A5g (Logger) lands last** because many other modules depend on Logger for their ERROR logs during teardown — migrate Logger only after the rest of the code is already using ThreadManager cleanly.

---

## 4. What this design does NOT do

- **Not a thread pool.** Each `spawn()` is a fresh `std::thread`. Reuse, queueing, scheduling: out of scope. Framework thread counts are in dozens; pooling is a different concern.
- **Not a stop-signal distributor.** Each owner keeps its own stop atomic/cv. ThreadManager never signals; it only joins + detaches.
- **Not a cancellation primitive.** No `pthread_cancel`. Detach-on-timeout is the escape; never kill-on-timeout. Killed threads leave shared-memory mutexes in irrecoverable states.
- **Not a cross-component coordinator.** No inter-manager messaging. Inter-component shutdown ordering uses LifecycleGuard's existing ModuleDef dependency graph.
- **Not a replacement for LifecycleGuard.** LifecycleGuard manages process-level module init/deinit topology. ThreadManager manages per-component threads within a module's lifetime. Each ThreadManager *is* a dynamic lifecycle module so the two systems compose.

## 4a. Future enhancement — explicit intra-manager shutdown order

**Current policy**: `join_all()` joins threads in **reverse spawn order** (last spawned = first joined). For simple producer/consumer pairs where the sender depends on the receiver (or vice-versa), spawning in dependency order and relying on LIFO is sufficient.

**Limitation**: when an owner spawns a set of threads whose shutdown dependencies do **not** match spawn order — e.g., a heartbeat thread spawned first but that should drain last because other threads signal it via enqueue; a receiver thread that must drain before a worker thread that depends on its output — LIFO may join the wrong thread first and either:
- Deadlock (thread A is joining and A depends on B's thread body that still references the owner).
- Detach unnecessarily (thread B hits its timeout while waiting on A which A waits on B — neither signals stop to the other in the right order).

**Proposed extension (not implemented)**: add an optional `shutdown_order` field to `SpawnOptions`, interpreted as a sort key:

```cpp
struct SpawnOptions {
    std::chrono::milliseconds join_timeout{kMidTimeoutMs};
    int shutdown_order{0};   // lower = joined earlier; ties broken by reverse spawn order
};
```

`join_all()` would sort the slot list by `(shutdown_order ascending, spawn_index descending)` before joining. Default `0` preserves today's LIFO behavior; callers that need explicit ordering assign negative values to threads that should drain first (e.g., receivers before workers before heartbeats).

**Alternatives considered**:
- Explicit dependency graph per spawn — too heavy for the ~dozen threads any owner manages in practice.
- Cross-manager dependency graph — already provided by LifecycleGuard's `ModuleDef::add_dependency()`; ThreadManager instances register as dynamic modules so inter-manager ordering is covered at a higher layer. This proposal is strictly for intra-manager ordering.

**When to revisit**: if any migrated component (BrokerService, hub-python HubScript, processor role host) has shutdown dependencies that LIFO can't express. None encountered so far through A5g; shelving until a concrete case motivates it.

---

## 5. Test plan per migration

For each A5b–A5g commit, at minimum one new test proves the bounded-shutdown contract:

```cpp
TEST_F(ThreadManagerTest, StuckThreadDetachesWithinTimeout)
{
    ThreadManager tm("test", std::chrono::milliseconds{200});
    std::atomic<bool> unblock{false};
    tm.spawn("stuck_thread",
             [&] { while (!unblock) std::this_thread::sleep_for(10ms); });

    const auto start = std::chrono::steady_clock::now();
    tm.join_all();   // should return in <=  ~200ms + overhead
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
    EXPECT_EQ(tm.active_count(), 0u);
    // Verify ERROR log emitted (via Logger test capture):
    EXPECT_TRUE(log_capture.contains(
        "thread 'stuck_thread' did NOT exit within 200ms — detaching"));

    unblock.store(true);   // let the detached thread exit cleanly
}

TEST_F(ThreadManagerTest, NormalThreadJoinsCleanly)
{
    ThreadManager tm("test");
    std::atomic<bool> ran{false};
    tm.spawn("fast_thread",
             [&] { ran = true; /* returns immediately */ });

    tm.join_all();
    EXPECT_TRUE(ran.load());
    EXPECT_EQ(tm.active_count(), 0u);
    EXPECT_FALSE(log_capture.contains("did NOT exit"));   // no ERROR
}

TEST_F(ThreadManagerTest, LifecycleModuleRegistration)
{
    ThreadManager tm("prod");
    EXPECT_EQ(tm.module_name(), "ThreadManager:prod");
    EXPECT_EQ(LifecycleManager::instance().dynamic_module_state(tm.module_name()),
              DynModuleState::LOADED);
}
```

---

## 6. Commit sequence (merged into `flexzone_api_design.md` §4)

Insert between current Commit 4 (RoleAPIBase surface cleanup) and Commit 5 (cycle_ops migration):

- **A5b** — introduce `utils::ThreadManager`. Migrate `RoleAPIBase::Impl` to own a `ThreadManager` instance. Existing `spawn_thread(name, body)` API preserved; bounded-join + detach-on-timeout + ERROR log added underneath. Register as dynamic lifecycle module. +2 new tests (bounded-join, clean-exit).
- **A5c** — migrate `BrokerRequestComm::Impl`. +1 test.
- **A5d** — migrate `BrokerService::Impl`. +1 test.
- **A5e** — migrate `ZmqQueue::Impl`. +1 test.
- **A5f** — migrate `InboxQueue::Impl` + `InboxClient::Impl`. +2 tests.
- **A5g** — migrate `Logger::Impl`. +1 test.
- **A6** — (was Commit A in the flexzone plan) — delete `hub::Producer` / `hub::Consumer`. At this point all their internal threads have been migrated to their respective owner's ThreadManager, so their deletion is a pure class-removal.

After A5g, **every** background thread in the process runs under a `ThreadManager` whose bounded-join + detach-on-timeout guarantees a non-silent shutdown. The 60s mystery hangs disappear — they become 5s logged events identifying the stuck thread by owner tag.

---

## 7. Locked decisions

All design decisions above are locked against: existing `LifecycleGuard` / `timedShutdown` mechanism, HEP-CORE-0023 startup coordination, and the session's observation of the 60s silent-hang pattern. This document is the plan of record for execution; no further sign-off gates between A5b–A5g commits.
