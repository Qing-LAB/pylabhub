# DRAFT: HEP-CORE-0041 Test Completeness Plan (v3.1)

**Status:** Tech draft — designer review (2026-06-23, v3.1 stale-text cleanup).
**Drives:** #275 (1i-cleanup), #270 (1i-coverage), #257 (1j L3 broker tests),
#258 (1k L4 e2e), and the test side of #262 (mutual auth).
**Author intent:** *test completeness goes ahead of coding* — pin the
expected behavior + outcome for every surface the new design exposes,
classify every existing test against the new design, then write code
to match the documented contract.

**v3.1 changes from v3** (stale text from v3 drop/split — no semantic changes):
- Updated stale "§1.1 (f)" references to "§1.1.B" after the §1.1 split.
- §2.1 cluster label clarified (combined surfaces share one test file).
- §3.5 body replaced — was still describing the v2 docstring fix that v3 dropped.
- §5.3 REVIEW gates: removed `GAP-L3-C/D` references (both dropped in v3).
- §7 designer checkpoints: v2 → v3.1; removed docstring reference.
- §8 references: removed `GAP-L3-D` from REVIEW-B carry mapping.
- §5.2 `GAP-L3-B` target file corrected (`broker_admin_workers.cpp`, no `datahub_` prefix).

**v3 changes from v2** (all backed by actual code reads, not speculation):
- §1.1.B (f) `chmod 0700` confirmed by `shm_capability_channel.cpp:291`.
- §2.2 + §5.1: `datahub_e2e_workers.cpp` is honestly named — its file
  header reads "End-to-end multi-process integration test.  Pipeline:
  broker (in-thread) ← BRC → producer subprocess ← shm → consumer
  subprocess."  Producer and consumer are real subprocesses; only the
  broker runs in-thread.  The "in-process roundtrip" claim in v2 was
  wrong.  **The proposed docstring fix in #275-S2 is DROPPED.**
- §1.6 + §4.7 + GAP-L3-D: the "end-to-end dial budget within 2s" row
  in v2 was wrong.  Code-reading reveals a chain of separate timers,
  not one ceiling.  Rewritten as a **timer-chain inventory**; the
  contract that needs pinning (retry-loop wait < producer accept
  timeout) is already documented by code comment at
  `role_api_base.cpp:1013-1016` and verifiable by static analysis.
  **GAP-L3-D is DROPPED** (no clean runtime contract to pin without
  timing-brittle assertions).
- §4.2: "L2b challenge mismatch" was ⚠ in v2.  Reading
  `test_attach_protocol.cpp::RejectsTamperedCipher` confirms it flips
  the MAC byte — fails at MAC verify, never reaches the plaintext-vs-
  challenge equality check.  **Promoted from ⚠ to confirmed GAP.**
- §5.2 GAP-L3-C: dropped.  L2 covers L2b/L2c exhaustively (verified in
  v2); #258 1k covers the wired-up dial through real producer +
  broker.  A synthetic L3 dial test in between would be ~250 LOC for
  marginal additional value over L2 + L4 coverage.
- §5.2 LOC totals replaced with a prose disclaimer (the per-row
  estimates were guesses, not bottom-up sized).
