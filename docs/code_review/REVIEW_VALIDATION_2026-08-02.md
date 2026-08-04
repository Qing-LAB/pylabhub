# Review-record validation pass — 2026-08-02

**Why this exists.** Review passes wrote their findings straight into
`TODO_MASTER.md`, where a machine's suggestion became indistinguishable from
work the owner had decided on. The three findings sitting at the head of
band 1 turned out to be two non-problems and one that had been misdiagnosed —
and they were worked FIRST, because they were at the top. This pass checks
every remaining review finding against the actual code and the governing HEP
before any of it is allowed near a plan.

**Method, per finding.** Locate what the finding names in the code as it
stands today. Read the design document that says what the thing is supposed
to do. Then decide between four verdicts:

| Verdict | Meaning |
|---|---|
| **VALID** | Reproduces today, and the design agrees it is wrong |
| **STALE** | The code it describes no longer exists in that form |
| **RESOLVED** | Fixed since, but never marked |
| **MISREAD** | The code was read wrongly, or the design says otherwise |

A finding is not scheduled work until it is VALID.

---

## Scope

| Review | Findings | Unresolved at start | Status |
|---|---|---|---|
| `REVIEW_FullSystem_2026-07-20.md` | 56 | 4 | ✅ validated |
| `REVIEW_FullModule_2026-04-06.md` | 7 | 7 | ✅ validated |
| `REVIEW_Connection_Inbox_Band_2026-05-17.md` | 24 blocks (~15 real findings) | ~15 | ✅ validated |
| `REVIEW_CURVE_Integration_2026-07-19.md` | 3 category blocks | 3 | ✅ validated (code + gaps); doc backlog partial |
| `REVIEW_CatchBlocks_2026-05-01.md` | 0 | 0 | n/a |

---

## `REVIEW_FullSystem_2026-07-20` — 4 unresolved, all now settled

Three were closed by work done this same day; their resolutions are now
recorded in the review itself rather than only in commit messages.

