# Documentation archive log

**Purpose:** Work log of document clearances: when transient documents were merged into core docs and archived. Use this to find historical content or to see what was merged where. For **how** to clean up, merge, and archive correctly, see **`docs/DOC_STRUCTURE.md`**.

---

## Archive batches

### 2026-08-07 (pass 4 — design session: two features decided out, one abstraction filed)

Not a cleanup pass — a design session, recorded here because it retired a HEP
and closed two open lines. Every claim below was checked against source before
the decision was taken.

**Vault hot reload — DECIDED AGAINST (#104).** Scope was clarified first: HEP-0035
§4.8.5 covers the **allowlist only**; the vault also holds the broker keypair
and admin seal key, but neither was ever in the proposal. Owner's call: hot-managing
auth invites inconsistency. §4.8.5 argues the same side unprompted — a reload that
stops at the hub leaves every role deciding on the roster it got at registration.
**Restart is the revocation boundary.** Follow-up: §4.8.5 still reads "DEFERRED",
which means "not yet"; rewrite it as a decision.

**HEP-CORE-0045 SHM observer — RETIRED (not deferred).** The decisive fact was
found by reading, not grepping: `snapshot_metrics_for_presence()` **is** the
heartbeat payload and already ships `QueueMetrics` per presence, keyed and
freshness-stamped; and the role's own handle already reads `DataBlockMetrics`
(`ShmQueue::capacity()` calls `get_metrics()` today). Both halves existed and
were never joined. Adding seven fields to `PYLABHUB_QUEUE_METRICS_FIELDS` (#117)
feeds `api.metrics()` **and** the heartbeat from one X-macro — which is what the
observer was built to deliver for a healthy channel. Lost: readings from a role
that stopped heartbeating but still holds the segment mapped — a window the
broker is already closing by reaping. Gained: a privilege surface removed (the
broker stops mapping other processes' memory, and the by-name `collect_shm_info`
read goes with it), and the end of a half-shipped state where the observer key
was minted and published on every `PRODUCER_REG_ACK` with nothing dialling.

**The ephemeral keypair mechanism is KEPT (#119)** — owner's call, and correct:
mint at startup, publish the pubkey over an already-authenticated channel, peer
stores it as the anchor for one scoped operation, never persisted, dies with the
process. The abstraction ruling: **it must not become a third
`PubkeyOrigin::Kind`.** That enum's docstring says its distinction is
"load-bearing, not descriptive" — it governs *which identities a key may speak
for*. This key speaks for none; it answers *what may the holder do*. Correct
shape is a sibling in the security module. The tell that the abstraction was
already blurred: the key is minted by `generate_and_add_identity(...)`, an
**identity** API producing a **capability**, scope carried entirely by a name
string.

**Metrics design rulings (#117), recorded so they are not relitigated:**
flat fields, not a conditional `shm` sub-group (the convention is stated twice
in-tree: `metrics_json.hpp:10` and `ZmqQueue::metrics()`); **no `mechanism` field**
— `api.queue_mechanism(side)` already exists with engine parity, and a property
fixed at factory time is not a metric; **no N/A encoding** — JSON is not
compatible with N/A, numbers are for the user to interpret.

**A bad suggestion of mine, rejected and reframed (#118).** I proposed measuring
`offsetof` to detect cache-line false sharing. Owner rejected it: machine-dependent
(64B x86-64 vs 128B Apple Silicon), and pointless unless mechanistically addressed.
The correct question is **static** — does `SharedMemoryHeader` group fields by
access pattern and express it in source (`hardware_destructive_interference_size`),
the way `SlotRWState` already does with `alignas(64)`? Filed as a review with an
explicit "do not benchmark". Recorded because the failure mode is worth
remembering: I proposed empiricism where design was called for, and kept
explaining the mechanism instead of checking whether the answer was load-bearing.

**System lifecycle / deployment (#116)** — opened in place of hot reload. If
restart is the revocation boundary, restart must be orchestrated. Today
`ADMIN_REQUEST_SHUTDOWN_REQ` stops one process and tells no role anything; roles
infer death from a dead socket, so **an orderly shutdown and a crash are
indistinguishable**; `StopReason`'s seven values cannot express "planned"; roles
never reconnect, so hub restart always means role restart; and HEP-0023 §6's
entire cold-start design for the hub layer is one sentence — *"Hub brokers are
assumed to be running before any role starts."* No deployment doc exists.


### 2026-08-07 (pass 3 — verification pass: every resolution note checked against source, plus a full test sweep)

**Why this pass ran.** Passes 1 and 2 retired records by reading them. This
pass checked them by running the build and the tests, and by looking up the
replacement each ✅ note *names* rather than only the site it fixed. That
distinction is what the pass was for, and it is what found everything below.

**Evidence base.** `cmake --build build --target stage_all -j2` → exit 0.
Full unfiltered sweep via `tools/ctest_evidence.sh -j2` → **2780 tests, 100%
passed, 0 failed**, 4 skipped (1 ABI build-id case; 3 `BrokerFederationTest`,
consistent with federation being parked under #69). Timings: layer2 144.7s,
layer3 138.1s, layer4 102.6s; total 200.7s wall.

**Review verification — both records closed.**

- `REVIEW_Connection_Inbox_Band_2026-05-17.md` — 16 of 16 findings re-checked.
  8 resolved, 2 closed as accepted design, 6 open. Three had sat as ❌ OPEN
  since May while already fixed: **I3** (one `answer()` builder plus an explicit
  reason vocabulary at `broker_service.cpp:7023-7037` dissolved the
  `found`/`has_inbox` ambiguity), **S3** (mask init moved to Phase 1.5,
  `role_api_base.cpp:4353-4362` — exactly the recommended remedy),
  **TR2** (S1's FSM landed; `datahub_role_state_workers.cpp:1085-1142` pins
  Unregistered → Registered → Deregistered). **S2** and **TR3** closed as
  design notes — both findings' own dispositions said so, and carrying them
  open was the bookkeeping error. The 6 open moved to `API_TODO.md` + tasks
  #111 (C5/B4/X5/S4) and #112 (X2/X4).
- `REVIEW_FullSystem_2026-07-20.md` — all ~50 self-reported ✅ notes checked.
  **Every one held on substance.** Nine that had been verified only by "the
  test name appears in a file" were re-checked against real ctest output and
  all nine ran and passed (`LoggerTest.SyncLogging`, 6×`ClassifyPeerVerdictTest`,
  `ReapAbandonedOnDeadBroker`, both `EndpointChange*` Pattern-4 tests,
  `Eval_SyntaxError_ReturnsScriptError`,
  `ZmqE2E_ConsumerSchemaMismatch_AbortsAndTearsDown`,
  `Sch_WireHelpers_FlexzoneRoundTrip`,
  `OnSchemaRegistered_FlexzoneOnlyRecord_Created`); count-specific claims met or
  exceeded (HubHost 12 ≥ 10 claimed; loop-timing 16 ≥ 12).
  One note's *evidence* had gone stale though its finding was resolved: the
  `known_roles.cpp:359` note described `as_peer_allowlist()`, retired under #83
  for `PubkeyOriginIndex::zap_allowlist()` (`pubkey_origin.hpp:325`) —
  corrected in place. Three residual doc drifts filed as **#110**.

**Drafts archived → `transient-2026-08-07/tech_drafts/`.**

- `DRAFT_HEP-0041-pattern4-reform-coverage_2026-06.md` — premise resolved. It
  existed to pre-decide coverage before #275-S3/S4/S5 broke deferred tests;
  that work landed (`reserved_capability_token[64]` at `data_block.hpp:234`
  with the rename note at `:223`; C-API secret param gone; factories now take
  `(name, policy, config)`). Nothing left to decide.
- `DRAFT_reg_wire_alignment_cleanup_2026-07-13.md` — edit plan verified
  group-by-group. Groups 1-4 and 6 landed: `RegReqBody` split into
  `ProducerRegReqBody` (`wire_bodies.hpp:154`) + `ConsumerRegReqBody` (`:317`);
  phantom `schema_version()` and legacy `broker_proto()` accessors gone;
  `gate_supported_proto` retired per its own C3 resolution
  (`admission_gates.cpp:103,445`); `BrokerAdmissionConfig::broker_proto` gone;
  `raw-req-anon` test fallback gone. **Three items outlived it, all recorded in
  `MESSAGEHUB_TODO.md`** — most importantly that its **C2/D1 rows are actively
  dangerous and must not be executed**: they call `CHANNEL_BROADCAST_REQ`
  "retired" and plan to delete `send_broadcast()` + the dispatch row, a premise
  overturned four days later by finding B6/T3 (HEP-CORE-0030 §9 amended, §9.1
  added: channel-bound and band-bound broadcast coexist). Also extracted: **D3**,
  a live contradiction between HEP-CORE-0046 **I-CORRELATION-STABLE**
  (line 1023: keys on `(msg_type, correlation_id)`) and the shipping code
  (`broker_request_comm.cpp:281`, keyed on correlation_id alone) — task **#113**,
  owner call.

**Permanent guidance moved out of a transient tracker.** `TESTING_TODO.md`
lines 16-93 held MANDATORY test-design rules — permanent guidance inside a file
whose whole purpose is to be trimmed and archived. Moved to
`README_testing.md` **§1.3**: layer purpose, pin-path-not-outcome + mutation
sweep, pin-what-the-design-says, replicate-production-scenarios, retirement
discipline. Two rules deliberately **not** copied because the destination
already carried them (mocking discipline → §1.2; `SetUpTestSuite`-owned
`LifecycleGuard` antipattern → "Choosing a test pattern" + Pattern 1+). The
retirement *ledger* stayed in `TESTING_TODO.md` — that is tracking, not
guidance.

**Corrections to records.**

- `TESTING_TODO.md:85` claimed four `RoleIdentityPolicyEnumTest` TEST_F's were
  "RE-LAYERED (not retired)" to `tests/test_layer2_service/test_role_identity_policy.cpp`.
  That file does not exist and the suite appears nowhere under `tests/`. They
  were retired with their subject; `check_role_identity` was deleted under #68.
- `TODO_MASTER.md` "Active code reviews (3)" listed four entries. Now reads
  "1 active + 2 archivable".
- `LINT_FIXES_PLAN.md` marked **stale input, do not action as written** — built
  from an April 2026 clang-tidy log that no longer exists, *partially* stale
  (`actor_vault.cpp` deleted; `hub_config.cpp` rows point at the legacy
  singleton) while other cited symbols survive, which lends the dead rows false
  credibility. Regenerate-then-diff tracked as **#114**.
- `tech_draft/README.md` gained a tracked-inventory table; every remaining
  draft now names where it is tracked, with `future-persistence-and-discovery/`
  recorded as the one deliberate untracked exception (reference material, no
  open items).

**Third sweep — the three TODO files the earlier passes never opened.**
`TOPOLOGY_TODO`, `QUERY_LAYER_TODO` and `PLATFORM_TODO` had not been re-read
this cycle. All three held items that would have misdirected planning:

- **`TOPOLOGY_TODO` Phase E was gated on a retired phase.** The section said
  it "only starts AFTER Phase D has migrated all callers", and its Blocking
  line named "Phase D R6 symmetrization live + verified". **Phase D was retired
  2026-07-25** — superseded wholesale by the owner-first contract. Phase E
  therefore waited on work that will never happen and read as permanently
  blocked, which is why its retirements (deleting `handle_consumer_attach_req_zmq`
  ~400 LOC and the legacy `hub_zmq_queue.hpp` factories) have sat untouched.
  Re-gated per-surface: the real precondition is "no caller depends on this",
  and `CONSUMER_ATTACH_REQ_ZMQ` genuinely still has 16 `src/` references.
- **`TOPOLOGY_TODO` finding #18 resolved, by a route it did not predict.**
  It expected three dead accessors to "become live when Phase D wires
  ENDPOINT_UPDATE_REQ" — Phase D died, but `_set_channel_data_endpoint` became
  live anyway via the §16.4 endpoint-update handler (`broker_service.cpp:5871`),
  and `bump_channel_version` / `set_confirmed_version` were retired outright
  2026-07-13 (tombstone at `hub_state.hpp:1025-1026`).
- **`QUERY_LAYER_TODO` named replacements that were never built.** Its
  retirement row for `query_shm_info` / `collect_shm_info_json` said they are
  "subsumed by `list_shm_blocks(snap)` / `get_shm_block(snap, channel)`" —
  **neither function exists anywhere in `src/`**, so nothing subsumes anything
  and "after call sites migrate" describes a migration with no destination.
  This is the same failure class as the stale ✅ evidence above: a plan naming
  a successor that does not exist. Folded into #112 with an explicit warning,
  including that band 2 (SHM observer, #78) may want exactly this surface, so
  deleting it is a decision and not a cleanup. The row also joins two records
  that were each tracking one end of the same dead wire.
- **`QUERY_LAYER_TODO` had ready work reading as blocked.**
  `ChannelSnapshotEntry` / `ChannelSnapshot` were gated on "after L3 worker
  tests migrate"; **zero test files reference them today**. The gate was
  satisfied and the row never closed. Filed as **#115**.
- **`PLATFORM_TODO` and `LINT_FIXES_PLAN` are two halves of one job** — the
  former holds the clang-tidy invocation and says "run periodically", the
  latter holds the stale results, and neither referenced the other. Cross-linked
  both ways and folded the recipe into #114 so the regeneration does not
  reinvent it.

**Fourth sweep — the routing tables themselves were wrong.** `AUTH_TODO` and
`docs/todo/README.md` had not been opened this cycle either. The README turned
out to be the root cause of a problem earlier passes had been treating as a
symptom:

- **`CLAUDE.md`'s canonical subtopic table pointed at a file archived in March
  and omitted three that exist.** It routed memory-layout items to
  `docs/todo/MEMORY_LAYOUT_TODO.md` (archived 2026-03-02, absorbed into
  `API_TODO` + `TESTING_TODO`) and listed five areas while seven trackers
  exist — **omitting `AUTH_TODO.md`, which carries the security critical
  path and priority band 1.** This is the project instruction file: it tells
  every session where to file work. Corrected, with the three missing rows
  added and memory-layout routed to `API_TODO.md`.
- **`docs/todo/README.md`'s index was five months stale** — four files listed,
  seven exist, and the three omitted were `AUTH_TODO`, `TOPOLOGY_TODO`,
  `QUERY_LAYER_TODO`. Descriptions on the four it did list referenced
  long-dead labels ("Steps 4–5", "HP-C1/HP-C2/BN-H1", "six implementation
  phases pending"). Rebuilt from `ls`.
- **The same README instructed the exact practice `DOC_STRUCTURE.md` §2.1.1
  forbids.** It told maintainers to "move completed tasks to **Recent
  Completions**" in *ten* places — the template structure, the How-to-Use
  steps, the Maintenance regime, and a worked `### Recent Completions
  (2026-02-14)` example in the DO block — while §2.1.1 states "No 'Recent
  Completions' walls. No dated 'Closed' subsections piling up." Its "Purpose"
  bullet even read "Completions stay in subtopic TODOs", the precise inverse
  of the rule. **This is the plausible root cause of the completion-wall bloat
  that passes 1 and 2 spent their effort trimming by hand.** A process doc
  sitting next to the work will beat a structure doc one directory up. All ten
  sites fixed; the self-defined Weekly/Monthly/Quarterly cadence replaced by a
  pointer to §2.1.1 as the single authority. Also dropped the "Master TODO
  stays under 100 lines" claim (it is 423, and the number was never a gate).

**Reviews archived** to `transient-2026-08-07/code_review/` now that both are
fully verified and their open items rehomed: `REVIEW_FullSystem_2026-07-20.md`
and `REVIEW_Connection_Inbox_Band_2026-05-17.md`. `docs/code_review/` retains
`LINT_FIXES_PLAN.md` (active, stale input — #114) and
`REVIEW_VALIDATION_2026-08-02.md` (audit trail).

**Findings slotted into the owner's five priority bands** in `TODO_MASTER.md` —
#111 → band 4 (same surface as the role-host collapse), #110/#112/#114/#115 →
band 3, and #113 marked *needs a decision, not a slot* because a locked HEP
invariant and shipping code disagree. **Nothing landed in band 1**: the
security band's open set is unchanged by this cleanup, which is the honest
result rather than a disappointing one.

**One self-correction.** Mid-pass I flagged the checksum-codec note for citing
`zmq_wire_helpers.hpp` as a nonexistent file and edited the review accordingly.
The file exists at `src/utils/hub/zmq_wire_helpers.hpp` (`fixarray[5]` line 6,
`ChecksumPolicy` 15, `pack_frame` 227, `decode_frame` 326) — the grep behind
that call had been scoped to `src/include/utils/`. Edit reverted. Recorded
because this pass leaned heavily on absence proofs: **a path-scoped miss is not
an absence proof.**

### 2026-08-07 (pass 2 — drafts + reviews, retired against code evidence)

The first pass covered the TODO files only.  This one covers what it skipped:
`docs/tech_draft/` and `docs/code_review/`.  Every retirement below was
checked by locating the thing in today's source, not by reading a status
line.

**Tech drafts archived** → `transient-2026-08-07/tech_drafts/`:

| Draft | Evidence it is done |
|---|---|
| `DRAFT_owner_first_establishment_S1_S3_2026-07-26.md` | `AWAITING_OWNER` in `role_api_base.hpp:1114`, `AwaitOwner` in `hub_state.hpp:228` (task #76) |
| `DRAFT_topology_singular_side_2026-07.md` (1736 L) | self-marked RETIRED 2026-07-25, superseded by the establishment contract |
| `PLAN_inbox_sender_from_proven_key.md` | slice 4 shipped `f507b334`/`e73f9340`; `InboxAuthority` live in `hub_inbox_queue.hpp` |
| `DRAFT_verified_peer_identity_2026-07-27.md` | task #83 closed; `PubkeyOrigin` `pubkey_origin.hpp:68`, `PeerAuthority` `:193` |
| `DRAFT_HEP-0041-test-completeness_2026-06.md` | claimed tests exist: `test_attach_protocol.cpp` (15 TESTs, claim said 9), `test_shm_attach_orchestrator.cpp` (8, claim said 8) |

**Reviews archived** → `transient-2026-08-07/code_review/`:

- `REVIEW_CatchBlocks_2026-05-01.md` — **an empty shell for three months.**
  Sections 2–4 were never populated beyond `(populated below, file-by-file)`.
  Its single commit `4a724619` says the sweep was actually performed ("read
  all 51 source files with catches (226 total) and fixed the silent-failure
  cases I found"); two cited fixes verified live today —
  `broker_service.cpp:7125` warns on a malformed stored `inbox_schema_json`
  instead of returning an empty array, and `python_engine.cpp:1056` logs the
  `ctypes_sizeof` failure instead of returning 0.  The work happened; only
  the write-up was skipped.  **Carried forward:** `src/` now holds **385**
  catch blocks against the 226 swept in May, so ~159 have never been read.
  Folded into #87 (privilege & concealment audit) rather than left as a
  dead review file.
- `REVIEW_CURVE_Integration_2026-07-19.md` — self-marked complete, 0 open.
  Spot-checked two: `dev_mode` appears nowhere in `src/` (the claimed
  tombstone holds), and `with_seckey` is the use-not-export callback form in
  `curve_keypair.hpp:39`.

**Verified still open, kept:** `REVIEW_Connection_Inbox_Band_2026-05-17`
(16 items).  Sampled three: **X2 `query_shm_info` is genuinely dead** —
declared `broker_request_comm.hpp:447`, defined `:1506`, zero callers in
`src/` or `tests/` (independently matches `REVIEW_VALIDATION`'s O1b).  **X6
is already fixed** — `ChecksumRepairPolicy::Repair` is now a comment, not an
enumerator (`broker_service.hpp:48`).  **X3's forwarders are gone**; only
doc comments mention `resolve_bc_for_*`, which is finding X4.

**Count reconciliation.** `TODO_MASTER` carried "29 OPEN by cluster" for the
FullSystem review while the file header said "4 open".  The 29 predates the
#72 reconciliation (hep-gap + dead-residue clusters, 15 findings) and #57
(typed-envelope bypass, 4) — stale, not a competing measurement.  Of the
header's 4, three test-coverage items were verified present today and the
fourth is the federation bypass that #69 names verbatim.

**Sampling the unverified ✅ notes.** `REVIEW_VALIDATION` records that ~50
FullSystem resolutions are self-reported and never independently checked.
Six were sampled across this pass and **all six held** — including the one
that looked wrong at first glance: the `hub_cli` finding (help text cited a
`plh_role --print-pubkey` flag that does not exist) was resolved by pointing
at `--keygen` instead, and `plh_role_main.cpp:291` does print
`public_key : <pubkey>` to stdout.  Six of fifty is enough to trust the
record, not enough to call it verified — so the review was **not** archived.

### 2026-08-07 (TODO quality check per DOC_STRUCTURE §2.1.1 — 6136 → 5066 lines)

The subtopic TODOs had drifted from "what to do" into "what has been done":
every major file was 3–5× the ~300-line trigger, and the two most recent
entries I added myself were "Recent Completions" walls, which §2.1.1
explicitly forbids.

**Snapshot:** `docs/archive/transient-2026-08-07/todo-snapshot/` holds all
nine files verbatim as of this pass, so nothing below is lost.

**Trimmed in place** (git holds the history; the working surface should not):

| File | Was | Now | What went |
|---|---|---|---|
| `TESTING_TODO.md` | 1463 | 1084 | `## Recent Completions` wall + 3 ✅ sections + the all-RESOLVED L4 evidence log |
| `MESSAGEHUB_TODO.md` | 1178 | 803 | `## Recent Completions` wall + 7 completed ### sections + the closed native-inbox parity gap |
| `AUTH_TODO.md` | 956 | 809 | completed header narrative (CURVE migration, #101, slice 4) + the whole task-#101 section |
| `API_TODO.md` | 770 | 661 | 3 ✅ FIXED sections + the S-11 entry |
| `TOPOLOGY_TODO.md` | 553 | 506 | the ✅ SHIPPED fan-in arc |
| `PLATFORM_TODO.md` | 207 | 188 | `## Recent Completions` wall |

**Corrections found by reading code, not markers** — the reason §2.1.1 says
to verify against source:

- `API_TODO` carried `#92 S-11` as **❌ OPEN**. `hub_zmq_queue.cpp:525` shows
  the send loop checking `send_stop_ || ctx.shutdown_requested()`, with a
  comment explaining the asymmetry it fixed. Fixed; entry removed.
- `AUTH_TODO` asserted "**no code yet compares a claimed identity against
  the connection's verified key**" and that `PubkeyOrigin` /
  `pubkey_to_origin` "**neither exists in `src/`**". Both false —
  `struct PubkeyOrigin` is `security/pubkey_origin.hpp:68`, `PeerAuthority`
  is `:193`, and #83/#95/#96 shipped the comparison. Written before the
  work landed, never revisited.
- `AUTH_TODO` still titled a section "steps 1-6 of 9 built, **uncommitted**"
  for task #101, which shipped and committed on 2026-08-06.

**Verified still open** (kept, with the evidence): `channel_broadcast` has
no three-engine parity test — the four L2 dispatcher tests drive a *fake*
engine (`test_dispatch_notifications.cpp` defines its own
`invoke_on_channel_broadcast` capturing double), so no real binding is
exercised. `CONSUMER_ATTACH_REQ_ZMQ` is live in 4 files / 10 references
(Topology Phase E). `expected_schema_owner` is live in `wire_bodies.hpp` +
`broker_service.cpp`. HEP-0033 §11.0.5's *scoped* actuation origin does not
exist in `src/` at all — `origin_uid` is hand-threaded into exactly two
queue records.

**Not done in this pass, recorded so it is not mistaken for done:** the
large narrative bodies (`MESSAGEHUB` "Current Status" ~600 lines,
`API_TODO` "Current Focus", `TESTING_TODO` "Current Focus" ~1000 lines)
were not line-by-line re-verified — only their completed-marked
subsections were removed. And `TESTING_TODO` lines 16–106 hold *permanent*
test-design rules living in a transient file; they belong in
`README_testing.md`, and moving them is filed rather than done.

### 2026-07-31 (#92 security-tree review — 11 findings, merged into HEP-0035 + IMPLEMENTATION_GUIDANCE)

`REVIEW_SecurityTree_2026-07-30.md` completed (S-1..S-11 all ✅ FIXED) and moved
to `docs/archive/transient-2026-07-31/code_review/`.

The review existed because an earlier same-day pass over the same tree was
signal-driven — residue markers, compiler warnings, greps — rather than a read.
That pass covered ~8,500 of ~14,500 lines and produced two findings that were
**wrong** because they reasoned about unread code; one proposed a fix that
would have broken the HEP-CORE-0036 §6.7 Standby state. The completion pass
read every file in scope and carried a per-file coverage ledger throughout.

| Finding | Merged into | Shipped-code evidence |
|---|---|---|
| S-1, S-2 (vault parent-dir write bits; symlink-following read) | HEP-CORE-0035 §4.6.2 | `append_parent_dir_warning` sticky-bit carve-out + `read_file` `O_NOFOLLOW` (`key_file_acl.cpp`, `vault_crypto.cpp`); commit `654c2083` |
| S-3, S-4 (`SO_PEERCRED` fail-closed; memfd `F_SEAL_SHRINK\|GROW`) | — (local, no doc change) | `shm_capability_channel.cpp:469`, `:148` |
| S-5 (mutual-auth default) | HEP-CORE-0041 §9 | Default **removed** from `initiate_consumer_handshake`, not flipped — every call site now states its flow (`attach_protocol.hpp:231`) |
| S-6, S-9 (docs claiming more than the code checked) | — | `compute_blake2b_array` doc; CURVE guard comment now says *configured*, not *negotiated*, citing libzmq `options.cpp:1159` |
| S-7 (unbounded msgpack decode) | — | `wire_detail::decode_frame` — one bounded entry point replacing three raw `msgpack::unpack` sites; nine Pattern-1 tests in `test_layer1_zmq_wire_frame` |
| S-8 (failed `start()` left the queue Active-looking; retry reported success) | **IMPLEMENTATION_GUIDANCE** § "Derive state from the resource" | `make_scope_guard` + non-throwing boundary in all three `start()`s; pinned by `ZmqQueueAuthTest.FailedStart_LeavesQueueStandby_AndRetryStillFails`; commit `fab0b460` |
| S-10 (HEP §4.6.1's write recipe was prose, typed out 3×, drifted) | **HEP-CORE-0035 §4.6.1** + **IMPLEMENTATION_GUIDANCE** § "A prescribed recipe needs one implementation" | `security::write_keyfile(path, contents, role, policy)`; ~185 lines net removed; the vault was the only one of the three writers without `fsync`, and one Windows branch silently truncated where POSIX refused |
| S-11 (send retry ignored the shutdown signal) | — | Inner loop now tests `send_stop_ \|\| ctx.shutdown_requested()`, plus the `SendBlocked`/`SendRecovered` edge latch |
| Review method | **IMPLEMENTATION_GUIDANCE** § "Reading discipline for module-wide reviews" | read-don't-infer, per-file coverage ledger, prefer structural refusals, write the test before believing the finding |

**Two findings were falsified by their own regression tests, after they had
already shipped in the review document.** Recorded rather than quietly amended:

- S-8 was filed HIGH claiming a mistyped key name in config would abort the
  process. `validate_curve_factory_params` calls `ks.has(name)`, so the factory
  returns nullptr and a typo never reaches `start()`. The real window is
  construct → `KeyStore::remove` → `start()`. **Severity corrected to MED.**
- S-7's doc comment called `max_payload_fields` "the true ceiling" for the array
  bound. msgpack exposes one array limit for every array in a message and the
  outer frame is always a 5-tuple, so the bound cannot go below 5. Comment
  corrected; a test now pins the floor so nobody "tightens" it and breaks
  decoding.

One deliberate non-fix: S-11's original write-up proposed a retry ceiling. Not
implemented — discarding the owner's data after N attempts is a policy decision
that HEP-CORE-0011 assigns to the script, not the framework. The operator's
actual gap was visibility, which the edge latch closes.

Verified: Debug 2736/2736, Release 2733/2733. Commits `654c2083`, `0d497a01`,
`fab0b460`, `ea2ca457`, `e5f663d0`.


### 2026-07-26 (#74 objective peer counts — merged into HEP-0028/0007/0017/0036)

Design draft `DRAFT_objective_peer_counts_2026-07-26.md` (SHIPPED) merged into
canonical HEPs and moved to `docs/archive/transient-2026-07-26/tech_drafts/`.

| Document | Merged into | Shipped-code evidence |
|----------|-------------|------------------------|
| `DRAFT_objective_peer_counts_2026-07-26.md` | HEP-CORE-0028 §6a.2/§6a.3 (objective, `CHANNEL_COUNT_NOTIFY` source), HEP-CORE-0007 (wire catalog), HEP-CORE-0017 §3.3.2 (dialing-side count), HEP-CORE-0036 §I11 (count fan-out vs binding-only identity stream) | `BrokerServiceImpl::compute_channel_live_counts` + `fire_channel_count_notify` (`broker_service.cpp`); `NotificationId::ChannelCount` (`role_host_core.hpp`); `channel_counts` + count accessors (`role_api_base.cpp`); L4 e2e `ZmqE2E_MultiProducer_TwoAuthorized` asserts every role reads `producers=2 consumers=1` (commit `904318cb`). |

Residual (NOT lost): a focused L2 unit test for `compute_channel_live_counts` +
`CHANNEL_COUNT_NOTIFY` fan-out is deferred and tracked in `TESTING_TODO.md`
Recent Completions — the L4 e2e covers the behavior end-to-end.

### 2026-07-22 (admin console output-buffer design — merged into HEP-CORE-0033 §11)

Design note `DRAFT_admin_console_output_buffer_2026-07-22.md` merged into
`docs/HEP/HEP-CORE-0033-Hub-Character.md` §11 and archived to
`docs/archive/transient-2026-07-22/`. This **replaces the reverse push
notification path** the HEP previously sketched (an admin-thread notification
queue that pushed unsolicited `{notify, origin_uid}` frames) with a
**client-polled output buffer**.

**Merge map (draft → HEP-0033 §11):**
- Layer 6 rewritten push → pull output buffer + `admin_console_print` source →
  §11.0.1 (text + flowchart).
- Output-poll wire (`RESPONSE_QUERY_REQ` → `{status, lines[], dropped_count}`),
  queue record gains `request_id`, response-semantics completion-via-buffer →
  §11.0.4.
- `response_query` method (return-and-clear, no replay nonce) → §11.2.
- Plane-character + thread-model + transport rejustified (no push; persistent
  session + polling) → §11.0.2 table, §11.0.3 (table + hand-off rule + sequence
  diagram), §11.1 (message set, ROUTER/DEALER rationale, status blockquote,
  transport sequence diagram).

**Design decisions folded in:** one console at a time → one buffer (init on
connect, flush-to-log + clear on disconnect); return-and-clear poll; log is a
spill/flush sink only (overflow + non-empty-at-shutdown), never a per-line trail;
cap is a safety valve for the abnormal path (hung console / script flood); every
response JSON with explicit `status:"empty"` and each line's `content` a JSON
object; poll is session-id + skew checked with **no replay nonce** (idempotent
read); script source named `admin_console_print` (snake_case, matches
`band_join`/`on_init`). The §11.0.4 "fire-and-forget vs synchronous `not_found`"
tension needed no change — §11.0.4 already documented the accepted-not-completed
model and the synchronous validation reject.

**Remaining (IMPL, tracked `AUTH_TODO.md` Line E):** the buffer + `response_query`
handler + `admin_console_print` HubAPI (Lua/Python/native) + `origin_uid`/`request_id`
on the broker request records. Cap defaults + exact `admin_console_print` signature
are impl-time choices.

### 2026-07-22 (schema two-zone unification draft — implemented, merged into HEP-CORE-0034)

Design draft `DRAFT_schema_two_zone_unification.md` implemented and folded into
`docs/HEP/HEP-CORE-0034-Schema-Registry.md`, then archived to
`docs/archive/transient-2026-07-22/`.

**Merge map (draft → HEP-0034):**
- Two-zone `SchemaRecord` + single 64-byte `datablock_half ‖ flexzone_half`
  fingerprint → §2.2, §4.1, §6.3 (with byte-layout table + construction Mermaid).
- Unified API (`compute_zone_hash`, `compute_fingerprint_from_wire`,
  `make_schema_record`, `schema_records_equivalent`, `verify_request_fingerprint`)
  → §2.4 I2/I4/I6, §3 source-file table, §9.4.
- Two-jobs split (registry fingerprint vs data-plane `schema_tag`) → §2.2, §6.3, §3.
- Registration paths B/C flow + wrong-flexzone worked example → §9.2.
- Wire (128-hex `schema_hash`/`expected_schema_hash`, both zones on SCHEMA_ACK)
  → §10.1, §10.2, §10.3.

**As-built deviations from the draft checklist:** (1) the "inbox record →
make_schema_record" item was *superseded* — the inbox-as-schema-record was
removed entirely (broker block + HEP-0034 §11.4 rewritten to "not a registry
record" + HEP-0027 §4.0 "Inbox initiation & execution" + 5 `Sch_Inbox*` tests
retired); (2) `flexzone_packing` intentionally not added to channel structs
(packing folds into the fingerprint); (3) per-zone rejection-detail labeling
implemented (`hub_state.cpp::mismatch_zone`).

**Verification:** full sweep green — 2639/2639 (from 2644; −5 retired inbox tests).
New coverage added: L2 unit (`compute_zone_hash`, `make_schema_record`
both/db-only/fz-only, `schema_records_equivalent`, `verify_request_fingerprint`),
L2 state (flexzone-only record, zoneless-forbidden, flexzone-mismatch citation).

Also archived this batch: **`DRAFT_schema_validation_consolidation_2026-07-20.md`**
(single-validator consolidation, task #68 item 1). Verified fully implemented +
merged: `verify_request_fingerprint` (Job A) + `_validate_schema_citation` as the
single validator on every joiner path (Jobs B+C) + `CitationOutcome::kSchemaIdMismatch`
all shipped (commit `2020078a`); model merged into HEP-0034 §9 (three-questions
process), §2.4 I4, §9.2, §9.4. Its stale "No code yet" status was corrected before
archiving. (The draft's `compute_canonical_hash_from_wire` references predate the
two-zone rename to `compute_fingerprint_from_wire`.) Also fixed superseded
`REVIEW_FullSystem_2026-07-20.md` findings — schema_utils.hpp:239 duplication
(RESOLVED-BY-REDESIGN: two-jobs split) and schema_utils.hpp:331 flexzone-loss
(FIXED: two-zone record).

Also archived this batch: **`DRAFT_curve_admin_protocol_2026-07-15.md`** — the
admin-CURVE implementation checklist. The design SHIPPED 2026-07-19 (`132732ca`,
`07ca94c9`, `be9d8dfc`, `5cd3be62`, `43050a98`, `f54da590`): CURVE-secured admin
socket + typed operator-console (ROUTER + session + all 11 methods) + in-session
replay defense (`HubHost::nonce_seen`). Design of record is now HEP-CORE-0033
§11.1 + §11.3; the draft already carried its own "Superseded 2026-07-19" banner.
The 2026-07-18 KEEP note ("impl not started = next work") is thereby superseded.
Residual polish (reverse-notify, `origin_uid`, 3 admin-test migrations) tracked in
`AUTH_TODO.md` Line E, not in the archived draft.

### 2026-07-12 (REG/REG_ACK protocol redesign draft promoted to HEP-CORE-0046)

Design draft `DRAFT_reg_ack_protocol_redesign.md` (2143 lines, DESIGN
LOCKED with 21 named invariants + typed wire envelope contract in
§14) promoted to `docs/HEP/HEP-CORE-0046-REG-Protocol-Redesign.md`
alongside adding cross-references from 8 REG-adjacent HEPs
(HEP-CORE-0007, -0017, -0021, -0023, -0033, -0035, -0036, -0042) and
adding a normative "REG Protocol Wire Discipline (HEP-CORE-0046)"
rule to `docs/IMPLEMENTATION_GUIDANCE.md`.

Rationale for HEP promotion (rather than merging as amendments into
existing HEPs):

- Cross-cutting normative invariants (21 stated in §8.1) that any
  future REG-family work MUST follow — HEP is the only doc tier
  in this project that carries normative authority.
- Wire spec (§14 typed envelope + §14.5 admission-gate pipeline)
  and phase sequencing (§12) are subsystem-scoped, not
  amendment-scoped.
- Consistency with HEP-CORE-0036 (auth wire), HEP-CORE-0042
  (attach coordination), HEP-CORE-0045 (SHM observer) — one HEP
  per subsystem contract.
- Promotion happens BEFORE Phase B (dispatch rewire) lands so
  Phase B code is written against normative HEP text, not
  against a discussion-tier tech draft.

Phase A (typed envelope + body classes) and Phase C islanded
modules (admission gates, REG pipeline, broker adapter,
HubState::nonce_seen) are shipped as compile-verified
L1/L2-tested modules but PENDING Phase B (broker dispatch rewire
+ BRC envelope migration) to be live in production.  Phase B is
the next active commit.

### 2026-07-12 (queue-owned topology + layer cleanup plan closed)

Design draft `DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md`
tracked the seven-phase queue-owned topology + layer cleanup arc
(P1 contract → P2 correctness bugs → P3 queue-owned dial →
P4 gate reads queue → P5 queue-owned confirm → P6 data-structure
cleanup → P7 HEP sweep).  All arc-close phases landed with
commits `c665de0c` (P1–P5 code + tests) and `db2bbc21` (P7 HEP
sweep + trailing §I9.1 code fixes).  Deferred follow-ups (P6
data-structure refactor + D-3 clang-tidy rule) tracked under
canonical TODOs.  Draft archived; lasting design in HEP-CORE-0011
§"Loop-ready gate" + HEP-CORE-0036 §I9.1 / §6.5 step 6 / §6.6.3
+ HEP-CORE-0042 §5.5.2 amendment.

### 2026-07-11 (loop-ready gate + fan-in binding queue draft merged into HEPs)

Design draft `DRAFT_loop_ready_gate_and_binding_queue_2026-07-11.md`
served as the working sketch for the fan-in binding-side reader
correctness arc (Tasks #7 / #8 / #9).  All lasting design content
now lives in the permanent HEPs and README topic summaries; draft
archived to `docs/archive/transient-2026-07-11/`.

Where the draft's content landed:

- **§4.1 (`handle_channel_auth_notifies` consumer branch — binding-
  side APPLIED_REQ)** → **HEP-CORE-0036 §6.5 step 6** (consumer
  emits APPLIED_REQ after installing snapshot allowlist) and
  **HEP-CORE-0042 §5.5.2 amendment 2026-07-11** (extended wire
  with `role_type` discriminator; consumer branch has no
  stale-instance guard).  Shipped code differs mechanically from
  the draft sketch — draft used a `hub::Queue *q` intermediate
  with `is_binding_side()`; shipped code uses direct `dynamic_cast`
  on the separate `QueueReader` / `QueueWriter` hierarchies with
  rx-then-tx fallback.  Semantically identical; the draft sketch
  captured intent, the HEPs capture the final contract.

- **§4.2 (loop-ready gate: framework floor AND script hook)** →
  **HEP-CORE-0011 §"Loop-ready gate"** (contract) and shipped
  `data_loop.hpp` `run_data_loop()` around
  `init_done = Ops::default_init_ready(api) && script_hook_result`.
  100 ms pacer (`kLoopReadyPollInterval`) lives in
  `loop_timing_policy.hpp` and is referenced from HEP-CORE-0011
  as the "pre-Ready cycle cadence" constant.

- **§6 (per-role `default_init_ready` semantics)** → shipped
  `cycle_ops.hpp` — Producer floor always true, Consumer /
  Processor floor gated on `admitted_peers_count >= 1`.  Pinned
  by the L2 `RunDataLoopTest.FrameworkFloorHoldsGate` scenario
  (Task #7).

- **§6.6.3 (dial-side readiness pull, new wire `CHECK_PEER_READY_REQ`)**
  → **HEP-CORE-0036 §6.6.3** (authorization mirror of §6.6.1,
  reply shape `"ready" | "not_ready"` with reason
  `"not_admitted" | "not_confirmed"`, error codes
  `CHANNEL_NOT_FOUND` and `NOT_A_ROLE_OF_CHANNEL`).  Shipped
  handler at `src/utils/ipc/broker_service.cpp`
  `handle_check_peer_ready_req` matches wire fields exactly
  (`channel_name`, `role_uid`, `pubkey_z85`, `correlation_id`).

- **Deferred-connect two-phase** (draft §4.1 tail) →
  `src/utils/hub/hub_zmq_queue.cpp` `apply_master_approval`
  detects fan-in DIALING PUSH and sets `dial_pending=true` without
  calling `start()`; producer host calls `api_ref.dial_now()` only
  after `wait_for_peer_ready` observes broker's readiness.  Test
  `TopologyFactory_FanInProducer_WireApplyMasterApproval` pins
  the two-phase contract.

TODO index status recorded via Task #8 update pass on 2026-07-11
(`docs/TODO_MASTER.md` fan-in arc entry, `docs/todo/API_TODO.md`,
`docs/todo/MESSAGEHUB_TODO.md`, `docs/todo/TESTING_TODO.md`,
`docs/todo/TOPOLOGY_TODO.md`).

---

### 2026-07-10 (REG protocol addendum merged — task #96 closes)

Doc-only consolidation.  No code changes.  Task #96 (merge
invariants addendum into main REG protocol draft) closes with the
merge of `DRAFT_reg_ack_protocol_redesign_addendum_invariants.md`
(design gap audit surfaced by the fan-in NOTIFY-routing bug on
2026-07-10) into the main draft `DRAFT_reg_ack_protocol_redesign.md`.

Main draft §8.1 expands from 4 invariants to 21, grouped by
concern: state/ordering (I-ROUTER-SERIAL, I-STATE-MUTATION-ATOMIC,
I-OPT-ADMIT, I-MONOTONIC-VERSION), identity (I-DEALER-IDENTITY,
I-PUBKEY-BINDING, I-CURVE-IS-DECLARED, I-CHANNEL-SINGLE-BINDING-SIDE),
security (I-REPLAY-BOUND, I-ENVELOPE-BODY-BINDING,
I-KEY-ROTATION-VIA-DEREG), wire (I-WIRE-ENVELOPE,
I-WIRE-VERSION-ATOMIC), delivery (I-CORRELATION-STABLE,
I-NOTIFY-BEST-EFFORT, I-ROUTER-NOT-MANDATORY), state model
(I-INSTANCE-ID-EPHEMERAL, I-PENDING-EPHEMERAL), policy
(I-BRC-BUDGET, I-REQ-IDEMPOTENT, I-MSG-TYPE-TAXONOMY).

Main draft new §14 (typed wire envelope contract) locks the wire
skeleton (4 skeleton frames + 1 body frame) and the per-msg-type
typed body class catalog (RegReqBody, RegAckBody, ...,
BandJoinNotifyBody).  Replaces scattered `body.value("field")`
JSON extraction across broker + role + BRC with typed accessors.

Main draft §2 subsections rewritten to reference the typed body
classes by name; §2's own wire-shape blocks retired (single source
of truth in §14 avoids drift).

Main draft §12 sequencing updated to the new phase order:
A (typed envelope skeleton) → B (BRC + broker rewired) →
C (admission-gate ordering) → D (retirements) → E (coverage tests)
→ F (federation follow-on).

Addressing primitives newly stated as invariants closed the fan-in
NOTIFY-routing bug at design level (not patch level): libzmq's
default `rand()`-derived DEALER identity collided across fresh role
processes; I-DEALER-IDENTITY makes routing_id ≡ role_uid mandatory
and wire-enforced.  Security invariants I-REPLAY-BOUND +
I-ENVELOPE-BODY-BINDING + I-KEY-ROTATION-VIA-DEREG close attack
surfaces (message replay after DEREG, envelope↔body splice, in-band
key rotation) that CURVE frame encryption does not cover.

Archived: `DRAFT_reg_ack_protocol_redesign_addendum_invariants.md`
→ `docs/archive/transient-2026-07-10/`.

Follow-on tasks: #97 (WireEnvelope class + typed body classes),
#98 (handler sweep), #99 (retire zmq_identity), #95 (fan-in NOTIFY
test passes as consequence).

---

### 2026-07-08 (HEP-CORE-0021 §16 amendment — task #94 closes)

Doc-only consolidation.  No code changes.  Task #94 (ephemeral
port binding) closes with a normative amendment to HEP-CORE-0021
§16 (previously RESERVED) that respects HEP-CORE-0036 §3.5.1
("no data-plane footprint before auth") by binding at S3
(post-REG_ACK) and publishing the resolved port over the
CURVE-authenticated CTRL via `ENDPOINT_UPDATE_REQ` before the
producer's first heartbeat.  Consumer admission gated on
`Live AND (SHM OR zmq_node_endpoint_resolved)` via an extended
R6 gate.  Mid-life re-update rejected with a new
`ENDPOINT_CHANGE_FORBIDDEN` error code when consumers are
already attached (prevents stranding live PULL connections).

Pre-existing code/doc drift resolved: the broker's
`handle_endpoint_update_req` at `broker_service.cpp:4451` has
been alive since Wave M2.5 and still emits `INVALID_ENDPOINT`,
`NOT_CHANNEL_OWNER`, `INBOX_UPDATE_NOT_SUPPORTED`,
`UNKNOWN_ENDPOINT_TYPE` — but HEP-CORE-0007 §12 lines
1535/1554/1555/1556 marked all four as RETIRED 2026-06-12.  The
amendment un-retires the four (they were live in the handler)
and adds `ENDPOINT_CHANGE_FORBIDDEN` for the new mid-life
change contract.  `ENDPOINT_ALREADY_SET` stays retired — it
belonged to the pre-REG bind variant and is superseded by the
idempotent-if-same / consumers-attached-forbidden semantics.

Cross-references cascaded so HEP-CORE-0021 §16 is the single
source of truth for the ENDPOINT_UPDATE_REQ wire and state
machine:

- **HEP-CORE-0007 §12** — Request/Response table row + wire
  catalog list + REG_REQ `zmq_node_endpoint` field comment
  updated to point at HEP-CORE-0021 §16.5 / §16.6.  Error-code
  catalog: 4 rows un-retired, `ENDPOINT_CHANGE_FORBIDDEN` added,
  `ENDPOINT_ALREADY_SET` clarified.
- **HEP-CORE-0033** — code-catalog comment §1247 + wire-catalog
  row §2994 un-retired with pointers to HEP-CORE-0021 §16.
- **HEP-CORE-0036 §I7** — "Post-task-#94 (future)" caveat
  replaced with the shipped-design pointer.  Explicitly notes
  the new path does NOT reintroduce a pre-REG bind requirement.
- **HEP-CORE-0027 §4.1 / §4.2** — clarified that port-0 inbox
  endpoints remain unsupported (rejected via
  `INBOX_UPDATE_NOT_SUPPORTED`); only `zmq_node` is updatable.
  Rationale explained (inbox discovery via ROLE_INFO_REQ on
  demand creates cache-consistency issues that post-bind update
  wouldn't cleanly solve).
- **HEP-CORE-0017 §3.3** — `ProducerPeer::endpoint` pointer
  redirected from HEP-CORE-0007 §12.3 to HEP-CORE-0021 §16.3
  (the per-producer scope authority).

No HEPs merged or retired.  Each keeps its distinct purpose:
HEP-CORE-0007 is the wire catalog (short pointers to owning
HEPs); HEP-CORE-0021 owns ENDPOINT_UPDATE_REQ; HEP-CORE-0033
is the broker-character index; HEP-CORE-0036 owns the
auth-door principle; HEP-CORE-0027 owns inbox messaging.

Amendment marked "ADOPTED 2026-07-08 (draft)" — flips to
"(final)" when user approves the completed design + doc-only
consolidation.  Production code wiring (`role_api_base.cpp` S3
sequence) and L3/L4 tests land in a follow-up commit after
approval.

---

### 2026-07-08 (HEP-CORE-0044 + HEP-CORE-0045 promoted)

Two new HEPs promoted to authoritative status; one tech draft
archived; two content-migration blocks in HEP-CORE-0041 replaced by
pointers.

- **HEP-CORE-0044 (AttachProtocol)** authored fresh — the
  transport-agnostic application-layer challenge-response primitive
  that HEP-CORE-0041 §5.5 / §D4 / §D4.5 / §10.5 previously specified
  by name.  Wire spec, `IAttachChannel` seam, transport-agnostic
  protocol helpers (`run_producer_handshake` /
  `run_consumer_handshake`), Frame 3 mutual auth, and the
  `role_type="observer"` extension are all authoritative here now.
- **HEP-CORE-0045 (Broker SHM Channel Observer)** promoted from
  `docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md`.  The design
  captured D1-D5 decisions + risks + slice map for task #317; the
  promoted HEP wraps them with a plain-language overview + worked
  example + refactored section structure for single-source lookup.
- Archived:
  `docs/archive/transient-2026-07-08/DRAFT_broker_shm_observer_2026-07.md`
  — content lives in HEP-CORE-0045.
- HEP-CORE-0041 §5.5 replaced with a pointer to HEP-CORE-0044.
- HEP-CORE-0041 §D4.5 replaced with a pointer to HEP-CORE-0044 §8.
- HEP-CORE-0041 §10.5 replaced with a pointer to HEP-CORE-0045.
- HEP-CORE-0043 §6 cross-references HEP-CORE-0044 as the primary
  consumer of `box_encrypt_using` / `box_decrypt_using`.
- HEP-CORE-0043 §9.2 / §9.3 updated to point at HEP-CORE-0044 /
  HEP-CORE-0045 respectively.
- Speculative code deleted: `attach_channel_zmq.hpp/cpp`,
  `attach_protocol_zmq.cpp`, `test_zmq_attach_channel.cpp`,
  `test_zmq_attach_protocol.cpp`.  Rationale: none of the three
  lines of work (main auth chain / SMS consolidation / broker
  observer) needs a ZMQ AttachProtocol binding; HEP-0036 makes
  CURVE + ZAP the whole ZMQ auth story.  HEP-CORE-0044 §10
  formalizes why.

### 2026-07-06 (SEC-Fold-2 KeyStore merger — HEP-CORE-0043 §2.2 + §7 SHIPPED)

The KeyStore-into-SecureSubsystem merger shipped 2026-07-06 alongside
the SMS lifecycle-module refactor.  HEP-CORE-0043 §2.2 (KeyStore as
member of `SecureSubsystem::Impl`) and §7 (KeyStore API surface) are
now present-tense authoritative.  HEP-CORE-0040 §4 (SecureMemorySubsystem)
and §5 (KeyStore) received inline "SUPERSEDED IN FULL" callouts pointing
to HEP-CORE-0043.  HEP-CORE-0038 banner rewritten to reflect the same.

Archived rationale document (no longer needed as decision record — the
shipped HEP-CORE-0043 supersedes its purpose):

* `DRAFT_security_module_and_hep_consolidation_2026-07.md` — 479-line
  design-rationale draft from 2026-07-04 capturing the CI-triage
  sodium_init failure that motivated HEP-CORE-0043's creation.
  Content substance absorbed into HEP-CORE-0043 §0-§2 (the "Why"
  narrative + the fold decision).  Historical value only; not
  authoritative.  Location: `docs/archive/transient-2026-07-06/`.

Additional transient docs archived after SEC-Fold-2 Phases A-E +
review-follow-ups all shipped:

* `DRAFT_sec_fold_2_plan_and_guidance_2026-07.md` — the phased plan
  doc + five-point pattern rule.  Its substance is in HEP-CORE-0043
  §1-§2 + the shipped code + the `feedback_sms_five_point_pattern.md`
  memory file.  Location: `docs/archive/transient-2026-07-06/`.
* `DRAFT_sec_fold_2_resume_state_2026-07.md` — session-resume
  document tracking in-flight state.  Now obsolete — all listed
  work shipped 2026-07-06.  Location:
  `docs/archive/transient-2026-07-06/`.
* `DRAFT_task_list_snapshot_2026-07.md` — handoff snapshot of
  in-session task tracking.  Superseded by the shipped code +
  archived rationale docs.  Location:
  `docs/archive/transient-2026-07-06/`.


### 2026-07-01 (task #246 Phase 1 promotion — HEP-CORE-0042 adopted)

Task #246's tech_draft substance promoted to **HEP-CORE-0042 (Channel
Attach Coordination Protocol)** — a new HEP that owns the transport-
agnostic pre-attach coordination pattern.  Under Execution B, both SHM
and ZMQ pre-attach instantiate the same protocol; HEP-CORE-0041 §5.4
(SHM CONSUMER_ATTACH_REQ) relocated to HEP-0042 §6.1 Bindings.SHM
(HEP-0041 §5.4 becomes a 3-line pointer).  Sibling HEPs (0036, 0011,
0028) updated per HEP-0042 §11.

* `DRAFT_HEP-0036_zmq-pre-confirm_2026-06-30.md` — 645-line design
  draft from 2026-06-30 promoted verbatim to HEP-CORE-0042 §5-§8
  (abstract protocol + ZMQ binding).  Design principles P0-P4 +
  review guardrails table lifted from the draft's Design Principles
  section.  §7 designer decisions all locked 2026-07-01 with
  recommendations approved by user (see chat transcript).
  §10 sibling HEP contract text pasted verbatim into HEP-0042 §11
  cross-references.  Archived to
  `docs/archive/transient-2026-07-01/tech_draft-completions/DRAFT_HEP-0036_zmq-pre-confirm_2026-06-30.md`.

### 2026-06-30 (HEP-0041 1i-cleanup S5 close-out)

After #275 S5 shipped (renamed `SharedMemoryHeader::shared_secret[64]`
→ `reserved_capability_token[64]` per the Core Structure Change
Protocol), the pre-walked review doc was archived per
`REVIEW_S5_CoreStructure_2026-06-27.md` closing checklist:

* `REVIEW_S5_CoreStructure_2026-06-27.md` — 9-item impact-matrix
  pre-walk done at Phase 0b (2026-06-27); every checkpoint verified
  green when S5 executed 2026-06-30 (build clean, full ctest 2261/2261,
  static_assert(sizeof(SharedMemoryHeader)==4096) passes).  Lasting
  reference lives in `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md`
  §1i-cleanup status table + `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
  §"Security and Schema" refreshed layout diagram.  Archived to
  `docs/archive/transient-2026-06-30/code_reviews/REVIEW_S5_CoreStructure_2026-06-27.md`.

### 2026-06-18 (HEP-0041 doc-hygiene sweep — post-1h close-out)

After HEP-CORE-0041 Phase 1 substeps 1f-1h shipped (#253/#254/#255,
commits `dd7e2770..3c5563b0` + 1g/1h follow-ups) and the post-substep
doc audit fixed M1/M2/M3/S3 (commit `be6a5116`), the S/C-tier deferred
items from the same audit were closed out and one stale tech_draft was
archived:

* `DRAFT_C2-ZmqAuthOptions-deletion-audit_2026-06-08.md` — C2 cleanup
  audit notes for task #158 (Z85PublicKey strong type + ZmqAuthOptions
  deletion).  Task #158 shipped; every site listed in the audit's
  inventory tables was migrated.  No lasting design content (the file
  was an inventory checklist, not a design proposal) — no
  permanent-doc absorption needed.  Archived to
  `docs/archive/transient-2026-06-18/DRAFT_C2-ZmqAuthOptions-deletion-audit_2026-06-08.md`.

Companion doc edits in the same sweep (no archival — content stays
permanent):

* HEP-CORE-0041 — S1 callout above §3.2 making "Phase 1 ships Option
  A only" explicit (the Options B/C/D subsections survive as
  archaeology + future-phase enhancement paths).
* HEP-CORE-0031 §2 — one-paragraph callout pointing role-scope worker
  threads (DataLoop, BRC poll, RxQueue poll, SHM
  `ShmAttachOrchestrator` accept loop) at the role host's existing
  ThreadManager instance (vs. lifecycle modules, which are for
  process-singleton infrastructure shared across roles).
* HEP-CORE-0040 — "Related" line tightened: HEP-0041 is now described
  as an ACTIVE consumer of the SeckeyAccessor callback (Phase 1
  substep 1c), not just a future Phase-4 hypothetical.
* docs/todo/AUTH_TODO.md — added HEP-0041 to the "Authoritative design
  lives in" list with substep-chain + pre-flight task pointers.
* IMPLEMENTATION_GUIDANCE.md "Error Taxonomy" — added "SHM Channel
  Auth attach errors" subsection codifying the wire-protocol error
  vocabulary (OK / INVALID_REQUEST / CHANNEL_NOT_FOUND /
  PRODUCER_NOT_AUTHORIZED / INTERNAL_ERROR) used by
  `AttachProtocolAcceptor` and the L3 test-marker logging contract.

Active tech_drafts retained (5):
* `DRAFT_HEP-0036-implementation-guideline_2026-05.md` — drives the
  AUTH-1..7 chain (#103 in_progress; #74/#94/#102/#104/#106 pending).
  Annotated 2026-06-16 with HEP-0041 alignment banner.
* `SCRIPT_RELOAD_DESIGN_2026-05-20.md` — task #76.
* `engine_callback_tiers.md` — task #77.
* `raii_layer_redesign.md` — API_TODO "Template RAII".
* `abi_check_facility_design.md` — API_TODO "ABI Check Facility".

### 2026-06-16 (HEP-0041 supersession sweep — post-design cleanup)

After HEP-CORE-0041 (SHM Channel Auth) shipped (#244, commits
`565104b8..94b04576`) and the sibling HEP cross-reference sweep
(commit `df001a33`) added forward-pointers across the 8 sibling HEPs
that mention `shm_secret`, one stale tech_draft was archived:

* `HEP-0036_review_open_items.md` — pre-HEP-0041 review-open-items
  tracker.  Original review items closed under AUTH-1..3 (#103/#162/
  #163); residual SHM-secret references made obsolete by HEP-0041
  capability-transport model.  Archived to
  `docs/archive/transient-2026-06-16/HEP-0036_review_open_items.md`.
  Lasting design content (none — the file was a review tracker, not
  a design proposal) — no permanent-doc absorption needed.

Active tech_drafts retained (6):
* `DRAFT_HEP-0036-implementation-guideline_2026-05.md` — drives the
  AUTH-1..7 chain.  Annotated 2026-06-16 with HEP-0041 alignment
  banner at top (SHM passages are informational-historical).
* `SCRIPT_RELOAD_DESIGN_2026-05-20.md` — task #76.
* `engine_callback_tiers.md` — task #77.
* `raii_layer_redesign.md` — API_TODO "Template RAII".
* `abi_check_facility_design.md` — API_TODO "ABI Check Facility".
* `DRAFT_C2-ZmqAuthOptions-deletion-audit_2026-06-08.md` —
  C2-cleanup audit notes.

### 2026-06-09 (AUTH_TODO restructure — C-chain + HEP-0040 chain + HB audit)

After C1..C5 strict-CURVE cleanup chain shipped (#157-#161 + #186 +
#187) and the HEP-CORE-0040 Locked Key Memory chain shipped
(#165–#176), `docs/todo/AUTH_TODO.md` was reorganized from 635 lines
of history + plan into ~415 lines of clean AUTH-1..7 critical-path
plan + decision log + backlog.  Historical sections moved verbatim
to `docs/archive/transient-2026-06-09/todo-completions/AUTH_TODO_completions.md`:

* 2026-06-05 HB-1..HB-6 audit (the silent-fallback hole + verified
  hard-blocks + test blind spots).
* 2026-06-05 PM REFRAME — HEP-CORE-0040 absorbs the storage half of
  C3 + #102.
* Strict-CURVE cleanup chain — C1..C5 full inventory + per-commit
  scope tables + implementation order.
* Phase 5 deferred follow-ups (#186 mechanism binding + L3 NULL-mech
  test) + #187 vault load-path tightening.
* Phase C fresh-eye review (2026-06-09) — design residue cleanup.

Mapping of old labels → new AUTH-N numbering preserved in the new
AUTH_TODO.md §"Critical path — AUTH-1 .. AUTH-7" so prior commits +
references resolve cleanly.  `docs/TODO_MASTER.md` P3 section + the
production-readiness gap table updated to the new numbering.
`MEMORY.md` label-hygiene table extended with the AUTH-N / C1..C5 /
HB-1..6 / HEP-0040 entries.

### 2026-06-05 (HEP-CORE-0040 promotion)

`HEP-CORE-0040-Locked-Key-Memory-DRAFT.md` promoted from
`docs/tech_draft/` to `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md`
after four fresh-eye review rounds (task #166). Status banner
changed from DRAFT to "Design — impl in flight"; supersession of
HEP-0035 §4.7 utility-only spec is now in force. HEP-0035 §4.7
rewritten to a short consumer-requirement statement citing HEP-0040
for implementation (task #168).

### 2026-06-05 (TODO cleanup — verified-shipped completions)

Periodic TODO quality check (per `DOC_STRUCTURE.md` §2.1.1) verified
each completion claim against actual code rather than commit
messages.  Items confirmed shipped in code moved to
`docs/archive/transient-2026-06-05/todo-completions/`; active TODOs
trimmed to focus on open items only.  No content summarized —
verbatim prose preserved in the archive.

Per-area archive files:

* **`AUTH_TODO_completions.md`** — Phase A/B/C; D1+D2+D3; landing-
  phase §4.6.5 no-bypass cleanup; BRC monitor CURVE blindspot
  investigation (LOW, test artifact only — no production action
  needed); lib-stabilization exclusion procedure (now superseded
  by task #154); resolved decisions reference; considered-but-not-
  pursued.
* **`API_TODO_completions.md`** — Task #78 final closure
  (E′-1/E′-2a/E′-2b/E′-2c); Commit C′-1 `parse_auth_config`
  missing-field hard error; Task #101 HEP-0035 §4.6 key-file ACL
  discipline (`key_file_acl.{hpp,cpp}` shipped); C2 + X1 code-ahead
  items (heartbeat-tick iterates `presences()` per HEP-0033 §19.3;
  `BrokerRequestComm::send_notify` already removed per audit O1).
* **`TESTING_TODO_completions.md`** — Wave-B M9 (#72 + #100)
  RoleHostFrame closure with Q1+Q2+Q3 quality concerns resolved;
  N1 (#83) config→opts translation L2 round-trip test.
* **`QUERY_LAYER_TODO_completions.md`** — Pattern P8 full migration
  shape (Pass-1 + Pass-2 with `Pass2Decision` struct +
  `channel_torn_down` short-circuit + two-snapshot invariant);
  migration prerequisites; tasks #143-#150.
* **`MESSAGEHUB_TODO_completions.md`** — A1 `ctx_band_leave`
  semantic bug (was listed as "D1 must-fix" in active TODO; fix is
  in code at `src/utils/service/native_engine.cpp:289-305` with
  inline audit comment citing the 2026-05-20 discovery).

Stale references corrected in active TODO files:
* `MESSAGEHUB_TODO.md` Current Status table now reflects Wave-B M9
  shipped + HEP-0035 control plane partially shipped (D1+D2+D3).
* `TODO_MASTER.md` Production-readiness gap row now reflects D1+D2+D3
  shipped; D4-D7 + data-plane CURVE + producer-side ZAP open.

Method note: all completion claims were verified by either
`git log` for the cited commit hash, file existence + grep for the
cited symbol, or `git show --stat` to confirm the change matched
the description.  Items that named no concrete artifact were left
in the active TODO marked as scope items, not closures.

### 2026-06-02 (HEP-CORE-0039 promotion + tech_draft archive)

HEP-CORE-0039 "Hub State Query Layer" promoted from tech_draft to
permanent HEP after two fresh-eye reviews + one fix-pass re-review.
Promoted with substantial expansion: internal-broker query helpers
(Layer 2a typed surface alongside the original Layer 2b JSON
surface), inline-join hotspot inventory across `broker_service.cpp`
and `hub_state.cpp` (~25 sites grouped by pattern), naming /
return-type / `string_view` convention codification, Phase D
(PeerAdmission broker glue) coupling map.

Coordinated amendments landed in same commit batch:
* **HEP-CORE-0033 §8** — `HubStateSnapshot` metadata fields
  documented (`captured_at`, `captured_mono`, `hub_uid`,
  `snapshot_seq`)
* **HEP-CORE-0033 §12.3** — `HubAPI::snapshot()` recorded as the
  primary read primitive; single-aspect convenience reads
  recharacterized as "use when only one aspect needed; for
  multi-aspect coherence, prefer snapshot()"
* **HEP-CORE-0019 §3** — cross-reference paragraph noting the
  script-side snapshot path as a parallel entry alongside the
  broker-side `MetricsStore`/`METRICS_REQ` admin RPC path

Sibling subtopic TODO **`docs/todo/QUERY_LAYER_TODO.md`** created
with concrete file:line citations for the ~25 inline-join sites
organized by 8 patterns + 1 out-of-scope mutator pattern.

One transient moved to `docs/archive/transient-2026-06-02/`:

* **`docs/tech_draft/hub_state_query_layer_design.md`** — original
  2026-05-20 design sketch.  Lasting content fully merged into
  HEP-CORE-0039; archived per `DOC_STRUCTURE.md` §1.8 ("when content
  is agreed upon and finalized: merge → move to archive → record
  here").

### 2026-06-02 (tech_draft sweep — 13 docs archived after PeerAdmission close-out)

Following the PeerAdmission tech_draft archive (above), audit of the
remaining 20 entries in `docs/tech_draft/` against the locked HEP
corpus + MEMORY-recorded shipped work identified 12 additional
archive candidates.  All moved to `docs/archive/transient-2026-06-02/`.
Open items lifted into subtopic TODOs to preserve context.

**Unambiguous archives** (banner says SHIPPED / SUPERSEDED, work
recorded in MEMORY + git log):

| Document | Where the lasting content lives |
|---|---|
| `broker_test_migration_plan.md` | M1.2/M1.3 ship (commit `a41ce71`) + `docs/README/README_testing.md` patterns |
| `controlled_access_api_design.md` | Wave M2.5 ship + `docs/code_review/REVIEW_WaveM2.5_*` |
| `GROUP2_DECISIONS_2026-05-20.md` | HEP-CORE-0039 + completed tasks #140-#150 |
| `M1.5_channel_closing_redesign_2026-05-12.md` | Commit `c177c99` + D1 audit `81804d7b` |
| `M1_FSM_consolidation_handoff_2026-05-09.md` | M1.2 (`a41ce71`) + M1.4 (`4e902e1`) + M1.5 (`c177c99`) |
| `M3_role_entry_controlled_access.md` | Wave M3 ship + `docs/code_review/REVIEW_WaveM3_*_2026-05-11.md` |
| `MD1_role_teardown_ordering_2026-05-12.md` | HEP-CORE-0031 §4.1 + commits `42092cb..4a5347c` |
| `test_compliance_audit.md` | `docs/README/README_testing.md` + Pattern-3 wave commits `6dfb86d..1ed9cc8` |

**Follow-up archives** (lasting content in HEPs; open items lifted
into subtopic TODOs):

| Document | Where the lasting content lives | Open items moved to |
|---|---|---|
| `role_host_template_design.md` | Wave-B M0–M9 shipped (task #72 + #97-#100).  RoleHostFrame / presences / FlexzoneInfoCache referenced in HEP-CORE-0011 + HEP-CORE-0023 + HEP-CORE-0033.  The full 128k design history is preserved verbatim in the archive copy. | — (no open items; reference design only) |
| `DISCOVERY_2026-05-20.md` | §1 + §2.1-§2.3 + §4 + §5 closed (per banner + git log).  §3 review findings + §2.4 dead-code candidates + §7 doc bookkeeping debt = open. | `docs/todo/MESSAGEHUB_TODO.md` § "Open items from 2026-05-20 post-band-authority discovery" |
| `HUB_TEST_COVERAGE_PLAN.md` | 2026-05-05 snapshot of L2/L3/L4 coverage; L4 plans subsumed by demo framework + 9 manifests (task #44 done 2026-05-26). | `docs/todo/TESTING_TODO.md` § "Open coverage items from 2026-05-20 discovery + 2026-05-05 HubAPI coverage plan" |
| `SRC_STRUCTURE_PLAN.md` | Deferred-execution design; no urgency to promote. | Pointer added to `docs/todo/API_TODO.md` § Deferred future work; design preserved verbatim in archive |

### 2026-06-02 (PeerAdmission tech_draft archive)

PeerAdmission Phase D is unblocked by all P-* decisions resolving
through the locked HEPs.  Tech_draft fully superseded — every
load-bearing design point is now in HEP-CORE-0035 / HEP-CORE-0036 /
HEP-CORE-0017 §3.3 (Phases A/B/C shipped; the §8 decision table is
captured in the new TODO).  Audit confirmed 0 HEP merges needed.

One transient moved to `docs/archive/transient-2026-06-02/`:

* **`docs/tech_draft/peer_admission_architecture_design.md`** —
  PeerAdmission working design + threat-model + §8 decisions log.
  All load-bearing content already in:
    - HEP-CORE-0036 §4 (`ChannelAccessIndex` shape, three-tier
      architecture), §5 (REG / CONSUMER_REG / SHM sequences with
      `shm_secret` + `producers[]`), §6.5 (CHANNEL_AUTH_UPDATE
      snapshot wire), §7.1 (caller-pumped threading), §9.x
      (federation + inbox + admin parity)
    - HEP-CORE-0035 §4.6 (key-file ACL discipline), §4.7 (runtime
      key handling, deferred to task #102), §4.8 (vault + CLI +
      bootstrap including empty-known_roles semantics)
    - HEP-CORE-0017 §3.3 (queue auth contract)

Sibling subtopic TODO **`docs/todo/AUTH_TODO.md`** created to track
Phase D implementation steps + Phase E–H deferred items + parallel
tracks (#102, #103, #104, #105, #106, #120) — so open items survive
context resets without an active tech_draft.

### 2026-05-26 (M9 refactor closure)

Wave-B M9 (`RoleHostFrame` role-host unification) shipped end-to-end
(commits `bc3f9340` through `53cf11be`).  Tasks #72 + #100 closed.
One transient checklist moved to `docs/archive/transient-2026-05-26/`:

* **`docs/todo/M9_REFACTOR_CHECKLIST.md`** — Phase-by-phase tracker
  used during the multi-session refactor.  Phase 1 (Presence-driven
  setup with shadow), Phase 2 (retire legacy `RoleHostCore` fz
  storage; `FlexzoneInfoCache` becomes single source), Phase 3 (L2
  Q2 + Q3 test rework) all complete.  Lasting architectural facts
  merged into:
    - `docs/TODO_MASTER.md` Arc B status row updated to `✅ M0..M9 shipped`.
    - `docs/tech_draft/role_host_template_design.md` remains the
      canonical design reference.

### 2026-05-21 (demo-harness session — audit + unified plan merge)

Two transient docs from the multi-day demo-harness session merged
into canonical docs and moved to
`docs/archive/transient-2026-05-21/`:

* **`docs/tech_draft/DEMO_DOC_AUDIT_2026-05-20.md`** — 17 deployment-
  doc gaps (G1-G17 + G21) and 13 demo-surfaced bugs (B1-B13).  All
  G-items absorbed into doc updates this session (HEP-CORE-0011 §
  "Init Protocol" + §"API availability per callback";
  HEP-CORE-0019 §5.4.1-5.4.3 metrics tables; `README_Deployment.md`
  §4.2 / §4.3 / §5.1 / §6.1 / §7.1 / §8.3 / §8.5).  B-items: B1, B2,
  B5, B9, B11, B12, B13 ✅ FIXED in code (commit `2be5156` through
  `f6079ec8`); B3, B4, B6, B7, B8, B10 filed in `docs/todo/`
  subtopic TODOs for follow-up (see below).

* **`docs/tech_draft/UNIFIED_PLAN_2026-05-21.md`** (rev 2) —
  consolidated planning artifact.  Pre-existing TODO_MASTER items
  are unchanged; this session's new findings (N1-N11) filed in
  subtopic TODOs; the executive summary + critical-path narrative
  + rev-2 demos+benches inventory merged into `TODO_MASTER.md` §
  "Current Sprint Focus" closing paragraphs.

Subtopic-TODO updates this session:

  - **`docs/todo/API_TODO.md`** — new section "Session 2026-05-21 —
    demo harness audit closure" with B4 (init template SHM secret),
    B6+B7 (rx.fz binding + flexzone side doc), B10 (band_join in
    on_init), N2 (NativeEngine HubAPI surface extension), N3+N4
    (native plugin sig + lifecycle module cleanup), N5 (operator
    guide), N7+N10 (three-engine doc parity).
  - **`docs/todo/TESTING_TODO.md`** — new section "Session 2026-05-21
    — demo harness audit closure" with N1 (L3 setup_infrastructure_
    translation tests — HIGH priority), N8+N9 (bench variants), N11
    (cross-engine on_band_message signature regression), B8
    (`plh_pyenv install --requirements` in demo setup).
  - **`docs/todo/MESSAGEHUB_TODO.md`** — B3 (hard-error
    `hub.auth.keyfile=""`).
  - **`docs/todo/PLATFORM_TODO.md`** — N6
    (`cmake/pylabhubNativePlugin.cmake` user-oriented helper).

Demo framework delivery — `share/demo_framework/runner.py` + 9
demo manifests under `share/py-demo-*/` — closes harness Task #44
(L4 processor + consumer test infrastructure).

### 2026-05-19 (R3.5b wire-field unification migration plan)

`docs/tech_draft/R3.5b_wire_field_unification_2026-05-19.md` recorded
the broker_proto 4→5 migration plan + checklist: wire-field
unification (`consumer_uid`/`uid`/`sender_uid` → `role_uid` in
role-context messages), unconditional grammar check at every gate,
side-aware role-tag policy.  The work shipped 2026-05-19 with
1951/1951 tests green.

Section-by-section absorption:

  - **§"Why" + §"Wire format — proto 5"** → **HEP-CORE-0023 §2.5.4**
    "Wire-field naming + grammar enforcement (audit R3.5b)".  The
    wire-format table and rationale (default Open policy admitting
    empty uid; downstream silent no-op in `_on_consumer_joined`)
    are now there.
  - **§"Validator helper signature" + §"Migration steps"** →
    **HEP-CORE-0033 §G2.2.0b.8** "Enforcement points" extended with
    the gate-by-gate tag-set table + reference to
    `validate_identity_fields` / `validate_role_uid_only` in
    `broker_service.cpp`.  Grammar enforcement at the gate is
    now documented as unconditional (policy verification stacks
    on top).
  - **`BAND_BROADCAST_REQ.sender_uid` → `role_uid`** rename →
    **HEP-CORE-0030 §5.1 + §5.3** (`BAND_BROADCAST_REQ` and
    `BAND_BROADCAST_NOTIFY` payload fields updated, script-side
    code sample updated).
  - **§"Test plan"** → realised as 6 new L3 mutation-tests in
    `tests/test_layer3_datahub/test_datahub_broker.cpp`
    (`Gate_RegReq_*` + `Gate_ConsumerRegReq_*`).  No HEP absorption.
  - **§"Migration steps (sequence)"** → operational checklist for
    the one-shot patch; not a durable contract.  Archived in
    `transient-2026-05-19/`.

The closure record (what shipped + test count) lives in
`docs/todo/API_TODO.md` § "R3.5b closed (2026-05-19)".

Archived to `docs/archive/transient-2026-05-19/R3.5b_wire_field_unification_2026-05-19.md`.

### 2026-05-05 (Phase 8c script-response draft superseded by HEP-0033 §12.2/§12.4.1/§12.5)

`docs/tech_draft/HEP_0033_PHASE_8C_SCRIPT_RESPONSE.md` (dated 2026-05-04,
"Draft, not yet ratified") proposed a script-mediated request/response
mechanism for hub augmentation hooks.  During implementation the design
was revised — the draft's larger abstraction surface (`IConsultationHost`,
`ConsultationRequest`/`ConsultationReply` structs, `encode_request_id`
subsystem encoding, park-and-drain reply queues, `api.respond` script
method) was rejected as over-engineered.  The shipped design (HEP-0033
§12.2.2) reuses the engine's existing cross-thread machinery with one
new virtual `invoke_returning(name, args, timeout_ms)` and four
`HubAPI::augment_*` methods — no new abstraction classes.

Section-by-section absorption:

  - **§1–2** Motivation + architecture overview → **HEP-CORE-0033 §12.2**
    (Callbacks: design principle + 12.2.1 event observers + 12.2.2
    response augmentation hooks).  Rewritten to drop the "veto" framing
    (the user clarified the model is purely additive — bookkeeping
    always completes; scripts decorate the response).
  - **§3** Common abstraction (`script_consultation.hpp`,
    `IConsultationHost`, `ConsultationRequest`, `ConsultationReply`,
    `encode_request_id`) → **rejected**.  The shipped design uses a
    `timeout_ms` parameter on `invoke_returning` and the existing
    engine pending-queue + `std::future` machinery.
  - **§4–5** AdminService + BrokerService changes (per-method handler
    map, parked replies, drain in recv loop) → **simplified** to
    direct `host.hub_api()->augment_<rpc>(params, response)` calls
    inline in the existing handlers.  AdminService keeps its existing
    REP-socket synchronous shape; broker-side wiring deferred per
    HEP-0033 §12.2.2 table (needs `HUB_TARGETED_ACK` wire frame —
    HEP §13 follow-up).
  - **§6** HubAPI surface → absorbed into **HEP-CORE-0033 §12.3** with
    the four `augment_*` methods + `post_event` (the latter from §12.2.3
    user-posted events) + `augment_timeout_ms` / `set_augment_timeout`
    runtime knob (HEP §12.2.2 timeout block).
  - **§7** Script-side examples → absorbed into **HEP-CORE-0033
    §12.2.2** (callback signatures table) and **README_Deployment.md
    §4.4** (Hub script guide).
  - **§8** `send_timeout_heartbeats` socket hardening → **deferred**
    (separate slice; HEP §13 follow-up).
  - **§9** Threading model walk → absorbed into **HEP-CORE-0033 §12.4**
    (worker main-loop drain + augmentation request transport) + new
    **§12.4.1 Engine-specific threading models** with full Mermaid
    treatment of PythonEngine vs LuaEngine differences.
  - **§10** Implementation phase plan → reflected in **HEP-CORE-0033
    §15 Phase 8c entry**.

Files moved to `archive/transient-2026-05-05/`:

  - `HEP_0033_PHASE_8C_SCRIPT_RESPONSE.md`

The implementation landed in commit `3c65dfa` ("HEP-0033 Phase 8c:
response augmentation hooks + post_event + shutdown drain").

### 2026-05-04 (Engine thread model draft superseded by HEP-0011 + HEP-0028)

Static review of the post-Phase-7-closure tree (commit `b4c00c3`)
flagged `docs/tech_draft/engine_thread_model.md` (~1579 lines, dated
2026-03-20) for archival review.  Section-by-section audit confirmed
the draft's content was fully absorbed into canonical docs:

  - **§1–10** (ScriptEngine interface, queue/mutex execution model,
    LuaEngine thread states, PythonEngine queue, GIL strategy,
    lifecycle, shared resources, runtime cost, integration points,
    implementation phases) → **HEP-CORE-0011 ScriptHost Abstraction
    Framework** (1085 lines normative spec).
  - **§11** (NativeEngine — Dynamic C++ Library Extension: motivation,
    interface mapping, symbol convention, threading, zero-copy data
    access, schema validation, configuration, runtime cost, API
    header, comparison) → **HEP-CORE-0028 Native Plugin Engine**
    (722 lines normative spec) + `docs/README/README_NativePlugin.md`
    (829 lines developer guide) + `src/include/utils/native_engine_api.h`
    (the C ABI header itself).

Coexisting policy update: the related `dev_mode` admin config flag
was removed entirely on 2026-05-04 — see HEP-CORE-0033 §11.3 + §17.1.
Tests follow production-required-token semantics; the
`token_required=false` capability is preserved for the legitimate
single-host loopback-only operator scenario but is no longer
exercised in the test suite via a dev-mode bypass path.

| Archived | From | Reason |
|---|---|---|
| `engine_thread_model.md` | `docs/tech_draft/` | Fully merged into HEP-0011 + HEP-0028 + README_NativePlugin.md.  Preserved for historical reference (incl. §8 runtime-cost-analysis numbers not re-baselined into canonical docs). |

Merge map: `docs/archive/transient-2026-05-04/README.md` —
section-by-section pointers to the canonical destination of each
chunk of the draft's content.

---

### 2026-05-02 (HEP-0033 Phase 6.2 closed; AdminService review archived)

Phase 6.2 (AdminService structured RPC) shipped in three sub-commits:
`db9f8f9` (6.2a skeleton), `c0408a8` (6.2b queries),
`38591dc` (6.2c control methods).  All four §4.1 action items in the
pre-implementation review (`REVIEW_AdminService_2026-05-01.md`) are
✅ FIXED:

  - **A1** vault→HubHost wiring → option (a) shipped: `admin_token` lives
    on `cfg.admin().admin_token`, populated at vault unlock.
  - **A2** file path → `src/utils/ipc/admin_service.cpp` (matches
    `broker_service.cpp` placement; HEP §11.4 updated in 6.2a).
  - **A3** hub `init_directory` template → `HubAdminConfig` block
    fully populated.
  - **A4–A6** Phase 6.2a/b/c implementation per the recommended split.
  - **A7** HEP §11.2 deferred-method citations → done.
  - **A8** MESSAGEHUB_TODO refresh → folded into TODO_MASTER snapshot.

§16 item 10 (admin RPC error-code catalog) closed via new HEP-0033
§11.5 (commit `dd5ac0d`).  10 of 16 §11.2 methods wired; the 6
deferred methods carry explicit upstream-HEP citations
(HEP-0035 / §16 #1 / §16 #9 / Phase 7).

| Archived | From | Reason |
|---|---|---|
| `REVIEW_AdminService_2026-05-01.md` | `docs/code_review/` | All §4.1 action items resolved; Phase 6.2 closed. |

---

### 2026-05-02 (Test-correctness audit closed)

User-driven 2-day audit triggered by two silent-failure regressions
(slow-path `EXPECT_THROW`, envelope-only `status==ok`).  Final state:
all 204 inventory rows ✅ FIXED or n/a across all four bug classes
(A: outcome-only assertions; B: timing-by-sleep ordering; C: discarded
timeout returns; D: missing log-noise gate).  Suite: 1689/1689 green.

Lasting policies were merged into `docs/IMPLEMENTATION_GUIDANCE.md`
§ "Assertion Design — silent-failure prevention" and `CLAUDE.md`
§ "Testing Practice (Mandatory)" earlier in the audit; both are
now durable.  Subsequent regressions in any of the four bug classes
should be opened as fresh REVIEW files, not appended to the closed
audit.

Class D framework gate finding documented (the auditing-side win):
`expect_worker_ok` (`tests/test_framework/test_process_utils.cpp:632-643`)
already scans every Pattern-3 worker stderr for `[ERROR ]` lines —
~650 subprocess tests get the gate for free.  In-process fixtures
that touch log-emitting production code use `LogCaptureFixture`
(`tests/test_framework/log_capture_fixture.h`).

| Archived | From | Reason |
|---|---|---|
| `REVIEW_TestAudit_2026-05-01.md` | `docs/code_review/` | Audit closed; all acceptance criteria met. |
| `REVIEW_TestAudit_2026-05-01_inventory.md` | `docs/code_review/` | Companion inventory; archived alongside master plan. |

---

### 2026-04-30 (HEP-CORE-0033 prereqs doc fully superseded by HEP)

`HUB_CHARACTER_PREREQUISITES.md` archived.  All 13 gaps it tracked are
resolved: 9 have shipped (Phase 1, 3, 4, 5 of HEP-0033 §15) and 4 open
items moved into HEP-CORE-0033 itself (§15 Phase 9 note for L4 test
infrastructure; §16 items 9 + 10 for `reload_config` whitelist + admin
RPC error catalog; §16 item 2 already covered for tick cadence).
The doc's §G2 "broker-side `authorize_<op>()` with AuthContext"
sub-design was rejected — HEP-CORE-0033 §11.3 + §12.3 pin
authorization at the calling module's boundary (AdminService validates
token, HubAPI/HubHost gates script access; broker is a state-accessor
with no per-call acceptance logic).  Doc moved to
`docs/archive/transient-2026-04-30/` with a banner directing readers
to HEP-CORE-0033 as the single source of truth.

| Archived | From | Reason |
|---|---|---|
| `HUB_CHARACTER_PREREQUISITES.md` | `docs/tech_draft/` | All gaps resolved or moved into HEP-CORE-0033; §G2 broker-side-auth model rejected per HEP §11.3/§12.3. |

---

### 2026-04-05 (RoleAPIBase refactoring + lifecycle integration complete)

Five tech drafts archived — all fully implemented and verified against code.
Lasting insights merged into HEP-CORE-0011 (rewritten 2026-04-04).

| Archived | Reason |
|---|---|
| `role_context_simplification.md` | RoleContext eliminated; RoleAPIBase is sole context. Commit 480bfb1. |
| `role_api_base_design.md` | All 6 phases complete; unified API, ChannelSide, schema sizes. |
| `script_engine_lifecycle_module.md` | All 3 role hosts use engine_lifecycle_startup. |
| `lifecycle_dynamic_module_extensions.md` | Userdata support in ModuleDef fully implemented. |
| `script_engine_refactor.md` | Superseded by HEP-CORE-0011 (2026-04-04 rewrite). |

---

### 2026-03-20 (ScriptEngine refactor review — cleanup)

Two review files archived. All items resolved except PARITY-01 (Lua API parity), which carries forward into the new active review `REVIEW_ScriptEngine_2026-03-20.md`.

| Archived | Origin | Reason |
|---|---|---|
| `REVIEW_FullStack_2026-03-17.md` | `docs/code_review/` | All 30+ items FIXED/ACCEPTED/DEFERRED; one OPEN (PARITY-01) carried forward |
| `cursor_code_and_document_review_report.md` | `docs/code_review/` | Cursor IDE export; informational analysis; actionable items already captured in other reviews |

**Retained (deferred):** `LINT_FIXES_PLAN.md` — Section 2 (7 categories) still awaiting user confirmation.

---

### 2026-03-15 (codex deep review — doc fixes + TODO routing)

External codex review (2-pass, read-only). 14 findings triaged:
- 4 doc inaccuracies fixed directly (--init flow, self-contained claim, HEP-0024 API sig, HEP-0018 CLI flags)
- 4 code/design items routed to API_TODO (Lua gap, throwing destructors, config duplication, remap placeholders)
- 2 items routed to PLATFORM_TODO + TESTING_TODO (CI platform claims, stale test docs)
- 1 fixed previously (test count 884→1166)
- 1 false positive (g_hub_config_initialized — actually g_hub_config_state, properly used)
- 2 duplicates of #1 (Lua finding repeated in pass 2)

**Archived to `docs/archive/transient-2026-03-15/`:**

| Archived | Origin | Reason |
|---|---|---|
| `REVIEW_Codex_2026-03-15.md` | `docs/code_review/codex_review.md` | External codex review — all findings triaged; doc fixes applied; code items routed to subtopic TODOs |

---

### 2026-03-14 (code review closure + config review cleanup)

Two code reviews closed and archived. All findings either fixed (8 items) or accepted (4 items).
1166/1166 tests pass. Also archived stale root TODO.

**Archived to `docs/archive/transient-2026-03-14/`:**

| Archived | Origin | Reason |
|---|---|---|
| `REVIEW_CodeAndDocs_2026-03-14.md` | `docs/code_review/` | ✅ CLOSED — 12 findings triaged: CX-01 (HIGH, get_binary_dir platform macros), CX-02 (mutex CLOCK_REALTIME fix), CX-04 (doc timeout_ms rename), CX-09 (dead result_repr), CX-10 (unused config_filename param), CX-11 (stale dir layout), RC-01 (docstring alignment). 4 accepted. |
| `high_level_codex_review.md` | `docs/code_review/` | External codex review — all 5 findings triaged and addressed in REVIEW_CodeAndDocs_2026-03-14.md above |
| `TODO_root_orphaned.md` | `TODO.md` (root) | Severely stale; referenced Layer 2 migration from months ago; conflicts with canonical TODO in docs/ |

---

### 2026-03-13 (repo restructure — promote cpp/ to root)

Original Python prototypes and design docs archived as part of the repo restructure
that promoted `cpp/` contents to the repository root.

**Archived to `docs/archive/transient-2025-09-27/`:**

| Archived | Origin | Key content |
|---|---|---|
| `hub_design_original.md` | `docs/hub/design.md` (top-level) | Original Python Hub design: 3-channel message bus, adapter/service protocols |
| `hub_design2_original.md` | `docs/hub/design2.md` (top-level) | Extended Hub design doc with implementation details |
| `README_original_vision.md` | `README.md` (top-level) | Original project vision: target use cases, data persistence strategy (Zarr/Parquet/HDF5), run manifest schema, NSF/NIH-aligned features, roadmap |

**Preserved to `docs/tech_draft/future-persistence-and-discovery/`:**

| File | Origin | Purpose |
|---|---|---|
| `api.py` | `pylabhub/hub/api.py` | Async Python Hub skeleton (design reference) |
| `persistence_service.py` | `pylabhub/hub/persistence_service.py` | Zarr+Parquet writer service |
| `manifest.py` | `pylabhub/hub/manifest.py` | Run manifest tooling (SHA-256, validation) |
| `example_persistence_service.py` | `pylabhub/hub/example/` | Demo wiring |

### 2026-03-12 (code review clearance — 8 closed reviews)

All code reviews that reached ✅ CLOSED status archived. Gemini review triaged (never previously used).

**Archived to `docs/archive/transient-2026-03-12/`:**

| Archived | Status | Key content |
|---|---|---|
| `REVIEW_FullSource_2026-03-06.md` | CLOSED | 47 items: 28 fixed, 10 FP, 9 accepted |
| `REVIEW_DesignAndCode_2026-03-09.md` | CLOSED | DC-01 (METRICS_REQ SHM merge); DC-04/06 deferred |
| `REVIEW_FullStack_2026-03-10.md` | CLOSED | FS-01/MR-05/MR-10 FP; FS-02 fixed |
| `REVIEW_FullStack2_2026-03-10.md` | CLOSED | A1/A5/A6/A11/A12/A18/A20 fixed |
| `REVIEW_Processor_2026-03-10.md` | CLOSED | All 20 items fixed or false positive |
| `REVIEW_DeepStack_2026-03-10.md` | CLOSED | 16 findings; 13 fixed, 3 deferred |
| `review_high_level.md` | CLOSED | 9 findings (HIGH×3, MEDIUM×3, LOW×3); all resolved |
| `gemini_review.md` | TRIAGED | 9 findings; 5 stale/FP, 2 fixed, 1 accepted, 1 open (flexible_zone_size→API_TODO) |
| `gemini_review_triage_2026-03-12.md` | (new) | Triage notes for gemini_review.md |

Also completed this session:
- MR-01: Wire format deduplication — `zmq_wire_helpers.hpp` shared by `hub_zmq_queue.cpp` + `hub_inbox_queue.cpp`
- MR-09: `ShmQueue::is_running()` override — returns false on moved-from (null pImpl) instance
- LOW-2: `[[deprecated]]` added to 4 DataBlock remap stubs in `data_block.hpp`
- 3 new tests: ShmQueueIsRunning, DataBlockProducerRemapStubsThrow, DataBlockConsumerRemapStubsThrow → 1120/1120

---

### 2026-03-10 (tech_draft merge + archive — all 4 working docs)

All four tech_draft working documents finalized and merged into HEP docs:

| Source | Merged into | Key content |
|--------|-------------|-------------|
| `loop_design_producer.md` | HEP-CORE-0018 §0/§5/§6/§7 | Thread model, inbox design, timing policies, config reference |
| `loop_design_consumer.md` | HEP-CORE-0018 §0/§5/§6/§7 | QueueReader abstraction, slot access, spinlock, verify_checksum |
| `loop_design_hub.md` | HEP-CORE-0017 + HEP-CORE-0022 | Hub thread model, arbitration, federation |
| `zmq_queue_design.md` | HEP-CORE-0021 §13 | Wire format, send_thread_, InboxQueue, metrics |

HEP-CORE-0018 updated: §0 implementation timeline complete; §5.4 field table cleaned; §6.3 API
all "[PLANNED]" markers removed; §7 thread model current; §9.5 ConsumerConfig struct updated.
HEP-CORE-0021 updated: §4.1 interface updated to QueueReader/QueueWriter split; §7.3 ProcessorScriptHost
updated to use queue_reader()/queue_writer(); §13 inlined with ZmqQueue internals summary.

**Archived to `docs/archive/transient-2026-03-10/` (Batch 3):**

| Archived | Reason |
|---|---|
| `loop_design_producer.md` | All items done (996/996); content merged into HEP-0018 |
| `loop_design_consumer.md` | All items done (996/996); content merged into HEP-0018 |
| `loop_design_hub.md` | All items done; content in HEP-0017/HEP-0022 |
| `zmq_queue_design.md` | All items done; content merged into HEP-0021 §13 |

---

### 2026-03-10 (Informal review precursor archived)

`review_design_and_code.md` (informal P1/P2 findings, 2026-03-09) archived — all 6 findings promoted to
formal `REVIEW_DesignAndCode_2026-03-09.md` with status table. All actionable items now closed.

**Archived to `docs/archive/transient-2026-03-10/`:**

| Archived | Reason |
|---|---|
| `review_design_and_code.md` | Superseded by formal REVIEW_DesignAndCode_2026-03-09.md; all items triaged |

---

### 2026-03-10 (Design Review Triage + queue_refactor_plan archive)

Formal review REVIEW_DesignAndCode_2026-03-09.md created from informal review_design_and_code.md triage.
DC-01 (METRICS_REQ SHM merge) fixed: `handle_metrics_req` now calls `query_shm_blocks()` and merges
`shm_blocks` key into response (HEP-0019 §3.2 compliance). DC-02/DC-03/DC-05 confirmed already fixed.
DC-04/DC-06 deferred.

queue_refactor_plan.md archived — all 9 phases executed (QueueReader/QueueWriter split complete).
Lasting design decisions already merged into loop_design_consumer.md + hub_queue.hpp docstrings.

**Archived to `docs/archive/transient-2026-03-09/` (Batch 2):**

| Archived | Reason |
|---|---|
| `queue_refactor_plan.md` | All phases complete (975/975 tests); design in loop_design_consumer.md |

---

### 2026-03-09 (DataHub Inbox Code Review)

Code review `REVIEW_DataHubInbox_2026-03-09.md` closed after fixing all 13 actionable items:
CR-02 (inbox thread join order), CR-03 (ShmQueue checksum ordering), HR-01 (atomic script_errors_),
HR-02 (atomic reader_), HR-03 (ZMQ_RCVTIMEO caching), HR-05 (GIL release in open_inbox), HR-06,
MR-05, MR-08, MR-10 (send_stop_ guard), LR-04 (memory_order_release), LR-05 (error counting),
IC-04 (last_seq docstring). MR-04 confirmed false positive. Test count: 975/975.

Deferred items (accepted by design): MR-01 (dedup), MR-02 (per-sender gap), MR-07 (join-order safe), LR-01, MR-06, MR-09.

**Archived to `docs/archive/transient-2026-03-09/`:**

| Archived | Reason |
|---|---|
| `REVIEW_DataHubInbox_2026-03-09.md` | All actionable items fixed; CLOSED |

---

### 2026-03-06 Batch 2 (Closed Reviews + Deferred Design Docs)

Archived all remaining open tech_draft/ review documents after verifying all items
fixed or deferred. ZmqQueue+Broadcast review had 22 items (all fixed; PC4 deferred
to HEP-0023). ZmqVirtualChannel+Federation review had 6 items (all fixed). Memory
layout redesign (single flex zone + re-mapping) remains a deferred future design.
Deferred security items tracked in `docs/TODO_MASTER.md`. Test count: 882 (881 pass; 1 flake).

**Archived to `docs/archive/transient-2026-03-06/` (Batch 2):**

| Archived | Reason |
|---|---|
| `REVIEW_codebase_2026-03-06.md` | Consolidated review CLOSED; deferred items in TODO_MASTER.md |
| `REVIEW-ZmqQueue-Broadcast-2026-03-06.md` | All 22 items fixed; PC4 deferred to HEP-0023 |
| `REVIEW-ZmqVirtualChannel-Federation-2026-03-06.md` | All 6 items fixed; CLOSED |
| `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` | Deferred design (not actively in progress); find in archive when ready to implement |

---

### 2026-03-06 Batch 1 (Old Code Review Triage + Security Fixes)

Triaged three code review documents from 2026-03-03 against current source (882 tests).
Fixes applied: SHM-C1 (heartbeat CAS corruption), IPC-C3 (thread lambda this-capture),
SVC-C1/C2/C3 (key material not zeroed), HDR-C1 (namespace outside __cplusplus).

**Consolidated source-of-truth review**: `docs/archive/transient-2026-03-06/REVIEW_codebase_2026-03-06.md` (archived Batch 2)

**Archived to `docs/archive/transient-2026-03-06/` (Batch 1):**

| Archived | Reason |
|---|---|
| `REVIEW_full-codebase_2026-03-03.md` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303_detailed.md` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303.md` | Superseded by consolidated 2026-03-06 review |

---

### 2026-03-03 (HEP-0005 Archived + Actor Terminology Scrub)

Session 0 housekeeping for the comprehensive HEP document review plan. Two activities:

1. **HEP-CORE-0005 archived** — Script Interface Abstraction Framework superseded by
   HEP-CORE-0011 (ScriptHost Abstraction Framework with RoleHostCore + PythonRoleHostBase).
2. **Actor terminology scrub** — ~185 "actor" references replaced across 11 HEP files with
   current standalone binary terminology. The `pylabhub-actor` multi-role container was
   eliminated 2026-03-01; this pass ensures all HEP documents reflect the current architecture.

**Archived to `docs/archive/transient-2026-03-03/`:**

| Archived | Reason |
|---|---|
| `HEP-CORE-0005-script-interface-framework.md` | Superseded by HEP-CORE-0011; abstract `IScriptEngine`/`IScriptContext` replaced by `RoleHostCore` + `PythonRoleHostBase` |

**HEP files updated (actor scrub):**

| Document | Key changes |
|---|---|
| HEP-CORE-0002 | Identity fields (`producer_uid`), API layers, connection policy, embedded-mode refs |
| HEP-CORE-0006 | Implementation note rewritten for standalone binaries |
| HEP-CORE-0008 | Area, config, metrics domain, file references — all actor → binary/script host |
| HEP-CORE-0009 | Policy interaction diagram, default stack, config headers — actor → per-binary |
| HEP-CORE-0011 | Abstract, motivation, threading references |
| HEP-CORE-0013 | UID format (PROD-/CONS-/PROC-), provenance chain, vault section, code snippets |
| HEP-CORE-0015 | Area, identity, API, GIL, vault note — actor comparisons removed |
| HEP-CORE-0016 | Area, depends-on, motivation, config references |
| HEP-CORE-0017 | Processor role worker → standalone binary |
| HEP-CORE-0018 | ActorVault legacy note clarified |

**DOC_STRUCTURE.md updated:** HEP index expanded to 0001–0020; statuses refreshed for
0011, 0015, 0016, 0018, 0019, 0020.

**Remaining actor references (intentional):**
- HEP-0017 "Updated" field (historical note)
- HEP-0018 §1 Motivation (explains *why* actor was eliminated)
- HEP-0018 Supersedes table (historical cross-reference)

---

### 2026-03-02 (Completed TODO files — RAII, Memory Layout, Security)

Routine quarterly-style cleanup: three subtopic TODO files whose tracked work is fully complete
have been archived. Surviving open backlog items absorbed into remaining active TODOs.

**Archived to `docs/archive/transient-2026-03-02/`:**

| Archived | Reason | Surviving items relocated |
|---|---|---|
| `SECURITY_TODO.md` | All 6 security phases complete (2026-02-28) | None — all done |
| `RAII_LAYER_TODO.md` | RAII layer fully implemented; 3 minor backlog items survive | FlexZone example + move audit + zero-cost check → `TESTING_TODO.md` low priority |
| `MEMORY_LAYOUT_TODO.md` | Memory layout complete; layout checksum tests + stub doc | Layout tests → `TESTING_TODO.md`; stub doc → `API_TODO.md` backlog |

**Active TODO files after cleanup:** `API_TODO.md`, `TESTING_TODO.md`, `MESSAGEHUB_TODO.md`, `PLATFORM_TODO.md`

---

### 2026-03-01b (Actor Elimination — Design Revision)

Architectural decision: eliminate `pylabhub-actor` (multi-role container) in favour of
standalone `pylabhub-producer` and `pylabhub-consumer` binaries, each owning their own
directory, identity, vault, and PID lock — consistent with the existing `pylabhub-processor`
standalone model. This removes multi-broker identity ambiguity and multi-machine deployment
confusion inherent in the actor container design.

**Archived to `docs/archive/design-revision-2026-03-01/`:**

| Archived | Reason |
|---|---|
| `HEP-CORE-0010-Actor-Thread-Model-and-Unified-Script-Interface.md` | Actor eliminated. Thread model lives in HEP-CORE-0018 §7 |
| `HEP-CORE-0012-Processor-Role.md` | ProcessorRole-inside-actor eliminated. Standalone: HEP-CORE-0015 |
| `HEP-CORE-0014-Actor-Framework-Design.md` | Actor framework eliminated. Superseded by HEP-CORE-0018 |
| `REVISION_SUMMARY.md` | AI-generated transient session summary; no canonical content |

**New canonical documents:**

| Document | Content |
|---|---|
| `HEP-CORE-0018-Producer-Consumer-Binaries.md` | Full spec for `pylabhub-producer` and `pylabhub-consumer` |

**Updated canonical documents:**

| Document | Change |
|---|---|
| `HEP-CORE-0011` | Library structure, config examples, directory layouts updated for all four components; actor section removed |
| `HEP-CORE-0015` | Status updated to Phase 1 implemented; script path fixed (`script/python/__init__.py`); actor comparison removed; §1 motivation updated |
| `HEP-CORE-0017` | §6.1 binary table replaced (actor → producer + consumer); §6.2 config hierarchy updated for all four; §6.3 rewritten; cross-ref index updated |
| `DOC_STRUCTURE.md` | HEP index updated with archived/new/updated status |

See **`docs/archive/design-revision-2026-03-01/README.md`** for design decision record.

---

### 2026-03-01 (Code Review Round 2 — Complete)

Code Review Round 2 resolved. 7 confirmed bugs fixed; 11 items classified as false positives.
`ExponentialBackoff` renamed to `ThreePhaseBackoff` (Phase 3 is linear); HEP-0003/0012 doc fixes applied.

**Archived to `docs/archive/transient-2026-03-01/`:**

| Archived | Reason |
|---|---|
| `code_review/CODE_REVIEW_2026-03-01_hub-python-actor-headers.md` | All items resolved: HP-C1 HP-C2 AF-H2 ✅ FIXED; PH-C1 PH-C2 PH-C3 AF-H1 PH-H5 ❌ FALSE POSITIVE |
| `code_review/CODE_REVIEW_2026-03-01_utils-actor-hep.md` | All items resolved: NC3 NC4 NH1 NH2 ✅ FIXED; NC1 NC2 NH4 NH5 NM11 NM12 NM13 ❌ FALSE POSITIVE |

**Active review table in `TODO_MASTER.md` cleared.**

See **`docs/archive/transient-2026-03-01/README.md`** for full item disposition and fix summary.

---

### 2026-02-28 (Actor Framework HEP Promotion)

Promoted `docs/tech_draft/ACTOR_DESIGN.md` to canonical HEP status. Created
**HEP-CORE-0014** (Actor Framework Design) as the authoritative developer-facing spec
for the actor framework API. Trimmed HEP-CORE-0010 §4 and §8 to cross-references.

**Archived to `docs/archive/transient-2026-02-28/`:**

| Archived | Reason |
|---|---|
| `tech_draft/ACTOR_DESIGN.md` | Promoted to HEP-CORE-0014; §11 Gap Analysis dropped (stale) |

**New HEP:** `docs/HEP/HEP-CORE-0014-Actor-Framework-Design.md` — covers config format,
Python script interface, C++ class architecture, auth/security, schema validation, and
failure model. HEP-CORE-0010 retains threading internals; §4 and §8 now cross-reference 0014.

See **`docs/archive/transient-2026-02-28/README.md`** for full merge map.

---

### 2026-02-27 (Code Review Archive — post P1–P8 restructure + security phases)

End-of-sprint cleanup after `src/utils/` subdirectory restructure (P1–P8 source splits)
and HEP-CORE-0002 restructuring. Both active code reviews confirmed complete.

**Archived to `docs/archive/transient-2026-02-27/`:**

| Archived | Reason |
|---|---|
| `REVIEW_2026-02-26_data-hub-branch.md` | All P0/P1/P2/P3 items ✅ FIXED or ⚠️ DEFERRED (documented); 550/550 tests |
| `CODE_REVIEW.md` | All C/H items ✅ FALSE POSITIVE; M items ⚠️ DEFERRED in subtopic TODOs; no ❌ OPEN items |

**Active review table in `TODO_MASTER.md` cleared.**

See **`docs/archive/transient-2026-02-27/README.md`** for full item disposition.

---

### 2026-02-21 (Doc Consolidation + HEP Consistency Session)

Audited and consolidated todo files, code review docs, and HEP consistency.

**Archived to `docs/archive/transient-2026-02-21/`:**

| Archived | Reason |
|---|---|
| `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` | Phase 3 RAII complete; content in IMPL_GUIDANCE + HEP-0007 |
| `DATAHUB_TODO.md` | Legacy monolithic TODO; superseded by `docs/todo/` subtopic system |
| `DESIGN_VERIFICATION_RULE.md` | Content inlined into `CODE_REVIEW_GUIDANCE.md` §2 |
| `tech_draft/BROKER_DATABLOCK_INTEGRATION.md` | Stable; content in HEP-CORE-0002 §6 |
| `tech_draft/CHANNEL_EXPANSION_DESIGN.md` | Implemented; design in HEP-CORE-0002 §6.2 |
| `DATAHUB_NAMING_CONVENTIONS.md` | Outdated (old "Source/Terminal" roles); new conventions in `uid_utils.hpp` + ACTOR_DESIGN.md |
| `code_review/code_review_utils_2025-02-21.md` | Early untracked review; open items migrated to subtopic TODOs |
| `code_review/CPP_CODE_REVIEW.md` | 2026-02-20 review (renamed); open items migrated to subtopic TODOs |

**Promoted:**
- `docs/DATAHUB_PROTOCOL_AND_POLICY.md` → `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`
  (full HEP header added; Mermaid diagrams added for state machine, protocol flows, heartbeat, DRAINING)

**Restored to `docs/tech_draft/` (prematurely archived, design not implemented):**
- `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (structure re-mapping APIs throw at runtime)

**Key doc updates:**
- `HEP-CORE-0002`: fixed incorrect state machine transitions (COMMITTED→FREE, DRAINING→FREE both wrong); added HEP-0007 cross-references
- `HEP-CORE-0006`: corrected `send_ctrl` return type `void` → `bool`
- `CODE_REVIEW_GUIDANCE.md`: restructured — principles + pitfall reference table; removed technical detail duplication
- `MESSAGEHUB_TODO.md`: compacted 489→~150 lines; added 6 open code review items
- `API_TODO.md`, `RAII_LAYER_TODO.md`, `MEMORY_LAYOUT_TODO.md`: open code review items integrated

See **`docs/archive/transient-2026-02-21/README.md`** for full merge map and open-item disposition.

---

### 2026-02-17 (Code Review Archived — REVIEW_utils_2026-02-15.md)

All 11 items in `REVIEW_utils_2026-02-15.md` are ✅ FIXED (last items resolved 2026-02-17).
Review moved to `docs/archive/transient-2026-02-17/`. Active review table in `TODO_MASTER.md` cleared.

---

### 2026-02-17 (docs/ Root Cleanup — 29 non-core documents)

Audited all .md files directly under `docs/` root. Identified 29 non-core documents
(design notes, audit reports, session summaries, test refactoring plans, API analyses).
Content verified against codebase; key governance rules and open items merged into core docs;
all 29 archived.

**Key merges:**
- `C_API_TEST_POLICY.md` → `IMPLEMENTATION_GUIDANCE.md` § "C API Test Preservation" + `CLAUDE.md`
- `CORE_STRUCTURE_CHANGE_PROTOCOL.md` → `IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
- `TEST_REFACTOR_TODO.md` + test audit docs → `TESTING_TODO.md` (coverage gaps + completions)
- `API_ISSUE_NO_CONFIG_OVERLOAD.md` → `API_TODO.md` (verified resolved)
- Transient document rule → `IMPLEMENTATION_GUIDANCE.md` § Session Hygiene + `CLAUDE.md`

Moved to: **`docs/archive/transient-2026-02-17/`**
See **`docs/archive/transient-2026-02-17/README.md`** for the full merge map.

**docs/ root now contains only canonical core documents.**

---

### 2026-02-17 (code_review/ Normalization — Session/Phase Docs)

Archived 20 non-conforming session and phase documents from `docs/code_review/` that did not follow
the `REVIEW_<Module>_YYYY-MM-DD.md` naming convention. All were verified as processed/obsolete before
archiving. Key implementations confirmed in codebase: (1) `DataBlockSlotIterator`/`with_next_slot()`
removed, (2) dual schema hashes (`flexzone_schema_hash`, `datablock_schema_hash`) in `SharedMemoryHeader`,
(3) factory functions generate and validate both hashes.

Moved to: **`docs/archive/transient-2026-02-15/`**
See **`docs/archive/transient-2026-02-15/README.md`** for the full list and disposition of each file.

**`docs/code_review/` now contains only the active review:**
- `REVIEW_utils_2026-02-15.md` — 11 items open, tracked in subtopic TODOs

---

### 2026-02-14 (Standalone Documents Merge)

Merged standalone guidance documents into **`docs/IMPLEMENTATION_GUIDANCE.md`** for consolidation; originals moved to **`docs/archive/standalone-2026-02-14/`**.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| emergency_procedures.md | IMPLEMENTATION_GUIDANCE.md § Emergency Recovery Procedures | Recovery tools, failure scenarios, diagnosis and recovery commands |
| NAME_CONVENTIONS.md | IMPLEMENTATION_GUIDANCE.md § Naming Conventions | Display name format, logical_name() helper, suffix marker rules |
| NODISCARD_DECISIONS.md | IMPLEMENTATION_GUIDANCE.md § [[nodiscard]] Exception Sites | Test and production code that intentionally ignores [[nodiscard]] returns |

**Rationale**: Consolidate all implementation guidance in single reference document. Easier to maintain, search, and cross-reference. See **`docs/archive/standalone-2026-02-14/README.md`** for details.

### 2026-02-14 (Test Plan Cleanup)

Cleanup: Test plan document archived. Memory layout and RAII layer design documents remain active for ongoing implementation work.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md | README_testing.md, IMPLEMENTATION_GUIDANCE | Test plan (Phase A–D, checklist, coverage) → README_testing.md § DataHub and MessageHub test plan. MessageHub review (Part 2) → IMPLEMENTATION_GUIDANCE § MessageHub code review. `docs/testing/` removed. |

**Active documents (not yet archived):**
- `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` — Key design document for ongoing memory layout implementation
- `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` — Key design document for ongoing RAII layer implementation

These documents will be merged into canonical docs (HEP, IMPLEMENTATION_GUIDANCE) and archived after implementation is complete.

---

### 2026-02-13

Transient docs merged into IMPLEMENTATION_GUIDANCE (and where noted, DATAHUB_TODO, HEP); originals moved to **`docs/archive/transient-2026-02-13/`**.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| DATAHUB_CPP_ABSTRACTION_DESIGN.md | IMPLEMENTATION_GUIDANCE | C++ layer map (Layer 0–2), transaction API as recommended default. |
| DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md | IMPLEMENTATION_GUIDANCE | Required parameters, config validation, Pitfall 7. |
| DATAHUB_DATABLOCK_CRITICAL_REVIEW.md | IMPLEMENTATION_GUIDANCE | Cross-platform table; design review summary and gaps in § Deferred refactoring. |
| DATAHUB_DESIGN_DISCUSSION.md | IMPLEMENTATION_GUIDANCE | Flexible zone semantics; integrity "lighter repair" note. |
| CODE_QUALITY_AND_REFACTORING_ANALYSIS.md | IMPLEMENTATION_GUIDANCE | § Deferred refactoring is the active summary; full analysis in archive. |
| CODE_REVIEW_REPORT.md | — | 2026-02-13 full review; follow-ups completed. Kept for history. |

See **`docs/archive/transient-2026-02-13/README.md`** for the full merge map.

---

### 2026-02-12

Spinlock/guards, flexible zone flow, FileLock test patterns, versioning/ABI, test pattern and CTest docs, and testing supporting material merged into IMPLEMENTATION_GUIDANCE, README_Versioning, README_testing, or HEP; originals moved to **`docs/archive/transient-2026-02-12/`**.

See **`docs/archive/transient-2026-02-12/README.md`** for the list of archived files and merge targets.

---

## Quick reference: where to find historical content

| Looking for | Location |
|-------------|----------|
| Phase 3 RAII layer implementation history | `docs/archive/transient-2026-02-15/` (20 session/phase docs) |
| Dual schema validation design (FlexZone + DataBlock) | `docs/archive/transient-2026-02-15/PHASE4_DUAL_SCHEMA_API_DESIGN.md` |
| FlexZone schema gap root cause analysis | `docs/archive/transient-2026-02-15/ROOT_CAUSE_ANALYSIS.md` |
| Phase 2 cleanup plan (DataBlockSlotIterator removal) | `docs/archive/transient-2026-02-15/POST_PHASE3_CLEANUP_PLAN.md` |
|-------------|----------|
| Review findings / follow-up actions from last full review | `docs/archive/transient-2026-02-13/CODE_REVIEW_REPORT.md` (2026-02-13; follow-ups done) |
| Full code quality / refactoring analysis | `docs/archive/transient-2026-02-13/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md` (summary in IMPLEMENTATION_GUIDANCE § Deferred refactoring) |
| Memory layout / single flex zone design (superseded) | `docs/archive/transient-2026-02-14/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (merged into HEP §3 and IMPLEMENTATION_GUIDANCE) |
| DataHub C++ abstraction layer (original) | `docs/archive/transient-2026-02-13/DATAHUB_CPP_ABSTRACTION_DESIGN.md` |
| DataHub policy & schema analysis (original) | `docs/archive/transient-2026-02-13/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md` |
| DataHub critical review / design discussion (originals) | `docs/archive/transient-2026-02-13/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, DATAHUB_DESIGN_DISCUSSION.md |
| RAII layer draft (not merged) | `docs/archive/transient-2026-02-14/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` |
| DataHub/MessageHub test plan and MessageHub review (original) | `docs/archive/transient-2026-02-14/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` (merged into README_testing + IMPLEMENTATION_GUIDANCE; docs/testing/ removed) |

For current documentation layout and where to put or find active content, use **`docs/DOC_STRUCTURE.md`**.

## 2026-04-16: L3.γ/ζ refactor doc cleanup

| Document | Destination | Reason |
|----------|-------------|--------|
| `step7_messenger_removal_scope.md` | `archive/transient-2026-04-16/` | Messenger class fully deleted; investigation complete; no HEP content to merge |
| `step7_continuation_guide.md` | `archive/transient-2026-04-16/` | Session handoff doc; §1.1 facilities table superseded by HEP-0018 §15; §1.2 deleted-entities table is historical; §1.3 Producer/Consumer factory now also deleted (L3.γ A6.3) |
| `obsolete_code_replacement.md` | `archive/transient-2026-04-16/` | All listed deletions verified executed; replacement mapping covered by HEP-0018 §15 + HEP-0007 |
| `config_single_truth.md` | `archive/transient-2026-04-16/` | Implemented 2026-03-30; timing/checksum/whitelist merged into HEP-0008 §11 (Mermaid diagrams + checksum mapping table). All claims verified against code. |
| `unified_role_loop.md` | `archive/transient-2026-04-16/` | §3-5 (CycleOps interface, run_data_loop frame, AcquireContext, retry_acquire, LoopConfig, 14-step lifecycle) merged into HEP-0011 "Unified Data Loop Architecture" section. All entities verified against cycle_ops.hpp + role_api_base.hpp/cpp + producer_role_host.cpp. |
| `schema_architecture.md` | `archive/transient-2026-04-16/` | Layered type diagram merged into HEP-0016 §11.0 as Mermaid flowchart; all types verified in code (SchemaLibrary, SchemaEntry, SchemaSpec, ZmqSchemaField, compute_field_layout) |
| `flexzone_api_design.md` | `archive/transient-2026-04-16/` | Fully implemented (L3.ζ). Queue abstraction diagram + flexzone access table merged into HEP-0002 §17.2/17.2.1. §17.2 rewritten (hub::Producer/Consumer deletion, abstract QueueWriter/QueueReader). All §2.1-2.7 verified against code. T4 (processor dual-fz) test still pending. |
| `thread_manager_design.md` | `archive/transient-2026-04-16/` | API, identity, drain, lifecycle thunk, Logger rationale, adoption table merged into HEP-0011. Doc was stale: described 1-param ctor (code has 2-param), join_all (renamed drain), join_all_done flag (removed). All current API verified against thread_manager.hpp/cpp. |
| `broker_and_comm_channel_design.md` | `archive/transient-2026-04-16/` | Messenger→BrokerRequestComm split rationale merged into HEP-0030 Appendix. BrokerRequestComm verified in code. All old entities (Messenger, ChannelHandle, P2C sockets) confirmed deleted. |
| `channel_implementation_plan.md` | `archive/transient-2026-04-16/` | Band implementation plan fully executed. BandRegistry, BAND_* protocol messages, BRC band methods all verified. Procedural content archived. |
| `channel_redesign.md` | `archive/transient-2026-04-16/` | Band-replaces-channels rationale merged into HEP-0030 Appendix (Mermaid diagram). All old entities deleted. |
| `loop_design_unified.md` | `archive/transient-2026-04-16/` | Timeout formula + processor output policy merged into HEP-0008 §2.2. Timing pseudocode (CycleOps frame) already in HEP-0011. 2 stale refs (read_flexzone/write_flexzone) noted — doc otherwise verified current 2026-04-14. |
| `on_idle_callback_design.md` | `archive/transient-2026-04-16/` | Feature NOT implemented (grep confirms no on_idle in codebase). Design proposal for separating control from data plane — valuable concept but deferred indefinitely. No HEP content to merge. |
| `role_unification_next_session.md` | `archive/transient-2026-04-16/` | Session handoff doc from 2026-04-14. Content superseded by role_unification_design.md implementation status table (updated 2026-04-16) and 21 commits on feature/lua-role-support. |

## 2026-04-21: HEP-CORE-0024 role binary unification closure + tech-draft archival

| Document | Destination | Reason |
|----------|-------------|--------|
| `config_module_design.md` | `archive/transient-2026-04-21/` | Verified fully implemented 2026-04-21: RoleConfig is the sole config class (ProducerConfig / ConsumerConfig / ProcessorConfig all gone); JsonConfig backend in role_config.cpp; pImpl ABI-safe; typed const accessors; strict key whitelist with unknown-key throws; in_/out_ directional slots. No remaining divergence between design and code. Rationale folded into HEP-CORE-0024. |
| `role_unification_design.md` | `archive/transient-2026-04-21/` | CLOSED 2026-04-21. Binary unification phases (α/γ/δ/ζ) done via HEP-CORE-0024 Phases 15-22. L3.β (3 CycleOps → 1) and L3.ε (ScriptEngine framework-agnostic invoke) **intentionally not pursued** — they conflict with zero-copy SlotView semantics and template-specialized duck-typed cycle operations. Design spirit met — role identity contained to CycleOps + role host + config; everything else role-neutral. Authoritative documentation pending HEP-CORE-0011 rewrite (API_TODO SE-03). Draft preserved as historical record of original refactor plan and the divergence rationale. Draft's final status block explains why the divergence was accepted. |
| `HUB_CHARACTER_DESIGN.md` | `archive/transient-2026-04-21/` | Promoted to HEP-CORE-0033 (2026-04-21). Verified content-complete merge: every substantive section (premises, 7 functions, HubHost class + lifecycle, CLI, config schema + parsing rules + sub-config headers, vault/keygen three-mode semantics, directory layout, HubState tables + retention + consistency, query-driven metrics model, AdminService RPC surface, script callbacks + HubAPI, protocol changes, HEP cross-refs, open items, out-of-scope) absorbed into HEP-0033 (with §6.5 added during the absorb pass). HEP-0033 is now the normative spec. Draft archived. |
| `invoke_convention_redesign.md` | `archive/transient-2026-04-21/` | CLOSED 2026-04-21. All proposals implemented and shipped; implementation evolved past the draft on two points (improvements over the draft): (1) flexzone removed from `InvokeTx` / `InvokeRx` structs entirely — accessed via `api.flexzone(ChannelSide::Rx|Tx)` cached typed view at `build_api()` time; cleaner separation of slot lifecycle from flexzone access. (2) Runtime predicates use `Rx`/`Tx` (`has_rx_fz()` / `has_tx_fz()` / `ChannelSide::Rx|Tx`) while config keys + spec accessors keep `in`/`out` (`in_fz_spec` / `in_flexzone_schema`) — formalises the runtime-vs-config distinction. All HEP-side documentation (HEP-CORE-0011 callback table, HEP-CORE-0024 §15.3 typed invoke rationale, HEP-CORE-0027 inbox API) consistent with shipped code. Draft preserved with rationale for the divergence. |
| `datablock_queue_ownership.md` | `archive/transient-2026-04-21/` | CLOSED 2026-04-21. All proposals implemented and shipped. Draft's status banner said "Partially implemented" with §9 Step 11 (role host size members) PENDING — verified actually DONE (`grep -rn out_schema_slot_size_ src/` returns zero hits). Two sections describe deleted entities: §5.2 references `hub::Producer::spinlock(idx)` (hub::Producer deleted L3.γ A6.3 2026-04-15; state migrated into RoleAPIBase::Impl), and §6 references `establish_channel()` (function gone; logic now in RoleAPIBase::build_tx_queue/build_rx_queue). Authoritative spec is HEP-CORE-0002 §17.2 (Queue Abstraction, with Mermaid class diagram + ownership note) and §17.2.1 (Flexzone Access). No merge into HEP needed — HEP-0002 already covers the substance with the right level of detail. Draft preserved as historical record. |

> **Backlog note (added 2026-06-27):** Many archive batches between 2026-04-21 and 2026-06-27 are not logged in this file (the `docs/archive/transient-2026-MM-DD/` dirs visible under `ls docs/archive/` predate the log).  Backfill is out of scope for the current Phase 0 cleanup; future archive batches use the rolling format below.

## 2026-06-27: Phase 0 cleanup — closed code reviews

Six code reviews moved to `docs/archive/transient-2026-06-27/code_reviews/`.  All verified ✅ FIXED / 0 ❌ OPEN by independent triage agent before archive.  Rationale: TODO_MASTER §"Active code reviews" (lines 252-258) did not list any of these as active; review status tables confirm full close-out; per `DOC_STRUCTURE.md §2.2` reviews with 0 OPEN items + completed before the 2026-06-15 cutoff are archive-ready.

| Document | Status table | Closed-by task / area |
|----------|-------------|------------------------|
| `REVIEW_ConfigAndEngine_2026-03-21.md` | 13 FIXED / 0 OPEN | Config-and-Engine modernisation; superseded by HEP-CORE-0024 + role_unification_design closure |
| `REVIEW_WaveM2.5_PostStep6_2026-05-11.md` | 7 FIXED / 0 OPEN | Wave-M2.5 side-arc post-step-6 audit; closed by Wave-M2.5 wrap |
| `REVIEW_WaveM3_FullChain_2026-05-11.md` | 5 FIXED / 0 OPEN | Wave-M3 full-chain audit; closed by Wave-M3 close-out |
| `REVIEW_WaveM3_SixthPass_2026-05-11.md` | 2 FIXED / 0 OPEN | Wave-M3 sixth-pass audit; closed by Wave-M3 close-out |
| `REVIEW_HEP_Lifecycle_Sync_2026-06-13.md` | 8 FIXED / 0 OPEN | HEP lifecycle-sync sweep (task #213); HEP-0011/0017/0019/0031/0018 |
| `REVIEW_ZapRouter_UAF_2026-06-13.md` | 5 FIXED / 0 OPEN | ZapRouter Slice A UAF + reentrance + single-pumper (task #215) |

**Active reviews (NOT archived):** `REVIEW_Connection_Inbox_Band_2026-05-17.md` (16 ❌ OPEN — explicitly active per TODO_MASTER), `REVIEW_CatchBlocks_2026-05-01.md` (full-codebase silent-failure sweep), `REVIEW_FullModule_2026-04-06.md`, `REVIEW_HEP_0033_PostP9_2026-05-05.md` (F1 BLOCKER open), `REVIEW_ScriptEngine_2026-03-20.md` (3 OPEN), `REVIEW_WaveM2.5_2026-05-10.md` (5 OPEN), `REVIEW_WaveM3_{2026-05-11,PostFix,Rigorous,FifthPass}.md` (various OPEN items).

**Tech drafts:** 0 archived this batch.  All 8 active tech_drafts drive in-progress work — see `docs/tech_draft/README.md` for the live status table.  Promotion candidates (once their work ships): `DRAFT_HEP-0036-implementation-guideline_2026-05.md` → fold I1-I12 invariants into HEP-CORE-0036 §3.5bis once AUTH-1..7 ship; `engine_callback_tiers.md` → HEP-CORE-0011 once #77 Tier 2 callbacks ship; `raii_layer_redesign.md` Phases 2-5 → HEP-CORE-0024 "Typed C++ Addon Layer" once Phase 5 SimpleRoleHost template ships.

## 2026-06-27 (Phase 0b): AUTH_TODO compression + S5 protocol pre-walk

Doc-only cleanup that compresses the active AUTH_TODO from 1616 → 564 lines and produces a pre-flight Core Structure Change Protocol walkthrough for #275 S5.

**New documents:**

| Document | Purpose |
|----------|---------|
| `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md` | Index of AUTH_TODO sections extracted on 2026-06-27 with line ranges into the prior commit (`dfe86a61`) so future readers can fetch verbatim narrative on demand without paging the full pre-compression file.  Companion to the 2026-06-05 and 2026-06-09 archives. |
| `docs/code_review/REVIEW_S5_CoreStructure_2026-06-27.md` | Walked checklist for #275 S5 — renaming `SharedMemoryHeader::shared_secret[64]` → `reserved_capability_token[64]`.  Each of the 9 Core Structure Change Protocol matrix items (size+alignment, schema macro, ctor init, producer registration, consumer discovery, schema validation, checksum logic, test coverage, documentation) walked against current code; sequencing pre-conditions enumerated; ship-step checklist captured.  Lands as a 2026-MM-DD archive after S5 ships green. |

**Compressed file:**

`docs/todo/AUTH_TODO.md` reduced from 1616 → 564 lines.  Completed-phase narratives (AUTH-1 full sub-deliverables 4(a)-(g) + B1 + producer-S3 + follow-ups 6.1-6.8; AUTH-2/3 detailed; AUTH-4 SUPERSEDED block; HEP-0041 Phase 1 substep 1a-1k narratives + REVIEW-A/B close-outs; HEP-0036 §5b parallel track table; pre-flights #263-#265) extracted to the archive index.  Active AUTH_TODO retains: design principles (verbatim — load-bearing); current PeerAdmission state table; AUTH-5/6/7 active scope; HEP-0041 critical-path table with active rows only; #275 S2..S5 detailed plan; HEP-0041 Phases 2-5 brief; design audit gaps with active anchors; backlog; deferred decisions; parallel tracks; decision log; memory rules.

**Verification correction recorded:** Pre-compression line 1137 claimed `S1+S2a+S2b+S2c-1..6+S3 ✅ shipped`.  Verified against code at HEAD: **S3 is NOT actually shipped** (`hub_shm_queue.cpp:375` still has `set_shm_secret()`; `:134/198/216/253` still has `shared_secret` parameters).  Corrected status carries forward in the compressed AUTH_TODO.

## 2026-07-18: Doc hygiene — code-verified archive of closed reviews + shipped tech-drafts

Every verdict below was verified against **actual `src/` code** (not commit
logs, not the doc's own status line), following the precedent set by the
2026-06-27 batch's "S3 not shipped" catch.  Two independent verification
passes produced file:line evidence for each move.  Nothing deleted — all
files `git mv`'d to `docs/archive/transient-2026-07-18/`.

### Code reviews → `archive/transient-2026-07-18/code_reviews/` (13)

| Document | Status table | Code evidence for the verdict |
|----------|-------------|-------------------------------|
| `REVIEW_AUTH_ReviewD_2026-07-17.md` | 5 met, 1 intentional DEFER, 0 OPEN | PID sweep removed (`check_dead_consumers` = 0 hits); `_on_consumer_revoked` live `hub_state.cpp:2049` wired `broker_service.cpp:3902` |
| `REVIEW_AUTH_ReviewE_2026-07-17.md` | 8-threat model ✅, 0 OPEN | `zap_router.cpp:521` `is_peer_allowed`; `broker_service.cpp:1026` `curve_server=1` |
| `REVIEW_HEP0041_ReviewC_2026-07-16.md` | 6 resolved, 0 OPEN | MSG_CTRUNC fail-close `shm_capability_channel.cpp:635`; `reserved_capability_token` `data_block.hpp:227` |
| `REVIEW_C2_2026-06-29.md` | 15 findings all disposed, 0 OPEN | `KeyStore::with_keypair_z85` `key_store.cpp:461` (F14); `apply_5b_canonical_fields` keyed on wire identity (F7).  (F14 production-caller migration is a separate tracked follow-up.) |
| `COVERAGE_AUDIT_Broker_Queue_CURVE_2026-07-17.md` | COMPLETE, 0 blocking OPEN | gap-fix tests exist: `MutualAuth_RejectsFrame3PubkeyMismatch` `test_attach_protocol.cpp:910`; `RejectsMultiFdTruncatedScmRights` `test_shm_capability_channel.cpp:438`.  Residuals tracked in TESTING_TODO + #57. |
| `REVIEW_AUTH6_TestDisposition_2026-06-27.md` | audit delivered + dispositions executed | AUTH-6 (#154) batches 2a/2b shipped 2026-06-27; only tracked "File 10 Suite 2 delete" housekeeping residual remains (in AUTH_TODO). |
| `REVIEW_HEP_0033_PostP9_2026-05-05.md` | F1–F4 fixed, F5 was OPEN, F6 non-issue | F5 now RESOLVED — L4 hub-role tests exist (`test_plh_hub_role_shm_e2e.cpp`, `test_plh_hub_role_zmq_e2e.cpp`).  **Correction:** the 2026-06-27 log note "F1 BLOCKER open" was inaccurate — F1 was fixed 2026-05-05 (`1439ef4`). |
| `REVIEW_WaveM3_2026-05-11.md` + `_PostFix` + `_Rigorous` + `_FifthPass` (4) | chain of 6 passes; each pass's opens fixed in the next | mechanism live: `_set_role_disconnected` `hub_state.cpp:749`, `_dispatch_role_disconnected_if_dead` `:795` wired `:1486/:1574`; `subscribe_role_disconnected` broker-wired `hub_state.hpp:1795`.  Two sibling passes (FullChain, SixthPass) already archived 2026-06-27; this completes the Wave-M3 review set. |

### Code reviews archived AS-MOOT (findings superseded by later refactors) (2)

| Document | Why moot | Residual items preserved here (NOT lost) |
|----------|----------|------------------------------------------|
| `REVIEW_ScriptEngine_2026-03-20.md` | Code findings moot/fixed: SE-09 legacy `ScriptHost`/`LuaScriptHost` classes gone from `src/`; SE-14 `script.type` now parser-validated (`role_config.cpp:88`).  Subsystem rewritten by role-unification. | **Residual doc-parity items SE-03 / SE-04 / SE-08** (HEP-0011/0018/0015 doc-staleness, March 2026) were NOT re-verified as folded.  They concern the *pre-role-unification* ScriptEngine; re-derive against HEP-0011 if that doc is next revisited rather than treating verbatim.  Archived file retains full text. |
| `REVIEW_WaveM2.5_2026-05-10.md` | Blockers shipped: F1 `UID_CONFLICT` (`hub_state.hpp:450/493`, `hub_state.cpp:1327`); deferrals resolved — F12 `disconnected_fired` retired (`hub_state.hpp:1171`), F13 `metrics_store_` retired (`broker_service.cpp:655`).  Side-arc labeled "closed" in all label-hygiene tables. | **Residual F2 / F3** were HEP-0007/0021 doc-shape updates said to have "co-landed with step 3"; not independently re-confirmed against current HEP wording.  Archived file retains full text. |

### Tech-drafts → `archive/transient-2026-07-18/tech_drafts/` (2)

| Document | Merged into | Shipped-code evidence |
|----------|-------------|------------------------|
| `DRAFT_versioned_admission_ledger_2026-07-13.md` | HEP-CORE-0042 §5.5.2.1 (INVARIANT-BIND-CONFIRM-1..3) | `src/include/utils/versioned_admission_ledger.hpp` exists; used `broker_service.cpp:2792/2807/4062/4355/4748`, `hub_state.cpp:2046/2070/2126/2199`.  Old quartet survives only in doc-comments (the cosmetic residue the draft's §7 predicted). |
| `abi_check_facility_design.md` | HEP-CORE-0032 §8/§8.5 (references `check_abi()` as already-existing) | Facility fully shipped: `check_abi()` `version_registry.cpp:273`; 7-axis `ComponentVersions` incl. `script_engine`; startup call `plh_role_main.cpp:191`; native-plugin extension `native_engine_api.h`.  The "draft" label was stale — code is *ahead* of the doc.  (Only `test_abi_check.cpp` from §9 absent — non-blocking, tracked.) |

### Doc-hygiene bugs fixed this batch
- `TODO_MASTER.md` "Active code reviews" listed `REVIEW_TestAudit_2026-05-01.md` as "TOP PRIORITY active" — it was already archived 2026-05-02.  Entry removed; section rewritten to the 4 genuinely-active/KEEP reviews with their reproducing-in-code evidence.
- Corrected the 2026-06-27 log's inaccurate "REVIEW_HEP_0033_PostP9 (F1 BLOCKER open)" note (see table above).

### KEEP (verified genuinely open — NOT archived)
- **Reviews:** `REVIEW_Connection_Inbox_Band_2026-05-17.md` (X6/X2 reproduce), `REVIEW_CatchBlocks_2026-05-01.md` (unstarted sweep), `REVIEW_FullModule_2026-04-06.md` (C-1/D-2 reproduce), `LINT_FIXES_PLAN.md` (§2 undecided, partly moot).
- **Tech-drafts (9):** every "pending/ahead" item verified ABSENT from `src/` — `DRAFT_curve_admin_protocol` (admin CURVE, impl not started = next work), `DRAFT_topology_singular_side` (C7/D-R6 not shipped), `DRAFT_HEP-0036-implementation-guideline` (AUTH-1..7 open), `DRAFT_HEP-0041-pattern4-reform-coverage` (#275 S2 + #285 open), `DRAFT_keystore_ephemeral_and_script_crypto` (script-crypto bindings absent), `DRAFT_HEP-0031-bounded-thread` (`spawn_bounded`/`BoundedThreadRegistry` absent — base `ThreadManager` is a different, shipped thing), `DRAFT_reg_wire_alignment_cleanup` (HEARTBEAT rename half-applied, §7 errata open), `engine_callback_tiers` (Tier-2 `supports_dynamic_callbacks`=false), `raii_layer_redesign` (`TypedInboxClient`/`SimpleRoleHost` absent), `SCRIPT_RELOAD_DESIGN` (only stub `reload_script()=false`).
- **KEEP-BUT-FIXED-STATUS:** `DRAFT_HEP-0041-test-completeness_2026-06.md` — planned tests all shipped + gaps closed; status line corrected to "SUBSTANTIALLY COMPLETE, archive-pending merge into README_testing".

## 2026-07-27 — schema/metrics query-integration arc complete (slices 1–4 shipped)

### Tech-drafts → `archive/transient-2026-07-27/tech_drafts/` (1)

| Document | Merged into | Shipped-code evidence |
|----------|-------------|------------------------|
| `DRAFT_schema_metrics_query_integration_2026-07-26.md` | HEP-CORE-0034 §2.4 (I10), §6.4 (fingerprint chain + packing recovery), §10.2 (owner axis), §10.3a (schema-at-establishment + `from-channel` sentinel + runtime-resolution diagram + script proxies), §11.1 (ChannelEntry format fields); HEP-CORE-0007 §12.3 + §12.4a (SCHEMA/METRICS payloads + error rows); HEP-CORE-0036 §5b.7 (ACK schema fields) + NOT_A_ROLE_OF_CHANNEL row; HEP-CORE-0011 cross-engine parity rows (`get_schema`/`get_channel_schema`/`get_channel_metrics`); HEP-CORE-0028 §4.7 (`get_*_json`); IMPLEMENTATION_GUIDANCE § "Role-side query surface"; README_Deployment §6.1/§8.3; README_topology_channels §5 | All four slices shipped + swept green (final sweep 2705/2705, commit `d208add7`): member-gated queries (`handle_schema_req`/`handle_metrics_req` SI-tier gates → `Control_EnvelopeWithRoleUid`); 3-engine bindings (ABI v13 `get_*_json`); ACK delivery (`broker_service.cpp` unified consumer success-ACK builder); schema-pending queue (`hub_zmq_queue.cpp` `configure_slot_schema`); runtime resolution (`role_api_base.cpp` `resolve_runtime_slot_schema` + `parse_canonical_fields_str`/`recover_zone_packing` in `schema_utils.hpp`); `from-channel` sentinel (`SchemaSpec.runtime_resolved`); engine slot proxies (late `InSlotFrame` registration, native adoption gate).  Open residuals all live in `MESSAGEHUB_TODO.md` (SHM runtime path, config-pin field, typed SchemaReqBody/MetricsReqBody); observer role kind rides #292. |

## 2026-08-03 — review-record validation pass: one review fully dispositioned

Every finding in the four active review records was re-checked against the
current tree (record: `code_review/REVIEW_VALIDATION_2026-08-02.md`). The
table below archives the one review where that left nothing open.

### Reviews → `archive/transient-2026-08-03/code_reviews/` (1)

| Document | Why archived | Residual |
|----------|--------------|----------|
| `REVIEW_FullModule_2026-04-06.md` | All ten rows dispositioned with code evidence. Two findings' subjects no longer exist (**A-1** `data_transport()`, **B-2** `engine_module_params.cpp` — both files deleted by the topology/transport rework). **C-1** fixed in `d92a7b0b`; **D-2** and **E-1** were already resolved and never marked. **F-1**/**F-4** are test-*count* comparisons, not defects — nothing actionable was ever stated. **F-3**'s two named offenders are gone (`test_datahub_broker_protocol.cpp` went from 21 `sleep_for` to zero; `poll_until` adoption 2 files → 14). | **B-1** (`should_continue_loop`/`should_exit_inner` tested-but-uncalled) is genuinely open and moved to `docs/todo/API_TODO.md` as band-4 work before archiving — it had been living only in this review, which is exactly how items get lost. |

### Doc-hygiene bugs fixed this batch

- `TODO_MASTER.md` "Active code reviews" described `REVIEW_FullSystem_2026-07-20.md`
  as "52 RESOLVED, 4 OPEN" and, in the same bullet, "**29 OPEN**, by cluster".
  Both numbers cannot be right. Corrected to the cluster breakdown, which is
  the one with per-finding file references behind it.
- The 2026-07-18 log listed `REVIEW_FullModule_2026-04-06.md` under KEEP
  because "C-1/D-2 reproduce". C-1 is fixed and D-2 was already resolved when
  that note was written, so the KEEP rationale no longer holds.

---

## 2026-08-06 — auth-list replication plan (#101)

### Transient → `archive/transient-2026-08-06/` (1)

| Document | Why archived | Residual |
|----------|--------------|----------|
| `PLAN_auth_list_replication.md` | Execution record for HEP-CORE-0035 §4.9, complete and verified against code (Debug 2779/2779, Release 2776/2776, commit `6254b628`). Every lasting piece has a permanent home: the design in HEP-0035 §4.9, the connection/retry/script narrative and the interleaved two-level sequence in HEP-0027 §3.5/§4.2, the wire rows in HEP-0047 §3.2/§3.7, the coverage statement in `todo/AUTH_TODO.md`, and the test lessons in `todo/TESTING_TODO.md`. What remains in the archived file is execution state — the five-layer order, the rejected alternatives, the reverted test backdoor — which is history, not design. | **"Either side is enough" is unpinned** and moved to `todo/TESTING_TODO.md` § Current Focus before archiving: a processor's two roster sides need two HUBS to distinguish the OR from a single-side lookup, and one hub cannot show it. Recorded there rather than left in the plan, which is exactly how an open item disappears. |

### Why this one is worth reading later

Two results in it were negative, and both cost more to establish than the code
they judged. **A test that appears to cover a fix, and does not**: the L4
two-sender case passed with its fix disabled, because the periodic report
rescues the hub's view inside any budget a subprocess test can use — a
safety net makes the one-shot fix it backs up untestable by outcome. And **a
proposal that reading the code refuted**: a role with two channels on one hub
was supposed to receive two same-version REG_ACKs and hit the version guard
deterministically; a producer has one channel, and a processor's two
registrations land on different sides, so no such path exists.

The arc's headline number belongs here too: **five substantive defects, all
found by review or owner pushback, none by the suite** — which went 2774/2774
with the first of them live.

---

## 2026-08-07 (pass 5) — AUTH_TODO reduced to open items

### Transient → `archive/transient-2026-08-07/todo-completions/` (1)

| Document | Why archived | Residual |
|----------|--------------|----------|
| `AUTH_TODO_closed_2026-08-07.md` | Extraction record for the `todo/AUTH_TODO.md` rewrite, 850 → ~190 lines. Holds the closure **evidence** (what was read, and where) for six items the file listed as open and that were not, the list of completed phases and chains, the decision log, and the explanation of the task-ID collision. The narrative prose itself is in git at `e5de9a53` and in the 06-05 / 06-09 / 06-27 completions files, so it was not copied again. | Seven live tasks carried out: #121 SEC-Fold, #122 CTRL ZAP allow-path pin, #123 role vault `.pub`, #124 demo auth migration, #125 CURVE doc backlog, #126 macOS/Windows SHM backends, #127 CLI `--init`. Three of them (#121–#123) are security items and were added to band 1 in `TODO_MASTER.md`. |

### What the pass was actually for

Not compression. The file had begun to **misreport in both directions**, and
a tracker that misreports is worse than a long one — it sends work to places
that are already finished and leaves real gaps unlisted.

Six items marked open were closed, each verified by reading the tree:

- The **admin reverse-notify channel** was listed as a missing feature. It
  had been *retired by design* on 2026-07-22 and replaced by the polled
  output buffer — which shipped. A later section of the same file recorded
  the retirement. The file contradicted itself.
- The **7 masked `RoleIdentityPolicy` tests** were listed as awaiting a
  delete decision. Commit `c7f4f608` deleted the whole file on 2026-07-20.
  This one is worth singling out: **the same claim had been written into
  this file's own hazard banner earlier the same day**, as the one item
  verified still open. It was taken from the audit table, not the tree —
  the exact failure the banner existed to warn about. A banner warning that
  markers are unverified is not itself verification.
- **#275 S2**'s "16 workers still to scan" named a file that does not exist;
  the surviving matches are tombstone comments.
- **Native `allowed_peers` / `producers`** were marked "deferred per MVP".
  Both are in the C ABI at `native_engine_api.h:323` and `:354`.
- The **§11.0.4 fire-and-forget tension** was recorded as fixed in one row
  and unresolved in another.
- **Vault hot reload** was still listed as the top open security item after
  the owner decided against it.

### Cross-references corrected in the same pass

The stale claims had propagated outward, which is why this was not a
single-file edit:

- **`HEP-CORE-0033` §11** — the design authority told readers that the
  console output buffer, `admin_console_print`, and `origin_uid` on broker
  records were "not yet implemented". All three had shipped. Replaced with
  the verified state plus the one thing that *is* open (the scoped actuation
  origin, #105).
- **`MESSAGEHUB_TODO`** — "HEP-0035 auth: D4–D7 open" survived eight weeks
  past the 2026-06-09 restructuring that replaced D4–D7 with AUTH-1..7, all
  of which have shipped. It read as four open protocol phases.
- **`API_TODO`** — "Blocks AUTH_TODO D4 + D5", same dead numbering.
- **`TODO_MASTER`** — the next-actions table still led with vault hot reload
  as "the biggest open security item"; the `#112` row still said to check
  band 2 first, after band 2 was retired.
- **`REVIEW_VALIDATION_2026-08-02`** — its "Not yet done" list still carried
  two items the same document's own body had completed.
- **`todo/README.md`** — the lower half still taught "move completed tasks
  to Recent Completions" and "archive old completions monthly", directly
  contradicting both `DOC_STRUCTURE.md` §2.1.1 and the correction notice at
  the top of the same file. A first pass had fixed the visible sections and
  missed the pitfalls, examples, and command crib below them.

### The task-ID collision, and why no mapping table was built

`AUTH_TODO` cited IDs `#52`–`#317` while the live list ended at `#119`, so
the ranges overlapped with different meanings — `#103` meant two different
things inside the one file. No mapping was produced, deliberately: nearly
every legacy ID hung off *finished* work, and an ID pointing at finished
work does not need a new name, it needs deleting. A blind rename would also
have been wrong, since many `#NNN` in the text are HEP numbers, PR numbers,
or line references. Recovered anchors for reading old commits are listed in
the archived file.

**The generalisation, now three passes deep:** a record is written when
true, the code moves, nobody re-reads. The habit that catches it is to look
up the *replacement* a note names, not just the site it fixed — several
notes named successors that were never built, and several named predecessors
that were already gone.

---

## 2026-08-07 (pass 6) — `REVIEW_VALIDATION` archived; three sweep items withdrawn as false

**Archived:** `docs/code_review/REVIEW_VALIDATION_2026-08-02.md` →
`docs/archive/transient-2026-08-07/code_review/`.

Its purpose — dispositioning the other review records — is complete. Of ~26
items it carried as open: 12 were already fixed and never marked, 2 stale, 2
misreads, 5 advisory, 5 genuinely valid. Its standing caveat about the
`REVIEW_FullSystem` review's ~50 unverified ✅ notes was discharged when that
verification ran and every note held.

Per §1.7, nothing was left living only in the archived document. Residue at
the time of the move: CURVE doc backlog → **#125**; the one unvalidated
finding, `start_handler_threads` phase 2-4 observability (S3) → **#135**,
created specifically so the move would not drop it; `query_shm_info` →
**#112**. The `REVIEW_FullModule` F-1/F-2/F-4 counting items are recorded as
deliberately unscheduled — re-deriving a count is worth doing only when a
decision hangs on it.

`code_review/LINT_FIXES_PLAN.md` remains the single active record (task #114,
input stale, do not action as written).

**The finding this pass actually turned on.** Three items filed during the
2026-08-07 tracker sweep were re-verified and did not survive:

| Item | Claim | Reality |
|---|---|---|
| #122 | CTRL ZAP has no allow-path pin | No gap. Unknown-key refusal is pinned; known-key admission is proven by every role that registers. Allow-branch counter pins already existed elsewhere. |
| #128 | Four ShmQueue contracts lost coverage | All four exist and pass in `test_hub_shm_queue_contract.cpp`. The "tombstone comment listing what was lost" is the header of the `add_executable` that *builds* them. |
| #130 | `expected_schema_owner` is an orphan field; delete the accessor | Enforced twice in the broker with a documented operator-facing error code, ruled deliberately 2026-07-26. Deleting it would have re-opened the hole that ruling closed. |

All three were labelled "verified against code". All three were **absence
claims produced by counting search hits instead of reading them** — a grep for
an invented test name, a single guessed file path, and a hit-count split by
directory. The items verified by a different method (grepping *invocations* —
#132, #133) held, as did the ones asserting presence (#124, #131).

**The rule, now earned four times over:** to show a behaviour is untested or a
field unused, grep the **behaviour** — the accessor, the config field, the wire
type — repo-wide, and **read every hit**. Never a hypothetical test name, never
a single guessed path. And a comment enumerating things is not evidence of
their absence: read what the comment is attached to before quoting it as proof.

**Cost of not doing that:** #130 would have deleted live validation. It was
ranked the most expensive failure mode on the sweep's own list, and the sweep
committed it.

### 2026-08-07 (pass 6, addendum) — two HEP status corrections

Both are factual syncs to shipped code; neither changes a design decision.

**HEP-CORE-0035 status table.** `§4.2` was labelled *"Layer-2 federation-trust
gate ⏳"*. Two errors in one cell: §4.2 is *"Pubkey index — single source of
truth"* (the federation-trust policy modes are §4.3), and it is **built** —
`PeerAuthority` / `PubkeyOrigin` ship in
`src/include/utils/security/pubkey_origin.hpp`, and the broker arms its ZAP
allowlist from `zap_allowlist()` at startup. Corrected to §4.2 ✅ with its own
subject, §4.3 ⏳ carrying the federation-trust gate. Also removed a dead task-ID
citation whose number now resolves to an unrelated live task.

**HEP-CORE-0035 §4.7 note added.** While correcting the table: §4.7.4 prescribes
a `src/utils/security/runtime_key_handling.{hpp,cpp}` utility that does not
exist — and must not be built. All three measures §4.7.2 mandates already ship
in facilities that own the subject properly: page-locking is `LockedKey`
(`sodium_malloc` — mlock, guard pages, canary) in the KeyStore; core-dump
suppression and compiler-proof zeroing are the secure memory subsystem.
Building the prescribed module would add a second security surface beside the
one already carrying the contract. Marked superseded, not pending. Recorded
honestly: the *mechanisms* are verified present, the *coverage* — whether every
in-memory secret routes through them — is not traced and is not claimed.

**HEP-CORE-0036 §5b.6.** Added the missing `expected_schema_owner` row to the
CONSUMER_REG_REQ field catalog: OPTIONAL, required when a named citation opens
a fan-in channel, openers restricted to `""` or `"hub"`, joiners matched
exactly, owner-without-id rejected. The omission is what made the field look
orphaned and produced the withdrawn proposal to delete it.

### 2026-08-08 — SEC-Fold withdrawn; HEP-CORE-0043's tail repaired

**No documents were archived or merged. That is the outcome.**

SEC-Fold-1 proposed folding six security HEPs into one. It is **withdrawn,
not deferred.** Three structures were proposed in sequence and each died on
evidence:

1. *Fold six into one* — ~12,000 lines, 45% of it HEP-0036. An archive with
   a table of contents, not a design contract.
2. *Merge the vault documents* — the vault already has **finalized** owners:
   HEP-CORE-0024 §3.4/§3.4.1 (role side) and HEP-CORE-0033 §6.5/§7.1/§7.2
   (hub side). Merging would have stripped a finalized contract out of the
   documents that reasoned about it.
3. *Merge by measured coupling* — the citation matrix puts the densest
   cluster at 0036/0041/0042/0044 (0036↔0041 = 46 mutual citations), the
   wire protocols, which is the opposite of every grouping proposed. And
   citation counts cannot distinguish healthy layering from tangled
   ownership, so they license investigation, not a restructure.

**Decisive:** the concrete defect that justified the fold — the `sodium_init`
triangle, where each HEP assumed another established the discipline — was
fixed by HEP-CORE-0043 *existing* and giving that decision one owner. The
trigger was already spent.

**What was repaired instead** — the HEP-0043 tail, which was the live hazard:

| Defect | Reality |
|---|---|
| §8 cited "HEP-0038 §5-§9" for vault format | HEP-0038 is 200 lines and **ends at §5**; its own header disclaims the content |
| §9.1 marked HEP-0036 "SUPERSEDED-STATUS-ONLY — content authoritative" | Self-contradictory; 0036 says DESIGN FINAL and has **grown ~1,000 lines** since being marked superseded |
| §9.3 carried a live build plan for the broker observer | Retired 2026-08-07 |
| §11 support matrix: "Script vault ✅ ✅ ✅ ✅" | **No script vault exists on any platform** |
| §13 instructed banner-marking four HEPs superseded | Withdrawn; replaced by an ownership map |
| "SEC-Fold-1b — remaining work" section | ~2,000–3,000 lines of prescribed migration, including "delete old HEPs' authoritative content" — deleted |

§8/§9/§10 are now **index sections that name owners and forbid migration**,
not stubs. That distinction is the durable fix: a stub reads as *"this
document intends to own this,"* which invites the migration that must not
happen. Empirically the stubs never produced migration — of three, none was
filled and two **fissioned outward** into HEP-0044 and HEP-0045. In this
session alone they generated three wrong restructure proposals before the
pattern was recognised.

Also stripped from the document per DOC_STRUCTURE §0: four commit hashes and
three task IDs. Two `Related documents` paths pointed at `tech_draft/` for
files that live in `archive/`.

**Split out:** script access to on-disk secret storage is now its own
design-first item. `RoleVault` is write-once, so the long-standing framing
("extend the payload with a `scripts` map") hid a missing write path on the
file that holds the role's identity key.

**Naming ruled by the owner:** the on-disk container keeps the word *vault*
— 1,891 references across `src/`, `tests/`, configs and docs, and the
meaning is load-bearing. The unbuilt script-facing store gets a different
name; renaming it is free now and expensive later.
