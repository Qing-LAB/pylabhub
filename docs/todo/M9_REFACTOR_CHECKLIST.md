# M9 Refactor — Full File Checklist (durable record)

> **Purpose.**  Survive session compression / context loss.  Lists
> every file the M9 role-host-frame refactor touches or needs to touch,
> what's done, what's pending, and what to verify.  Created
> 2026-05-23 mid-refactor.
>
> Canonical design: `docs/tech_draft/role_host_template_design.md`
> §10.6 (architecture overview), §11 (frame design), §11.6 (setup
> consolidation), §11.9 (adding a new role).
>
> Task tracker: `#72` (Wave-B M9: RoleHostFrame plain class).

---

## Sub-step status

| Sub-step | Status | Commit |
|---|---|---|
| 2a — Frame skeleton + inheritance migration | ✅ done | `bc3f9340`, polish `f560ad40` |
| 2b — Absorb `teardown_infrastructure_` + inbox state | ✅ done | `e368d627`, polish `51422f10` |
| **Design pin: schemas → Presence + Presence struct extension** | ✅ done | `7a1033e0` |
| **2c Phase 1 — Presence-driven setup, schemas resolved by `build_presences_()`, frame's `setup_infrastructure_` body, per-role `setup_infrastructure_` removed** | ⏳ in progress (most edits done; needs commit + verification) | — |
| **2c Phase 2 — Remove RoleHostCore's `*_slot_spec_` + `*_fz_spec_` storage; update downstream consumers; update test fixtures** | ⏳ pending | — |
| 2c Phase 3 — L2 test rework (Q2 SchemaSpec coverage + Q3 has_fz=false) | ⏳ pending | — |

---

## Phase 1 — files (~10 files, in-progress)

| File | Change | Status |
|---|---|---|
| `src/include/utils/role_host_frame.hpp` | Add `presences_` member, `build_presences_()` pure virtual, `has_*_fz()` accessors, `setup_infrastructure_()` decl | done (uncommitted) |
| `src/utils/service/role_host_frame.cpp` | Implement `setup_infrastructure_()` (uses presences_ + make_*_opts) + `has_*_fz()` | done (uncommitted) |
| `src/producer/producer_role_host.hpp` | Add `build_presences_()` override decl; remove per-role `setup_infrastructure_` decl | done (uncommitted) |
| `src/producer/producer_role_host.cpp` | Implement `build_presences_()` (resolves schemas inline); update `worker_main_` to call it early; remove per-role `setup_infrastructure_` body | done (uncommitted) |
| `src/consumer/consumer_role_host.hpp` | Same surgery as producer | done (uncommitted) |
| `src/consumer/consumer_role_host.cpp` | Same surgery as producer | done (uncommitted) |
| `src/processor/processor_role_host.hpp` | Same surgery + processor has TWO presences | done (uncommitted) |
| `src/processor/processor_role_host.cpp` | Same surgery + processor `build_presences_()` returns Consumer + Producer | done (uncommitted) |

**Phase 1 transitional shadow** — KNOWN duplication retained for this phase:

- Each role's `worker_main_` still does the legacy step-1 schema-resolve into `*_slot_spec_` member + calls `core_.set_*_slot_spec()` / `set_*_fz_spec()`.
- `build_presences_()` ALSO resolves schemas (into Presence fields).
- Schemas resolved twice per role startup.  Acceptable transitional cost.
- Removed in Phase 2.

**Phase 1 verification**:
- ✅ `cmake --build build -j 2 --target stage_all` clean.
- ✅ `test_layer2_setup_infrastructure_translation` 6/6 PASS.
- ✅ `py-demo-lua-single-hub` PASS.
- TODO: `py-demo-single-processor-zmq` PASS.
- TODO: `py-demo-dual-hub-processor` PASS.
- TODO: Commit Phase 1 with a clear message marking the transitional shadow.

---

## Phase 2 — files (~10–12 files, pending)

Goal: eliminate the schema-storage shadow.  Single source of truth is
`Presence` per channel.  Update all downstream consumers.

