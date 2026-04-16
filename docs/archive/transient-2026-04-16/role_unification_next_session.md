# Next Session Continuation: Role Unification (L3)

**Authored**: 2026-04-14 (end of this-session handoff)
**Branch**: `feature/lua-role-support`
**Baseline tests**: 1275/1275 passing at commit `e05a338` (L3.α end-state).
**Primary design doc**: `docs/tech_draft/role_unification_design.md`
**Superseded doc**: `docs/tech_draft/unified_role_loop.md` (historical only)

---

## 1. Where we are

### 1.1 Done in prior sessions

- HEP-CORE-0023 Phase 2 landed: heartbeat-multiplier role-liveness state
  machine, role-close cleanup hooks (federation + band), `RoleStateMetrics`
  counters, no-skip timeout floor, voluntary-vs-heartbeat-death path
  separation. Commits `cf53ed3`, `3201e08`, `6558b2c`, `1edea38`.
- Doc refresh: `unified_role_loop.md` marked superseded, `loop_design_unified.md`
  API-verified current, HEP-CORE-0011 obsolete-term scrub, `API_TODO.md`
  updated. Commits `696c101`, `13f4525`, `66b785c`, `e5eaf0d`.

### 1.2 Done in this (last) session

- **L3 role-unification design fully specified** in
  `docs/tech_draft/role_unification_design.md`:
  - §1-2: principles (role identity is data not class shape; engine is
    framework-agnostic; four concern groups carving RoleAPIBase).
  - §3: target architecture (delete `hub::Producer`/`hub::Consumer`; flat
    `write_*`/`read_*` verbs on RoleAPIBase; three empty `*RoleHost`
    subclasses as extension points; plain-struct `*ExtConfig` by value
    with no ExtConfig base class; engine handle-based with
    `role::callback_name_for()` in a role-layer helper).
  - §4: timing-invariance constraint, authoritative timing reference
    (`loop_design_unified.md`), §4.3 ten-item timing-parity checklist,
    §4.5a test-evaluation principles (built-in metrics + harness
    measurements; forbidden to rely on return values), §4.5b
    six-test baseline suite spec, §4.5 per-phase timing-risk table,
    §4.6 eight-item discrepancy-source checklist for post-β diagnosis.
  - §5: six-phase implementation plan (L3.α → L3.ζ).
- **L3.α implemented and committed as `e05a338`**:
  - Added flat data-plane verbs on `RoleAPIBase`: `write_acquire`,
    `write_commit`, `write_discard`, `read_acquire`, `read_release`,
    `write_item_size`, `read_item_size`, `write_flexzone_size`,
    `read_flexzone_size`, `sync_flexzone_checksum`.
  - Three CycleOps classes (`ProducerCycleOps`, `ConsumerCycleOps`,
    `ProcessorCycleOps`) migrated from raw `hub::Producer&`/`hub::Consumer&`
    refs to `scripting::RoleAPIBase&`. Mechanical substitution only; loop
    frame in `run_data_loop` untouched.
  - 1275/1275 tests green.
- Doc commits `75aa26f`, `71f14ef`, `e1a9ea8`, `0e77dcf`.

### 1.3 Stashed but not committed

There is a WIP stash titled **"WIP L3.β — unified CycleOps, uncommitted"**
from this session. It contains an attempted L3.β implementation (unified
`scripting::CycleOps` in role_api_base.hpp/cpp + migrated role hosts).
**Do not just pop it** — the β plan changed in this session to require a
baseline test suite written first. Either re-implement from scratch after
the baseline suite is in place, or pop and review carefully against the
updated plan. `git stash list` to find it.

---

## 2. What to do next (in strict order)

### Step 1 — housekeeping (small)

1. `git push origin feature/lua-role-support` — the last 4 commits
   (`71f14ef`, `e1a9ea8`, `0e77dcf`, and commit `e5eaf0d` if not
   already pushed) haven't been pushed to origin yet. Check
   `git log origin/feature/lua-role-support..HEAD --oneline` to
   confirm what's ahead.
2. Confirm 1275/1275 still green:
   `cmake --build build -j2 && ctest --test-dir build -j2 --output-on-failure`

### Step 2 — baseline test suite (BEFORE L3.β)

Per `role_unification_design.md` §4.5 through §4.5f. This is a standalone,
dedicated work item — estimated 3-4 hours of focused engineering (revised
upward from 2-3 hours after branch-inventory was specified).

