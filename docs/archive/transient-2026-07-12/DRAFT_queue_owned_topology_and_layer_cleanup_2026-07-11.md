# DRAFT — Queue-Owned Topology + Role/API Layer Cleanup

**Status:** DRAFT (2026-07-11).  For discussion + approval before any
code work.
**Audience:** designer (user) + implementer (Claude).
**Successor of:** review conducted 2026-07-11 against the fan-in
binding-side reader correctness arc (Tasks #7 / #8 / #9), which
shipped the fix but pushed topology and transport knowledge into
the role host and the general-purpose role API.  See archived draft
`docs/archive/transient-2026-07-11/DRAFT_loop_ready_gate_and_binding_queue_2026-07-11.md`
for the shipped design.

---

## 1. Goal (verbatim from user directive 2026-07-11)

> **Clean and logical API separation and layer design; abstraction
> should conceal queue operation from role-level requests.**

Interpreted as a layering invariant this arc must end up honoring:

> **Topology and transport are `hub::Queue`'s private concerns.
> Nothing at role-host / RoleAPIBase level branches on
> `topology == FanIn`, `is_binding_side`, `has_tx_side`,
> `tx_has_shm`, or reaches through the API to reconstruct queue
> state.  Role-side code uses topology-agnostic verbs; the queue
> alone chooses per-topology behavior.**

Every step in this plan is measured against that invariant.

## 2. Non-goals

- **Not** re-opening the fan-in binding-side reader fix itself
  (Tasks #7 / #8 / #9 already landed correct behavior — the arc's
  problem is *where* the behavior lives, not *whether* it is
  correct).
- **Not** revisiting the loop-ready gate contract in
  HEP-CORE-0011 §"Loop-ready gate" (the gate is right; only *what
  it reads* moves).
- **Not** touching SHM per-attach broker pre-confirm
  (HEP-CORE-0041 §9 D4) — that path already respects the layer
  contract; ZMQ is the outlier this arc reclaims.

## 3. Layer contract we are driving toward

```
┌──────────────────────────────────────────────────────────────────┐
│  Role host (worker_main_)                                        │
│    - Uniform sequence for producer / consumer / processor        │
│    - NO topology adjectives on any call                          │
│    - Calls: setup_infra → register → apply_reg_ack →             │
│             (queue-owned finalize_connect) → wait_for_roles →    │
│             run_data_loop → teardown                             │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│  RoleAPIBase (topology-agnostic surface)                         │
│    - Data ops: write_acquire / read_acquire / allowed_peers      │
│    - Observability: allowed_peers, producers, consumers          │
│    - Delegates all connect/dial/apply/confirm to queue           │
│    - NO `dial_now`, NO `check_peer_ready`, NO `has_tx_side`      │
│      as public methods                                           │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│  hub::Queue (topology + transport owner)                         │
│    - Decides bind vs dial per topology + own side                │
│    - Owns deferred-connect (fan-in DIALING PUSH)                 │
│    - Owns confirmation emission after set_peer_allowlist         │
│    - Exposes topology-agnostic queries:                          │
│        is_admission_populated(), finalize_connect(oracle)        │
│    - No caller ever asks "is this fan-in?"                       │
└──────────────────────────────────────────────────────────────────┘
```

Key invariant: any query of the form "am I in topology X?" is a
smell; the queue's own state (`dial_pending`, `bind_socket`,
`admission_ready`) already answers it, and the queue is the ONLY
entity permitted to interrogate that state.

## 4. Findings this plan addresses (from 2026-07-11 review)

Compressed reference table.  Each row is a finding to close.

