# Full-Stack Deep Code Review — 2026-03-17

**Reviewer**: Claude Opus 4.6 (systematic review, 160+ source files, 100+ test files)
**Scope**: All layers (L0–L4), all modules, code + tests + design consistency
**Branch**: `feature/lua-role-support` (2051b80)
**Test baseline**: 1166/1166 passing

---

## Status Table

| ID | Sev | Module | Description | Status |
|----|-----|--------|-------------|--------|
| **PARITY-01** | HIGH | All Lua hosts | Lua API surface ≈30% of Python — missing metrics, inbox outbound, spinlocks, diagnostics, inter-role messaging | ❌ OPEN |
| **L2-01** | HIGH | DataBlockMutex | `timeout_ns` computed as `long` — overflows on 32-bit `long` platforms for timeout_ms > 2147 | ✅ FIXED 2026-03-17 |
| **L2-02** | ~~HIGH~~ | ZMQ Context | ~~Comment misleading~~ — FALSE POSITIVE: `ctx->shutdown()` signals ETERM to in-flight ops; `store(nullptr)` prevents new callers; two-phase shutdown is correct | ✅ ACCEPTED |
| **L0-01** | MEDIUM | platform.cpp | Windows `shm_create()` passes size as single DWORD — silently truncates SHM > 4GB | ✅ FIXED 2026-03-17 |
| **L0-02** | MEDIUM | platform.cpp | POSIX `shm_create()` calls `shm_unlink` on ftruncate failure even for pre-existing segments | ✅ FIXED 2026-03-17 |
| **L2-03** | MEDIUM | SharedSpinLock | Recursive lock check loads owner_pid/owner_tid non-atomically — theoretical TOCTOU | ✅ FIXED 2026-03-17 |
| **L2-04** | MEDIUM | DataBlockMutex | `try_lock_for(-1)` maps to non-blocking on BOTH Windows and POSIX (`<= 0` instead of `== 0`) | ✅ FIXED 2026-03-17 + 2 tests added (parameter contract + multi-process timestamp contention) |
| **L2-05** | ~~MEDIUM~~ | FileLock | Infinite CV wait — ACCEPTED: inherent to CV-based sync; documented in header; health monitor concept added to API_TODO | ✅ ACCEPTED |
| **L2-06** | ~~MEDIUM~~ | Logger | Pre-init behavior divergence — ACCEPTED: intentional design (macros = fire-and-forget safe anytime; config API = programmer error if pre-init) | ✅ ACCEPTED |
| **L2-07** | MEDIUM | JsonConfig | `consume_write_` uses indefinite-blocking FileLock without timeout | ✅ FIXED 2026-03-17 (3 retries × 2s timeout, returns errc::timed_out on failure) |
| **L2-08** | ~~MEDIUM~~ | InteractiveSignalHandler | Static raw pointer — documented: lifecycle rejects duplicates; one-per-process enforced by OS + lifecycle; comments added to header + impl | ✅ FIXED 2026-03-17 |
| **L2-09** | MEDIUM | ScriptHost | Member ordering dependency `init_promise_` before `init_future_` undocumented | ✅ FIXED 2026-03-17 (comment added) |
| **L3-A1** | MEDIUM | DataBlock policy | `parse_consumer_sync_policy` rejects `"sequential_sync"` — enum value exists but is unparseable from JSON | ✅ FIXED 2026-03-17 + test added |
| **L3-A8** | ~~MEDIUM~~ | DataBlock | Sequential_sync min scan — ACCEPTED: stale min only delays writer (never corrupts); atomic scan would defeat lock-free design | ✅ ACCEPTED |
| **L3-A7** | ~~MEDIUM~~ | DataBlock | Flexzone checksum every commit — ACCEPTED: sub-µs for typical sizes; `suppress_flexzone_checksum()` exists as opt-out | ✅ ACCEPTED |
| **SL-01** | ~~MEDIUM~~ | PythonRoleHostBase | Redundant GIL acquire — ACCEPTED: defensive pybind11 practice; ensures function is context-independent | ✅ ACCEPTED |
| **SL-02** | ~~MEDIUM~~ | RoleHostCore | `drain_messages()` — ACCEPTED: move+clear preserves allocated capacity; swap would cause heap allocation churn; comment added | ✅ ACCEPTED |
| **SL-05** | LOW | PythonRoleHostBase | Stale comment on `stop_reason_` — escalated to full refactor (see SL-05 refactor above) | ✅ FIXED 2026-03-17 |
| **SL-04** | LOW | StopReason | Duplicate enums — unified in RoleHostCore; using aliases in both engine bases | ✅ FIXED 2026-03-17 |
| **L0-03** | LOW | plh_platform.hpp | `monotonic_time_ns()` docstring referenced wrong clock | ✅ FIXED 2026-03-17 (cross-platform description) |
| **L0-04** | LOW | plh_platform.hpp | `get_pid()` missing `noexcept` | ✅ FIXED 2026-03-17 |
| **L0-05** | LOW | platform.cpp | Windows `GetModuleFileNameW` truncation off-by-one | ✅ FIXED 2026-03-17 |
| **L0-06** | LOW | uuid_utils.cpp | `sodium_init()` failure silently ignored | ✅ FIXED 2026-03-17 (PLH_PANIC on failure — CSPRNG is safety-critical) |
| **L0-07** | LOW | debug_info.cpp | Duplicate `#include` | ✅ FIXED 2026-03-17 |
| **L2-10** | LOW | SharedSpinLock | `padding[4]` uninitialized | ✅ FIXED 2026-03-17 (memset in init_spinlock_state) |
| **L2-11** | LOW | HubVault | Secret keys in `std::string` — ACCEPTED: sodium_memzero on secret key; mlock is platform-specific hardening for future | ✅ ACCEPTED |
| **L2-12** | LOW | ZMQ Context | Double-init race — ACCEPTED: lifecycle ordering prevents concurrent startup; comment documents constraint | ✅ ACCEPTED |
| **L3-C1** | LOW | hub_producer.hpp | ABI guard — `static_assert(sizeof)` added for both Producer (64) and Consumer (48) facades; HEP §6.9.1 written | ✅ FIXED 2026-03-17 |
| **L3-D2** | LOW | BrokerService | Callback lifetime warnings added to on_hub_connected/disconnected/message (ref on_ready doc) | ✅ FIXED 2026-03-17 |
| **PR-03** | LOW | LuaProducerHost | const overload on `shm()` — deferred (Lua WIP) | ✅ DEFERRED |
| **HS-01** | LOW | AdminShell | Token comparison — `sodium_memcmp()` constant-time comparison | ✅ FIXED 2026-03-17 |
| **HS-02** | LOW | hubshell.cpp | Duplicated TTY/password helpers — deleted, delegated to role_cli.hpp | ✅ FIXED 2026-03-17 |

