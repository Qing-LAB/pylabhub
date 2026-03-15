# DataBlock Module: Critical Design & Implementation Review

**Scope:** `data_block.cpp`, `data_block_mutex.cpp`, `shared_memory_spinlock.cpp`, `data_block_recovery.cpp`  
**Date:** 2026-02-10

**Cross-platform:** Review and design **always** consider all supported platforms (Windows, Linux, macOS, FreeBSD). No feature or API is complete until behavior is defined and verified across the platform layer; platform-specific branches must be documented and tested. See §2.3 and DATAHUB_TODO “Cross-Platform and Behavioral Consistency”.

---

## 1. Functionality & Logical Design

### 1.1 Layering (good)

- **DataBlock** – Raw shm segment: create/attach, header layout, SlotRWState array, flexible/structured buffers, spinlock slot allocation. No broker; no schema beyond header storage.
- **DataBlockMutex** – OS-backed cross-process mutex (Windows named mutex; POSIX `pthread_mutex_t` in shm or dedicated segment). Intended for control zone; currently **not** held by DataBlock (removed as member).
- **DataBlock spinlock** – Tier-2 PID-based spinlock in shared memory (`SharedSpinLockState`), used for flexible zones and allocation coordination.
- **Slot RW** – Writer/reader coordination per slot (write_lock, reader_count, slot_state, write_generation) with TOCTTOU-safe acquire_read and zombie reclaim.
- **Recovery** – C API on top of `DataBlockDiagnosticHandle`: diagnose slot, force reset slot(s), release zombie writer/readers, cleanup dead consumers, validate integrity.

Layering is clear: platform → shm → header/slots → producer/consumer/diagnostic APIs.

### 1.2 Gaps / correctness

- **DataBlockMutex not used by DataBlock.** Design docs (HEP) say Tier 1 mutex protects control zone (e.g. spinlock alloc). Currently `DataBlock` has no mutex; spinlock allocation in `acquire_shared_spinlock` is lock-free (CAS on `owner_pid`). If multiple processes allocate spinlocks concurrently, that’s acceptable; if you later need to protect other header updates under a single lock, reintegrating DataBlockMutex is pending (see DATAHUB_TODO).
- **Consumer flexible_zone_info.** Consumer ctor that only takes `name` does not populate `m_flexible_zone_info` (comment: “handled in factory with expected_config”). So consumer-side flexible zone access by name may be wrong unless created via `find_datablock_consumer` with `expected_config`. Worth documenting and/or enforcing.
- **Integrity validation (repair path).** `datablock_validate_integrity` with `repair=true` creates a full producer/consumer (MessageHub) to recalc checksums. A **lighter** path (verify + checksum-only repair using only `DataBlockDiagnosticHandle`, no broker) is planned; see **`docs/DATAHUB_DESIGN_DISCUSSION.md`** §2 for definition and **Integrity policy** (§2.4).
- **Clock for timeouts.** Addressed: `shared_memory_spinlock.cpp` now uses `platform::monotonic_time_ns()` and `elapsed_time_ns()` for timeout (aligned with data_block and recovery).

---

## 2. API Design

### 2.1 Strengths

- **pImpl** used for public types (`DataBlockProducer`, `DataBlockConsumer`, `DataBlockDiagnosticHandle`, slot handles, iterator); ABI and compilation firewalling are in good shape.
- **C API** for slot RW (`slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`, `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`) and recovery is clear and usable from C.
- **DataBlockDiagnosticHandle** – read-only attach for recovery/tooling without creating a full producer/consumer is the right abstraction.
- **Slot write/read flow** – acquire → use → commit or release / validate_read → release_read – is clear and matches the design.

### 2.2 Inconsistencies / improvements

