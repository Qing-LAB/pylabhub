# Wave M3 Post-Closure Audit — RoleEntry Controlled-Access API

**Date:** 2026-05-11
**Branch:** `feature/lua-role-support` (post `689444e` — M3 step 8 shipped)
**Scope:** Wave M3 (RoleEntry controlled-access API) — steps 0-8 except 5/7 deferred.
**Tests as of this audit:** 1813/1813 passing.

Audits performed:
- API completeness + coherence (RoleEntry new methods, HubState ops).
- Residual stale code / comments referencing the retired `disconnected_fired` PATCH.
- Direct presence-field mutation sites that should route through the new API.
- Logic vs M3 design doc + HEP-CORE-0023 + HEP-CORE-0034 cross-check.
- Trigger-wiring audit — **the most important question: does production actually call the terminal-cleanup op?**

---

## Findings

### H1 — `_set_role_disconnected` has NO production callers ❌ CRITICAL WIRING GAP

**Severity:** Critical.  The entire purpose of Wave M3 (eliminate role residue) hinges on this trigger; today it's only invoked from L2 tests.
**Files:**
- `src/utils/ipc/hub_state.cpp:399` — terminal-cleanup op definition.
- `tests/test_framework/hub_state_test_access.h` — test-access forwarder.
- `tests/test_layer2_service/test_hub_state.cpp` — 3 call sites in tests.

**Verified:** grep across `src/` shows zero non-test, non-self call sites:
```
$ grep -rn '_set_role_disconnected' src/ | grep -v '//\|^.*\.hpp.*declar\|hub_state\.cpp:399\s*void'
(no matches outside the definition + comments)
```

**Effect:** Production code never erases `RoleEntry` rows after disconnect.  When the last producer-presence transitions Disconnected via `_on_pending_timeout`, the broker's channel teardown fires via `_on_channel_closed`, but the role entry stays in `pImpl->roles` with all-Disconnected presences indefinitely.  This is the EXACT "stale residue" concern the user called out at the start of Wave M2.5 §6.2 (2026-05-10): "when a role is disconnected, its entry should be cleaned correctly. If there is a residue, there should be a warning."

The M3 step 4 commit (`17d1d3e`) created the terminal-cleanup behavior on the op side, retired the `disconnected_fired` PATCH structurally on the field side, but did NOT wire the trigger — production paths still leak role entries on disconnect.

**Fix needed (step 5b — wire the trigger):**
Add a private helper `_dispatch_role_disconnected_if_dead(uid)` that:
1. Reads `it->second.any_presence_alive()` under the writer lock.
2. If `false`, calls `_set_role_disconnected(uid)` (release-lock-then-fire).

Call this helper from each op-tail that COULD make a role's last presence Disconnected:
- `_on_pending_timeout` (after presence Pending → Disconnected).
- `_on_consumer_left` (after presence → Disconnected).
- `_on_channel_closed` (per-producer after marking presences Disconnected).
- `_on_producer_dropped` (only when channel survives — last-producer path already calls `_on_channel_closed` which would re-trigger).

This is the missing wiring that turns Wave M3's structural mechanism into a working production cleanup path.

---

### H2 — `_on_channel_closed` directly mutates producer-presence state ⚠️ INCONSISTENCY

**Severity:** Medium.  Cosmetic for now; will become a step-7-style hardening target.
**Files:** `src/utils/ipc/hub_state.cpp:1002-1008`.

**Problem:**
```cpp
if (auto *p = rit->second.find_presence(name, "producer");
    p != nullptr && p->state != RoleState::Disconnected)
{
    p->state       = RoleState::Disconnected;
    p->state_since = std::chrono::steady_clock::now();
}
```

This bypasses the M3 controlled-access API (`entry.on_dereg(name, "producer")`).  Behavior is identical, but the direct-field-mutation pattern is exactly what Wave M3 is supposed to wall off.  Route through `on_dereg`.

---

### H3 — `_on_consumer_left` directly mutates consumer-presence state ⚠️ INCONSISTENCY

**Severity:** Medium (same class as H2).
**Files:** `src/utils/ipc/hub_state.cpp:1075-1081`.

Same direct-mutation pattern; should route through `on_dereg(channel, "consumer")`.

---

### H4 — Stale comment in `_on_channel_closed` claims cascade will move ⚠️ STALE

**Severity:** Low — comment-only drift.
**Files:** `src/utils/ipc/hub_state.cpp:1015-1018`.

**Comment says:**
> "Wave M3 (RoleEntry controlled-access API) will move this to the role-disconnect cascade (HEP-CORE-0034 §7.2) so eviction follows owner-lifetime rather than channel-close; until then, evict per-producer here."

