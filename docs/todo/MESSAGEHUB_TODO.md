# Messenger / Broker TODO

**Scope:** MessageHub / broker protocol / federation / hub-side
connection lifecycle.
**Canonical strategic status:** `docs/TODO_MASTER.md` § "Current Sprint
Focus".  This file holds broker-specific detail for open items only;
completed work lives in git history + DOC_ARCHIVE_LOG.

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/MESSAGEHUB_TODO_completions.md`
(A1 `ctx_band_leave` semantic bug — was listed as "D1 must-fix" but
the fix is in production code at `native_engine.cpp:289-305`).

---

## Current Status (broker-specific summary)

| Track | Where it stands | Active item here |
|---|---|---|
| **Arc A — `plh_hub` renovation** (HEP-0033 §15 Phase 1..10) | Phases 1-9 shipped; Phase 10 doc-amendment ⏳ partial. | HEP-0033 Phase 10 (task #73) |
| **Arc B — role-host renovation** (Wave-B M0..M9) | M0..M9 shipped (M9 closed 2026-05-26). | — |
| **HEP-CORE-0035 auth** | 🚧 partial — Phase B + #101 + D1 + D2 + D3 shipped (per `AUTH_TODO.md`).  D4–D7 open.  Critical-path for production readiness. | task #74; detail in `docs/todo/AUTH_TODO.md` |
| **HUB_TARGETED_ACK wire frame** (HEP-0033 §12.3.6) | ⏸ Deferred — C++ `augment_peer_message` surface in place; wire bit not landed | (task #75) — federation-only |

Wave-M2 / Wave-M2.5 / Wave-M3 side-arcs all closed.  M1.2 / M1.4 /
M1.5 / MD1 / MD1.5 all closed.

### Queue-owned topology + layer cleanup — P1+P2+P3 SHIPPED (2026-07-11)

**Plan + landing log:** `docs/tech_draft/DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md`

Broker/HubState landings + open items:

- **[HIGH bug] Stale `binding_side_confirmed_allowlist` (C3) ✅
  FIXED 2026-07-11.**  `_on_consumer_revoked` now clears the
  confirmed snapshot on any successful erase (D-2 whole-set
  clear).  Next `_on_binding_confirmed` reconstructs.  Six L2
  tests in `test_hub_state.cpp` pin the fix.  Channel-access-
  close path is already covered by full-entry erase in
  `_on_channel_access_closed` (no additional change needed).
- **[MED] `handle_check_peer_ready_req` failure-mode split (C2)
  ✅ ADDRESSED 2026-07-11.**  BRC method behavior unchanged;
  role-side collapse eliminated by moving the readiness path
  inside the queue's `finalize_connect` with a typed
  `PeerReadinessOracle::PollResult` (Ready / NotReady /
  PermanentError).
- **[VERIFY] C4 admission derivation ✅ VERIFIED 2026-07-11.**
  `role_api_base.cpp:2349-2364` binds `admission` from rx_queue
  first with tx_queue fallback; fan-in consumer's rx_queue IS a
  PeerAdmission, cast succeeds.  No bug.
- **[Refactor P6 — PENDING] `binding_side_confirmed_allowlist`
  full-set copy → version-tagged membership.**  Replace with
  `confirmed_version: uint64` per channel + `authorized_at_version:
  uint64` per pubkey.  Membership check becomes
  `authorized_at_version <= confirmed_version`.  Removes the
  duplicate string set; correctness matches P2.a semantics
  without needing a separate invalidation step.  Reuse P2.a's
  L2 test suite as the correctness gate.
- **[Refactor P5 — PENDING] Queue emits its own apply-
  confirmation.**  Retire the role-side `handle_channel_auth_notifies`
  branching on `rx_queue->is_binding_side()`.  Queue's
  `set_peer_allowlist` returns `AppliedResult{side,
  applied_version}`; queue calls an injected `ConfirmationEmitter`
  that publishes `CHANNEL_AUTH_APPLIED_REQ` with `role_type`
  derived from `AppliedResult.side`.

---

### Loop-ready gate + fan-in binding-side reader arc ✅ SHIPPED (2026-07-11)

Broker-side landings on the arc — full arc summary in
`docs/todo/API_TODO.md`:

- **`GET_CHANNEL_AUTH_REQ` handler.**  Caller authorization moved
  from "must be a registered producer of the channel" to "must be
  the binding-side role" (topology-aware).  Wire error code
  `PRODUCER_NOT_AUTHORIZED` preserved for parser back-compat;
  message names the topology-aware rule.  Response semantics
  split: `CHANNEL_NOT_FOUND` when channel doesn't exist;
  `status="success", allowlist=[]` (plus loud ERROR log) when
  channel exists but `ChannelAccessEntry` is missing.  See
  HEP-CORE-0036 §6.6.1 + §6.6.2.
- **`CHECK_PEER_READY_REQ` handler (new wire).**  Dialing-side
  role's readiness pull.  Authorization mirror of §6.6.1 — caller
  must be a registered dialing-side role of the channel.
  Read-only against `ChannelAccessEntry.binding_side_confirmed_allowlist`.
  Reason codes: `not_admitted` (broker hasn't admitted caller) vs
  `not_confirmed` (admitted but binding-side hasn't confirmed apply).
  New error code `NOT_A_ROLE_OF_CHANNEL` in the §6.6 taxonomy.
- **`CHANNEL_AUTH_APPLIED_REQ` handler extended.**  Accepts
  `role_type` discriminator; consumer branch skips the
  stale-instance guard (consumers have no `instance[C]`) and
  snapshots the current `authorized_consumer_pubkeys` into
  `binding_side_confirmed_allowlist` — the state
  `CHECK_PEER_READY_REQ` consults.  Producer branch unchanged.
- **Fan-in consumer-opens-channel `_on_channel_access_opened`
  wiring.**  The invariant "channel opens ⟹ access record
  exists" now holds symmetrically: producer opens on fan-out /
  one-to-one; consumer opens on fan-in.
- **Fan-in consumer self-admission suppression.**  On
  CONSUMER_REG_REQ under fan-in, broker no longer adds the
  consumer's own pubkey to `ChannelAccessEntry.authorized_consumer_pubkeys`
  (that set holds producer pubkeys under fan-in) and no longer
  fires the self-targeted CHANNEL_AUTH_CHANGED_NOTIFY — both
  would have inflated the consumer's `admitted_peers_count`
  reading of its own gate.
- **`HubState::_on_binding_confirmed` mutator + `is_pubkey_in_binding_confirmed`
  accessor.**  Snapshots `authorized_consumer_pubkeys` into
  `binding_side_confirmed_allowlist`.  Read-only accessor for the
  readiness handler.  `ChannelAccessEntry` gains one new
  `unordered_set<string>` field; existing per-channel state is
  unchanged.

Design authority: HEP-CORE-0036 §6.5 + §6.6.1 + §6.6.2 + §6.6.3
+ HEP-CORE-0042 §5.5.2 amendment.

---

## Open broker-specific items

### #92 (HIGH-leverage) — Audit all `_REQ` frames against HEP-0007 §12.2.1

The REQ shape contract (Sync vs Fire-and-forget) was codified in
HEP-CORE-0007 §12.2.1 + HEP-CORE-0021 §16.3 on 2026-05-21 after
finding ENDPOINT_UPDATE_REQ was in the prohibited half-mix shape
(broker emitted `_ACK`, client dropped it).  The fix shipped as
commits `5ccae1b2` (HEP) + `8228f1ac` (code) + `66e71894` (error-
path tests).

Now scan every other `_REQ` frame in `BrokerRequestComm` (header) +
`BrokerService` (broker side) and classify each.  Known suspects
where the client method is fire-and-forget `void` and we should
check whether the broker also sends an ACK that's being dropped:

- `send_broadcast` (`broker_request_comm.cpp`)
- `send_checksum_error`
- `send_heartbeat`

For each half-mix found:
1. Decide intended shape per HEP-0007 §12.2.1 (does caller's next
   decision depend on broker acceptance?).
2. Align BRC + broker: either make BRC sync (`do_request`) OR
   remove broker's `_ACK`.
3. Update the relevant HEP to declare the shape (don't leave it
   implicit).

Trigger: any half-mix is a latent flake source like the
ENDPOINT_UPDATE one we just fixed.  Estimated effort M.

### H43 — Federation propagation of role-disconnect

Verified open 2026-05-12: `broker_service.cpp` does not call
`subscribe_role_disconnected`; only `hub_script_runner.cpp:281`
subscribes (for script-side `on_role_disconnected` callback).
Peer hubs therefore do NOT learn about non-channel-close role
disconnects.  Whether they NEED to is HEP-CORE-0022 federation
scope, not Wave-B M8 (dual-hub presence is role-side, not peer-hub
state replication).  Trigger to address: a concrete federation
scenario where peer-hub bookkeeping diverges.  Bands are not
federated per HEP-CORE-0030 §3 so impact is bounded.

### Wave-M2 / Wave-M3 deferred sub-items (broker-internal)

All trigger-gated — not actively blocking.  Re-open when the
trigger fires.

- **Wave-M3 Step 5** — strict `add_role` admission with global-uid
  uniqueness.  Trigger: spoofing-attempt observation OR security-
  design pass requirement.
- **Wave-M3 Step 7** — privatize `RolePresence` state-bearing
  fields.  Trigger: concrete misuse bug OR audit observation.
- **H15** — `_on_heartbeat` direct metrics-field mutation
  (`src/utils/ipc/hub_state.cpp:1294-1306`).  Trigger:
  `RoleEntry::set_presence_metrics(...)` API addition for a
  concrete reason.
- **H40** — `active_router_` concurrency hardening (atomic + DEBUG
  assertion).  Trigger: any new HubState mutator caller from a
  non-broker-IO thread.

### Wave M2 — Multi-Producer Channel Bookkeeping

API-layer MP detail in `docs/todo/API_TODO.md` § "Wave M2 — Multi-
Producer Channel Bookkeeping (open MP4 work)".  Broker-side
remaining items mirror those — same handler list, broker-internal
mutation only.

### Open 2026-05-03: `IncomingMessage` `sender` field semantics

`HubScriptRunner::worker_main_()` (Phase 7 Commit C) reuses
`scripting::IncomingMessage` from `role_host_core.hpp` as the
cross-thread queue payload between broker subscriptions and the
script-thread runner.  Hub-side semantics differ from role-side:
the role-side `sender` is the broker peer; hub-side it should be
the originating role's uid (per HEP-0033 §12).  Audit field-use
sites + document the semantic split, OR introduce a sibling type.

### Hub State Query Layer (HEP-CORE-0039)

Full design: `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`
(promoted from tech_draft 2026-06-02; tasks #140-#150 shipped Phase A
+ P8 sweep migration).  Layer-1 metadata + Layer-2 free-function
query API + `hub.snapshot()` script binding.  Absorbs `query_shm_info`
/ `count_by_observable` wiring (Group 2 #2/#3 from 2026-05-20
decision log).  Phases B+ remain open.

---

## Naming hygiene (mirror TODO_MASTER, for quick disambiguation)

| Looks like | Actually means | Status |
|---|---|---|
| Wave-B MN | Arc B Wave (`docs/archive/transient-2026-06-02/role_host_template_design.md` §14) | M0..M9 shipped |
| HEP-0033 §15 Phase N | Arc A Wave | Phases 1-9 shipped; Phase 10 partial |
| Wave-M2 / Wave-M2.5 / Wave-M3 | Side-arc waves (multi-producer / controlled-access) | closed |
| M1.2 / M1.4 / M1.5 / MD1 / MD1.5 | Side-arc FSM-consolidation + race-fix cleanups | closed |

If a sentence says "M3" without a prefix, it almost certainly means
**Wave-B M3** — but verify against context.

---

## Open items from 2026-05-20 post-band-authority discovery (migrated 2026-06-02)

Carried over from the archived
`docs/archive/transient-2026-06-02/DISCOVERY_2026-05-20.md`.  Each
should be verified-fixed-or-still-open before next sprint.

### D2 drift

- **B1 — empty `correlation_id` in BAND_JOIN/LEAVE validator errors.**
  ✅ FIXED — verified 2026-06-27 against current code.  Both handlers extract
  `corr_id = req.value("correlation_id", "")` at entry (`broker_service.cpp:5540`
  and `:5651`) and pass it through to `validate_role_uid_only(...)` (`:5577-5580`
  and `:5678-5681`).  Audit B1 anchors at `:5573-5576` and `:5676-5677` record
  the fix.  Earlier line references (`:4488,4584`) are stale post-refactor.
- **Stale-comment scrub (~15 sites).**  References to deleted
  `set_broker_comm` / `start_ctrl_thread` / `pImpl->broker_channel`
  in `role_api_base.{hpp,cpp}`, `hub_script_runner.cpp`, the 3 role
  hosts, `engine_host.hpp`, `role_host_helpers.hpp`, `role_host_core.hpp`.
- **Obsolete includes:** `#include "utils/broker_request_comm.hpp"`
  in three role hosts (~line 22) — BRC reaches them via
  `role_handler.hpp` now.  Mechanical.