- **DataBlockProducer::release_write_slot** – Returns `bool`; other APIs mix returns, `nullptr`, and exceptions. Document when it returns false (e.g. already released / invalid handle).
- **Slot handle lifetime** – SlotWriteHandle and SlotConsumeHandle hold pointers into shared memory. Users must release or destroy all handles before destroying the producer or consumer; otherwise use-after-free. Documented in data_block.hpp and data_block.cpp.
- **Factory vs direct ctor.** Producers/consumers are created via factory (`create_datablock_producer`, `find_datablock_consumer`). DataBlockMutex is only used in tests with direct ctor; when reintegrated, decide whether creation is factory-only or also direct (and document exception vs optional/expected).
- **C recovery API error reporting.** Return codes (0, -1, -2, …) and `RecoveryResult` are documented in headers; a single place (e.g. `utils/recovery_api.hpp` or a short recovery.md) listing all codes and meaning would help.

### 2.3 Cross-platform consistency (Windows, macOS, Linux, FreeBSD)

The four files use only **plh_platform.hpp** macros (`PYLABHUB_PLATFORM_WIN64`, `PYLABHUB_IS_POSIX`) and **pylabhub::platform::\*** APIs. There are no raw `#ifdef _WIN32` or `#ifdef __linux__` (or similar) in DataBlock, spinlock, or recovery code, in line with DATAHUB_TODO’s cross-platform rules.

| File | Platform branching | Behavior / semantics |
|------|--------------------|----------------------|
| **data_block.cpp** | `PYLABHUB_IS_POSIX`: creator calls `shm_unlink` before create (POSIX-only; Windows has no equivalent in same call). `PYLABHUB_PLATFORM_WIN64`: error message uses `GetLastError()`; else uses `errno`. | **Identical semantics:** create/attach, timeouts (ms), magic wait, version/size checks. Only error reporting differs. |
| **data_block_mutex.cpp** | Full split: **Windows** = named kernel mutex (`CreateMutexA` / `OpenMutexA`); **POSIX** = `pthread_mutex_t` in shm (embedded or dedicated segment via platform `shm_*`). | **Aligned semantics:** lock/unlock; abandoned-owner recovery (Windows: `WAIT_ABANDONED`; POSIX: `EOWNERDEAD` + `pthread_mutex_consistent`). POSIX path is the same for macOS, Linux, FreeBSD (single `#else`). |
| **shared_memory_spinlock.cpp** | None. | Uses only `platform::get_pid`, `get_native_thread_id`, `monotonic_time_ns`, `elapsed_time_ns`, `is_process_alive` and atomics. **Identical on all platforms.** |
| **data_block_recovery.cpp** | None. | Uses only `platform::monotonic_time_ns`, `open_datablock_for_diagnostic`, and data_block types. **Identical on all platforms.** |

**Timeouts:** All timeouts are in milliseconds and use `platform::monotonic_time_ns()` / `elapsed_time_ns()` (or platform-backed paths in the mutex), so behavior is monotonic and consistent across Windows, macOS, Linux, and FreeBSD.

**PID and liveness:** All use `platform::get_pid()` and `platform::is_process_alive()`; PID 0 is “not alive” on all platforms (enforced in the platform layer).

**Assumption:** `platform.cpp` implements `get_pid`, `get_native_thread_id`, `monotonic_time_ns`, `elapsed_time_ns`, `is_process_alive`, and `shm_create` / `shm_attach` / `shm_close` / `shm_unlink` for Windows, macOS, Linux, and FreeBSD. As long as that holds, the investigated modules have consistent behavior and design across these four platforms.

---

## 3. Completeness

- **Slot RW:** Implemented and used by producer/consumer and C API. Metrics (e.g. writer_timeout_count, reader_race_detected) are updated when header is provided.
- **Checksums:** Flexible zone and slot checksums (BLAKE2b), update/verify paths, and repair in integrity validation are present.
- **Schema:** Producer stores schema hash/version in header; consumer validates in `find_datablock_consumer_impl`; mismatch increments `schema_mismatch_count`.
- **Recovery:** Diagnose slot, force reset one/all slots, release zombie writer/readers, cleanup dead consumers, validate integrity (and repair). C API is complete for the current design.
- **Missing from design (not bugs):** DataBlockMutex integration into DataBlock (control zone), broker schema registry (Phase 1.4), optional GET_SCHEMA API.