---

## Detailed Findings by Layer

### Layer 0 — Platform (4 files, 4 test files)

**Files**: `plh_platform.hpp`, `platform.cpp`, `debug_info.hpp/.cpp`, `uuid_utils.hpp/.cpp`

| Finding | Details |
|---------|---------|
| **L0-01 (MEDIUM)** | `platform.cpp:399` — `CreateFileMappingA(... 0, static_cast<DWORD>(size), ...)`. The `dwMaximumSizeHigh` is hardcoded to 0, so SHM segments > 4GB are silently truncated on Win64. Fix: use `size >> 32` for high DWORD. |
| **L0-02 (MEDIUM)** | `platform.cpp:500-503` — On `ftruncate` failure, `shm_unlink(name)` is called unconditionally. If `O_EXCL` was NOT set and an existing segment was opened, this destroys someone else's data. Fix: only unlink if this process created the segment. |
| **L0-03 (LOW)** | `plh_platform.hpp:253` — Docstring says `high_resolution_clock`; implementation correctly uses `steady_clock`. |
| **L0-04 (LOW)** | `plh_platform.hpp:207` — `get_pid()` lacks `noexcept`; all other simple platform queries have it. |
| **L0-05 (LOW)** | `platform.cpp:131-135` — `len < buf.size() - 1` is off-by-one for `GetModuleFileNameW` truncation detection. |
| **L0-06 (LOW)** | `uuid_utils.cpp:22-23` — `sodium_init()` returning -1 (catastrophic) is silently discarded via `(void)sodium_rc`. |
| **L0-07 (LOW)** | `debug_info.cpp:63` — Duplicate `#include "utils/debug_info.hpp"` (first at line 14). |

