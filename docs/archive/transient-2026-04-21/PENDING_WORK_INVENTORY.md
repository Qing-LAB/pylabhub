# Pending Work Inventory — pyLabHub (2026-04-21)

**Status**: 🔵 Scope-visibility snapshot (point-in-time; re-run to refresh).
**Created**: 2026-04-21.
**Purpose**: Single consolidated view of all open TODOs + deferred items
across the project, so priorities can be set and cruft removed.
**Re-generation**: Manual audit across `docs/todo/*.md`, `docs/TODO_MASTER.md`,
HEP Status fields, tech-draft statuses. Update after each major sprint close.

---

## 1. Headline state

- Test suite: **1456/1456 passing** on Linux (as of 2026-04-21 after
  Stage 1 of role unification Phase 20 cleanup completed).
- Active branch: `feature/lua-role-support`.
- Next planned major work: **HEP-CORE-0033 Hub Character** (design ratified
  2026-04-21; implementation not started).
- Blocked on HEP-0033: hub-involved L4 tests; hub-side metrics pull design.

---

## 2. By bucket

### 2.1 HEP-0033 Hub Character — PRIORITY 0

- **Prereqs** (must land before Phase 1): see
  `docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md` — 13 items (G1–G13);
  8 are spec gaps, 4 are small code/CMake changes, 1 is a rename ripple.
- **Phases**: 10 implementation phases per HEP-0033 §14.
- **Reference**: `docs/HEP/HEP-CORE-0033-Hub-Character.md`,
  `docs/tech_draft/HUB_CHARACTER_DESIGN.md`.

### 2.2 ABI / Primitive API — `docs/todo/API_TODO.md`

- **ABI phase work** (7 items): Phase 2 (TEST_EXPORT tagging) through
  Phase 7 (`std::filesystem::path` → `const char*` in C API, 31 signatures
  across 10 classes). Large aggregate scope.
- **Template RAII** (3 items): template factories on SHM queue were
  removed during the Ownership refactor; needs restoration.
  `SlotIterator::configured_period_us` plumbing broken.
- **ABI check facility** (HEP-CORE-0026 extension): compile-time struct
  layout validation. Design doc in `docs/tech_draft/abi_check_facility_design.md`.
- **Engine / script integration**:
  - SE-03 (HIGH): HEP-CORE-0011 (ScriptHost Abstraction Framework)
    **stale — needs rewrite** to match RoleAPIBase + composition model.
  - SE-07: `--validate` CLI flag impl.
  - SE-08: HEP-0018/0015 class-name reference updates.
- **Engine thread model** (6 phases, see
  `docs/tech_draft/engine_thread_model.md`): invoke/eval dispatch, cross-thread
  dispatch per engine, shared state, NativeEngine plugin.
- **Diagram / doc gaps** (7 items): protocol sequence diagrams, data-loop
  state machine, queue abstraction class diagram, metrics flow diagram,
  thread-per-role inventory, cross-thread dispatch per engine, callback
  thread placement.
- **Lifecycle / config**: `engine_lifecycle_startup` integration in role
  hosts; `RoleHostCore` encapsulation (CR-03); hubshell migration to
  `PythonEngine` (low priority; subsumed into HEP-0033); obsolete thread-name
  references in docs.

### 2.3 Broker / Messenger / Hub — `docs/todo/MESSAGEHUB_TODO.md`

- HEP-CORE-0033 open section (tracked in §2.1).
- **Deferred backlog**: file-based discovery, protocol version handshake,
  embedded broker mode (for testing), connection pooling, per-frame checksum
  (BLAKE2b-32) + type-tag handshake (HEP-0023 deferred data-plane safety),
  user-facing Python SDK (pip-installable).

### 2.4 Testing — `docs/todo/TESTING_TODO.md`

- **Framework sweep deferred** (2 items): delete V2 `LuaEngineTest` wrapper
  when last V2 test converts; unify worker-side script helpers (~70%
  duplication).
- **Cross-engine Script API contract** (live-vs-frozen spec): `log_level`
  LIVE, path accessors FROZEN, other accessors LIVE — needs spec table +
  Lua/Python/Native binding audit (~3 days).
- **Coverage gaps** (8 items): invoke_consume discard path, custom slot
  name read-only default, rx-write-in-invoke_on_inbox, ZMQ checksum policy
  execution, config key whitelist edge cases (Unicode/prefix/nested/empty),
  L3 padding-sensitive schema round-trip, L3 SHM complex schema round-trip,
  L3 aligned-vs-packed same-data comparison.
- **Stress-level calibration** (4 files): filelock multi-thread/multi-process
  tests hardcode counts; should use `get_stress_num_threads` / `get_stress_iterations`.