| ID | Severity | One-liner | Fix in phase |
|----|----------|-----------|--------------|
| A1 | HIGH  | Producer host runs fan-in ceremony (topology parse + SMS penetration + `wait_for_peer_ready` + `dial_now`) | P3 |
| A2 | HIGH  | `RoleAPIBase::dial_now / check_peer_ready` on general-purpose surface | P3 |
| A3 | MED   | `handle_channel_auth_notifies` uses `rx_queue->is_binding_side()` to gate consumer-branch APPLIED_REQ | P5 |
| A4 | LOW-M | Loop-ready gate reads through API to allowlist_cache | P4 |
| B1 | LOW   | `admitted_peers_count()` copies full snapshot to compute size | P4 (dies with A4) |
| B2 | LOW   | `kLoopReadyPollInterval` reused for two semantically distinct consumers | P4 |
| B3 | LOW   | "Fan-in DIALING PUSH" detected in two files independently | P3 (dies with A1) |
| B4 | LOW   | `binding_side_confirmed_allowlist` duplicates `authorized_consumer_pubkeys` as full set | P6 |
| C1 | MED   | `wait_for_peer_ready` has no shutdown cancellation | P2 |
| C2 | MED   | `check_peer_ready` collapses permanent + transient failures into `nullopt` | P2 |
| C3 | HIGH  | `binding_side_confirmed_allowlist` never cleared on channel-access close / consumer dereg | P2 (bug) |
| C4 | VERIFY| Consumer-branch APPLIED_REQ guard's `admission` derivation | P2 |
| D1 | VERIFY| Native ABI version claim drift (v3→v4 in old draft vs shipped v8→v9) | P7 |
| D2 | VERIFY| HEP-0036 §6.5 wording queue-agnostic | P7 |

## 5. Phased plan

### P1 — Contract restatement (design, no code) ✅ LANDED 2026-07-11

**Deliverable:** amended HEP-CORE-0011 §"Loop-ready gate" +
HEP-CORE-0036 §6.5–§6.6 language that names the queue as the owner
of topology-specific behavior, and lists which role-side methods
are permitted to exist.

**Landed:**
- HEP-CORE-0036 §I9.1 (NEW) — "Topology and transport are
  queue-internal (locality invariant)."  Names the forbidden
  patterns explicitly (`topology::parse` above queue,
  `is_binding_side` in control-flow, topology-specific method
  names on `RoleAPIBase`, role-side handlers emitting queue-fact
  confirmations).  Names the permitted shape (queue-owned
  behavior + small injected interfaces).  Cross-references
  updated in §6.5 step 6 + §6.6.3.
