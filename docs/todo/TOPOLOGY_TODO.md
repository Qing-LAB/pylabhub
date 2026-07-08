# TOPOLOGY_TODO.md — Singular-side ownership migration

**Scope:** Migration to topology-parametric data-plane design.  Three
explicit topologies:
- **Fan-in** (N→1): ZMQ only; consumer binds PULL, producers connect PUSH.
- **Fan-out** (1→N): ZMQ (PUB/SUB, producer binds) or SHM (producer creates DataBlock).
- **1-to-1** (1→1): ZMQ (PUSH/PULL, producer binds) or SHM (producer creates DataBlock).

Singular side (or producer, for 1-to-1) owns endpoint and ZAP allowlist.
`channel_topology` is REQUIRED on every REG_REQ — no lenient default.
Cardinality enforcement at broker's REG_REQ handler.

**Design authority:** [`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`](../tech_draft/DRAFT_topology_singular_side_2026-07.md)

**Status:** ⏳ DESIGN DRAFT — not adopted yet; awaiting user approval.

**Owner:** Line 1 (main auth chain) architectural direction.

**Blocking (or reevaluating) recent work:**
- Commit `2c604280` — HEP-CORE-0017 §3.3 multi-endpoint PULL fix.  Under this migration, retires in Phase E.
- Commit `9d0ca4c8` — HEP-CORE-0021 §16 amendment as drafted.  Reparametrized (not reverted) in Phase A.

---

## 1. Why we're doing this

The current framework binds producer PUSH sockets regardless of channel
topology.  For fan-in (many producers → one consumer), this is the wrong
side to bind — it forces per-producer endpoint tracking on the broker,
per-producer connect loops on the consumer's PULL, per-producer
allowlist coordination via HEP-CORE-0042 §7.1, and per-producer ENDPOINT
updates.  Every one of these is complexity we've been chipping away at,
and every one goes away under the "singular side binds" rule.

Under the new design:
- Broker tracks one `data_endpoint` per channel (on `ChannelEntry`, not on `ProducerEntry`).
- Queue's PULL binds once (fan-in) or SUB connects once (fan-out).
- Allowlist lives on the binding side; wire chain (NOTIFY → GET_AUTH → APPLIED) direction inverts per topology.
- HEP-0042 §7.1 pre-attach loop retires entirely.
- ENDPOINT_UPDATE_REQ still exists but from whichever side is singular.

Net effect: ~50 LOC net delta but architecturally much simpler; 1 full
wire coordination protocol retires; state model shrinks; multi-endpoint
PULL loop retires.

---

## 2. Phased implementation

Each phase should be one commit or a tight batch.  Phases MUST land in
order to keep the tree green.

### Phase A — HEP amendments (docs only, no code)

Draft coordinated amendments for the 7 HEPs affected.  Each amendment
lands in a single commit; the batch lands together after full review.

- [ ] **HEP-CORE-0017 §3.3, §4.6, §4.6.1** — major rewrite.  Replace per-peer connect model with singular-binds-plural-dials.  Retire `ProducerPeer` vector.  Retire `add_producer_peer` / `remove_producer_peer` API.
- [ ] **HEP-CORE-0021 §16** — reparametrize.  Rewrite the previously-adopted amendment (2026-07-08) with "producer" replaced by "singular side of the channel's topology" throughout.  State machine + mid-life rules + R6 extension carry over unchanged in mechanism.
- [ ] **HEP-CORE-0033** — update ChannelEntry description.  Add `topology` + `data_endpoint` + `data_endpoint_resolved`.  Retire `ProducerEntry.zmq_node_endpoint`.  Update wire catalog §2994 for wire retirements + new fields.
- [ ] **HEP-CORE-0036 §3.5.3, §6.4, §6.5, §6.5.1, §I7, §5.2 R6, §14** — symmetrize.  R6 gate direction generalizes.  NOTIFY chain direction parameterizes.  Endpoint disclosure rule updates.  Rollout tables un-mark HEP-0042 as active work; note as retiring.
- [ ] **HEP-CORE-0042 (major scope narrowing)** — retire §5 dispatch + §7.1 pre-attach loop.  Preserve `confirmed_version` bookkeeping (now scalar).  Preserve HEP-0044 AttachProtocol reference (SHM, orthogonal).  Note: the whole "pre-attach coordination" concept is subsumed by the REG_REQ R6 gate.
- [ ] **HEP-CORE-0023 §2.1.1** — generalize channel-existence rule.  "Last producer disconnected → channel torn down" becomes "singular side disconnected → channel torn down."
- [ ] **HEP-CORE-0007 §12** — wire catalog updates.  Add `channel_topology` field to REG_REQ / CONSUMER_REG_REQ (REQUIRED — no default).  Wire values: `"fan-in"` / `"fan-out"` / `"one-to-one"`.  Add `data_endpoint` + `data_pubkey` fields to dialing-side ACKs.  Retire per-producer array on CONSUMER_REG_ACK.  Add error codes: `TOPOLOGY_MISMATCH`, `TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT`, `FAN_IN_IS_SINGLE_CONSUMER`, `FAN_OUT_IS_SINGLE_PRODUCER`, `ONE_TO_ONE_CARDINALITY_VIOLATED`, `CHANNEL_CLOSED` (for pending REGs when channel torn down).
- [ ] **HEP-CORE-0028** — script API accessor amendment.  Add `api.peer_count(channel_name)` + `api.peers(channel_name)` for both binding-side roles (producer under fan-out/1-to-1, consumer under fan-in).  Cross-engine parity (Lua + Python + Native).  Update wire request-response table with retirements.

