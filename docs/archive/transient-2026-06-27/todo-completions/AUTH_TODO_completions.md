# AUTH_TODO completions archive — 2026-06-27

This file is the index for AUTH_TODO sections that were extracted on
2026-06-27 as part of the Phase 0b compression pass (AUTH_TODO 1616 →
~500 lines).  All sections cited below were verified shipped against
current code BEFORE archival.

**Verbatim bytes preserved at** `docs/todo/AUTH_TODO.md` **as of git
commit** `dfe86a61` **(immediate parent of this archive).**  Use:

```
git show dfe86a61:docs/todo/AUTH_TODO.md
```

to retrieve the full pre-compression text exactly as it stood.  The
extraction below covers what was moved, with line ranges for each
section so future readers can fetch a specific narrative on demand
without paging through the whole file.

Companion archives:

- `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
  — Phase A/B/C; D1+D2+D3; landing-phase §4.6.5 no-bypass cleanup;
  BRC monitor investigation; lib-stabilization exclusion procedure;
  resolved decisions; considered-but-not-pursued.
- `docs/archive/transient-2026-06-09/todo-completions/AUTH_TODO_completions.md`
  — 2026-06-05 HB-1..HB-6 audit + 2026-06-05 PM REFRAME + C1..C5
  cleanup chain (full inventory + per-commit scope) + HEP-CORE-0040
  impl chain (#165–#176 + #187) + Phase D close-out follow-ons.

---

## Why this compression

The active AUTH_TODO had grown from 415 lines (post-2026-06-09
compression) back to 1616 lines through three waves of detail
accumulation:

1. AUTH-1 sub-deliverables 4(a)-(g) + B1 vocabulary fix + producer
   S3 activation + 8 follow-ups (6.1-6.8) — all shipped 2026-06-10
   through 2026-06-13.  ~500 lines of narrative.
2. HEP-0041 Phase 1 substep narratives 1a-1k (#248-#258), pre-flight
   tasks (#263-#265), pre-REVIEW-A cluster (#266-#269, #279, #267,
   #280), REVIEW-A/B close-outs.  ~300 lines.
3. HEP-0036 §5b parallel track (#286-#291, B-1..B-5) — all shipped
   2026-06-25/26.  ~30 lines.

Of the 1616 lines, ~1100 documented completed work that no longer
gates active decision-making.  Compression preserves the SHAPE of
each closed work-stream (one-line summary + pointer) while moving
the implementation narrative behind a git fetch.

---

## What was archived (with prior-file line ranges)

The line ranges below refer to `docs/todo/AUTH_TODO.md` at commit
`dfe86a61`.  All citation references inside each section (e.g.
`broker_service.cpp:5573`) were code-verified before the section
was moved.

### §1 — HEP-0036 §5b canonical wire schema unification (lines 116-135)

**Status:** ✅ ALL SHIPPED 2026-06-25/26.  Closed as #286 + sub-tasks
#287/#288/#289/#290.

**Phases:**

| Phase | Task | Shipped | Scope |
|---|---|---|---|
| A — §5b normative authoring | #286 | 2026-06-25 | HEP-CORE-0036 §5b with class diagrams + wire schemas + sequence + info flow + cleanup list |
| B-1 — `channel_id` → `channel_name` fix | #286 | 2026-06-25 | `broker_service.cpp:2082`; `:1156` log read; `:1105` drop legacy fallback |
| B-2 — retire `pattern`/`has_shared_memory`/`shm_name` | #287 | 2026-06-25 | 3 legacy fields + `ChannelPattern` enum + `channel_pattern.hpp` retired; 10 test sites; HEP-CORE-0009 §2.8 marked RETIRED |
| B-3 — `role_tag` → `role_type` + `short_tag` | #288 | 2026-06-25 | ~30-site identifier rename; Native ABI v6→v7 |
| B-4 — unified `producers[]` shape | #289 | 2026-06-25 | Emit `producers: [{role_uid, pubkey_z85, endpoint}]` for both SHM + ZMQ; drop flat `shm_capability_endpoint` + `producer_pubkey_z85` + `pubkey` dual-name |
| B-5 — hard-error on missing canonical fields | #290 | 2026-06-26 | `apply_*_reg_ack` LOGGER_ERROR + return false on missing `channel_name` / unknown `data_transport`; 9 L3 RoleApiFlexzoneTest sites migrated; commit `0825d32e` |
| D — L4 SHM e2e green | #258 | 2026-06-26 | DISABLED_ prefix dropped; fs::absolute path-resolution fix; read_role_log() switch; #291 root-cause fixed via HEP-CORE-0040 §8.5.2 normative seckey contract |

**Adjacent:** #291 SHM consumer-attach handshake failure (`peer closed
connection mid-frame`) closed via KeyStore raw 32-byte seckey
representation at the security-module boundary (HEP-CORE-0040 §8.5.2
NORMATIVE).  Commit `e0f16a44`.

### §2 — AUTH-1 sub-deliverables 1-6 + follow-ups 6.1-6.8 (lines 139-647)

**Status:** ✅ ALL SHIPPED 2026-06-10 through 2026-06-13.  Closed as
#103 (umbrella) + #211/#212/#213/#214/#215/#217/#219 (follow-ups).

**Key sub-deliverables (each verbatim narrative in dfe86a61):**

- **D5** — `CONSUMER_REG_ACK.producers[]` emission ✅ 2026-06-10.
  Broker emits `{role_uid, pubkey, endpoint}` per producer; L3 pin
  `DatahubBrokerConsumerTest.ConsumerRegAckEmitsProducersZmq`.
- **B2** — Layer-2 identity verification (verification model, REVISED
  from original `zmq_msg_gets("User-Id")` plan per user decision
  2026-06-10) ✅ 2026-06-10.  Helper `verify_known_role_binding` called
  in both `handle_reg_req` and `handle_consumer_reg_req`.  New error
  codes `UNKNOWN_ROLE` + `PUBKEY_MISMATCH`.  L3 pins
  `ConsumerRegUnknownRole` + `ConsumerRegPubkeyMismatch`.
- **D4** — BRC notify dispatch ✅ 2026-06-10.  Implements §6.5
  producer-side handler flow; closes HB-3 (`set_peer_allowlist` now
  has live callers).  `RoleAPIBase::handle_channel_auth_notifies`
  worker-thread handler; new `BrokerRequestComm::get_channel_auth`
  sync REQ/REP API; L3 pins `GetChannelAuthReturnsAllowlist` +
  `GetChannelAuthRejectsNonProducer`.
- **4(a)** Consumer-side switch via Stages 1A/1B/1C ✅ shipped via
  #188/#189/#190/#193 — ZmqQueue Standby state machine; cycle_ops
  Standby guard; `api.is_channel_ready` script binding (Lua + Python +
  Native parity).
- **4(b)** Broker wire-shape `GET_CHANNEL_AUTH_ACK.allowlist` Z85
  string array (HEP §6.5 locked 2026-06-12) ✅ shipped via follow-up
  6.1.
- **4(c)** Script API polling accessors `api.allowed_peers(channel)`
  + `api.producers(channel)` ✅ Lua + Python + Native parity; Native
  deferred per #84 MVP.
- **4(d)** Script event callback `invoke_on_allowlist_changed` — **⏳
  DEFERRED post-AUTH-7** as MVP-optional (polling accessors 4(c) cover
  the use case).  Not on critical path.
- **4(e)** Engine-parity discipline — see #232 cross-role observation
  surface sweep.
- **4(f)** Safety guardrail `tools/check_auth_guardrail.sh` ✅ shipped
  2026-06-10; wired into ctest as `AuthGuardrail_NoScriptAllowlistMutator`.
- **4(g)** Tests — L3 wire-shape pin shipped; L3 callback-firing folded
  into AUTH-7 (real script engine + plh_role binaries); L2 unit test
  for `handle_channel_auth_notifies` skipped per L3 sufficiency.
- **B1** `awaiting_endpoint` vocabulary fix ✅ shipped 2026-06-10.
  Both port-0 sites in `broker_service.cpp:2063-2069` + `:2273-2279`
  include `(awaiting_endpoint)` in the WARN log + error message.
- **Producer-side S3 activation** ✅ shipped 2026-06-12 (commit
  `72021c54`).  `hub_zmq_queue.cpp::apply_master_approval` PUSH side
  extracts `initial_allowlist`, builds `PeerAllowlist`, seeds ZAP
  cache, drives `start()`.  `role_api_base.cpp::build_tx_queue` no
  longer calls `writer->start()` inline.  New
  `RoleAPIBase::apply_producer_reg_ack(ack)` mirror.
  `setup_infrastructure_` no longer calls `start_*_queue()`.  Producer
  + processor role hosts call `apply_producer_reg_ack(*reg_result)`
  after registration.  L2+L3 sweep 1734/1734 green.

**Follow-ups (each verbatim in dfe86a61):**

- **6.1** `GET_CHANNEL_AUTH_ACK.allowlist` shape flip to Z85 string
  array ✅ 2026-06-12.
- **6.2** Fatal-on-failure for bare registration branches ✅ 2026-06-12.
  All three role hosts abort startup on REG_REQ / CONSUMER_REG_REQ
  failure per HEP-CORE-0036 §3.5.1.  L2+L3+L4 sweep 1862/1862.
- **6.3** Migrate 10 test sites off legacy direct-activation
  ✅ 2026-06-13.  `role_api_flexzone_workers.cpp` lines 206/233/363/
  434/788/825/930/972/1056/1098 flipped to `apply_*_reg_ack(json::object())`.
- **6.4** Delete `RoleAPIBase::start_rx_queue()` / `start_tx_queue()`
  from public API ✅ 2026-06-13.
- **6.5** HEP lifecycle-sync sweep (Option B) ✅ 2026-06-13 (task #213,
  review doc archived 2026-06-27).  HEP-CORE-0011 + 0017 + 0019 + 0031
  + 0018 + 0033 §19.8 all re-synced with current code.
- **6.6** ZapRouter Slice A (UAF + reentrance + single-pumper) ✅
  2026-06-13 (task #215, review doc archived 2026-06-27).
  `zap_router.cpp::pump_one` shared_lock extension + RecursionGuard;
  refuses reentrant `register_domain` + PANICs on reentrant
  `~ZapDomainHandle`; four L2 regression tests with mutation sweep.
- **6.7** Modern-C++ polish on Slice A surface ✅ 2026-06-13 (task
  #217).  Reference-not-pointer registration; `reference_wrapper` map;
  `std::string_view` reply API; FIRST `std::jthread` in repo
  (`ZapPumpThread`); `[[nodiscard]]` on `pump_one`.
- **6.8** `DomainRoutingTable` extraction + jthread/ThreadManager note
  ✅ 2026-06-13 (task #219).  New `pylabhub::utils::security::DomainRoutingTable`
  utility encapsulates `(string → reference_wrapper<PeerAdmission>)`
  map + shared_mutex + lock-bounded callback contract; future reusers
  HEP-CORE-0037 federation peer ROUTER, CURVE-wrapped admin REP.

### §3 — AUTH-2 detailed scope (lines 649-705)

**Status:** ✅ SHIPPED 2026-06-16 as #162.

**Title change 2026-06-16:** moved from "Producer-side ZAP pump on
BRC poll thread" to "Role-side ZAP pump via ZapPumpThread lifecycle
module" after multi-BRC analysis.

**Design choice — Option F (dedicated lifecycle module).** Originally
scoped to wire `ZapRouter::pump_one(0ms)` into the BRC poll thread.
Reconsidered because dual-hub processor has TWO BRCs in one process,
both wanting to pump the same process-singleton REP socket; single-
pumper invariant (§7.4 #3) would PLH_PANIC the moment two handshakes
raced.  Dedicated thread is invariant-preserving by construction for
both single-BRC roles and multi-BRC processor.

**Scope shipped:**
- `ZapPumpThread` dynamic LifecycleManager module (depends on
  `ZapRouter`).  Two activation modes: direct RAII for tests/demos;
  module path for production wiring.
- `ZapRouter::ensure_module_registered()` — idempotent bridge that
  registers `ZapRouter` dynamic module without registering a domain.
- `plh_role_main.cpp` loads `ZapPumpThread::ensure_registered_and_loaded()`
  after `KeyStore` construction.  Process-singleton `std::jthread`
  loops `pump_one(100ms)` until LifecycleGuard shutdown.

**Validation:** L2 (1478) + L3 (265) + L4 (128) + Pattern 4 (4) all
green.

### §4 — AUTH-3 detailed scope (lines 707-746)

**Status:** ✅ SHIPPED 2026-06-13 as #163.

**Scope shipped:**
- `RegistrationState::Authorized` added to `role_presence.hpp:95-101`
  + transitions per §4.3.2.
- `any_presence_authorized()` predicate.
- Data-loop outer guard extended at `data_loop.hpp:129-131`:
  ```cpp
  while (core.is_running() && !core.is_shutdown_requested()
         && !core.is_critical_error() && any_presence_authorized())
  ```

### §5 — AUTH-4 SUPERSEDED 2026-06-16 by HEP-CORE-0041 (lines 748-817)

**Status:** SUPERSEDED.  Original tasks #164 + #79 closed as
SUPERSEDED.  Replaced by HEP-CORE-0041 capability transport (#244
shipped 2026-06-16).

**Why superseded.** The 2026-06-13 SHM threat-model audit identified
three gaps in the `shm_secret` model:

1. **POSIX 0666 default** (`platform.cpp:478` `kShmModeRw`) lets any
   local UID open the shm and read the secret from header bytes.
2. **Plaintext discriminator, not cryptographic check** — `memcmp` on
   header bytes (`data_block.cpp:2768-2777`) is at best a typo-catcher.
3. **Drift window between revocation and broker pushback** — same
   fast-path problem hit in ZMQ (see #246).

HEP-CORE-0041 replaces with capability-transport model: producer
creates anonymous shm (Linux `memfd_create`, macOS `SHM_ANON`, Windows
`CreateFileMapping(NULL)`), broker holds the only handle after consumer
authorization, broker passes capability to authorized consumer via
`SCM_RIGHTS` / `DuplicateHandle` after pre-attach `CONSUMER_ATTACH_REQ`.
Unauthorized peers cannot obtain the capability — no plaintext token
to sniff or replay.

### §6 — HEP-0041 Phase 1 substeps 1a-1k narratives (lines 1057-1205)

**Status:** ✅ Substeps 1a-1h, 1i-mig (multiple), 1k all SHIPPED;
1i-cleanup (#275) S1+S2a+S2b+S2c-1..6 ✅; **S3+S4+S5 NOT YET SHIPPED
(see correction below)**; 1j (#257), #262 mutual auth, REVIEW-C/D/E
all ⏸.

**Verified substep status (against code at commit `dfe86a61`):**

| Substep | Task | Status | Commit |
|---|---|---|---|
| 1a | #248 | ✅ | initial |
| 1b | #249 | ✅ | initial |
| 1c | #250 | ✅ | initial |
| 1d | #251 | ✅ | initial |
| 1e | #252 | ✅ | initial |
| 1f | #253 | ✅ | initial |
| 1g | #254 | ✅ | initial |
| 1h | #255 | ✅ | initial |
| 1i-mig-1 | #256 | ✅ | `e283a4ac` ShmQueue fd plumbing |
| 1i-mig-2a | #256 | ✅ | `e5575329` TxQueueOptions::shm_capability_fd |
| 1i-mig-2b-1 | #256 | ✅ | `31072b3e` Producer owns L1 IShmCapabilityProducer |
| 1i-mig-2b-2 | #256 | ✅ | `4bae7cfb` Producer wires L2b/L2c + accept thread |
| 1i-mig-2c review fixes | #256 | ✅ | `78fc6b9d` strip_unix_scheme + mkdir-0700 + accept-loop try/catch |
| 1i-mig-2c M3 | #256 | ✅ | `8e05a821` 3-pointer SHM auth bundle to RoleHostFrame |
| 1i-mig-3 | #256 | ✅ | `6f31a346` Processor OUT-side wiring |
| 1i-mig-M3.5 | #266 | ✅ | `e8e10738` prepare_tx_capability_ default impl |
| 1i-doc-sync | #267 | ✅ | `47fe4806` doc-vs-code bundle |
| 1i-prod-hardening | #268 | ✅ | `9c0947f1` H3a-d producer hardening |
| 1i-api-scope | #269 | ✅ | `0533102e` RoleAPIBase::consumer_attach FRAMEWORK-INTERNAL |
| Mechanism enum widening | #279 | ✅ | `56deb3ac` |
| REVIEW-A | #271 | ✅ | `8c6ca64a` |
| 1i-mig-4 | #272 | ✅ | `2793a394` consumer dial (~150 LOC) |
| 1i-mig-5 | #273 | ✅ | `9e923c12` cutover + L3 worker migration |
| REVIEW-B | #274 | ✅ | `8122bafc` close-out: B1+B3 fixes; B2 deferred to #275 |
| #280 EDGE-2 shm_accept_loop UAF | #280 | ✅ | `22d80291` |
| #281 broker data_transport strict | #281 | ✅ | `ecc72337` |
| 1i-cleanup S1 | #275 | ✅ | 2026-06-23 |
| 1i-cleanup S2a/S2b/S2c-1..6 | #275 | ✅ | per task description |
| **1i-cleanup S3** | #275 | **❌ NOT SHIPPED** | see correction below |
| 1k | #258 | ✅ | `0a341a5b` 2026-06-26 (path-resolution + read_role_log + LastTest.log fixes) |

**CORRECTION 2026-06-27 — line 1137 mis-claim:**

AUTH_TODO line 1137 at commit `dfe86a61` claimed "S1+S2a+S2b+S2c-1..6+S3
✅ shipped".  Verified against code: **S3 is NOT actually shipped.**
- `hub_shm_queue.cpp:375` still implements `set_shm_secret()`.
- `:134/198/216/253` still has `shared_secret` parameters in
  `create_writer`/`create_reader` overloads.
- `RoleAPIBase::build_rx_queue/build_tx_queue` still has the
  `shm_shared_secret != 0` branches.

The compressed AUTH_TODO at HEAD records the correct S3 ⏸ status.
S3 + S4 + S5 are sequential — see #275 1i-cleanup detailed plan in
the active AUTH_TODO.

### §7 — Pre-flights closed 2026-06-22 (lines 1057-1077)

**Status:** ✅ ALL THREE PRE-FLIGHTS SATISFIED.

- **#263** PeerAllowlist populated for SHM channels via
  `apply_producer_reg_ack` (REG_ACK `initial_allowlist` seed) +
  `handle_channel_auth_notifies` (CHANNEL_AUTH_CHANGED_NOTIFY +
  GET_CHANNEL_AUTH refresh).  Orchestrator's CacheLookup reads
  `api.allowed_peers(channel)` successfully.
- **#264** `BrokerRequestComm::consumer_attach` API at
  `broker_request_comm.cpp:884-898` matches orchestrator
  `BrokerQuery` callback shape (channel, consumer_pubkey,
  consumer_role_uid, producer_role_uid, timeout_ms) →
  `optional<json>`.  `RoleAPIBase::consumer_attach` added as public
  router.
- **#265** `SeckeyAccessor` in `RoleHostFrame::spawn_shm_auth_listener_`
  uses `key_store().with_seckey(kRoleIdentityName, cb)` with a
  string_view→span<const byte> adapter.  HEP-CORE-0040 §8.5.1
  use-not-export discipline respected.

### §8 — REVIEW-B (#274) close-out 2026-06-23 (lines 1152-1205)

**Status:** ✅ CLOSED 2026-06-23.

**Applied in single batched commit:**
- **B1 HIGH** — `attach_protocol.cpp:438-480` `initiate_consumer_handshake`
  stripped `unix://` scheme before `connect(2)`.  Production-breaking
  bug: consumer was connecting to literal `unix://...sock` rather than
  bare filesystem path, returning ENOENT for every dial.  Symmetric
  with `MemfdConsumer` ctor's existing strip at
  `shm_capability_channel.cpp:559`.