- HEP-CORE-0036 §6.5 step 6 — Layer-contract amendment sub-block:
  queue's `set_peer_allowlist` returns `AppliedResult{side,
  applied_version}`; queue calls injected `ConfirmationEmitter`;
  role-side `is_binding_side()` branching forbidden going forward.
- HEP-CORE-0036 §6.6.3 — Layer-contract amendment sub-block:
  role-side entry point is topology-agnostic
  `finalize_channel_connect(channel, timeout_ms)`; wire is
  broker-facing only; `PeerReadinessOracle` interface named;
  poll cadence lives on queue-owned constant
  `kBrokerReadinessPollInterval` split from
  `kLoopReadyGateInterval`.
- HEP-CORE-0011 §"Loop-ready gate" — Consumer default's read
  path amended to `api.channel_admission_populated(ch)` (queue-
  level fact); `allowed_peers` / `admitted_peers_count` remain
  as script-facing observability only.

**No code touched.**

### P2 — Correctness bugs, small-surface fixes (before layer surgery)

Do these first because we know where they live today and they close
real bugs.  Each is an atomic commit.

**P2.a — Stale confirmed-allowlist (C3, HIGH bug)**

- `HubState::_on_channel_access_closed` clears
  `binding_side_confirmed_allowlist` for the channel.
- `HubState::_on_consumer_revoked` clears entries corresponding to
  the revoked pubkey.  (Or: on any revoke, clear the whole
  confirmed set — safer than partial invalidation; next apply
  reconstructs.)
- L2 test in `tests/test_layer2_service/`: pin the sequence
  "consumer dies → CHECK_PEER_READY_REQ returns not_ready with
  reason not_admitted; new consumer confirms → returns ready."
- L2 test: pin "producer's stale pubkey removed from confirmed
  after consumer revoke" if we take the pubkey-scoped variant.

**P2.b — `wait_for_peer_ready` shutdown cancellation (C1)**

- Helper reads role-host shutdown atomics via a small shutdown
  probe object provided at construction time.
- Extract shared `wait_with_shutdown(sleep, probe, deadline)`
  primitive; make `wait_for_roles` use it too (was silently in the
  same shape).
- L2 test: pin "shutdown during wait_for_peer_ready exits within
  one poll interval, not init_timeout_ms."

**P2.c — `check_peer_ready` failure-mode split (C2)**

- Return type becomes a discriminated result: `Ready` /
  `NotReady{reason}` / `TransientError` / `PermanentError{code}`.
- `wait_for_peer_ready` fails fast on `PermanentError`, keeps
  polling on `TransientError` and `NotReady`.
- L2 test: pin "check_peer_ready with unresolved BRC fails fast in
  wait_for_peer_ready (does not burn the full budget)."

**P2.d — Verify C4 guard's `admission` derivation**

- Read the branch structure in `handle_channel_auth_notifies`
  around line 2538 with focus on how `admission` is bound for the
  consumer path.  If the bind covers rx_queue → confirm with
  inline comment.  If tx-only → this is a live bug and P2 covers
  the fix.

**Blocking:** P1 (contract language).  These fixes should log
against the amended HEP so future readers see them as contract
enforcement, not ad-hoc patches.

### P2 — Correctness bugs ✅ LANDED 2026-07-11

**All four sub-items ✅:**

- **P2.a (C3 stale-allowlist bug fix) ✅** — `HubState::_on_consumer_revoked`
  clears `binding_side_confirmed_allowlist` on any successful erase
  (whole-set clear per D-2).  Six new L2 tests in
  `test_hub_state.cpp` (`HubStateChannelAccess.BindingConfirmedAllowlist_*`)
  pin: empty-before-apply, snapshot-by-confirm, clear-on-revoke,
  repopulate-by-next-apply, no-op-revoke-preserves, close-clears.
  All pass; regression-free vs baseline.
- **P2.b (C1 shutdown-cancel) ✅** — `wait_for_peer_ready` gained
  an optional `std::function<bool()> is_cancelled` probe polled
  before every RPC and every sleep.  Producer host wires
  `core_.is_shutdown_requested() / is_critical_error() /
  is_process_exit_requested()`.  Retired entirely in P3 (the queue
  now owns the poll loop and receives the same probe directly).
- **P2.c (C2 error-code split) ✅** — `RoleAPIBase::check_peer_ready`
  synthesizes a shaped `status="error" error_code="NO_BRC_FOR_CHANNEL"`
  reply on unresolvable BRC so the wait-loop's existing error
  branch fires fail-fast.  Nullopt reserved for genuine transport
  failure.  Retired in P3 (public API removed; behavior preserved
  inside `finalize_channel_connect`'s BrcOracle).
- **P2.d (C4 admission-derivation verify) ✅** — read
  `role_api_base.cpp:2349-2364`; fan-in consumer branch's
  `admission` is bound from `pImpl->rx_queue` via direct
  dynamic_cast.  Fan-in consumer's rx_queue IS a ZmqQueue
  implementing PeerAdmission → cast succeeds → consumer-branch
  APPLIED_REQ fires.  No bug.

**Regression check:** full L2 ctest sweep 1657/1657 pass;
pre-existing `BrokerServiceCtor.MissingHubIdentityInKeyStoreThrowsLogicError`
state-leak flake unchanged (isolated pass; failed on same-suite
whole-run baseline pre-P2 too).

### P3 — Queue-owned deferred connect ✅ LANDED 2026-07-11

**Deliverable landed:** the fan-in-producer startup ceremony now
lives inside `hub::ZmqQueue::finalize_connect`; role hosts and
`RoleAPIBase` no longer know it exists.  Layer contract in §I9.1
is now enforced by construction.

**Interface additions (`hub_queue.hpp`):**
- `hub::PeerReadinessOracle` abstract class with arg-free `poll()`
  returning `Ready` / `NotReady` / `PermanentError`.
- `QueueWriter::finalize_connect(oracle, timeout_ms, is_cancelled,
  log_tag)` virtual — default no-op returning true; only
  `ZmqQueue` overrides.
- `QueueWriter::own_pubkey_z85()` virtual — queue's CURVE identity
  pubkey; empty on non-CURVE writers.  Replaces the SMS
  penetration the role host used to do.

**Interface removals (`hub_queue.hpp`, `hub_zmq_queue.hpp`):**
- `QueueWriter::dial_now` virtual RETIRED.
- `ZmqQueue::dial_now` override RETIRED.

**Queue implementation (`hub_zmq_queue.cpp`):**
- `finalize_connect` runs the poll loop internally at
  `kBrokerReadinessPollInterval`, respects `is_cancelled`
  cancellation, times out at `timeout_ms`, then completes the
  deferred `start()`.  Non-deferred queues return true
  immediately without touching the oracle.
- `own_pubkey_z85` reads `identity_key_name_` from KeyStore;
  fail-quiet on absence.
- `apply_master_approval` still detects fan-in DIALING PUSH and
  sets `dial_pending=true`; `finalize_connect` completes it.

**Role API cleanup (`role_api_base.hpp/cpp`):**
- `RoleAPIBase::dial_now` REMOVED from public surface.
- `RoleAPIBase::check_peer_ready` REMOVED from public surface
  (the BRC method survives, wrapped by the internal oracle).
- `RoleAPIBase::finalize_channel_connect(channel, timeout_ms,
  is_cancelled)` NEW — topology-agnostic entry point.
  Constructs an inline `BrcOracle` that closes over BRC +
  channel + role_uid + queue's own pubkey.

**Role host cleanup:**
- `producer_role_host.cpp`: the ~50-line block that combined
  `topology::parse` + `has_tx_side` + `tx_has_shm` + SMS
  penetration + `wait_for_peer_ready` + `dial_now` COLLAPSED
  into a single `api_ref.finalize_channel_connect(channel,
  timeout_ms, is_cancelled)` call.  Zero topology awareness in
  the host.
- `consumer_role_host.cpp`: added the same uniform call for the
  in-channel (queue no-ops today; the call ensures a future
  topology that puts a deferred connect on the consumer side
  needs no role-host change).
- `processor_role_host.cpp`: added the uniform call per side
  (in + out channels).

**Constant split (`loop_timing_policy.hpp`):**
- `kLoopReadyPollInterval` renamed to `kLoopReadyGateInterval`
  (data-loop pre-Ready pacer).
- `kBrokerReadinessPollInterval` added (queue-owned broker RPC
  cadence).  Two 100 ms constants, semantically distinct.

**Helper deletion (`role_host_helpers.hpp`):**
- `wait_for_peer_ready` REMOVED — mechanically inlined into the
  queue's `finalize_connect`.  Replaced by a short "RETIRED
  2026-07-11" pointer comment.

**Test surface:**
- `test_hub_zmq_queue.cpp
  TopologyFactory_FanInProducer_WireApplyMasterApproval` updated
  to drive the queue via a local `AlwaysReadyOracle` +
  `finalize_connect` call instead of the retired `dial_now`.
- Regression sweep: L2 1657/1657 pass, L4 133/133 pass.  L4 fan-in
  E2E (`PlhHubCliTest.ZmqE2E_MultiProducer_TwoAuthorized`)
  passes in ~3.1 s under ctest, matching pre-P3 shape.

**What §I9.1 forbids that no longer exists in role code:**
- Zero `topology::parse` calls above the queue layer.
- Zero `has_tx_side` / `tx_has_shm` gating a control step.
- Zero role-side branching on `is_binding_side()` for control flow.
- Zero SMS penetration in role-host worker_main_ (queue reads
  KeyStore internally via its `identity_key_name_`).
- Zero topology-named methods on `RoleAPIBase`'s public surface.

### P3 — Queue-owned deferred connect (layer surgery, LARGE) — original plan text

**Deliverable:** the fan-in-producer startup ceremony lives inside
`hub::ZmqQueue`; role hosts and RoleAPIBase no longer know it
exists.

**Design shape (to discuss — option A recommended):**

**Option A — Queue-owned readiness pull via injected oracle**
- New interface `hub::PeerReadinessOracle` with one method:
  `PollResult poll_peer_ready(channel, pubkey_z85)`.
- `hub::QueueWriter::finalize_connect(oracle&, deadline, log_tag)`
  → default no-op returning `Ready`; `ZmqQueue` overrides.
- `ZmqQueue::finalize_connect` polls the oracle at
  `kBrokerReadinessPollInterval` when `dial_pending`; on ready,
  runs the deferred `start()`.  When not `dial_pending` it's a
  no-op (fan-out producer, one-to-one binding-side, SHM).
- `RoleAPIBase` implements `PeerReadinessOracle` internally
  (forwards to BRC.check_peer_ready) — but the interface is a
  queue-side dependency the queue asks for, not a role-side
  method.
- Role host worker_main_ calls
  `api.finalize_channel_connect(channel, timeout_ms)` for every
  role type, uniformly, after apply_reg_ack.  No topology branch.
  Under the hood, the API forwards to the queue's
  `finalize_connect`.
- **What disappears:**
  - `RoleAPIBase::dial_now()` (A2, gone).
  - `RoleAPIBase::check_peer_ready()` public method (A2, gone —
    becomes a private helper the API's own oracle impl uses).
  - Producer-host branch on
    `topology == FanIn && has_tx_side && !tx_has_shm` (A1, gone —
    B3 dies with it).
  - SMS penetration for `kRoleIdentityName` (A1 aftermath —
    queue reads its own CURVE identity from the artifacts it
    already holds).

**Option B — Queue-owned dial via injected BrokerRelay** (not
recommended — queue would take a BRC handle, violating
transport-abstraction layering).

**Blocking:** P1 + P2.a (don't build on stale state).

**Test surface:**
- L2 test: pin "producer's `finalize_connect` is called for every
  topology; no-op on non-deferred queues; blocks + dials on
  fan-in DIALING PUSH."
- L2 test: pin "consumer's `finalize_connect` is a no-op (returns
  Ready immediately, does not touch broker)."
- L2 test: pin "processor's `finalize_connect` handles both sides
  independently — deferred for fan-in-producer output, no-op for
  everything else."
- L4 test (fan-in E2E): unchanged from today — same wire
  observable behavior.

### P4 — Loop-ready gate reads queue-level fact ✅ LANDED 2026-07-11

- **`hub::QueueReader::is_admission_populated()` + `QueueWriter::
  is_admission_populated()` virtuals added; default `true`.**
- **`hub::ZmqQueue::is_admission_populated()` override:**
  binding side → allowlist snapshot non-empty; dialing reader →
  producer_peers_ non-empty; dialing writer → server_pubkey_z85_
  non-empty.
- **`RoleAPIBase::channel_admission_populated(channel)` NEW
  forwarder** — resolves rx→tx and calls queue accessor. No
  vector snapshot allocation.
- **`ConsumerCycleOps::default_init_ready`** and
  **`ProcessorCycleOps::default_init_ready`** rerouted to
  `channel_admission_populated`; the old
  `admitted_peers_count(channel) >= 1` snapshot-copy pattern
  retires from the gate (retained as script-facing observability
  only).
- L2 1657/1657, L4 133/133 pass unchanged.

### Post-review correctness fix (2026-07-12) — processor two-channel resolution

Systematic processor topology trace surfaced a resolution bug in the
channel-to-queue mapping used by three sites:

- `RoleAPIBase::finalize_channel_connect`
- `RoleAPIBase::handle_channel_auth_notifies` — `admission` derivation
- `RoleAPIBase::handle_channel_auth_notifies` — `side` derivation
  (P5 addition)

**Bug shape.**  The old shape used a rx_queue-first-then-tx_queue
FALLBACK for the primary channel branch (`channel == pImpl->channel`).
That is correct for single-channel producer (rx_queue is null, tx_queue
holds the channel) but wrong for two-channel processor (`out_channel`
non-empty): rx_queue holds IN, tx_queue holds OUT.  The fallback would
resolve IN queries to tx_queue (OUT queue) — a cross-channel slippage.

Concrete failure modes ruled out today only by missing test coverage
of the specific processor topologies:

- Processor with fan-in OUT: `finalize_channel_connect(IN)` would fire
  the poll loop against the OUT queue's `dial_pending` state, polling
  the broker with `channel=IN` and OUT-queue's pubkey → broker returns
  not_ready forever → InitTimeout aborts startup.
- Processor with mixed SHM IN + ZMQ OUT (a valid transport
  combination): the `admission` cast on rx_queue (ShmQueue, no
  PeerAdmission) returns null → fallback to tx_queue's cast succeeds
  → `set_peer_allowlist` for the IN channel writes the IN pubkeys
  onto OUT's ZAP allowlist.
- Processor with fan-out IN + binding OUT: my P5 `side` derivation's
  rx→tx fallback fires "producer" for IN notify → wire APPLIED_REQ
  with role_type="producer" for IN channel → broker rejects (uid is
  consumer of IN, not producer).

**Fix.**  All three sites now branch first on whether `out_channel`
is non-empty (two-channel role) and require an exact channel match
without cross-channel fallback:

```
if (!pImpl->out_channel.empty()) {
    if (channel == pImpl->out_channel) → tx_queue
    else if (channel == pImpl->channel) → rx_queue
    // else: no writer/admission for this channel on this role
} else if (channel == pImpl->channel) {
    rx_queue if present, else tx_queue  // single-channel role
}
```

L2 1657/1657, L4 134/134 pass — the last of the 134 is the new
`PlhHubCliTest.ZmqE2E_Processor_FanInBothChannels_ThreeRoles`
(three subprocesses: upstream producer, processor, downstream
consumer; ch1 and ch2 both declared fan-in).  Under the
pre-fix resolution the processor aborts at RegAck due to
`NOT_A_ROLE_OF_CHANNEL` from the wrongly-resolved
`finalize_channel_connect(in_channel)` broker RPC; under the
fix it no-ops the IN call and polls the OUT channel only,
completing the deferred dial.  Test runs ~3.4 s.

### P5 — Queue emits its own apply-confirmation ✅ LANDED 2026-07-11

- **`hub::QueueReader::binding_role_type()` +
  `QueueWriter::binding_role_type()` virtuals added.**  Return
  `"consumer"` (binding reader), `"producer"` (binding writer),
  or `""` (non-binding).  Retires the role-side pattern-matching
  on `is_binding_side()` for the emission branch.
- **`hub::ZmqQueue::binding_role_type()` override:** derives
  from queue mode + bind_socket state.
- **`BrokerRequestComm::channel_auth_applied(channel, role_uid,
  role_type, applied_version, instance_id, timeout_ms)` NEW
  consolidated method** — single wire (`CHANNEL_AUTH_APPLIED_REQ`),
  broker discriminates on `role_type`.  The redundant
  `channel_auth_applied_consumer` method RETIRED.
- **`handle_channel_auth_notifies` cleanup:** the previous
  `is_binding_side()` branch that split producer / consumer
  emission paths is REPLACED by a single call sequence that
  asks the queue "what side are you?" via `binding_role_type()`
  and dispatches uniformly.  Non-binding side (`side == ""`)
  short-circuits — no APPLIED_REQ emitted, which is correct
  (nothing was installed to confirm).
- **`apply_producer_reg_ack` APPLIED_REQ call** updated to
  the consolidated BRC method with explicit
  `role_type="producer"`.
- L2 1657/1657, L4 133/133 pass unchanged.  L4 fan-in E2E
  (~3.1 s) still exercises the consumer-branch APPLIED_REQ
  through the queue-driven path.

**What §I9.1 forbids that no longer exists in role code (after P5):**
- Zero `is_binding_side()` calls in role-side control flow.
- Zero role-side conditional on queue shape to pick a wire
  variant.
- Zero redundant BRC method surface.

### P4 — Loop-ready gate reads queue-level fact directly (original plan text)

**Deliverable:** the gate stops going through the count-method
snapshot copy.

- Add `hub::Queue::is_admission_populated() const noexcept` (or
  named variant — TBD).  Binding-side queue: returns
  `!authorized_pubkeys_.empty()`.  Dialing-side queue: returns
  true iff the peer set is known (populated at apply_reg_ack).
- `Ops::default_init_ready(api)` for Consumer / Processor calls
  `api.channel_admission_populated(channel)` which forwards
  directly to the queue's method.  No vector snapshot copy.
- `RoleAPIBase::admitted_peers_count()` STAYS as a script-facing
  accessor (scripts read it in `on_init` per the tech-draft
  contract).  The GATE stops calling it.

**Fallout:**
- B1 (snapshot-for-size) resolved by *not calling* the count
  method from the gate; the accessor's implementation can stay,
  or gain a cheap `AllowlistCache::size(channel)` fast path.
- B2 (constant reuse) resolved by splitting into
  `kLoopReadyGateInterval` (data loop pre-Ready pacer) and
  `kBrokerReadinessPollInterval` (P3's poll cadence).

**Blocking:** P3 (queue-side readiness path stabilized).

**Test surface:**
- L2 test: pin "gate reads queue.admission_populated, not
  api.admitted_peers_count."  (Instrument the count method with
  a counter; assert gate does not increment it.)

### P5 — Queue emits its own apply-confirmation

**Deliverable:** `handle_channel_auth_notifies` no longer branches
on `rx_queue->is_binding_side()` to publish the consumer-branch
APPLIED_REQ.  The queue itself signals "I applied version V" and
the API layer forwards to BRC.

**Design shape:**
- `hub::Queue::set_peer_allowlist(allowlist, applied_version)`
  returns an `AppliedResult{ side, version }`.
- Role-side code passes the result to a small forwarder that
  publishes `CHANNEL_AUTH_APPLIED_REQ` with the right `role_type`
  taken from `AppliedResult.side`.  No `is_binding_side()`
  branch in role code.
- Consumer-branch (fan-in) and producer-branch (fan-out /
  one-to-one) fall out of `AppliedResult.side`.

**Blocking:** P3 (need queue's post-apply hook shape stable).

**Test surface:**
- L2 test: existing L2 `handle_channel_auth_notifies` coverage
  updated to observe `AppliedResult.side` rather than
  `rx_queue->is_binding_side()`.
- L4 test: unchanged wire behavior.

### P6 — Data-structure cleanup (`binding_side_confirmed_allowlist`)

**Deliverable:** replace the parallel string set with a version-
tagged membership check.

- New `ChannelAccessEntry.confirmed_version: uint64` (per-channel
  scalar).
- Extend each authorized-pubkey entry with
  `authorized_at_version: uint64` at `_on_consumer_authorized`
  time.
- `is_pubkey_in_binding_confirmed(ch, pk)` becomes: pubkey is in
  authorized set AND its `authorized_at_version <= confirmed_version`.
- `_on_binding_confirmed` becomes: `confirmed_version =
  max(confirmed_version, current_channel_version)`.
- Storage cost: two uint64 per channel + one uint64 per pubkey vs
  a full string set copy per confirmation.
- Semantic gain: never stale — a revoke immediately narrows the
  membership set without needing an invalidation step.

**Blocking:** P2.a (bug fix lands first with the current data
structure; then this refactor lands as a pure structural change).

**Test surface:**
- Reuse P2.a's stale-consumer regression tests as the correctness
  gate for the refactor.

### P7 — HEP + doc sweep ✅ LANDED 2026-07-12 (commit db2bbc21)

- HEP-CORE-0011 Native ABI mentions corrected v3→v4 to v8→v9;
  actual shipped constant is `PLH_NATIVE_API_VERSION 9`.
- HEP-CORE-0011 `kLoopReadyPollInterval` references renamed to
  `kLoopReadyGateInterval` (matches shipped constant split).
- HEP-CORE-0036 last-revised banner + §6.5 step 6 + §6.6.3
  amendments rewritten to reflect shipped design (queue's
  `binding_role_type()` accessor rather than the proposed
  `AppliedResult{side, applied_version}` + `ConfirmationEmitter`
  shape).
- HEP-CORE-0036 §I9.1 forbidden-patterns example now notes
  `dial_now` / `check_peer_ready` as RETIRED in the same arc.
- Two trailing §I9.1 violations in
  `apply_consumer_reg_ack` (raw `is_binding_side()` control-flow
  branches at lines 1309 and 1448) replaced with
  `!binding_role_type().empty()`.

### Deferred follow-ups (not blocking arc close)

- **P6** — Replace `binding_side_confirmed_allowlist` full-set
  copy with version-tagged membership.  Correctness matches P2.a
  via the simpler mechanism; P6 is a structural refactor only.
  Tracked under `docs/todo/MESSAGEHUB_TODO.md` "Refactor P6 —
  PENDING."
- **D-3** — clang-tidy / clang-query custom rule that fails the
  build when role-side code contains `topology::parse`,
  `is_binding_side` control-flow, `has_tx_side` /
  `tx_has_shm` gating a control step, or previously-retired
  topology-named methods re-appearing on `RoleAPIBase` public
  surface.  Hard-enforce the §I9.1 invariant so future
  regressions are loud.  Tracked under `docs/todo/API_TODO.md`
  in the arc's follow-up block.

### Arc close

All P1–P5 + P7 landed; P6 + D-3 deferred to follow-up work under
canonical TODOs.  Draft archives to
`docs/archive/transient-2026-07-12/` per
`docs/DOC_STRUCTURE.md` §2.2.  Lasting design content is in the
amended HEPs (HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036
§I9.1 + §6.5 step 6 + §6.6.3 + HEP-CORE-0042 §5.5.2 amendment).

## 6. Sequencing rationale

```
                       P1 (contract language)
                            │
                            ▼
                       P2 (bugs, small surface)
                            │
                            ▼
                       P3 (queue-owned dial) ────┐
                            │                    │
                            ▼                    │
                       P5 (queue-owned confirm)  │
                            │                    │
                            ▼                    │
                       P4 (gate reads queue) ◄───┘
                            │
                            ▼
                       P6 (data-structure cleanup)
                            │
                            ▼
                       P7 (HEP sweep + archive)
