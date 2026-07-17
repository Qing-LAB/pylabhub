# Coverage Audit — Broker Protocols, Queue Operations, SHM/ZMQ CURVE Integration

**Date:** 2026-07-17 · **Status:** ✅ COMPLETE — audit + two security-test gaps fixed.

## Scope & method

A grounded coverage audit across the whole broker + queue + CURVE surface, to
confirm there is no missing gap or surprise in the test coverage of the auth /
admission / attach / queue machinery. Six dimensions were mapped independently,
each tracing **HEP contract → production handler (file:line) → test (file:line)
→ verdict** (COVERED / PARTIAL / GAP), with a "surprises" pass for code-vs-HEP
divergences:

1. Broker control-plane REQ/ACK (HEP-0007 / 0046 / 0023)
2. Channel attach coordination (HEP-0042 / 0041 / 0036)
3. SHM CURVE / capability transport (HEP-0041 / 0044)
4. ZMQ CURVE / admission ledger (HEP-0036 / 0035)
5. Queue operations (HEP-0002 / 0017)
6. Inbox / Band CURVE integration (HEP-0027 / 0030 / 0036)

All load-bearing findings below were re-verified against source by hand.

## Bottom line

**No hidden critical gap in the core auth/admission path.** The load-bearing
security mechanisms — admission ledger, ZAP allowlist, attach accept/deny, the
crypto handshake — are genuinely tested. The audit surfaced a catalogued set of
real gaps and a few code-vs-HEP divergences; two security-relevant test gaps
were fixed this session. The rest are either explicitly deferred (federation,
inbox CURVE) or defensive / low-severity, plus doc-hygiene divergences.

## What is genuinely solid (verified)

- **`VersionedAdmissionLedger`** — 30 unit cases incl. the #2480 over-confirmation
  clamp (adversarial `UINT64_MAX`, `versioned_admission_ledger.hpp:110-120`) and
  BIND-CONFIRM-1/2/3 (`test_versioned_admission_ledger.cpp:151/271/342`).
- **ZAP allowlist** accept/deny via *real* CURVE handshakes
  (`zap_router.cpp:366/547`, `test_zap_router.cpp:62/139`); **known_roles**
  admission + rejection (`broker_service.cpp:5643`, `test_known_roles.cpp:197`).
- **Broker attach accept/deny** — SHM covers all error codes
  (`INVALID_REQUEST`/`CHANNEL_NOT_FOUND`/`PRODUCER_NOT_AUTHORIZED`/denied,
  `test_pattern4_broker_consumer.cpp:531/581/682/708/742`); ZMQ fast-path +
  wait-path enqueue/drain/timeout (`test_pattern4_attach_coordination.cpp`).
- **Queue layer** — `hub::Queue` topology dispatch + SHM+fan-in reject (defended
  in triplicate), ZmqQueue/ShmQueue lifecycle, ZMQ envelope, DataBlock slot
  protocol, DRAINING, checksum-before-commit
  (`test_hub_queue_factory.cpp`, `test_hub_zmq_queue.cpp`,
  `test_hub_shm_queue_capability.cpp`, `test_datahub_c_api_slot_protocol.cpp`,
  `datahub_c_api_draining_workers.cpp`).
- **SHM handshake MAC-failure** (real impersonator,
  `test_attach_protocol.cpp:347`), **consumer-dial nullopt-vs-throw** contract
  (`:313/331/651`), **L4 SHM authorized data-flow + denied**
  (`test_plh_hub_role_shm_e2e.cpp:233/460`).
- **Band CURVE** — enforced on the broker CTRL ROUTER (see §Inbox/Band).

## Confirmed gaps — by bucket

### ① Security-relevant test gaps — FIXED this session

- **`MSG_CTRUNC` / malformed-cmsg rejection had zero tests.** The fail-closed
  guard added in REVIEW-C #276 (`shm_capability_channel.cpp:635`) was
  unverified. → **FIXED**: `ShmCapabilityChannelTest.RejectsMultiFdTruncatedScmRights`
  drives a 4-fd SCM_RIGHTS message through the from-socket factory. (Subtlety
  found: `CMSG_SPACE(int)`=24B holds 2 fds on LP64, so a 2-fd attack is caught
  by the strict `cmsg_len` check; 3+ fds trigger `MSG_CTRUNC` — both fail-closed.)