- **Platform coverage** (5 items): Windows, macOS, FreeBSD CI builds; ASan;
  UBSan.
- **Native engine `supports_multi_state`** (2026-04-20 open):
  user-settable via optional symbol + HEP-CORE-0028 update.
- **L4 plh_role deferred broker tests** (6 items): lifecycle, round-trip,
  channel broadcast, processor pipeline, hub-dead detection, inbox round-trip.
  **Gated on HEP-0033** (need working hub binary).
- **Watchlist**: `DatahubShmQueueTest.ShmQueueWriteFlexzone` intermittent
  60s timeout (capped to 10ms in 1d3e584); BrokerProtocolTest timing audit.
- **Documentation staleness**: `README_testing.md` still says Phase C
  "to be implemented" — Phase C has been complete for months.

### 2.5 Platform / Windows / CMake — `docs/todo/PLATFORM_TODO.md`

- CI is Linux-only; docs advertise macOS/Windows/FreeBSD support.
- MSVC `/Zc:preprocessor` PUBLIC propagation audit.
- MSVC `/W4 /WX` gate for early C4251/C4324/C4996 catch.
- Full clang-tidy quality pass (infrastructure wired, not executed).

### 2.6 Structure refactor (deferred) — see `SRC_STRUCTURE_PLAN.md`

Three phases:
- Phase A: file moves (`src/utils/roles/`, `src/scripting/roles/`), `core` →
  `basic` rename, binary mains flat under `src/`.
- Phase B: include reorganization mirroring `src/utils/` subdirs.
- Phase C: umbrella-header public/internal audit.

Not scheduled; sits behind HEP-0033 + any active hot work.

### 2.7 HEP status — summary

- **28 HEPs total**; 25 implemented or active-maintained.
- HEP-CORE-0011 (ScriptHost Abstraction) — stale, rewrite needed.
- HEP-CORE-0026 (Version Registry) — design, not yet implemented (ABI check
  facility extension pending).
- HEP-CORE-0032 (ABI Compatibility) — draft.
- HEP-CORE-0033 (Hub Character) — design ratified 2026-04-21; pre-impl.

### 2.8 Tech drafts

- **Active/in-flight** (as of end of session 2026-04-21):
  - `HUB_CHARACTER_PREREQUISITES.md` — gap analysis; absorbed into HEP-0033 as prereqs land.
  - `SRC_STRUCTURE_PLAN.md` — deferred refactor plan.
  - `PENDING_WORK_INVENTORY.md` — this doc; point-in-time snapshot.
  - `abi_check_facility_design.md` — draft for HEP-CORE-0026 extension.
  - `engine_thread_model.md` — active per API_TODO.
  - `test_compliance_audit.md` — active for 21.L4/21.L5 Pattern 3 work.
  - `raii_layer_redesign.md` — partially implemented; review-then-decide.
- **Archived 2026-04-21** (5 drafts):
  - `HUB_CHARACTER_DESIGN.md` → promoted to HEP-CORE-0033 (content-complete merge verified, with §6.5 vault/keygen added during absorb pass)
  - `config_module_design.md` → verified fully implemented; rationale in HEP-CORE-0024
  - `role_unification_design.md` → CLOSED; L3.β/L3.ε intentionally not pursued (rationale in HEP-CORE-0024 §15.2/15.3)
  - `invoke_convention_redesign.md` → CLOSED; implementation evolved past draft (flexzone removed from invoke structs; Rx/Tx runtime vs in/out config naming)
  - `datablock_queue_ownership.md` → CLOSED; all done (Step 11 was actually done; §5.2 + §6 stale references to deleted entities); authoritative spec is HEP-CORE-0002 §17.2/§17.2.1

### 2.9 Code reviews — `docs/code_review/`

- **None active**. All recent reviews closed + archived to
  `docs/archive/transient-2026-03-12/` (2026-03-12).

---

## 3. Rough size estimates

| Bucket | Item count | Estimated effort (person-days) |
|---|---|---|
| HEP-0033 hub character (prereqs + 10 phases) | ~25 | 20-30 |
| ABI phases 2-7 | 6 | 8-15 |
| ABI check facility (HEP-0026 ext) | 1 | 3-5 |
| Template RAII restoration | 3 | 2-4 |
| HEP-0011 rewrite (SE-03) | 1 | 2-3 |
| Engine thread model phases | 6 | 5-8 |
| Testing coverage gaps + framework sweep | ~15 | 5-10 |
| Platform CI + MSVC | 4 | 3-5 |
| Structure refactor (Phases A/B/C) | 3 | 5-10 |
| Doc sync (HEP-0011, README_testing, diagrams) | ~10 | 3-5 |

Aggregate: on the order of **60-100 person-days** of un-prioritized work.
Hub character alone is the largest single block.