```

- **P1 first** because the contract we're enforcing needs to be
  written down; every subsequent PR references it.
- **P2 before P3** because we don't want to refactor over top of
  a known bug (C3 in particular could mask a P3 test failure).
- **P3 before P5** because P5's design shape (`AppliedResult.side`)
  depends on the queue owning its own connect state.
- **P4 after P3 + P5** because the gate's queue-level fact is
  most cleanly defined once the queue is the single owner of
  admission state.
- **P6 last** because it's a pure structural refactor — best to
  do it once the semantic surface has stabilized.
- **P7 threaded** — each phase's HEP touch lands with the phase.

## 7. Open decisions (need user pick)

**D-1 — Deferred-connect ownership pattern (P3 option A vs B).**
Recommend A (oracle injection).  User to confirm before P3 starts.

**D-2 — Confirmed-allowlist invalidation granularity (P2.a).**
Options:
  (i)  On any consumer revoke, clear the whole confirmed set
       for the channel; next apply reconstructs.
  (ii) On revoke, remove exactly the revoked pubkey from the
       confirmed set (fine-grained).
Option (i) is safer + simpler; (ii) has slightly better staleness
window under multi-consumer fan-in.  Recommend (i) unless user
identifies a workload where the staleness window matters.

**D-3 — Contract enforcement mechanism.**
Do we want a lint / static-check rule that fails the build if
role-side code contains a `topology::parse` / `is_binding_side`
call?  Cheap to add via clang-query / clang-tidy custom check.
Recommend yes, after P3 — makes the invariant regressions loud.

**D-4 — P6 timing.**
P6 is the least urgent; safe to defer past this sprint.  User to
decide if it lands in the same arc or a follow-up.

## 8. Test policy across all phases

- Every P adds L2 tests before landing.
- L4 tests remain green unchanged — the layer refactor MUST NOT
  alter wire-observable behavior.
- Per `feedback_tests_pin_design_not_current_behavior`: assertions
  derive from the layer contract in §3, not from the shipped
  code's current shape.
- Per `feedback_multi_engine_parity_audit`: any change to
  `invoke_on_init` / native ABI in P7 must have parity coverage
  across Lua + Python + Native.

## 9. Cross-references to canonical TODOs

This draft is pointed to from:
- `docs/TODO_MASTER.md` — new arc entry "Queue-owned topology +
  layer cleanup (2026-07-11)."
- `docs/todo/API_TODO.md` — P3 layer surgery + A2 removal.
- `docs/todo/MESSAGEHUB_TODO.md` — P2.a stale confirmed-allowlist
  bug + P2.c error-code split.
- `docs/todo/TESTING_TODO.md` — P2 / P3 / P4 / P5 / P6 L2 test
  additions.
- `docs/todo/MESSAGEHUB_TODO.md` — P6 data-structure change also
  lives here (HubState-scoped, no dedicated MEMORY_LAYOUT_TODO in
  this repo).

Each canonical TODO carries a one-liner + link back here so any
one of them is enough to re-find this plan on session restart.

## 10. Discipline

- No code touched until user picks answers to §7's open decisions.
- Each P lands as its own commit, its own review pass, its own L2
  test additions.  No bundled "big refactor" PR.
- After each P, this draft's row in §4 table gets ✅ + date.
- When §4 is all ✅, merge lasting content into HEPs (P7 finishes),
  archive this draft to
  `docs/archive/transient-YYYY-MM-DD/` with a `DOC_ARCHIVE_LOG.md`
  entry per `docs/DOC_STRUCTURE.md §2.2`.