- **Mutual-auth Frame-3 wrong-key rejection was only weakly covered.**
  `MutualAuth_RejectsWrongProducerPubkey` (`test_attach_protocol.cpp:752`) — named
  the "PRIMARY THREAT MODEL" test — per its own comments routes AROUND Frame 3
  (fails at Frame 2) and asserts only a non-empty error, never the
  `attach_producer_not_authenticated` marker. → **FIXED**:
  `MutualAuth_RejectsFrame3PubkeyMismatch` reaches the real Frame-3 branch with a
  realistic adversary (KeyStore identity storing real `secA` but advertising
  `pubB`) and asserts the marker. Tightens the #262 claim.

### ② Real-framework E2E on timing-sensitive positive paths

- ZMQ *authorized* data-delivery E2E deferred for timing
  (`test_plh_hub_role_zmq_e2e.cpp:473-486`); only the deny path runs live.
- #2480 multi-producer over-confirmation race regression-pinned only at the
  ledger primitive (`test_versioned_admission_ledger.cpp:293`), not end-to-end
  (the L4 `MixedAdmitDeny` scenario is explicitly deferred, `:855-866`).
- SHM divergence-WARN has no L4 backstop — intentional, documented as the
  sanctioned L2 mock-inputs exception (`test_shm_attach_orchestrator.cpp:24-60`).
- `CHECK_PEER_READY_REQ` has no dedicated broker-wire test (only the ledger
  primitive + implicit L4); `phase=live` NOTIFY untested end-to-end (broker fire
  `broker_service.cpp:5273` + client dispatch `role_api_base.cpp:2317`);
  consumer-side D3 confirmation branch (`APPLIED_REQ` consumer,
  `broker_service.cpp:4498`, + its `NOT_A_ROLE_OF_CHANNEL` anti-poisoning guard)
  only implicit at L4.

### ③ Deferred subsystems (tracked, not accidental)

- **Federation control plane** — `handle_hub_peer_hello/bye/relay/targeted`
  handlers are live, but **every** federation test is `GTEST_SKIP`'d pending
  **#105** (`test_datahub_hub_federation.cpp:15-25`). Untested: unconfigured-peer
  NACK, reconnect, `HUB_RELAY_MSG` dedup, BYE→Disconnected.
- **Inbox CURVE** — deferred to HEP-0036 Phase 4+ / **#103** (see §Inbox/Band).

### ④ Defensive / error-code branches (low severity)

- Many REG_REQ reject codes tested only at HubState-unit level, not through the
  broker wire (topology codes, inbox codes: `broker_service.cpp:2198/2460/2487`,
  tested at `test_hub_state.cpp:2682+`).
- DISC_REQ `pending` / `CHANNEL_NOT_READY` branches untested — three-response
  model only 2-of-3 covered (`handle_disc_req:2929/2971`).
- `MISSING_ROLE_UID` untested for `ROLE_PRESENCE_REQ`/`ROLE_INFO_REQ`
  (`:6328/:6392`); `CONSUMER_DEREG` `INVALID_REQUEST` untested (`:3777`);
  HEARTBEAT silent-drop guards untested (`:5099/5116/5146`); orchestrator
  exception paths untested (`shm_attach_orchestrator.cpp:116/166/203`).

## Surprises — code vs HEP / comment (confirmed; doc bugs, not functional)

1. **`reason="attach_wait_path"` is documented but never emitted.** The wait-path
   NOTIFY payload (`broker_service.cpp:4389-4396`) sets only
   `channel_name/channel_version/role_uid/role_type/phase="admitted"`; the
   comment (`:4379`) and test comment claim a `reason` field that isn't there.
2. **`STALE_INSTANCE` returns a shaped ERROR frame** where HEP-0042 §5.4 specifies
   a silent drop (`broker_service.cpp:4558`); the test `StaleInstanceGuardOnAppliedReq`
   pins the *current* behavior, i.e. locks in the divergence (interim compromise
   pending "Phase 2.4 timer wiring").
3. **`CHECKSUM_ERROR_REPORT` emits `CHANNEL_EVENT_NOTIFY`**, not the
   `CHANNEL_ERROR_NOTIFY` HEP-0007 prose says (`broker_service.cpp:6151`);
   code+test agree, HEP diverges. `ChecksumRepairPolicy::Repair` is a no-op stub.
4. **`DISCOVER_CHANNEL_REQ` is a phantom** — a dangling comment
   (`broker_service.cpp:4282`) references a handler that does not exist;
   discovery is actually `DISC_REQ`.
5. **HEP-0046 §7.1 "CHANNEL_NOT_FOUND retires" is partial/asymmetric** — the REG
   side no longer emits it, but CONSUMER_REG (`:3222`) and DISC (`:2880`) still do.
6. **Stale phase-status comments** — `broker_service.cpp:4183-4207` marks Phase
   2.3b/2.3c "⏳ pending" though both are implemented and tested.

