# TOPOLOGY_TODO.md — Singular-side ownership migration

**Design authority:** [`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`](../tech_draft/DRAFT_topology_singular_side_2026-07.md) (status: **DESIGN LOCKED**, rev 9 + rev 10 pending).

**Scope in one sentence:** Every channel has exactly one data-plane
endpoint, owned by the singular side of its topology.  Three
topologies (fan-in / fan-out / one-to-one) declared per channel;
cardinality + transport-compatibility gates enforced at broker
admission.

**Topologies:**
- **Fan-in** (N→1): ZMQ only; consumer binds PULL, producers connect PUSH.
- **Fan-out** (1→N): ZMQ (PUB/SUB, producer binds) or SHM (producer creates DataBlock).
- **1-to-1** (1→1): ZMQ (PUSH/PULL, producer binds) or SHM (producer creates DataBlock).

---

## Status snapshot (2026-07-09)

| Phase | Description | Status | Next action |
|---|---|---|---|
| A | HEP amendments (docs only) | ✅ **COMPLETE** — 9 steps + rev 2/3/3.1 landed | — |
| B | Broker state + wire schema + admission gates | ✅ **COMPLETE** — 10 slices landed | — |
| B rev 1 | Review findings — correctness + architecture + cleanup | ✅ **COMPLETE** — bugs #1/#2/#3 fixed | — |
| B rev 2 | Group C: AdmissionSide enum replaces is_consumer_reg bool | ✅ **COMPLETE** (`58b29ba8`) | — |
| C step 1 | Topology-parametric queue factory API + PUSH/PULL dispatch | ✅ **COMPLETE** (`50ceb5b6`) | — |
| C step 2 | PUB/SUB support — fan-out ZMQ live | ✅ **COMPLETE** (`58c1a321`) | — |
| C step 2 rev 2 A+B | Drift, defensive polish, PUB/SUB coverage | ✅ **COMPLETE** (`60fe0921`) | — |
| C step 2 rev 2.3 | B1 fix + unified peer-list wire shape | ✅ **COMPLETE** (`b71dd9ec`) | — |
| **D** | **CHANNEL_AUTH_CHANGED_NOTIFY phase field + R6 gate symmetrization** | ⏳ **PENDING** | **See §Phase D delta below** |
| E | Retirements (delete pre-attach queue + per-producer endpoints + old test file) | ⏳ Blocked on D | Phase E plan below |
| F | Demo + L4 flip | ⏳ Blocked on E | — |
| G | Fan-out ZMQ role-host integration + demo | ⏳ Blocked on F | — |
| H | Full verification sweep | ⏳ Blocked on G | — |

### Phase D — verified delta vs. HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_NOTIFY (lines 1803-1864)

**Source of truth:** HEP-CORE-0007 lines 1803-1864 already specifies
the post-2026-07-08 wire payload (REQUIRED: `channel_name`,
`channel_version`, `role_uid`, `role_type`, `phase`).  Scope is
match-code-to-doc, not invent-new-mechanism.

**What already exists in code — DO NOT reinvent:**
- `ChannelEntry::consumer_count()` / `producer_count()` (`hub_state.hpp:721-722`) — broker-side state accessors.
- `fire_channel_auth_changed_notify` (`broker_service.cpp:4351`) — fan-out helper; needs payload extension only.
- `ConsumerEntry.zmq_identity` (`hub_state.hpp:218`) — enables `send_to_identity` fan-out to binding-side consumers.
- `handle_channel_auth_notifies` (`role_api_base.cpp:1961`) — role-side handler; needs phase dispatch.
- `RoleAPIBase::allowed_peers(channel)` script accessor + binding — pattern for `consumer_count`/`producer_count`.
- `handle_heartbeat_req` (`broker_service.cpp:4413`) — insertion point for first-heartbeat detection.

**Gaps (exact code deltas):**

