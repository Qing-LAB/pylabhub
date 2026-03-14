# Archive: transient-2026-03-01

**Archived:** 2026-03-01
**Reason:** Code Review Round 2 complete — all actionable items resolved (fixed or classified as false positive).

## Archived documents

| Document | Original location | Disposition |
|----------|-------------------|-------------|
| `CODE_REVIEW_2026-03-01_hub-python-actor-headers.md` | `docs/code_review/` | All items resolved: HP-C1 ✅ HP-C2 ✅ AF-H2 ✅ FIXED; PH-C1 PH-C2 PH-C3 AF-H1 PH-H5 ❌ FALSE POSITIVE |
| `CODE_REVIEW_2026-03-01_utils-actor-hep.md` | `docs/code_review/` | All items resolved: NC3 ✅ NC4 ✅ NH1 ✅ NH2 ✅ FIXED; NC1 NC2 NH4 NH5 NM11 NM12 NM13 ❌ FALSE POSITIVE |

## Fix summary

**Bug fixes implemented (2026-03-01):**
- **NC3**: `PLATFORM_WIN64` → `PYLABHUB_PLATFORM_WIN64` in `syslog_sink.cpp` + `syslog_sink.hpp`
- **NC4**: Reversed atomic ordering in `release_write_handle()` abort path: `slot_state=FREE` before `write_lock=0`
- **NH1**: `sodium_memzero()` added at 4 CurveZMQ secret key sites in `messenger.cpp`
- **NH2**: `sodium_memzero()` added for `sec`/`pub` stack arrays in `actor_vault.cpp`
- **HP-C1**: Added `PythonInterpreter::reset_namespace_unlocked()` and `Impl::do_reset()`; binding calls unlocked variant
- **HP-C2**: Added `make_scope_guard` in `exec()` to restore `sys.stdout/stderr` on any exception path
- **AF-H2**: Changed `ctypes.from_buffer()` → `from_buffer_copy()` for both consumer slot view and flexzone view

**Rename (Part 1):**
- `ExponentialBackoff` → `ThreePhaseBackoff` in 4 source files + tests (Phase 3 is linear, not exponential)

**Clarifying comments added (Part 3, false positive confirmations):**
- `lifecycle.cpp`: `std::map` reference stability guarantees `&node` valid until explicit `erase()`
- `lifecycle_dynamic.cpp`: Erase runs AFTER cleanup loop; `n->name` dereferences valid throughout
- `slot_rw_coordinator.h`: Namespace forward declarations intentionally outside `extern "C"` (already present)
- `shared_memory_spinlock.hpp`: `SharedSpinLockState::padding[4]` uninitialized intentionally; never read by sync logic or hashing
- `data_block.hpp`: `SlotRWState::padding[24]` is cache-line padding only; not part of any hash or logic
- `actor_role_workers.cpp`: RAII cleanup comment after `start_embedded()` failure; `has_value()` check confirmation
- `hub_config.cpp`: `load_()` called once at lifecycle init before worker threads; read-only thereafter
- `schema_blds.hpp`: `compute_hash()` uses BLAKE2b-256 of BLDS string; no `typeid()`

**HEP doc fixes (Part 4):**
- `HEP-CORE-0003`: Updated API section to match actual `bool is_directory, bool blocking` parameters (not enum)
- `HEP-CORE-0012`: Added `[UNIMPLEMENTED — FUTURE WORK]` banner

## Items deferred to subtopic TODOs

Test coverage gaps (no automated tests for HP-C1/HP-C2/AF-H2 regressions) added to `docs/todo/TESTING_TODO.md`.

Remaining round-2 carry-forward unfixed items (H3/H5/M2/M3/M5/M10/M13/M18/M19/M22) and new round-2
medium items (NM1–NM10) remain open in `docs/todo/API_TODO.md` or relevant subtopic TODOs where applicable.
