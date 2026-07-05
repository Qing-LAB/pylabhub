# In-Session Task List Snapshot (2026-07-04)

**Purpose:** persist the full name AND context/details of every
pending + in-progress task in the Claude Code session-scoped
TaskCreate list, so that clearing the session does not lose them.

**Method:** dumped verbatim from `TaskList` + `TaskGet <id>` for
every non-completed task at end of session 2026-07-04.
Completed tasks (#44-329 completed subset) are omitted from bodies
here — their subject lines are preserved at the bottom for
provenance.  If the archived body of a completed task is needed
later, git log / commit messages / TODO archives carry it.

**Reload plan:** when starting a fresh session, this file is the
source of truth for pending work.  Either (a) leave it as-is
because most items already exist in `docs/todo/*.md` +
`TODO_MASTER.md`, and use this file as a cross-check, or (b)
mechanically re-file each pending item into TaskCreate at session
start if you rely on the in-session task tool.

---

## Table of contents

- §1 In-progress tasks
- §2 SEC-Fold-2 anchor (separate documents)
- §3 HEP-0041 SHM capability transport
- §4 HEP-0042 pre-confirm + ABI fingerprint (HEP-0032 §X)
- §5 AUTH / HEP-0035 / CURVE cleanup
- §6 Pattern 4 test ladder (single-hub + dual-hub variants)
- §7 Native ABI + engine parity
- §8 Script engine surfaces
- §9 Test framework hardening
- §10 Federation + cross-hub
- §11 Doc hygiene + reorganization
- §12 Recent review findings
- §13 Older B-items / small polish
- §14 Completed-task subject-line log (provenance)

---

## §1 In-progress tasks

### #154 — [validation] Re-create L3 broker tests against the refactored lib code
Status: in_progress

L3 broker test re-creation against the HEP-CORE-0040 refactored lib code.

PROGRESS 2026-06-07:
Migrated 5 of 9 originally-masked broker test files:
  1. datahub_broker_request_comm_workers (4 scenarios) — batch 1
  2. broker_admin_workers (6 scenarios) — batch 1
  3. broker_schema_workers (5 scenarios) — batch 1 + #181 heartbeat fix
  4. broker_consumer_workers (6 scenarios) — batch 2
  5. datahub_channel_group_workers (7 scenarios) — batch 2

Pattern proven across (a) helper-style `run_with_host` files, (b) per-scenario fixture files, and (c) start_direct_broker files.

Established infrastructure (this task drove these changes):
  - `KeyStore::add_identity_from_z85` production API (single packing site for tests + production)
  - `CurveKeyStoreFixture` (caller-side SMS+KS+seed; both broker helpers only read)
  - `start_hubhost_broker` rewritten with explicit BYPASS PATTERN documentation
  - Stale doc references cleaned in 6 lib headers

Deferred to dedicated tasks (4 files use deleted `BrokerService::Config::use_curve` + `enforce_ctrl_admission` + local helpers; each scenario needs uid-definition ordering changes that aren't bulk-editable):
  - #182 datahub_broker_health_workers (11 scenarios) — SUPERSEDED by #285
  - #183 datahub_e2e_workers (3 scenarios + WorkerProcess argv complexity) — SUPERSEDED by #285
  - #184 datahub_broker_workers (40 scenarios) + datahub_role_state_workers (23 scenarios) — SUPERSEDED by #285

Test evidence:
  - L3 BrokerRequestComm + Admin + Schema + Consumer + ChannelGroup: 29/29 passing
  - L2 KeyStore: 19/19 passing
  - L2 zmq_queue_auth: 13/13 passing
  - L2 hub_state: 137/137 passing
  - No regressions across all migrations

### #155 — CLI gatekeeper/clearance Phase 3: --init one-shot bundling + L4 test updates
Status: in_progress

Phases 1 + 2 shipped: --validate parity (commit 3215e5aa), helper + flag plumbing get_required_uid / --skeleton / --uid / --vault-path / --no-prompt (commit c684776a). Phase 3 (out-of-scope of c684776a per its commit body) is: `--init` one-shot that bundles --keygen + --add-known-role + first-launch warmup, plus ~24 L4 test updates that currently invoke the multi-step flow. Design lives in HEP-CORE-0033 §6.5 and HEP-CORE-0035 §2 (vault-is-gatekeeper, validate-is-clearance).

### #206 — Audit response: fix A1-A5 bugs + B1-B2 doc + Audit2 B1/B5 cleanup
Status: in_progress

Address findings from fresh-eyes audits on native engine: A1 empty-UID guard, A3 snapshot lifetime doc, A4 destructor ordering, A5 lifecycle flag race, B1 policy_info LOGGER_WARN_ONCE, B2 Handle lifetime Doxygen warnings, Audit2 B1 drop redundant inline keywords, Audit2 B5 constexpr on metrics_snapshot factory.

### #285 — Pattern 4 reform — L3 datahub multi-process tests (folded scope)
Status: in_progress

**SCOPE NARROWED 2026-06-25** following L2/L4 lesson — SHM tests (§2.3 + §2.6 from matrix) moved to #258 because their producer subprocess would need either a parallel production scaffold (test-faithfulness antipattern per #270) or a real `plh_role` binary (which makes it L4 territory).  This task now covers ONLY the §2.7 wire-protocol sibling files that don't have the SHM scaffolding problem.

**Scope (after narrow):**

1. `datahub_broker_health_workers.cpp` (was task #182) — BRC heartbeat + hub-dead detection over the production wire.  Multi-process Pattern 4 with custom workers: broker subprocess + role subprocess(es); pin heartbeat cadence + hub-dead detection FSM transitions.
2. `datahub_broker_workers.cpp` + `datahub_role_state_workers.cpp` (was task #184) — broker subprocess + role subprocess; CHANNEL_NOTIFY_REQ + REG_REQ wire-protocol pinning.

These exercise wire-protocol over ZMQ — no SHM RoleHostFrame setup needed, so the Pattern 4 custom-worker pattern (existing `pattern4_smoke` / `pattern4_registration` / `pattern4_heartbeat` / `pattern4_consumer_lifecycle` family) fits naturally.

**Removed from scope (moved to #258 per L2/L4 lesson):**
- 6 `DatahubE2ETest.*` SHM tests (matrix §2.3) — were planned as `Pattern4DataPipelineTest.*`
- 2 `DatahubStressRaiiTest.*` SHM tests (matrix §2.6) — were planned as `Pattern4DataStressTest.*`

**Reference docs:**
- `docs/tech_draft/DRAFT_HEP-0041-pattern4-reform-coverage_2026-06.md`
- `docs/README/README_testing.md` § "Pattern 4 — Multi-process wire protocol test"
- Existing Pattern 4 ladder: #221, #222, #223

**Suggested execution shape:** start with broker_health (simplest — extends existing Pattern4HeartbeatTest infrastructure with hub-dead injection).

**Critical-path note:** NOT a blocker on #275 S4 + S5 cleanup.  #275 S4 + S5 are now blocked on #258 (which covers the SHM tests this task used to cover).  #285 is parallel work.

---

## §2 SEC-Fold-2 anchor (see separate documents)

The SEC-Fold-2 refactor (folding SMS into a proper lifecycle
module, plus consumer migration to `secure().X()` wrappers) is
covered by two dedicated persistent docs in this directory:

- **`DRAFT_sec_fold_2_plan_and_guidance_2026-07.md`** — authoritative
  plan + naming discipline + phase order.  Committed `60de75a2`.
- **`DRAFT_sec_fold_2_resume_state_2026-07.md`** — current position,
  attempts + reverts, session-start checklist.  Committed `a258c7a5`.

Related task IDs (already captured in resume-state §3): #244
(SEC-Fold parent), #106 (script vault — folds into HEP-0043 §5/§8/§10
under Phase G), #247 (script crypto — folds into HEP-0043 §10),
#317 (broker SHM observer — resume after Phase E).

---

## §3 HEP-0041 SHM capability transport

### #244 — SHM Channel Auth (cross-platform) — design HEP equivalent to HEP-CORE-0036 for SHM transport

## Why this exists

While starting AUTH-4 (#164 — broker mints shm_secret + delivers via CONSUMER_REG_ACK) on 2026-06-16, we surfaced a fundamental gap: the SHM transport's access-control story is structurally weaker than the ZMQ CURVE story by **multiple orders of magnitude**, and cannot be papered over with the planned secret-mirror plumbing.

Production-readiness requires SHM channel data to be at LEAST as protected as ZMQ channel data.  Today's POSIX `0666` default + plaintext-data-at-rest in `/dev/shm/<name>` means any non-root user on the host with shell access can list, probe, and `mmap` channel data without crossing any auth check.

AUTH-4 (#164) is **deferred** behind this design.  The secret-mirror plumbing it would add is the WRONG layer to fix this at.  Decisions here supersede AUTH-4's wire shape.

## Platform constraint

Must work cross-platform on Windows, Linux, FreeBSD, macOS.  Rules out the "easy modern Linux answer" of `memfd_create + SCM_RIGHTS fd-passing` as a single solution.  Options A/B/C/D enumerated in the original task body (capability fd-passing / POSIX shm_open+ACL / encrypt-at-rest / hybrid).  Recommend A+D.

Deliverables: HEP draft, per-platform impl plan, wire protocol delta, HEP-0036 cross-reference, migration plan.

### #257 — HEP-0041 Phase 1 substep 1j — L3 broker tests
Blocked by: #252, #253, #276, #270 (all completed); Blocks: #258, #277.

Pattern 4 subprocess-per-role L3 tests pinning the CONSUMER_ATTACH_REQ contract.

Scenarios (each its own TEST):
- ConsumerAttachSucceedsWhenAuthorized — consumer in allowlist; broker replies success; capability transferred; data flows.
- ConsumerAttachDeniedWhenNotAuthorized — consumer NOT in allowlist; broker replies denied; no capability transfer; consumer drops cleanly.
- ConsumerAttachDivergeBrokerAllowedCacheDenied — set up cache out-of-sync; broker allows; producer admits + logs WARN with stable marker.
- ConsumerAttachDivergeBrokerDeniedCacheAllowed — set up cache out-of-sync; broker denies; producer denies + logs WARN.
- ConsumerAttachBrokerUnreachable — BRC down; producer fails closed (does NOT fall back to cache).

Pin: WARN log markers per #238 stable-marker format. Pin: connection-rejected outcome path (not just exit code) per #243.

### #259 — HEP-0041 capability transport: FreeBSD backend

FreeBSD ≥13.0 backend for IShmCapabilityProducer / IShmCapabilityConsumer.  Currently `#error` placeholder in `src/utils/security/shm_capability_channel.cpp`; detailed design in comment block above `#error`.

Substitutions from Linux backend: `getpeereid(fd, *uid, *gid)` instead of `getsockopt(SO_PEERCRED, struct ucred)`; AcceptedPeer.pid stays 0 (xucred no PID).  Everything else same as Linux.  Validation: L2 round-trip must pass on FreeBSD ≥13.0.

### #260 — HEP-0041 capability transport: macOS backend

macOS (Darwin) backend.  `#error` placeholder currently.

No memfd_create.  Emulate anon SHM via `shm_open` with `O_CREAT|O_EXCL|O_RDWR, 0600` on random 24-char name (PSHMNAMLEN=31), then `shm_unlink` immediately (fd remains, name gone).  No accept4 — use accept() + `fcntl(F_SETFD, FD_CLOEXEC)`.  `getpeereid` for cred.  SCM_RIGHTS works.

### #261 — HEP-0041 capability transport: Windows backend

Architecturally different from POSIX:
- Named pipe (`\\\\.\\pipe\\plh_shm_<random>`) via CreateNamedPipeA / CreateFileA
- Anon SHM: `CreateFileMappingA(INVALID_HANDLE_VALUE, ...)` (pagefile-backed, no on-disk name)
- Accept: `ConnectNamedPipe` overlapped + `WaitForSingleObject` for timeout
- Peer identity: `GetNamedPipeClientProcessId`
- Capability transfer: `OpenProcess(PROCESS_DUP_HANDLE)` + `DuplicateHandle` + `WriteFile` of the duplicated HANDLE value
- AcceptedPeer: (peer_pipe_handle: HANDLE as void*, peer_pid: ULONG) — already declared

Public-header changes for Windows have landed.  Coordinate with #120 for SECURITY_ATTRIBUTES DACL hardening.

### #262 — HEP-0041 Phase 1: mutual auth (producer→consumer proof-of-possession)
Blocked by: #277; Blocks: #278.

HEP-0041 Phase 1 today proves CONSUMER identity to producer.  Producer identity is NOT proved to consumer — a hostile process could bind to producer's shm_capability endpoint after the real producer crashes and offer a memfd containing corrupted or malicious data.

Threat: post-producer-death squatter binds to same endpoint; consumer connects, receives memfd, mmaps, reads corrupted slot data.  Attacker at same uid can inject payload via crafted DataBlock.

Three design options: (A) Symmetric challenge-response (5th frame; recommended); (B) SO_PEERCRED + broker-verified pubkey (weaker); (C) TLS-style two-way handshake (overkill).

**Recommendation (A) impl sub-plan:**
1. Extend `AttachProtocol` wire — frames 3+4 (producer challenge + consumer response with proof).  Detail in HEP-CORE-0041 §9 D4 amendment.
2. `AttachProtocolAcceptor::accept_one` runs consumer-verify AS TODAY, then sends producer challenge, verifies consumer's proof.
3. `initiate_consumer_handshake` extended for producer-verify step; producer pubkey from CONSUMER_REG_ACK context.
4. Amend HEP-CORE-0041 §9 D4 with extended flow + mermaid.
5. L2 test: matched producer pubkey succeeds; mismatched rejects.
6. L4 test: producer squatter attack fails.
7. Update HEP-CORE-0041 Phase 1 acceptance — Phase 1 complete only after this ships.

Blocks REVIEW-E (#278).  Awaiting user's choice of A/B/C before impl begins.

### #277 — REVIEW-D: full systematic review after 1j + 1k tests land
Blocked by: #257, #258 (which is completed); Blocks: #262.

Fourth of five REVIEW-A..E milestones.  Trigger: after #257 (1j L3 broker tests) + #258 (1k L4 e2e) both ship.

Scope: parallel multi-agent review.  Verifies:
- L3 tests actually pin HEP §9 D4 8-step sequence + divergence-WARN four-way table.
- L4 demo exercises the full producer → broker pre-confirm → consumer chain with auth + allowlist + revocation cycle.
- Test contract markers stable.
- No test-vs-production divergence.

Last review before mutual auth (#262).

### #278 — REVIEW-E: full systematic review after #262 mutual auth lands (Phase 1 production-readiness final gate)
Blocked by: #262.

Fifth and final REVIEW-A..E milestone.  Trigger: after #262 (mutual auth) ships.

Scope: parallel multi-agent review.  Verifies:
- 3rd handshake frame closes producer-impersonation window.
- HEP-0041 §5.5 + §9 D4 reflect bidirectional handshake.
- Threat model statement matches code defences.
- No caller relies on one-way-auth assumption.
- Cross-platform plan inherits symmetric handshake.

PHASE 1 PRODUCTION-READY GATE: passing this milestone formally declares HEP-0041 Phase 1 shippable for production SHM channels on Linux.

### #282 — HEP-0041 1i-mig-4 follow-up: consumer-dial budget split + BRC-poll-thread offload

From #272 self-review (2026-06-23): `RoleAPIBase::apply_consumer_reg_ack_shm_` runs synchronously on the BRC poll thread and can block for up to ~3.9s during the dial.

**Worst-case (corrected):** D3 retry loop 10×100ms handshake timeout + 9×100ms inter-attempt sleep = 1900ms + SCM_RIGHTS recv up to 2000ms = ~4s.  Commit #272 said "~3s" which is the H3a-race-only worst case.

Concerns: HEP-0031 §4.1 Shutdown Contract (no `should_stop` in dial loop; teardown waits ~4s); racing producer-side 2000ms `consumer_attach` timeout from #280; #262 mutual auth adds Frame 3 tightening budget further.

Options: (a) move dial onto worker thread (async) — rewrite success-path FSM; (b) dedicated short-lived dial worker per channel — thread proliferation; (c) shorten windows — less H3a tolerance; (d) accept + document — smallest change; (e) generalize via ThreadManager `spawn_bounded` primitive (#283).

No immediate action.  Pick during REVIEW-B (#274 completed) or with #262 mutual-auth design.

### #283 — HEP-CORE-0031 amendment + impl: ThreadManager::spawn_bounded primitive

Generalize bounded-deadline cooperative threads pattern recurring in #272, #280/L2c, #262 Frame 3, #75, #247.

Primitive `spawn_bounded(name, step_body, wall_time_budget, on_timeout_message_provider, on_complete)`:
1. TM-managed loop thread.  Ticks: call step_body → check stop_token → small sleep → repeat until Done/Failed or stop_token requested.
2. Sweeper (50-100ms) walks bounded threads; on budget exceeded calls message_provider, logs WARN, `request_stop()` on stop_token.
3. **Cancellation lives in loop, not body.**  Body's contract: non-blocking step (~10ms ceiling per call).
4. **Log filed by sweeper, not body.**  If body deadlocks, log still fires.
5. **Worst-case:** deadlocked thread destroyed at process death + bounded leak within that thread.  Log honored, resources bounded, process reap.
6. Per-step budget defines stop_token honor resolution.  ~10ms + sweeper period.

**Two completion modes:** truly-async (`on_complete(std::optional<R>)` fires on bounded thread; nullopt=timeout); bounded-sync (caller uses `handle.wait_for(budget)`).

**Payoff is async offload + uniform observability**, not timeout enforcement (per-syscall timeouts already bound wall-time).  Pilot migrations: consumer SHM dial (#272), producer L2c broker pre-confirm, #262 mutual-auth Frame 3.

Amendment to HEP-CORE-0031 → tech_draft → design review → promote → implement.

Open question: per-step budget loop-level enforcement (abort if step >50ms) or trust contract?  Probably latter + log on violation.

### #317 — HEP-0041: Broker SHM observation fd via SCM_RIGHTS

**Phase A ✅** shipped 2026-07-03 (b3d5e36d) — `datablock_get_metrics_from_fd` C API + L2 test.
**Phase B ✅** shipped 2026-07-03 (da2a5e76) — `DataBlockObserverHandle` header-only observer factory with `validate_header_layout_hash` ABI gate.
**Phase C remaining — plan below:**

**C.1 — Wire mechanism (design + producer-side gate).**  Extend `AttachProtocol` hello frame with `role_type` field ("observer" value).  `AttachProtocolAcceptor::accept_one` routes on role_type.  Producer config knob `producer.shm_metrics_observer` (bool, default true).

**C.2 — Broker-side observer dial + fd cache.**  After successful `CONSUMER_REG_ACK` on SHM channel, spawn lightweight observer-attach.  Dial producer, CURVE handshake with broker identity keypair, receive memfd via SCM_RIGHTS.  Store fd in broker-side `channel → int fd` map.  On producer-death, `close(fd)` + drop entry.

**C.3 — Metrics fetch + `metrics_source` envelope.**  Modify `collect_shm_info` at `broker_service.cpp:5866`.  Look up cached fd; if present + fresh use `datablock_get_metrics_from_fd`, `metrics_source="live_observation"`.  Else fall back to heartbeat piggyback per HEP-CORE-0019 §2.3 Phase 6, `metrics_source="heartbeat"`.  Neither: `"unavailable"`.

**C.4 — Tests.**  L4 end-to-end + negative (observer denied) + crash-safety (producer SIGKILL mid-observation).

**C.5 — Doc sync.**  Verify HEP-CORE-0041 §10.5 matches code.

**Deferred to Phase D:** Cross-platform (macOS/Windows) observer impls.  Linux-only for Phase C.

**Paused for SEC-Fold-2.**  Resume after Phase E class rename per resume-state doc.

---

## §4 HEP-0042 pre-confirm + ABI fingerprint

### #324 — HEP-CORE-0032 §X: Handshake ABI Fingerprint — design amendment
Blocks: #325, #326.

Amend HEP-CORE-0032 with new §X "Handshake ABI Fingerprint" section.

**Wire envelope on REG_REQ + CONSUMER_ATTACH_REQ:**
```
{
  "abi_major": <uint32>,      // breaking-change counter
  "abi_minor": <uint32>,      // additive-change counter
  "build_hash": "<12 hex>"    // 48-bit build fingerprint
}
```
Broker echoes its own triple in REG_ACK for symmetric role-side verification.

**Versioning taxonomy (§X.1):**
- MAJOR — REJECT on mismatch (SharedMemoryHeader layout/semantic change, wire rename/type/removal, protocol_version bump, C API sig change, msg-type retirement, callback removal)
- MINOR — WARN + accept (new optional wire field, new enum value, new metrics field, new BROKER_XXX type, new optional callback)
- BUILD — INFO only (debug/release flags, log level default, timestamp precision — NOT log content)

**Log-format stability (§X.2):** log MESSAGE CONTENT is MINOR-bump concern (tests read content); framing/level/timestamp precision is BUILD-only.

**Behavioral policy (§X.3):**
- Default: WARN + accept on any mismatch.  `broker.strict_abi_mismatch = false`.
- Strict: HARD REJECT on MAJOR when true.  MINOR always WARN.  BUILD always INFO.

**Log format:**
```
REG_REQ role_uid=X abi=3.12/a1b2 vs broker=3.12/f0e0 → OK
REG_REQ role_uid=X abi=3.12/a1b2 vs broker=3.13/f0e0 → MINOR MISMATCH (accept)
REG_REQ role_uid=X abi=2.99/a1b2 vs broker=3.00/f0e0 → MAJOR MISMATCH (rejected|accepted)
```

Mermaid sequence diagram.  Cross-refs: HEP-0028 C API, HEP-0036 §5b wire schema, HEP-0041 §10.5 observer.  Approved 2026-07-03.

### #325 — Impl: emit + verify + log ABI fingerprint on REG_REQ + CONSUMER_ATTACH_REQ
Blocked by: #324.  Blocks: #326.

Generated header (cmake configure time):
- `PLH_ABI_MAJOR` — constexpr uint32
- `PLH_ABI_MINOR` — constexpr uint32
- `PLH_BUILD_HASH` — 12-hex-char from `git describe --always --dirty` (or SOURCE_DATE_EPOCH+cmake for reproducible builds)

Wire binding (role→broker + broker→role): extend REG_REQ / CONSUMER_ATTACH_REQ / REG_ACK / CONSUMER_ATTACH_ACK with the triple.

Broker verification path: log INFO per §X.3 format.  MAJOR mismatch: strict → reject with `status=abi_major_mismatch`; loose → WARN + accept.  MINOR always WARN + accept.  BUILD only INFO.

Role-side symmetric check on REG_ACK.

Config knobs: `broker.strict_abi_mismatch` + `role.strict_abi_mismatch` (both default false).

Log content stability: L2/L3 tests will assert against those substrings.  Any refactor without MINOR bump breaks tests (desired contract).

### #326 — L2/L3 tests: ABI fingerprint matched/minor/major/build-only paths
Blocked by: #325.

**L2 tests (unit — pin constexpr + envelope):**
- `RegReqPayloadCarriesAbiFingerprint` — REG_REQ JSON includes three fields.
- `RegAckPayloadCarriesBrokerAbiFingerprint` — REG_ACK JSON includes broker triple.
- `CompareAbiFingerprint_MajorMismatch_ReturnsRejectVerdict` / `MinorMismatch_ReturnsWarnVerdict` / `BuildOnlyDiff_ReturnsInfoVerdict` — helper classifier.

**L3 tests (broker+role subprocess pair):**
- `AbiFingerprintMatch_AcceptsAndLogsOK`
- `AbiFingerprintMinorMismatch_AcceptsAndLogsWarn`
- `AbiFingerprintMajorMismatch_Lenient_Accepts`
- `AbiFingerprintMajorMismatch_Strict_Rejects`
- `AbiFingerprintRoleSideStrict_MajorMismatch_RoleRefusesRegAck`

Test-side mechanism: compile worker binary with `-DPLH_ABI_TEST_OVERRIDE_MAJOR=99`.  No runtime override in production code.

Pattern-3 IsolatedProcessTest for pair tests, Pattern-1+ for L2.

Coverage requirement: every log-line template pinned by exact substring — §X.3 contract enforcement.

Closes #324 → #325 → #326 chain.

---

## §5 AUTH / HEP-0035 / CURVE cleanup

### #66 — S1 Phase B: migrate ZmqQueue + InboxQueue to apply_socket_policy

Migrate ZmqQueue (`src/utils/hub/hub_zmq_queue.cpp`) + InboxQueue (`src/utils/hub/hub_inbox_queue.cpp`) to `apply_socket_policy` helper + 4-layer time-bound model.  Same pattern BRC got in S1 Phase A (commit 81804d7 era): reconnect_ivl=-1 terminal disconnect, ZMTP heartbeat 5s/30s, sndtimeo=500ms, layer-1 connected gate, connection-state monitor.  Helper at `src/include/utils/zmq_socket_policy.hpp`.  Derisks same bug class BRC had pre-S1 (hangs on dead-peer sends).  Est. ~half day.  Not blocking dual-hub deliverable.

### #74 — HEP-CORE-0035 auth implementation

Production gate.  Step-by-step plan in `docs/todo/AUTH_TODO.md`.  Strategic context in `docs/tech_draft/DRAFT_HEP-0036-implementation-guideline_2026-05.md`.  Replaces deleted #126-#131 (PeerAdmission D-H sub-trackers).

### #102 — HEP-0035 §4.7 runtime key handling — SUPERSEDED by HEP-CORE-0040 chain
Blocked by: #167, #168, #169, #170, #171 (all completed).

Original scope: §4.7 mlock + no-core-dump + zeroing as flat utility.  SUPERSEDED 2026-06-05 by HEP-CORE-0040 chain (tasks #165–#174) which lifts §4.7 utility into registered lifecycle subsystem.

Keep #102 open as tracker until §4.7 cross-references HEP-0040 (#168 completed).  Close.

### #106 — HEP-CORE-0038 + impl: script-accessible vault keystore (api.vault_save/load)
Blocked by: #167, #170 (completed).

After HEP-CORE-0040 chain lands, dynamic KeyStore is the storage layer for scripted secrets.  HEP-0038 impl consumes same LockedKey + KeyStore primitives.  Script-saved bytes go into a LockedKey owned by role's KeyStore.  Cite HEP-0040; NO separate locked-memory implementation needed.

### #120 — Windows pathway hardening for HEP-CORE-0035 §4.6 floor

Deferred from #119 second-review batch.  Address after Linux validation complete.

- B6: `hub_vault.cpp` `publish_public_key` has fragile `#if PYLABHUB_PLATFORM_WIN64` split into two non-adjacent blocks.  Refactor to one outer `#ifdef` with two clean halves.
- C4: `vault_crypto.cpp:101-103` Windows ofstream branch still throws "vault: write failed" / "vault: cannot write" without GetLastError detail.  L1 strerror polish (50b93f23) is POSIX-only; mirror on Windows.

Scoped to `hub_vault.cpp` + `vault_crypto.cpp` Windows branches; zero POSIX code.

### #152 — Lib refactor: delete legacy RoleIdentityPolicy per HEP-0035 §8 Phase 6

Retire legacy placeholder code pre-dating CURVE-required model.

Delete:
- `pylabhub::broker::RoleIdentityPolicy` enum (`role_identity_policy.hpp`)
- `BrokerServiceImpl::check_role_identity()` and supporting state
- `KnownRole` struct's string-name fields (placeholders; §4.1 ZAP allowlist uses `pubkey_z85`)
- `ChannelPolicyOverride` per-channel glob override

Grep audit before delete.  Independent of #151 — clean slice.

### #246 — HEP-CORE-0036 amendment — retrofit ZMQ to pre-confirm pattern (no cached fast-path; cache as observability)

Current HEP-0036 pattern (cached allowlist + passive revocation) has drift-window vulnerability.  Cross-transport fix: ZMQ adopts same pre-confirm pattern as HEP-0041 SHM.  Every attach/handshake → producer queries broker → broker checks current state → producer admits only on broker's ACK.

**Scope:**
1. Broker: add `CONSUMER_ATTACH_REQ` / `CONSUMER_ATTACH_ACK` handler.
2. Role-side ZAP path (`ZapRouter::pump_one` → `ZmqQueue::is_peer_allowed`): ALWAYS query broker via BRC.  Log WARN on divergence.  Admit on broker's answer.
3. Cached allowlist: stays for observability.  Divergence = health signal for NOTIFY pipeline.
4. `CONSUMER_ATTACH_REQ` happens on BRC poll thread, async with ZAP.  Options: (a) sync inside `pump_one` — simpler, blocks; (b) per-handshake FSM — more plumbing, cleaner.
5. HEP-0036 §3 amendment: "passive revocation" scoped to "entry-gate-only" post-admission.

Wire delta: `CONSUMER_ATTACH_REQ` req/ack shapes (JSON with channel_name, consumer_pubkey, consumer_role_uid, correlation_id → status success/denied + denial_reason).

Cost: one BRC round-trip per CURVE handshake.  Sub-ms on healthy broker.  Invisible for pylabhub workloads.

Sequencing: after HEP-0041 SHM ships (which establishes pre-confirm pattern).

### #247 — Script-accessible crypto primitives — `api.crypto.*` bindings

Sibling to #106 (HEP-0038 vault keystore) and consumer of #244 / HEP-0041 §9 D8 (which lands public C++ AEAD + KDF + random + LockedKey exports).

Add `api.crypto.*` cluster:
- `api.crypto.generate_random_bytes(n)` / `generate_random_u64()`
- `api.crypto.aead_encrypt(key, ad, plaintext)` / `aead_decrypt(...)`
- `api.crypto.kdf_derive(master_key, context, info)`

Key handling: opaque `KeyHandle` wrapping `LockedKey` reference.  Raw bytes accepted only for non-secret payloads (the plaintext).

Out of scope: asymmetric (sign/verify/DH) — phase 2 if demand exists; format choices beyond ChaCha20-Poly1305 default; HSM/enclave — user integrates at C ABI.

Sequencing: depends on #106 (KeyHandle shape) and #244 Phase 4 (public C++ AEAD wrappers).

Three engines × five primitives = 15 bindings + parity tests + doc.

**Folds into SEC-Fold-2 Phase G (HEP-0043 §10).**

---

## §6 Pattern 4 test ladder (single-hub + dual-hub variants)

### #224 — Pattern4DeregistrationTest — rung 7

Pin `DEREG_REQ`/`DEREG_ACK`, `CONSUMER_DEREG_REQ`/`CONSUMER_DEREG_ACK`, `DISC_REQ`/`DISC_ACK`/`DISC_PENDING` (HEP-CORE-0023 §2.2).

New `test_pattern4_deregistration.cpp` + workers.  Production INFO markers (one-shot): role-side `DEREG_REQ sending`, `presence Registered→Deregistered`; broker-side `DEREG_REQ accepted`, `DEREG_ACK sending`, `Presence row dropped`.  Test pins clean shutdown.  DISC variant: pin each branch.

Depends on: #221 (registration markers + Presence FSM).

### #225 — Pattern4ChannelNotifiesTest — rung 8

Pin broker→role fire-and-forget notify family: `CHANNEL_CLOSING_NOTIFY`, `CHANNEL_ERROR_NOTIFY`, `CONSUMER_DIED_NOTIFY`, `CHANNEL_EVENT_NOTIFY`, `CHANNEL_PRODUCERS_CHANGED_NOTIFY`.

**Carries forward contract from retired L3 test 2026-06-28 (AUTH-6 batch-2a C1 retirement):**
- L3 `BrokerProtocolTest.ClosingNotify_DeliveredToProducerAndConsumer` RETIRED — only test pinning CHANNEL_CLOSING_NOTIFY fan-out invariant.  Replacement at rung 8 must cover:
  - **Fan-out cardinality:** broker emits to ALL channel members (both producer + consumer BRCs).  Mutation catches single-recipient bug.
  - **Multi-actor verification:** producer + consumer subprocesses; both observe within heartbeat timeout via `received CHANNEL_CLOSING_NOTIFY` markers.
  - **Trigger:** in-process `broker.request_close_channel(channel)`.  Mirror via CLI/script at L4.
- Retired L3 used `EXPECT_GE(prod_closing, 1) && EXPECT_GE(cons_closing, 1)` after `poll_until(5s)`; L4 replacement pins same dual-receipt via subprocess log-marker observation.

Depends on: #222 (channel lifecycle markers).

### #226 — Pattern4RegistrationErrorTest — rung 9

Pin REG_REQ negative paths — broker rejects on bad pubkey, bad uid claim, length violation (HEP-CORE-0036 §I10 one-pubkey-per-uid, §I1 two-conditions admission).

Three sub-scenarios: (1) empty `zmq_pubkey` (broker_service.cpp:~1418); (2) wrong-length `zmq_pubkey` (~1429); (3) uid claim mismatch with KnownRolesStore.

Reuses broker ERROR log at 1414, 1429; adds role-side INFO `REG_REQ rejected error_code='...' message='...'`.  Test: REG_REQ sent → REG_ACK never appears → ERROR appears → role stays/returns Unregistered.

Depends on: #221.

### #227 — Pattern4AuthUpdateTest — rung 10

Pin runtime auth-update notify-then-pull contract per HEP-CORE-0036 §6.5 (Amendment 2026-06-04): `CHANNEL_AUTH_CHANGED_NOTIFY` → `GET_CHANNEL_AUTH_REQ`/`ACK`.

Production INFO markers: broker sends notify; producer receives notify; producer sends GET_CHANNEL_AUTH_REQ; broker sends ACK; producer receives ACK; producer applies master_approval.

Scenario: producer Active → broker mutates allowlist → producer pulls + applies → traffic gating reflects new allowlist.  Buffered-update edge case: notify arrives while queue Standby → buffered → merged at master_approval.

Depends on: #162 (AUTH-2 — producer channel Active).

### #228 — Pattern4BandsTest — rung 11
Blocked by: #298.

Pin Hub Character band protocol per HEP-CORE-0033 §12: `BAND_JOIN_REQ`/`ACK`/`NOTIFY`, `BAND_LEAVE_REQ`/`ACK`/`NOTIFY`, `BAND_MEMBERS_REQ`/`ACK`, `BAND_BROADCAST_REQ`/`NOTIFY`.

**SCOPE EXPANSION 2026-06-29 (#154 C3 absorption — dual-hub variant):**
Dual-hub TEST_F absorbs contract from retired L3 test `role_api_base_source_hub_uid_disambiguates_dual_hub` (A3 audit).

Dual-hub scenario: 2 plh_hub subprocesses + 1 plh_role (processor with 2 presences) + 2 external observer roles.  Processor joins `!band_a` on hub-A and `!band_b` on hub-B via per-connection BRC.  External observers join → each broker fans BAND_JOIN_NOTIFY to processor on corresponding connection.

**Contract (HEP-CORE-0023 §7 + HEP-CORE-0033 §18.3 / §19.4):** role-side BRC on_notification lambda MUST populate `IncomingMessage::source_hub_uid` with connection's broker endpoint (§19.2 dedup key).  Assertions: both notifications carry non-empty `source_hub_uid`; distinct values; hub-A notification matches hub-A endpoint; same for hub-B.

Pin via log markers: `event=BandNotifyEnqueued band='X' source_hub_uid='tcp://...'` per enqueue role-side.  VERIFY in `role_api_base.cpp` Phase 2 lambda before assuming.

Depends on: #221 + #298 for dual-hub variant.

### #229 — Pattern4RoleIntrospectionTest — rung 12
Blocked by: #298.

Pin role introspection surface: `ROLE_PRESENCE_REQ`/`ACK` (HEP-CORE-0007), `ROLE_INFO_REQ`/`ACK`, `SHM_BLOCK_QUERY_REQ`/`ACK`.

**SCOPE EXPANSION 2026-06-29 (#154 C3 absorption — dual-hub Class-B fall-through):**
Absorbs contract from retired L3 `role_api_base_wait_for_role_dual_hub_fallthrough` (A3 audit).

`wait_for_role` is Class-B (role-bound) wrapping `ROLE_PRESENCE_REQ` — belongs here.  HEP-CORE-0033 §18.3: Class-B queries MUST fall through across ALL connections; pre-A3 `wait_for_role` only polled `brc_for_role()` (= connections()[0]).

Dual-hub scenario: 2 plh_hub + 1 target role (hub-B only) + 1 querier (dual-presence).  Querier calls `wait_for_role(target_uid, 2000)`.  Pre-A3 returns false; post-A3 returns true via fall-through to connection 1.

Contract: `wait_for_role(target_uid, kMidTimeoutMs)` returns `true`.  Pin path via markers: querier emits ONE `event=RolePresenceReqSending role_uid='target' connection_idx=0` then ANOTHER at `connection_idx=1` (proves iteration).  Hub-B ACK marker confirms second query reached.

Depends on: #221 + #298.

### #230 — Broker-reachable-before-REG_REQ contract — production-side decision

Production-side contract question surfaced during Pattern 4 rung 2 bring-up (#221): behavior when role's BRC sends REG_REQ before broker ready?

Current (informal): operator responsible for ordering; libzmq DEALER `connect()` non-blocking; `BrokerRequestComm::register_channel(timeout=N)` ONE shot, no retry; HEP-CORE-0023 §2.5.3 "Disconnection is terminal" but pre-connection slowness isn't disconnection.

Production race scenarios: systemd unit ordering without `After=broker.service`; K8s init containers parallelism; `docker compose up`; container orchestrator restart cascades; operator typo.

Options: (A) document ordering invariant in HEP-CORE-0023 §2.5 — clearest, pushes to operator; (B) `register_producer_channel` retry-with-backoff — risks hiding auth failures; (C) pre-flight broker reachability check in `RoleHandler::start_connections()` — cleanest separation; (D) keep as-is + scripts handle in `on_init` — most flexible, transfers complexity to scripts.

Recommendation 2026-06-14: A+C combined.  B explicitly avoided (masks key-mismatch indefinitely).

Not a blocker on AUTH chain.

### #296 — L4 hub-death observability test — pin HEP-CORE-0023 §2.5.3 cross-process
Blocked by: #298.

**Origin:** L3 test `HubHostIntegrationTest.HubHost_Shutdown_BreaksClientConnection` + worker DELETED 2026-06-27.  Reason: cannot be expressed at L3.  Under shared `pylabhub::hub::get_zmq_context()` libzmq CURVE engine suppresses `ZMQ_EVENT_DISCONNECTED` on clean peer close — false negative no L3 rewrite can fix.

**SCOPE EXPANSION 2026-06-29 (#154 C3 absorption):**  absorbs 3 dual-broker L3 tests retired from `datahub_role_state_workers.cpp` (dual `BrokerService` in one process violates HEP-CORE-0036 §7.1 single-pumper invariant).

| Retired L3 | Contract at L4 |
|---|---|
| `role_api_base_hub_dead_peer_keeps_role_alive` (A2) | Kill PEER broker (subprocess idx 1).  Assert `is_connection_alive(0)==true`, `is_connection_alive(1)==false`, role does NOT `request_stop` (no `event=HubDeadStopRequested`), `connections_alive_count() == 1`. |
| `role_api_base_hub_dead_transitions_presences_to_deregistered` (R3.3) | After peer death, presence on dead conn MUST transition `Registered → Unregistered` (per HEP-CORE-0036 §4.3.3 amendment 2026-06-16 — NOT Deregistered).  Master presence stays Registered.  Pin via `event=PresenceStateTransition` marker. |
| `role_api_base_hub_dead_master_exits_role` (D1/D2 + S1) | Kill MASTER (idx 0).  EXACTLY ONE `event=HubDead is_master=true` per conn lifetime (S1 no-reconnect), `default_hub_dead` fires `request_stop()` with `stop_reason="hub_dead"`, BRC `reconnect_disabled() == true` for both conns (S1: `ZMQ_RECONNECT_IVL=-1`). |

**Contract (single-hub baseline HEP-0023 §2.5.3):** BRC observes `ZMQ_EVENT_DISCONNECTED` within heartbeat-timeout, fires `on_hub_dead`, flips `is_connected()` to false, `reconnect_disabled() == true`.  Post-disconnect REG_REQ must FAIL FAST at layer-1 gate.

**MANDATORY first step — overlap audit:** evaluate existing L4 tests under `tests/test_layer4_plh_hub/` + `tests/test_layer4_plh_role/`.  Decide MERGE / REVISE / CREATE NEW.

**Production marker contract additions required:**
- `event=HubDeadDetected connection_idx=N is_master={true|false}` — VERIFY in role_api_base.cpp before assuming.
- `event=HubDeadStopRequested reason=hub_dead` — may need adding.
- `event=PresenceStateTransition channel='X' from=Registered to=Unregistered` — VERIFY in role_handler.cpp `mark_connection_disconnected`.

### #297 — C3 role_state migration

Migrate role_state_machine workers (12 single-broker L3 tests).  Retire 7 dual-broker tests; absorbed contracts tracked in TESTING_TODO §"Test retirements" (#296 expanded, #228 expanded, #229 expanded, #223→#299 new task) all blocked on #298.

### #298 — Pattern4Setup multi-hub extension (prereq for dual-hub L4 tests)
Blocks: #296, #228, #229, #299.

Extend `tests/test_framework/pattern4_helpers.{h,cpp}` for N>1 hubs in single Pattern 4 scenario.  Required by dual-hub L4 absorptions of L3 tests retired during #154 C3.

Current single-hub: `Pattern4Setup.broker_endpoint` string; `Pattern4Setup.curve.hub` single keypair; `make_pattern4_setup({role_uids})` picks ONE port + generates ONE hub keypair.

Required extension: replace `broker_endpoint` with `std::vector<HubInfo>` where each `HubInfo` carries `{endpoint, hub_uid, hub_keypair}`.  Each hub has OWN keypair (production-honest; sharing was L3 workaround).  New constructor `make_pattern4_setup(n_hubs, role_uids)` or `make_pattern4_setup_multi`.  Update `Pattern4Setup` JSON schema `hubs: [{ endpoint, hub_uid, public_z85, secret_z85 }, ...]`.  Update consumer sites.  Single-hub call-sites stay backward-compatible.

**Why required:** 7 dual-broker L3 tests retired during #154 C3 need L4 home.  L3 form violated HEP-CORE-0036 §7.1 single-pumper (two `BrokerService` in one process = two pumpers = PLH_PANIC in `ZapRouter::pump_one`).  L4 fixes by per-subprocess hub (each subprocess own ZMQ context + own ZapRouter → unique pumper per process).

Out of scope: N>2 (design vector to generalize, exercise N=2); federation peering (#75, #105); dual-hub role in same subprocess (OK — one BRC poll loop per connection, serialized via role's single ZapPumpThread per AUTH-2 #162).

~50-80 LOC to pattern4_helpers, ~5-10 LOC per call-site update.

### #299 — Pattern4HeartbeatTest dual-hub variant — per-presence iteration
Blocked by: #298.

Extend shipped #223 with dual-hub TEST_F absorbing `role_api_base_dual_hub_heartbeat_per_presence` (C2 audit).

Contract (HEP-CORE-0033 §19.3 step 3): heartbeat is per-PRESENCE, not per-connection.  Dual-hub processor (Consumer on hub-A + Producer on hub-B) emits exactly TWO heartbeats per tick.  Role-side fix pins post-C2 `handler_->presences()` iteration replacing pre-C2 short_tag string-branching.

Dual-hub scenario: 2 plh_hub + 1 upstream producer (hub-A ch_in, provides R6 kLive gate) + 1 processor (dual presence: Consumer hub-A ch_in, Producer hub-B ch_out).  Install heartbeat 50ms.

Assertions:
1. Hub-A observes consumer-presence for processor role_uid on ch_in with `first_heartbeat_seen=true role_type='consumer'`.
2. Hub-B observes producer-presence on ch_out.
3. Hub-A does NOT have producer-presence for ch_out (no cross-hub leakage).
4. Hub-B does NOT have consumer-presence for ch_in.

Production markers additions may be required (VERIFY first):
- `event=HeartbeatTickPerPresence channel='X' role_type='Y' connection_idx=N` role-side per tick per presence.
- `event=FirstHeartbeatReceived channel='X' role_type='Y' role_uid='Z'` broker-side per (channel, role_type) on first HB.

---

## §7 Native ABI + engine parity

### #84 — N2: NativeEngine build_api_(HubAPI&) — extend beyond MVP

B13 closed the gap (native is now hub-script engine) but MVP exposes only log + uid + name + request_stop.  Missing: HubAPI read accessors (list_channels, get_channel, list_roles, list_bands, list_peers, query_metrics); control delegates (close_channel, broadcast_channel, post_event, augment_*); event-shape callbacks (on_event, on_consumer_added, on_role_registered per HEP-0033 §12).  Each needs `ctx_X_hub` adapter wrapping HubAPI method as C fn ptr returning JSON-as-C-string where applicable.  Effort L.

### #85 — N3+N4: canonical native plugin on_init/on_stop sig + lifecycle module cleanup

N3: All canonical native plugin examples (`good_producer_plugin.cpp:96`, `native_multifield_module.cpp:72`, this session's demos) declare `on_init(const char *args_json)` but documented ABI in `native_engine_api.h:315-319` is `void name(void)`.  Framework's FnVoidNoArgs typedef calls no-args; extra param works by accident.  Fix all canonical examples to match documented ABI.

N4: NativeEngine registers lifecycle module with no-op shutdown (`native_engine.cpp:566-584`) while real finalization is in `finalize_engine_()`.  Misleading; document or remove.  Effort S.

### #86 — N5+N6: README_NativePlugins.md + cmake/pylabhubNativePlugin.cmake (user-oriented)

N5 (`docs/README/README_NativePlugins.md`): operator-facing native plugin guide.  Cover: required exported symbols (native_abi_info / native_init / native_finalize / on_*); filename convention (plugin.so); PLH_DECLARE_SCHEMA macro + schema string format (`name:type:count:dim_flag`, pipe-separated, trailing 0 is dim-marker NOT byte offset); on_init/on_stop signatures per N3 fix; request_stop() vs set_critical_error(msg) distinction; on_band_message signature (`const plh_band_message_args_t *args`); build command (g++ -shared -fPIC -std=c++20); N6 CMake helper.  Currently scattered across header comments + L2 examples + HEP-0028.

N6 (`cmake/pylabhubNativePlugin.cmake`): USER-ORIENTED CMake module.  Plugin author drops into own project, `find_package(pylabhubNativePlugin REQUIRED PATHS /path/to/pylabhub/install)`.  Deduces include + lib paths.  Provides `plh_add_native_plugin(target SOURCES files...)`.  Must NOT be project-internal — it's a deliverable.  Effort M.

### #87 — N7+N10: Three-engine doc parity (README_Scripting_{Python,Lua,Native}.md)

Currently Python has full coverage (HEP-CORE-0011 + README_Deployment.md §5-8), Lua ~10 lines in HEP-0011, Native only arch notes in HEP-0028.

Plan: 3 sibling docs `README_Scripting_Python.md` + `_Lua.md` + `_Native.md` — identical 8-section structure (when to use → lifecycle → data structure → API → example → build/setup → pitfalls → cross-refs).

N10 absorbed: Lua-specific notes (`api.X(...)` not `api:X(...)`, FFI access, no-numpy gotcha) into README_Scripting_Lua.md.  README_Deployment.md §5-8 keeps deployment-only + cross-refs.  Effort L (~2-3 sessions).

### #88 — N8+N9: Bench variants — scalar dispatch + multi-size sweep

N8 (Lua max-rate scalar bench, no random fill): current 4 KB-with-random-fill measures random-fill more than engine dispatch.  Pure dispatch bench (scalar payload, no work in callback) isolates per-slot framework cost (acquire + commit + checksum + log-flag check) across engines.

N9 (multi-slot-size sweep): rerun three-engine bench at 16B / 4KB / 64KB / 1MB.  Reveals where checksum dominates vs script work.  Effort M.

### #89 — N11: Cross-engine on_band_message signature parity audit

B-fix this session was native on_band_message signature (was 3 separate `const char*` args, correct is `const plh_band_message_args_t *args`).  Audit PythonEngine + LuaEngine on_band_message to confirm documented unambiguously + pinned by tests.  Python: `def on_band_message(band, sender, body, api)` per `python_engine.cpp:1255`.  Lua same.  Add explicit doc note + L2/L3 test that fails if sig changes.  Effort S.

### #194 — Native ABI parity sweep — wire Native RoleAPI surface to Lua/Python parity

Source incident: 2026-06-10 — Stage 1C (#190) claimed Native parity for `is_channel_ready` "via role_api() direct C++".  Hook does not exist in Native C ABI.  Native plugins receive only `PlhNativeContext*` with fn-ptr slots wired by `NativeEngine::build_api_`.  Anything not in struct is unreachable.

**Phase A ✅** (9627161f).  API v3→v4.  PlhNativeContext gains allowed_peers + is_channel_ready + queue_mechanism.  invoke_on_allowlist_changed replaced no-op stub.

**Phase B1 ✅** (3c9b6829).  PlhNativeContext gains metrics_json + is_in_band + update_flexzone_checksum + set_verify_checksum.

**Phase B2 ✅**.  API v4→v5.  PlhNativeContext gains out_capacity + out_policy + in_capacity + in_policy + last_seq.

**Phase B3 — PENDING:** Inbox subsystem (HEP-CORE-0027).
Requires stateful handle abstraction across C ABI:
- opaque `plh_inbox_handle_t*` (host-side `scripting::InboxHandle*` cached by target_uid in NativeContextStorage)
- `ctx_open_inbox(ctx, target_uid) → handle or NULL`
- `inbox_acquire(handle) → slot ptr`
- `inbox_send(handle, timeout_ms) → int` (1=success, 0=timeout, -1=error)
- `inbox_discard(handle)`, `inbox_is_ready(handle) → int`, `inbox_close(handle)`
- `ctx_clear_inbox_cache(ctx)`

Only audit item needing stateful handle lifetime.  Est ~120 LOC + ~40 LOC tests.

**Decided NON-gaps:** `flexzone` (accessed via `plh_tx_t.fz` / `plh_rx_t.fz`); `version_info` (compile-time PLH_NATIVE_API_VERSION); `in_channel`/`out_channel` (ctx const fields); `shared_data` (Native has static vars, no abstraction needed); `report_metrics` plural (loop on singular).

### #204 — #194 follow-up: split include/ into plugin-author + EmbeddedAPI public surfaces

M7 install allowlist reverted because EmbeddedAPI public header `<plh_datahub.hpp>` transitively depends on `utils/broker_service.hpp` + `utils/data_block.hpp` + `utils/hub_zmq_queue.hpp` — same headers framework-internal from plugin-author perspective.

Fix: split install layout.  (a) `include/plugin/` for HEP-CORE-0028 §2.4 Tier 2 (just `native_engine_api.h` + `native_invoke_types.h`); (b) keep `include/utils/` wholesale for EmbeddedAPI.  Update HEP-CORE-0028 §1 + §13 to use `<plugin/native_engine_api.h>`; update demo plugins under share/ and tests/.  Plumbing scope, not v6 ABI cleanup.

### #232 — Engine parity-test contract — cross-role observation surface sweep

HEP-CORE-0011 §"Cross-Engine Surface Parity" Read-only observation surface principle (added 2026-06-15) mandates: each engine (Lua/Python/Native) MUST keep one cross-role parity test exercising every observation surface on every role kind, pinning both wrong-side sentinel shape AND right-side empty-but-correct shape.

Without this, new surfaces silently degrade the principle — exactly how Native ABI ended up missing `producers` when AUTH-1 added it to Lua+Python without an engine-uniform forcing function.

Scope (minimum): Lua parity test + Python parity test + Native parity test each covering every row in HEP-0011 Cross-Engine Surface Parity table called from prod/cons/proc; assert wrong-side sentinel shape.

Surfaces: `queue_mechanism(side)`, `stop_reason()`, `out_policy()`, `in_policy()`, `is_channel_ready(channel)`, `is_in_band(channel)`, `allowed_peers(channel)`, `producers(channel)`, `band_members(channel)`, `metrics()`.

When new row added to HEP-0011 table, parity tests must extend to cover it.

### #233 — Native C ABI: add producers() observation surface — close engine-parity gap

HEP-CORE-0011 §"Cross-Engine Surface Parity" lists `producers(channel)` with: "visitor (no alloc) — `*_contains` + `*_count` siblings pending under #194" for Native C ABI.

Currently Native C ABI exposes `allowed_peers(channel)` (visitor + contains + count) but NOT `producers(channel)`.  Add consumer-side observation surface per symmetric read-only observation principle.

Scope:
- `src/include/utils/native_engine_api.h`: `producers` fn ptr reusing `plh_allowed_peer_visitor`; optional `producer_contains`, `producer_count`.
- `src/utils/service/native_engine.cpp`: `ctx_producers` shim calling `RoleAPIBase::producers(channel)` (returns empty when not applicable — symmetric stub); `hub_stub_producers` returns -1.  Wire in role-context build (~line 1132); hub stub (~line 1028).
- Tier-2 C++ wrapper `ProducersHandle` mirroring `AllowedPeersHandle`.
- README §7.5 doc update (line 585-594): add `producers` after `allowed_peers`.
- Bump C ABI version + README + plugin test coverage per #194 / HEP-CORE-0028.

Forcing function: #232 parity-test will fail on Native until this lands.  Recommendation: ship #232 first to make gap detectable.

### #234 — producers_contains / producers_count siblings — engine-parity gap with allowed_peers cluster

Two-axis parity gap from 2026-06-15 side-by-side analysis:
- Axis A — `producers` family lacks contains/count siblings across ALL engines.
- Axis B — Lua lacks `allowed_peer_contains` / `allowed_peer_count` (existing-family siblings on Python + Native).

Scope:
- Axis A: Python ConsumerAPI + ProcessorAPI (real impl); Python ProducerAPI (engine-parity stub); Lua bindings; Native C ABI (#233 / #194 follow-up).
- Axis B: Lua `allowed_peer_contains` + `_count` in lua_engine common closures.  Match Python sigs.
- Update HEP-CORE-0011 Cross-Engine Surface Parity table.
- .pyi stub entries for ProducerAPI `producers_contains`/`_count`.

---

## §8 Script engine surfaces

### #73 — HEP-CORE-0033 Phase 10 doc closure

Per-producer metrics tree shape amendment in HEP-CORE-0019 §9 + cross-reference survey.  Carryover from Wave-M2.5 step 8.  Phases 1-9 shipped end-to-end; Phase 10 is doc-only tail.  Currently HEP-0019 §9 marks "Phase 6: per-presence keying" ✅ but doesn't spell out per-producer metrics tree shape; cross-refs between HEP-0019 / -0023 / -0033 about counters/metrics/state aren't audited for drift.

### #75 — HUB_TARGETED_ACK wire frame (HEP-0033 §12.3.6)

Federation peer-message ACK wire frame.  C++ surface in place (`send_hub_targeted_msg` queue + `handle_hub_targeted_msg` handler) but ACK frame back to originating hub deferred.  Adds confirmation targeted message reached destination peer.  ~half day.

### #76 — Script reload (#2) — promote tech_draft to HEP + implement

`api.request_reload(path)` → state-machine guard (`EngineState::ReloadPending`) → cycle-boundary reload → cleanup hooks (`on_stop old → reload → on_init new`).  Per-engine: Python `importlib.reload`, Lua `luaL_loadfile+pcall`, Native `warn+false`.

Full design: `docs/tech_draft/SCRIPT_RELOAD_DESIGN_2026-05-20.md`.  ~1.5 days.  Currently stub `virtual bool reload_script() { return false; }` exists; design promotes to real feature with explicit guards.  HEP-CORE-0011 §"Script Reload" to be added on ship.

### #77 — Tier 2 dynamic callbacks (#3) — stubs + Python impl + HEP

Per 2026-05-20 decision:
1. Flip ScriptEngine default for `invoke_event`/`invoke_query`/`register_callback` from silent no-op to throwing "not implemented".
2. PythonEngine — full minimal impl (registry `std::map<string,py::object>` + `register_callback` pybind binding + `invoke_event`/`invoke_query` bridges under GIL).
3. LuaEngine — override that throws (per-thread state model incompatible without dispatcher-thread, deferred).
4. NativeEngine — override that throws (plugin handles dynamic dispatch).

HEP work: promote `docs/tech_draft/engine_callback_tiers.md` into HEP-CORE-0011 §"Callback Tiers — Standard vs Dynamic"; archive tech_draft.  Tests: Python Tier-2 round-trip; Lua/Native negative.  ~1 day.

### #80 — B6/B7: rx.fz binding + processor flexzone side

B6: README §8.4 claims consumer `rx.fz` but ConsumerAPI's RxChannel pybind binding (`consumer_api.cpp:191`) only exposes `.slot`.  Fix EITHER bind `.fz` OR update §8.4 to use `api.flexzone()`.

B7: README §8.4 example doesn't show processor needs `api.flexzone(api.Tx)` side arg.

Both doc/binding parity issues on same surface — bundle.  Effort S.

### #81 — B8: plh_pyenv install --requirements in demo setup

numpy not in bundled python by default.  Demos that need numpy (single-processor-shm with random fill, processor scripts) rely on ad-hoc `pip install numpy`.  Fix: add requirements.txt to demo dirs needing numpy + chain `plh_pyenv install --requirements demo/requirements.txt` into setup_commands.  Effort M.

### #82 — B10: api.band_join in on_init — surface failure instead of silent None

Calling `api.band_join` from `on_init` silently returns None because handler isn't up until Step 6 of `worker_main_`.  Doc workaround (lazy band_join in data callback) shipped in HEP-0011 §"API availability per callback".

Code fix candidates: (a) defer call (queue + replay after handler ready); (b) raise `runtime_error`/throw — (b) smaller.  Effort S.

### #94 — Implement HEP-CORE-0021 §16.5 ephemeral-binding production path

Currently no production code calls `BrokerRequestComm::send_endpoint_update`; sync REQ/REP API shipped 2026-05-21 (`8228f1ac` + `66e71894`) is forward-looking infrastructure waiting for this task as first real consumer.

When implementing ephemeral binding production path, producer / processor-tx roles will call `send_endpoint_update` at startup once actual bound endpoint known (post-ZMQ bind, kernel-assigned port replaces configured-or-zero port).

Until this lands, sync API contract pinned only by L3 tests (`test_datahub_zmq_endpoint_registry.cpp`).

### #95 — SCHEMA_REQ + METRICS_REQ — handlers without callers, KEEP-RESERVED or DELETE

RE-SCOPED 2026-06-27 — original claim "handlers without callers" is FACTUALLY INCORRECT per code verification.  `broker_service.cpp:1248,1274` dispatch tables route to `handle_schema_req` (line 3558) and `handle_metrics_req` (line 5233); both actively called.

Revised: KEEP-RESERVED based on HEP-0034 §10.3 + federation #105 schema requirements; no "zero callers" framing.  Action: document in HEP-0034 §10.3 that these handlers are part of production dispatch surface (decide KEEP-RESERVED, KEEP-PRODUCTION, or PROMOTE-EXPOSED-API).

### #191 — Future: CURVE-wire InboxQueue per HEP-0036 §9.3 (inherits channel allowlist)

Per HEP-0036 §9.3 design intent + discussion 2026-06-10:

InboxQueue currently NO CURVE wiring (`hub_inbox_queue.cpp:237-264` plain ROUTER bind).  HEP-0036 §9.3 declares inbox CURVE inherits from data channel:
- Reuse role-wide ZAP handler
- Inherit data channel's allowlist (no separate broker admission)
- Inbox lifetime ⊆ data channel lifetime
- No per-inbox keypair

Implementation:
1. `InboxQueue::start()` add CURVE_SERVER setsockopt + identity keypair binding
2. Inbox ZAP queries shared process-wide ZAP handler (NOT separate)
3. Inbox allowlist mirrors parent ZmqQueue's allowlist (link to PeerAdmission of data channel)
4. State machine: when parent ZmqQueue Standby/Configured/Active, inbox follows
5. `InboxClient::start()` add CURVE_SERVERKEY setsockopt with peer's public key

HEP-0036 §12 Phase 8 says "verification only" — inaccurate; wiring is genuinely missing.  Will update HEP-0036 plan when picked up.

Not part of AUTH-1..7 critical path.  Pick up after AUTH-7 (L4 end-to-end auth-gated data flow) ships.

### #192 — Audit: api.band_broadcast returns void — script can't detect failure

`RoleAPIBase::band_broadcast(channel, body)` returns `void` (`role_api_base.hpp:355`).  `ProducerAPI::band_broadcast` likewise `void`.  Script has no way to know if band exists / role in band / BRC connected / broker rejected / wire error.

Should change to `bool` or status object (matching `band_leave` which returns bool).  Pre-existing user-error-visibility gap; independent of HEP-0036 work.  Not part of AUTH-1..7 critical path.

---

## §9 Test framework hardening

### #156 — Doc hygiene batch — archive empty/resolved transient docs, promote stable taxonomies

Audited 2026-06-05 against code.  Six items:

1. ARCHIVE `docs/code_review/REVIEW_CatchBlocks_2026-05-01.md` — template created, body never populated.  No audit value.
2. ARCHIVE `docs/tech_draft/HEP-0036_review_open_items.md` — all Tier-1 items marked RESOLVED (T1/Q1 locked 2026-05-28).  Fold lasting context into HEP-0036 if not already there.
3. PROMOTE `docs/tech_draft/engine_callback_tiers.md` — stable reference taxonomy (3-axis matrix), not a draft.  Merge into HEP-CORE-0011 or new HEP.
4. ASSIGN task ID to `docs/tech_draft/abi_check_facility_design.md` OR merge design into HEP-CORE-0026/0032 §"Known Gaps".  Currently orphaned (no task #).
5. ARCHIVE `docs/tech_draft/future-persistence-and-discovery/` — Sept 2025 prototype, code unrelated to current C++ codebase.  Move to `docs/archive/ideas/`.
6. UPDATE `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` Table §8 with closing commit hashes for C2 + X1.

Each per `docs/DOC_STRUCTURE.md` archive conventions + DOC_ARCHIVE_LOG.md entry.

### #179 — Factor FlexzoneInfoCache::derive() to eliminate test-side duplication

HIGH-risk duplication identified 2026-06-06.

Site: `tests/test_layer3_datahub/workers/role_api_flexzone_workers.cpp:194-202` (tx) + `:222-228` (rx) — `build_payload_pair` helper does `physical = align_to_physical_page(compute_schema_size(fz_spec, packing))` + assigns has_tx_fz, tx_logical_size, tx_physical_size (and rx mirror).

Production: `src/utils/service/role_host_frame.cpp:210-228` `setup_infrastructure_` step 6.5 — same derivation.

Risk: adding new cache field or changing packing-axis updates production but leaves test silent → production-vs-test runtime divergence.

Fix: expose `FlexzoneInfoCache::derive(const FzSpec &fz_spec, const std::string &packing, Side side)` as public helper.  Call from both `setup_infrastructure_` step 6.5 AND `build_payload_pair`.  Remove BYPASS comment block from test.

### #180 — Consolidate ScriptEngine test mocks (RecordingEngine + MockEngine)

MEDIUM-risk duplication identified 2026-06-06.

Sites: `test_dispatch_notifications.cpp:124` (RecordingEngine, overrides 29 of 36 ScriptEngine virtuals) + `test_hub_api.cpp:397` (MockEngine, overrides 29 same shape).  Both reimplement name→bool has_callback switch table.

Risk: adding new ScriptEngine virtual (e.g. `invoke_on_band_quorum_lost`) leaves both mocks compiling thanks to default no-op virtuals — but new callback never exercised in tests.  Hidden coverage loss.

Options: (a) Production `NoOpScriptEngine` base shipping default virtuals; both mocks derive.  Reduces test code ~60% + makes new virtual visible (forces override or accept no-op explicitly).  (b) Share NotificationSwitchTable enum/dispatcher via macro between two test files.

Needs design discussion (which matches HEP-CORE-0011 ScriptEngine contract).  Likely belongs to N3+N4 (#85) or standalone.

### #216 — Queue-state Slice A2 — B1 PULL state rollback + B2 PUSH zap_handle symmetric cleanup

Split from #215 to keep per-commit surface narrow.  Both in `src/utils/hub/hub_zmq_queue.cpp`.

B1 — PULL `apply_master_approval` leaves half-Configured state on `start()` failure.  hub_zmq_queue.cpp ~983-1013: `set_producer_peers` succeeds and peer[0] promoted into `endpoint`+`server_pubkey_z85_` (only when previously empty); if `start()` fails, apply_master_approval returns false but field promotions persist.  Broker retry with different peer[0] hits "only-when-empty" guard at 1000-1004 → stale endpoint paired with new pubkey → wrong handshake target.

Fix shape: save pre-promotion values before promoting; on `start()` failure rollback.  OR drop "only-when-empty" guard so subsequent applies overwrite (changes invariant — needs design check first).

B2 — PUSH `start()` catch handlers leave `zap_handle_` populated.  hub_zmq_queue.cpp:1330-1356: catch handlers reset running_/mechanism_/close socket but not `zap_handle_`.  ZapRouter still has domain registered pointing at queue with closed socket.  Self-cleans on retry (emplace destructs prior handle) or on ~ZmqQueue → stop(), but asymmetry fragile.

Fix: catch handlers add `pImpl->zap_handle_.reset(); pImpl->resolved_zap_domain_.clear();` — symmetric with stop() at 1488-1501.

Tests in `zmq_queue_auth_workers.cpp`:
- `pull_state_rollback_on_start_fail`
- `push_zap_handle_clean_after_start_fail`

Depends on #215 landing first.  Consider folding into Slice B if priorities reshuffle.

### #239 — apply_master_approval silent-skip on second call — add log + clarify intended semantics

`ZmqQueue::apply_master_approval` at `hub_zmq_queue.cpp:996-1006` promotes `producer_peers.front()` ONLY when fields empty.  **On second-or-subsequent call, guards silently skip** — socket stays connected to original endpoint+serverkey.

Why matters: silent hard to diagnose; intent ambiguous ("first ACK wins" vs runtime updates re-target); connection to composite tracker #240.

Proposed fix:
1. Add `else` branch with `LOGGER_WARN` on second-call skip.
2. Expand comment above guard to make intent explicit.
3. Add L2 test asserting silent-skip + warn-log.

In scope: log + comment + test.  Out of scope: multi-producer fan-in or runtime re-target (#240).

### #240 — [DESIGN] Multi-producer fan-in + runtime peer-set updates — geometry constraints composite

Three interlocking gaps in Stage 1A consumer-side design:

**Gap 1** — Multi-producer fan-in not implemented.  `hub_zmq_queue.cpp:990` uses `producer_peers.front()` only.  N≥2 producers: ZAP admits all, socket connects to peer[0] only, no log/error.

**Gap 2** — Runtime `CHANNEL_PRODUCERS_CHANGED_NOTIFY` consumer-side not wired.  AUTH-1 #103 D4/D5 in-progress.

**Gap 3** — ZAP cache vs socket-state divergence post-runtime-update.

**Geometry constraint proposal (2026-06-16):**

| Geometry | Implementation | Producer | Consumer |
|---|---|---|---|
| Fan-in (N→1) | ZmqQueue PULL only | N producers connect | 1 consumer binds PULL |
| Fan-out (1→N) | ShmQueue only | 1 producer writes ring | N consumers read same ring |
| Work distribution (1→N round-robin) | ZmqQueue PUSH→PULL | 1 PUSH binds | N PULLs connect |

SHM is inherently broadcast; ZMQ PULL is inherently fan-in.  Each queue is one pattern; no queue tries to be all three.  Push complexity to role composition (bridging roles) rather than queue capabilities.

Already half-encoded (`hub_zmq_queue.hpp:115-117`).  Missing: explicit HEP statement that THIS IS THE FULL DESIGN — permanent geometry contract.

Deliverables:
- Phase 1 — Design decision (HEP-CORE-0017 §3.3 amendment)
- Phase 2 — Impl multi-producer fan-in for ZmqQueue (Gap 1)
- Phase 3 — Wire runtime notify consumer-side (Gap 2 — task #103)
- Phase 4 — Verify ZAP-vs-socket consistency (Gap 3)

### #241 — Test-faithfulness principle — mechanical enforcement design

Test-faithfulness principle is well-articulated across codebase but has ZERO mechanical enforcement.

Where principle currently lives (5 places): CLAUDE.md; IMPLEMENTATION_GUIDANCE.md § "Test Strategy"; README_testing.md § "Choosing a test pattern"; feedback memories (9 files: feedback_test_bypass_explicit, feedback_tests_replicate_production_scenarios, feedback_test_outcome_vs_path, feedback_no_mocks_via_observability, feedback_test_layering_and_no_mocks, feedback_refresh_guidance_before_work, feedback_test_lifecycle_pattern, feedback_test_rigor, feedback_test_design); per-test file docstring for L2 bypass tests.

Enforcement gap: no CI lint catches test constructing inline JSON mirroring production wire; no static check warns when test pokes cache directly bypassing production mutator; no PR template forces "this test mirrors production" articulation; no file-header validator catches missing bypass-declaration.

Options (cheapest first):
1. PR-template checklist — 3-line declaration: Production path mirrored / Bypasses / Why bypass.
2. File-header grammar enforcement — CI script requiring `@test-faithfulness:` block or `@pattern: 1|2|3|4` + `@bypass:`.
3. Canonical-helper enforcement — identify each production single-source helper, CI grep flags hand-rolled JSON literals.
4. Test-framework affordances (carrot) — make production path the easier path.
5. Centralize principle into ONE canonical doc.

Recommendation: Options 1+5 as single cheap PR (~2-3 hours); Option 2 as follow-up.  Options 3+4 bigger — defer until framework matures.

### #243 — Pattern 3 outcome-artifact contract — strengthen IsolatedProcessTest beyond exit-code-as-verdict

Pattern 3's current shape relies on single oracle: subprocess's gtest return code.  Works when subprocess writes rigorous in-process EXPECT/ASSERT against real contract.  DOES NOT catch: weak in-subprocess assertions; no-op setup that silently skips contract; subprocess writing wrong output but reporting success.

Highlighted 2026-06-16 CI failures: SIGABRT (rc=134) propagated correctly (easy case); L4 PlhHubCliTest.NoScriptPasses (rc=143 SIGTERM) looked like "keygen failed" — actual: vault file WAS written, failure was teardown hang (#242).

Proposed direction (HEP-level discussion): every Pattern 3 worker that claims outcome must write to known artifact path parent can inspect after subprocess exits.  Parent's pass/fail combines exit code + artifact verification.

Concrete design questions:
1. Mandatory worker-breadcrumb file (`<tmpdir>/_assertions_run.txt`).
2. Mandatory outcome-artifact for state-producing tests.
3. Worker stdout protocol vs file artifacts.
4. Backwards compat — grandfather old tests, opt-in via `expect_artifacts({...})`.
5. Relationship to existing L4 helpers.

Suggested next steps: draft `docs/tech_draft/DRAFT_pattern3_outcome_artifacts.md`; discuss; update README_testing.md Pattern 3; promote to HEP; refactor IsolatedProcessTest + 2-3 model workers; migrate remaining tests gradually.

Effort L.  MEDIUM priority — not blocking AUTH critical path.  Cleanest after #241 (test-faithfulness principle).

### #284 — Audit test-access patterns — narrow-accessor vs wide-pointer/setter discipline

Marker for deeper-look review following S2c-6 design discussion.

Context: Going with option (B) — narrow named accessor methods on `DataBlockProducer` (`active_consumer_count()`, `last_heartbeat_ns()`, `consumer_heartbeat_ns(i)`, `flexzone_schema_hash()`, `datablock_schema_hash()`) — instead of extending wide-surface `DataBlockDiagnosticHandle` to fd-source.  (B) matches precedent like `ZapRouter::registered_domain_count_for_test()` + `ThreadManager::reset_process_detached_count_for_testing()`.

Original design principle: "We designed this to avoid too much add-on to internal code without obvious use cases."  Original wide-surface (DataBlockDiagnosticHandle, HubStateTestAccess) was DELIBERATE — avoid growing production class surface with test-only inspection.

Audit:
1. `DataBlockDiagnosticHandle` (`data_block.hpp:1169`) — returns raw `SharedMemoryHeader*` + `SlotRWState*`.  38 test + 6 production (data_block_recovery.cpp) consumers.  Question: retire test-side usage after S2c-6 migration?  What fields do remaining 30 test consumers read?  Production side (recovery) stay wide-pointer or split into `DataBlockRecoveryHandle`?
2. `HubStateTestAccess` (`hub_state_test_access.h`) — 25+ static methods, 309 test usages — LARGEST test-access surface.  Split: `set_*` direct-state setters (~12) HIGH risk; `on_*` FSM event invokers (~13) LOW risk.  Question: how many usages are setters vs invokers?
3. Codebase convention document rule in README_testing.md.

Side observations: compile-gated test hooks (`#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)`) in `role_api_base.hpp` + `role_host_core.hpp` — when warranted?  Narrow-accessor precedents naming convention.

AFTER S2c-6 lands.  NOT BLOCKING #275 close.  Scope substantial — could touch 50-100 test sites + design doc.  Sibling of #243.

---

## §10 Federation + cross-hub

### #105 — Federation protocol design + cross-hub reg/comm verification

HEP-0036 T5 close-out (2026-05-28) deferred full federation design.  MVP inherits HEP-0022 HUB_RELAY_MSG carriage + HEP-0035 §4.3 federation_trust_mode + §4.4 HUB_PEER_HELLO.roles[] augmentation, with NO new wire field on CHANNEL_AUTH_UPDATE.

NOT verified today, needs dedicated protocol design + L4 coverage:
- Channel-name → owning-hub resolution (where does Hub-A's consumer learn channel X lives on Hub-B?)
- Cross-hub CONSUMER_REG_REQ → remote producer end-to-end path
- Reverse path: `allowlist_remove` on consumer death + CONSUMER_DIED_NOTIFY relay
- Multi-peer consistency model (eventual? strong?)
- Retry/backoff under partial peer reachability
- federation_trust_mode "peer_announced" semantics (transitive trust)

Existing L4 dual-hub processor (#44) covers broadcast relay + local-channel dual attachment, NOT cross-hub registration.

Output: new HEP (HEP-CORE-0037 "Federation Protocol" or HEP-0022 amendment) + L4 E2E for cross-hub REG.

---

## §11 Doc hygiene + reorganization

### #236 — json_py_helpers.hpp transitive-include burden — split peer_list_to_py or move to .cpp

`src/scripting/json_py_helpers.hpp` header-only with 3 inline functions.  `peer_list_to_py` (added AUTH-1 close-out #11) needs `AllowedPeer` → header now includes `utils/role_api_base.hpp` (~19 transitive headers).

Consumers, two cost tiers:
- Zero new cost (already include role_api_base): `producer_api.cpp`, `consumer_api.cpp`, `processor_api.cpp`
- Inherit new deps: `python_engine.cpp` (22 direct includes, doesn't need role-API), `hub_api_python.cpp` (6 direct includes, doesn't need role-API)

Options:
- **A (recommended)** — Split.  Move `peer_list_to_py` to new `src/scripting/peer_py_helpers.hpp`.  `json_py_helpers.hpp` reverts role-API-free.  Only 3 role-API .cpp files include new header.
- B — Move all 3 to .cpp.  Trade-off: lose inlining for recursive `json_to_py`.
- C — Hybrid: keep `json_to_py`/`py_to_json` inline; move `peer_list_to_py` to own header (Option A).

Start with A.  Effort S (1-2 hour).  LOW priority.

### #237 — Holistic public/internal header reorganization

Audit `src/include/` + `src/scripting/` + per-area header layout for:
1. Public vs internal header separation.  Only `src/include/utils/detail/` is explicit-internal.
2. Heavy-header transitive include audit.  Known case (2026-06-16): `role_api_base.hpp` pulls 19 transitive headers.
3. `fwd.hpp` strategy — `json_fwd.hpp` precedent.
4. Detail-namespace + private headers under `src/utils/<area>/`.

Concrete known burden cases:
- `json_py_helpers.hpp` — tracked at #236 (local fix)
- Per-role API headers (producer/consumer/processor pybind11 TU)
- Test-side workers pulling role_api_base.hpp for AllowedPeer + few small surfaces

Methodology: per-TU preprocessed-output size (`g++ -E -H | wc -l`); per-header fan-in + fan-out; priority list.

Deliverables: audit report (~1 page); convention doc in IMPLEMENTATION_GUIDANCE.md; top 3-5 splits as separate commit batch.

Out of scope: build-system rewrite; public-API contract change; Python client SDK.

Effort M.  LOW-MEDIUM priority.

---

## §12 Recent review findings

### #301 — broker_service.cpp:3090 audit log writes UNVERIFIED consumer_role_uid (id-mismatch attack surface)

Surfaced by REVIEW 2026-06-30 finding [1] CONFIRMED.  Security/forensics.

Bug: `handle_consumer_attach_req` at `broker_service.cpp:3090-3092` gates authorization on `consumer_pubkey` (correct — cryptographic identity).  BUT #238 stable-marker audit logs at `:3106` (`AttachAuthorized`) and `:3116` (`AttachDenied`) write `consumer_role_uid` verbatim from producer's CONSUMER_ATTACH_REQ, which itself echoes the role_uid string consumer claimed in L2 hello — a cryptographically UNVERIFIED field.

Failure: consumer presents `role_uid='consumer-attacker'` using legitimately-allowlisted pubkey of `consumer-victim`.  Broker authorizes correctly.  Audit log attributes to `consumer-attacker`.  Post-incident triage points at wrong role.

Fix:
- Look up role_uid bound to `consumer_pubkey` via `KnownRolesStore::find_by_pubkey`.
- Use verified role_uid in audit markers.
- If verified role_uid DIFFERS from claim, emit ERROR `event=ConsumerAttachIdentityMismatch verified_uid='X' claimed_uid='Y'` — pure observability, attach still proceeds.

Test pin: L3 broker test — connect with valid pubkey but wrong role_uid claim → assert BOTH `AttachAuthorized role_uid='<verified>'` AND `ConsumerAttachIdentityMismatch claimed_uid='<wrong>'`.

Cross-refs: HEP-0036 §6 PeerAdmission verification; HEP-0035 §4.8 known_roles ZAP gate; #238.

NOT BLOCKED.

### #302 — lua_engine.cpp:2540 mechanism docstring lies — Lua/Python/Native engine parity sweep for ShmCapability

Surfaced by REVIEW 2026-06-30 finding [3] CONFIRMED.  Production-script-breaking.

Bug: Docstring on `api.queue_mechanism()` (`lua_engine.cpp:2540`) read `"Returns a string: 'Curve' / 'Uninitialized'."` — but `hub::mechanism_name` now returns THREE values after #279 (Mechanism enum widening): `'Curve'` / `'ShmCapability'` / `'Uninitialized'`.

Failure: production Lua scripts gate on `api.queue_mechanism(side) == 'Curve'` as "is auth engaged?" check.  Now compare false for every SHM channel.  Scripts skip sensitive work, refuse to publish, or trigger error paths intended for Uninitialized case — silently wrong branching.

**PARTIALLY SHIPPED 2026-06-30 in 444016ec — Lua docstring + migration idiom + parity-note documented.**

Engine parity audit:
- **Lua** — docstring shipped (enumerates 3 values, migration idiom `m ~= "Uninitialized"` or explicit two-value check, Native ABI parity, Python gap).
- **Native** — `native_engine_api.h:87-89` defines PLH_MECHANISM_* macros (0/1/2).  PARITY CONFIRMED.
- **Python** — grep shows NO `queue_mechanism` binding at all.  Scripts cannot query auth-engagement state today.  REMAINING WORK.

Remaining — Python `queue_mechanism` binding:
1. Add pybind11 `.def("queue_mechanism", ...)` mirroring Lua signature.
2. Add Python L3 test pinning 3-value return.
3. Update Python script README + HEP-CORE-0028 §Cross-Engine Surface Parity table.

Cross-refs: #279, #186, #194, HEP-CORE-0028 §Cross-Engine Surface Parity, HEP-CORE-0035 §2 strict-CURVE.

Status: PARTIAL — Lua done, Python binding remains.

### #305 — data_block.cpp:3291 fd-source consumer dropping shared_secret check — defense-in-depth audit

Surfaced by REVIEW 2026-06-30 finding [10] PLAUSIBLE.  Design-intent tracking.

Current: `find_datablock_consumer_from_fd_impl` (`data_block.cpp:3291`, banner :3314-3319, body :3409-3439, comment :3420-3421) INTENTIONALLY skips `shared_secret` check that name-based `find_datablock_consumer_impl` enforced.  Comment: "cap-transport receipt IS the auth in new model" (HEP-0041 §6).

Design-intent rationale (#258 + #266): HEP-0041 attach protocol (broker pre-confirm + crypto_box challenge-response + SCM_RIGHTS via ShmAttachOrchestrator) IS auth boundary.  Receiving memfd via SCM_RIGHTS from peer that completed L2 challenge-response IS cryptographic proof.

Defense-in-depth gap (PLAUSIBLE): if future plugin/code path passes memfd to `set_shm_capability_fd` WITHOUT going through `ShmAttachOrchestrator` — someone wires own AF_UNIX socket + shares fd directly, bypassing L2 challenge + broker pre-confirm — consumer-side attach completes silently as long as fstat-size + header magic match.

Investigation:
1. Inventory callers of `set_shm_capability_fd`: grep every site, map back to provenance.
2. Decide: is "every fd-source caller MUST come from orchestrator" load-bearing (enforce in code) or aspirational (documented contract)?
3. If load-bearing: (a) opaque-handle wrapper from orchestrator that DataBlock unwraps; (b) leave secret-check codepath alive but feed from orchestrator-supplied proof (defeats #266 simplification, wrong); (c) leave gap, document invariant + add compile-time-checked `[[nodiscard]] AttachProof` type only orchestrator can produce.

LOW priority (no known caller exploits).  File so doesn't slip during future SHM transport plugin work (#259/#260/#261).

### #307 — BRC sync-REQ timeout conformance — CURVE-capable silent stub or L2 equivalent

Retired `ZmqEndpointRegistryTest.ReqShape_SyncReqTimesOutOnNoReply` during C5 unmask (#154 AUTH-6 batch-2a, 2026-06-30).

What it tested: REG_REQ / CHANNEL_LIST_REQ / ENDPOINT_UPDATE_REQ all wait `kTimeoutMs` and return nullopt when broker emits no reply (none silently degraded to fire-and-forget).

Why retired: used plain-TCP `StubBrcHandle` for silent-router fixture.  Under HEP-CORE-0035 §2, `BrokerRequestComm::connect()` refuses any cfg without `broker_pubkey` + KeyStore `role_identity`.  connect() returns false before any REQ.

Two paths:
- (a) Make `SilentRouterStub` speak CURVE: bind ZAP_DOMAIN, accept CURVE handshake, then silent on REQ.  Server-side CURVE-router fixture (probably test_framework).
- (b) Move REQ-shape timeout to L2 against `BrokerRequestComm::do_request` directly with mock socket — bypasses CURVE since test is about state machine.

Where it was: `tests/test_layer3_datahub/workers/zmq_endpoint_registry_workers.cpp::req_shape_sync_req_times_out_on_no_reply` + `test_datahub_zmq_endpoint_registry.cpp::ReqShape_SyncReqTimesOutOnNoReply`.  Originally #96.

Coverage lost until reinstated: future commit converting REG_REQ / CHANNEL_LIST_REQ / ENDPOINT_UPDATE to fire-and-forget shape not caught by L3 (would still be caught by L4 timing, noisier).

### #308 — Source-side suppress noisy ERROR logs that L3 tests have to allow-list (recovery + producer_pid)

Found during 2026-06-30 focused review of C5 metrics migration.

`datahub_metrics_workers.cpp::run_with_broker` carries baseline allow-list for two ERROR logs firing on every clean test run:
1. `"recovery: Failed to open '...' for diagnosis"` — `data_block_recovery.cpp:39`.  Fires when broker's SHM recovery API probes test's fresh channel name before producer registration.  Channel doesn't exist → open() fails → ERROR.  For recovery API "channel not present" IS the diagnostic answer — should be DEBUG.
2. `"Broker: HEARTBEAT_REQ for '...' missing or zero producer_pid"` — `broker_service.cpp:3301`.  Fires for CONSUMER heartbeats because BRC's `send_heartbeat` doesn't populate producer_pid on consumer side.  Broker accepts (not rejection); ERROR purely diagnostic.  Should gate on `wire_role_type == "producer"`.

L3 test allow-list is workaround that BUYS TIME but moves regression detection off-test: future "Failed to open" or "missing producer_pid" from NEW site (or genuinely-wrong reasons at existing sites) silently swallowed.

Suggested patches:
- `data_block_recovery.cpp:39`: LOGGER_ERROR → LOGGER_DEBUG when "no such SHM region" vs LOGGER_WARN for corruption.
- `broker_service.cpp:3294-3303`: wrap producer_pid check in `if (wire_role_type == "producer") { ... }`.

When ships, drop two `ExpectLogError` calls in `datahub_metrics_workers.cpp`.

### #309 — Canonical CURVE seckey set pattern — std::string temporaries leak z85 bytes on freed heap

Found during 2026-06-30 focused review of C5 metrics migration.

`datahub_metrics_workers.cpp::raw_request` materializes 40-byte z85 seckey as `std::string` temporary to pass to `dealer.set(zmq::sockopt::curve_secretkey, ...)`.  Temporary destroyed via plain `delete[]` — `std::string` does NOT `sodium_memzero`.  Seckey sits on freed heap until reallocated.

Inside this test: sec_tmp explicitly `sodium_memzero`'d (this commit).  BUT:
1. libzmq makes OWN internal copy on `set()`.  No public API to wipe libzmq's CURVE state; lives until socket destruction.
2. Any future production caller copying pattern has unmitigated leak.

Wanted: canonical "set CURVE seckey from LockedKey" helper.  Live where?  `src/include/utils/security/` next to LockedKey.  Shape:
```cpp
namespace pylabhub::utils::security {
  void apply_curve_secret(zmq::socket_t &sock, const LockedKeyView &key);
}
```
Calls `zmq_setsockopt` directly with locked-memory view's data pointer; libzmq copies internally but test/production never has freed-heap copy.

When exists: `raw_request` switches; any production CURVE socket setup migrates; `EPHEMERAL TEST KEYS — DO NOT COPY` warning drops "DO NOT COPY" half.

Lower priority than #308.  Touches #247 api.crypto.* territory.

---

## §13 Older B-items / small polish

### #292 — Role-host unification — collapse 3 concrete *RoleHost into RoleHostFrame

Goal: eliminate ~1300 LOC of duplicated `worker_main_()` logic by consolidating `ProducerRoleHost` (549 LOC), `ConsumerRoleHost` (456 LOC), `ProcessorRoleHost` (649 LOC) into single canonical impl inside `RoleHostFrame`.

Current: 3 concrete classes derived from `scripting::RoleHostFrame` (plain class from Wave-B M9).  Per `docs/tech_draft/raii_layer_redesign.md` §2: producer/consumer/processor `worker_main_()` bodies >80% identical.  Single `plh_role` binary at `src/plh_role/plh_role_main.cpp` dispatches by config.

Scope:
- Hoist canonical 14-substep `worker_main_()` skeleton into `RoleHostFrame`.
- Reduce 3 concrete classes to thin per-role hooks (tx-only vs rx-only vs both; flexzone symmetry).
- Preserve public ABI of ProducerRoleHost / ConsumerRoleHost / ProcessorRoleHost — internal refactor only.
- Update HEP-CORE-0011 / HEP-CORE-0024 refs.

Companion work: Phase 2a in user's CURVE+CLI completion plan.  Phase 2b layers `SimpleRoleHost<SlotT>` template from API_TODO §"Template RAII" Phase 5 over unified skeleton.

Dependency ordering:
- Blocked by: Phase 1 CURVE chain close (AUTH-5, AUTH-6 #154, AUTH-7, #275 completed, #257, REVIEW-C/D, #262, REVIEW-E).
- Blocks: Phase 2b Template RAII Phases 2-5; Phase 3 CLI --init bundling.

Anchor: `docs/tech_draft/raii_layer_redesign.md` (Phase 5 SimpleRoleHost design) + HEP-CORE-0024 §16.3 Step 3 (14-substep worker_main_ contract).

---

## §14 Completed-task subject-line log (provenance)

Completed tasks are omitted from bodies above.  Subject lines
retained here for reference so cross-links (e.g. "supersedes #123",
"closed by #178") remain readable.  Full commit messages carry the
context; do not restore bodies unless a specific historical detail
is needed.

- #44 [completed] L4 processor + consumer test infrastructure (Wave-D)
- #45 [completed] M4f: delete legacy broker_channel + set_broker_comm + start_ctrl_thread
- #46 [completed] A0: fix band_join regression introduced by M4f
- #47 [completed] A2: per-connection on_hub_dead policy
- #48 [completed] A3: Class B multi-hub fall-through
- #49 [completed] Holistic code review: connection / inbox / band
- #50 [completed] B1: Wire field channel→band rename (proto bump 3→4)
- #51 [completed] B2: find_presence_from_notification routing fix
- #52 [completed] Round-2 review: state explicitness + test rigor
- #53 [completed] C3: IncomingMessage source_hub_uid for dual-hub origin tagging
- #54 [completed] S1+O4: Registration FSM on Presence, retire Shared::producer_channel
- #55 [completed] TR1: Wire-conformance helper + ACK-shape regression tests
- #56 [completed] T1: Harmonize Connected/Ready vocabulary
- #57 [completed] D3: Dead-code cleanup (O1+O2+O3) + Round-3 fresh review
- #58 [completed] R3.5: Broker should reject invalid band names explicitly
- #59 [completed] R3.3: Hub-dead must transition presences out of Registered
- #60 [completed] R3.6: Test coverage for federation-relay CHANNEL_NOTIFY_REQ
- #61 [completed] Static code review of recent audit changes
- #62 [completed] V1 fix: context_valid_ flag + comprehensive flag contract doc
- #63 [completed] V1-followup polish: V2+V3+V4+V5+M3
- #64 [completed] D1+D2 fix: on_channel_closing default=stop, add on_hub_dead
- #65 [completed] D1/D2 follow-up: close-out + thorough fresh-eye audit
- #67 [completed] R3.5b: wire-field unification + grammar + side-aware tag (broker_proto 4→5)
- #68 [completed] R3.5b HEP doc updates + transient archive
- #69 [completed] R3.5b fresh-eye code review
- #70 [completed] FSM correctness slice: H17 docstring + S3 + S4 + TR2 tests
- #71 [completed] Band authority + typed callbacks (S4 expanded)
- #72 [completed] Wave-B M9: RoleHostFrame (plain class) — role host unification
- #78 [completed] B3: hard-error auth.keyfile="" in config load
- #79 [completed] SUPERSEDED by HEP-CORE-0041 (#244)
- #83 [completed] N1: L3 test for setup_infrastructure_ config→opts translation
- #90 [completed] ENDPOINT_UPDATE step 2: switch BRC to sync REQ/REP + test
- #91 [completed] ENDPOINT_UPDATE step 3: scan demos + callers
- #92 [completed] ENDPOINT_UPDATE step 4: audit ALL _REQ frames
- #93 [completed] PlhRoleInitTest.InitOutputValidates/producer — 60s hang on CI
- #96 [completed] Option A: BrokerStub test fixture + BRC-side shape-conformance tests
- #97 [completed] M9 doc: switch CRTP → plain class throughout §11
- #98 [completed] M9 code step 1: create role_config_translation.{hpp,cpp}
- #99 [completed] M9 code step 2: introduce RoleHostFrame (plain class)
- #100 [completed] M9 code step 3: rework L2 test for Q2 + Q3
- #101 [completed] HEP-0035 §4.6 key-file ACL discipline
- #103 [completed] HEP-0017 §3.3 + HEP-0036 implementation
- #104 [completed] Sibling HEP updates: schema/FSM/CURVE-wiring implementation
- #107-#117 [completed] C'-1 through S3: CLI/vault polish
- #118 [completed] Investigate three flaky L2/L4 test failures
- #119 [completed] #101 code-review cleanup
- #121 [completed] #119 round-2 cleanup
- #122-#125 [completed] PeerAdmission §8, Phase A/B/C
- #132-#136 [completed] Phase C close-out commits 1-5
- #137-#139 [completed] Snapshot decisions
- #140-#150 [completed] HEP-0039 Query Layer chain
- #151 [completed] Lib refactor: BrokerService + HubHost + HubConfig match HEP-0035
- #153 [completed] [plumbing] Mask broken L3 broker tests during lib-refactor window
- #157-#161 [completed] C1-C5 CURVE hardening
- #162 [completed] AUTH-2: Role-side ZAP pump via ZapPumpThread
- #163 [completed] HB-4+5: add RegistrationState::Authorized
- #164 [completed] SUPERSEDED by HEP-CORE-0041 (#244)
- #165-#178 [completed] HEP-CORE-0040 chain (Locked Key Memory)
- #181 [completed] 2 L3 broker_schema consumer-mismatch tests
- #182-#184 [completed] SUPERSEDED by #285 (folded scope)
- #185-#187 [completed] HEP-0036 §I10 invariant; C5 follow-up mechanism; §175 seckey load tighten
- #188-#190, #193 [completed] Stage 1A/B/C/D — ZmqQueue Standby state
- #195-#203 [completed] #194 Phase A/B/C + remediation
- #205 [completed] Tighten Release: couple BUILD_TESTS to Debug
- #207-#215 [completed] HEP-0036 §3.5, cross-doc cleanup, AUTH-1 critical-path, ZapRouter Slice A
- #217-#223 [completed] ZapRouter polish, DomainRoutingTable, L3 test infra refactor, Pattern4Registration/ConsumerLifecycle/Heartbeat
- #231 [completed] Z85PublicKey API tightening
- #235 [completed] Python band_member_contains/_count JSON nesting fix
- #238 [completed] Standardize key log-message format (task #238)
- #242 [completed] PlhHubCliTest.NoScriptPasses 60s hang on CI
- #245 [completed] KILLED — interim 0600 hardening
- #248-#256 [completed] HEP-0041 Phase 1 substeps 1a-1i
- #258 [completed] HEP-0041 Phase 1 substep 1k — L4 end-to-end SHM
- #263-#276 [completed] HEP-0041 1i-mig pre-flight + M3.5 + doc-sync + prod-hardening + api-scope + coverage + REVIEW-A/B/C + 1i-mig-4/5 + 1i-cleanup
- #279-#281 [completed] HEP-0041 Mechanism enum widening, EDGE-2 shm_accept_loop UAF fix, REG_REQ data_transport explicit-required
- #286-#291 [completed] HEP-0036 §5b canonical wire schema + phases B-2 through B-5 + consumer-attach handshake failure
- #293-#295 [completed] L3 metrics-plane + ZMQ endpoint registry + HubHost integration test revivals post-AUTH-6
- #300 [completed] role_api_base.cpp:1040 consumer-attach retry burn
- #303 [completed] role_reg_payload.hpp:106 data_transport invariant
- #304 [completed] shm_capability_channel.cpp recvmsg/sendmsg EINTR retry
- #306 [completed] role_api_base.cpp:1136 non-Linux REG_ACK SHM hard-fail
- #310-#316 [completed] #275-S2 step 2, S3a/b/c dead-field deletion, S4 C API secret param, S5 rename, #275-1i-cleanup residue
- #318-#323 [completed] HEP-0041: AttachProtocol receive_frame budget, send_all deadline, HEP-0042 §7.1 filter, TOCTOU /tmp bind, accept4 EINTR retry, L2 ShmQueue contract-test
- #327 [completed] Production role.strict_abi_mismatch
- #328-#329 [completed] Code-review remediation + phase 2 deferred findings

---

## Cross-references

- **`docs/TODO_MASTER.md`** — sprint status + area status table + Resume point
- **`docs/todo/AUTH_TODO.md`** — AUTH critical-path status
- **`docs/todo/API_TODO.md`, `TESTING_TODO.md`, `PLATFORM_TODO.md`, `MEMORY_LAYOUT_TODO.md`, `MESSAGEHUB_TODO.md`** — subtopic detail per CLAUDE.md session hygiene
- **`docs/tech_draft/DRAFT_sec_fold_2_plan_and_guidance_2026-07.md`** — SEC-Fold-2 plan
- **`docs/tech_draft/DRAFT_sec_fold_2_resume_state_2026-07.md`** — SEC-Fold-2 current position
- **`docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md`** — #317 design
- **`docs/tech_draft/DRAFT_HEP-0041-pattern4-reform-coverage_2026-06.md`** — #285 scope

---

**End of snapshot.  Regenerate at end of any session with material task-list changes.**