- §1.1 split into 1.1.A (anonymous memfd — NO filesystem footprint,
  NO chmod, NO hardening) and 1.1.B (auth rendezvous socket — the
  thing that gets `chmod 0700`).  v2 lumped the two surfaces under one
  table and made it easy to misread "chmod" as applying to the SHM
  itself.  The auth-socket hardening exists; the SHM hardening does
  NOT (killed #245 was that obsolete path).

---

## 1. New-design behavior surfaces (what MUST be covered)

Sourced from HEP-CORE-0041 §5 (wire protocol) and §6 (cross-platform
abstraction).  Each row is a behavior that the production-readiness
gate (REVIEW-E #278) MUST be able to point at a test pinning it.

### 1.1 L1 — transport primitive

The HEP-CORE-0041 capability transport is TWO separate L1 surfaces.
**There is no SHM `chmod` and no filesystem hardening on the SHM
side** — the SHM is an anonymous memfd with no name (that's the whole
point; killed #245 was the now-unnecessary 0600 hardening of the
deleted named-SHM path).

#### 1.1.A — Anonymous SHM memfd (no filesystem footprint)

| Surface | Behavior to pin |
|---|---|
| `create_shm_capability_producer(bytes)` | (a) creates memfd via `memfd_create()`; (b) ftruncates to exact `bytes`; (c) `MFD_CLOEXEC` set; (d) rejects 0 bytes |
| `IShmCapabilityProducer::borrow_fd` | (a) non-owning view of the memfd; (b) caller MUST NOT close; (c) stable for instance lifetime |
| `IShmCapabilityConsumer::data` + `size` + `borrow_fd` | (a) span valid until destruction; (b) size matches producer ftruncate value |
| `MemfdConsumer` / `MemfdProducer` ctor RAII | (a) failure mid-construction closes any opened fds; (b) destructor closes owned fd + munmaps region |

#### 1.1.B — Auth rendezvous socket (Unix-domain socket on the filesystem)

This is the rendezvous point where the producer's listener accepts
consumer connections to run the `crypto_box` challenge-response.
After successful handshake, the memfd from §1.1.A is sent over this
socket via `SCM_RIGHTS`.  This socket file IS on the filesystem and
HEP §5.1 requires its parent dir 0700 + the socket itself 0700.

| Surface | Behavior to pin |
|---|---|
| `IShmCapabilityProducer::bind_endpoint` | (a) succeeds with valid `unix://` URI; (b) strips `unix://` scheme before bind; (c) `mkdir -p` parent at 0700; (d) probe-then-unlink stale socket file; (e) refuses to clobber live producer (probe-then-unlink, 1i-prod-hardening H3d); (f) `chmod 0700` on the SOCKET FILE (`shm_capability_channel.cpp:291`) — restricts the rendezvous endpoint to the owning uid |
| `IShmCapabilityProducer::accept_one` | (a) returns AcceptedPeer with valid `peer_socket_fd` + cred triple from `SO_PEERCRED`; (b) `nullopt` on timeout (NOT error); (c) throws on socket-closed / EAGAIN-exhaustion |
| **SO_PEERCRED fail-closed** | If the kernel cred-read fails, AcceptedPeer fields default to zero → downstream uid check fails any non-root expectation (HEP §6.3 docstring) |
| `IShmCapabilityProducer::send_capability` | Sends the §1.1.A memfd to the accepted peer via SCM_RIGHTS; caller owns + closes peer_socket_fd after return; producer retains its own memfd copy |
| `attach_shm_capability_consumer_from_socket(fd, timeout)` | (a) `recvmsg` + SCM_RIGHTS parse on the accepted-then-handshake-completed peer socket; (b) takes ownership of socket_fd; (c) closes socket_fd on every return path; (d) **rejects MSG_CTRUNC** (multi-fd attack defense); (e) rejects zero-size received memfd; (f) returns IShmCapabilityConsumer with mmap'd region |

### 1.2 L2b — challenge-response handshake (per-connection FSM)

| Surface | Behavior to pin |
|---|---|
| `AttachProtocolAcceptor::accept_one` happy path | accept → SO_PEERCRED check → Frame1 send → Frame2 recv → crypto_box verify → returns AuthenticatedConsumer |
| L2b SO_PEERCRED uid mismatch | uid mismatch closes socket + returns nullopt |
| L2b Frame 2 timeout | AwaitingFrame2 timer fires → socket closed |
| L2b Frame 2 malformed JSON | reject → close |
| L2b Frame 2 oversized payload (4 KiB DoS cap) | reject → close |
| L2b crypto_box MAC failure | Decrypting → Closed (not Verified) |
| L2b challenge mismatch | decrypted plaintext ≠ issued challenge → Closed |
| L2b protocol_version mismatch | reject → close |
| `initiate_consumer_handshake` consumer side | (a) strip `unix://` before sockaddr_un (REVIEW-B B1 — fixed); (b) returns nullopt on ECONNREFUSED + ENOENT (H3a race); (c) bounded retry budget exhausted → returns nullopt (NOT exception); (d) throws on protocol-level error |

### 1.3 L2c — orchestrator (broker pre-confirm + send)

| Surface | Behavior to pin |
|---|---|
| Outcome::Sent | accept → handshake → cache-or-broker confirm → send_capability → Sent |
| Outcome::DeniedByBroker | broker returns `denied` → DO NOT send → close peer socket → DeniedByBroker |
| Outcome::DeniedTransportFail | broker query returns nullopt → DO NOT send → DeniedTransportFail |
| Outcome::HandshakeFailed | L2b returns nullopt → HandshakeFailed |
| Outcome::Timeout | accept_one returns nullopt without peer → Timeout |
| Cache hit fast path | pubkey already in cache → skip broker query → send |
| Cache divergence WARN | broker `success` for pubkey NOT in cache → emit WARN + still send |

### 1.4 L3 broker handler

| Surface | Behavior to pin |
|---|---|
| `handle_consumer_attach_req` happy | authorized → `status=success` + echoed pubkey |
| `handle_consumer_attach_req` denied | not allowlisted → `status=denied` + `denial_reason` |
| `handle_consumer_attach_req` errors | missing field → INVALID_REQUEST; unknown channel → CHANNEL_NOT_FOUND; non-producer caller → PRODUCER_NOT_AUTHORIZED; internal invariant → INTERNAL_ERROR |
| Envelope routing | success+denied → `CONSUMER_ATTACH_ACK`; error → `ERROR` |
| `handle_reg_req` `data_transport` gate | missing → INVALID_REQUEST; empty → INVALID_REQUEST; bogus → INVALID_REQUEST; `"shm"` + missing endpoint → INVALID_REQUEST; `"shm"` + valid endpoint → success; `"zmq"` + valid pubkey → success |
| **`CONSUMER_REG_ACK.shm_capability_endpoint` emission** | SHM channel ACK contains producer's endpoint + producer_pubkey_z85; ZMQ channel ACK does NOT contain those fields |
| **`REG_REQ.shm_capability_endpoint` parse + store** | producer's endpoint stored in HubState; used to populate CONSUMER_REG_ACK |

### 1.5 Integration — RoleHostFrame producer side (#270 scope)

| Surface | Behavior to pin |
|---|---|
| `prepare_tx_capability_` happy | mints L1 transport sized to `datablock_layout_total_size(cfg)`; bind_endpoint succeeds; sets `tx_opts.shm_capability_fd` |
| `prepare_tx_capability_` schema math | `datablock_layout_total_size` agrees with `ShmQueue::create_writer_standby` view byte-for-byte |
| `prepare_tx_capability_` ZMQ no-op | `has_shm=false` or `data_transport=="zmq"` → no-op |
| `spawn_shm_auth_listener_` lifecycle | thread via ThreadManager; `with_active_loop` bracket (EDGE-2 #280); terminates on role stop within `consumer_attach_timeout` ceiling |
| `cleanup_tx_capability_` ordering | transport teardown after queue teardown |

### 1.6 Integration — RoleAPIBase consumer side

| Surface | Behavior to pin |
|---|---|
| `apply_consumer_reg_ack` SHM branch dispatch | ACK with `shm_capability_endpoint` → routes to `_shm_`; ACK without it → ZMQ branch |
| `apply_consumer_reg_ack_shm_` happy | bounded retry connect; `initiate_consumer_handshake`; SCM_RIGHTS recv; `rx_queue->set_shm_capability_fd`; `start`; commit `Impl::shm_consumer` |
| **Timer-chain invariant** (was "End-to-end dial budget" in v2) | The consumer-side dial is a chain of separate timers, NOT a single budget: (1) retry-loop wait ~10 × 100ms ≈ 1s (`role_api_base.cpp:987-988`); (2) per-attempt handshake `kDialAttemptPeriod = 100ms`; (3) post-handshake SCM_RIGHTS recv 2s (`role_api_base.cpp:1042`); (4) ShmQueue::start negligible.  The contract per code comment at `role_api_base.cpp:1013-1016`: the retry-loop wait must stay under the producer-side accept timeout (2s, #280) so the producer doesn't give up while the consumer is retrying.  This invariant is verifiable by reading the two constants together; a runtime test would be timing-brittle.  No new test required. |
| Lifetime ordering | `shm_consumer` destroyed FIRST in reverse-declaration order; safe via rx_queue's internal fd dup (REVIEW-B medium fix) |
| Worker-thread context | Dial runs on `ConsumerRoleHost::worker_main_` Step 6 (NOT BRC poll thread); blocks up to ~3.1s today (1s retry + 100ms handshake + 2s recv; #282 / #283 future async work) |

### 1.7 DataBlock primitive — fd-source factories

| Surface | Behavior to pin |
|---|---|
| `create_datablock_producer_from_fd_impl` | wraps borrowed memfd; validates fstat size == `datablock_layout_total_size(cfg)`; dups internally; returns nullptr / throws on mismatch |
| `find_datablock_consumer_from_fd_impl` | attaches over borrowed memfd; **skips legacy shared_secret check** (cap-transport receipt IS the auth); validates schema hash; dups internally; returns nullptr on validation fail |

---

## 2. Layer doctrine + correctness check

Per `docs/README/README_testing.md` §2:

| Layer | Binary | Scope |
|---|---|---|
| L0 | `test_layer0_platform` | Platform basics |
| L1 | `test_layer1_*` | Pure utilities |
| L2 | `test_layer2_*` | Service-layer + standalone library tests (Logger, FileLock, Crypto, **ShmQueue**, **HubState**, **AttachProtocol**, **ShmAttachOrchestrator**) |
| L3 | `test_layer3_datahub` (single binary) | DataBlock primitive + role-host integration tests |
| L4 | `test_layer4_*` | Full role-host subprocess + broker subprocess (real lifecycle) |

### 2.1 Where each HEP-0041 surface BELONGS

| Behavior cluster | Target layer | Existing test file (confirmed) |
|---|---|---|
| L1 capability transport — anonymous memfd (§1.1.A) AND auth rendezvous socket (§1.1.B); two surfaces, one test file by historical co-location | L2 | `test_layer2_service/test_shm_capability_channel.cpp` ✅ (10 tests) |
| L2b challenge-response FSM | L2 | `test_layer2_service/test_attach_protocol.cpp` ✅ (9 tests) |
| L2c orchestrator | L2 | `test_layer2_service/test_shm_attach_orchestrator.cpp` ✅ (8 tests) |
| ShmQueue capability factories | L2 | `test_layer2_service/test_hub_shm_queue_capability.cpp` ✅ (7 tests) |
| L3 broker handler | L3 | `tests/test_layer3_datahub/test_datahub_broker_admin.cpp` (RegValidation) + `test_datahub_broker_consumer.cpp` (ConsumerAttach) ✅ |
| RoleHostFrame producer-side integration | L2 | **GAP — #270 scope, not yet started** |
| RoleAPIBase consumer-side dial | L3 | Indirect via post-S1 `role_api_flexzone_workers.cpp`; **no direct dial test** |
| DataBlock fd-source factories | L3 indirect via ShmQueue at L2 | Indirect via `test_hub_shm_queue_capability.cpp`; no direct C-API L3 test |
| E2E SHM auth-gated data flow | L4 | **GAP — #258 1k scope, not yet started** |

### 2.2 Layer-correctness of existing tests under #275 scope

No tests are at the wrong LAYER.  **No file is misnamed.**
`datahub_e2e_workers.cpp` was flagged in v1/v2 as "actually in-process";
v3 code-verification (reading the file header) confirms it IS a
genuine multi-process e2e test: producer + consumer subprocesses with
the broker in-thread.  The name is honest; no docstring or rename
needed.  Layer audit otherwise:

`datahub_producer_consumer_workers.cpp`, `datahub_transaction_api_workers.cpp`,
`datahub_schema_validation_workers.cpp`, `datahub_e2e_workers.cpp`,
`datahub_policy_enforcement_workers.cpp`, `datahub_handle_semantics_workers.cpp`,
`datahub_stress_raii_workers.cpp`, `datahub_recovery_scenario_workers.cpp`,
`datahub_integrity_repair_workers.cpp`, `datahub_c_api_checksum_workers.cpp`,
`datahub_config_validation_workers.cpp`, `datahub_hub_queue_workers.cpp`,
`role_api_raii_workers.cpp`, `role_api_loop_policy_workers.cpp`,
`role_api_flexzone_workers.cpp` — all **L3 correct**.

`datahub_write_attach_workers.cpp` — N/A (entire file retires).

---

## 3. Per-test classification (KEEP / MIGRATE / RETIRE)

### 3.1 Tests that RETIRE (Rule 6 — subject behavior disappears)

Confirmed by `TEST_F` enumeration (v2):

**Entire file retires — `datahub_write_attach.cpp` + `_workers.cpp`** (4 `TEST_F`):

1. `DatahubWriteAttachTest.CreatorThenWriterAttachBasic`
2. `DatahubWriteAttachTest.WriterAttachValidatesSecret`
3. `DatahubWriteAttachTest.WriterAttachValidatesSchema`
4. `DatahubWriteAttachTest.SegmentPersistsAfterWriterDetach`

Rationale: WriteAttach concept (hub creates / source attaches as
writer) disappears under HEP-CORE-0041 — producers own their own
memfds.  Zero production callers of `attach_datablock_as_writer_impl`
confirmed by grep.  The schema-validation aspect (sub-test 3) is
redundantly covered by `datahub_schema_validation_workers.cpp`.

**Standalone secret-gate tests** (2 tests):

5. `find_consumer_wrong_secret_returns_null` worker (in `datahub_producer_consumer_workers.cpp`).  Whichever `TEST_F` dispatches it — confirms via the wrong-secret return-null contract.  Pins a gate that disappears in #275-S4.
6. `shm_queue_create_reader_wrong_secret` worker (in `datahub_hub_queue_workers.cpp`).  Same — name-based `ShmQueue::create_reader` retires in #275-S3.

**Retirement total: 6 `TEST_F` entries across 3 files.** (Not "6 tests + 1 file" as v1 stated — the 4 WriteAttach tests ARE the file.)

### 3.2 Tests that MIGRATE (subject survives; receives fd instead of name)

Migration shape per file:

| File | # of test sites using legacy secret/name path |
|---|---|
| `datahub_producer_consumer_workers.cpp` | ~7 |
| `datahub_transaction_api_workers.cpp` | ~6 |
| `datahub_schema_validation_workers.cpp` | ~5 |
| `datahub_e2e_workers.cpp` | 2 |
| `datahub_policy_enforcement_workers.cpp` | ~10 |
| `datahub_handle_semantics_workers.cpp` | ~2 |
| `datahub_stress_raii_workers.cpp` | ~2 |
| `datahub_recovery_scenario_workers.cpp` | 1 helper-set |
| `datahub_integrity_repair_workers.cpp` | 1 helper-set |
| `datahub_c_api_checksum_workers.cpp` | ~2 |
| `datahub_config_validation_workers.cpp` | 1 helper-set |
| `datahub_hub_queue_workers.cpp` | ~1 (mechanism) |
| `role_api_raii_workers.cpp` | ~1 |
| `role_api_loop_policy_workers.cpp` | ~1 |
| `role_api_flexzone_workers.cpp` | DONE under #275-S1 |
| **Approx total** | **~40 sites** (precision ±5; exact count comes from #275-S2 commit diff) |

Each site swaps `(name, secret, cfg)` for `(label, fd, cfg)` against
the existing `create_datablock_producer_from_fd_impl` /
`find_datablock_consumer_from_fd_impl` factories.  Template sites
add `find_datablock_consumer_from_fd<F, D>` (~30 LOC header addition,
approved §3.4).

### 3.3 Tests that STAY untouched (enumerated)

The remaining 27 worker files in `tests/test_layer3_datahub/workers/`
are NOT on the migration list (do not use `shared_secret` /
`find_datablock_consumer*`):

`broker_admin_workers.cpp`, `broker_consumer_workers.cpp`,
`broker_schema_workers.cpp`, `datahub_broker_health_workers.cpp`,
`datahub_broker_protocol_workers.cpp`,
`datahub_broker_request_comm_workers.cpp`,
`datahub_broker_workers.cpp`,
`datahub_c_api_draining_workers.cpp`,
`datahub_c_api_recovery_workers.cpp`,
`datahub_c_api_slot_protocol_workers.cpp`,
`datahub_c_api_validation_workers.cpp`,
`datahub_channel_group_workers.cpp`,
`datahub_exception_safety_workers.cpp`,
`datahub_header_structure_workers.cpp`,
`datahub_metrics_workers.cpp`, `datahub_mutex_workers.cpp`,
`datahub_role_state_workers.cpp`,
`datahub_schema_blds_workers.cpp`,
`datahub_schema_loader_workers.cpp`,
`datahub_slot_allocation_workers.cpp`,
`hub_federation_workers.cpp`,
`hub_host_integration_workers.cpp`,
`hub_inbox_queue_workers.cpp`,
`hub_lua_integration_workers.cpp`,
`hub_python_integration_workers.cpp`,
`role_identity_policy_workers.cpp`,
`zmq_endpoint_registry_workers.cpp`.

If any of these turn out to indirectly depend on the legacy path
during S2 execution, surface it then; the enumeration above is the
working assumption.

### 3.4 Template wrapper decision

Add `find_datablock_consumer_from_fd<F, D>` template in
`src/include/utils/data_block.hpp` next to the existing
`find_datablock_consumer<F, D>` template.  ~30 LOC.  Justified by
the ~25 template call sites in §3.2; zero production users of the
legacy template confirmed by grep — symmetry argument holds.

### 3.5 ~~Rename / docstring on `datahub_e2e_workers.cpp`~~ **DROPPED in v3**

v1 proposed rename.  v2 replaced rename with a 5-line file-header
docstring "clarification."  **v3 dropped both** after code-reading the
file: its existing header literally reads "End-to-end multi-process
integration test.  Pipeline: broker (in-thread) ← BRC → producer
subprocess ← shm → consumer subprocess."  The name and existing
docstring are honest — no edit needed.

---

## 4. Coverage gap analysis (verified, not assumed)

For each behavior surface in §1, this section reports
**actually-confirmed** coverage from grepping the L2 + L3 test files.
"Confirmed gap" means I checked and the surface isn't pinned anywhere.

### 4.1 L1 transport (`test_shm_capability_channel.cpp`, 10 tests)

#### 4.1.A — anonymous memfd surface

| Surface | Status |
|---|---|
| `create_shm_capability_producer(0)` rejection | ✅ `ProducerCtorRejectsZeroSize` |
| memfd round-trip data visibility | ✅ `RoundTrip_ConsumerSeesProducerWrites` |
| **MemfdConsumer/Producer ctor RAII on partial failure** | ❌ **GAP** (REVIEW-B medium carry) |

#### 4.1.B — auth rendezvous socket surface

| Surface | Status |
|---|---|
| `bind_endpoint` scheme strip + `unix://` URI | ✅ `BindAndDialAcceptUnixSchemeURI` |
| `bind_endpoint` probe-then-unlink live | ✅ `BindRefusesWhenLivePeerHoldsThePath` |
| `bind_endpoint` probe-then-unlink stale | ✅ `BindCleansUpStaleSocketFile` |
| `bind_endpoint` empty path rejection | ✅ `BindEndpointReturnsFalseOnEmptyPath` |
| `bind_endpoint` idempotency / twice fail | ✅ `BindEndpointTwiceFails` |
| `bind_endpoint` mkdir -p parent | ✅ `BindCreatesMissingParentDirectory` |
| **`bind_endpoint` chmod 0700 on socket file** | ⚠ **POSSIBLY a GAP** — the code does it at `shm_capability_channel.cpp:291` but no test verifies the resulting socket-file mode via `stat()`.  Worth adding a small assertion (`stat(sock_path).st_mode & 0777 == 0700`) to one of the existing bind tests rather than a new test. |
| `accept_one` timeout = nullopt | ✅ `AcceptOneReturnsNulloptOnTimeout` |
| `consumer` throws on ENOENT endpoint | ✅ `ConsumerThrowsOnNonexistentEndpoint` |
| **SO_PEERCRED cred-triple populated** | ❌ **GAP — no direct test pinning AcceptedPeer.uid/gid/pid values** |
| **SO_PEERCRED fail-closed (cred-read fails → zeros)** | ❌ **GAP** |
| **`attach_shm_capability_consumer_from_socket` MSG_CTRUNC defense** | ❌ **GAP** (REVIEW-B medium carry) |
| **`attach_shm_capability_consumer_from_socket` zero-size received memfd** | ❌ **GAP** |

### 4.2 L2b challenge-response (`test_attach_protocol.cpp`, 9 tests)

| Surface | Status |
|---|---|
| Happy round-trip | ✅ `RoundTrip_HelloAndChallengeResponse` |
| Wrong seckey → MAC fail | ✅ `RejectsConsumerWithWrongSeckey` |
| Tampered cipher → MAC fail | ✅ `RejectsTamperedCipher` |
| Frame 2 timeout | ✅ `AcceptOneReturnsNulloptOnTimeout` |
| Wrong protocol_version | ✅ `RejectsHelloWithWrongProtocolVersion` |
| Missing required field | ✅ `RejectsHelloWithMissingRoleUid` |
| Oversized frame (4 KiB DoS) | ✅ `RejectsHelloOversizedFrame` |
| Malformed JSON | ✅ `RejectsHelloWithMalformedJson` |
| Consumer-side nullopt on absent endpoint | ✅ `ConsumerHandshakeReturnsNulloptOnAbsentEndpoint` |
| **L2b SO_PEERCRED uid mismatch close** | ❌ **GAP** |
| **L2b challenge mismatch (plaintext ≠ issued challenge)** | ❌ **CONFIRMED GAP** — v3 code-read of `RejectsTamperedCipher` shows it flips the MAC byte, so the test fails at `crypto_box_open_easy` MAC verify and never reaches the plaintext-vs-challenge equality check.  The equality branch is dead-path coverage. |
| **`initiate_consumer_handshake` strip_unix_scheme direct** | ❌ **GAP** (REVIEW-B B1 fixed without dedicated test) |
| **`initiate_consumer_handshake` bounded retry budget** | ❌ **GAP at L2** (only L3 integration covers it indirectly) |

### 4.3 L2c orchestrator (`test_shm_attach_orchestrator.cpp`, 8 tests)

| Surface | Status |
|---|---|
| Outcome::Sent | ✅ `BothAllowed_SilentAndSent` |
| Outcome::DeniedByBroker | ✅ `BothDenied_SilentAndDeniedByBroker` |
| Cache-divergence WARN (broker yes / cache no) | ✅ `BrokerAllowedCacheDenied_WarnAndSent` |
| Cache-divergence WARN (broker no / cache yes) | ✅ `BrokerDeniedCacheAllowed_WarnAndDenied` |
| Outcome::DeniedTransportFail (broker nullopt) | ✅ `BrokerNullopt_FailsClosed` |
| Outcome::DeniedTransportFail (broker unexpected status) | ✅ `BrokerUnexpectedStatus_FailsClosed` |
| Outcome::HandshakeFailed | ✅ `HandshakeFailure_FromImpersonator` |
| Outcome::Timeout | ✅ `TimeoutReturnsTimeout` |

**L2c is comprehensively tested. No gaps.**

### 4.4 ShmQueue capability path (`test_hub_shm_queue_capability.cpp`, 7 tests)

| Surface | Status |
|---|---|
| Writer Standby → set_fd → start → Active + write | ✅ Test 1 |
| Reader Standby → set_fd → start → Active + read producer data | ✅ Test 2 |
| `set_shm_capability_fd` refuses from Active state | ✅ Test 3 |
| `set_shm_capability_fd` refuses when secret already set (mutex) | ✅ Test 4 (deletes in #275-S3) |
| `set_shm_secret` refuses when capability already set (mutex) | ✅ Test 5 (deletes in #275-S3) |
| `set_shm_capability_fd` refuses negative fd | ✅ Test 6 |
| `mechanism()` returns ShmCapability after start | ✅ Test 7 |

**ShmQueue capability path is comprehensively tested.**

### 4.5 L3 broker handler

| Surface | Status |
|---|---|
| `handle_consumer_attach_req` happy + 4 error paths | ✅ `test_datahub_broker_consumer.cpp::ConsumerAttach*` (5 tests) |
| `handle_reg_req` `data_transport` gate (6 sub-cases) | ✅ `test_datahub_broker_admin.cpp::RegValidation_*` (6 tests post-#281) |
| **`CONSUMER_REG_ACK.shm_capability_endpoint` + `producer_pubkey_z85` field presence (SHM channels)** | ❌ **CONFIRMED GAP — verified by grep: `WireConformance_ConsumerRegAck_Shape` (test + worker) contains zero references to `shm_capability_endpoint` or `producer_pubkey_z85`** |
| **`CONSUMER_REG_ACK` field ABSENCE for ZMQ channels** | ❌ **Same — symmetric gap** |
| **`REG_REQ.shm_capability_endpoint` parse + store** | ❌ **GAP — not directly verified** |

### 4.6 RoleHostFrame producer-side (#270 scope)

`#270 1i-coverage` is the dedicated tracker.  All §1.5 surfaces are
gaps until #270 lands.  This plan defers to #270 rather than
duplicating its scope.

### 4.7 RoleAPIBase consumer-side

| Surface | Status |
|---|---|
| `apply_consumer_reg_ack` SHM branch dispatch | ⚠ Indirect via post-S1 `role_api_flexzone_workers.cpp` flow.  No direct dispatch assertion exists, but the integration coverage at L4 (#258 1k) will exercise the dispatch on the real wired-up path.  Disposition: **no new L3 test needed**; covered between post-S1 indirect + #258 e2e. |
| `apply_consumer_reg_ack_shm_` happy path | Same as above |
| Timer-chain invariant | ✅ **Static-only contract.**  Per v3 reanalysis (see §1.6 row), the contract is "retry-loop wait < producer-side accept timeout" and is verifiable by reading two constants in `role_api_base.cpp` next to each other.  No runtime test required; the REVIEW-B "budget allocation" medium item was speculative without a concrete failure scenario. |
| Lifetime ordering | Documented at `role_api_base.cpp:108-135` (REVIEW-B medium fix); runtime check needs introspection; deferred unless safety concern arises |

### 4.8 DataBlock fd-source factories (C API direct)

Indirect coverage via ShmQueue at L2.  Direct C-API test would
duplicate ShmQueue coverage; arguably not needed.  Optional — would
only matter if ShmQueue is bypassed in some future path.  **Default
disposition: don't add (avoid redundant coverage).**

### 4.9 L4 e2e (#258 1k scope)

Entire #258 scope is the L4 e2e plan.  This document defers to #258.

---

## 5. Action plan — mapped to staged commits

### 5.1 #275 staged commits

| Stage | Status | Scope (v3) |
|---|---|---|
| #275-S1 | ✅ 2026-06-23 (`9218ac31`) | role_api_flexzone_workers 7 tests migrated |
| #275-S2 | ⏸ | Add `find_datablock_consumer_from_fd<F,D>` template + migrate ~40 sites across 14 files + retire 6 tests (4 in WriteAttach file + 2 standalone secret-gate).  NO file rename, NO docstring item (v3 dropped). |
| #275-S3 | ⏸ | Delete role-layer secret machinery (per AUTH_TODO original) |
| #275-S4 | ⏸ | Delete C API secret param.  Migrate `data_block_recovery.cpp:707` to fd-source.  Delete `attach_datablock_as_writer_impl` impl entirely. |
| #275-S5 | ⏸ | Core Structure Change: rename `SharedMemoryHeader::shared_secret[64]` → `reserved_capability_token[64]`.  Walk Core Structure Change Protocol checklist first. |

### 5.2 New coverage commits (gaps + sequencing)

These add tests for confirmed §4 gaps.  All are sequenced before
REVIEW-C (#276); v3 removed two items (GAP-L3-C, GAP-L3-D) that
turned out to be poorly motivated (see §1.6 + §4.7 rewrites).

Rough-size column is an order-of-magnitude indicator only — each row
gets bottom-up sized when its commit is drafted.  Don't add the
numbers up; they aren't comparable.

| ID | Scope | Target file | Rough size | Gates |
|---|---|---|---|---|
| **GAP-L1-A** | SO_PEERCRED cred-triple + fail-closed (§4.1) | `test_shm_capability_channel.cpp` (extend) | small (2 tests) | Before REVIEW-C |
| **GAP-L1-B** | MSG_CTRUNC + zero-size fd defense (§4.1) | `test_shm_capability_channel.cpp` (extend) | small-medium (2 tests; MSG_CTRUNC needs a multi-fd sender helper) | Before REVIEW-C (REVIEW-B medium carry) |
| **GAP-L1-C** | MemfdConsumer/Producer ctor RAII partial-failure (§4.1) | `test_shm_capability_channel.cpp` (extend) | small (2 tests; fd-leak sanity via /proc/self/fd count or pre-test fd snapshot) | Before REVIEW-C (REVIEW-B medium carry) |
| **GAP-L2b-A** | SO_PEERCRED uid mismatch close (§4.2) | `test_attach_protocol.cpp` (extend) | small (1 test; mock the cred check or use a setuid-helper child) | Before REVIEW-C |
| **GAP-L2b-B** | `initiate_consumer_handshake` strip_unix_scheme direct (§4.2) | `test_attach_protocol.cpp` (extend) | small (1 test) | Before REVIEW-C |
| **GAP-L2b-C** | Challenge mismatch explicit assertion (§4.2 — confirmed dead-path coverage in v3) | `test_attach_protocol.cpp` (add 1 test: encrypt a different plaintext, pass MAC verify, expect producer reject) | small | Before REVIEW-C |
| **GAP-L3-A** | CONSUMER_REG_ACK SHM-field presence + ZMQ-field absence (§4.5) | `datahub_broker_protocol_workers.cpp` `wire_conformance_consumer_reg_ack_shape` (extend; +2 sub-cases) | small | Before REVIEW-C |
| **GAP-L3-B** | REG_REQ.shm_capability_endpoint parse + store (§4.5) | `broker_admin_workers.cpp` (extend) — test driver is `test_datahub_broker_admin.cpp`; worker file has no `datahub_` prefix | small | Before REVIEW-C |
| ~~GAP-L3-C~~ | ~~RoleAPIBase consumer-side direct dial test~~ | **DROPPED in v3** — L2 covers L2b/L2c exhaustively; #258 1k covers the real dial.  A synthetic L3 dial test would be marginal extra coverage. | — | — |
| ~~GAP-L3-D~~ | ~~End-to-end dial budget within 2s~~ | **DROPPED in v3** — there is no single 2s budget; the actual contract (retry-loop wait < producer accept timeout) is statically verifiable.  A runtime test would be timing-brittle. | — | — |
| **#270** | RoleHostFrame producer-side L2 coverage (§4.6) | New `test_role_host_frame_shm_auth_listener.cpp` | (sized under #270 itself) | Before REVIEW-C |
| **#257 1j** | L3 broker happy/denied/divergence-WARN tests | (under #257 tracker) | (sized under #257 itself) | After #275 lands |
| **#258 1k** | L4 e2e SHM auth-gated data flow | (under #258 tracker) | (sized under #258 itself) | After #275 + #270 + GAP-L3-A/B land |

**8 active gap-fill commits in this plan** (3 L1 + 3 L2b + 2 L3), plus
the 3 separately-tracked tickets (#270, #257, #258).

### 5.3 REVIEW gates updated

| Review | Existing trigger | Test-completeness addition (this plan) |
|---|---|---|
| REVIEW-C (#276) | After #275 lands | Verify GAP-L1-A/B/C, GAP-L2b-A/B/C, GAP-L3-A/B closed; verify #270 covers §1.5 |
| REVIEW-D (#277) | After #257 + #258 | Verify §1.4 broker tests pin SHM-specific ACK fields end-to-end; verify §4.9 e2e auth-gating |
| REVIEW-E (#278) | After #262 mutual auth | Symmetric direction tests added; threat model fully covered |

---

## 6. Out of scope (explicit)

- Mutual auth (#262) — Frame 3 + producer-prove-to-consumer side
- Cross-platform (#259/#260/#261) — FreeBSD/macOS/Windows L1 backends
- `api.crypto.*` script bindings (#247)
- HEP-0036 ZMQ retrofit (#246)

---

## 7. Designer review checkpoints (v3.1)

The structural decisions:

- [ ] **§3.1 retirement list (6 tests, 3 files)** — approve or push back
- [ ] **§4 confirmed gaps + §5.2 GAP-* coverage commits (8 active)** — approve scope; reorder if some should slip to REVIEW-D
- [ ] **§5.1 #275-S2 scope** — template (§3.4) + ~40 migrations + 6 retirements.  NO rename, NO docstring item.

The other v1/v2 items are now resolved or dropped:
- Template wrapper (§3.4) — your prior approval stands
- Rename (§3.5 v1) — DROPPED in v2
- Docstring fix (§3.5 v2) — DROPPED in v3 after verifying the file is honestly named
- Layer correctness (§2.2) — confirmed nothing to move
- "End-to-end dial budget" (§1.6 v2) — reframed in v3 as a static timer-chain invariant; GAP-L3-D dropped
- Synthetic L3 dial test (GAP-L3-C v2) — dropped in v3; L2 + #258 cover it

---

## 8. References

- `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` — authoritative design (§5 wire + §6 L1/L2 + §9 D4)
- `docs/HEP/HEP-CORE-0036-Peer-Admission.md` — ZMQ model HEP-0041 mirrors
- `docs/README/README_testing.md` — layer doctrine + mocking discipline (§1.2)
- `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
- `docs/todo/AUTH_TODO.md` § "#275 1i-cleanup staged execution" — live tracker
- REVIEW-B close-out commit `8122bafc` — B1/B3 fixed + 5 medium items carried to REVIEW-C.  v3.1 mapping: MSG_CTRUNC defense → GAP-L1-B; MemfdConsumer ctor RAII → GAP-L1-C; strip_unix_scheme direct test → GAP-L2b-B; lifetime-ordering deferred (docs-only); "budget allocation" reframed as static timer-chain invariant (§4.7) and dropped — no runtime test.