**Critical reference sections (read in full before writing tests):**

- §4.5a — **35 branches** enumerated across the 3 original CycleOps
  classes with stable IDs (P-*, C-*, PR-*). Every branch must be covered.
- §4.5b — **Identical-effect requirement**. Post-β must produce
  bit-identical observable state per branch. Not "similar". Not "close
  enough". Review artifact required at L3.β submission: a 35-row mapping
  table.
- §4.5c — **Commit/abandon/hold/release decision tables** for
  producer/consumer/processor. These are the decisions the tests observe.
- §4.5d — **Test → branch-ID mapping** ensuring full branch coverage;
  suggested ~13 tests total (not 6 — revised after branch enumeration).
- §4.5e — **Test-evaluation principles**: built-in metrics + harness
  measurements + cross-check. No return-value assertions. No test-only
  synthetic state.
- §4.5e1 — **Exact-equality assertion language**. `EXPECT_EQ` always;
  `EXPECT_GE`/`EXPECT_LT` forbidden except for wall-clock jitter
  tolerance. Expected values are symbolic predictions walked from the
  decision trees, NOT magic numbers recorded from pre-β runs.
- §4.6 — **Discrepancy-source checklist** (8 items). Any post-β metric
  divergence is diagnosed, not reverted.

**Components to build (in dependency order):**

1. **Publicise `*CycleOps` classes.** Today they're in anonymous
   namespaces inside `src/producer/producer_role_host.cpp` (etc).
   Move to a new header `src/include/utils/cycle_ops.hpp` so tests
   can instantiate them directly. Non-functional refactor; no behavior
   change; one commit.
2. **`RecordingEngine` stub.** Minimal `ScriptEngine` subclass that:
   - Records every `invoke_produce` / `invoke_consume` / `invoke_process`
     call with args (InvokeTx/InvokeRx snapshots) and the return value.
   - Has configurable return behavior per cycle (e.g.,
     `result_fn = [](int cycle){ return cycle < 10 ? Commit : Discard; }`).
   - Tracks `script_error_count()` via a simple atomic.
   - Stubs the rest of the 15+ virtuals (load_script, register_slot_type,
     eval, etc.) with trivial returns.
   Location: `tests/test_framework/recording_engine.{hpp,cpp}` or similar.
3. **In-process queue-pair helper.** Construct a matched `hub::QueueWriter`
   and `hub::QueueReader` backed by an in-process SHM segment (or a
   simple ring-buffer) without the broker/lifecycle overhead. Similar
   to helpers already in `test_datahub_loop_policy.cpp`.
4. **Six baseline tests per §4.5b table.** Each test asserts:
   - **(a) Built-in metrics**: `iteration_count`, `out_slots_written`,
     `out_drop_count`, `in_slots_received`, `loop_overrun_count`,
     `script_error_count`, etc.
   - **(b) Harness measurements**: RecordingEngine call log, queue
     contents at end, wall-clock elapsed (for timing tests).
   - **Cross-check**: (a) equals (b) where both measure the same thing
     (e.g., `out_slots_written == queue_contents.size()`).
5. **Run on pre-β code (current `e05a338` state). Must pass green.**

### Step 3 — L3.β (after baseline suite is green on pre-β code)

Per `role_unification_design.md` §5:

- Create unified `scripting::CycleOps` in header (or adapt from the stash).
  Policy: `drop_mode` parameter; input-hold derived at runtime from side
  presence + overflow; branching on `has_tx_` / `has_rx_` mirroring the
  three original classes' branch structure **exactly** (§4.5a).
- Migrate all three role hosts to construct `scripting::CycleOps` instead
  of `ProducerCycleOps` / `ConsumerCycleOps` / `ProcessorCycleOps`.
- Delete the three original classes.
- **Produce the 35-row branch-mapping table** (§4.5b) in the commit
  message or a separate review doc. Reviewer verifies every pre-β branch
  is addressable in the unified code and produces identical effect.
- **Run the baseline suite.** Counter values and queue contents must
  match the scenario-derived predictions exactly. If any divergence:
  apply §4.6 discrepancy-source checklist to identify the translation
  error. Fix. Re-run.
- Run full test suite (`ctest -j2`). Expect 1275+/N passing.
- Commit.

### Step 4 — L3.γ (delete `hub::Producer` / `hub::Consumer`)

