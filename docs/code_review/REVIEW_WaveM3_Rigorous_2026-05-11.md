# Wave M3 Rigorous Audit — No-Assumption Pass

**Date:** 2026-05-11 (fourth pass)
**Branch:** `feature/lua-role-support` (post H9-H13 fix sweep, 1819/1819 passing)
**Trigger:** User directive — "do the review thoroughly again. we keep missing things and every check exposes additional error. we need to make sure there is no assumption"

This pass questions every claim in the prior three reviews, re-reads each modified path, and checks the broker side + JSON serialization + subscriber wiring that the chain-audit review only examined at the surface.

---

## Findings the third-pass missed

### H16 — Broker missing `subscribe_role_disconnected` wiring ❌ CRITICAL

**Severity:** Critical.  State inconsistency that grows over time.
**Files:** `src/utils/ipc/broker_service.cpp` (no current `subscribe_role_*` calls anywhere); compare `src/utils/service/hub_script_runner.cpp:281`.

**Verified:** `grep -rn 'subscribe_role_disconnected' src/` returns exactly one match — `hub_script_runner.cpp`.  The broker has NO callback on role-disconnect.

**Why this is a real bug, not a theoretical one:**

`BrokerServiceImpl::band_on_role_closed(uid)` is the band-cleanup helper that:
1. Iterates `hub_state_->snapshot().bands` to find bands containing `uid`.
2. Calls `_on_band_left(band, uid)` for each.
3. Sends `BAND_LEAVE_NOTIFY` to remaining members.

It's wired imperatively from two places (`broker_service.cpp:2782` + `:2792`):
- `on_channel_closed` broker-fanout (called from DEREG_REQ last-producer + pending-timeout last-producer + script-requested close).
- `on_consumer_closed` broker-fanout (called from CONSUMER_DEREG_REQ + consumer-liveness sweep).

But Wave M3 step 5b+c introduced **TWO production paths** where role terminal-cleanup happens WITHOUT the broker fanout firing:
1. `_on_producer_dropped` non-last (multi-producer DEREG, H9 fix) — `drop.removed && !drop.channel_now_empty`; broker logs success and stops. No `on_channel_closed`, no band cleanup.
2. `_on_pending_timeout` non-last (multi-producer pending-timeout, H10 fix in `hub_state.cpp:1314-1325`) — same shape; broker increments counter and continues sweep. No band cleanup.

In both paths, the role being timed out may be erased by `_dispatch_role_disconnected_if_dead` if it has no other alive presence.  When that happens, the role still has stale band-membership entries in `pImpl->bands[].members`.

**Concrete scenario:**
- Role X registers as producer on Fan-In channel A (with co-producer Y) AND joins band Z.
- X DEREGs producer on A.  `_on_producer_dropped` non-last → X's producer-presence Disconnected → cache cleaned → dispatch fires → X has no other alive presence → role X erased from `pImpl->roles`.
- Broker's `on_channel_closed` is NOT called (channel A survives because Y is still there).
- Broker's `band_on_role_closed` is NOT called.
- `pImpl->bands["Z"].members` still contains X.
- A subsequent `BAND_FANOUT` to Z would attempt to deliver to X's `zmq_identity` — which is the stale identity from before X disconnected.  Behaviour: send-to-dead-identity is silently dropped by libzmq, but the broker thinks the message went out.

**Effect:** Stale band membership; bogus `BAND_LEAVE_NOTIFY` targets; admin queries to `query_bands` return wrong member counts.

**Fix needed (Wave M3 step 5f):**
In `BrokerService::run` (or wherever subscribers are wired — currently only in `hub_script_runner`), the broker should:
```cpp
hub_state_->subscribe_role_disconnected(
    [this, &router](const std::string &uid) {
        band_on_role_closed(router, uid);
    });
```

The existing `band_on_role_closed` is idempotent (snapshots bands, only fires `_on_band_left` if member found), so this subscriber path will safely no-op when the broker's `on_channel_closed` already cleaned up.