**Blocking:** none.  Purely documentation.

**Success criterion:** User approves the coordinated amendment package.

---

### Phase B — Broker state + wire schema + confirmed_version collapse (bigger atomic commit)

Q3 answered (2026-07-08): `channel_topology` REQUIRED (no lenient
default), `confirmed_version` collapses to scalar in Phase B (not
deferred).  Consequence: Phase B is a bigger atomic change — broker
handler + ALL tests + ALL demos must update in the same commit.

- [ ] `src/include/utils/hub_state.hpp` — Add `ChannelTopology` enum (`FanIn` / `FanOut` / `OneToOne`) + `topology` + `data_endpoint` + `data_endpoint_resolved` fields on `ChannelEntry`.  Collapse `confirmed_version` from `[K][P]` map to scalar `[K]`.  Retire `ProducerEntry.zmq_node_endpoint` with a mirror-and-mark-read-only guard for transitional callers.
- [ ] `src/utils/hub/hub_state.cpp` — Add `set_channel_data_endpoint` + `channel_data_endpoint` accessors.  Update all `confirmed_version` read sites to scalar shape.
- [ ] `src/utils/ipc/broker_service.cpp` — REG_REQ / CONSUMER_REG_REQ handlers: (a) require `channel_topology` field (reject `INVALID_REQUEST` if missing), (b) validate transport × topology matrix upfront (reject `TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT`), (c) check cardinality per topology (reject with specific error codes), (d) extract `create_or_join_channel(topology, transport, ...)` shared internal.
- [ ] All existing demos + L4 tests updated in same commit to declare `channel_topology` explicitly.
- [ ] L2 broker unit tests — Add `TopologyValidation` + `TopologyMissingField` + `TopologyMismatch` + `TopologyNotSupportedForTransport` + `CardinalityViolation` × 3 tests.

**Blocking:** Phase A HEP amendments adopted.

**Success criterion:** State fields live; full ctest passes with explicit topology on all REG_REQs; no `confirmed_version` map shape remains.

---

### Phase C — Queue factory rewire

Add topology-aware factory alongside the legacy factories.  Both should
coexist during transition.

- [ ] `src/include/utils/hub_zmq_queue.hpp` — Add new factory signatures: `Queue::create_reader(topology, transport, opts)` + `create_writer`.  Keep `pull_from` / `push_to` legacy factories (transitional).  Add `zmq::socket_type::pub` / `sub` code paths behind topology dispatch.
- [ ] `src/utils/hub/hub_zmq_queue.cpp` — Implement per §6.2 decision matrix.  All six (side × topology × transport) combos handled.  Fan-in ZMQ: PULL bind (consumer) + PUSH connect (producer).  Fan-out ZMQ: PUB bind (producer) + SUB connect (consumer) + `setsockopt(ZMQ_SUBSCRIBE, "")`.  1-to-1 ZMQ: PUSH bind (producer) + PULL connect (consumer).  SHM paths unchanged (fan-out + 1-to-1 use existing capability transport).  Reuse existing CURVE + ZAP wiring; only bind/connect direction changes.
- [ ] L2 queue tests — Add `create_reader/writer` factory tests for all six (side × topology × transport) combos.  Add PUB/SUB CURVE tests.  Add 1-to-1 PUSH/PULL tests.

**Blocking:** Phase B state fields live.

**Success criterion:** Both factory paths compile + pass tests.  Existing L4 tests still use legacy factories and still pass.

---

### Phase D — R6 gate symmetrization

