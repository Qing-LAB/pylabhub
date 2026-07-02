# Testing TODO

**Scope:** Test architecture, coverage gaps, new scenarios, regression
hardening.
**Source of truth for test pattern choices:**
`docs/README/README_testing.md` § "Choosing a test pattern".
**Authoritative test rigor + mocking + layering rules:** memory files
under `/home/qqing/.claude/projects/-home-qqing-Work-pylabhub/memory/`
(`feedback_test_rigor.md`, `feedback_test_outcome_vs_path.md`,
`feedback_no_mocks_via_observability.md`,
`feedback_test_layering_and_no_mocks.md`,
`feedback_tests_replicate_production_scenarios.md`).

---

## Test Design Principles (MANDATORY — apply to every new + reviewed test)

### Layer purpose (L1 / L2 / L3 / L4)
| Layer | Purpose | Allowed |
|---|---|---|
| **L1** | Pure-function unit | Direct function call.  No threads, no sockets, no SHM. |
| **L2** | Single class / module | Real production class instance.  Pattern 1+ (`BinaryLifecycleEnvironment`) or in-process workers; no broker. |
| **L3** | In-process integration | Real broker + role components in-process via `IsolatedProcessTest` + `SpawnWorker` (Pattern 3).  Cross-thread + cross-component contract verification. |
| **L4** | Real-binary subprocess | Drives the staged binaries (`plh_hub`, `plh_role`) via subprocess.  Verifies CLI / config-load / file-system paths.  **Data-pipeline coverage now lives in the demo framework** (`share/demo_framework/runner.py` + `share/py-demo-*/`). |

### Maximize real production modules; mocks only when absolutely necessary
Per `feedback_no_mocks_via_observability.md`: extend real classes
with observability hooks rather than writing parallel-production
scaffolding.  A mock is allowed only when it satisfies a narrow
protocol contract that the real class cannot construct cheaply in
the test environment.

### Tests pin path + timing + payload, not just outcome
Per `feedback_test_outcome_vs_path.md`: outcome-only assertions
(`EXPECT_TRUE(result.has_value())`) hide regressions where the
right outcome is reached via the wrong path.  Pin payload shape,
sequence, error_code AND outcome.  Add mutation-sweep verification
(flip the code under test in both directions; confirm the test
fails both ways).

### Tests replicate production scenarios
Per `feedback_tests_replicate_production_scenarios.md`: mirror the
real path.  No synthetic stress harnesses that diverge from how
production actually drives the code.

### No `SetUpTestSuite`-owned `LifecycleGuard` antipattern
Migration wave closed 2026-05-13 (21 files).  Two `SetUpTestSuite`
sites remain — `test_slot_view_helpers.cpp` (one-shot
`py::scoped_interpreter`) + `test_role_init_directory.cpp`
(idempotent module registration); both legitimate.  Any NEW test
must use Pattern 1+ (`BinaryLifecycleEnvironment`) or Pattern 3
(`IsolatedProcessTest` + `SpawnWorker`).