Caveat: `band_on_role_closed` needs a socket reference (for sending `BAND_LEAVE_NOTIFY`).  The subscriber runs on whatever thread fires the handler — for HubState ops invoked from the broker's IO thread, that's the broker IO thread → safe to use the router socket.  For ops invoked from L2 tests or admin RPC, the socket might be uninitialised — needs a null check or a different subscriber lifecycle.

**This bug existed pre-Wave M3** but was hidden because pre-M3, role-disconnect terminal cleanup ALSO didn't fire (H1 wiring gap).  H1 wiring fixed the role-disconnect path; H16 is the corresponding broker-side wiring that should ride with it.

---

### H17 — `upsert_presence_row_locked` no-op when presence exists, even if Disconnected ❌ MEDIUM

**Severity:** Medium.  Cache invariant transiently violated; producer/consumer state ambiguity.
**Files:** `src/utils/ipc/hub_state.cpp:629-630`.

**Problem:**
```cpp
inline void upsert_presence_row_locked(...) {
    if (channel.empty() || role_type.empty()) return;
    if (role.find_presence(channel, role_type) != nullptr) return;  // ← no-op even if Disconnected
    RolePresence p;
    p.state = RoleState::Connected;
    // ...
    role.presences.push_back(std::move(p));
}
```

The docstring claims "re-registration after voluntary close is handled by the heartbeat path which transitions Disconnected → Connected when a fresh heartbeat arrives" — this is incomplete now.

**Concrete scenario (post-Wave M3 step 5b+c+d+e):**
- Role X is producer on channel A AND consumer on channel B.  `X.presences = [(A,"producer",Connected), (B,"consumer",Connected)]`; `X.channels = [A, B]`.
- X DEREGs producer on A (Fan-In non-last; H9 path).
  - `on_dereg(A, "producer")` → presence `(A,"producer")` → Disconnected.
  - `drop_channel_if_orphaned(A)` → no alive presence on A → drops A from cache.
  - `X.presences = [(A,"producer",Disconnected), (B,"consumer",Connected)]`; `X.channels = [B]`.
  - Dispatch: X is alive on B → no-op.
- X re-REGs producer on A (e.g., process recovery).
  - `_on_producer_added` admits X to `ChannelEntry.A.producers[]`.
  - `upsert_role_locked` finds existing X, appends A to `channels`: `[B, A]`.
  - `upsert_presence_row_locked` finds existing `(A,"producer")` presence → **no-op (state stays Disconnected)**.

**Resulting three-view state mismatch:**
- `ChannelEntry.A.producers` contains X (admitted).
- `X.channels` contains A (re-added).
- `X.presences[(A,"producer")].state` is **Disconnected** (stale tombstone, not refreshed).

DISC_REQ for channel A from a consumer would see X as Disconnected (line `broker_service.cpp:1584` skips Disconnected presences) — even though X is admitted and channel A is live.  Channel observable computation gets the wrong answer.

**Eventual self-healing:** X sends first heartbeat → `on_heartbeat` transitions Disconnected → Connected.  But before that, the cache+presence+admission triad is internally inconsistent.

**Fix needed (Wave M3 step 5g):**
```cpp
inline void upsert_presence_row_locked(...) {
    if (channel.empty() || role_type.empty()) return;
    if (auto *p = role.find_presence(channel, role_type); p != nullptr) {
        // Re-arm: a re-REG after Disconnected (multi-presence role) MUST
        // transition the presence back to Connected (registering
        // sub-state) so the cache invariant + admission state are
        // consistent.  First heartbeat will set first_heartbeat_seen=true.
        if (p->state == RoleState::Disconnected) {
            p->state = RoleState::Connected;
            p->first_heartbeat_seen = false;
            p->last_heartbeat = now;
            p->state_since = now;
        }
        return;
    }
    // ... create new
}
```

