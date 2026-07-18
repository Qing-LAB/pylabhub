# Wave M3 Fifth-Pass Audit — No-Assumption Pass v2

**Date:** 2026-05-11 (fifth pass)
**Branch:** `feature/lua-role-support` (post H16/H17/H18/H20 fix sweep)
**Tests:** 1823/1823 passing.
**Trigger:** User directive — "do the review thoroughly again. we keep missing things and every check exposes additional error."

This pass tightens what the fourth-pass didn't fully nail down: log gating, defensive cleanup in `~BrokerServiceImpl`, mutation-test verification for the new tests, and a re-audit of paths the prior pass marked "out of scope" or "inspection-only."

---

## Findings the fourth-pass missed (or under-cooked)

### H23 — Log signal regression in `handle_band_leave_req` ❌ FIXED in this pass

**Severity:** Low (log noise, not behaviour).
**Files:** `src/utils/ipc/broker_service.cpp:4234` (post-fix).

**Problem:** When I converted `handle_band_leave_req` to the subscriber pattern in the H16 fix, I removed the `was_member` pre-check along with the explicit NOTIFY fanout.  The `LOGGER_INFO("Broker: BAND_LEAVE 'ch' role='uid'")` line was preserved but its gating was lost — so the log fired for every BAND_LEAVE_REQ regardless of whether the role was actually a member.  Pre-fix, the log fired only on real removals.

**User caught this:** "why did you remove the log from that function?"

**Fix shipped this pass:** restored the `was_member` pre-check so the log fires only on real removals.  The subscriber's `send_band_leave_notify` also gates its log on real removal (handler fires only when `_on_band_left` actually removed a member).  Net: both logs fire only on real action; spurious leave requests are silently no-op.

```cpp
bool was_member = false;
if (auto pre = hub_state_->band(channel); pre.has_value())
    for (const auto& m : pre->members)
        if (m.role_uid == role_uid) { was_member = true; break; }
hub_state_->_on_band_left(channel, role_uid);
if (was_member)
    LOGGER_INFO("Broker: BAND_LEAVE '{}' role='{}'", channel, role_uid);
```

### H24 — `send_band_leave_notify` log was after the early-return ❌ FIXED in this pass

**Severity:** Low (diagnostic visibility).
**Files:** `src/utils/ipc/broker_service.cpp:2858` (post-fix).

**Problem:** First version of `send_band_leave_notify` put `LOGGER_INFO("...removed from band...")` AFTER the early-return for an auto-deleted band, so the log wouldn't fire for last-member leaves.  Pre-fix `band_on_role_closed` logged BEFORE the early-return.

**Fix shipped:** moved the LOGGER_INFO above the `if (!remaining.has_value()) return;` check.  Now every real removal logs, even when the band has no remaining members to NOTIFY.

### H25 — `~BrokerServiceImpl` lacked defensive unsubscribe ❌ FIXED in this pass

**Severity:** Medium (latent UAF risk in abnormal shutdown).
**Files:** `src/utils/ipc/broker_service.cpp:163` (post-fix).

**Problem:** The `band_left` subscriber captures `this` (the `BrokerServiceImpl*`).  If `run()` exits abnormally (exception, signal) before reaching its unsubscribe block, the handler is still registered on `HubState` (which outlives `BrokerServiceImpl`).  A subsequent role-disconnect cascade would invoke the lambda — UAF.

**Fix shipped:** added defensive unsubscribe in `~BrokerServiceImpl`.  Idempotent — no-op if already unsubscribed or never subscribed.  Belt-and-suspenders against abnormal exit paths.

```cpp
~BrokerServiceImpl()
{
    if (hub_state_ != nullptr &&
        band_left_handler_id_ != pylabhub::hub::kInvalidHandlerId)
    {
        hub_state_->unsubscribe(band_left_handler_id_);
        band_left_handler_id_ = pylabhub::hub::kInvalidHandlerId;
    }
    active_router_ = nullptr;
    // ... existing key-zeroing ...
}
```

### H26 — Mutation-test verification VERIFIED in this pass ✅

**Significance:** Earlier reviews noted "mutation sweep not done" as follow-up.  This pass actually ran two mutation tests against the H16+H20 + H17+H18 contracts and verified they fail when the production code is broken.

**Mutation 1 — band cascade disabled in `cascade_role_terminal_cleanup_locked`:**
- `HubStateBandCascade.TerminalCleanup_RemovesUidFromAllBands_FiresBandLeftWithReason` **FAILS** (band still exists, 0 band_left events) ✓
- `HubStateBandCascade.MultiPresenceRole_StaysAlive_KeepsBandMembership` passes (cascade body irrelevant when role survives via `any_presence_alive` early-return).  ✓ (test is complementary, not a regression of the mutation)