- **B3 HIGH** — corrected "BRC poll thread" → "worker thread" in
  AUTH_TODO + `DRAFT_HEP-0031-bounded-thread_2026-06.md`.  The dial
  is invoked from `ConsumerRoleHost::worker_main_` Step 6, not BRC
  poll loop.
- **B2 HIGH** — deferred to #275 (REVIEW-B agent flagged 2 specific
  tests but the scope is actually 7 SHM tests in
  `role_api_flexzone_workers.cpp` — doing only the 2 would be partial
  work that #275 has to revisit).
- **Medium docs** — `hub_shm_queue.hpp:17` `@par Lifecycle` rewritten
  to cover both capability and legacy paths; `role_api_base.cpp:108-135`
  declaration-order docstring corrected.

**Carry-forward to REVIEW-C (#276):** Five medium-tier items from
the REVIEW-B agent reports were named but their concrete agent text
was lost when the close-out session context compacted.  Candidate
sites identified:
- HEP §11 row — `docs/HEP/HEP-CORE-0041-*.md:1050` "Next steps" row
- HEP §6.4 missing factory — `:649` consumer-side `create_reader_standby`
- MemfdConsumer ctor RAII — `shm_capability_channel.cpp:552-...`
  asymmetric cleanup on construction failure
- SCM_RIGHTS multi-fd defense — `:494-512` recvmsg cmsg parsing
  does NOT check `msg.msg_flags & MSG_CTRUNC`
- D3 retry budget — `role_api_base.cpp:987-1029` budget allocation

REVIEW-C (#276) is the natural home for these.  Decision: do not
fabricate fixes in the absence of original finding text.

### §9 — Design audit 2026-06-10 — closed items (subset of lines 1329-1438)

**Acknowledged trade-offs (not bugs; documented for future readers):**

- **A1** Revocation race window (HEP §I5) — documented; no action.
- **A2** Stale-cache during long script callback — documented in
  §I11; operator's responsibility to bound script callback time.
- **A3** Allowlist size scalability — `set_peer_allowlist` mutex
  during update; concurrent handshakes wait.  Fine for tens-to-low-
  hundreds per channel; revisit if >1000.

**Federation (HEP-0037, post-MVP):**

- **F1** Cross-hub `CHANNEL_AUTH_CHANGED_NOTIFY` propagation — defer
  to HEP-0037 (#105).
- **F2** `allowed_peers` semantics in federation — HEP-0037's call.

The G1/G2/R1/R3/R4/T1/T2/T3/T4 items remain ACTIVE — see active
AUTH_TODO for their AUTH-N anchors.

---

## What was NOT archived (stays in active AUTH_TODO)

- AUTH-5 (#104) sibling-HEP doc sync — open; 7 doc-only HEPs
- AUTH-6 (#154) L3 broker test re-creation — in flight
- AUTH-7 L4 end-to-end gate close — pending
- #275 1i-cleanup S2 + S3 + S4 + S5 — staged plan
- #257 1j L3 broker tests (success / denied / divergence-WARN)
- #262 mutual auth (3rd handshake frame)
- REVIEW-C/D/E (#276 / #277 / #278) — gate the chain
- HEP-0041 Phases 2-5 (cross-platform + crypto + ZMQ retrofit) brief
- Design audit gaps tied to active tasks (G1/G2/R1/R3/R4/T1-T4)
- Backlog items (test infra, operator workflow, Phase E/F/G/H)
- Deferred decisions table
- Parallel/adjacent tracks (#74/#102/#103/#104/#105/#106/#120/#152)
- Decision log (load-bearing history — kept verbatim)
- Memory rules adopted during auth track

---

## How to retrieve a specific narrative

```bash
# All AUTH-1 sub-deliverables + follow-ups (lines 139-647):
git show dfe86a61:docs/todo/AUTH_TODO.md | sed -n '139,647p'

# AUTH-2 detailed:
git show dfe86a61:docs/todo/AUTH_TODO.md | sed -n '649,705p'

# HEP-0041 Phase 1 substep narratives + REVIEW-A/B close-outs:
git show dfe86a61:docs/todo/AUTH_TODO.md | sed -n '912,1205p'
```

Or simply browse the prior file:

```bash
git show dfe86a61:docs/todo/AUTH_TODO.md > /tmp/AUTH_TODO_pre_compression.md
```

---

## Cross-references

- `docs/TODO_MASTER.md` §"Current Sprint Focus" — Phase 0/1/2a/2b/3
  execution plan (locked 2026-06-27)
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md`
  — normative auth contract
- `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md` — Locked Key Memory
  framework primitives
- `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` — SHM capability
  transport
- `docs/code_review/REVIEW_S5_CoreStructure_2026-06-27.md` — Core
  Structure Change Protocol walkthrough for #275 S5
