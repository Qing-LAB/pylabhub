# Wave M3 — RoleEntry / RolePresence Controlled-Access API

| | |
|---|---|
| **Status**  | ✅ **SHIPPED 2026-05-11** in a commit batch covering H1+H5 wiring, H9+H10+H11+H12 cache invariant + schema cascade owner-lifetime, H16+H17+H18+H20 cross-aggregate cleanup + presence row erase, H23+H24+H25 log gating + dtor + mutation verification.  Six review passes archived as `docs/code_review/REVIEW_WaveM3_*_2026-05-11.md`.  Two mutation tests + the 2026-05-12 mutation-sweep on H1/cascade/erase contracts verify the design is properly pinned.  Suite at landing: 1823/1823; current: 1836/1836 (post-M1.4 strengthening).  Explicitly deferred (bug-driven): step 5 strict `add_role` admission, step 7 privatize fields, H15 metrics direct-mutation, H29/H36/H39 cosmetic dead-code, H40 concurrency hardening — all tracked in `docs/todo/MESSAGEHUB_TODO.md` "Wave M3 explicitly DEFERRED items".  **Status banner updated 2026-05-12** during the cross-record audit pass. |
| **Created** | 2026-05-11 |
| **Wave**    | M3 (follows Wave M2.5 closure) |
| **Drives**  | (1) Retired the `RoleEntry.disconnected_fired` 🚧 PATCH (formerly `hub_state.hpp:733-757`) by making role-disconnect cleanup terminal — when the last presence transitions Disconnected, the `RoleEntry` is erased from `pImpl->roles`.  (2) Applied the same controlled-access pattern as M2.5 (typed result enums, additive admission ops, per-presence FSM state behind methods) so `RoleEntry` accumulates no per-party scalars and per-presence FSM transitions go through a wall API. |
| **Naming note** | **Wave M3** here = RoleEntry controlled-access API.  This is DISTINCT from **Wave B M3** in `M1_FSM_consolidation_handoff_2026-05-09.md` (RoleHandler skeleton).  Wave B is the architectural M0-M9 stack for the role-side template refactor; Wave M3 is a HubState-side closure of the M2.5 pattern.  Always use the full "Wave M3" or "Wave B M3" prefix to avoid confusion. |
| **Resume point — return to main line** | Closed.  Wave M2's MP3-MP5 absorbed into M2.5 + Wave M3 as side effects; M1.4 also shipped 2026-05-11.  Remaining chain (re-verified + re-ordered 2026-05-12): **MD1** (role teardown use-after-free race) → **M1.5** (`on_channel_closing` callback + auto-stop — re-framed; see `docs/tech_draft/M1.5_channel_closing_redesign_2026-05-12.md`, not the original "FORCE_SHUTDOWN handler" framing) → **Wave B M8 / MP6** (federation/dual-hub).  See `docs/TODO_MASTER.md` for the current canonical chain. |

---

## 1. Scope

The M2.5 pattern applied to `RoleEntry` + `RolePresence`:

| Concern | Today | After M3 |
|---|---|---|
| Per-presence FSM state (`state` / `last_heartbeat` / `state_since` / `first_heartbeat_seen`) | Public fields on `RolePresence`; mutated directly inside HubState ops | Stays in `RolePresence` but the only writers go through `RoleEntry::on_heartbeat(channel, role_type, now)` / `on_pending_timeout` / `on_dereg` returning a typed `TransitionEffect` enum (per design doc §5.2). |
| Per-uid liveness | Derived via `any_presence_alive()` (already correct) | Same; no change. |
| `role_disconnected` event-emit memoization | `RoleEntry.disconnected_fired` flag — 🚧 PATCH (an event-emit flag that ought to be a one-shot, not a stored field) | Retired.  When the last presence transitions Disconnected, `_set_role_disconnected` erases the `RoleEntry` from `pImpl->roles` (terminal cleanup).  A future REG_REQ for the same uid would NOT find a residual entry — the `add_role` op rejects on residue (same shape as M2.5 §6.2 strict-uid-reject policy applied at role scope). |
| Presence add | `upsert_presence_row_locked` returns early when (channel, role_type) already exists | `RoleEntry::add_presence` purely additive; same-tuple re-add returns `RejectedDuplicate` (per design doc §5.2). |
| Cascade evictions on role disconnect | Driven from `_on_channel_closed` per-producer-uid (HEP-CORE-0034 §7.2) | Driven from `_set_role_disconnected` per-uid as the canonical owner-lifetime trigger.  Schemas evict on role-disconnect, not channel-close. |

## 2. Field classification — `RoleEntry`

Today `RoleEntry` fields:
- `uid` / `name` / `role_tag` / `first_seen` / `pubkey_z85` — role-wide invariants (set on create, immutable after).
- `channels[]` — convenience list of channel names this role participates in.
- `presences[]` — per-(channel, role_type) FSM rows.
- `disconnected_fired` — 🚧 PATCH to be retired.

Post-M3: same fields minus `disconnected_fired`.  All FSM state continues to live on `RolePresence`.

## 3. API surface