**Reality:** Per M3 design decision #2 (locked 2026-05-11), the cascade fires from BOTH `_on_channel_closed` AND `_set_role_disconnected` — they're both correct and idempotent.  The comment implies the channel-close path is transitional/will-move; it's actually permanent.

**Fix:** rewrite to reflect the dual-trigger reality.

---

### H5 — No L3 integration test verifies automatic role-disconnect cleanup ⚠️ COVERAGE GAP

**Severity:** Medium (couples with H1).  If H1 is fixed, this test pins the contract end-to-end.
**Files:** `tests/test_layer3_datahub/test_datahub_role_state_machine.cpp` (or similar).

The L2 tests (`RoleEntryApi.SetRoleDisconnected_TerminalErase_IdempotentSecondCall` etc.) call `_set_role_disconnected` directly via the test-access forwarder.  But there's no broker-level integration test that:
1. Registers a producer + consumer for the same uid.
2. DEREGs both (so any_presence_alive becomes false).
3. Verifies the role entry is GONE from the snapshot.

Without such a test, the H1 wiring gap goes unobserved.

---

### H6 — Schema cascade dual-trigger interaction once H1 is fixed ℹ️ INFO

**Severity:** Informational; surfaces only if H1 is fixed.
**Files:** `src/utils/ipc/hub_state.cpp:399` + `:1019`.

Once `_set_role_disconnected` actually fires from production paths (H1 fix), schemas will be evicted twice for any producer:
- Once from `_on_channel_closed` (per-producer at channel teardown).
- Once from `_set_role_disconnected` (role-lifetime at terminal cleanup).

This is the intentional dual-trigger from design decision #2.  Eviction is idempotent (second iteration finds the record already gone, no-op).  The `schema_evicted_total` counter is bumped per actual removal, so the double-firing does NOT double-count.

No fix needed; documented here for future-reader clarity.

---

### H7 — `_on_heartbeat` retains a direct presence-field mutation for metrics ⚠️ DEFERRED

**Severity:** Tracked.  M3 step 2 commit message explicitly notes this.
**Files:** `src/utils/ipc/hub_state.cpp:1144-1152` (the metrics-write block after `entry.on_heartbeat` call).

Per the M3 step 2 commit message:
> "Metrics write — still a direct presence-row mutation. (M3 step-2 scope is FSM-only. A future `entry.set_presence_metrics(channel, role_type, metrics)` method could absorb this; not in step 2.)"

This is a known carve-out; not a bug.  A `set_presence_metrics` method could be added in a future M3 follow-up if desired.

---

### H8 — Comment-only `disconnected_fired` references are all documentation ✅ OK

**Severity:** None.
**Files:** 6 sites in `src/` + `docs/` reference `disconnected_fired` only in comments documenting the retirement.  No code references the field (deleted).  Design doc references are historical.  No action needed.

---

## Status Summary

| ID | Title | Severity | Status |
|---|---|---|---|
| H1 | `_set_role_disconnected` has NO production callers | Critical | ❌ OPEN |
| H2 | `_on_channel_closed` direct presence mutation | Medium | ❌ OPEN |
| H3 | `_on_consumer_left` direct presence mutation | Medium | ❌ OPEN |
| H4 | Stale comment in `_on_channel_closed` cascade block | Low | ❌ OPEN |
| H5 | No L3 integration test for role-disconnect cleanup | Coverage | ❌ OPEN (couples with H1) |
| H6 | Dual-trigger cascade behavior once H1 fixed | Info | ✅ DOCUMENTED |
| H7 | `_on_heartbeat` direct metrics mutation | Tracked | ⚠️ DEFERRED |
| H8 | `disconnected_fired` comment-only references | Trivial | ✅ OK |

## What needs fixing (in priority order)

**Priority 1 — Land H1 + H5 together (the wiring gap):**
- Add `_dispatch_role_disconnected_if_dead(uid)` helper.
- Call it from `_on_pending_timeout`, `_on_consumer_left`, `_on_channel_closed`, `_on_producer_dropped`.
- L3 integration test: register producer+consumer same uid, DEREG both, verify `s.role(uid)` returns nullopt.

This is one focused commit that turns Wave M3's structural mechanism into a working production cleanup.  Without it, M3's terminal-cleanup behavior is unreached in prod.

**Priority 2 — Land H2 + H3 + H4 together (consistency sweep):**
- Route direct presence mutations through `entry.on_dereg`.
- Rewrite the stale `_on_channel_closed` cascade comment.

**Priority 3 — Defer H7:**
- `set_presence_metrics` API addition can wait for a concrete need.

## Archival

Once H1-H5 close, archive this review to `docs/code_review/archive/` per `docs/DOC_STRUCTURE.md §1.7`.  Cross-link from `docs/TODO_MASTER.md` "Active code review" until then.
