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
| `REVIEW_Connection_Inbox_Band_2026-05-17.md` | 24 blocks (~14 real findings) | ~14 | ⏳ not started |
| `REVIEW_CURVE_Integration_2026-07-19.md` | 3 category blocks | 3 | ⏳ not started |
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

## Carried forward

Two findings survive validation and are genuinely open, both LOW:

- **B-1** — delete `should_continue_loop()` / `should_exit_inner()` **and**
  their tests, or adopt them in the three role hosts. Do not leave them
  tested-but-uncalled.
- **C-1** — one `to_channel_side()`, not three.

Neither is scheduled. They go to the owner as candidates, not as plan.

## Not yet done

- `REVIEW_Connection_Inbox_Band_2026-05-17` — S1–S4, T1–T3, TR1–TR3, O1–O4
  (~14 findings, 2.5 months old, span role-side state, counter naming, and
  dead BRC methods).
- `REVIEW_CURVE_Integration_2026-07-19` — three category blocks
  ("real code cleanups", "real design gaps", "doc reconciliation backlog").
- Spot-validation of the 52 findings in `REVIEW_FullSystem` already marked
  ✅ FIXED. Lower risk than the open ones, but the resolution notes are
  self-reported and none has been independently checked.
