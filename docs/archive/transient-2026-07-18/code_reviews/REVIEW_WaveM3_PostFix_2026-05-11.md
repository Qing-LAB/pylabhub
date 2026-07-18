# Wave M3 Post-Fix Audit — RoleEntry Controlled-Access API

**Date:** 2026-05-11 (second pass)
**Branch:** `feature/lua-role-support` (post-fix sweep — H1+H2+H3+H4+H5)
**Scope:** Verify the H1+H5 wiring + H2+H3 routing + H4 comment fix, AND re-audit `hub_state.cpp` for any remaining inconsistencies the first pass missed.
**Tests after fix:** 1815/1815 passing, including 2 new L3 integration tests pinning the H1 wiring.

This is the formal second-pass review the user requested ("then perform another round of thorough review").  Previous review: `REVIEW_WaveM3_2026-05-11.md`.

---

## Status of first-pass findings

| ID | Title | Severity | Status |
|---|---|---|---|
| H1 | `_set_role_disconnected` had NO production callers | Critical | ✅ FIXED — `_dispatch_role_disconnected_if_dead` helper added; wired from 4 sites; 2 new L3 tests pin the contract end-to-end |
| H2 | `_on_channel_closed` direct presence mutation | Medium | ✅ FIXED — routes via `entry.on_dereg(name, "producer")` |
| H3 | `_on_consumer_left` direct presence mutation | Medium | ✅ FIXED — routes via `entry.on_dereg(channel, "consumer")` |
| H4 | Stale cascade comment in `_on_channel_closed` | Low | ✅ FIXED — rewritten to reflect locked dual-trigger decision (#2) |
| H5 | No L3 test for role-disconnect cleanup | Coverage | ✅ FIXED — `RoleEntry_TerminalCleanup_OnLastPresenceDisconnect` + `RoleEntry_TerminalCleanup_OnConsumerLeftLast` |
| H6 | Dual-trigger cascade interaction | Info | ✅ NO ACTION — idempotent by design |
| H7 | `_on_heartbeat` direct metrics mutation | Tracked | ⚠️ DEFERRED |
| H8 | `disconnected_fired` historical references | Trivial | ✅ NO ACTION |

L2 tests that codified the pre-M3 residue behavior:
- `ChannelClosed_RemovesChannelAndScrubsRole` → renamed `..._ErasesRoleEntry`; now asserts `EXPECT_FALSE(role(...).has_value())` (terminal cleanup contract).
- `ConsumerLeft_RemovesFromChannelAndDisconnectsPresence` → renamed `..._ErasesRoleEntry`; same update.

Both were the *expression* of the bug class M3 was meant to eliminate — keeping them as-is would mean shipping H1 unfixed.  Updated tests now pin M3's terminal-cleanup contract directly at L2.

---

## New findings in second pass

### H9 — `_on_producer_dropped` multi-producer path does NOT transition the role's producer-presence ❌ PRE-EXISTING, NEWLY VISIBLE

**Severity:** Medium.  Pre-existing bug not introduced by M3, but newly visible because H1 wiring shines a light on it.
**Files:** `src/utils/ipc/hub_state.cpp:972-977`.

**Problem:**
```cpp
if (!is_last_producer)
{
    auto rm = it->second.remove_producer(role_uid);  // drops from ChannelEntry.producers[]
    result.removed             = rm.removed;
    result.channel_now_empty   = false;
    return result;
}
```

A voluntary DEREG_REQ from a producer on a multi-producer (Fan-In) channel:
1. Removes the producer from `ChannelEntry.producers[]`.
2. Does NOT transition the role's `presences[channel, "producer"]` row to Disconnected.
3. Does NOT evict the producer's schemas (cascade only fires from `_on_channel_closed`).
4. Does NOT dispatch role-disconnect cleanup.

**Effect:**
- Role's `presences` shows a `(channel, "producer", Connected)` row even though the producer is no longer admitted on that channel.
- Snapshot consumers (admin queries, federation peers, hub script) observe ghost-Connected presences.
- If this was the role's last alive presence anywhere, the role entry lingers (the exact residue concern Wave M3 was supposed to eliminate, leaking through a code path Wave M3 didn't reach).
- Schemas owned by the producer survive past the producer's actual disconnection until the broker's heartbeat-timeout sweep eventually catches them on a different code path.