- [ ] `src/utils/ipc/broker_service.cpp` — Add plural-side pending logic to REG_REQ handler (fan-in producer waits).  Add wake events: ENDPOINT_UPDATE_REQ ACK, CHANNEL_AUTH_APPLIED_REQ ACK, HEARTBEAT arrival.  Re-evaluate pending REGs on each event.  Redirect `CHANNEL_AUTH_CHANGED_NOTIFY` target based on topology (fan-in → consumer, fan-out → producer).
- [ ] L3 broker-role integration tests — Add `FanIn_ProducerRegReqBlocksUntilConsumerReady` + `FanIn_ProducerRegReqDrainOnAllowlistApplied` + `FanOut_ConsumerRegReqBlocksUntilProducerReady`.

**Blocking:** Phase C queue factory live.

**Success criterion:** R6 gate provably blocks/unblocks correctly under both topology directions.

---

### Phase E — Retirements

Delete the code paths that this design supersedes.

- [ ] `src/utils/ipc/broker_service.cpp` — Delete `handle_consumer_attach_req_zmq` (~400 LOC).  Delete pre-attach queue draining logic.  Delete `CHANNEL_PRODUCERS_CHANGED_NOTIFY` broadcast + `GET_CHANNEL_PRODUCERS_REQ` handler.  Collapse `confirmed_version[K][P]` to scalar `confirmed_version[K]`.
- [ ] `src/include/utils/broker_request_comm.hpp` + `.cpp` — Delete `consumer_attach_zmq` client method.
- [ ] `src/include/utils/hub_state.hpp` — Retire `ProducerEntry.zmq_node_endpoint`.  Delete `set_producer_zmq_node_endpoint` / `producer_zmq_node_endpoint` per-producer accessors.
- [ ] `src/utils/hub/hub_zmq_queue.cpp` — Delete multi-endpoint PULL loop from commit `2c604280`.  Delete `add_producer_peer` / `remove_producer_peer` / `set_producer_peers` methods.  Delete the `producer_peers_` vector's dual role (peer set becomes scalar).
- [ ] `src/utils/service/role_api_base.cpp` — Delete consumer's §7.1 pre-attach loop.  Delete the `attach:begin/success/complete` log emission.
- [ ] `tests/test_layer3_pattern4/test_pattern4_attach_coordination.cpp` — **DELETE ENTIRELY** (~500 LOC).  Rule-6 doc-block at the CMakeLists site if applicable.
- [ ] Legacy factories in `hub_zmq_queue.hpp` — retire `pull_from(endpoint, key, ...)` and `push_to(...)`.  Update L2 tests to use new factory.

**Blocking:** Phase D R6 symmetrization live.

**Success criterion:** Full ctest passes.  No dead code left from HEP-0042 §7.1.

---

### Phase F — Demo + L4 flip