From `controlled_access_api_design.md` §5.2 (copied here for M3-local reference):

```cpp
// Reads:
const RolePresence* find_presence(string_view channel, string_view role_type) const noexcept;
std::span<const RolePresence> presences() const noexcept;
bool any_presence_alive() const noexcept;  // existing

// Mutation:
enum class AddPresenceResult { Created, RejectedDuplicate };
AddPresenceResult add_presence(string channel, string role_type,
                                RolePresence** out = nullptr);
bool              remove_presence(string_view channel, string_view role_type);

// Transition primitives:
enum class TransitionEffect {
    NoChange,        // presence already in target state
    Refreshed,       // last_heartbeat updated, no FSM transition
    NewlyConnected,  // first heartbeat seen (was registering)
    ToPending,       // Connected -> Pending
    ToDisconnected,  // Pending -> Disconnected (or direct from REG cleanup)
};
TransitionEffect on_heartbeat(string_view channel, string_view role_type,
                              steady_clock::time_point now);
TransitionEffect on_pending_timeout(string_view channel, string_view role_type);
TransitionEffect on_dereg(string_view channel, string_view role_type);

// Decision #3 (2026-05-11): NO memoization API.  Terminal-erase of
// the RoleEntry on last-presence-Disconnected IS the memoization —
// a second call to `_set_role_disconnected` finds no entry and
// returns immediately.  The 🚧 PATCH retires by construction.
```

## 4. HubState ops (post-M3 shape)

| Op | Today | M3 |
|---|---|---|
| `_set_role_registered` | Inserts/updates `RoleEntry` | Stays.  Rejects on residue (same-uid not in normal flow). |
| `_set_role_disconnected` | Marks every presence Disconnected; clears `disconnected_fired` memoization; runs schema cascade if not already fired | Becomes terminal cleanup: walks presences in-place; runs schema cascade (HEP-0034 §7.2); ERASES the `RoleEntry` from `pImpl->roles`.  Idempotent — second call finds no entry. |
| `add_role` (new) | n/a | Used by REG_REQ to admit a fresh role.  Rejects with `RejectedUidConflict` if `pImpl->roles[uid]` exists. |

## 5. Migration plan

8-step pattern mirroring M2.5:

| Step | Scope | Files |
|---|---|---|
| **0** | Lock design (THIS doc) + open decisions: schema cascade owner-lifetime semantics (HEP-0034 §7.2 confirms); residue policy (matches M2.5 §6.2); whether `RoleEntry.channels[]` becomes derived or stays cached. | (this file) |
| **1** | Additive API methods on `RoleEntry` (`add_presence` / `on_heartbeat` / `on_pending_timeout` / `try_consume_disconnect_event` returning typed enums).  Existing direct mutators (`upsert_presence_row_locked` etc.) stay; fields stay public.  Suite must still pass 100%. | `src/include/utils/hub_state.hpp` |
| **2** | Migrate `_on_heartbeat` to call `entry.on_heartbeat(channel, role_type, now)` returning `TransitionEffect`; the observability-changed dispatch reads off the enum.  No behavior change in the typical path. | `src/utils/ipc/hub_state.cpp` |
| **3** | Migrate `_on_heartbeat_timeout` + `_on_pending_timeout` likewise. | `src/utils/ipc/hub_state.cpp` |
| **4** | Rewrite `_set_role_disconnected` as terminal cleanup: on last presence Disconnected, fire schema cascade, then erase `RoleEntry`.  Retire `disconnected_fired`.  Test fixture (HubStateTestAccess) gets `inject_orphan_role` for residue-test scenarios. | `src/utils/ipc/hub_state.cpp`, `tests/test_framework/hub_state_test_access.h` |
| **5** | ⚠️ **DEFERRED 2026-05-11** — original scope was "new `add_role(uid, ...)` op with strict residue-reject (RejectedUidConflict)."  Step 4 already eliminated the residue concern structurally (terminal cleanup means `pImpl->roles[uid]` is gone after disconnect).  A `add_role(strict)` would change a different contract: cross-channel multi-presence (processor: one role, producer presence on out_channel + consumer presence on in_channel via separate REG_REQs).  The M2.5 §6.2 policy as implemented is per-(channel, role_type, uid) — NOT global per-uid.  Codifying "global strict reject" would break the processor case; codifying "global strict reject with identity-based extension allowance" needs a security-design pass (compare ZMQ identity on second admission).  **Trigger: a concrete spoofing-attempt observation OR a policy decision to require strict global uid uniqueness.**  Until then, `upsert_role_locked` continues to handle "create or extend existing role's presence list" with the existing per-channel uid uniqueness gates (`ChannelEntry::add_producer/add_consumer`). | `src/utils/ipc/hub_state.cpp`, `src/utils/ipc/broker_service.cpp:handle_reg_req` |
| **6** | Tests: residue rejection, terminal cleanup, schema cascade timing.  Per-presence FSM tests pin `TransitionEffect` values. | `tests/test_layer2_service/test_hub_state.cpp` |
| **7** | Privatize `RolePresence` state-bearing fields (or move into Impl) once all writers go through the API.  Optional polish; same trigger condition as M2.5 step 7 (a concrete misuse bug or audit observation). | `src/include/utils/hub_state.hpp` |
| **8** | HEP doc sync.  Update HEP-CORE-0023 §2 (FSM rewrite mentions the new transition primitives), HEP-CORE-0034 §7.2 (cascade-on-role-disconnect already specified; add cross-ref to `_set_role_disconnected` terminal-cleanup behavior). | `docs/HEP/HEP-CORE-0023.md`, `docs/HEP/HEP-CORE-0034.md` |