**Fix needed (Wave M3 step 5c):**
In the multi-producer branch, after `remove_producer`, also:
1. Call `rit->second.on_dereg(channel_name, "producer")` to transition presence Disconnected.
2. Maintain the `channels` cache invariant (drop channel iff no other presence on this role references it — decision #1).
3. Run schema cascade via `_on_schemas_evicted_for_owner(role_uid)` to align with `_on_channel_closed`'s per-producer eviction trigger.
4. Call `_dispatch_role_disconnected_if_dead(role_uid)` after lock release.

This is a separate commit because it requires the `channels` cache invariant work (H10).

---

### H10 — `_on_consumer_left` removes channel from cache unconditionally ⚠️ CACHE INVARIANT BUG

**Severity:** Medium.  Pre-existing.
**Files:** `src/utils/ipc/hub_state.cpp:1127-1130`.

**Problem:**
```cpp
auto &chs = it->second.channels;
auto  rm  = std::find(chs.begin(), chs.end(), channel);
if (rm != chs.end()) chs.erase(rm);
(void)it->second.on_dereg(channel, "consumer");
```

The `channels` cache should hold "channels this role has at least one alive presence on" (Wave M3 design decision #1, 2026-05-11).  The code drops the channel unconditionally even if the role still has another presence on the same channel — e.g., role X is producer AND consumer on `ch1`; CONSUMER_DEREG drops `ch1` from `channels` cache while the producer-presence on `ch1` still references it.

**Effect:**
- Inconsistent state: `presences` contains `(ch1, "producer", Connected)` but `channels` is empty.
- Admin queries that derive "which channels does role X work with?" from `channels` give wrong answers.
- Affects any code that reads `RoleEntry.channels` after partial disconnect.

**Fix needed (Wave M3 step 5d):**
After `on_dereg`, conditionally drop the channel from `channels`:
```cpp
(void)it->second.on_dereg(channel, "consumer");
// Decision #1: drop channel from cache only if no other presence references it.
bool other_presence = false;
for (const auto &p : it->second.presences)
    if (p.channel == channel && p.state != RoleState::Disconnected)
    {
        other_presence = true;
        break;
    }
if (!other_presence)
{
    auto &chs = it->second.channels;
    auto  rm  = std::find(chs.begin(), chs.end(), channel);
    if (rm != chs.end()) chs.erase(rm);
}
```

---

### H11 — `_on_channel_closed` removes channel from EVERY role's cache, including roles whose consumer-presences are still Connected ⚠️ DESIGNED-BUT-INCONSISTENT

**Severity:** Low–Medium.  Designed behavior, but inconsistent with `RoleEntry.channels` semantics.
**Files:** `src/utils/ipc/hub_state.cpp:1037-1041`.

**Context:**
Channel close marks ALL producer-presences Disconnected via the H2 fix.  Consumer-presences are NOT atomically transitioned by `_on_channel_closed` — they wait for their own CONSUMER_DEREG_REQ (or consumer-liveness sweep).  But the same loop unconditionally removes the channel from EVERY role's `channels` cache.

**Effect:**
- A consumer role with a still-Connected `(channel, "consumer")` presence loses `channel` from its `channels` cache, but the presence row stays Connected.
- Same `channels`-vs-`presences` divergence as H10.
- Resolves itself when the consumer eventually issues CONSUMER_DEREG (and `_on_consumer_left` runs the cache fix from H10's proposed pattern).  Until then, the snapshot is inconsistent.

**Fix needed (Wave M3 step 5d, same commit as H10):**
Iterate roles; for each, drop the channel from `channels` ONLY if the role has no alive non-producer presence on it (since producer-presences are already being marked Disconnected here).  More precise: defer the cache cleanup to the post-`on_dereg` per-presence path used by H10's fix.

---

### H12 — Per-producer schema cascade at channel-close fires even when the producer is alive on OTHER channels ⚠️ HEP-CORE-0034 §7.2 SEMANTIC GAP

**Severity:** Medium (correctness — currently silent because tests don't exercise multi-channel producers).
**Files:** `src/utils/ipc/hub_state.cpp:1063-1064`.

**Problem:**
```cpp
for (const auto &producer_uid : producer_uids)
    _on_schemas_evicted_for_owner(producer_uid);
```

This evicts a producer's owner-namespaced schemas whenever ONE of their channels closes — even if the producer is still alive on other channels.  Per HEP-CORE-0034 §7.2, schema eviction follows owner lifetime, not channel lifetime.  Hub-globals are immune; producer-owned schemas should die when the OWNER's last presence dies, not when one channel of theirs closes.

**Effect:**
- Producer X owns schema `(X, "frame")`.  X is alive on `ch1` and `ch2`.
- `ch1` closes (heartbeat timeout).  `_on_channel_closed(ch1)` evicts `(X, "frame")`.
- X is still alive on `ch2`, but its schema record is now gone.
- Any future operation on `ch2` that resolves `(X, "frame")` through HubState (consumer joining, cite-by-id) fails.

This is the dual-trigger from M3 decision #2 — but decision #2 was stated as "both correct and idempotent", not "the per-channel trigger may evict prematurely".  The reasoning was that schema-records-die-at-role-disconnect is the primary trigger; the per-producer-at-channel-close was a *belt-and-suspenders* duplicate.  In single-channel-per-producer reality, the two triggers fire at the same logical moment.  In multi-channel-per-producer, they diverge — and the per-channel trigger is wrong.

**Fix needed (Wave M3 step 5e):**
Remove the per-producer schema cascade from `_on_channel_closed`.  Schema eviction becomes exclusively owner-lifetime via `_dispatch_role_disconnected_if_dead` → terminal cleanup body.  This restores HEP-CORE-0034 §7.2's "owner-lifetime" intent and eliminates the divergence.

Test gap: no L2 test currently exercises a producer alive on two channels who loses one.  Add one as part of the H12 fix.

---

### H13 — Consumer-presences on a closed channel stay Connected until CONSUMER_DEREG_REQ ℹ️ DESIGNED, BUT WORTH DOCUMENTING

**Severity:** Info.  Designed behavior; not a bug.
**Files:** `src/utils/ipc/hub_state.cpp:1037-1052`.

**Context:**
When a channel closes via producer-side teardown (last producer leaves), `_on_channel_closed`:
- Marks producer-presences on that channel Disconnected (via H2 fix).
- Does NOT transition consumer-presences on that channel.

Consumer-presence cleanup happens asynchronously: broker fan-outs `CHANNEL_CLOSING_NOTIFY` → consumer issues `CONSUMER_DEREG_REQ` on its next iteration → `_on_consumer_left` → presence Disconnected → dispatch.

This is correct because (a) consumer state is owned by the consumer process, (b) crash detection is the consumer-liveness sweep's job.  But until the consumer sends DEREG (or is reaped), snapshots show `(closed_channel, "consumer", Connected)` rows on consumer roles.

**Fix needed:** None — this is intentional separation of producer-side and consumer-side state machines.  Should be documented in HEP-CORE-0023 §2.1.1 atomic-teardown text (currently the docstring at `_on_channel_closed:986-991` is the only place this is spelled out).

---

### H14 — Schema cascade redundancy: `_on_channel_closed` runs eviction twice per producer when H1 wiring fires ℹ️ PERFORMANCE / IDEMPOTENCY-BY-DESIGN

**Severity:** Info.
**Files:** `src/utils/ipc/hub_state.cpp:1063` + `:464` (inside `_dispatch_role_disconnected_if_dead`).

After H1+H2 wiring:
- `_on_channel_closed` runs `_on_schemas_evicted_for_owner(producer_uid)` → evicts K records, bumps counter by K.
- Then runs `_dispatch_role_disconnected_if_dead(producer_uid)` → terminal cleanup iterates schemas again, finds 0 owned by this uid, bumps counter by 0.

Two iterations per producer, but only one effective eviction.  No correctness issue.  Resolves automatically when H12 fix lands (per-producer-at-close cascade removed; dispatch becomes the sole trigger).

---

### H15 — `_on_heartbeat` metrics block remains a direct field mutation ⚠️ DEFERRED (was H7)

**Severity:** Tracked.
**Files:** `src/utils/ipc/hub_state.cpp:1232-1240` (metrics-write block after `entry.on_heartbeat`).

Same finding as previous pass.  Not in scope for this fix sweep.  A future `entry.set_presence_metrics(channel, role_type, metrics)` method would absorb this — out of scope for Wave M3 step 5b.

---

## Cross-cutting observations

### O1 — The H1 wiring revealed an architectural truth: "channel-close cascade" was a stand-in for missing role-lifetime cascade

Three findings (H9, H11, H12) all share a root cause: pre-M3 code used "channel close" as a proxy event for "role disconnect" because the role-disconnect path didn't fire in production (the H1 wiring gap).  With H1 fixed, the channel-close-triggered cleanup is increasingly redundant or wrong:
- H9: `_on_producer_dropped` multi-producer needs to transition presence + dispatch.
- H11: `_on_channel_closed` cache-erase loop violates `channels`-vs-`presences` invariant for consumer roles.
- H12: per-producer schema cascade at channel-close evicts too eagerly.

These three should land in a single follow-up commit (Wave M3 step 5c+d+e) that shifts cleanup duties from "channel-close" to "role-disconnect" wherever the two diverge.

### O2 — `channels` cache vs `presences` derivation invariant is under-specified

`RoleEntry.channels` is documented as a cache, but the precise rule for when entries are added/removed is split across several call sites with subtle differences.  Wave M3 design decision #1 stated the rule informally; no code review enforces it.

Suggestion: add a `RoleEntry::cache_consistent() const` debug-check method that walks `presences` and verifies every channel referenced has a corresponding `channels` entry, and every `channels` entry has at least one alive presence.  Invoke from a debug-build assertion at op-tail boundaries.  Out of scope for this sweep; tracked.

### O3 — `_dispatch_role_disconnected_if_dead` body duplicates `_set_role_disconnected` logic

The schema-cascade-and-erase recipe is implemented twice (once in `_set_role_disconnected`, once in `_dispatch_role_disconnected_if_dead`).  Intentional per the commit comment ("not factored out so each entry point reads top-to-bottom"), but invites drift if the cascade gets a new responsibility (e.g., HEP-CORE-0034 §10.x adds a new eviction class).

Mitigation: a code-review checklist note that any change to the schema-cascade-and-erase recipe in `_set_role_disconnected` MUST be mirrored in `_dispatch_role_disconnected_if_dead`.  Add to `docs/IMPLEMENTATION_GUIDANCE.md` "Cross-method synchrony" section if one exists; otherwise add a brief item.

---

## Status Summary (second pass)

| ID | Title | Severity | Status |
|---|---|---|---|
| H1–H8 | First-pass findings | Various | ✅ Resolved or correctly deferred |
| H9 | `_on_producer_dropped` multi-producer presence not transitioned | Medium | ❌ OPEN (pre-existing, newly visible) |
| H10 | `_on_consumer_left` cache erase unconditional | Medium | ❌ OPEN (pre-existing) |
| H11 | `_on_channel_closed` cache erase unconditional | Low–Medium | ❌ OPEN (pre-existing) |
| H12 | Per-producer schema cascade at channel-close evicts prematurely | Medium | ❌ OPEN (multi-channel producers) |
| H13 | Consumer-presences stay Connected past channel-close | Info | ✅ Documented (no fix) |
| H14 | Double schema-iteration in `_on_channel_closed` after H1 | Info | ✅ Resolves with H12 fix |
| H15 | `_on_heartbeat` direct metrics mutation | Tracked | ⚠️ DEFERRED |
| O1–O3 | Architectural observations | n/a | Documented |

## What needs fixing next

**Priority 1 — Wave M3 step 5c+d+e (one focused commit):**
- H9: route `_on_producer_dropped` multi-producer through `on_dereg` + cache fix + schema cascade + dispatch.
- H10 + H11: `channels` cache invariant fix (drop channel only when no other presence references it).
- H12: remove per-producer schema cascade from `_on_channel_closed`; rely on `_dispatch_role_disconnected_if_dead` exclusively.
- Add the missing L2 test for multi-channel producer schema retention.

**Priority 2 — Documentation (no code):**
- HEP-CORE-0023 §2.1.1 — spell out that consumer-presences on a closed channel transition asynchronously via CONSUMER_DEREG.
- Decision #1 — codify the cache rule in `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` (currently only in `docs/tech_draft/M3_role_entry_controlled_access.md`).

**Priority 3 — Defer:**
- H15: `set_presence_metrics` API, on demand.

## Archival

Once H9–H12 close, archive both review documents (this + the first pass) to `docs/code_review/archive/transient-2026-05-11/` per `docs/DOC_STRUCTURE.md §1.7`.  Cross-link from `docs/TODO_MASTER.md` "Active code review" until then.