---

## 4. Duplication & Redundancy

### 4.1 PID and time

- **data_block.cpp** – Local `get_current_pid()` that just calls `pylabhub::platform::get_pid()`. Redundant; call `pylabhub::platform::get_pid()` directly and remove the wrapper.
- **shared_memory_spinlock.cpp** – `SharedSpinLock::get_current_pid()` and `get_current_thread_id()` wrap platform. Keeping thin wrappers in the class is acceptable for encapsulation, but they duplicate the concept; ensure they are not duplicated elsewhere (e.g. no second “get_pid” helper in data_block).

### 4.2 Backoff and timeout

- **data_block.cpp** – Uses `pylabhub::utils::backoff(iteration)` (from backoff_strategy.hpp) and `platform::monotonic_time_ns()` / `elapsed_time_ns()`.
- **shared_memory_spinlock.cpp** – Uses `pylabhub::utils::ExponentialBackoff` and `std::chrono::high_resolution_clock`. Logic is similar (spin + backoff + timeout) but implemented separately. Unify on platform monotonic time; consider reusing a single backoff policy or a shared “spin_until_timeout” helper to avoid drift in behavior.

### 4.3 Zombie / liveness

- **data_block.cpp** – `acquire_write` and `check_consumer_health` use `pylabhub::platform::is_process_alive()` and force-reclaim or clear heartbeats.
- **shared_memory_spinlock.cpp** – `try_lock_for` uses `is_process_alive` for zombie lock reclaim.
- **data_block_recovery.cpp** – Multiple C APIs use `is_process_alive` for safe reset / zombie release.

No duplication of liveness logic; all go through platform. Good.

### 4.4 Recovery “boilerplate”

- **data_block_recovery.cpp** – Every C API repeats: `open_datablock_for_diagnostic(shm_name)` → `handle->header()` null check → optional `slot_index` bounds check → `handle->slot_rw_state(slot_index)` → same catch blocks and logging. This is a lot of repeated code.
- **Suggestion:** Add an internal helper, e.g. `RecoveryContext` or `with_diagnostic_handle(shm_name, callback)` that opens the handle, checks header, and invokes a lambda with `(handle*, header*)` (and optionally slot_index validation). Slot-level APIs can then use a second helper that also resolves `slot_rw_state`. Reduces duplication and keeps error codes consistent.

### 4.5 Timestamp for “last error” / “last heartbeat”

- **data_block_recovery.cpp** – Several places set `header->last_error_timestamp_ns` (or similar) using `std::chrono::high_resolution_clock::now()`. Rest of codebase uses `pylabhub::platform::monotonic_time_ns()` for timeouts. For consistency and monotonicity, use a single convention (e.g. platform monotonic or a shared “now_ns()” that’s documented).

---

## 5. Code That Could Move to Other Modules

### 5.1 Checksum helpers (data_block.cpp)

- `update_checksum_flexible_zone_impl`, `update_checksum_slot_impl`, `verify_checksum_flexible_zone_impl`, `verify_checksum_slot_impl` are long and work on `DataBlock*` and header layout. They are only used by producer/consumer and (indirectly) by integrity validation.
- **Option A:** Leave in data_block.cpp as internal helpers (current).  
- **Option B:** Move to a `data_block_checksum.cpp` (or `integrity_checksum.cpp`) that takes `SharedMemoryHeader*`, buffer pointers, and config (sizes, offsets) so that recovery/integrity can use them without a full DataBlock. That would simplify repair without creating a producer.

### 5.2 Header layout and schema (data_block.cpp)