**Mutation 2 — `on_dereg` reverted to mark Disconnected (pre-H18 tombstone):**
- `RoleEntryApi.OnDereg_ErasesPresenceRow_IdempotentSecondCall` **FAILS** ✓
- `HubStateOps.ChannelClosed_MultiChannel_LeavesOtherPresences` **FAILS** ✓
- `HubStateOps.ConsumerLeft_RemovesFromChannelAndErasesRoleEntry` **FAILS** ✓
- `HubStateProducerDropped.MultiChannel_Producer_StaysAliveAfterOneDereg_SchemasSurvive` **FAILS** ✓

4/4 H18 tests catch the regression.  Contracts are truly pinned.

### H27 — Stale `ChannelEntry.producers/consumers` on direct `_set_role_disconnected` ℹ️ NON-ISSUE

**Severity:** None (no production caller).
**Files:** `src/utils/ipc/hub_state.cpp:462`.

`_set_role_disconnected` (unconditional force-erase) is called only from L2 tests via `HubStateTestAccess`.  No production caller.  So the theoretical "admin force-disconnects a role while ChannelEntry still references it" scenario doesn't manifest.

If admin RPC is ever added that exposes force-disconnect on the wire, this op would need an enriched cascade that also walks `ChannelEntry.producers/consumers` to remove the uid.  Tracked as a future concern; no fix needed today.

### H28 — Three separate lock acquisitions in `_on_consumer_left` ℹ️ PRE-EXISTING, ACCEPTABLE

**Severity:** Inspection.  Pre-existing pattern.
**Files:** `src/utils/ipc/hub_state.cpp:1115-1140`.

`_on_consumer_left` takes the writer lock three times: once in `_remove_consumer`, once for the on_dereg block, once in `_dispatch_role_disconnected_if_dead`.  Between locks, snapshot readers could see intermediate states (consumer gone from ChannelEntry.consumers, presence still in RoleEntry.presences).

In production: broker IO thread is single-writer; readers (admin queries, federation snapshots) see consistent-enough state across the brief window.  In L2 tests with no broker: no concurrent readers, no issue.

Tightening would require restructuring `_remove_consumer` to take an externally-held lock.  Out of scope.

### H29 — DISC_REQ's `if (p->state == Disconnected) continue` is dead code post-H18 ℹ️ COSMETIC

**Severity:** None (correctness preserved).
**Files:** `src/utils/ipc/broker_service.cpp:1638`.

Under H18, Disconnected presence rows are erased on transition.  `find_presence` returns `nullptr` for "absent or disconnected".  The explicit `state == Disconnected` skip becomes unreachable — handled by the `p == nullptr` check immediately above.

The check is harmless (just unreachable).  Could be cleaned up in a tidying commit.  No urgency.

### H30 — Federation propagation of role-disconnect events ⚠️ OUT-OF-SCOPE

**Severity:** Design-question (likely correct as-is, but unverified).
**Files:** `src/utils/ipc/broker_service.cpp` federation paths.

When a role disconnects on hub A, peer hubs B, C don't receive a role-disconnect notification.  Pre-H1, no production path triggered role-disconnect cleanup; post-H1, it does.  Federation only propagates channel-close (via `federation_on_channel_closed`, which is currently a no-op stub).

Does federation NEED to know about role-disconnect?  Per HEP-CORE-0022, federation cross-relays channel messages and tracks peer hub state.  Individual roles on remote hubs are not directly tracked.  So probably no propagation needed.

But: a role on hub A that's a band member of a federated band — does its disconnect need to reach peer hubs to update their copy of band membership?  Looking at HEP-CORE-0030 §3 + the broker code: bands are NOT mirrored across hubs.  Each hub has its own band state; cross-hub band messages are relayed via the `relay_channels` mechanism.  So no per-hub band-member state to keep in sync.

Net: federation likely doesn't need role-disconnect events.  But this should be verified by reading HEP-CORE-0022 carefully + sanity-checking the dual-hub test scenarios in Wave B M8.  Tracked as a Wave B prerequisite.

### H31 — `_on_metrics_reported` writes to presence rows that may not exist ℹ️ NON-ISSUE UNDER H18

**Severity:** None.
**Files:** `src/utils/ipc/hub_state.cpp:1444-1451`.

`_on_metrics_reported` iterates `rit->second.presences` and writes metrics for any matching `(channel, *)` presence.  Under H18, Disconnected presences are erased — so this loop only writes to live (Connected/Pending) rows.  If a stale METRICS_REPORT_REQ arrives for a presence that was just DEREGed, the loop finds no matching row → no-op.  Correct.

### H32 — Federation peer disconnect cleanup audit ℹ️ NOT TOUCHED, NOT BROKEN

**Severity:** None.
**Files:** `src/utils/ipc/hub_state.cpp:1471` (`_on_peer_disconnected`).

`_on_peer_disconnected` is for HUB peers (federation), not for roles.  Wave M3 didn't touch it.  Same pre-fix behavior.  Just verifying we haven't accidentally broken it.

### H33 — JSON serialization no longer emits Disconnected presence rows ℹ️ NEW WIRE BEHAVIOR