| Finding | Verdict |
|---|---|
| inbox worker `:366` — frame magic / gap-count / seq-reset | **MISREAD (2 of 3) + VALID (1)** |
| `test_logger.cpp:220` — broken diagnostic | **VALID → fixed** |
| `test_hub_vault.cpp:316` — `known_roles` round trip | **VALID → fixed** |
| federation ingress bypasses `receive_and_validate` | **VALID but PARKED** (owner, #69/#99) |

The inbox finding is the instructive one, because it is three claims wearing
one heading:

- *"frame-magic validation is untested"* — **MISREAD.** Magic is checked in
  exactly one place, `wire_detail::decode_frame`, shared by the inbox and the
  data plane, and pinned at L1 by `ZmqWireFrameTest.RejectsWrongMagic` — the
  very line the finding's own evidence cites. What is true is narrower: the
  L3 worker named for it cannot reach the decoder, so the inbox's own
  `!env.valid` branch is not entered via a bad magic. Its sibling triggers are
  already pinned and do the same thing, so a fourth is low value.
- *"a reconnected sender's reset seq must not be mistaken for a replay"* —
  **MISREAD.** HEP-0027 §3.6 states seq is metrics-only and explicitly must
  NOT gate replay, precisely because it resets on reconnect. There is no code
  path in which a reset seq could be read as a replay, so there is nothing to
  pin.
- *"`recv_gap_count` has no test"* — **VALID**, now closed. Note that a later
  pass called it *untestable* on the grounds that a hand-built frame cannot
  carry a valid schema tag. That was also wrong, and in the same way: the
  counter is not reached by forging a frame, it is reached by causing a real
  loss.

---

## `REVIEW_FullModule_2026-04-06` — 7 findings, 4 months old

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| **A-1** | Processor data-socket poll comment contradicts code | **STALE** | `data_transport()` no longer exists anywhere in `src/processor/` or `src/consumer/`; the `!= "zmq"` construct is gone with the topology/transport rework. Nothing to fix or verify. |
| **B-1** | `should_continue_loop()` / `should_exit_inner()` unused | **VALID** | Both still defined (`role_host_core.hpp:516,525`) and still have **zero** production callers — the only references outside the header are in `test_role_host_core.cpp`, which tests them. Tested-but-unused is worse than the original finding: the tests make the dead code look live. |
| **B-2** | Redundant null-check ternary | **STALE** | `src/scripting/engine_module_params.cpp` no longer exists. |
| **C-1** | `to_channel_side()` duplicated 3× | **VALID** | Verbatim in `producer_api.cpp:228`, `consumer_api.cpp:213`, `processor_api.cpp:208`. Each is a file-static copy of the same `optional<int> → optional<ChannelSide>` mapping. |
| **D-2** | Three source files cite archived tech drafts | **RESOLVED** | No `script_engine_refactor` or `script_engine_lifecycle_module` reference survives in `script_engine.hpp` or `python_engine.hpp`; the third file is deleted. |
| **E-1** | Native engine API incomplete vs Python/Lua | **RESOLVED** | 6 of the 8 named methods are now in `native_engine_api.h` (`in_policy`, `in_capacity`, `out_policy`, `out_capacity`, `last_seq`, `set_verify_checksum`) — the #194 parity work. The remaining two, `update_last_seq` and `ctrl_queue_dropped`, exist nowhere in `src/include/` at all: they were retired framework-wide, so they are not a native gap. |
| **F-1..F-4** | Test coverage/quality | **MIXED — see below** |

### F-3 deserves care (the shape of a half-true finding)

Its two named offenders are gone: `test_datahub_hub_zmq_queue.cpp` no longer
exists, and `test_datahub_broker_protocol.cpp` now contains **zero**
`sleep_for` calls (was 21). `poll_until` adoption is up from 2 files to 14.

But the raw `sleep_for` count across `tests/` is **248**, against the 110 the
finding cited. The suite has grown a great deal since April, so this is not
evidence of regression — and it is not evidence of resolution either. The
honest verdict is **the named instances are RESOLVED; the general pattern is
unmeasured.** Anyone reviving F-3 must re-derive the number for the files
that matter rather than trusting either figure.

`F-1` (native L2 test count), `F-2` (the named
`test_datahub_engine_roundtrip.cpp` no longer exists) and `F-4` are
**not yet validated** — they are counting exercises against a suite that has
roughly doubled, and the old numbers carry no meaning.

---

## `REVIEW_Connection_Inbox_Band_2026-05-17` — 2.5 months old

Several of these were fixed by the audit that produced them, or by the
role-state rework that followed, and simply never got marked.

| # | Finding | Verdict | Evidence |
|---|---|---|---|
| **S1** | Role-side has no explicit registration FSM | **RESOLVED** | The `Shared` struct's implicit "have I registered?" strings are gone; registration state is now per-presence via `Presence::registration_state` (HEP-0023 §2). The comment at `role_api_base.cpp:398-404` records the change. |
| **S2** | `RoleState` has 3 values but 4 observable states | **BY DESIGN** | The finding's own disposition says the split is documented in the docstring and acceptable. Not a defect; revisit only if the enum is extended. |
| **S3** | `start_handler_threads` phase 2-4 window unobservable | **NOT VALIDATED** | Needs a read of the current phase sequence; deferred rather than guessed. |
| **S4** | Band membership has no per-role local state | **RESOLVED** | `RoleAPIBase::is_in_band()` now reports the role's cached membership from `band_index_`, which is exactly the missing local state. |
| **T1** | Counters say "Ready", state says "Connected" | **VALID — not cosmetic; FIXED 2026-08-03** | The counters were emitted as JSON metrics keys, so this was an interface question, not a tidy-up. Settled by confirming with the owner that the codebase is the only consumer. See "T1 — how this row went wrong twice". |
| **T2** | `register_*` wire-field comments uniform | **CLEAN** | The finding records a passed spot-check, not a defect. Nothing to do. |
| **T3** | HEP-0030 §9 over-retires `CHANNEL_BROADCAST_REQ` | **RESOLVED** | §9.1 now states explicitly that the channel-bound broadcast family is NOT superseded, and carries the coexistence table. Corrected by this very audit. |
| **TR1** | Wire-conformance pinning is new and isolated | **ADVISORY** | An observation about test practice with no specific defect. Its suggestions (pin BAND_*_ACK / REG_ACK key sets) are candidate work, not findings. |
| **TR2** | Tests pin HubState well, role-side poorly | **ADVISORY** | Same shape. Superseded in spirit by the role-side pins added since. |
| **TR3** | Mutation-sweep coverage uneven | **ADVISORY, and now house practice** | The finding recommends a mutation comment on new tests. That is now the norm — every security-relevant test added in #83/#95/#96/#98 was mutation-checked and says so. |
| **O1a** | `BrokerRequestComm::send_notify` dead | **RESOLVED** | Deleted; only the removal notes remain (`broker_request_comm.hpp:186`, `.cpp:1128`). |
| **O1b** | `BrokerRequestComm::query_shm_info` dead | **VALID** | Still declared (`broker_request_comm.hpp:429`) and defined (`.cpp:1491`) with **zero** callers in `src/` or `tests/`. See the caution below before deleting. |
| **O2** | Stale "Wave-B M4d/e/f" migration labels | **VALID** | 6 occurrences remain in `role_api_base.cpp`. Also a standing house rule: phase labels do not belong in comments — task IDs are fine, migration-wave prefixes are not. |
| **O3** | `resolve_bc_for_{channel,role,band}` are pure forwarders | **VALID (factually)** | Confirmed one-line forwarders (`role_api_base.cpp:498-501`). Whether to inline ~30 call sites is a judgement call, not a defect — the wrappers do read as a stable seam. |
| **O4** | `Shared::producer_channel`/`consumer_channel` duplicate presences | **RESOLVED** | Both fields deleted; same rework as S1. |
| **R2.5** | `IncomingMessage` lacks `source_hub_uid` | **RESOLVED** | The field exists and is live — `invoke_user_hub_dead` reads it for dual-hub attribution. |

**Caution on O1b.** `query_shm_info` is dead *today*, but band 2 (the broker
SHM observer, HEP-0045) is about to build exactly this capability. Deleting it
now and rebuilding it in three weeks is worse than leaving it. Decide it as
part of that work, not as a dead-code sweep.

---

## `REVIEW_CURVE_Integration_2026-07-19`

### Real code cleanups — all five RESOLVED

Every item in this block has been done, none was marked.

| # | Cleanup | Evidence |
|---|---|---|
| 1 | `arm_curve_server` used by only one of three arm sites | **RESOLVED** — all three now call the helper: `admin_service.cpp:175`, `hub_inbox_queue.cpp:442`, `broker_service.cpp:1054`. This is exactly the "real dedup" the finding asked for, and it means the `curve_socket.hpp` docblock the reviewer called an over-claim is now simply true. |
| 2 | Two bare `§7.4` references | **RESOLVED** — the surviving reference is qualified (`HEP-CORE-0036 §7.4 single-pumper invariant`); no bare form remains at either cited site. |
| 3 | A third local hex encoder | **RESOLVED** — `bytes_to_hex` lives in `format_tools.hpp:91,186` and callers go through `format_tools::`. No local copy in `wire_envelope.cpp`. |
| 4 | Two stale `SeckeyAccessor` comments | **RESOLVED** — the symbol appears nowhere in `src/`. |
| 5 | HEP-0033 §11.0.2 admin "message form" table | **NOT VALIDATED** — doc-only; folded into the backlog below. |

### Real design gaps — the reviewer's own framing was right

These were filed as "track, don't fix inline", and that judgement holds.

- **Inbox has no replay defense** — **RESOLVED.** Shipped as the shared
  `ReplayGuard` plus a 24-byte nonce/wall-ts frame (HEP-0027 §3.6, task #64);
  `hub_inbox_queue.cpp` references the guard six times. A follow-on (#67) then
  fixed the guard's own clock source.
- **Observer initiator send-side not implemented** — still open, and it is
  precisely band 2 (HEP-0045). Not a stray finding; it is the next feature.
- **Admin in-session replay window**, **post-`SCM_RIGHTS` consumer
  revocation**, **online key rotation / per-key revocation** — all still
  unspecified and unimplemented. These are genuine design gaps rather than
  defects, and none is scheduled. They belong with #89 (SMS/vault design)
  when that opens.

### Doc reconciliation backlog — partially overtaken

Eleven doc items. Task #72 (HEP↔code reconciliation) already swept several,
and `known_roles` storage was settled by the vault hard cutover (#63) — the
backlog entry still says "reconcile to `known_roles.json`", which is now the
**opposite** of the shipped design, where the encrypted vault is
authoritative. That entry is not just stale, it is actively wrong and would
mislead anyone who worked it.

The remaining items need a doc-by-doc pass, which is a different kind of work
from this one (reading HEPs against each other rather than against code) and
is not attempted here.

---

## Spot-validation of findings already marked ✅ FIXED

52 resolution notes exist across `REVIEW_FullSystem`; every one is
self-reported and none had been independently checked.  Verified by reading
the code and the governing contract, **not** by confirming a symbol exists —
a symbol's presence says nothing about whether it is reached or whether it
does what the HEP requires.

Triaged by stakes.  Depth is recorded per finding so this section cannot be
read as more confident than it is.

### `[high/risk-error]` shared ReplayGuard with an attacker-controllable clock (#67) — **CLAIM HOLDS**

Reviewed in full against HEP-0027 §3.6 (I-REPLAY-BOUND).  Four independent
properties, three of them stronger than the finding asked for:

1. **The attack is structurally impossible, not merely avoided.**
   `check_and_record(identity, nonce, window_ms)` takes **no timestamp
   parameter at all** (`replay_guard.hpp:85`).  The reference time is read
   from `clock_()` inside the lock.  A caller cannot supply the reference
   time even by mistake — which is what the HEP means by "deliberately no
   per-call timestamp argument".
2. **The one seam is genuinely test-only.**  The `ClockFn` constructor is the
   sole way to influence the clock; grepping every construction site in
   `src/` outside the header returns **nothing**, so production never injects
   one.  The header's claim that "production never points at client input" is
   a fact, not an aspiration.
3. **The window ≥ 2 × skew invariant holds on all three planes.**  Inbox:
   skew 30 s, window `2 * kInboxReplaySkewMs`.  Admin: skew 30 s, window
   `2 * kReplaySkewMs`.  REG: skew 30 s, window 60 s.
4. **Fail-closed beyond the finding.**  Empty identity or nonce returns
   `false` — a caller that cannot name the sender is rejected rather than
   admitted.  Nothing required this; it is the right default.

### NEW — the REG plane states its replay invariant in a comment; the other two make it structural

Found while verifying property 3 above; **not** part of the original finding.

The inbox and admin planes derive the window from the skew:

    kInboxReplayWindowMs = 2 * kInboxReplaySkewMs;   // hub_inbox_queue.cpp:190
    kReplayWindowMs      = 2 * kReplaySkewMs;        // admin_service.cpp:363

The REG plane sets two independent literals and explains the relationship in
a comment (`broker_service.cpp:7062-7071`):

    context.skew_tolerance_ms = 30'000ULL;
    // I-REPLAY-BOUND ... nonce_window_ms MUST be >= 2 * skew_tolerance_ms
    context.nonce_window_ms   = 60'000ULL;

The values are correct today.  But raising `skew_tolerance_ms` to 45 s
silently breaks I-REPLAY-BOUND — window becomes 60 s where 90 s is required,
a late-but-skew-valid replay finds its nonce already pruned, and it is
admitted.  No compile error, no test failure, and the comment that states the
rule sits two lines above the literal that violates it.

This is the codebase's own "make it structural, not conventional" principle,
applied unevenly across three planes that share one invariant.  Fix is one
line: derive the window from the skew as the other two do.

**Severity: low today, latent.**  Nothing is wrong now; the guard rail is
missing, not the behaviour.

### `[high/risk-error]` four dead/no-op identity validators (#68) — **CLAIM SUBSTANTIALLY HOLDS**

The note claims four validators across three subsystems are "gone or armed".
Two sub-claims verified in full, one corroborated, one unchecked.

**Sub-claim 1 — the schema-citation validator runs on every joiner path
"including the consumer one": VERIFIED, and this is the one that mattered.**
The original defect was that the consumer path skipped the check. Four
production call sites of `HubState::_validate_schema_citation` exist, and the
question is not their count but *which handlers* contain them. Handler
boundaries: `handle_reg_req` (before 2956) holds the calls at 2340 and 2510;
`handle_consumer_reg_req` spans 3239-4073 and holds the calls at 3680 and
3734. So the producer path has two and **the consumer path has two** — the
gap is genuinely closed. The reject counter
`schema_citation_rejected_total` is pinned 11 times in `test_hub_state.cpp`,
so a validator that silently stopped rejecting would fail tests.

*Method note.* A first pass attributed 3680/3734 to `handle_dereg_req` — a
leave path, which would have made the "including the consumer one" claim
false — because the enclosing-function pattern missed a signature split
across two lines (`nlohmann::json` on one, `BrokerServiceImpl::handle_...` on
the next). That near-miss is the argument for reading boundaries rather than
counting matches: a grep total of "four call sites" is compatible with the
claim being true *or* false, and only the boundaries decide which.

**Sub-claim 2 — the `RoleIdentityPolicy` string gate deleted: VERIFIED.**
`effective_role_identity_policy` has zero occurrences anywhere in `src/`, and
the two surviving mentions of `RoleIdentityPolicy` / `check_role_identity`
are inside comments explaining the removal.

**Sub-claim 3 — no separate key-rotation gate: CORROBORATED, not
independently proven.** `broker_service.cpp:7035-7038` states the rule (a
re-REG under a different pubkey is refused as `PUBKEY_MISMATCH` by
`check_known_role_binding`) and that no separate gate exists. Consistent with
the note; the rejection path itself was not exercised here.

**Sub-claim 4 — stale-SHM `producer_uid` cross-check retired: NOT CHECKED.**

### NEW (minor) — `role_identity_policy.hpp` is a repurposed file with a stale name and docblock

The file survives at 55 lines, but it no longer defines a role-identity
policy: its `@brief` says it defines `KnownRole`, the live vault-backed ZAP
pubkey carrier that #68 deliberately kept. Lines 8-9 still narrate the
deleted `RoleIdentityPolicy` enum as though it were the file's subject.

So a reader looking for the deleted gate finds a file named after it, and a
reader looking for `KnownRole` has no reason to open it. Rename to
`known_roles`-adjacent and trim the docblock to what the file now contains.
**Cosmetic, zero behavioural risk** — but it is precisely the
"migration residue that actively misleads" class this review already flagged
elsewhere.

### Depth actually achieved

| Finding | Depth |
|---|---|
| ReplayGuard clock (#67) | **Full** — code + HEP-0027 §3.6 + all three call sites |
| Dead identity validators (#68) | **Substantial** — 2 of 4 sub-claims full, 1 corroborated, 1 unchecked |
| The other 50 ✅ notes | **NOT VERIFIED** |

50 resolution notes remain unchecked, including the checksum-contract drift
and the resource-cleanup findings.  Both notes verified so far have held,
which is mild evidence the resolution notes are honest — but two out of
fifty-two is not a basis for trusting the rest.

---

## Fixes applied 2026-08-02

Three of the six candidates were fixed.  Checking the other three before
touching them changed two of the verdicts — which is the whole point of
validating before scheduling.

| Candidate | Action | Note |
|---|---|---|
| REG plane restates the replay invariant | **FIXED** | `nonce_window_ms` is now `2 * skew_tolerance_ms`, matching the inbox and admin planes.  The drift path is closed structurally. |
| Six stale `Wave-B M4d/e/f` labels | **FIXED** | Rewritten to say what the code does, keeping the dates.  Migration-wave prefixes are meaningless to anyone who did not live through the wave. |
| `to_channel_side()` in three files | **FIXED** | One `inline` definition in `scripting/json_py_helpers.hpp` — the header where the other shared py-argument conversions already live — and the three file-static copies deleted. |
| **T1** counter naming | **FIXED 2026-08-03 — after a verdict correction that was itself wrong** | See "T1 — how this row went wrong twice" below.  The counters are now `connected_to_pending_total` / `pending_to_connected_total` / `pending_to_disconnected_total`, matching the §2.1 state names. |
| **B-1** `should_continue_loop` / `should_exit_inner` | **NOT FIXED — deliberately** | The choice is adopt-or-delete, and both are wrong to make casually.  Adopting means rewriting the loop condition in three role hosts; a subtly different condition there is a hang or a premature exit, not a compile error.  Deleting throws away the right abstraction weeks before band 4 (role-host unification) is going to want exactly it.  Best done inside band 4, where the three loops are being collapsed anyway. |
| **O1b** `query_shm_info` | **NOT FIXED — as recorded** | Dead today, but band 2 is about to build this capability.  Settle it there. |
| **O3** three forwarders | **NOT FIXED — as recorded** | Factually pure forwarders, but inlining 30 call sites to delete a one-line seam is a judgement call, not a defect. |

## T1 — how this row went wrong twice

Worth recording, because both errors are repeatable.

**First error (the May review).** It classified the Ready/Connected
naming drift as cosmetic. It is not: the counters are emitted as JSON
keys in the broker's metrics blob, so a rename reaches anything reading
that output.

**Second error (this validation, 2026-08-02).** Having caught that, I
recorded T1 as an unmade owner decision and took it to the owner as one.
It was not unmade. `HEP-CORE-0023` §2.5 had already decided it in
writing — keep the legacy spelling for now, rename in a future cleanup —
and gave the reason: backward compatibility with test fixtures and
"production log scrapers". The May review closed T1 deliberately on that
basis and harmonized the comments instead.

So I re-derived from scratch a decision the governing HEP had already
made, and presented the result as a new question. The rule that would
have caught it is the one already in `CLAUDE.md`: refresh against the
doc at the moment of starting work. A finding that cites a HEP section
is a prompt to open that section, not a summary of it.

**Resolution (2026-08-03).** Owner confirmed no log scrapers exist —
the codebase is the consumer. That voids the HEP's stated justification,
so the deferral ended and the rename shipped. HEP-0023 §2.5 and
HEP-0033 §9.4 were updated first, then the code. `ready_timeout` and
`ready_miss_heartbeats` keep their spelling: they are configuration
keys, and renaming them would change a user's config file — a different
surface with a different answer.

The lasting lesson is narrower than "read the HEP". It is: when a
deferral is justified by a fact about the outside world ("scrapers
exist"), the fact needs an owner to confirm it, and it should be
re-confirmed before the deferral is quoted as settled. A justification
nobody has checked in a year is not a decision, it is an assumption
wearing one.

## Carried forward

Three findings survive validation and are genuinely open, all LOW.
None is scheduled — they go to the owner as candidates, not as plan.

- **B-1** — delete `should_continue_loop()` / `should_exit_inner()` **and**
  their tests, or adopt them in the three role hosts. Do not leave them
  tested-but-uncalled. Best done inside band 4, which collapses those
  three loops anyway.
- **O1b** — `query_shm_info` is uncalled, but see the caution above: settle it
  inside band 2, not before.
- **O3** — the three forwarders are real, but inlining 30 call sites to remove
  a one-line seam is a judgement call, not a defect. Recorded, not
  recommended.

Closed since this record was opened: **C-1** (one `to_channel_side()`),
**O2** (six stale labels), the **REG-plane replay-window derivation**,
and **T1** (counter rename).

## Not yet done

- `REVIEW_CURVE_Integration` doc-reconciliation backlog — 11 items needing a
  HEP-against-HEP pass.  One of them (`known_roles` storage) is known to be
  inverted relative to the shipped design; the rest are unchecked.
- `REVIEW_FullModule` F-1, F-2, F-4 — counting exercises against a suite that
  has roughly doubled; the cited numbers carry no meaning until re-derived.
- `REVIEW_Connection_Inbox_Band` S3 — the only finding in that review left
  unvalidated; needs a read of the current `start_handler_threads` phase
  sequence.
- Spot-validation of the 52 findings in `REVIEW_FullSystem` already marked
  ✅ FIXED. Lower risk than the open ones, but the resolution notes are
  self-reported and none has been independently checked.