This makes re-REG behave identically to fresh-REG: both put the presence in Connected/registering sub-state.

---

### H18 — Disconnected presence rows accumulate as tombstones ⚠️ MEDIUM (memory leak class)

**Severity:** Medium.  Bounded leak; matters for long-lived processor roles that churn channels.
**Files:** `src/include/utils/hub_state.hpp` (RoleEntry methods); `src/utils/ipc/hub_state.cpp`.

**Problem:**
`RoleEntry::on_dereg` and `on_pending_timeout` transition the presence state to Disconnected but DO NOT remove the row from `presences[]`.  Over a role's lifetime, Disconnected rows accumulate:
- Processor that attaches/detaches 100 input channels accumulates 100 Disconnected presence tombstones.
- `find_presence` becomes O(N) over the accumulation.
- JSON snapshot emits all tombstones (`hub_state_json.cpp:107`).
- Memory pressure for long-running hubs.

**Why this didn't surface earlier:**
- Pre-Wave M3, channels were 1:1 with producers; role-presence was always cleaned up at role-disconnect (which erased the role anyway).
- Wave M3 multi-presence model created the scenario where a role survives across many presence Disconnects.

**Design question:** is the Disconnected tombstone retention intentional (for diagnostic history)?  Probably not — `ChannelEntry.producers` doesn't keep tombstones; `BandEntry.members` doesn't keep tombstones.  Only `RoleEntry.presences` does.

**Fix needed (Wave M3 step 5h):**
Make `on_dereg` and `on_pending_timeout` remove the row instead of marking it Disconnected:

```cpp
TransitionEffect on_dereg(channel_, role_type_) noexcept {
    for (auto it = presences.begin(); it != presences.end(); ++it) {
        if (it->channel != channel_ || it->role_type != role_type_) continue;
        if (it->state == RoleState::Disconnected) return TransitionEffect::NoChange;
        presences.erase(it);
        return TransitionEffect::ToDisconnected;
    }
    return TransitionEffect::NoChange;
}
```

Then `any_presence_alive()` simplifies to `!presences.empty()`, and the cache rule simplifies to "drop channel iff no presence row references it" (no need to check state).

**Cascade implication:** if presences are erased on dereg, the Disconnected state is not externally observable — diagnostics only see Connected/Pending or "presence absent."  Consumers of the snapshot (admin queries, federation peers, the script subscribers) currently see Disconnected presences.  If we erase, those consumers see "no row" instead.  This is a small semantic shift.

If retaining diagnostic value is important, a hybrid: keep the row for some short window then erase via a sweep.  But that's a tombstone-with-TTL pattern which adds complexity.  Simpler to erase immediately and trust that "presence absent" = "was Disconnected" semantically.

---

### H19 — `_on_pending_timeout` last-producer path doesn't clean `channels` cache before falling through ⚠️ INSPECTION (no actual bug, but worth pinning)

**Severity:** Inspection.  No observable bug — `_on_channel_closed` cleans it up.  But worth documenting the call-chain assumption.
**Files:** `src/utils/ipc/hub_state.cpp:1314-1325` (the `is_last_producer` branch of `_on_pending_timeout`).

**Observation:**
In `_on_pending_timeout`, the non-last path explicitly calls `drop_channel_if_orphaned`:
```cpp
auto rm = it->second.remove_producer(role_uid);
(void)rit->second.drop_channel_if_orphaned(channel);
```

The last-producer path does NOT call `drop_channel_if_orphaned` — it falls through to `_on_channel_closed`, which iterates ALL roles on the channel (producer_uids + consumer_uids) and cleans each.

**Why this is fine but worth noting:**
- `_on_channel_closed`'s loop covers the timed-out producer (whose presence is now Disconnected).
- The producer's cache cleanup happens inside that loop.
- But if `_on_channel_closed` is ever refactored to NOT iterate producer_uids (e.g., to take an externally-provided list), this path breaks.

