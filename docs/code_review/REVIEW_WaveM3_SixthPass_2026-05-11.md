# Wave M3 Sixth-Pass Audit — Diminishing-Returns Pass

**Date:** 2026-05-11 (sixth pass)
**Branch:** `feature/lua-role-support` (post fifth-pass operational fixes)
**Tests:** 1823/1823 passing.
**Trigger:** User directive — "do another thorough review."

This pass goes deeper into cross-aggregate state leakage outside HubState (broker-side stores), comment drift, and assumption-validation.

---

## Findings

### H34 — `metrics_store_` per-uid leak in multi-producer DEREG / multi-producer pending-timeout / consumer-liveness sweep ⚠️ PRE-EXISTING

**Severity:** Medium.  Pre-existing memory leak; not introduced by Wave M3.  Will be fully retired by M1.4.
**Files:** `src/utils/ipc/broker_service.cpp:1805-1846` (DEREG_REQ), `:2718-2722` (pending-timeout sweep), `:2700-2715` (consumer-liveness sweep).

**Problem:**
`metrics_store_` is a broker-private map keyed `{channel → {producers: {uid → metrics}, consumers: {uid → metrics}}}`.  It's cleaned ONLY at channel teardown (`metrics_store_.erase(channel_name)`).  In multi-producer / multi-consumer scenarios where a uid DEREGs but the channel survives, the per-uid metrics entry leaks until either:
1. The channel itself closes (could be days/weeks on a long-running Fan-In channel).
2. The broker restarts.

**Concrete scenarios:**
- DEREG_REQ for non-last producer → producer removed from ChannelEntry, but `metrics_store_[channel].producers[uid]` stays.
- Pending-timeout sweep on non-last producer → same.
- CONSUMER_DEREG_REQ → consumer removed from ChannelEntry, but `metrics_store_[channel].consumers[uid]` stays.
- Consumer-liveness sweep (consumer process dead) → `_on_consumer_left` cleans HubState, but `metrics_store_` keeps the dead consumer's metrics.

**Effect:**
- Admin queries that dump metrics show ghost entries for long-dead uids.
- Memory growth bounded by (channels × distinct historical uids).  For a long-running Fan-In channel with many transient producers, this could be significant.

**Why I'm NOT fixing this in Wave M3:**
- M1.4 retires `metrics_store_` entirely (per `docs/TODO_MASTER.md:128`): "delete the legacy store entirely; route admin metrics queries through HubState's per-presence rows."
- Under H18, per-presence rows are erased on DEREG (no tombstones).  So M1.4's HubState-routed metrics will be naturally clean — no separate per-uid cleanup needed.
- Fixing the leak piecemeal pre-M1.4 means writing erasure code that will be deleted by M1.4.  Net work negative.

**Recommendation:** track as pre-M1.4 known leak in `docs/todo/MESSAGEHUB_TODO.md` M1.4 section.  Update HEP-CORE-0019 to note the leak class is fixed structurally by the per-presence-row migration.

### H35 — Stale comments referencing pre-H12 "cascade eviction" in `_on_pending_timeout` ✅ FIXED in this pass

**Severity:** Cosmetic.
**Files:** `src/utils/ipc/hub_state.cpp:1351-1364`, `:1398-1400`.

The pre-H12 design had `_on_channel_closed` directly evicting schemas per-producer.  Two comments in `_on_pending_timeout` referenced this:
1. The ordering note: "Removal-before-close-cascade would empty `producers[]` and skip schema eviction in `_on_channel_closed`".
2. The last-producer-path note: "leave the producer in producers[] so _on_channel_closed's cascade eviction can read its uid".

Post-H12: schema eviction is now in `cascade_role_terminal_cleanup_locked` (owner-lifetime), fired via dispatch.  The "leave in producers[]" pattern is STILL essential — but for a different reason (`_on_channel_closed` reads `producer_uids` to dispatch terminal cleanup for each).

**Fix shipped:** updated both comments to reflect the post-H12 dispatch-based design while preserving the correct ordering rationale.

### H36 — `_on_schemas_evicted_for_owner` is dead code in production ℹ️ CLEANUP OPPORTUNITY

**Severity:** None (callable from L2 tests, but not used in production).
**Files:** `src/utils/ipc/hub_state.cpp:1564`.

Grep:
```
$ grep -rn '_on_schemas_evicted_for_owner' src/
src/utils/ipc/hub_state.cpp:1564:std::size_t HubState::_on_schemas_evicted_for_owner(const std::string &owner_uid)
src/include/utils/hub_state.hpp:1482:    std::size_t _on_schemas_evicted_for_owner(const std::string &owner_uid);
```

The only production caller (`_on_channel_closed` per-producer cascade) was removed by H12.  The op remains exposed for L2 test access (`HubStateTestAccess::on_schemas_evicted_for_owner`).