| File | Change | Risk |
|---|---|---|
| `src/include/utils/role_host_core.hpp` | Remove `in_slot_spec_`, `out_slot_spec_`, `in_fz_spec_`, `out_fz_spec_` member declarations.  Remove `set_*_slot_spec()`, `set_*_fz_spec()`, `*_slot_spec()`, `*_fz_spec()`, `has_in_slot()`, `has_out_slot()`, `has_rx_fz()`, `has_tx_fz()` methods.  KEEP scalar size caches (`*_slot_size_`, `*_fz_size_`) — those are not duplicate state, they're hot-path caches. | Significant — public API |
| `src/utils/service/role_host_core.cpp` | Remove corresponding implementations.  KEEP size-cache methods. | Significant |
| `src/producer/producer_role_host.cpp` | Remove legacy step-1 schema-resolve block.  Remove calls to `core_.set_out_slot_spec()` / `set_out_fz_spec()`.  Update `wire_slot_schema` callers + similar that read `out_slot_spec_` / `core_.out_fz_spec()` to read from `presences_[i].slot_spec` / `presences_[i].fz_spec` instead.  Step 6's inline presence build: replace with read from `presences_`. | Significant — schema readers throughout |
| `src/producer/producer_role_host.hpp` | Remove `out_slot_spec_` member.  Keep build_presences_ override. | Trivial after .cpp |
| `src/consumer/consumer_role_host.cpp` | Symmetric surgery to producer.  In particular `compute_in_message_info(...)` call passes `in_slot_spec_` + `core_.in_fz_spec()` — update both. | Significant |
| `src/consumer/consumer_role_host.hpp` | Remove `in_slot_spec_` member | Trivial |
| `src/processor/processor_role_host.cpp` | Symmetric (BOTH directions).  Two `compute_*_message_info` calls. | Significant |
| `src/processor/processor_role_host.hpp` | Remove `in_slot_spec_` + `out_slot_spec_` members | Trivial |
| `src/utils/service/role_api_base.cpp` | Lines ~1964-1978: schema introspection reads `core_.in_fz_spec()`, `core_.out_fz_spec()`, `core_.has_*_fz()`.  Migrate to read from queue's internal opts (queue already has fz_spec via build_*_queue), OR add accessor that frame populates at setup time. | **Blocker risk** — needs careful design |
| `src/utils/service/engine_module_params.cpp` | Lines 58, 61, 89, 94: `core_.has_*_fz()` consistency assertions.  Switch to frame's `has_*_fz()`. | Trivial (4 lines) |
| `tests/test_layer3_datahub/workers/role_api_flexzone_workers.cpp` | 8+ calls to `core_.set_*_fz_spec()` in test fixtures.  Migrate to construct Presence with fz_spec set OR use new API. | Significant — test fixture pattern |
| `tests/test_layer2_service/workers/python_engine_workers.cpp` | 2 calls to `core_.set_*_fz_spec()` | Trivial |
| `tests/test_layer2_service/workers/lua_engine_workers.cpp` | 2 calls | Trivial |
| `tests/test_layer2_service/workers/scriptengine_native_dylib_workers.cpp` | 1 call | Trivial |
| `tests/test_layer2_service/test_role_host_core.cpp` | Remove `has_*_fz()` and `*_fz_spec()` assertions (lines 172-173, 439, 441-442, 447, 456, 458-459).  Tests for the removed API surface are no longer applicable. | Significant — test rewrite |

**Phase 2 risk: `RoleAPIBase` introspection**.  This is the only
downstream consumer that's NOT a role-host file.  Two viable
strategies:
1. Have queue objects expose their stored fz_spec (queue already
   stores it internally; just needs an accessor).
2. Add a `set_introspection_fz_spec()` method on RoleAPIBase that
   the frame's `setup_infrastructure_` calls per direction after
   queue construction.