1. `broker_service.cpp:4379-4381` — payload emits `{channel_name, reason}`; extend to `{channel_name, channel_version, role_uid, role_type, phase}` per HEP-0007 lines 1840-1849.  Retire `reason`.
2. `broker_service.cpp:4351` signature — accept `(phase, role_uid, role_type, channel_version)`.
3. `broker_service.cpp:4384` fan-out target — currently `ch->producers`; dispatch by topology: fan-in → `ch->consumers`, fan-out/OneToOne → `ch->producers` (HEP-0007 line 1806).
4. Call sites `broker_service.cpp:3370` (`"consumer_joined"`), `:3544` (`"consumer_left"`), `:4020` (targeted doorbell) — pass phase/role_uid/role_type instead of reason.
5. `broker_service.cpp:handle_heartbeat_req` — first-heartbeat detection per role_uid per channel; on first heartbeat, fire `phase=live` NOTIFY (HEP-0007 lines 1819-1822).
6. `role_api_base.cpp:1961 handle_channel_auth_notifies` — dispatch on `phase`: `admitted`/`left` = pull `get_channel_auth` (current behavior); `live` = local map update, no pull (HEP-0007 lines 1829-1838).
7. `RoleAPIBase::pImpl` — add `live_peers` map (channel → set of live role_uids by role_type).
8. `RoleAPIBase` — add `consumer_count(channel)` / `producer_count(channel)` accessors reading from `live_peers` (HEP-CORE-0028 §6a).
9. Engine bindings — bind `consumer_count`/`producer_count` in native/lua/python; multi-engine parity audit per `feedback_multi_engine_parity_audit`.
10. L4 test — fan-out slow-joiner scenario using `consumer_count()` gate (retires the 200ms sleep in the L2 `TopologyFactory_FanOut_Roundtrip_PubSubSingleSubscriber` test).

**Anti-scope (things I proposed but were wrong — DO NOT touch):**
- Socket monitor on PUB/SUB.  libzmq has no per-message HWM-drop event; the readiness mechanism is broker→NOTIFY, not framework introspection.
- New `NOTIFY` message type or new wire field beyond the four listed above.
- New `live_peers` structure on broker side — broker already has `ChannelEntry.consumers`/`producers` and can derive.
- HEP doc updates — HEP-CORE-0007 lines 1803-1864 + HEP-CORE-0028 §6a already document the target design.  Only new material would be an amendment banner if the wire schema drifts, which it should not.

**Blocking (Phase D wire migration for the still-legacy tests):**
- L4 `PlhHubCliTest.ZmqE2E_MultiProducer_TwoAuthorized` currently uses legacy PUSH-bind / PULL-connect fan-in.  Phase D wire ships the `phase=live` mechanism; the topology-model L4 flip lands in Phase F.  Phase D can keep the legacy L4 green by not breaking `phase=admitted`/`phase=left` back-compat during migration.

---

## 1. Phase A — HEP amendments ✅ COMPLETE

10 coordinated HEP amendments landed 2026-07-08 across commits
`007b749d..e6a80070`.  Design authority for wire + state model
settled; no code work in this phase (docs only).

| Step | HEP | Commit | Scope |
|---|---|---|---|
| 1 | HEP-CORE-0007 | `007b749d` | §12.3 wire schema (channel_topology, data_endpoint/pubkey scalar), §12.4a six new error codes, §12.5 phase field |
| 2 | HEP-CORE-0033 | `315e2a4b` | §8 ChannelEntry gains topology + data_endpoint + versions; §18.2 wire catalog retirements |
| 3 | HEP-CORE-0017 | `e5a26ba6` | §3.3 major rewrite: topology-parametric queue model; §4.5/§4.5a/§4.6/§4.6.1 |
| 4 | HEP-CORE-0036 | `660ed1d4` | §3.5.2 R6 direction, §I7 endpoint disclosure, §6.5 NOTIFY chain with phase field |
| 5 | HEP-CORE-0023 | `63df2157` | §2.1.1 channel-life rule generalized ("binding side dies → channel dies") |
| 6 | HEP-CORE-0021 | `adc448fe` | §16 reparametrized: "producer" → "binding side" |
| 7 | HEP-CORE-0042 | `4af2e314` | Major scope narrowing: §5 dispatch + §7.1 pre-attach loop RETIRED |
| 8 | HEP-CORE-0028 | `415ac556` | §6a script API accessors: consumer_count / producer_count / consumers / producers |
| 9 | HEP-CORE-0044 | `383d6108` | SHM field-name unification: data_endpoint / data_pubkey |
| rev 2 | HEP-CORE-0036 + HEP-CORE-0033 | `c920938d` | §9 amendment banners; §18.2 ↔ §9.2 cross-refs |
| rev 3 | HEP-CORE-0018 | `3dac2761` | §5 config schema completeness fix (was overlooked in original nine) |
| rev 3.1 | HEP-CORE-0018 + HEP-CORE-0007 + tech draft | `e6a80070` | Directional prefix (`in_/out_channel_topology`) + Q3a REVISED (OPTIONAL default one-to-one) |