**Test coverage**: Solid. All public APIs tested. **Gap**: No `uuid_utils` tests in L0 scope; no `bytes_to_hex`/`bytes_from_hex` unit tests for format_tools inline helpers.

---

### Layer 1 — Base Utilities (9 headers, 1 impl, 8 test files)

**Files**: `in_process_spin_state.hpp`, `spinlock_owner_ops.hpp`, `scope_guard.hpp`, `recursion_guard.hpp`, `module_def.hpp`, `result.hpp`, `backoff_strategy.hpp`, `format_tools.hpp/.cpp`

No CRITICAL or HIGH findings. All modules well-designed.

| Finding | Details |
|---------|---------|
| `in_process_spin_state.hpp:224-233` | `try_lock()` leaves `token_` non-zero after failure. `holds_lock()` is correct (checks `state_` too), but the stale token is surprising. Documented behavior — acceptable. |
| `result.hpp:129` | Default constructor creates error state with `E{}` (value 0 = `Timeout`). Could confuse callers. |
| `result.hpp:214-217` | `value_or()` missing rvalue overload — prevents move from temporary Results. |

**Test coverage**: Comprehensive. 8 test files, 70+ tests. ScopeGuard (16 tests), RecursionGuard (6), Result (13), SpinGuard (10+), BackoffStrategy, ModuleDef, format_tools all well-covered.

---

### Layer 2 — Services (25+ headers, 20+ impls, 22 test files)

**Critical sub-modules**: SharedMemorySpinlock, DataBlockMutex, FileLock, Logger, Lifecycle, JsonConfig, Crypto/Vault

| Finding | Details |
|---------|---------|
| **L2-01 (HIGH)** | `data_block_mutex.cpp:377` — `const long timeout_ns = static_cast<long>(timeout_ms) * kNsPerMs`. On Win64 (LLP64, `long` = 32-bit), timeouts > 2.1s overflow. Fix: use `int64_t`. |
| **L2-02 (HIGH)** | `zmq_context.cpp:86-89` — Comment claims `store(nullptr)` before `delete` eliminates races. It only prevents new callers; threads that already loaded the pointer can still use-after-free. Safe in practice due to lifecycle ordering, but comment is misleading. |
| **L2-03 (MEDIUM)** | `shared_memory_spinlock.cpp:51-56` — Recursive lock check reads `owner_pid` then `owner_tid` as separate loads. Between them, lock could be released and re-acquired by a different process with TID reuse. Astronomically unlikely but theoretically unsound. |
| **L2-04 (MEDIUM)** | `data_block_mutex.cpp:131` — Windows path maps `timeout_ms <= 0` to `DWORD ms = 0` (non-blocking). SharedSpinLock convention is `-1 = infinite wait`. |
| **L2-05 (MEDIUM)** | `file_lock.cpp:466-471` — Blocking `cv.wait()` with no timeout. If the owning thread crashes without unlocking, waiter hangs forever. |
| **L2-06 (MEDIUM)** | `logger.hpp` vs `logger.cpp:62` — LOGGER_INFO macros silently drop pre-init messages, but `set_console()` etc. abort via PLH_PANIC. Divergence undocumented. |
| **L2-07 (MEDIUM)** | `json_config.hpp:601-623` — Write transaction constructs `FileLock(path)` (blocking, infinite wait). |
| **L2-08 (MEDIUM)** | `interactive_signal_handler.cpp:526` — `static InteractiveSignalHandler *s_lifecycle_instance` — raw pointer, no CAS, no thread-safety guarantee. |
| **L2-09 (MEDIUM)** | `script_host.hpp:231-232` — `init_future_` initialized from `init_promise_` relies on declaration order. |