---

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/TESTING_TODO_completions.md`
(Wave-B M9 RoleHostFrame Q1+Q2+Q3 closure; N1 #83 L2 round-trip test;
task #44 demo framework — inventory pointer still in this file below).

## Test retirements / cross-layer migrations (CRITICAL — read before retiring any test)

Discipline: a test retirement is incomplete until its contract is
absorbed into a tracked replacement.  Each retirement entry below
lists (1) the retired test, (2) the contract it was the SOLE site
pinning, (3) the destination task ID, and (4) the explicit description
update made to that task on retirement.  When closing one of the
destination tasks, verify the absorbed contract is exercised by the
new test — coverage continuity is the retirement's load-bearing
guarantee.

| Date | Retired test (file:test) | Contract absorbed | Destination |
|---|---|---|---|
| 2026-06-27 | `test_datahub_hub_host_integration.cpp:HubHost_Shutdown_BreaksClientConnection` | HEP-CORE-0023 §2.5.3 "disconnect is terminal" — cross-process hub-death observability (libzmq shared-context CURVE quirk makes L3 impossible) | task #296 (L4 hub-death observability test) |
| 2026-06-28 | `test_datahub_broker_protocol.cpp:BrokerProtocolTest.ClosingNotify_DeliveredToProducerAndConsumer` | CHANNEL_CLOSING_NOTIFY fan-out to ALL channel members (both producer-side AND consumer-side BRCs after `broker.request_close_channel`) | task **#225** (Pattern 4 rung 8 `Pattern4ChannelNotifiesTest`) — description extended 2026-06-28 with explicit fan-out cardinality / dual-receipt / trigger-path requirements |
| 2026-06-29 | `test_datahub_broker.cpp:Sch_ConsumerCitationMatch` (worker `broker_sch_consumer_citation_match`) | Named-citation match: consumer's `expected_schema_id`+`expected_schema_hash` matches channel's stored hash → `CONSUMER_REG_REQ` succeeds (HEP-CORE-0034 §10.3) | **REINSTATED 2026-06-29** (REVIEW_C2 F2).  Earlier "duplicate" claim doesn't hold: `consumer_schema_id_match_succeeds` uses BrcHandle (production BRC client with implicit retry through CHANNEL_NOT_READY) while this test pins the raw_req wire-layer shape with explicit HEP-CORE-0036 §5.2 R6 heartbeat sequencing.  Two abstractions, two contracts; both stay.  **Lesson recorded in `feedback_test_retirement_tracked_handoff`:** when judging whether two tests cover the same contract, the abstraction level matters — wire-layer + high-level-client pairs are SIBLINGS not DUPLICATES. |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_StartHandlerThreads_DualHub_E2E` (worker `role_api_base_start_handler_threads_dual_hub_e2e`) | M4c multi-connection master/peer ctrl-thread spawn + per-connection `brc_for_channel` routing across 2 brokers (Wave-B M4c review follow-up) | task **#296** (extended scope — L4 hub-death observability now also pins dual-hub master/peer spawn + routing via `expect_log_sequence` markers on each hub's REG_ACK).  **Reason for L3 retirement:** dual `BrokerService` in one process violates HEP-CORE-0036 §7.1 single-pumper invariant (`ZapRouter::pump_one` PANICs on concurrent entry from two broker poll loops sharing one ZapRouter). |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_HubDead_PeerKeepsRoleAlive` (worker `role_api_base_hub_dead_peer_keeps_role_alive`) | A2: peer broker death must NOT trigger role-wide shutdown; `is_connection_alive(0)==true`, `(1)==false`, role does NOT call `request_stop` (HEP-CORE-0023 §2.5) | task **#296** (extended scope — see absorbed-contracts table in #296 description).  Same single-pumper-invariant reason. |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_HubDead_TransitionsPresencesToDeregistered` (worker `role_api_base_hub_dead_transitions_presences_to_deregistered`) | R3.3 (post-HEP §4.3.3 amendment 2026-06-16): peer-connection presences must transition `Registered → Unregistered` (not Deregistered); master presences unaffected | task **#296** (extended scope).  Same single-pumper-invariant reason.  Note the §4.3.3 state-name amendment: the original L3 test asserted `Deregistered`; the post-amendment expectation is `Unregistered` (Deregistered is reserved for voluntary DEREG_REQ). |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_HubDead_MasterExitsRole` (worker `role_api_base_hub_dead_master_exits_role`) | D1/D2 + audit S1: master broker death enqueues EXACTLY ONE HUB_DEAD msg with `is_master=true`, default_hub_dead requests stop with reason `hub_dead`, BRC `reconnect_disabled()==true` for both connections (HEP-CORE-0023 §2.5.3 no-reconnect) | task **#296** (extended scope).  Same single-pumper-invariant reason. |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_WaitForRole_DualHub_Fallthrough` (worker `role_api_base_wait_for_role_dual_hub_fallthrough`) | A3: Class-B (role-bound) queries iterate ALL connections; first present-true answer wins (HEP-CORE-0033 §18.3) | task **#229** (extended scope — Pattern4RoleIntrospectionTest now also pins Class-B fall-through dual-hub variant since `wait_for_role` is an introspection-surface call).  Same single-pumper-invariant reason. |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_SourceHubUid_Disambiguates_DualHub` (worker `role_api_base_source_hub_uid_disambiguates_dual_hub`) | C3: `IncomingMessage::source_hub_uid` populated per-connection so dual-hub processors can disambiguate notify origin (HEP-CORE-0023 §7 + HEP-CORE-0033 §19.2/§19.4) | task **#228** (extended scope — Pattern4BandsTest now also pins source_hub_uid dual-hub variant on BAND_JOIN_NOTIFY fanout). Same single-pumper-invariant reason. |
| 2026-06-29 | `test_datahub_role_state_machine.cpp:RoleAPIBase_DualHub_Heartbeat_PerPresence` (worker `role_api_base_dual_hub_heartbeat_per_presence`) | C2: `on_heartbeat_tick_` iterates `handler_->presences()` (per-presence) rather than `pImpl->channel` string-branching; dual-hub processor emits one heartbeat per (channel, role_type) row to the correct hub (HEP-CORE-0033 §19.3 step 3) | task **#299** (new task — Pattern4HeartbeatTest dual-hub variant; #223 single-hub already shipped).  Same single-pumper-invariant reason. |
| 2026-06-30 | `test_datahub_zmq_endpoint_registry.cpp:ZmqEndpointRegistryTest.ReqShape_SyncReqTimesOutOnNoReply` (worker `req_shape_sync_req_times_out_on_no_reply`) | HEP-CORE-0007 §12.2.1 sync-REQ timeout-path conformance: REG_REQ / CHANNEL_LIST_REQ / ENDPOINT_UPDATE_REQ all wait `timeout_ms` and return nullopt on no-reply (not fire-and-forget degeneration; not block-past-budget) | task **#307** — option (a) CURVE-capable silent ROUTER fixture OR option (b) L2 test against `BrokerRequestComm::do_request` with a mock socket.  **Reason for retirement:** test used a plain-TCP `StubBrcHandle` to drive BRC into the timeout branch; HEP-CORE-0035 §2 strict-CURVE causes `BrokerRequestComm::connect()` to refuse any cfg without `broker_pubkey` + KeyStore `role_identity` entry — the plain-TCP wire-level bypass is no longer legal. |
| 2026-06-30 | `test_datahub_role_identity_policy.cpp:RoleIdentityPolicyEnumTest.*` (4 TEST_F's: ToStrAllValues, FromStrKnownValues, FromStrUnknownFallsToOpen, ToStrFromStrRoundTrip) | Pure value-conversion contract for `role_identity_policy_to_str` / `_from_str` (HEP-CORE-0035 §1 placeholder mechanism enum↔string helpers) | **RE-LAYERED** (not retired): moved verbatim to `tests/test_layer2_service/test_role_identity_policy.cpp`.  **Reason for re-layer:** Pattern-1 unit tests; no broker, no `LifecycleGuard`.  L3-aggregate placement was historical.  Helpers themselves stay in production for `broker_service.cpp::check_role_identity` WARN logs + error responses (helpers survive #152 retirement of the policy enum itself).  Source: REVIEW_AUTH6_TestDisposition_2026-06-27.md File 10 Suite 1. |
| 2026-06-30 | `test_layer2_service/test_hub_state.cpp:HubStateChannelAccess.Opened_ShmSecret_Preserved` + `HubStateChannelAccess.Opened_Idempotent_DoesNotOverwriteShmSecret` | Round-trip + idempotent-preservation semantics of the broker-minted `ChannelAccessEntry::shm_secret` field from the SUPERSEDED AUTH-4 design (#164) — preserved on first open, not overwritten on re-open | **NO DESTINATION** — contract retired by design.  The `shm_secret` field was deleted in HEP-CORE-0041 1i-cleanup S3 (#275, commit `b1a2d9ce`).  SHM auth runs on the capability-fd handshake at L2 (HEP-CORE-0041 §5.5), not a broker-minted shared token; the wire never carried `shm_secret` after substep 1g (#254) closed CONSUMER_REG_ACK.  The idempotent-open semantic for the surviving `authorized_consumer_pubkeys` set stays pinned by `HubStateChannelAccess.OpenedThenClosed_Roundtrip` + `HubStateChannelAccess.ConsumerAuthorized_Idempotent`.  Inline Rule-6 doc-blocks in test_hub_state.cpp record the retirement at the original site. |
| 2026-06-30 | `test_layer2_service/test_hub_shm_queue_capability.cpp:ShmQueueCapabilityTest.SetCapabilityFd_RefusesWhenSecretAlreadySet` + `SetShmSecret_RefusesWhenCapabilityAlreadySet` | HEP-CORE-0041 D7 "single unified mechanism" mutual-exclusion guards between the legacy secret-based path and the capability-transport path on `ShmQueue` | **NO DESTINATION** — contract retired by design.  S3c (#275, commit `<this commit>`) deleted `ShmQueue::set_shm_secret` + the legacy state-machine half entirely; there is no "secret path" left to guard against, so mutual exclusion is satisfied trivially.  The surviving "refuse-from-Active" guard on `set_shm_capability_fd` stays pinned by Test 3 (`SetCapabilityFd_RefusesFromActive`).  Inline Rule-6 doc-block at the original test site records the retirement. |
| 2026-06-30 | `test_layer3_datahub/test_datahub_hub_queue.cpp` (25 TEST_F's: `DatahubShmQueueTest.*` + worker file `workers/datahub_hub_queue_workers.cpp` with 25 worker functions / 45 legacy-factory call sites) | ShmQueue scenario coverage exercised through the legacy `ShmQueue::create_writer(name, ..., shared_secret, ...)` + `ShmQueue::create_reader(name, shared_secret, ...)` factories: factory smoke (from_consumer / from_producer / create_factories), read/write acquire/commit/abort lifecycle, ring buffer scenarios (round_trip, multiple_consumers, latest_only, ring_wrap, discard_then_reacquire), flexzone (read/write/round_trip/bidirectional/no_flexzone), accessors (last_seq, capacity_policy, is_running), checksum mismatch handling, destructor safety, remap stub throws, error paths (empty_schema / nonexistent / zero_size_schema) | **Multi-destination** per the user-chosen retirement strategy (#275-S3b discovery 2026-06-30):  ① ShmQueue state-machine + mutual-exclusion + invalid-fd guards → L2 `test_hub_shm_queue_capability.cpp` (Tests 1-7).  ② DataBlock ring-buffer + slot-allocation + ring-wrap + draining + last_seq + checksum + discard/reacquire → L3 fd-source workers `datahub_slot_allocation_workers.cpp` + `datahub_c_api_draining_workers.cpp` + `datahub_c_api_slot_protocol_workers.cpp` (all migrated under #275-S2 to `make_fd_backed_pair`).  ③ End-to-end producer→consumer SHM data flow + flexzone bidirectional + multi-consumer state isolation → L4 `test_plh_hub_role_shm_e2e.cpp` (#258).  ④ Error-path nonexistent-name attach → moot under capability-transport (no names; the fd-source factory's negative paths are covered by L2 capability test refusals + L4 e2e WARN/ERROR contracts).  **Acknowledged scope gap:** the ShmQueue-API-level scenario tests (`shm_queue_round_trip`, `shm_queue_latest_only`, `shm_queue_ring_wrap`, `shm_queue_last_seq`) are not pinpoint-replaced — equivalent behavior surfaces at L3 (DataBlock layer below ShmQueue) and L4 (full e2e above ShmQueue); the ShmQueue layer itself is shimmed in production by `RoleAPIBase::build_{tx,rx}_queue` exclusively through `create_writer_standby` + `set_shm_capability_fd` + `start`, which is L2-covered.  Files masked from L3 CMakeLists 2026-06-30 with top-of-file Rule-6 doc-blocks; physical deletion in REVIEW-C (#276). |
| 2026-06-30 | `test_plh_hub_role_zmq_e2e.cpp:PlhHubCliTest.ZmqE2E_AuthorizedConsumerReceivesAllSlots` | AUTH-7 ZMQ-transport happy path: producer + consumer roles register over CURVE-authenticated CTRL, broker pre-confirms allowlist, consumer dials producer's ZMQ_CURVE endpoint, N slots flow producer → consumer | **DEFERRED** (test body written; `GTEST_SKIP() << "#246"` gate active).  Re-enable when **#246** ("HEP-CORE-0036 amendment — retrofit ZMQ to pre-confirm pattern") ships.  **Reason for deferral:** observed sequencing race — broker sends `CONSUMER_REG_ACK` (consumer dials producer) BEFORE the producer's CHANNEL_AUTH_CHANGED_NOTIFY → GET_CHANNEL_AUTH chain seeds the producer's PUSH allowlist with the consumer's pubkey.  libzmq's initial CURVE handshake fails on the empty allowlist; the consumer never receives data.  Producer allowlist update lands ~110 ms after consumer PULL goes Active.  Sibling `ZmqE2E_UnauthorizedConsumerDeniedByBroker` (deny path) works without depending on this gate and stays green.  Coverage handoff: #246 ships the pre-confirm fix and unskips this test. |

When proposing a future retirement: update this table FIRST, then
update the destination task's description, then delete the test.

## Recent Completions

- **2026-06-27 — #177 KeyStore fixture infrastructure shipped.**  `CurveKeyStoreFixture`
  RAII guard (`tests/test_framework/curve_test_setup.h`) constructs
  `SecureMemorySubsystem` + `KeyStore` and seeds identities under
  `"hub_identity"` + `"role.<uid>"` names per HEP-CORE-0040 §172.
  Validated by 5-mutation protocol sweep in
  `tests/test_layer2_service/workers/curve_test_fixtures_workers.cpp`.
  Migrated files this session: L3 `hub_host_integration` +
  `hub_lua_integration` + `hub_python_integration` workers.  Side-effect:
  L3 `HubHost_Shutdown_BreaksClientConnection` deleted (libzmq shared-
  context CURVE quirk; L4 replacement filed as #296).  Commits
  `6e819b73` + `db774840`.  Remaining L3 broker test migrations
  (broker_health, role_state_machine, broker_protocol, metrics,
  zmq_endpoint_registry) are AUTH-6 (#154) batch 2a/2b scope.

## Current Focus — Open coverage gaps

### HEP-CORE-0042 Phase 3a.4 — L4 end-to-end scenarios (2026-07-02) ⏳

Phase 3a shipped the producer-side emission chain for the HEP-CORE-
0042 ZMQ attach-coordination protocol.  L3 coverage is complete for:
- Broker-side handler flow (8 scenarios in
  `test_pattern4_attach_coordination.cpp`).
- Producer-side initial-REG emission via real producer (Test A:
  `Pattern4ConsumerLifecycleTest.IdleProducerYieldsTimeoutDrainToConsumer`).
- Timeout drain via real producer (same Test A).

**Gap:** happy-path with active-cycle producer + fast-path admit
observation.  Belongs at L4 rather than L3 because:
- The scenario needs a producer that actually iterates `run_data_loop`
  (so `cycle_ops::invoke_and_commit` runs, which calls
  `handle_channel_auth_notifies`, which drives the NOTIFY-triggered
  APPLIED_REQ emission).
- Every existing Pattern4 worker uses `install_heartbeat` + sleep;
  none run a real data-loop.  Adding one worker per scenario is
  linear cost.
- L4 uses the real `plh_role` binary with a Lua script.  One test
  infrastructure, many scenarios — happy path + kill-mid-flight +
  allowlist revoke + multi-consumer + re-registration all belong
  in the same L4 test bundle.

**Scope for the L4 follow-up:**

1. Happy path — real producer + real consumer, consumer attach
   observes broker's `event=AttachReqZmqFastPath` after producer's
   NOTIFY-triggered APPLIED_REQ.
2. Real producer dies mid-flight — consumer's attach observes the
   §5.4 producer-disconnect drain with `producer_not_live`.
3. Real producer registered but its cycle is paused (e.g. a slow
   Lua callback) — consumer observes the §5.4 timeout drain with
   `producer_did_not_confirm_within_budget` (extends Test A's
   coverage from a subprocess with no cycle to a subprocess with
   a slow cycle).
4. Allowlist revoke mid-attach — consumer's attach observes
   `consumer_not_in_channel_allowlist` even though the consumer
   had previously registered.

Filed as follow-up to Phase 3a rather than a Phase 3a.4 Test B
because the scope is naturally wider than "one more log-marker
assertion" — the same real-role infrastructure that enables Test B
also enables the other three scenarios above.

### HEP-CORE-0042 Phase 2.4b — post-review follow-up flake risks (2026-07-01) ⏳

Filed as follow-ups from the xhigh review of `tests/test_layer3_pattern4/
test_pattern4_attach_coordination.cpp` (5 test-adding commits `4693c83d..
6a7a7508`).  Reviewer flagged these as PLAUSIBLE-but-refuted — the
mechanisms are real but not confirmed to fire today.  Track so they
don't get lost when broker defaults or NOTIFY behaviour changes.

- **WaitPathTimeoutOnMissingAppliedReq — 6000ms budget is tight.**  Test
  polls consumer for 6000ms; broker's timeout drain fires at
  `producer_apply_wait_ms + heartbeat_interval` = 3000ms + 500ms = ~3500ms.
  Cushion is only 2500ms.  Under high `-j2` CI load or an ubsan/asan
  build with 3-5x slowdown, the sweep could easily slip past the poll
  budget → hard fail with misleading "sweep_pending_attach_timeouts_
  likely broken" diagnostic.  Fix: raise to `2 * producer_apply_wait_ms`
  budget OR pass an explicit shorter `producer_apply_wait_ms` config to
  the broker subprocess (requires a new pattern4_smoke.broker variant
  with config-override CLI).

- **WaitPathTimeoutOnMissingAppliedReq — broker default coupling.**
  Test's magic 6000ms is bound to `BrokerService::Config::
  producer_apply_wait_ms` default (3000ms).  If the default is ever
  changed (raised for production robustness, lowered for latency), the
  test either times out spuriously or runs stale.  No comment in the
  test says "coupled to broker default X".  Fix (compat): add a docstring
  block that names the source-of-truth constant so a grep for the field
  name finds this test.  Fix (proper): pass the config value explicitly
  and compute the receive budget from it.

- **WaitPathDrainOnProducerDisconnect — receive() doesn't filter frames.**
  After the DEREG, consumer's `receive()` asserts the FIRST frame is
  `CONSUMER_ATTACH_ACK_ZMQ` — no filter.  The sibling channel-close
  test at line 924-937 DOES filter past `CHANNEL_CLOSING_NOTIFY`.
  Asymmetric.  If HEP-CORE-0036 §6.5 ever gains a fan-out notification
  on producer-DEREG (e.g. "producer-count changed"), this test would
  flake by picking up the notification instead of the ACK.  Fix:
  apply the same receive-and-filter loop pattern used in the channel-
  close test.

None of these fire today (all 8 tests pass sub-second in normal
runs).  Address before adding new similar tests so the pattern is
established, or when a broker default is next touched.

### REVIEW_C2 follow-ups (deferred from 2026-06-29 review of commit b22313c4) ⏳

Three follow-up items deferred from the C2 review remediation chain
(commits 4a4ef967 / af3880ef / ded802ab / a2b7d739).  Each has a
strong reason to defer and a tracked plan; full context in
`docs/tech_draft/REVIEW_C2_2026-06-29.md`.

- **F10 — DONE 2026-06-29.**  Consolidated into
  `tests/test_framework/broker_test_harness.{h,cpp}` as the 2-param
  keystore-derived `make_reg_opts(channel, role_uid, [pid])` /
  `make_cons_opts(channel, role_uid, [pid])` + an explicit-pubkey
  overload (`make_*_with_explicit_pubkey`) for negative-path tests
  that DELIBERATELY inject a wrong-pubkey shape to pin the broker's
  UNKNOWN_ROLE / PUBKEY_MISMATCH rejection.  All 4 live worker files
  migrated; the obsolete 2-param-no-pubkey copy in masked
  `datahub_broker_health_workers.cpp` will be replaced when that
  file is unmasked under batch-2b.  Full ctest 2231/2231 green.

- **F13 — KILLED 2026-06-29 (designer decision).**  Test fixture's
  `make_test_hub_directory` writes hub.json twice (init_directory
  template + 3-field patch).  Not a correctness issue, just
  convenience.  Cost is ~40 ms per full broker-test run, no file
  contention (each test uses its own temp dir).  Revisit only if
  file contention or perf becomes a real concern.

- **F14 production-callers follow-up — `KeyStore::with_keypair_z85` rollout.**
  Test-side migration shipped in `ded802ab`.  Production sites still
  doing the 2-lookup pattern (`pubkey(name)` + `with_seckey(name, ...)`):
  - `src/utils/network_comm/broker_request_comm.cpp:520-526`
  - `src/utils/service/role_api_base.cpp:999 + :1012`
  - `src/utils/service/hub_host.cpp` (broker bind site — comment at line 187 mentions the pair)
  Defer reason: holistic grep + per-site review needed; not mechanical
  (some sites need `with_seckey` raw bytes for libsodium, not `with_seckey_z85`
  — those need a parallel `with_keypair` raw-bytes variant).
  Acceptance criteria: grep `key_store\(\).pubkey\|with_seckey` for any
  remaining pair-pattern sites, migrate each, decide whether to add
  raw `with_keypair` as a second overload.

### #296 — L4 hub-death observability test (HEP-CORE-0023 §2.5.3 cross-process) ⏳

L3 test `HubHost_Shutdown_BreaksClientConnection` was deleted 2026-06-27 —
it cannot pin its contract in-process (libzmq's CURVE engine suppresses
`ZMQ_EVENT_DISCONNECTED` on clean peer close under a shared
`zmq::context_t`).  The contract being pinned is HEP-CORE-0023 §2.5.3
"disconnect is terminal" — a cross-process property.

L4 replacement spawns `plh_hub` + `plh_role` as separate processes,
kills `plh_hub`, asserts the role observes the disconnect within the
heartbeat-timeout window.  Full requirements in task #296 (description
includes the contract, the mandatory overlap-audit step against existing
L4 tests, and the merge / revise / create-new decision flow).

Closure analysis (libzmq shared-context quirk + designer call) archived
at `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
§"Phase 1 known bugs surfaced during landing — BRC monitor CURVE
blindspot".

### Pattern 4 test ladder — multi-process wire-protocol coverage

Pattern 4 = subprocess-per-role + observing parent.  Canonical:
`docs/README/README_testing.md` § "Pattern 4 — ...".  The ladder
rungs each pin one HEP contract clause, with focused failure
diagnostics per rung.

- **Rung 1 — `Pattern4SmokeTest`** ✅ shipped (task #220).
  Broker CURVE bind + role CURVE connect; sabotage-verified.
  Reference impl at `tests/test_layer3_pattern4/`.
- **Rung 2 — `Pattern4RegistrationTest`** ⏳ task #221.
  Pins `REG_REQ`/`REG_ACK` wire shape + Presence FSM
  `Unregistered → RegRequestPending → Registered`.
- **Rung 3 — `Pattern4HeartbeatTest`** ✅ task #223.
  Pins four axes: REG_ACK `heartbeat_interval_ms` negotiation,
  broker first-tick observability, rate-band cadence
  (±25% on a 2 s measurement window, CI 4 s), and role-sent ↔
  broker-received symmetry.  Role-side counter via shutdown
  summary `[role.x] heartbeat counter: sent=N over Mms`;
  broker-side counter via test-fixture periodic snapshot
  thread reading `state.snapshot()` (no per-tick production log).
- **Rung 4 — `Pattern4ConsumerLifecycleTest`** ✅ task #222.
  Pins `CONSUMER_REG_REQ`/`ACK` + consumer channel
  `Standby → master_approval → Active` across 3 subprocesses
  (broker + producer with bound tx queue + consumer).  7-marker
  forward-only sequence covers consumer FSM transitions, broker
  REQ accept + ACK send, and rx-queue Standby→Configured→Active.
  Shipped 2026-06-15 (see commit log) — closed alongside the
  Z85PublicKey lib bug fix (#231) the test surfaced.
- **Rung 5 — `Pattern4ProducerLifecycleTest`** ⏳ blocked on
  task #162 (AUTH-2).  Producer PUSH channel `Standby→Active`.
- **Rung 6 — `Pattern4DataFlowTest`** ⏳ blocked on task #163
  (AUTH-3).  `Authorized` + data-loop guard + payload.
- **Rung 7 — `Pattern4DeregistrationTest`** ⏳ task #224.
  Pins `DEREG_REQ`/`ACK`, `CONSUMER_DEREG_REQ`/`ACK`,
  `DISC_REQ`/`ACK`/`PENDING` (HEP-0023 §2.2).
- **Rung 8 — `Pattern4ChannelNotifiesTest`** ⏳ task #225.
  Pins fire-and-forget notify family (`CHANNEL_CLOSING_NOTIFY`,
  `CHANNEL_ERROR_NOTIFY`, `CONSUMER_DIED_NOTIFY`,
  `CHANNEL_EVENT_NOTIFY`, `CHANNEL_PRODUCERS_CHANGED_NOTIFY`).
- **Rung 9 — `Pattern4RegistrationErrorTest`** ⏳ task #226.
  Pins REG_REQ rejection paths (bad pubkey, bad uid claim,
  length violation; HEP-0036 §I10 / §I1 negative cases).
- **Rung 10 — `Pattern4AuthUpdateTest`** ⏳ task #227, depends
  on rung 5.  Pins `CHANNEL_AUTH_CHANGED_NOTIFY` →
  `GET_CHANNEL_AUTH_REQ`/`ACK` (HEP-0036 §6.5).
- **Rung 11 — `Pattern4BandsTest`** ⏳ task #228.  Pins
  `BAND_*_REQ`/`ACK`/`NOTIFY` family (HEP-0033 §12).
- **Rung 12 — `Pattern4RoleIntrospectionTest`** ⏳ task #229.
  Pins `ROLE_PRESENCE_REQ`/`ACK`, `ROLE_INFO_REQ`/`ACK`,
  `SHM_BLOCK_QUERY_REQ`/`ACK`.
- **Rung 13 — `Pattern4HeartbeatTimeoutTest`** ⏳ deferred.
  Pins HEP-0023 §2 dead-detection + recovery FSM.  Needs
  back-channel pause/resume signal in Pattern 4 helpers.

**Off-ladder** until blockers clear: federation
(`HUB_PEER_HELLO_ACK`, `HUB_RELAY_MSG`, `HUB_TARGETED_MSG` —
tasks #75 + #105); reserved/undecided (`SCHEMA_REQ`/`ACK`,
`METRICS_REQ`/`ACK` — task #95).  `ENDPOINT_UPDATE_REQ`/`ACK`
has L3 coverage from #90–#92; Pattern-4 rung only if a
multi-process gap surfaces.

Verification floor (mandatory for every rung): four axes
(sequence + timing + payload + state) + mutation discipline.
Logging discipline: INFO is one-shot only; hot paths get
counters or consequence-pinning.

### Test-faithfulness lesson from #270 layer pivot (2026-06-25)

The HEP-0041 1i-coverage (#270) effort initially shipped an L2 test
that subclassed `RoleHostFrame` with a `RoleHostFrameTestShim`
exposing protected `prepare_tx_capability_` + `spawn_shm_auth_listener_`
+ `cleanup_tx_capability_` as one-line public forwards (commits
`8716d91a` + `31b1d2af`).  Designer review surfaced the smell: the
shim's `worker_main_` override mirrored production's
`teardown_infrastructure_` ordering, making it a **parallel production
scaffold** rather than a test of production.  Reverted same day
(`e8ca91b5`).

**Critical clarification — the contracts are not dropped, only the
layer moved.**  The L2 tests were removed because the methods can
only be invoked correctly inside an L4 environment (no parallel
production scaffold needed), NOT because the contracts they were
going to verify are unnecessary.  Every contract is still required;
each is now embedded inside #258 L4 via production marker grepping
(`event=ShmCapabilityTransportBound`, `event=ShmAcceptLoopSpawned`,
`event=AttachAuthorized`, `event=AttachDenied`,
`event=ShmAttachOrchestratorReleased`,
`event=ShmCapabilityTransportReleased`, plus broker + consumer
markers).  Complete checklist with source lines in #258 task
description — implementer MUST pin each via `expect_log_sequence`.

Generalisable rule (candidate addition to README_testing.md §
"Mocking discipline"):

> When the methods you want to test are setup-step contributors to a
> multi-process flow — not unit-testable behaviors in isolation —
> the natural test unit IS the multi-process scenario, not the
> individual method.  Building a thin subclass with one-line forwards
> "just to invoke the methods" is a parallel production scaffold,
> even when each forward is mechanically trivial.  The scaffold's
> own lifecycle (worker_main_, teardown ordering, fake api
> construction) becomes test code with no production analogue —
> precisely what test-faithfulness §2 says to avoid.  Test at the
> layer where the methods actually do meaningful work.
>
> **Layer migration ≠ contract drop.**  When folding lower-layer
> tests up into a higher-layer test, every contract the lower-layer
> test was going to verify MUST be explicitly embedded as a
> production-marker pin (`required_substrings` /
> `expect_log_sequence`) at the upper layer.  Otherwise the upper
> layer's happy-path "did it work end-to-end" assertion silently
> drops the specific invariants the lower-layer test cared about.
> Build the marker checklist as part of the layer-migration
> decision, not as a follow-up.

Promotion path: when a second instance of this antipattern is
identified, fold the rule into README_testing.md § "Mocking
discipline" with both incidents as precedent.

### B8 (#81) — Demo-setup numpy pin

Demos that use numpy currently rely on ad-hoc `pip install numpy`.
Add `requirements.txt` per demo dir + chain
`plh_pyenv install --requirements <demo>/requirements.txt` into
`setup_commands`.

### N8 + N9 (#88) — Bench variants

- **N8** scalar / no-fill bench — pure dispatch overhead measurement.
  Current 4 KB-with-random bench measures fill cost as much as
  dispatch cost.  Strip random-fill to isolate per-slot framework
  cost (acquire + commit + checksum + log-flag check) across engines.
- **N9** multi-slot-size sweep — rerun the three-engine bench at
  slot sizes 16 B / 4 KB / 64 KB / 1 MB to characterize per-slot-
  size cost curve.  Reveals where checksum dominates vs script
  work.

### N11 (#89) — Cross-engine on_band_message signature parity audit

`Native` signature was wrong in my bench plugin this session
(used 3 separate `const char *` args; correct is
`const plh_band_message_args_t *args`).  Audit Python + Lua signatures
to confirm they're documented unambiguously and pinned by tests.
Add explicit doc note + L2/L3 regression that fails if the engine-
side dispatch signature changes.

### #93 — PlhRoleInitTest.InitOutputValidates/producer 60s hang

Flake-rate 1/N on CI: validate subprocess runs 60s without exiting,
parent kills with SIGTERM (143).  Last visible stderr line is
"Switching log sink to: RotatingFile..." — subsequent logs go to
the per-role file which the test harness can't read post-failure.

Hang is in worker thread's `worker_main_` (parent blocks on
`EngineHost::startup_()`'s `ready_future.get()`, `engine_host.cpp:148`).
Producer's validate-path skips `setup_infrastructure_` (no broker
connection) so this is NOT related to the 2026-05-21 ENDPOINT_UPDATE
sync REQ/REP change.

Candidate hang sites:
1. `scripting::engine_lifecycle_startup` — Python script load.  Slow
   imports?  GIL handoff bug between main (Py_InitializeFromConfig)
   and worker (PythonGilLease)?
2. `engine.finalize()` in validate-only exit path.
3. `cli::get_password` blocking on stdin (no TTY) — unlikely; init
   template generates `auth.keyfile = ""` and the unlock path is
   gated on non-empty.

**Cheap first-step diagnostic** — instrument the validate path: add
a log line at each major step entry/exit in
`producer_role_host.cpp::worker_main_` (Step 0 engine ctor / Step 1
schema / Step 2a api wiring / Step 4 engine_lifecycle_startup
entry+exit / engine.finalize() entry+exit).  The next CI flake will
pinpoint which step hangs.  ~10 LOC; risk-free.

Effort: M (diagnosis + likely fix).

### Open coverage items from 2026-05-20 discovery + 2026-05-05 HubAPI coverage plan (migrated 2026-06-02)

Carried over from archived `DISCOVERY_2026-05-20.md` §3.3 and
`HUB_TEST_COVERAGE_PLAN.md` (both under
`docs/archive/transient-2026-06-02/`).  Verify each before adding;
some may have been closed by Wave-A.5 / band-authority / HEP-0039
test work already.

**Broker / band protocol gate-mutation tests** (typo in
`expected_tags` initializers compiles silently today; one focused
commit can cover these):
- DEREG_REQ — expects `{prod, proc}`.
- CONSUMER_DEREG_REQ — expects `{cons, proc}`.
- HEARTBEAT_REQ — tag derived from `role_type`; mismatch path not pinned.
- ROLE_INFO_REQ + ROLE_PRESENCE_REQ — `{prod, cons, proc}`.
- BAND_JOIN_REQ + BAND_LEAVE_REQ + BAND_BROADCAST_REQ — `{prod, cons, proc}`.

**S4 broker-side band tests** (3 missing):
- `BAND_BROADCAST_REQ` sender-not-member drop (`broker_service.cpp:4685-4697`).
- `BAND_LEAVE_REQ` `NOT_A_MEMBER` typed-error path (`broker_service.cpp:4605-4622`).
- `BAND_BROADCAST` band-doesn't-exist drop (`broker_service.cpp:4677-4684`).

**S4 role-side bookkeeping tests** (3 missing):
- `band_join` on `{status:error}` → erase index entry (`role_api_base.cpp:1551-1560`).
- `band_leave` on `{status:error}` → erase index entry (`role_api_base.cpp:1597-1605`).
- `mark_connection_disconnected` band_index_ sweep returning `bands_lost` (`role_handler.cpp:337-349`).

**L3 / engine-parity gaps:**
- L3 hub-dead → `on_band_lost` cascade end-to-end (`role_api_base.cpp:1166-1176`).
- Real-engine parity tests for the 4 band callbacks (Python / Lua / Native) and `on_hub_dead`.
- `api.is_in_band()` — untested at any layer.
- **Cross-role observation surface parity test** (task #232) — per
  HEP-CORE-0011 §"Cross-Engine Surface Parity" Read-only observation
  surface principle (added 2026-06-15): each engine MUST keep one
  parity test that exercises every observation surface
  (`allowed_peers`, `producers`, `band_members`, `is_channel_ready`,
  `queue_mechanism`, etc.) on every role kind and pins both the
  wrong-side sentinel shape AND the right-side empty-but-correct
  shape.  Without this, new surfaces silently degrade the principle
  — Native ABI missing `producers` after AUTH-1 was the inciting
  incident.  When a new row is added to the HEP-0011 surface table,
  the parity tests must extend to cover it.

**HubAPI Phase 8c L2 surface — completely untested** (from 2026-05-05
coverage plan; pre-host-wiring fallbacks + happy-path delegation):
- `post_event(name, data)` — name validation + enqueue + fire-and-forget.
- `augment_query_metrics` / `augment_list_roles` / `augment_get_channel` /
  `augment_peer_message` — `has_callback` probe + `invoke_returning`
  routing + return-value capture + null/error fallback.
- `augment_timeout_ms()` / `set_augment_timeout(ms)` — atomic load/store
  + project-convention values (-1 / 0 / >0).
- Phases 8a/8b read accessors + control delegates — currently only
  exercised through L3 integration tests; should pin at L2.

**Hub Lua/Python integration gaps** (extend
`test_hub_lua_integration.cpp` + `test_hub_python_integration.cpp`):
- Augmentation hook flow (`on_query_metrics` mutates response).
- `post_event` / `on_app_<name>` dispatch (W-thread drain).
- Augmentation timeout path (slow callback + `set_augment_timeout(50)`).
- Event observers — none of the 11 §12.2.1 observers have an
  end-to-end fire-from-real-broker test.
- Control delegates from `on_tick` (e.g. `api.close_channel`).

**L3 federation + admin-RPC-over-wire:**
- `HUB_TARGETED_MSG` peer wire frame end-to-end (unblocks deferred
  `on_peer_message` augment hook).
- L3 admin-RPC-over-wire (token gate, response shape, error
  marshaling, timeout) — currently L2 (in-process REP) only.

**L4 lifecycle gaps:** several of the 2026-05-05 plan's L4 items
(`plh_role`/`plh_hub` run-mode lifecycle, broker round-trip, channel
broadcast, processor pipeline, hub-dead detection, admin-RPC-over-wire)
are subsumed by the demo framework + 9 demo manifests under
`share/demo_framework/` (task #44 — closed 2026-05-26).  Audit
which scenarios are NOT exercised by demo manifests; pin those.

### Code-review-deferred items from earlier sweeps

- **2026-05-03 `PlhRoleCliTest.LogBackupsBelowSentinelRejectedByValidate`
  parallel-load flake** — open, low priority; framework concern,
  not the test itself.
- **Native engine — user-settable `supports_multi_state()`**
  (2026-04-20) — open feature for native plugin opt-in to
  multi-state mode (script-spawned worker threads).

### Phase D — high-load and edge cases (long-running)

Long-running edge cases that aren't blocking but should land
eventually.  Items tracked under HEP-CORE-0033 phase scope; see
`docs/code_review/REVIEW_TestAudit_2026-05-01.md` § Phase D for
the resume bookmark.

---

## Test infrastructure inventory

### Demo framework (`share/demo_framework/runner.py`) — closes harness task #44

Delivers the L4 data-pipeline coverage that earlier plans tracked as
"L4 processor + consumer test infrastructure (Wave-D)".  Provides a
manifest-driven harness with built-in evaluators (`all_processes_
started`, `log_marker_present`, `log_marker_min_count`,
`no_unexpected_errors`, `clean_shutdown`, `count_sequence_e2e`).
Demo dirs under `share/py-demo-*/`:

* Python single + dual hub × SHM + ZMQ (4 demos)
* Lua single-hub SHM (`py-demo-lua-single-hub`)
* Native single-hub SHM (`py-demo-native-single-hub`)
* Hub-side Native (`py-demo-hub-native`) — validates the
  hub_script_runner B12+B13 fixes end-to-end
* Three-engine throughput bench (`py-demo-bench-{python,lua,native}`)

Use these as the L4 reference for any new pipeline scenario; clone
+ tweak rather than re-rolling the launcher.

### Multi-process test framework — `IsolatedProcessTest` + `SpawnWorker`

Pattern 3 mechanics canonical in
`docs/README/README_testing.md` § "Choosing a test pattern".
Pattern 4 (multi-process wire-protocol; subprocess-per-role +
observing parent) is the extension introduced for tests that
require honoring the HEP-CORE-0036 §7.4 single-pumper-per-process
invariant; helpers live in
`tests/test_framework/pattern4_helpers.{h,cpp}` and the smoke
reference at `tests/test_layer3_pattern4/test_pattern4_smoke.cpp`.

### Platform / sanitizer coverage

Tracked in `docs/todo/PLATFORM_TODO.md`.

---

## Phase checklist (high-level)

- **Phase A — Protocol / API correctness** ✅
- **Phase B — Slot Protocol (single process)** ✅
- **Phase C — Integration (multi-process)** ✅ (demo framework
  covers the long tail)
- **Phase D — High-load + edge cases** ⏳ ongoing

---

## Related Work

- `docs/README/README_testing.md` — test architecture + pattern
  selection.
- `docs/IMPLEMENTATION_GUIDANCE.md` § Test Strategy.
- `docs/code_review/REVIEW_TestAudit_2026-05-01.md` — resume
  bookmark for the test-correctness audit.
- `share/demo_framework/runner.py` — L4 demo-driven coverage.