**Total:** ~1010 lines of new HEP text.  All amendments preserve
pre-migration content as archaeological reference (retire-in-place
pattern).

---

## 2. Phase B — Broker state + wire + admission ✅ COMPLETE

10 slices landed 2026-07-08 across commits `bba5e401..2c960cca`.
Every slice L2-verified before commit; final atomic slice L4-verified.

| Slice | Commit | Content |
|---|---|---|
| 0 | `bba5e401` | `ChannelTopology` enum + `parse_channel_topology` + `to_string` in hub_state.{hpp,cpp} |
| 1 | `d115d71b` | `ChannelEntry` gains 5 fields (topology, data_endpoint, data_endpoint_resolved, channel_version, confirmed_version) |
| 2 | `ed987afc` | HubState accessors on ChannelEntry (8 methods) |
| 3 | `b0e2b1d3` | JSON serialization for the 5 new fields |
| 4a | `60918571` | `ProducerRegInputs` + `ConsumerRegInputs` gain `channel_topology`; builder plumbing |
| 5 | `a90d15e2` | RoleConfig parses `in_/out_channel_topology`; role hosts populate `RegInputs.channel_topology` |
| 6 | `e58308ba` | Overwrite semantics simplified — no `topology_explicit`, no promotion branch (topology immutable at creation) |
| 7 | `637f08ed` | Helpers: `transport_topology_compatible` + `check_topology_against_stored` + 7 L2 tests |
| 8 | `268a2d5c` | Helper: `check_cardinality_admission` + 5 L2 tests |
| N (atomic) | `2c960cca` | Broker `handle_reg_req` + `handle_consumer_reg_req` wire the helpers; L4 fan-in test updated to declare `channel_topology: "fan-in"` |

**Verified:** L2 1592/1592 + L4 133/133.

**Also carrying (unrelated to Phase B scope):** `6c4d0e55` CI fix
for `attach_channel.hpp` include (`<nlohmann/json_fwd.hpp>` →
`utils/json_fwd.hpp`).

---

## 3. Phase B rev 1 — Review findings ⏳ PENDING

Multi-agent code + doc review 2026-07-08 evening surfaced **23
findings**.  15 to address in rev 1; 7 truly deferred to Phase
C/D/E (see §5); 1 false positive.

### 3.1 Layered fix ordering