- `get_shared_memory_header_schema_info()` builds a large BLDS description of `SharedMemoryHeader` and is used for layout hash and consumer validation. It’s ~100+ lines of field listing.
- **Suggestion:** Keep in data_block (or a dedicated `data_block_header_schema.cpp`) so that layout hash stays next to the header definition. If multiple modules need to “describe” the header for diagnostics, consider a single shared routine rather than duplicating.

### 5.3 Slot RW state machine (data_block.cpp anonymous namespace)

- `acquire_write`, `commit_write`, `release_write`, `acquire_read`, `validate_read`, `release_read` – these are the core slot coordination algorithms. They are used by C++ producer/consumer and by the C API wrappers.
- **Suggestion:** Keep in data_block.cpp; they are the heart of the DataBlock slot protocol. Extracting to a `slot_rw.cpp` could be done for clarity, but would require passing header/metrics and slot state by pointer from data_block anyway; current colocation is reasonable unless you want a standalone C-only library.

### 5.4 DataBlockMutex (data_block_mutex.cpp)

- Already a separate module; uses platform shm API for the dedicated-segment path. No need to move; when reintegrated into DataBlock, it stays as a dependency, not merged into data_block.cpp.

---

## 6. File Size and Structure

- **data_block.cpp** – Very large (~2250 lines). It contains:
  - Slot RW logic (anonymous namespace)
  - DataBlock and DataBlockDiagnosticHandle impl
  - Checksum helpers
  - Slot handles and producer/consumer impl
  - DataBlockSlotIterator
  - C slot_rw_* wrappers
  - Header schema (get_shared_memory_header_schema_info, validate_header_layout_hash)
  - Factory functions (create_datablock_producer, find_datablock_consumer)
  - open_datablock_for_diagnostic

**Suggestions (optional refactors):**

- **data_block_slot_rw.cpp** – Move anonymous `acquire_write`/`commit_write`/… and the C wrappers `slot_rw_*` into a separate TU; data_block.cpp includes or links it. Keeps data_block.cpp focused on DataBlock lifecycle, handles, and factories.
- **data_block_factory.cpp** – Move `create_datablock_producer_impl`, `find_datablock_consumer_impl`, and the public factory overloads to a separate file. Reduces size and separates “construction and discovery” from “runtime slot and checksum logic.”
- **data_block_header_schema.cpp** – Move `get_shared_memory_header_schema_info` and `validate_header_layout_hash` if you want a clearer “header ABI” boundary.

These are structural improvements; correctness does not depend on them.

---

## 7. shared_memory_spinlock.hpp

- **Comment:** The file is now `shared_memory_spinlock.hpp`; banner and comment updated. No legacy filename in banner.
- **Includes:** It includes POSIX headers (`fcntl.h`, `pthread.h`, `sys/mman.h`, etc.). `SharedSpinLock` does not use shm or pthread directly; it only uses atomics and platform (for PID and liveness). Those includes look legacy; remove them if unused so the module stays “platform + atomics only.”

---

## 7. C API and test coverage review (2026-02-12)

This section evaluates the **primitive C APIs** (Slot RW and Recovery/Diagnostics) and the tests built around them. MessageHub has no C API (C++ only); its public C++ API is the primitive for broker integration.

### 7.1 Slot RW C API (`slot_rw_coordinator.h`)

**Surface:** `slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`, `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`, `slot_rw_get_metrics`, `slot_rw_reset_metrics`, `slot_acquire_result_string`; types `SlotAcquireResult`, `DataBlockMetrics`.

**Tests:** Layer-0 isolation (`test_layer2_service/test_slot_rw_coordinator.cpp`): single `SlotRWState` + synthetic header in plain memory. Covers writer acquire/commit/release, reader acquire/validate/release, generation wrap-around, metrics get/reset, high-contention stress. Integration: DataBlock slot protocol and writer_timeout_metrics_split use the same protocol; slot_rw_get_metrics/reset exercised in slot_protocol_workers.