## 6. Decisions locked 2026-05-11

All four open items locked.  All chose the recommended option.

1. **`RoleEntry.channels[]` — KEEP AS CACHE.**  Mutated alongside
   `add_presence` / `remove_presence`.  O(1) read for "which channels
   is this role on" admin queries.  Document explicitly so
   contributors don't write a stale-cache bug; add a maintenance
   invariant comment on the field.

   **Cache-refresh discipline (note added 2026-05-11):** the hub's
   script-level `on_tick` hook can run periodic tasks that re-sync
   the `channels[]` cache from `presences[]` to defend against any
   transient drift.  This is normal hub-script work — treat it as a
   recoverable invariant, not a hard guarantee.  The expectation is
   that the cache is correct after every controlled-access mutation;
   `on_tick` is a backstop, not the primary mechanism.

2. **Schema cascade — FIRE FROM BOTH PATHS (idempotent).**
   `_on_channel_closed` continues to fire the per-producer cascade
   (M2.5 ordering preserved); `_set_role_disconnected` adds a
   second firing for the role-lifetime trigger.  Eviction is
   already idempotent (second call on a gone record is a no-op),
   so the dual-trigger has zero behavior risk.  Matches HEP-CORE-0034
   §7.2 wording ("evict on owner Disconnected") via the
   `_set_role_disconnected` path while keeping the channel-close
   path that already works.

   > **⚠️ SUPERSEDED 2026-05-11 (Wave M3 step 5e / H12 fix).**
   > The dual-trigger broke HEP-CORE-0034 §7.2 owner-lifetime
   > semantics in the multi-channel-producer case: a producer
   > alive on channels A and B would lose all owner-namespaced
   > schemas the moment EITHER channel closed, even though the
   > role itself was still alive on the other channel.  The
   > per-producer cascade in `_on_channel_closed` has been
   > REMOVED; schema eviction is now owner-lifetime ONLY, fired
   > from `_dispatch_role_disconnected_if_dead` →
   > `_set_role_disconnected` body when the role has no remaining
   > alive presence.  Test pinning the new contract:
   > `HubStateProducerDropped.MultiChannel_Producer_StaysAliveAfterOneDereg_SchemasSurvive`
   > in `tests/test_layer2_service/test_hub_state.cpp`.

3. **`disconnected_fired` memoization — DROPPED ENTIRELY.**
   Once terminal cleanup is in place, the entry being gone IS the
   proof that the event already fired — no second call can fire
   because no entry survives.  This retires the 🚧 PATCH at
   `hub_state.hpp:733-757` by construction.  No
   `try_consume_disconnect_event()` API needed.

4. **Wave B M3 vs Wave M3 naming** — resolved 2026-05-11 via a
   top-of-doc note in `M1_FSM_consolidation_handoff_2026-05-09.md`
   (commit `f124a69`).  Always use full prefix.

## 7. Trigger condition — when to start

- **Prerequisite:** Wave M2.5 closed (it is, post `416cbec`).
- **Optional prerequisite:** M1.4 (`metrics_store_` retirement).  Could land first, but doesn't block — `metrics_store_` is independent of role-cleanup.
- **Suggested ordering:** Start M3 immediately after the user signs off on this design doc.  The 🚧 PATCH at `hub_state.hpp:733-757` is a small but real correctness gap (it's marked `disconnected_fired` for a reason — the current cleanup pattern needs an event-emit-once gate, which is exactly what terminal-erase makes structural).

## 8. Risks / non-goals

- **NOT** a redesign of `RolePresence` itself.  The FSM stays Connected/Pending/Disconnected.
- **NOT** a federation change.  Peer presence replication is Wave B M8 / MP6 territory.
- **NOT** the role-side template refactor (`Wave B M3-M9` from the handoff doc).  Those are separate work tracks.
- **Risk:** terminal cleanup of `RoleEntry` means handlers subscribing to `role_disconnected` fire with a uid whose entry is gone.  Handlers must not assume the entry is queryable from the callback.  Mitigation: pass the uid (string) directly to the handler, not a `const RoleEntry&` pointer; this is already the current signature.
- **Risk:** test fixtures that build channels with `make_channel` and call `HubStateTestAccess::on_channel_registered` (the M2.5 test-only legacy primitive) need to be re-checked — those tests create implicit `RoleEntry` rows via the upsert path.  After M3 the test path still works (the upsert creates the entry; `_set_role_disconnected` erases on last presence Disconnected, which tests trigger via timeout).  Verify the cascade fires consistently.
