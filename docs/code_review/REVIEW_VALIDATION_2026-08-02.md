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
| **T1** | Counters say "Ready", state says "Connected" | **VALID (cosmetic)** | `ready_to_pending_total` survives at `hub_state.hpp:1498` while `RoleState` is `{Connected, Pending, Disconnected}`. Harmless but the two names describe one transition. |
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

## Carried forward

Two findings survive validation and are genuinely open, both LOW:

- **B-1** — delete `should_continue_loop()` / `should_exit_inner()` **and**
  their tests, or adopt them in the three role hosts. Do not leave them
  tested-but-uncalled.
- **C-1** — one `to_channel_side()`, not three.
- **O2** — strip the 6 `Wave-B M4d/e/f` labels from `role_api_base.cpp`.
- **T1** — `ready_to_pending_total` / `pending_to_ready_total` name a state
  the enum no longer calls "Ready".
- **O1b** — `query_shm_info` is uncalled, but see the caution above: settle it
  inside band 2, not before.
- **O3** — the three forwarders are real, but inlining 30 call sites to remove
  a one-line seam is a judgement call, not a defect. Recorded, not
  recommended.

None is scheduled. They go to the owner as candidates, not as plan.

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