**Gaps:** `slot_acquire_result_string` not directly asserted; explicit tests for SLOT_ACQUIRE_TIMEOUT / NOT_READY return codes could be added.

**Conclusion:** Slot RW C API is **complete and correct**. Safe to treat as stable base for C++ abstraction.

### 7.2 Recovery / diagnostics C API (`recovery_api.hpp`)

**Surface:** `datablock_diagnose_slot`, `datablock_diagnose_all_slots`, `datablock_is_process_alive`, `datablock_force_reset_slot`, `datablock_force_reset_all_slots`, `datablock_release_zombie_readers`, `datablock_release_zombie_writer`, `datablock_cleanup_dead_consumers`, `datablock_validate_integrity`; types `SlotDiagnostic`, `RecoveryResult`.

**Tests:** Via C++ wrappers (recovery_workers): is_process_alive, SlotDiagnostics (diagnose_slot), SlotRecovery (release_zombie_readers), IntegrityValidator (validate_integrity). Direct C API for force_reset, release_zombie_writer, cleanup_dead_consumers, diagnose_all_slots exercised by datablock_admin; no dedicated unit test per C function.

**Conclusion:** Recovery C API is **complete and correctly specified**. Test coverage adequate for current policy; recovery-scenario tests deferred per DATAHUB_TODO.

### 7.3 MessageHub (no C API)

C++ only. No-broker tests in place; with-broker after C++ abstraction.

### 7.4 Overall verdict

| API            | Completeness | Test coverage        | Verdict |
|----------------|-------------|----------------------|---------|
| Slot RW C API  | Complete    | Strong               | **Stable base** for C++ layer. |
| Recovery C API | Complete   | Adequate (wrappers)   | **Accepted**; add scenario tests when defined. |
| MessageHub     | N/A (C++)   | No-broker covered    | Primitive for broker; build on C++ abstraction. |

Proceed with C++ abstraction design and implementation on top of the C API; use C API directly only when performance or flexibility require it.

---

## 8. Summary Table

| Area                 | Assessment | Action |
|----------------------|------------|--------|
| Layering             | Good       | Keep; document DataBlockMutex reintegration in TODO. |
| Slot RW correctness  | Good       | None. |
| Clock/timeouts       | Addressed   | Spinlock and recovery now use platform monotonic time. |
| Recovery boilerplate | Addressed   | `open_for_recovery()` + `RecoveryContext`; `set_recovery_timestamp()`. |
| get_current_pid in data_block | Addressed | Removed; use `platform::get_pid()` directly. |
| Backoff/time in spinlock | Addressed | Timeout now uses `platform::monotonic_time_ns()` / `elapsed_time_ns()`. |
| data_block.cpp size  | Very large | Optional: split slot_rw, factory, header schema into separate TUs. |
| Checksum repair      | Heavy (full producer) | Optional: low-level repair using diagnostic handle only. |
| shared_memory_spinlock.hpp | Comment and includes | Fixed; module renamed from data_block_spinlock to reflect shared-memory abstraction. |
| Consumer flexible_zone_info | May be empty | Document; ensure factory path always sets it when flexible zones used. |
| C API + test coverage       | Reviewed     | §7: Slot RW and Recovery C APIs complete and correct; tests adequate. Proceed with C++ abstraction. |

---

## 9. Code split plan and schema module (for API separation)

### 9.1 Why complete the schema module first

The **schema module** (`schema_blds.hpp`) already provides:

- BLDS type IDs, `SchemaInfo`, `SchemaVersion`, `generate_schema_info` (generic), `validate_schema_match` / `validate_schema_hash`, `SchemaValidationException`.

What is still **inside data_block** and schema-related:

- **Header layout schema** – `get_shared_memory_header_schema_info()` and `validate_header_layout_hash(header)` live in `data_block.cpp`. They build the BLDS for `SharedMemoryHeader` and compare it to `header->reserved_header` hash.