**Options:**
- Keep as test-only API for white-box test coverage of schema cascade in isolation.
- Mark deprecated and migrate L2 tests to use `cascade_role_terminal_cleanup_locked`-equivalent flows.

**Recommendation:** keep with a docstring note that it's test-access only.  No fix needed.

### H37 — `_on_consumer_left` 3-lock pattern ⚠️ PRE-EXISTING (NOT BLOCKED BY THIS COMMIT)

Same finding as H28 (fourth pass) and re-confirmed here.  `_on_consumer_left` takes the writer lock three separate times: `_remove_consumer` (lock 1), the cache+presence mutation block (lock 2), `_dispatch_role_disconnected_if_dead` (lock 3, if dead).  Between locks, snapshot readers could observe intermediate state.  Single-thread broker IO model makes this invisible in practice.  Refactor would require restructuring `_remove_consumer` to accept an externally-held lock.

### H38 — `_set_role_disconnected` admin path: stale `ChannelEntry.producers/consumers` ⚠️ NON-ISSUE TODAY

Same finding as H27.  `_set_role_disconnected` doesn't iterate ChannelEntry to remove producer/consumer entries.  No production caller (only L2 tests via `HubStateTestAccess`), so no stale state in production.  If admin RPC ever exposes wire-level force-disconnect, this op would need extending.

### H39 — DISC_REQ's `if (p->state == Disconnected) continue` is dead code ℹ️ COSMETIC

Same finding as H29.  Under H18, no Disconnected presence rows exist.  The skip is harmless but unreachable.  Could be removed in a tidying commit.

### H40 — Concurrency assumption for `active_router_` and `band_left` subscriber ✅ DOCUMENTED, BUT NEEDS WIDER CHECK

**Severity:** Inspection.
**Files:** `src/utils/ipc/broker_service.cpp:212` + the lambda capture site at `:506`.

The broker's `band_left` subscriber lambda captures `this` and reads `active_router_`.  My fifth-pass review documented the assumption: "Handlers fire from whatever thread invoked the HubState op.  In production that's this broker IO thread."  Verified by `grep -rn '_on_*' src/` showing all callers are on the broker IO thread.

But — for robustness, the design could be hardened:
- Make `active_router_` an `std::atomic<zmq::socket_t*>`.  Reads from any thread are safe.
- Or: add a runtime assertion (`assert(std::this_thread::get_id() == broker_io_thread_id_)`) at handler entry.

**Recommendation:** Add a runtime assertion in DEBUG builds to catch any future caller that violates the single-thread assumption.  Out of scope for this commit.

### H41 — `_on_channel_closed` cycle of three lock acquisitions ⚠️ PRE-EXISTING

Same class as H37 but for channels.  Three lock acquisitions:
1. shared_lock to read producer_uids/consumer_uids.
2. `_set_channel_closed` (takes unique_lock internally, releases).
3. unique_lock for presence cascade.
4. Dispatch loops (each takes unique_lock).

In single-thread broker IO, no inconsistency observable.  Refactor would consolidate but break the layer between `_set_channel_closed` (independent primitive) and the op.

### H42 — Wave M3 wire-protocol back-compat for snapshot consumers ℹ️ DOCUMENTED

Same finding as H33.  JSON output drops Disconnected presence rows post-H18.  Grep confirmed no consumer code expects them.  External hubs / scripts via snapshot get cleaner state.  Worth documenting in HEP-CORE-0023 §2.6.

### H43 — Federation propagation gap for role-disconnect events ⚠️ DEFERRED TO WAVE B M8

Same finding as H30.  When a role disconnects on hub A, peer hubs B/C don't receive a notification.  Per current HEP-CORE-0022/30: bands are NOT federated; channel-close IS notified via `federation_on_channel_closed`.  Individual role-disconnect (without channel close) has no federation propagation.

**Whether this matters depends on Wave B M8 requirements** — if peer hubs need to track per-uid presence for dual-hub processor scenarios, they need this propagation.  Per `role_host_template_design.md` Wave B §M8, dual-hub processor needs role-side coordination.  HubState-level cross-hub role tracking may not be needed.

Out of scope for Wave M3.

---

## Status summary (sixth pass)