### Pending discussion (dead-code candidates)

- `RoleAPIBase::close_all_inbox_clients()` — zero callers; verify
  no script reflection path before removing.
- `BrokerRequestComm::query_shm_info()` + `BrokerService::collect_shm_info_json()`
  + `SHM_INFO_REQ` wire frame — paired client+server; remove together
  if wire frame is also dead.
- `ChannelSnapshot::count_by_observable()` — verify exposure to
  admin queries before removing.

### Doc bookkeeping debt

- 4 `docs/code_review/REVIEW_*WaveM3*.md` files from 2026-05-11 are
  archive candidates (open items verified resolved during the
  2026-05-19 review-triage pass).

---

## Notes

### Error taxonomy (Cat 1 / Cat 2)

Broker / producer / consumer follow the two-category error
taxonomy in `docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy".
- **Cat 1** — recoverable; log + continue (e.g. transient frame
  decode error).
- **Cat 2** — fatal; emit notification, fail closed (e.g. schema
  mismatch on registration).

### Key design decisions (single source of truth)

- Per-presence FSM on `RoleEntry` (Connected / Pending /
  Disconnected) replaced the Pending / Ready / Closing channel
  FSM in HEP-CORE-0023 §2 rewrite (2026-05-07).  Channel teardown
  is **atomic** on producer-presence Disconnected — no separate
  channel-grace window, no `FORCE_SHUTDOWN` escalation.
- Wire-field unification (broker_proto 5, 2026-05-19): every role-
  context message uses `role_uid`/`role_name`; federation peer-
  context uses `sender_uid`; inbox uses `sender_uid` (authoring
  producer).  See HEP-CORE-0023 §2.5.4 + HEP-CORE-0033 §G2.2.0b.
- Side-aware role-tag policy at every REG/DEREG/HEARTBEAT gate:
  REG/DEREG accept `{prod, proc}`; CONSUMER_REG/DEREG accept
  `{cons, proc}` (processor dual-role).  `HEARTBEAT_REQ` cross-
  checks `role_type` against tag.

---

## Related Work

- `docs/todo/API_TODO.md` — API-layer view of the same renovation.
- `docs/todo/TESTING_TODO.md` — broker-protocol test gaps.
- `docs/HEP/HEP-CORE-0033-Hub-Character.md` — Arc A canonical spec.
- `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` — per-presence
  FSM canonical spec.
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — auth design.
- `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md` — hub state
  query layer design (promoted from tech_draft 2026-06-02).