**Fix needed:** None today.  Add an inline comment in `_on_pending_timeout` that says "last-producer path delegates cache cleanup to `_on_channel_closed`'s iteration; if that contract changes, add explicit `drop_channel_if_orphaned` here."

---

### H20 — `_set_role_disconnected` body doesn't clean `pImpl->bands[].members[].role_uid` ❌ MEDIUM

**Severity:** Medium.  Mirror of H16 at HubState scope.
**Files:** `src/utils/ipc/hub_state.cpp:399-448` (`_set_role_disconnected`) + `:450-491` (`_dispatch_role_disconnected_if_dead`).

**Problem:**
Both terminal-cleanup entry points:
1. Iterate `pImpl->schemas` and evict owner-namespaced records.  ✓
2. Erase the role from `pImpl->roles`.  ✓
3. Do NOT touch `pImpl->bands`.

After terminal cleanup, `pImpl->bands[band].members` may still contain a `BandMember{role_uid=X, ...}` for the now-deleted role X.

**Effect:**
- `s.band(B)` returns members including X (stale entry).
- JSON snapshot emits stale members.
- Admin `query_bands` returns inconsistent state.
- `band_on_role_closed` (broker side) eventually cleans this up IFF triggered — which H16 says is incomplete.

**Fix needed (Wave M3 step 5i):**
In `_set_role_disconnected` and `_dispatch_role_disconnected_if_dead`, after erasing the role, iterate `pImpl->bands` and remove the uid from each band's members.  Fire `band_left` handler per removal (so the broker's subscriber — once H16 lands — can issue `BAND_LEAVE_NOTIFY`).

Alternative: have the broker subscriber (H16 fix) BE the one that calls `_on_band_left` per affected band.  This keeps HubState focused on roles+schemas and lets the broker handle cross-aggregate cleanup.  But it requires the broker subscriber to be called BEFORE the role entry is gone (so it can read the role's band memberships) — which doesn't fit the "handler fires after lock release with uid only" pattern.

**Cleanest fix:** HubState's terminal cleanup also evicts band memberships.  Broker's subscriber fires `BAND_LEAVE_NOTIFY` via the `band_left` handler chain.  Single source of truth for state; broker handles wire-level fanout only.

---

### H21 — `_on_channel_closed` consumer_uids iteration: post-H13 cleanup of cache is correct BUT `consumer_uids` is derived from snapshot BEFORE `_set_channel_closed` ⚠️ INSPECTION

**Severity:** Inspection.  Probably correct but the ordering deserves a pin-down test.
**Files:** `src/utils/ipc/hub_state.cpp:1023-1040`.

**Observation:**
```cpp
{
    std::shared_lock rlk(pImpl->mu);
    auto it = pImpl->channels.find(name);
    if (it != pImpl->channels.end()) {
        producer_uids = ...
        consumer_uids = ...
    }
}
_set_channel_closed(name);  // ← erases channel from pImpl->channels
{
    std::unique_lock lk(pImpl->mu);
    for (producer_uids: ...) on_dereg + drop_channel_if_orphaned
    for (consumer_uids: ...) on_dereg + drop_channel_if_orphaned
    ...
}
```

Between the shared_lock and the unique_lock, the channel is erased.  Then the unique_lock iterates the previously-captured uid lists and transitions their presences.  This is correct BUT requires that no other thread re-adds the channel between shared-lock release and unique-lock acquire.  If a concurrent `_on_producer_added` for the same channel slipped in during that window, it would create a fresh ChannelEntry with the same name + producer, and our subsequent loop would mark that producer's presence Disconnected — wrong.

In practice the broker serialises wire processing on its IO thread, so this race doesn't occur via DEREG_REQ + REG_REQ races.  But the heartbeat sweep, consumer-liveness sweep, and script-requested closes run on the same broker thread too — they're all serialised.  So the race is theoretical given the current architecture.

**Fix needed:** None today.  Note in the docstring that `_on_channel_closed` assumes single-writer serialisation upstream.  If we ever expose channel-close to multiple threads (e.g., federation peer race), this needs reinforcement.

---

### H22 — Re-REG after terminal cleanup that DIDN'T fire (multi-presence role) loses the cache append ordering invariant ⚠️ MINOR

**Severity:** Minor.  Cosmetic ordering issue.
**Files:** `src/utils/ipc/hub_state.cpp:682-687`.

**Observation:**
`upsert_role_locked` appends `added_channel` to `ex.channels` if not already present.  After H10 fixes, the cache is correctly maintained.  But the APPEND-IF-NOT-PRESENT semantics yield an ordering that depends on REG sequence + DEREG drops + RE-REG appends.  For a role that bounces between channels, the `channels` list order is no longer "registration order" — it's "current alive order."

This is fine for correctness, but tests asserting on `channels[0]` (like my H12 test) depend on this ordering.  Worth documenting the contract.

**Fix needed:** None.  Note the ordering contract in `RoleEntry::channels`'s docstring.

---

## Findings still open from prior passes (re-confirmed)

| ID | Title | Status |
|---|---|---|
| H15 | `_on_heartbeat` direct metrics-field mutation | ⚠️ DEFERRED — same trigger condition (no concrete misuse yet) |
| M3 step 5 | Strict `add_role` admission with global-uid uniqueness | ⚠️ DEFERRED — same trigger |
| M3 step 7 | Privatize `RolePresence` state-bearing fields | ⚠️ DEFERRED — same trigger; **H17/H18 fixes may surface this need earlier** |

---

## Test mutation sweep (sanity check on tests added in this round)

I'm verifying the new tests would actually fail against the pre-fix code (per the project's "sensitivity check before claim" rule).