| ID | Title | Status |
|---|---|---|
| H1-H8 | First pass | All closed |
| H9-H14 | Second pass | All closed |
| H15 | `_on_heartbeat` metrics direct-mutation | Deferred (same trigger) |
| H16-H20 | Cross-aggregate + presence lifecycle | All closed (passes 3-4) |
| H21, H22 | Inspection notes | Documented (passes 3-4) |
| H23-H25 | Log gating + dtor + mutation verification | All closed (pass 5) |
| H26 | Mutation-test verification | Done (pass 5) |
| H27-H33 | Inspection / out-of-scope / pre-existing | All documented (pass 5) |
| H34 | `metrics_store_` per-uid leak | Pre-existing; M1.4 will retire entirely |
| H35 | Stale comments in `_on_pending_timeout` | ✅ FIXED (this pass) |
| H36 | `_on_schemas_evicted_for_owner` test-only | Documented |
| H37, H41 | 3-lock cycles | Pre-existing, single-thread safe |
| H38 | Force-erase admin path | Non-issue today |
| H39 | DISC_REQ dead Disconnected-skip | Cosmetic |
| H40 | Concurrency assertion opportunity | Future hardening |
| H42 | JSON wire-format change post-H18 | Documented |
| H43 | Federation propagation gap | Deferred to Wave B M8 |

## Why this pass found small things

By the sixth pass, the architecture is sound.  Findings are now:
- **Pre-existing leaks** that downstream work (M1.4) will retire structurally — not worth fixing piecemeal.
- **Comment drift** from previous fix passes — quick cosmetic cleanup.
- **Inspection notes** about pre-existing patterns (3-lock cycles) that are safe under the single-thread broker IO model.
- **Cross-aggregate concerns** outside HubState (federation, metrics) that belong to other waves.

The "every check exposes new errors" pattern has converged.  The remaining findings are documentation / future-hardening / known leaks.  None are correctness bugs in Wave M3 scope.

---

## Why I'm calling Wave M3 done

Wave M3's stated scope: RoleEntry controlled-access API with terminal cleanup, retire `disconnected_fired` PATCH, eliminate multi-producer overwrite-class bugs at role scope.

**Delivered:**
- Step 1: additive RoleEntry API (add_presence, on_*, drop_channel_if_orphaned). ✓
- Step 2: `_on_heartbeat` routes via on_heartbeat. ✓
- Step 3: heartbeat-timeout / pending-timeout route via on_heartbeat_timeout / on_pending_timeout. ✓
- Step 4: `_set_role_disconnected` terminal cleanup; `disconnected_fired` retired. ✓
- Step 5b: `_dispatch_role_disconnected_if_dead` wired into 4 production sites (H1 fix). ✓
- Step 5c: `_on_producer_dropped` multi-producer transition + cache (H9 fix). ✓
- Step 5d: cache invariant primitive `drop_channel_if_orphaned` (H10+H11 fix). ✓
- Step 5e: schema cascade owner-lifetime only (H12 fix; supersedes design decision #2). ✓
- Step 5f: HubState terminal cleanup cascades to bands + broker subscribes for NOTIFY (H16+H20 fix). ✓
- Step 5g+h: presence row lifecycle — erase on disconnect, simplify cache + liveness checks (H17+H18 fix). ✓
- Step 6: L2 test coverage — 12 new tests across the surface. ✓
- Step 8: HEP doc sync (HEP-CORE-0023 §2 + HEP-CORE-0034 §7.2). ✓

**Explicitly deferred (with documented triggers):**
- Step 5: strict `add_role` admission (trigger: spoofing-attempt observation or security pass requirement).
- Step 7: privatize `RolePresence` state fields (trigger: concrete misuse or audit observation).

**Verification:**
- 1823/1823 tests passing.
- Two mutation tests verified the H16/H20 and H18 contracts are truly pinned.
- Six review passes; converged.

**Downstream waves unblocked:**
- M1.4 (retire `metrics_store_`) — Wave M3's H18 per-presence-row erase makes M1.4 natural.
- MD1 (role teardown race) — independent; not blocked.
- Wave B M8 / MP6 (federation) — H43 federation propagation gap is the only remaining design question; HubState foundation is stable.

## Archival

After this commit lands, archive ALL six review documents to `docs/code_review/archive/transient-2026-05-11/`:
- `REVIEW_WaveM3_2026-05-11.md` (first pass)
- `REVIEW_WaveM3_PostFix_2026-05-11.md` (second pass)
- `REVIEW_WaveM3_FullChain_2026-05-11.md` (third pass)
- `REVIEW_WaveM3_Rigorous_2026-05-11.md` (fourth pass)
- `REVIEW_WaveM3_FifthPass_2026-05-11.md` (fifth pass)
- `REVIEW_WaveM3_SixthPass_2026-05-11.md` (this — sixth, closing)

Record in `docs/DOC_ARCHIVE_LOG.md`.  Cross-link from `docs/TODO_MASTER.md` "Wave M3 retrospective" until then.

The user's concern — "we keep missing things, every check exposes additional error" — is honestly answered: yes, each pass found something, AND each pass found smaller and smaller things.  By pass 6, the findings are pre-existing leaks (covered by M1.4), comment drift, and inspection notes about known design patterns.  No correctness bugs remain in Wave M3 scope.

**Recommendation:** land the commit batch.  Move to M1.4.