## Inbox / Band CURVE verdict (verified)

| Plane | Transport | CURVE? | ZAP / allowlist? | Broker admission? | Verdict |
|---|---|---|---|---|---|
| **Band** | broker CTRL ROUTER (BRC DEALER ↔ broker ROUTER) | ✅ `curve_server` + keys (`broker_service.cpp:1026`); BRC refuses non-CURVE (`broker_request_comm.cpp:729`) | ✅ ZAP `broker.ctrl.<uid>` allowlist (`:1083`) | ✅ member-check before fan-out (`handle_band_broadcast_req:7710`) | **Correct** |
| **Inbox** | own ROUTER/DEALER, direct role↔role, bypasses broker | ❌ **plaintext** — ROUTER `:247-250` (linger+rcvhwm+bind), DEALER `:515-522` (linger+routing_id+connect); zero CURVE refs; no external code arms it | ❌ none — `recv_one` reads the sender id from the self-asserted `routing_id` frame (`:63`), no admission | ❌ inbox never touches broker (discovery only) | **Gap (deferred)** |

- **Band: CURVE correctly enforced.** Two caveats: (a) the membership check
  compares a payload `role_uid` (set BRC-side from the role's own uid, not
  caller-supplied) rather than the CURVE-bound identity — forgeability
  **unconfirmed**, worth a follow-up; (b) the non-member-broadcast-*sender*
  rejection (`:7710`) has no test.
- **Inbox: CURVE not enforced, but a deliberate, documented deferral** —
  *verified in the actual code*: both the `InboxQueue` ROUTER and the
  `InboxClient` DEALER set only benign socket options (no `curve_server`, no
  keys), `recv_one` trusts the self-asserted `routing_id` as the sender id, and
  **no external code anywhere in `src/` arms the inbox socket with CURVE/ZAP**
  (grep returns only aspirational comments — `peer_admission.hpp:156` labels it
  "`InboxQueue` (CURVE+ZAP)" but the implementation contradicts it). HEP-0027
  Status: *"CURVE wiring deferred to HEP-CORE-0036 Phase 4+"*; §3.5: *"`hub_inbox_queue.cpp`
  has zero CURVE references today."* Default bind is loopback; a `tcp://0.0.0.0`
  inbox is reachable by any network peer, unauthenticated. **Task-number
  discrepancy to reconcile:** code comments cite **#191** (`hub_queue.hpp:227`)
  for the CURVE-wiring, HEP-0027 §3.5 cites **#103** — same deferred work, two
  different tracking ids.

## Actions taken & recommended follow-ups

**Done this session:** both ① security-test gaps fixed + committed
(`MutualAuth_RejectsFrame3PubkeyMismatch`, `RejectsMultiFdTruncatedScmRights`);
full ctest 2555/2555.

**Doc-hygiene divergences — FIXED 2026-07-17:**
- `reason="attach_wait_path"` comment claim removed (code never emits it) —
  `broker_service.cpp` + `test_pattern4_attach_coordination.cpp`.
- Phantom `handle_discover_channel_req` reference removed (`broker_service.cpp:4282`).
- Stale "Phase 2.3b/2.3c ⏳ pending" header corrected to ✅ shipped+tested.
- Inbox CURVE task-id reconciled: #191 is the inbox-specific task, #103 the
  reused rx-queue plumbing (HEP-0027 §3.5 updated to cite both).

**Flagged — needs a decision, NOT simple doc-hygiene:**
- **CHECKSUM_ERROR_REPORT emits `CHANNEL_EVENT_NOTIFY`** (`broker_service.cpp:6151`),
  which HEP-0007:1945 separately marks REMOVED, while HEP-0007:1471 says the
  checksum flow uses `CHANNEL_ERROR_NOTIFY`. Code + test agree on the "removed"
  name — a code/HEP contradiction that needs a design call (rename the emission,
  or un-retire the message), left untouched here.
- **CONSUMER_REG still emits `CHANNEL_NOT_FOUND`** (`broker_service.cpp:3222`)
  where HEP-0046 §7.1/§7.2 says the dialing side should PEND — tracked
  implementation work (HEP-0046 Phase B, **#57**), not a doc error.

**Recommended (not blocking):**
- Add the band non-member-broadcast-sender rejection test; investigate the band
  `role_uid`-forgeability question.
- Federation (#105) and inbox CURVE (#191) are legitimately deferred and tracked.
- The E2E-positive-path gaps are timing-sensitive and already noted in
  `TESTING_TODO`; close opportunistically.