**Completing the schema module** (for better integration/separation) can mean:

1. **Move header layout schema into a schema-owned TU**  
   - Add e.g. `schema_header.cpp` (or `data_block_header_schema.cpp` in utils) that implements `get_shared_memory_header_schema_info()` and optionally `validate_header_layout_hash()`.  
   - Depends on `SharedMemoryHeader` (from data_block.hpp) but not on DataBlock lifecycle.  
   - Result: “Layout description and ABI validation” live under the schema/layout concern; data_block only uses the validation function and stores the hash at creation.

2. **Broker schema registry (Phase 1.4)**  
   - Broker stores `schema_hash`, `schema_version`, `schema_name`; DISC_RESP carries them.  
   - Enables discovery by schema without opening every producer; tooling and consumers can filter by schema.  
   - Complements the existing producer/consumer schema validation.

3. **Optional: GET_SCHEMA API (Phase 1.4)**  
   - Request/response for “return BLDS + hash + version” for a channel or shm name.  
   - Enables tooling and docs to show schema without linking producer code.

**Recommendation:** Complete (1) when you are ready to split: move header schema to a dedicated TU that uses `schema_blds` types and only depends on `SharedMemoryHeader`. Then do (2) and optionally (3) for broker and API separation. That gives a clear boundary: schema module = “describe and validate layout/schema”; data_block = “runtime shm and slots”; broker = “registry and discovery.”

### 9.2 Code split order (after schema boundary is clear)

| Step | Content | Rationale |
|------|--------|-----------|
| 1 | **Header schema** | Move `get_shared_memory_header_schema_info` and `validate_header_layout_hash` to e.g. `data_block_header_schema.cpp` (or schema module if you add a dependency from schema → SharedMemoryHeader). Keeps ABI/layout concern in one place. |
| 2 | **Slot RW** | Move anonymous `acquire_write`/`commit_write`/… and C wrappers `slot_rw_*` to `data_block_slot_rw.cpp`. data_block.cpp keeps DataBlock, handles, and producer/consumer; slot protocol lives in one TU. |
| 3 | **Factory** | Move `create_datablock_producer_impl`, `find_datablock_consumer_impl`, and public factory overloads to `data_block_factory.cpp`. Shrinks data_block.cpp and separates “construction and discovery” from “runtime.” |
| 4 | **Checksum** (optional) | Move `update_checksum_*_impl` and `verify_checksum_*_impl` to `data_block_checksum.cpp` with a header/buffer-only interface so integrity repair could use them without a full producer (future improvement). |

Do **not** split for the sake of it: do step 1 when you want a clear schema/layout boundary; do 2–3 when data_block.cpp size or build times justify it.

### 9.3 Duplication and redundancy – addressed (2026-02-10)

- **get_current_pid in data_block.cpp** – Removed; callers use `pylabhub::platform::get_pid()` directly.
- **Spinlock timeout clock** – `shared_memory_spinlock.cpp` now uses `pylabhub::platform::monotonic_time_ns()` and `elapsed_time_ns()` for timeout (aligned with data_block).
- **Recovery boilerplate** – Added `open_for_recovery(shm_name_cstr)` and `RecoveryContext`; all recovery C APIs use it. Added `set_recovery_timestamp(header)` using `platform::monotonic_time_ns()`.
- **shared_memory_spinlock.hpp** – Renamed from data_block_spinlock; `@file` and brief updated; module is abstraction over shared memory, not DataBlock-specific.

---

## 10. References

- `cpp/docs/DATAHUB_TODO.md` – DataBlockMutex follow-ups, Phase 1.4/1.5, Phase 2.
- `cpp/docs/hep/HEP-CORE-0002-DataHub-FINAL.md` – Intended Tier 1 (DataBlockMutex) vs Tier 2 (spinlock) and slot semantics.
