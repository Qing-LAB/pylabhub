# TOPOLOGY_TODO.md — Singular-side ownership migration

**Permanent design authority:**
- **[HEP-CORE-0017 §3.3.0 + §3.3.0.1](../HEP/HEP-CORE-0017-Pipeline-Architecture.md)** — abstraction layer + decision matrix + factory dispatch.
- **[HEP-CORE-0017 §4.7](../HEP/HEP-CORE-0017-Pipeline-Architecture.md)** — per-topology sequence diagrams + Tier 2 pseudocode.
- **[docs/README/README_topology_channels.md](../README/README_topology_channels.md)** — user-facing tutorial (role JSON, script accessors, pitfalls).

**Migration-in-flight state authority:** [`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`](../tech_draft/DRAFT_topology_singular_side_2026-07.md) (status: **DESIGN LOCKED**, rev 10 shipped `ac820af9`; §7 + §10 promoted to HEP-CORE-0017 §4.7 on 2026-07-09).

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

## Status snapshot (2026-07-09) — REORDERED

**Priority principle (2026-07-09 correction):** the abstraction layer is
built FIRST (Phase C completion), then role code migrates to it (was
scattered across G), then tests flip to the new spawn order, then wire
work sits on the correct role behavior.  The previous ordering
(C wire → D wire → G role migration) landed wire work on top of
legacy role code, which is why R6 gate slice E1 broke tests (2026-07-09
attempt reverted): the wire enforced the topology model against roles
that hadn't been migrated yet.  All phases below aim at the SAME final
goal: one `hub::Queue::create_reader / create_writer(topology,
transport, opts)` call per role side, with all wire migrations sitting
on top of that abstraction.

