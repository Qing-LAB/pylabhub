# Code & Documentation Review — 2026-03-14

**Scope:** Full codebase review of recent changes (python_venv feature, HEP-0025, documentation
updates) plus triage of external codex review findings against current code.

**Status:** ✅ CLOSED — all actionable items resolved 2026-03-14; 1166/1166 tests pass

---

## Source: Recent Changes (python_venv / HEP-0025 feature)

### RC-01 — `python_venv` docstring inconsistency across configs
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ✅ FIXED 2026-03-14 | `src/producer/producer_config.hpp:209-212`, `src/consumer/consumer_config.hpp:177-179`, `src/processor/processor_config.hpp:257-259` |

Removed extra `/// Created by:` line from ProducerConfig to match Consumer/Processor.

---

## Source: Codex Review Triage (high_level_codex_review.md)

Each finding from the external codex review has been verified against current code.

### CX-01 — `get_binary_dir()` uses undefined platform macros (HIGH)
| Severity | Status | File(s) |
|----------|--------|---------|
| HIGH | ✅ FIXED 2026-03-14 | `src/utils/config/hub_config.cpp:74,82` |

**Confirmed real bug.** `get_binary_dir()` branches on `PYLABHUB_IS_LINUX` (line 74) and
`PYLABHUB_IS_APPLE` (line 82), but `plh_platform.hpp` only defines `PYLABHUB_IS_POSIX` and
`PYLABHUB_IS_WINDOWS`. On Linux/macOS, all three `#if` branches are false, function returns
`{}`, and fallback path initialization in `load()` silently skips `root_dir` / `config_dir`.

**Contrast:** `platform.cpp::get_executable_name()` correctly uses `PYLABHUB_PLATFORM_LINUX` /
`PYLABHUB_PLATFORM_APPLE` / `PYLABHUB_PLATFORM_WIN64`.

**Fix:** Replace with `PYLABHUB_IS_POSIX` (readlink + `/proc/self/exe` works on Linux;
macOS needs `_NSGetExecutablePath`), or use the fine-grained `PYLABHUB_PLATFORM_LINUX` /
`PYLABHUB_PLATFORM_APPLE` macros. Best approach: delegate to `platform::get_executable_name(true)`
and take `.parent_path()`, eliminating the duplication entirely.

### CX-02 — POSIX mutex timeout uses CLOCK_REALTIME (MEDIUM)
| Severity | Status | File(s) |
|----------|--------|---------|
| MEDIUM | ✅ FIXED 2026-03-14 | `src/utils/shm/data_block_mutex.cpp:366-403` |

**Confirmed.** `try_lock_for()` calls `clock_gettime(CLOCK_REALTIME, &abstime)` on line 369
and passes the absolute deadline to `pthread_mutex_timedlock()`. No
`pthread_mutexattr_setclock(CLOCK_MONOTONIC)` is set on the mutex. Backward NTP adjustments
can cause indefinite hangs. Previously identified in archived reviews; fix not yet implemented.

**Note:** This is a cross-process shared mutex (in SHM), and `CLOCK_MONOTONIC` support with
`pthread_mutexattr_setclock` is not universally available on all POSIX platforms (notably
macOS lacks it). A portable fix is non-trivial. Consider documenting the limitation if not
fixing immediately.

### CX-03 — RAII guard destructors can throw (MEDIUM)
| Severity | Status | File(s) |
|----------|--------|---------|
| MEDIUM | ✅ ACCEPTED (design decision) | `src/utils/shm/shared_memory_spinlock.cpp:176-179`, `src/utils/shm/data_block_mutex.cpp:432-435` |

**Confirmed but intentional.** Both destructors call `unlock()` which can throw on
non-owner / OS error. Documented as a deliberate design choice in
`docs/archive/transient-2026-02-12/GUARD_RACE_AND_UB_ANALYSIS.md`: prefer fail-loud over
silent corruption masking. Both have `NOLINTNEXTLINE(bugprone-exception-escape)` suppressions.

Under normal RAII usage the destructor never throws (only the owning thread calls it).
Throwing from destructor during stack unwinding → `std::terminate()` is the intended
behaviour for corruption/misuse scenarios.

### CX-04 — README_Deployment.md: `timeout_ms` vs `slot_acquire_timeout_ms` (MEDIUM)
| Severity | Status | File(s) |
|----------|--------|---------|
| MEDIUM | ✅ FIXED 2026-03-14 | `docs/README/README_Deployment.md:328,425,537` |