**Test coverage**: Good overall. FileLock (826 lines), JsonConfig (1089 lines), Lifecycle (405 lines). **Gaps**: ZMQ context (88 lines, minimal), InteractiveSignalHandler (165 lines, no interactive prompt test), no WAIT_ABANDONED test for DataBlockMutex Windows path.

---

### Layer 3 — DataHub (35+ headers/impls, 35+ test files)

**Critical sub-modules**: DataBlock state machine, ZmqQueue, InboxQueue, Messenger, BrokerService, Schema

| Finding | Details |
|---------|---------|
| **L3-A1 (MEDIUM)** | `data_block_policy.hpp:247-255` — `parse_consumer_sync_policy` rejects `"sequential_sync"` even though the enum value `Sequential_sync` exists. Users cannot configure this policy from JSON. |
| **L3-A7 (MEDIUM)** | `data_block.cpp:1726-1734` — BLAKE2b flexzone checksum runs on every `release_write_handle` commit, even when flexzone content is unchanged. Performance concern for high-frequency producers. |
| **L3-A8 (MEDIUM)** | `data_block.cpp:1862-1875` — Sequential_sync min-position scan reads individual heartbeat positions non-atomically. Stale minimum possible if a consumer advances between reads. |

**Slot state machine correctness**: VERIFIED SOUND. The COMMITTED→DRAINING→WRITING transition uses correct memory ordering: `slot_state.store(DRAINING, release)` + `seq_cst fence`, reader acquisition uses double-check `load(acquire)` + `fetch_add(acq_rel)` + `seq_cst fence` + re-check. No TOCTTOU gap.

**ZmqQueue**: Move assignment explicitly calls `stop()` first (prevents `std::terminate`). Wire format (msgpack fixarray) verified correct. recv_thread_ joined before socket close.

**Test coverage**: Extensive — 35+ test files with multi-process worker infrastructure. Slot state machine, DRAINING, checksum, recovery, metrics, and all queue types well-tested.

---

### Scripting Layer (5 headers, 6 impls, 2 test files)

| Finding | Details |
|---------|---------|
| **SL-01 (MEDIUM)** | `python_role_host_base.cpp:250` — `call_on_init_common_()` acquires GIL when it's already held by caller. pybind11 handles recursive acquire safely but it's wasteful. |
| **SL-02 (MEDIUM)** | `role_host_core.cpp:30-42` — `drain_messages()` loops over elements; `std::swap` would be O(1). |
| **SL-04 (LOW)** | `StopReason` (python_role_host_base.hpp:61) and `LuaStopReason` (lua_role_host_base.hpp:67) are identical. Should be unified. |
| **SL-05 (LOW)** | `python_role_host_base.hpp:122` — Comment says "2=CriticalError" but enum says `HubDead=2, CriticalError=3`. |

**Thread safety**: VERIFIED. Python GIL management correct (main_thread_release_ emplaced before worker starts, reset before stop_role). Lua single-threaded access to lua_State enforced by design. RoleHostCore message queue mutex-protected, shutdown flags atomic.

**Shutdown ordering**: VERIFIED. Both Python and Lua paths: `running_threads=false` → join threads → on_stop callback → teardown. Double-shutdown protection via `worker_thread_.joinable()` check.

---

### Binary Layer — Producer, Consumer, Processor, HubShell (15+ impls, 10 test files)