**Severity:** Documentation worth noting.  No tests broke.
**Files:** `src/utils/ipc/hub_state_json.cpp:107-125`.

Pre-fix: snapshot JSON included Disconnected presence rows (for diagnostics).
Post-H18: no Disconnected rows ever exist; snapshot JSON only emits Connected/Pending.

Any external consumer that parsed presence state and looked for "disconnected" strings would now see no such rows.  Semantic mapping: "presence absent" = "was DEREGed, now gone".

Grep found ZERO consumer parsing for `"disconnected"` presence state in tests, src, or workers.  No production consumer impact.  Worth noting in HEP-CORE-0023 §2.6 (snapshot semantics) — added as documentation TODO.

---

## Status summary (fifth pass)

| ID | Title | Severity | Status |
|---|---|---|---|
| H1-H8 | First pass | — | All closed |
| H9-H14 | Second pass | — | All closed |
| H15 | `_on_heartbeat` metrics direct-mutation | Tracked | Deferred (same trigger) |
| H16 | Broker missing role_disc subscription | Critical | ✅ CLOSED |
| H17 | upsert_presence_row_locked re-REG no-op | Medium | ✅ CLOSED (subsumed by H18) |
| H18 | Disconnected presence tombstones | Medium | ✅ CLOSED |
| H19, H21, H22 | Inspection notes | — | Documented |
| H20 | `_set_role_disconnected` no band cascade | Medium | ✅ CLOSED |
| H23 | Log gating regression in handle_band_leave_req | Low | ✅ CLOSED (this pass) |
| H24 | send_band_leave_notify log ordering | Low | ✅ CLOSED (this pass) |
| H25 | ~BrokerServiceImpl missing defensive unsubscribe | Medium | ✅ CLOSED (this pass) |
| H26 | Mutation-test verification | Coverage | ✅ DONE (this pass) |
| H27 | Stale ChannelEntry on direct force-erase | Non-issue | Documented |
| H28 | 3-lock pattern in `_on_consumer_left` | Pre-existing | Documented |
| H29 | DISC_REQ dead Disconnected-skip | Cosmetic | Documented |
| H30 | Federation propagation of role-disconnect | Out-of-scope | Tracked for Wave B |
| H31 | `_on_metrics_reported` to absent presence | Non-issue under H18 | Confirmed |
| H32 | Federation peer disconnect | Untouched | Confirmed |
| H33 | JSON output drops Disconnected rows | New wire behavior | Documented |

## Why this pass found smaller things

The previous four passes targeted **architectural** gaps (wiring, cascades, cache invariants).  Once those landed, the rigorous pass surfaced **semantic + lifecycle** gaps (subscriber wiring, cross-aggregate cleanup, presence row lifecycle).  This pass surfaced **operational** gaps (log gating, defensive cleanup, mutation-test rigor) — the long tail of any cleanup.

**Pattern observation:** each pass shrinks in finding-magnitude:
- Pass 1: H1-H8 (1 critical, 4 medium, 3 minor) — architectural.
- Pass 2: H9-H14 (4 medium, 2 informational) — cross-cutting + semantic.
- Pass 3: H15 + clean — chain audit.
- Pass 4: H16-H22 (1 critical, 3 medium, 4 inspection) — subscriber wiring + presence row lifecycle.
- Pass 5: H23-H33 (3 low/medium fixed, 7 documented) — operational cleanup.

The findings are converging.  If we run another pass we'll likely find:
- A handful more cosmetic issues (dead code, comment drift).
- Maybe one more concurrency edge case if I keep being paranoid.
- Documentation that hasn't been updated to reflect the new design.

The pattern user has worried about ("every check exposes additional error") is converging.  Five passes is enough for this slice; subsequent passes will hit diminishing returns.

## Recommended next steps

1. **Land this commit batch** (H16+H20+H17+H18+H23+H24+H25 combined or split).  All tests pass; mutation tests verify contracts.
2. **Doc sync** (post-fix): update HEP-CORE-0023 §2.6 with the "presence row erased on disconnect" semantic, the cache invariant rule, and the H30 federation propagation note.  Update HEP-CORE-0030 §8 with the dual-trigger band cleanup (voluntary + cascade-via-role-disconnect).
3. **Move to downstream waves** (M1.4, MD1, Wave B M8) — Wave M3 foundation is stable.

## Archival

All five review documents archive together to `docs/code_review/archive/transient-2026-05-11/` after the commit lands:
- `REVIEW_WaveM3_2026-05-11.md` (first pass)
- `REVIEW_WaveM3_PostFix_2026-05-11.md` (second pass)
- `REVIEW_WaveM3_FullChain_2026-05-11.md` (third pass)
- `REVIEW_WaveM3_Rigorous_2026-05-11.md` (fourth pass)
- `REVIEW_WaveM3_FifthPass_2026-05-11.md` (this — fifth)

Record in `docs/DOC_ARCHIVE_LOG.md`.