The correctness bugs (#1 TOCTOU race, #2 false invariant) share a
common root: broker's admission logic snapshots state then mutates
in a separate lock window.  The clean fix moves ALL admission
checks under the writer lock in `HubState`, at which point the SHM
dead-code branch (#10) also falls out.  Doing this refactor first
avoids compounding the same anti-pattern into Phase C/D work.

**Ordering to keep tree green at every step:**

1. **Step 1 — Layer 2 grouping.**  Wrap the five topology helpers
   in `pylabhub::hub::topology` namespace (finding #23).  Pure
   move, adjusts imports in tests + broker.  Trivial.
2. **Step 2 — Layer 1 cleanup.**  Rename ChannelEntry accessors
   (drop `channel_` prefix per finding #19).  Refactor
   `data_endpoint` + `resolved` bool → `std::optional<std::string>`
   (#20).  Fix `topology` field default `FanOut` → drop initializer
   or `OneToOne` (#3).
3. **Step 3 — Layer 3 redesign.**  Consolidate admission checks
   inside `_on_producer_added` + `_add_consumer` under the writer
   lock.  Expand signatures with topology inputs; add typed
   outcomes for topology-mismatch / transport-not-compat /
   cardinality-violated.  Delete `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`
   branch (#10).  This fixes #1 + #2 + #10 together.
4. **Step 4 — Layer 4 thinning.**  Broker's `handle_reg_req` +
   `handle_consumer_reg_req` become protocol adapters: parse wire
   → build inputs → call atomic op → translate outcome to wire
   error.  ~110 lines of topology gates collapse to ~15 lines of
   outcome switch.
5. **Step 5 — Cleanup sweeps.**  Phase-label comment removal (#4),
   stale future-tense comments (#5), REQUIRED-vs-OPTIONAL drift
   (#6), consumer log role_uid (#7), HEP §5 vs §8 refs (#8), doc
   "or change" wording (#9), delete HEP-0033 §8 false
   "controlled-access" claim (#22).

### 3.2 Findings detail

**Correctness (2):**

| # | Finding | File:line | Fix in step |
|---|---|---|---|
| 1 | **TOCTOU race in cardinality gate.**  Broker snapshots via `hub_state_->channel(name)` (shared lock, releases), later `_on_producer_added` takes writer lock.  Concurrent REG_REQs on fan-out or 1-to-1 can both pass cardinality on stale snapshot, both append.  Tech draft §5.1 requires atomicity. | `broker_service.cpp:2452` + `hub_state.cpp:1091` (producer); `broker_service.cpp:3043` + `hub_state.cpp:602` (consumer) | Step 3 |
| 2 | **`ChannelTransportInvariants.topology` false invariant.**  Passed to `_on_producer_added` but never compared on existing-channel branch (unlike `data_transport`, `schema_*`).  Maintenance trap. | `hub_state.hpp:397` + `hub_state.cpp:1144-1150` | Step 3 |

**Architecture (5):**

| # | Finding | Fix in step |
|---|---|---|
| 19 | `channel_` prefix on new ChannelEntry accessors breaks pre-existing convention (`is_shm`, `producer_count`, `first_producer`, `add_producer`).  Rename to `topology()`, `data_endpoint()`, `endpoint_resolved()`, `channel_version()`, `confirmed_version()`. | Step 2 |
| 20 | `data_endpoint` (string) + `data_endpoint_resolved` (bool) encode one state.  Replace with `std::optional<std::string> data_endpoint`; `has_value()` == resolved.  Setter idempotence becomes explicit. | Step 2 |
| 22 | HEP-0033 §8 claims "direct field mutation is forbidden — controlled-access API only" but ChannelEntry is a struct with public fields directly assigned in `_on_producer_added`.  Delete the false claim from HEP-0033 §8; the struct-with-friend model isn't enforced by the compiler. | Step 5 |
| 23 | Five topology helpers (`parse_channel_topology`, `to_string`, `transport_topology_compatible`, `check_topology_against_stored`, `check_cardinality_admission`) as free functions in `pylabhub::hub`.  Cohesive concept — group in `pylabhub::hub::topology` namespace.  Callers: `topology::parse(...)`, `topology::matches_or_inherit(...)`, etc. | Step 1 |
| 10 | `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` at `broker_service.cpp:2609` (dispatched from `add_producer` `RejectedShmCardinality`) is dual-path residue.  After #1 fix, all paths are pre-gated by topology cardinality.  Delete the branch. | Step 3 |

**State model (1):**

| # | Finding | Fix in step |
|---|---|---|
| 3 | `ChannelEntry.topology` default is `FanOut` (`hub_state.hpp:607`).  Broker admission defaults to `OneToOne` when wire absent; `ChannelTransportInvariants::topology` defaults `OneToOne`.  Field-level `FanOut` default is dead + wrong.  Drop initializer (aggregate init in `_on_producer_added` sets it) or align to `OneToOne`. | Step 2 |

**Comment / doc hygiene (7):**

| # | Finding | Fix in step |
|---|---|---|
| 4 | **"Phase B slice N" labels in code comments** — 18 instances in `src/`, 1 in `tests/`.  Memory rule: no phase labels in comments.  Rewrite as "2026-07-08 topology migration" or drop tense. | Step 5 |
| 5 | Stale future-tense comments — "the atomic Phase B commit will populate" language despite atomic commit already landed (`2c960cca`).  Files: `hub_state.hpp`, `role_reg_payload.hpp`, `hub_state_json.cpp`. | Step 5 |
| 6 | `REQUIRED` claims in comments contradict lenient-default reality after rev 3.1.  Files: `role_reg_payload.hpp:80,90,125,128`.  Update to "OPTIONAL, default one-to-one". | Step 5 |
| 7 | Consumer reject logs drop `role_uid` (`broker_service.cpp:3066,3084`).  Accept-side (`:3403`) carries it.  Add `role='{}'` to both consumer reject WARN lines. | Step 5 |
| 8 | HEP §-reference drift — `hub_state.hpp:933` cites "HEP-CORE-0033 §5 (ChannelEntry row amendment)" but §5 is CLI; ChannelEntry lives in §8. | Step 5 |
| 9 | Doc "Overwrite semantics" wording — HEP-CORE-0018 §5 + HEP-CORE-0007 §12.3 row-descriptions use "set or change" phrasing that implies promotion.  Actual behavior is IMMUTABLE at creation.  Drop "or change". | Step 5 |
| 22 (dup) | HEP-0033 §8 encapsulation claim — see Architecture section. | Step 5 |

**False positive (1):**

- **#21 Missing `channel_version` reset op.**  HEP-0042 §5.4
  which specified this is RETIRED (Phase A step 7 scope narrowing).
  Under new model no reset needed.  **Do not fix — no work item.**

### 3.3 Success criteria

- Full L2 sweep 1592/1592 passing after every step.
- Full L4 sweep 133/133 passing after step 3 (Layer 3 redesign).
- L2 regression test for concurrent-REG_REQ under fan-out /
  one-to-one that would have caught #1 pre-fix.
- Grep confirms zero "Phase B slice" strings in `src/` + `tests/`.
- Grep confirms zero "atomic Phase B commit" future-tense in `src/`.
- Grep confirms zero `REQUIRED` in role_reg_payload.hpp comments.
- HEP-0033 §8 no longer claims controlled-access enforcement.
- HEP-0018 §5 + HEP-0007 §12.3 no longer say "set or change".

---

## 4. Phase C onwards — plan detail

### Phase C — Queue factory rewire

Add topology-aware factory alongside the legacy factories.  Both
coexist during transition.

**Files:**
- [ ] `src/include/utils/hub_zmq_queue.hpp` — Add
  `Queue::create_reader(topology, transport, opts)` +
  `create_writer(...)`.  Keep `pull_from` / `push_to` legacy
  factories (transitional).  Add `zmq::socket_type::pub` / `sub`
  code paths behind topology dispatch.
- [ ] `src/utils/hub/hub_zmq_queue.cpp` — Implement per
  HEP-0017 §3.3.0 decision matrix.  All six (side × topology ×
  transport) combos.  Fan-in ZMQ: PULL bind (consumer) + PUSH
  connect (producer).  Fan-out ZMQ: PUB bind (producer) + SUB
  connect (consumer) with empty subscription.  1-to-1 ZMQ: PUSH
  bind (producer) + PULL connect (consumer).  SHM paths unchanged.
- [ ] L2 queue tests — Add all six (side × topology × transport)
  factory tests.  Add PUB/SUB CURVE tests.  Add 1-to-1 PUSH/PULL
  tests.

**Blocking:** Phase B rev 1 (must fix admission architecture before
adding more callers of the accessors).

**Success:** Both factory paths compile + pass tests.  Existing L4
tests still use legacy factories and still pass.

### Phase D — R6 gate symmetrization

Adds "pending REG_REQ" mechanism per tech draft §5.4 — dialing side
pends until binding side is Live + endpoint resolved + allowlist
confirmed.  Wire additions: `role_registration_version` capture,
wake events, timeout handling.

**Files:**
- [ ] `src/utils/ipc/broker_service.cpp` — Add pending queue for
  dialing-side REG_REQs.  Add wake events: ENDPOINT_UPDATE_REQ
  ACK, CHANNEL_AUTH_APPLIED_REQ ACK, HEARTBEAT arrival.
  Re-evaluate pending REGs on each event.  Redirect
  `CHANNEL_AUTH_CHANGED_NOTIFY` target based on topology (fan-in
  → consumer, fan-out → producer).  Emit `phase` field on NOTIFY.
- [ ] `src/utils/ipc/broker_service.cpp` — Emit `CHANNEL_CLOSED`
  when binding side disconnects with pending REG_REQs.  (Only
  becomes reachable in Phase D.)
- [ ] `src/utils/ipc/broker_service.cpp` — Fan-in consumer-first
  create: `handle_consumer_reg_req` allows channel creation when
  no channel exists AND `channel_topology="fan-in"` explicit.
- [ ] Wire `ChannelEntry::data_endpoint` + `resolved` via
  ENDPOINT_UPDATE_REQ handler (broker-side accessor calls that
  were dead through Phase B).
- [ ] Wire `set_confirmed_version` on CHANNEL_AUTH_APPLIED_REQ.
- [ ] `CONSUMER_REG_ACK` payload update — replace `producers[]`
  array with scalar `data_endpoint` + `data_pubkey` per
  HEP-0007 §12.3.
- [ ] L3 broker-role integration tests — Add
  `FanIn_ProducerRegReqBlocksUntilConsumerReady`,
  `FanIn_ProducerRegReqDrainOnAllowlistApplied`,
  `FanOut_ConsumerRegReqBlocksUntilProducerReady`.

**Blocking:** Phase C queue factory live.

**Success:** R6 gate provably blocks/unblocks correctly under both
topology directions.

### Phase E — Retirements

Delete code paths superseded by the migration.  Only starts AFTER
Phase D has migrated all callers off the retiring surfaces.

**Files:**
- [ ] `src/utils/ipc/broker_service.cpp` — Delete
  `handle_consumer_attach_req_zmq` (~400 LOC).  Delete pre-attach
  queue draining logic.  Delete `CHANNEL_PRODUCERS_CHANGED_NOTIFY`
  broadcast + `GET_CHANNEL_PRODUCERS_REQ` handler.  Collapse
  `confirmed_version[K][P]` map to scalar (retire the map form).
- [ ] `src/include/utils/broker_request_comm.hpp` + `.cpp` —
  Delete `consumer_attach_zmq` client method.
- [ ] `src/include/utils/hub_state.hpp` — Retire
  `ProducerEntry.zmq_node_endpoint` field + related accessors.
- [ ] `src/utils/hub/hub_zmq_queue.cpp` — Delete multi-endpoint
  PULL loop from commit `2c604280`.  Delete `add_producer_peer`
  / `remove_producer_peer` / `set_producer_peers` methods.
- [ ] `src/utils/service/role_api_base.cpp` — Delete consumer's
  §7.1 pre-attach loop.  Delete `attach:begin/success/complete`
  log markers.
- [ ] `tests/test_layer3_pattern4/test_pattern4_attach_coordination.cpp` —
  **DELETE ENTIRELY** (~500 LOC per estimate, likely ~1000 LOC
  actual).
- [ ] Legacy factories in `hub_zmq_queue.hpp` — retire
  `pull_from(endpoint, key, ...)` and `push_to(...)`.

**Blocking:** Phase D R6 symmetrization live + verified.

**Success:** Full ctest passes.  No dead code left from HEP-0042
§7.1.

### Phase F — Demo + L4 flip

- [ ] `share/py-demo-single-processor-zmq/*.json` — Add explicit
  `channel_topology` where fan-in / fan-out is intended (default
  one-to-one for simple demos).
- [ ] `tests/test_layer4_plh_hub/test_plh_hub_role_zmq_e2e.cpp` —
  Add tests for cardinality rejection paths.  Confirm
  `ZmqE2E_OneToOne_ProducerBinds`,
  `ZmqE2E_FanIn_SecondConsumerRejected`,
  `ZmqE2E_FanOut_SecondProducerRejected`,
  `ZmqE2E_OneToOne_SecondSideRejected`.
- [ ] L4 SHM 1-to-1 test — `test_plh_hub_role_shm_e2e.cpp` add
  `ShmE2E_OneToOne_SingleConsumer` + cardinality-rejection variant.

**Blocking:** Phase E retirements complete.

**Success:** Demos run.  L4 tests green.

### Phase G — Fan-out ZMQ implementation

Currently only SHM supports fan-out.  This phase adds PUB/SUB paths.

- [ ] `src/utils/hub/hub_zmq_queue.cpp` — PUB/SUB socket paths +
  subscribe on connect.  Handle slow-joiner per tech draft §7.6
  (script decides policy).
- [ ] `tests/test_layer4_plh_hub` — Add
  `ZmqE2E_FanOut_OneProducerTwoConsumers` scenario.
- [ ] Demo — Add `share/py-demo-fan-out-zmq/` example.

**Blocking:** Phase F complete.

**Success:** ZMQ fan-out works end-to-end.

### Phase H — Full verification

- [ ] `ctest -j2` — full sweep passes.
- [ ] SHM fan-out (HEP-CORE-0041) — verify unaffected.
- [ ] HEP-CORE-0044 AttachProtocol — verify unaffected.
- [ ] HEP-CORE-0045 broker observer — verify unaffected.
- [ ] Cross-check demo runs manually.

---

## 5. Deferred review findings — Phase C/D/E scope

Review surfaced 7 items that ARE work-to-do but NOT Phase B (rev 1)
scope.  Each requires infrastructure from the phase noted.

| Finding | Phase | Why not Phase B rev 1 |
|---|---|---|
| **#11 `CHANNEL_CLOSED` unreachable** — HEP-0007 §12.4a catalogs but no emission site. | D | Depends on #12 pending REG_REQ mechanism. |
| **#12 No REG_REQ pending / `role_registration_version` capture** — tech draft §5.4 R6 gate not built. | D | Substantial new infrastructure (correlation IDs, wake events, timeout).  Phase D's whole scope. |
| **#13 Consumer-first-create for fan-in** — `handle_consumer_reg_req` returns `CHANNEL_NOT_FOUND` if channel doesn't exist, but under fan-in consumer is BINDING side and should be able to create. | D | Part of the R6 symmetrization work.  Current producer-first ordering works for L4 fan-in test. |
| **#14 `CONSUMER_REG_ACK` still emits legacy `producers[]` array** — HEP-0007 §12.3 marks it retired, wire not migrated. | C or D | Queue factory rewire depends on this (dialing side reads `data_endpoint` scalar).  Land with Phase C or D. |
| **#15 `CHANNEL_AUTH_CHANGED_NOTIFY` missing `phase` field** — HEP-0007 §12.5 requires phase (admitted/live/left), not yet emitted. | D | Needs first-heartbeat wire event + live_peers tracking. |
| **#16 Duplicate `channel_version` / `confirmed_version` state** — new scalar coexists with old `[K][P]` map (`ChannelAccessEntry.channel_version` + `confirmed_version_per_producer` map). | E | Retirement phase — needs all callers migrated first. |
| **#17 `ProducerEntry.zmq_node_endpoint` retirement** — HEP-0033 §8 says retires; code keeps field + accessor. | E | Retirement phase — all callers migrate first. |
| **#18 Dead accessors** — `set_channel_data_endpoint`, `bump_channel_version`, `set_confirmed_version` have no callers. | C/D | Become live when Phase D wires ENDPOINT_UPDATE_REQ + CHANNEL_AUTH_APPLIED_REQ handlers. |

**Trigger note:** Phase B rev 1's Layer 1 accessor rename (finding
#19) applies to `set_channel_data_endpoint` etc. even though they're
uncalled — Phase C/D handlers pick them up under the new names.

---

## 6. Design decisions locked

| ID | Question | Answer | Source |
|---|---|---|---|
| Q1 | Fan-in death when consumer dies | ✅ Intended — "no consumer, what's the point of fan-in?" | 2026-07-08 user |
| Q2 | PUB/SUB slow joiner handling | ✅ Script decides via `api.consumer_count()`.  Framework provides mechanism, script decides policy. | 2026-07-08 user; see `feedback_framework_mechanism_not_policy` in memory |
| Q3a original | `channel_topology` requirement | ~~REQUIRED, no default. Missing → INVALID_REQUEST.~~ REVISED 2026-07-08 evening. | Tech draft §13.1 (struck through) |
| **Q3a revised** | `channel_topology` requirement | ✅ **OPTIONAL, default `one-to-one`.**  Only EXPLICIT declarations set stored topology; empty inherits.  Immutable at creation. | 2026-07-08 evening user; Phase A rev 3.1 |
| Q3b | `confirmed_version` collapse timing | ✅ Phase B added SCALAR fields; retirement of `[K][P]` map deferred to Phase E (both coexist during transition — no field on ChannelAccessEntry deleted yet). | Tech draft §13.1 + review finding #16 |
| 1-to-1 topology | Added as distinct third topology | ✅ Broker enforces exactly-one-consumer + exactly-one-producer cardinality. | 2026-07-08 user |

**Revised phase-B state model philosophy:** Under Q3a revised,
Phase B lands *state fields* + *admission enforcement of cardinality
& topology-immutability* but does NOT force topology to be declared
on every REG_REQ.  Existing 1-to-1 demos work unchanged; fan-in /
fan-out flows must declare explicitly.

---

## 7. Files touched — actual scope (through 2026-07-08)

### Docs (Phase A: ~1010 lines added across 10 HEPs)

`docs/HEP/HEP-CORE-0007.md`, `HEP-CORE-0017.md`, `HEP-CORE-0018.md`,
`HEP-CORE-0021.md`, `HEP-CORE-0023.md`, `HEP-CORE-0028.md`,
`HEP-CORE-0033.md`, `HEP-CORE-0036.md`, `HEP-CORE-0042.md`,
`HEP-CORE-0044.md`; `docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`.

### Code (Phase B: ~750 lines added across 12 files)

| File | Lines added | Purpose |
|---|---|---|
| `src/include/utils/hub_state.hpp` | ~260 | ChannelTopology enum, ChannelEntry fields, accessors, helper prototypes |
| `src/utils/ipc/hub_state.cpp` | ~90 | Enum impls, helper functions, topology persistence in `_on_producer_added` |
| `src/utils/ipc/broker_service.cpp` | ~190 | Admission gates in `handle_reg_req` + `handle_consumer_reg_req` |
| `src/utils/ipc/hub_state_json.cpp` | ~15 | 5-field JSON serialization |
| `src/include/utils/role_reg_payload.hpp` | ~60 | RegInputs.channel_topology + builder plumbing |
| `src/include/utils/config/role_config.hpp` | ~10 | Directional accessors |
| `src/utils/config/role_config.cpp` | ~15 | Directional parsing |
| `src/producer/producer_role_host.cpp` | ~5 | Populate channel_topology on REG_REQ |
| `src/consumer/consumer_role_host.cpp` | ~10 | Named-field refactor + populate |
| `src/processor/processor_role_host.cpp` | ~15 | Both sides |
| `src/include/utils/security/attach_channel.hpp` | 1 | Include swap (CI fix, orthogonal) |

### Tests (~180 lines added across 2 files)

| File | Lines added | Content |
|---|---|---|
| `tests/test_layer2_service/test_hub_state.cpp` | ~150 | 12 topology tests (parse, roundtrip, transport-compat, three-branch check, cardinality × 3 topologies × 2 role-types) |
| `tests/test_layer4_plh_hub/test_plh_hub_role_zmq_e2e.cpp` | ~15 | Fan-in test declares channel_topology="fan-in" |

### Phase C-H estimates (not yet implemented)

Per tech draft §11: total migration ~1600 changed / ~1500 deleted
across queue impl + broker + demos + tests.  Retirement of
`test_pattern4_attach_coordination.cpp` alone is ~1000 LOC delete.

---

## 8. Recent-work re-evaluation

| Commit | Content | Fate |
|---|---|---|
| `2c604280` | HEP-CORE-0017 §3.3 multi-endpoint PULL fix | Vestigial after Phase D (dialing-side single-connect).  Delete in Phase E. |
| `9d0ca4c8` | HEP-CORE-0021 §16 amendment | Reparametrized in Phase A step 6 (`adc448fe`).  Not reverted. |

Both still work today; L4 tests still pass.  Retirement scheduled,
not immediate.

---

## 9. Session hygiene

**When resuming this work in a future session:**

1. Read this file top-to-bottom.
2. Check status snapshot (top of file) for current phase.
3. Read the tech draft
   (`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`)
   at the same time — it's the design authority.
4. Cross-check current git log against phase completion — a
   prior session may have advanced without updating this file.
5. Open the relevant HEPs at start of code work per CLAUDE.md
   "Refresh guidance before code/test work" rule.

---

## 10. Rejection criteria

If any surfaces during review, redesign or reject:

- N×M topology becomes a real deployment requirement (design
  assumes N→1 or 1→N only).
- Producer identity is expected to include a stable data endpoint
  claim (fan-in producers under this design don't own endpoints).
- Broker's per-producer bookkeeping is expected to include
  endpoints for discovery mechanisms other than CONSUMER_REG_ACK.

---

## 11. Change log

| Date | Change |
|---|---|
| 2026-07-08 morning | Initial creation.  Design draft, awaiting approval. |
| 2026-07-08 midday | Phase A landed (10 HEP amendments + rev 2). |
| 2026-07-08 afternoon | Phase A rev 3 (HEP-0018 §5 completeness fix).  Rev 3.1 (Q3a REVISED to OPTIONAL default one-to-one; directional prefix `in_/out_channel_topology`). |
| 2026-07-08 evening | Phase B landed (10 slices, atomic completion `2c960cca`).  Multi-agent code + doc review surfaced 23 findings.  Phase B rev 1 planned. |
