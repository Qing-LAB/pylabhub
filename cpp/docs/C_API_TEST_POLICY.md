# C API Test Policy

**Document ID**: POLICY-C-API-TEST  
**Status**: Mandatory  
**Applies to**: All test changes, refactors, and test reorganization

---

## Rule

**The C API is the foundation. Other layers (C++ primitive, schema-aware, RAII) are built on top of it. The C API must be correct, and it must be tested extensively.**

**DO NOT DELETE C API TESTS.** Any reorganization, modernization, or refactor of tests must preserve and preferably extend C API test coverage. Removing or disabling C API tests is not acceptable unless the underlying C API itself is removed (which would be a separate, explicit design decision).

---

## What Counts as C API

- **Slot RW Coordinator** (`utils/slot_rw_coordinator.h`): `slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`, `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`, `slot_rw_get_metrics`, `slot_rw_reset_metrics`, `slot_rw_get_total_slots_written`, `slot_rw_get_commit_index`, `slot_rw_get_slot_count`.
- **Recovery API** (`utils/recovery_api.hpp`, `extern "C"`): recovery, integrity validation, slot diagnostics, slot recovery, heartbeat manager, and any other C/FFI entry points used by tooling or other languages.

---

## Current C API Test Assets (Do Not Delete)

| Asset | Purpose |
|-------|--------|
| **tests/test_layer2_service/test_slot_rw_coordinator.cpp** | Pure C SlotRWCoordinator API: acquire/commit/release write, acquire/validate/release read, metrics, reset. Single-process, no DataBlock. |
| **tests/test_layer3_datahub/test_recovery_api.cpp** | Recovery API: `recovery.datablock_is_process_alive`, `recovery.integrity_validator_validate`, `recovery.slot_diagnostics_refresh`, `recovery.slot_recovery_release_zombie_readers`, `recovery.heartbeat_manager_registers`, producer heartbeat / `is_writer_alive`. |
| **tests/test_layer3_datahub/workers/recovery_workers.cpp** | Worker implementations for recovery API tests (uses `recovery_api.hpp`). |
| **tests/test_layer3_datahub/workers/slot_protocol_workers.cpp** | Uses C API `slot_rw_coordinator.h`: `slot_rw_state()`, `slot_rw_get_metrics()`, `slot_rw_reset_metrics()`. Must remain so that slot protocol and metrics are validated at the C API level where used. |

When moving or renaming tests, these files (or their replacement C API test coverage) must remain. Any new structure (e.g. `c_api/`) should include or reference these tests; do not drop them.

---

## Extending C API Coverage

- Prefer adding tests that call the C API directly (e.g. more `slot_rw_*` scenarios, more recovery_api scenarios, error codes, NULL handling).
- When adding higher-level tests (C++ primitive, schema, RAII), do not replace or remove the C API tests that cover the same behavior at the C layer.

---

## References

- API levels: `docs/API_SURFACE_DOCUMENTATION.md` (Level 0 = C API).
- Test layout: `docs/TEST_ORGANIZATION_STRUCTURE.md` (c_api/ and C API coverage).