| Test | Mutation that should break it | Verified? |
|---|---|---|
| `HubStateProducerDropped.MultiProducer_VoluntaryDereg_TransitionsPresenceAndCleansCache` | Remove the `on_dereg` + `drop_channel_if_orphaned` calls in `_on_producer_dropped` multi-producer path → role A's presence stays Connected → terminal cleanup doesn't fire → role A entry survives → `EXPECT_FALSE(s.role(A).has_value())` FAILS | ✅ Inspection-confirmed (matches old broken code) |
| `MultiChannel_Producer_StaysAliveAfterOneDereg_SchemasSurvive` | Restore the per-producer schema cascade in `_on_channel_closed` → schemas evicted even though X alive on B → `EXPECT_EQ(schema_count, 2u)` FAILS | ✅ Matches the bug we just fixed |
| `ConsumerPresence_AtomicallyTransitionsDisconnected` | Remove the consumer_uids loop from `_on_channel_closed` → consumer-presence stays Connected → dispatch sees alive presence → role NOT erased → `EXPECT_FALSE(role(cons.x.test).has_value())` FAILS | ✅ Matches the H13 bug |
| `RoleWithBothProducerAndConsumer_SameChannel_PartialDeregKeepsChannel` | Restore unconditional `chs.erase` in `_on_consumer_left` → channel dropped from cache even though producer alive → `EXPECT_EQ(channels.size(), 1u)` FAILS | ✅ Matches the H10 bug |

**Not done in this commit (would require running each mutation):** the actual deliberate-break-the-code-and-watch-it-fail process the project requires before claiming a test guards a contract.  Each new test was added with this mutation in mind; the inspection above stands in for the formal sweep until a follow-up commit.  **Recommend a separate test-rigor commit** to do the actual mutation runs against the four new tests.

---

## Status summary (fourth pass)

