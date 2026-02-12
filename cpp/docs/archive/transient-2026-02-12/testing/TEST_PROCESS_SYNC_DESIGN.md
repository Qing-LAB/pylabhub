# Test Process Synchronization Design

## Overview

This document describes the design for reliable parent–child process synchronization in multi-process tests. It replaces sleep-based coordination with explicit IPC (pipe, semaphore, or kernel mutex) to ensure deterministic ordering and avoid flakiness on slow CI or varying machine load.

## Motivation

Several tests rely on process ordering:

1. **DataBlockMutex: AttacherAcquiresAfterCreator** – Creator must create the mutex and hold it long enough for the attacher to attach. Sleep-based timing is brittle.
2. **DataBlockMutex: ZombieOwnerRecovery** – Zombie exits; recoverer attaches. A short sleep is used to “let the OS mark the mutex abandoned,” which may be unnecessary or insufficient on some platforms.
3. **Slot protocol: ZombieWriterRecovery** – Zombie acquires a write slot and `_exit(0)`s; reclaimer attaches and force-reclaims. Parent waits for zombie exit before spawning reclaimer; no cross-process sync needed today, but timing assumptions exist.
4. **Slot protocol: CrossProcessDataExchangeWriterThenReaderVerifiesContent** – Writer and reader run concurrently; writer sleeps 3s so shm persists. Sleep-based, not sync-based.

Sleep-based coordination fails when:
- CI is slow or overloaded
- Different platforms have different scheduling
- Tests become flaky and hard to debug

## Proposed Approach: Process Ready Signal

### Design

Add a **ready signal** mechanism:

1. **Parent** creates a sync primitive (pipe or named semaphore) before spawning the child.
2. **Child** receives a handle/fd or semaphore name via argv or environment.
3. **Child** signals “ready” (e.g., writes one byte to pipe, or posts semaphore) when it has completed its init phase.
4. **Parent** blocks on the sync primitive until the child signals, then proceeds.

This gives deterministic ordering without sleeps.

### Platform Options

| Platform | Recommended primitive | Notes |
|----------|------------------------|-------|
| POSIX    | Pipe (inherited fd)    | Child inherits write end; writes when ready; parent reads. No extra syscalls. |
| POSIX    | Named semaphore        | `sem_open` + `sem_post` / `sem_wait`. Good for unrelated processes. |
| Windows  | Pipe (inherited handle)| `CreatePipe` + `bInheritHandles=TRUE`; child writes when ready. |
| Windows  | Named mutex / event    | `CreateEvent` + `SetEvent` / `WaitForSingleObject`. |

**Recommendation:** Use **pipe** on both platforms. The child inherits the write fd/handle; parent blocks on read. Simple, portable, no named objects to clean up.

### API Sketch

```cpp
// In test_process_utils.h / test_patterns.h
class WorkerProcessWithSync {
public:
    WorkerProcessWithSync(const std::string& exe_path, const std::string& scenario,
                          const std::vector<std::string>& args);
    void wait_for_ready();  // Blocks until child signals ready
    int wait_for_exit();
    // ... same as WorkerProcess
};

// Or extend WorkerProcess:
WorkerProcess SpawnWorkerWithReadySignal(const std::string& scenario,
                                         std::vector<std::string> args = {});
// Returns WorkerProcess; caller must call wait_for_ready() before proceeding
```

Worker convention: if `PLH_TEST_READY_FD` (POSIX) or `PLH_TEST_READY_HANDLE` (Windows) is set, the worker writes one byte to that fd when init is complete. The test framework creates the pipe, sets the env var (or passes fd number in args), and blocks on read.

### Worker Changes

Workers that need to signal “ready” would:

1. Check for `PLH_TEST_READY_FD` (or equivalent) in environment or argv.
2. After init (e.g., mutex created and locked, or DataBlock created), write one byte to that fd.
3. Proceed with the test logic.

Example (DataBlockMutex creator):

```cpp
// In acquire_and_release_creator_hold_long:
pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);
{
    pylabhub::hub::DataBlockLockGuard lock(mutex);
    log("Mutex acquired");
    signal_test_ready();  // Writes to PLH_TEST_READY_FD if set
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
```

The attacher would wait for the parent to spawn it; the parent would `wait_for_ready()` on the creator before spawning the attacher. So the flow becomes: spawn creator → creator creates and locks → creator signals ready → parent reads (unblocks) → parent spawns attacher → attacher attaches (creator still holds lock) → creator releases after 300ms → attacher acquires. No sleep-based “let creator create first” in the attacher.

## Implementation Phases

1. **Phase 1 (done):** Added `SpawnWorkerWithReadySignal` + pipe plumbing to `test_process_utils`. Workers call `signal_test_ready()` when init is complete; parent blocks on `wait_for_ready()`.
2. **Phase 2 (done):** Migrated DataBlockMutex `AttacherAcquiresAfterCreator` to use ready signal.
3. **Phase 3:** Migrate other tests that use sleep-based coordination (ZombieOwnerRecovery, CrossProcessDataExchange, etc.) as needed.

## DataBlockConsumer Single-Thread Requirement

`DataBlockConsumer` is **not** thread-safe. Its `last_consumed_slot_id` is shared state that tracks the consumer’s position in the stream. Multiple threads calling `acquire_consume_slot()` on the same instance cause data races.

See `data_block.hpp` class documentation. Use a single consumer thread, or one consumer per thread with explicit slot distribution logic.

## References

- `tests/test_framework/test_process_utils.cpp` – current spawn/wait implementation
- `tests/test_layer3_datahub/workers/datablock_management_mutex_workers.cpp` – workers using sleep-based sync
- `cpp/src/include/utils/data_block.hpp` – DataBlockConsumer thread-safety note