| Finding | Details |
|---------|---------|
| **PARITY-01 (HIGH)** | All three Lua role hosts implement ≈30% of their Python counterparts' API. Missing: `report_metric`/`report_metrics`/`clear_custom_metrics` (metrics), `spinlock`/`spinlock_count` (SHM), `open_inbox`/`wait_for_role` (outbound inbox), `notify_channel`/`broadcast_channel`/`list_channels` (inter-role), `loop_overrun_count`/`last_cycle_work_us`/`metrics` (diagnostics), `flexzone`/`logs_dir`/`run_dir` (accessors). |
| **PR-03 (LOW)** | `lua_producer_host.cpp:305` — `const_cast<hub::Producer&>(*out_producer_).shm()` needed because `shm()` is non-const. |
| **HS-01 (LOW)** | `admin_shell.cpp:172` — Token comparison uses `!=` (not constant-time). Mitigated by localhost binding. |
| **HS-02 (LOW)** | `hubshell.cpp:100-146` — `is_stdin_tty()` and `read_password_interactive()` duplicated from `role_cli.hpp`. |

**Config validation**: All three role configs validated thoroughly — transport, packing, buffer depth, overflow policy, timing policy, cross-field constraints. No gaps found.

**Main function correctness**: All four binaries follow correct ordering: signal handler install → config load → lifecycle guard → script host start → main loop → signal handler uninstall → ordered shutdown.

---

## Test Coverage Summary

| Layer | Test Files | Lines | Assessment |
|-------|-----------|-------|------------|
| L0 Platform | 4 | ~400 | Solid; gaps in uuid_utils, hex conversion |
| L1 Base | 8 | ~800 | Comprehensive; 70+ tests |
| L2 Service | 22 + workers | ~4000 | Good; weak on ZMQ context, signal handler interactive |
| L3 DataHub | 35 + workers | ~8000+ | Extensive; multi-process infrastructure |
| L4 Binaries | 10 | ~2000 | Adequate for CLI/config; **no Lua integration tests** |

**Critical gap**: No Lua role integration tests exist. The Lua producer/consumer/processor data loops, inbox drain, and startup coordination have zero integration test coverage beyond the `test_lua_role_host_base.cpp` unit tests.

---

## Prioritized Action Items

### Must-fix (HIGH)
1. **PARITY-01**: Track Lua API parity gap in TESTING_TODO.md — plan phased implementation
2. **L2-01**: Change `long` to `int64_t` in DataBlockMutex timeout computation (1-line fix)
3. **L2-02**: Correct misleading comment in zmq_context.cpp shutdown

### Should-fix (MEDIUM)
4. **L0-01**: Windows shm_create >4GB truncation — use `(size >> 32)` for high DWORD
5. **L0-02**: POSIX shm_create — only `shm_unlink` on failure if this process created the segment
6. **L2-04**: Document or fix DataBlockMutex Windows `try_lock_for(-1)` semantic
7. **L3-A1**: Add `"sequential_sync"` to `parse_consumer_sync_policy` or document exclusion
8. **SL-05**: Fix stale `stop_reason_` comment (2=HubDead, 3=CriticalError)

### Nice-to-have (LOW)
9. **SL-04**: Unify StopReason + LuaStopReason into single enum
10. **L0-03**: Fix `monotonic_time_ns()` docstring (steady_clock, not high_resolution_clock)
11. **L0-04**: Add `noexcept` to `get_pid()`
12. **L0-07**: Remove duplicate include in debug_info.cpp
13. **PR-03**: Add const overload to `hub::Producer::shm()`
14. **HS-02**: Consolidate duplicated helper functions in hubshell.cpp

---

## Commendations

- **Slot state machine**: The double-check reader acquisition with `fetch_add(acq_rel)` + `seq_cst fence` + re-check is textbook correct concurrent design
- **ZmqPollLoop**: Centralized EINTR handling eliminates the historical spin-bug class entirely
- **Lifecycle topological sort**: Kahn's algorithm with cycle detection is correct and well-tested
- **Pimpl discipline**: Consistently applied across all public classes; destructors in .cpp files
- **Test infrastructure**: Multi-process worker framework with port-0 binding is production-grade testing
- **JsonConfig TransactionProxy**: rvalue-qualified `read()`/`write()` with debug warning on unconsumed proxy is excellent API design