| Phase | Description | Status | Notes |
|---|---|---|---|
| A | HEP amendments (docs only) | ✅ **COMPLETE** — 10 steps + rev 2/3/3.1 landed | — |
| B | Broker state + wire schema + admission gates | ✅ **COMPLETE** — 10 slices + rev 1/2 | — |
| C step 1 | Topology-parametric `ZmqQueue::create_reader/writer(topology, opts)` — PUSH/PULL dispatch on concrete ZmqQueue | ✅ **COMPLETE** (`50ceb5b6`) | Half the abstraction (ZMQ-only). |
| C step 2 | PUB/SUB support on `ZmqQueue::create_*` — fan-out ZMQ live | ✅ **COMPLETE** (`58c1a321..b71dd9ec`) | Full topology matrix on ZmqQueue side. |
| C step 3 | `ShmQueue::create_reader/writer(topology, opts)` — SHM half of the concrete transport factories | ✅ **COMPLETE** (`fbf5df68`) | Wraps existing `create_writer_standby` + `create_reader_standby` behind the topology-parametric shape.  Fan-in refused per §3.3.0 gate 1 (SHM host-local single-producer).  7 new L2 pin tests. |
| C step 4 | `hub::Queue::create_reader/writer(topology, transport, opts)` unified factory (`hub_queue_factory.hpp`) | ✅ **COMPLETE** (`830f8383`, folded with step 5) | Static-methods-only class per HEP-0017 §3.3.0.  Two-level dispatch: §3.3.0 gate 1 (`FanIn+Shm` refused) then translate opts + delegate to `ZmqQueue::create_*` / `ShmQueue::create_*`.  Zero new state, zero new mechanisms. |
| C step 5 | Transport-agnostic `RxOptions` / `TxOptions` structs per HEP-0017 §3.3.0 | ✅ **COMPLETE** (`830f8383`) | Landed with C step 4 — factory + option types shipped in one commit (factory without option types isn't independently useful).  Flat struct: common + zmq-specific + shm-specific side-by-side; factory reads only fields matching requested transport.  14 new L2 pin tests (dispatch + gate + parser + side-legality). |
| **C step 6** | **Role code migration** — `role_api_base.cpp::build_tx_queue / build_rx_queue` migrate to `hub::Queue::create_*` | ⏳ **NEXT** | Deletes the manual `if (transport == "shm") ... else ...` dispatch.  Deletes `zmq_bind` propagation from `role_config_translation.cpp`.  Reads `channel_topology` from role config into the new options struct. |
| **C step 7** | **Test spawn-order migration** — L3 + L4 fan-in tests flip to consumer-first (BINDING side goes first) | ⏳ **BLOCKED on C step 6** | Fixes the 3 tests that broke in the reverted R6 slice E1 attempt (`ZmqE2E_MultiProducer_TwoAuthorized`, `FanIn_TwoProducers_MetricsDoNotOverwrite`, `WaitPathDrainOnProducerDisconnect`). |
| D phase field | CHANNEL_AUTH_CHANGED_NOTIFY phase field + engine bindings | ✅ **COMPLETE** — `8655f2fe..ed0456d5` | Landed early (before role migration); wire is correct but sits on legacy role until C step 6.  No rework needed post-C step 6 — the wire payload matches the spec independent of role code shape. |
| **D R6 gate** | **R6 gate symmetrization** — dialing-side REG_REQ pends until binding side is Live + endpoint resolved + confirmed_version catches up | ⏳ **BLOCKED on C step 7** | Reverted 2026-07-09 (broke 3 tests due to premature enforcement); reinstates cleanly after Phase C completion, since C step 7 flips test spawn order to match R6 semantics. |
| E | Retirements — delete `push_to`/`pull_from`, `zmq_bind`, `producer_peers` vector, multi-endpoint PULL loop, `CONSUMER_ATTACH_REQ_ZMQ` handler | ⏳ Blocked on D R6 | Phase E plan below. |
| F | L4 demos + full-topology verification sweep | ⏳ Blocked on E | Fan-in, fan-out, one-to-one demos; validates each against the reference sequence flows (tech draft §7). |
| H | Full verification sweep | ⏳ Blocked on F | — |

**Retired phase labels (for traceability):**
- Phase C step 2 rev 2 A+B, rev 2.3 — folded into "C step 2" above (shipped complete).
- Phase D slices A/B/C/D — folded into "D phase field" above (shipped complete).
- Phase G (was: "fan-out ZMQ role-host integration + demo") — its role-host migration content moved UP into C step 6; its demo content moved into Phase F.  Phase G label retired.

### Fan-in binding-side reader correctness arc ✅ SHIPPED (2026-07-11)

Not a topology phase, but the reason C step 7 is important: the
fan-in binding-side *reader* had multiple correctness gaps
underneath the topology migration, all rooted in pre-migration
assumptions ("writer always binds", "REG_ACK carries the peer
set", "producer opens the channel").  Landed as five related
pieces per the arc summary in `docs/todo/API_TODO.md` +
`docs/todo/MESSAGEHUB_TODO.md`:

1. **Bug A binding-queue resolution** in `handle_channel_auth_notifies`
   — `set_peer_allowlist` targets the queue-that-binds by channel,
   not always `tx_queue`.
2. **G2 `allowlist_cache` seed** in `apply_consumer_reg_ack` —
   symmetric with `apply_producer_reg_ack`; makes
   `admitted_peers_count` correct on both dialing- and binding-side
   readers.
3. **Loop-ready gate** (HEP-CORE-0011 §"Loop-ready gate") — per-cycle
   AND composition of framework floor × user script.  Consumer's
   default gate reads `admitted_peers_count >= 1` for each rx-side
   channel.
4. **Broker topology-aware authorization + response semantics**
   for `GET_CHANNEL_AUTH_REQ` (§6.6.1 + §6.6.2) + fan-in
   consumer-opens `_on_channel_access_opened` wiring + fan-in
   consumer self-admission suppression.
5. **Dial-side readiness pull** (§6.6.3) — new `CHECK_PEER_READY_REQ`
   wire, extended `CHANNEL_AUTH_APPLIED_REQ` consumer branch,
   `ZmqQueue::apply_master_approval` defer + `dial_now()`, producer
   host `wait_for_peer_ready` pre-loop step.  Closes the CURVE
   handshake ordering race that was making the fan-in E2E test fail
   at 20 s timeout with zero data flow.

L4 `ZmqE2E_MultiProducer_TwoAuthorized` (fan-in E2E) now passes in
~3.4 s.  L4 `ZmqE2E_AuthorizedConsumerReceivesAllSlots` (fan-out
regression) continues to pass.  1651 L2 tests pass, 133 L4 tests
pass.  Design captured in HEP amendments; test coverage detail
in `docs/todo/TESTING_TODO.md`.

**Cross-reference for Phase C step 7.**  These fixes are
independent of the C step 7 spawn-order flip — the fan-in tests
pass whether the test harness spawns consumer-first or
producer-first, because the producer's `wait_for_peer_ready`
holds it until the consumer is ready regardless of spawn order.
Landing C step 7 remains the correct next topology step, but the
fan-in reader correctness gaps are no longer a blocker for it.

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

**Gaps (exact code deltas) — closed by `8655f2fe..ed0456d5`:**

1. ✅ `broker_service.cpp` fan-out helper — payload extended to `{channel_name, channel_version, role_uid, role_type, phase}`.  Retired `reason`.  Signature accepts `(phase, role_uid, role_type)`; reads `channel_version` from HubState.  Fan-out target dispatches by topology (fan-in → consumers; else producers).  All four call sites updated (consumer admitted / consumer left / consumer timeout / attach-wait targeted doorbell).  (`8655f2fe`)
2. ✅ `broker_service.cpp:handle_heartbeat_req` — first-heartbeat gate (`!eff.was_first_heartbeat_seen`) fires `phase=live` NOTIFY on DIALING-side heartbeats.  Existing `RolePresence.first_heartbeat_seen` state + `HeartbeatEffect` gate reused; no new state.  (`1d52050b`)
3. ✅ `role_api_base.cpp:handle_channel_auth_notifies` — dispatches on `phase`: `live` = local `live_peers` insert, no pull; `left` = `live_peers` erase + pull; `admitted` / absent = pull.  (`af06f065`)
4. ✅ `RoleAPIBase::pImpl` — `live_peers` map (channel → role_type → set<role_uid>) with dedicated mutex.  Distinct from `allowlist_cache` (allowlist for security vs live peers for observability).  (`af06f065`)
5. ✅ `RoleAPIBase::consumer_count(channel)` / `producer_count(channel)` — accessors snapshot live_peers under the lock.  (`af06f065`)
6. ✅ Engine bindings — `PlhNativeContext::consumer_count` / `producer_count` in native; `api.consumer_count` / `api.producer_count` in Lua; pybind `.def(...)` on Producer/Consumer/Processor APIs.  Multi-engine parity audit clean.  (`ed0456d5`)

**Follow-ups (deferred by design, not by drift):**
- L4 fan-out slow-joiner test using `api.consumer_count()` gate belongs to Phase F/G (fan-out role-host integration).  The mechanism is proven by 2362/2362 full ctest coverage across L2/L3/L4.
- R6 gate symmetrization (dialing-side REG_REQ pends on binding-side Live) per tech draft §5.4 — separate work item; the mechanism (phase=live NOTIFY + binding-side live_peers tracking) is now in place, so the R6 slice can consume it.

**Anti-scope (things I proposed but were wrong — DO NOT touch):**
- Socket monitor on PUB/SUB.  libzmq has no per-message HWM-drop event; the readiness mechanism is broker→NOTIFY, not framework introspection.
- New `NOTIFY` message type or new wire field beyond the four listed above.
- New `live_peers` structure on broker side — broker already has `ChannelEntry.consumers`/`producers` and can derive.
- HEP doc updates — HEP-CORE-0007 lines 1803-1864 + HEP-CORE-0028 §6a already document the target design.  Only new material would be an amendment banner if the wire schema drifts, which it should not.

**Blocking (Phase D wire migration for the still-legacy tests):**
- L4 `PlhHubCliTest.ZmqE2E_MultiProducer_TwoAuthorized` currently uses legacy PUSH-bind / PULL-connect fan-in.  Phase D wire ships the `phase=live` mechanism; the topology-model L4 flip lands in Phase F.  Phase D can keep the legacy L4 green by not breaking `phase=admitted`/`phase=left` back-compat during migration.

---

> **Extracted 2026-07-18 (✅ COMPLETE):** Phase A (10 HEP amendments), Phase B
> (broker state + wire + admission), and Phase B rev 1 + rev 2 (review findings
> 1-15) full detail.  Lasting design record: HEP-CORE-0017 §4.7 +
> `README_topology_channels.md`.  Verbatim at commit `633d51c0`; index
> `docs/archive/transient-2026-07-18/todo-completions/`.  Phase C step 3 ✅ also
> shipped; **C step 7, D R6 gate, Phases E-H remain open** (below).

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
| ✅ **#14 `CONSUMER_REG_ACK` still emits legacy `producers[]` array** | C step 2 rev 2.3 | Shipped `b71dd9ec` — unified peer-list wire shape (array of `{role_uid, endpoint, pubkey_z85}` objects); dialing-side ACK carries scalar `data_endpoint`/`data_pubkey` per HEP-CORE-0007 §12.3. |
| ✅ **#15 `CHANNEL_AUTH_CHANGED_NOTIFY` missing `phase` field** | D phase field | Shipped `8655f2fe..ed0456d5` — phase-field emission + first-heartbeat detection + live_peers cache + `consumer_count`/`producer_count`/`consumers`/`producers` accessors + engine bindings. |
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