Their state migrates into `RoleAPIBase::Impl`. Producer/Consumer currently
hold: queue pointer + event-callback slots + realtime-mode handler thread
+ job queue + ZMQ socket handles + registration lifetime. All of this
moves to Impl. The two header files + two cpp files get deleted.

Callers in tests and examples that use `hub::Producer::create(...)` or
`hub::Consumer::create(...)` directly must migrate to new helper
functions (`hub::make_write_side(...)`, `hub::make_read_side(...)`,
or similar). ~6 test files + 2 example files to update.

Baseline suite + full suite must remain green.

### Step 5 — L3.δ (`RoleHost` base + empty subclasses with ExtConfig)

Extract `worker_main_()` into `RoleHost::run()`. The three existing
`*RoleHost` classes become empty subclasses with typed `*ExtConfig`
members by value. `on_extra_setup` / `on_extra_teardown` /
`on_extra_runtime` virtual hooks on base, empty defaults.

### Step 6 — L3.ε (handle-based ScriptEngine)

`ScriptEngine::resolve_callback(name) → ScriptHandle` +
`invoke_cycle(ScriptHandle, rx, tx, msgs)`. Engines (Python/Lua/Native)
implement resolution internally. Add
`pylabhub::role::callback_name_for(role_tag)` helper in a new
role-layer header.

After all callers migrate to handle-based: delete legacy
`invoke_produce`/`invoke_consume`/`invoke_process` from ScriptEngine.

Note per design: Native plugin ABI does NOT change — plugins keep their
three function-pointer slots; the Native engine's internal resolver
selects the right slot when `resolve_callback` is called with
`on_produce` / `on_consume` / `on_process`.

### Step 7 — L3.ζ (doc updates)

- Rewrite HEP-CORE-0011 §"Threading Model" and §"Invocation Model" against
  the final unified surface.
- Archive `role_unification_design.md` and `unified_role_loop.md` and
  `loop_design_unified.md` to `docs/archive/transient-YYYY-MM-DD/`.
- Update `API_TODO.md`, `TODO_MASTER.md`, `MESSAGEHUB_TODO.md` to close
  HEP-0011 rewrite and role-unification entries.
- Record in `DOC_ARCHIVE_LOG.md`.

---

## 3. Reference anchors (read before starting)

| Topic | Where |
|---|---|
| Full L3 design | `docs/tech_draft/role_unification_design.md` |
| Loop timing authoritative spec | `docs/tech_draft/loop_design_unified.md` |
| Historical L3 plan (superseded, read only if curious) | `docs/tech_draft/unified_role_loop.md` |
| ScriptHost HEP (obsolete-term scrub only; full rewrite post-L3) | `docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md` |
| State machine HEP (current) | `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` |
| Active TODO pointers | `docs/TODO_MASTER.md`, `docs/todo/API_TODO.md`, `docs/todo/MESSAGEHUB_TODO.md` |

---

## 4. Known open items (not blockers for L3 but relevant)

- Task #13 evaluate test coverage for new design — partially addressed via
  `test_datahub_role_state_machine.cpp`; remaining gaps (BRC reconnect,
  ctrl-thread lifecycle, heartbeat self-healing) logged in `API_TODO.md`.
- Task #17 `const char*` audit — closed with note "keep existing uses;
  each justified by platform-API, C-ABI, or hot-path concerns; only
  `hubshell.cpp` + `hub_config.cpp` lambda changed".
- BRC reconnect-on-broker-restart — follow-up in `API_TODO.md`.
- Test-helper dedup (`BrcHandle`/`BrokerHandle`) — follow-up in `API_TODO.md`.
- Federation-index cleanup symmetry test (mirrors the band test) — follow-up.

---

## 5. Session hygiene reminders

- Follow the two-source cross-check principle for any new tests
  (§4.5a of the design doc).
- Run `ctest -j2` output into `/tmp/test_output.txt` once and grep it —
  don't re-run for different queries (see CLAUDE.md testing practice).
- Never write code without explicit user approval. Present findings →
  discuss → agree → then implement.
- After each phase: build + test + commit, before starting the next phase.

---

## 6. If you need to resume mid-phase

- Check `git status` for uncommitted work.
- Check `git stash list` — there's a `WIP L3.β` stash from the prior
  session; decide to pop (and adapt to §4.5a principle) or discard
  (and re-implement after baseline suite).
- Check `git log origin/feature/lua-role-support..HEAD --oneline` for
  unpushed commits.
- Reference the relevant phase section in `role_unification_design.md` §5.