| ID | Title | Severity | Status |
|---|---|---|---|
| H1–H8 | First-pass findings | — | All closed |
| H9–H14 | Second-pass findings | — | All closed |
| H15 | `_on_heartbeat` metrics mutation | Tracked | Deferred (same trigger) |
| H16 | Broker missing role_disc subscription → band cleanup leaks | **Critical** | ❌ OPEN |
| H17 | `upsert_presence_row_locked` re-REG doesn't re-arm Disconnected | Medium | ❌ OPEN |
| H18 | Disconnected presence tombstone accumulation | Medium | ❌ OPEN |
| H19 | `_on_pending_timeout` last-producer cache cleanup delegated (correct but undocumented) | Inspection | ⚠️ Note |
| H20 | `_set_role_disconnected` doesn't cascade to bands | Medium | ❌ OPEN |
| H21 | `_on_channel_closed` shared→unique lock window assumption | Inspection | ⚠️ Note |
| H22 | `channels` ordering after bounce | Minor | ⚠️ Note |
| Mutation sweep | New tests not yet broken-and-verified | Test rigor | ⚠️ Follow-up |

## What needs fixing (priority order)

**Priority 1 — Wave M3 step 5f+i (one commit): cross-aggregate cleanup**
- H16: broker subscribes to `role_disconnected` → `band_on_role_closed`.
- H20: HubState terminal cleanup cascades to `pImpl->bands[].members[]` removal + fires `band_left` handler.
- Add L3 test: Fan-In multi-producer DEREG of a band member → band membership cleaned, BAND_LEAVE_NOTIFY delivered.

**Priority 2 — Wave M3 step 5g+h (one commit): presence-row lifecycle**
- H17: `upsert_presence_row_locked` re-arms Disconnected presence on re-REG.
- H18: `on_dereg` and `on_pending_timeout` erase the row instead of leaving a tombstone.
- Update `any_presence_alive` and `drop_channel_if_orphaned` to match the simpler semantics.
- Add L2 tests: re-REG after multi-presence DEREG transitions state correctly; long-running role doesn't accumulate `presences[]` entries.

**Priority 3 — Test rigor sweep**
- Actually run the mutation tests for the 4 new tests + the 2 L3 H1/H5 tests.

**Priority 4 — Doc-only items (H19, H21, H22)**
- Add cache-ordering note to `RoleEntry::channels` docstring.
- Add single-writer assumption note to `_on_channel_closed` docstring.
- Add "delegated cleanup" note to `_on_pending_timeout` last-producer path.

---

## Honest assessment

**The pattern the user called out is real.**  Each review pass finds new errors because:

1. **The change-set is large.**  M3 step 5b+c+d+e touched 5 op functions across `hub_state.cpp` + added a `RoleEntry` method + updated 4 tests + added 4 new tests.  Three full audit passes were not enough to surface everything.

2. **Cross-aggregate coupling is invisible from `hub_state.cpp` alone.**  H16 + H20 require reading the broker's fanout helpers AND the script-runner subscribers AND the HubState handlers — three different files.  My prior audits focused on `hub_state.cpp` because that's where the M3 work landed; the broker side was implicitly assumed correct.

3. **Idempotency masks latent gaps.**  H16's broker missing-subscription doesn't crash anything; it silently leaks band membership.  Same with H17 (heartbeat self-heals the state).  Same with H18 (memory grows but doesn't crash).  Without a deliberate audit that questions "what other state should this op cascade to?", these stay hidden.

4. **My third-pass review claimed "chain audit complete" too quickly.**  The Layer C event-processing audit listed the broker handlers but didn't trace whether each `_on_*` op had its corresponding broker-side cleanup wired.  Specifically: I should have grep'd for `subscribe_role_disconnected` in `broker_service.cpp` and noticed it's absent.

**To prevent another iteration of this pattern:** the Priority 1 + Priority 2 fixes need an L2/L3 test pair each that exercises the cross-aggregate cascade end-to-end.  Without such tests, the next refactor will reintroduce gaps that the unit-level L2 tests won't catch.

---

## Archival plan

When H16-H20 close:
- Archive all four review documents to `docs/code_review/archive/transient-2026-05-11/`.
- Record in `docs/DOC_ARCHIVE_LOG.md`.
- Cross-link from `docs/TODO_MASTER.md` until then.