**Confirmed.** The field reference tables at lines 328 (producer), 425 (consumer), and 537
(processor) incorrectly use `timeout_ms`. The actual config field parsed by all three
`*_config.cpp` files is `slot_acquire_timeout_ms`. The JSON examples (lines 395, 490) are
correct. The table rows need renaming.

### CX-05 — Root README.md test count stale (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ❌ OPEN | `README.md` |

README.md says "1120+ tests" but current count is 1166+. Update to reflect reality.

### CX-06 — `parse_overflow_policy()` duplicated in processor_config.cpp (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ❌ OPEN | `src/processor/processor_config.cpp:54-61` vs `src/include/utils/hub_queue.hpp:53-60` |

**Confirmed.** `processor_config.cpp` defines a local `parse_overflow_policy()` in its
anonymous namespace instead of using the shared `hub::parse_overflow_policy()` from
`hub_queue.hpp`. The shared version accepts an optional context string parameter.

### CX-07 — Startup-wait parsing duplication across configs (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ✅ ACCEPTED | `producer_config.cpp:205-233`, `consumer_config.cpp:168-196`, `processor_config.cpp:307-335` |

Near-identical `wait_for_roles` parsing blocks (~30 lines each). Acceptable: each is
self-contained, has role-specific error prefixes, and extracting to a shared helper would
couple config namespaces for minimal benefit.

### CX-08 — Inbox-thread shutdown duplication (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ✅ ACCEPTED | `*_script_host.cpp` stop_role() methods |

Structurally identical inbox thread join + queue stop pattern in all three script hosts.
Acceptable: each class owns its members, and the critical ZMQ single-thread-per-socket
invariant comment is valuable to keep visible in each implementation.

### CX-09 — `PyExecResult::result_repr` dead field (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ❌ OPEN | `src/hub_python/python_interpreter.hpp:55-56` |

**Confirmed.** Documented as "not yet implemented; always empty". Never read or written
anywhere. Either remove or add a tracking item for AdminShell exec path.

### CX-10 — `RoleDirectory::create()` config_filename parameter unused (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ❌ OPEN | `src/include/utils/role_directory.hpp:90-91`, `src/utils/config/role_directory.cpp:33-34` |

**Confirmed.** Parameter is declared but commented out in the implementation
(`/*config_filename*/`). Either implement it or remove from the signature.

### CX-11 — README_DirectoryLayout.md references non-existent paths (LOW)
| Severity | Status | File(s) |
|----------|--------|---------|
| LOW | ❌ OPEN | `docs/README/README_DirectoryLayout.md:59-60` |

Documents `share/scripts/python/examples/` and `pylabhub_sdk/` which do not exist.
Actual structure uses `share/py-examples/`. Update to match reality.

---

## Summary

| ID | Severity | Status | One-liner |
|----|----------|--------|-----------|
| CX-01 | HIGH | ✅ FIXED 2026-03-14 | `get_binary_dir()` → delegates to `platform::get_executable_name()` |
| CX-02 | MEDIUM | ✅ FIXED 2026-03-14 | POSIX mutex: monotonic-clock retry loop (500 ms chunks) |
| CX-03 | MEDIUM | ✅ ACCEPTED | Guard destructors throw by design |
| CX-04 | MEDIUM | ✅ FIXED 2026-03-14 | Docs `timeout_ms` → `slot_acquire_timeout_ms` |
| CX-05 | LOW | ✅ FIXED 2026-03-14 | Test count updated to 1160+ |
| CX-06 | LOW | ✅ ACCEPTED | Different enum types (`processor::OverflowPolicy` vs `hub::OverflowPolicy`) |
| CX-07 | LOW | ✅ ACCEPTED | Startup-wait parsing duplication (acceptable) |
| CX-08 | LOW | ✅ ACCEPTED | Inbox shutdown duplication (acceptable) |
| CX-09 | LOW | ✅ FIXED 2026-03-14 | Removed dead `result_repr` field |
| CX-10 | LOW | ✅ FIXED 2026-03-14 | Removed unused `config_filename` parameter |
| CX-11 | LOW | ✅ FIXED 2026-03-14 | README_DirectoryLayout.md updated to match actual staging |
| RC-01 | LOW | ✅ FIXED 2026-03-14 | `python_venv` docstring aligned |

**8 FIXED, 4 ACCEPTED — all resolved 2026-03-14**