**Verify Phase 2**:
- `cmake --build build -j 2 --target stage_all` clean.
- `test_layer2_setup_infrastructure_translation` 6/6 PASS.
- `test_role_host_core` updated tests PASS.
- `test_layer3_role_api_flexzone` PASS.
- `test_layer2_python_engine` PASS.
- `test_layer2_lua_engine` PASS.
- `test_layer2_scriptengine_native_dylib` PASS.
- All 9 demos under `share/py-demo-*/` PASS.
- Mutation sweep: deliberately break a per-presence schema copy in `make_*_opts`; confirm an L2 test catches it.

---

## Phase 3 — files (~3 files, pending)

L2 test rework — Q2 (SchemaSpec propagation with non-empty specs) +
Q3 (`has_fz=false` per direction).  See design doc §11.6.4.

| File | Change |
|---|---|
| `tests/test_layer2_service/test_setup_infrastructure_translation.cpp` | Add tests using non-empty `SchemaSpec` with at least one field; assert propagation through `make_*_opts`.  Add `has_fz=false` case per direction. |
| Possibly new test: `tests/test_layer2_service/test_build_presences.cpp` | Per-role unit tests for `build_presences_()` — verify hub/channel/role_kind + slot_spec + fz_spec all populated correctly from config. |

---

## Reference: existing schema-storage locations to remove in Phase 2

Surveyed 2026-05-23.  See design doc §11.6.5 for the table.

### In `RoleHostCore` (`src/include/utils/role_host_core.hpp`):
- Line 178: `void set_in_slot_spec(SchemaSpec spec, size_t logical_size)`
- Line 180: writes `in_slot_spec_`
- Line 183: `void set_out_slot_spec(SchemaSpec spec, size_t logical_size)`
- Line 185: writes `out_slot_spec_`
- Line 188: `in_slot_spec()` getter
- Line 189: `out_slot_spec()` getter
- Line 192: `has_in_slot()` derived
- Line 193: `has_out_slot()` derived
- Line 197: `set_in_fz_spec(...)` setter
- Line 199: writes `in_fz_spec_`
- Line 202: `set_out_fz_spec(...)` setter
- Line 204: writes `out_fz_spec_`
- Line 207: `in_fz_spec()` getter
- Line 208: `out_fz_spec()` getter
- Line 211: `has_rx_fz()` derived
- Line 212: `has_tx_fz()` derived
- Line 614: `SchemaSpec in_slot_spec_` member
- Line 615: `SchemaSpec out_slot_spec_` member
- Line 618: `SchemaSpec in_fz_spec_` member
- Line 619: `SchemaSpec out_fz_spec_` member

### Role host members to remove:
- `ProducerRoleHost::out_slot_spec_` (producer_role_host.hpp:98 or thereabouts)
- `ConsumerRoleHost::in_slot_spec_` (consumer_role_host.hpp similar)
- `ProcessorRoleHost::in_slot_spec_` + `out_slot_spec_` (processor_role_host.hpp similar)

### Schema-resolve sites in role hosts (to remove from worker_main_ step 1):
- `producer_role_host.cpp:144` — `hub::resolve_schema(...)` for slot
- `producer_role_host.cpp:146` — `hub::resolve_schema(...)` for fz
- `consumer_role_host.cpp:139, 141` — same
- `processor_role_host.cpp:145, 147, 149, 151` — same

Schema-resolve for inbox spec stays (it's per-role, not per-channel):
- `producer_role_host.cpp:149`
- `consumer_role_host.cpp:144`
- `processor_role_host.cpp:154`

### Schema-spec reader call sites (need to update from `core_.` / member to `presences_[i].`):
- `producer_role_host.cpp:330` — `wire_slot_schema(..., out_slot_spec_, core_.out_fz_spec())`
- `consumer_role_host.cpp:297` — symmetric
- `processor_role_host.cpp:351, 360` — both directions
- `engine_module_params.cpp:58, 61, 89, 94` — `has_*_fz()` consistency checks

---

## Maintenance

- Update this checklist as each item completes.
- Archive after Phase 3 ships + design doc §11.9 finalizes.
- Per `docs/DOC_STRUCTURE.md` §1.7: transient docs are in `docs/todo/`; archive on completion to `docs/archive/transient-YYYY-MM-DD/`.
