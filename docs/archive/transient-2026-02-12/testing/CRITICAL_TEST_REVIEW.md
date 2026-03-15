# Critical Test Review: Platform, Lifecycle, FileLock, Logger, JsonConfig

**Date:** 2026-02-10  
**Scope:** Platform, Lifecycle, FileLock, Logger, JsonConfig modules  
**Focus:** Correctness, completeness, coverage, MT/MP race conditions, cross-platform behavior

---

## 1. Platform (`plh_platform.hpp`)

### Current Coverage
- Core APIs: PID, thread ID, monotonic time, elapsed time, process liveness, executable name, version
- Shared memory: create/attach/close/unlink, flags, read/write semantics
- Time: monotonicity, resolution, clock skew protection
- Cross-platform: POSIX vs Windows branches where applicable

### Critical Gaps
| Gap | Severity | Notes |
|-----|----------|-------|
| Macro `PYLABHUB_PLATFORM_POSIX` | **Critical** | Header defines `PYLABHUB_IS_POSIX`; tests use `PYLABHUB_PLATFORM_POSIX`. POSIX-specific tests may never run. |
| `SHM_CREATE_EXCLUSIVE` | High | Never tested; behavior when segment already exists is undocumented by tests |
| Multi-process shared memory | High | All tests run in one process; no create-in-A / attach-in-B scenario |
| `shm_unlink` semantics | Medium | Order of close vs unlink, Windows vs POSIX differences not explicitly tested |

### Race-Condition Analysis
Shared memory has no inherent race for basic create/attach/close; multi-process usage patterns (including races on create/attach) are untested.

---

## 2. Lifecycle (`lifecycle.hpp`)

### Current Coverage
- Static: registration, init order, circular deps, unresolved deps
- Dynamic: load/unload, ref counting, diamond deps, finalize
- ModuleDef: null name, max length, null deps
- LifecycleGuard: multiple guards, ownership warning

### Critical Gaps
| Gap | Severity | Notes |
|-----|----------|-------|
| Init/finalize idempotency | High | Documented but not explicitly tested |
| `is_finalized()` | Medium | Never asserted |
| Concurrent `register_dynamic_module` | Medium | Thread-safety claimed; no concurrent registration tests |
| `load_module` / `unload_module` under MT | Medium | No multi-thread stress on load/unload |
| Unload timeout behavior | Medium | Test exists but exact timeout/cleanup semantics not verified |

### Race-Condition Analysis (MT and MP)
Lifecycle is primarily single-threaded; dynamic modules are documented as thread-safe but:
- No MT tests for `register_dynamic_module`, `load_module`, `unload_module`
- `shutdown_idempotency` exercises concurrent `finalize` from 16 threads—good
- No MP tests (lifecycle is process-local; MP out of scope unless documented)

---

## 3. FileLock (`file_lock.hpp`)

### Design
Two-layer model: (1) process-local registry (`g_proc_locks`, mutex + condition variable) for threads in the same process; (2) OS-level lock (`flock` / `LockFileEx`) for cross-process serialization. Order: acquire process-local first, then OS; release in reverse.

### Multithread (MT) Contention Coverage
| Test | Design | Assessment |
|------|--------|------------|
| `test_blocking_lock` | 2 threads, main holds, second blocks | Good |
| `test_multithreaded_non_blocking` | 64 threads, 1000 iterations, barrier sync, assert exactly 1 success/iteration | **Strong** |
| `test_timed_lock` | Main holds, second thread times out | Good |
| `MultiThreadedContention` (singleprocess) | 10 threads, try_lock NonBlocking | Good |
| `BlockingLockTimeout` | Main holds, thread with 100ms timeout | Good |

**Verdict:** MT coverage is strong.

### Multiprocess (MP) Contention Coverage
| Test | Design | Assessment |
|------|--------|------------|
| `MultiProcessNonBlocking` | Parent holds, child try_lock fails | Good |
| `MultiProcessBlockingContention` | 8 procs, 100 iters each, log ACQUIRE/RELEASE, assert no overlap | **Strong** |
| `MultiProcessParentChildBlocking` | Parent holds, child blocks until release | Good |
| `MultiProcessTryLock` | Parent holds, child try_lock returns nullopt | Good |

**Verdict:** MP coverage is strong.

### MT+MP Combined
- No test with multiple processes each running multiple threads contending on the same lock.
- Design suggests process-local handles intra-process, OS handles inter-process; combined stress would increase confidence.

### Other Gaps
| Gap | Severity | Notes |
|-----|----------|-------|
| `get_expected_lock_fullname_for` | Medium | Used in workers; output never asserted (path format, canonicalization) |
| `get_locked_resource_path` / `get_canonical_lock_file_path` | Medium | Never exercised |
| Non-blocking `error_code` | Low | Non-blocking failure returns `resource_unavailable_try_again`; tests don't assert |
| Lifecycle-before-FileLock | High | No test that FileLock without lifecycle aborts as specified |
| `ResourceType::File` vs `Directory` naming | Medium | `.lock` vs `.dir.lock` documented but not explicitly asserted |

---

## 4. Logger (`logger.hpp`)

### Design
Asynchronous command queue, dedicated worker thread; thread-safe enqueue. Cross-process via `flock` when `use_flock=true`.

### MT Contention
| Test | Design | Assessment |
|------|--------|------------|
| `test_multithread_stress` | 16 threads, 200 msgs each, line count | Good |
| `test_shutdown_idempotency` | 16 threads call `finalize` | Good |
| `test_concurrent_lifecycle_chaos` | Log + flush + sink switch from multiple threads | Good chaos test |
| `test_reentrant_error_callback` | Log from inside write error callback | Good |