- [ ] `share/py-demo-single-processor-zmq/producer/producer.json` — Set `out_zmq_bind: false`, add `channel_topology: "fan-in"`.  Remove `out_zmq_endpoint` (producer doesn't bind).
- [ ] `share/py-demo-single-processor-zmq/consumer/consumer.json` — Set `in_zmq_bind: true`, add `channel_topology: "fan-in"`.  Set `in_zmq_endpoint: "tcp://127.0.0.1:0"` (ephemeral bind).
- [ ] `share/py-demo-single-processor-zmq/processor/processor.json` — Flip in-side bind (consumer of upstream channel).  Add topology.
- [ ] Other ZMQ demos under `share/` — same pattern.
- [ ] `tests/test_layer4_plh_hub/test_plh_hub_role_zmq_e2e.cpp` — Flip Scenario A + Scenario C to consumer-binds pattern.  Add `channel_topology: "fan-in"` to configs.  Remove pid-based port workaround (consumer binds `tcp://127.0.0.1:0`).  Distinguisher-value assertion (`offsets_seen=0,100`) transfers unchanged.  Add new tests: `ZmqE2E_OneToOne_ProducerBinds`, `ZmqE2E_FanIn_SecondConsumerRejected`, `ZmqE2E_FanOut_SecondProducerRejected`, `ZmqE2E_OneToOne_SecondSideRejected`.
- [ ] L4 SHM 1-to-1 test — `test_plh_hub_role_shm_e2e.cpp` add `ShmE2E_OneToOne_SingleConsumer` + cardinality-rejection variant.

**Blocking:** Phase E retirements complete.

**Success criterion:** Demos run.  L4 tests green.  No pid-based port workarounds remaining.

---

### Phase G — Fan-out ZMQ (optional, may defer)

If we want ZMQ fan-out (currently only SHM supports fan-out), add PUB/SUB
support.

- [ ] `src/utils/hub/hub_zmq_queue.cpp` — PUB/SUB socket paths + subscribe on connect.  Handle "slow joiner" per §13 open question.
- [ ] `tests/test_layer4_plh_hub` — Add `ZmqE2E_FanOut_OneProducerTwoConsumers` scenario.
- [ ] Demo — Add `share/py-demo-fan-out-zmq/` example.

**Blocking:** Phase F complete.

**Success criterion:** ZMQ fan-out works end-to-end with 1 producer + N consumers.

---

### Phase H — Full verification

- [ ] `ctest -j2` — full sweep passes.
- [ ] SHM fan-out (HEP-CORE-0041) — verify unaffected via existing L4 SHM tests.
- [ ] HEP-CORE-0044 AttachProtocol L2 tests — verify unaffected.
- [ ] HEP-CORE-0045 broker observer — verify unaffected (still pending implementation per Line 3, but the design isn't invalidated).
- [ ] Cross-check demo runs manually.

---

## 3. Design questions — answered 2026-07-08

- **Q1** (fan-in channel death when consumer dies): ✅ intended per user — "no consumer, what's the point of fan-in?"
- **Q2** (PUB/SUB slow joiner): ✅ script decides via `api.peer_count()` accessor.  Framework doesn't baby-sit.  See [[feedback_framework_mechanism_not_policy]] in memory.
- **Q3a** (topology field default): ✅ REQUIRED, no default.  Missing → `INVALID_REQUEST`.
- **Q3b** (`confirmed_version` collapse): ✅ Phase B (early).  Single source of truth from day one.
- **1-to-1 topology added** (2026-07-08 user request) — distinct from fan-out because broker enforces exactly-one-consumer cardinality.

## 3.5. Still-open items (not blocking Phase A drafting)

1. **XPUB vs PUB for fan-out ZMQ binding side.**  XPUB gives socket-level subscription events, which the role host can bubble to `api.peer_count()` as wire-level truth (vs broker-level bookkeeping).  Draft lean: **XPUB**.  Confirm in Phase A.
2. **Script API accessor exact names.**  `api.peer_count()` unified vs `api.consumer_count()` + `api.producer_count()` split.  Under HEP-CORE-0028 amendment in Phase A.
3. **Recent commit fate.**  `2c604280` (multi-endpoint PULL) + `9d0ca4c8` (HEP-0021 §16 amendment) forward-migrate rather than revert.  Retirement of the multi-endpoint PULL code lands in Phase E; the §16 amendment gets rewritten in Phase A.

---

## 4. Files touched — index

Full breakdown in tech draft §11.  Summary:

- **Code (7 files, ~+1070/-1120 LOC net):** `hub_state.hpp/.cpp`, `hub_zmq_queue.hpp/.cpp`, `role_api_base.hpp/.cpp`, `broker_service.cpp`, `broker_request_comm.hpp/.cpp`.
- **Tests (7 files):** L2 broker (`test_broker_service.cpp`, `test_zmq_queue_auth.cpp`), L3 datahub (`test_datahub_zmq_endpoint_registry.cpp`), L3 pattern4 (`test_pattern4_attach_coordination.cpp` — delete, `test_pattern4_registration.cpp`, `test_pattern4_heartbeat.cpp`), L4 (`test_plh_hub_role_zmq_e2e.cpp`).
- **Demos (all `share/py-demo-*-zmq/`):** flip bind direction, add topology.
- **HEPs (7):** 0007, 0017, 0021, 0023, 0033, 0036, 0042.

---

## 5. Rejection criteria

If any surfaces during review, redesign or reject:

- N×M topology becomes a requirement (design assumes N→1 or 1→N only).
- Producer identity is expected to include a stable data endpoint claim (fan-in producers under this design don't own endpoints).
- Broker's per-producer bookkeeping is expected to include endpoints for discovery mechanisms other than CONSUMER_REG_ACK.

---

## 6. Session hygiene

**When resuming this work in a future session:**

1. Read this file top-to-bottom.
2. Open the tech draft (`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`) at the same time — it's the design authority.
3. Check user approval status at top of tech draft.  If still DRAFT, DO NOT touch code — resume design discussion.
4. If ADOPTED, proceed to the next unfinished Phase checkbox above.
5. Cross-check current git log against Phase completion — a prior session may have advanced some checkboxes without updating this file.

---

## 7. Recent-work re-evaluation

Two commits from this session need to be re-evaluated once the migration
proceeds:

| Commit | Content | Fate |
|---|---|---|
| `2c604280` | HEP-CORE-0017 §3.3 multi-endpoint PULL fix | Vestigial once fan-in flips to Pattern A.  Delete in Phase E. |
| `9d0ca4c8` | HEP-CORE-0021 §16 amendment + cross-HEP consolidation | Reparametrize (not revert) in Phase A.  Same mechanism, "singular side" replaces "producer." |

Neither is being reverted immediately — the code still works and the L4
test still passes.  They're just being flagged as pending re-evaluation.

---

## 8. Change log

| Date | Change |
|---|---|
| 2026-07-08 | Initial creation of this TODO, tracking the singular-side migration design. |