### MP Contention
| Test | Design | Assessment |
|------|--------|------------|
| `StressLog` | 8 procs, 200 msgs each, line count | Good |
| `InterProcessFlock` | 4 procs, 250 msgs each, use_flock=true, unique payloads, integrity check | **Strong** |

### Other Gaps
| Gap | Severity | Notes |
|-----|----------|-------|
| `get_total_dropped_since_sink_switch` | Medium | Dropping tested via log parsing; API asserted in queue-full worker |
| `set_log_sink_messages_enabled` | Low | Never tested |
| Sync APIs (`*_fmt_sync`, `*_fmt_rt`) | Medium | Never exercised |
| Use-before-init | High | Documented behavior (abort) not explicitly tested |
| Queue-full under MP | Low | Queue-full test is single-process; MP behavior unknown |

---

## 5. JsonConfig (`json_config.hpp`)

### Design
- In-process: `std::shared_mutex` for readers-writer
- Cross-process: `FileLock` for file I/O
- `RecursionGuard` prevents nested transactions on same object

### MT Contention
| Test | Design | Assessment |
|------|--------|------------|
| `MultiThreadFileContention` | 16 threads, separate JsonConfig per thread, FullSync/ReloadFirst, assert no read_failures | **Strong** |
| `MultiThreadSharedObjectContention` | Shared JsonConfig, 4 writers + 8 readers, in-memory only | **Strong** |
| `RecursionGuard` | Nested read/read, write/write, read/write, write/read | Good |

### MP Contention
| Test | Design | Assessment |
|------|--------|------------|
| `MultiProcessContention` | 8 procs, write_id FullSync | Good |

### Other Gaps
| Gap | Severity | Notes |
|-----|----------|-------|
| `release_transaction` | Low | Deprecated; untested |
| Empty or invalid path in `init` | Medium | Edge cases not tested |
| `atomic_write_json` crash safety | Medium | Atomic write design not explicitly tested |

---

## 6. Summary: Race-Condition Coverage Matrix

| Module | MT Contention | MP Contention | MT+MP Combined | Verdict |
|--------|---------------|---------------|----------------|---------|
| Platform | N/A | N/A | N/A | No concurrency surface |
| Lifecycle | Partial | N/A | N/A | Finalize concurrency tested; load/unload not |
| FileLock | **Strong** | **Strong** | Missing | Both layers exercised; combined stress would help |
| Logger | **Strong** | **Strong** | Missing | MT and MP well covered |
| JsonConfig | **Strong** | Adequate | Missing | MT very strong; MP could be extended |

---

## 7. Priority Recommendations

### Critical
1. Fix macro name: `PYLABHUB_PLATFORM_POSIX` → `PYLABHUB_IS_POSIX` in tests

### High
2. FileLock: Add test for use-without-lifecycle (expect abort)
3. FileLock: Assert `get_expected_lock_fullname_for`, `get_locked_resource_path`, `get_canonical_lock_file_path`
4. Platform: Add multi-process shared-memory test (create in one process, attach in another)
5. Platform: Add `SHM_CREATE_EXCLUSIVE` test
6. Logger: Assert `get_total_dropped_since_sink_switch` in queue-full test (done)
7. Lifecycle: Test init/finalize idempotency and `is_finalized()`

### Medium (Race-Condition Focus)
8. FileLock: Add MT+MP combined stress (multiple processes, each with multiple threads)
9. Logger: Add MT+MP combined stress
10. JsonConfig: Add MT+MP combined stress
11. Lifecycle: Add MT stress for register_dynamic_module / load_module / unload_module if supported

### Lower Priority
12. Logger: Test sync APIs, use-before-init
13. JsonConfig: Test empty/invalid path, init under external modification
14. FileLock: Assert non-blocking `error_code` (`resource_unavailable_try_again`)
15. Document NFS / network-filesystem limitations for FileLock

---

## 8. Implementation Status (2026-02-11)

### Implemented
- **Critical:** Fixed `PYLABHUB_PLATFORM_POSIX` → `PYLABHUB_IS_POSIX` in test_platform_core, test_platform_debug
- **Platform:** SHM_CREATE_EXCLUSIVE test; multi-process shm (POSIX fork); `is_process_alive` rejects PIDs > pid_t max (fixes UINT64_MAX truncation); unified `IsProcessAlive_DetectsAliveThenDeadProcess` — fork+pipe (POSIX) and CreateProcess+TerminateProcess (Windows), spawn child, assert alive, signal exit, assert dead
- **Lifecycle:** Init/finalize idempotency workers; is_finalized_flag worker and tests
- **FileLock:** get_expected_lock_fullname_for, get_locked_resource_path, get_canonical_lock_file_path tests; use-without-lifecycle abort worker and test
- **Logger:** `get_total_dropped_since_sink_switch` (renamed from `get_dropped_message_count`), accumulated total, reset on sink switch; assertion in queue-full worker; use-without-lifecycle abort worker and test
- **JsonConfig:** InitWithEmptyPathFails test
- **Recovery/DataHub modules:** test_recovery_api.cpp + recovery_workers: datablock_is_process_alive, IntegrityValidator.validate(), SlotDiagnostics.refresh(), SlotRecovery.release_zombie_readers(), HeartbeatManager (register, pulse, destruction order); SlotDiagnostics::is_valid() implementation added (was declared, missing)

### Existing Recent Tests (schema_blds, schema_validation, shared_memory_spinlock)
- Meet review standards: public API, type mapping, MT/MP contention where applicable
