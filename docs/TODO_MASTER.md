# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current status, pointers to subtopic
detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1: **keep this concise
(≤ 200 lines)** — detailed task tracking lives in `docs/todo/<area>_TODO.md`;
git is the historical record.  Completed-phase narrative extracted to
`docs/archive/transient-2026-07-{18,22}/todo-completions/TODO_MASTER_completions_2026-07-{18,22}.md`
(07-18 verbatim pre-compression text at commit `633d51c0`; 07-22 = the
post-reconcile shipped-sprint detail).

---

## Current status (2026-08-03)

> **Shipped-work narrative lives in git and in
> `archive/transient-2026-07-22/todo-completions/`.**  Task-list validations
> (2026-08-03, 2026-08-07) are recorded in `DOC_ARCHIVE_LOG.md`; what they
> found that is still OPEN is in the next-actions table below.

**Closed lines** (detail in the completions index above):
- **Line 1 — CURVE auth chain:** 🟢 Phase 1 production-ready (REVIEW-E); + vault
  `known_roles` (HEP-0035 §4.8) + inbox replay defense (HEP-0027 §3.6).
- **Line E — Admin-plane CURVE:** ✅ shipped 2026-07-19 (was the #1 open security
  surface), including the typed console and the polled output buffer that
  replaced the push reverse-notify path.  Residual: #105 `origin_uid` cascade,
  three L3 stubs under #52.  (#103 anti-hijack pin ✅ closed 2026-08-07.)
- **Inbox:** ✅ CURVE + cross-engine parity + replay + schema two-zone.
- **Line 2 — SMS (HEP-0043):** ✅ shipped.  Residual: SEC-Fold-1b §8/§10 vault +
  script-crypto content migration (housekeeping).
- **Line 4 — IAttachChannel (HEP-0044):** ✅ shipped.

**Open lines:**
- **Line 3 — Broker SHM observer (HEP-0045):** ⛔ **RETIRED 2026-08-07**, not
  deferred.  The role already holds the SHM counters and already ships
  `QueueMetrics` to the hub on every heartbeat — joining them (**#117**) does
  what the observer was built to do, and retiring it removes a privilege
  surface (the broker stops mapping other processes' memory).  Shipped pieces
  come back out; the ephemeral-keypair mechanism is kept and abstracted
  (**#119**).  Reasoning in HEP-CORE-0045 § "Retirement notice".
- **Topology migration:** STATIC layer ✅ + live (topology factories, role-code
  migration C step 6 shipped `3d4fe07a`, multi-producer fan-in DATA plane
  proven green at L4 — code-verified 2026-07-25).  **DYNAMIC layer ✅
  CODE-COMPLETE 2026-07-26 (T3): S1 owner-locked `ChannelEntry` +
  `AWAITING_OWNER` role-host retry, S2 owner-death teardown, S3 dialer
  fast-fail shipped as one unit** (S4 peer-join callbacks 2026-07-25; S5
  objective counts = task #74).  L3 fan-in wire tests flipped
  consumer-first (C step 7 L3 half).  **The R6 broker-pends gate is
  RETIRED 2026-07-25 — do NOT build** (superseded by the owner-first
  contract).  Remaining: Phase E retirements (T4, unblocked) + Phase F
  demos incl. the L4 producer-first-spawn keystone (T5).  Detail:
  `TOPOLOGY_TODO.md`; permanent design = HEP-CORE-0017 §4.7 (draft retires
  with Phase F).
- **REG protocol redesign (HEP-0046):** ✅ **Phase B COMPLETE 2026-07-24**
  (task #57): all nine REG-family handlers typed; `to_legacy` bridge +
  `BrokerRegHandler` skeleton retired; `inbox_schema_json` → typed
  `SchemaSpec` boundary-parse (separate `inbox_packing` wire field
  retired); B.3 audit found the BRC/ACK flip already landed via the
  envelope/adapter arcs; `receive_and_validate` drift-guard L1 suite
  landed.  Residual REG-adjacent tracks: EnvelopeOnly-tier body classes
  (per-msg_type follow-ons; the #72 pass itself COMPLETED 2026-07-24 —
  wire-inventory audit 2026-07-26 names the uncovered inbound-notify
  bodies: CHANNEL_COUNT_NOTIFY, CHANNEL_EVENT_NOTIFY,
  CHANNEL_ERROR_NOTIFY, CHANNEL_BROADCAST_DELIVER_NOTIFY,
  BAND_BROADCAST_DELIVER_NOTIFY — each needs a wire_bodies class + a
  BRC shape-table arm).  Federation ingress is parked with #69.
- **Full-system audit (`REVIEW_FullSystem_2026-07-20`):** ✅ 56 findings, **52
  resolved / 4 open** after the #72 reconciliation pass (2026-07-24) closed the
  hep-gap + dead-residue clusters.  Every ✅ note independently verified against
  source 2026-08-07 — all held; three residual doc drifts filed as #110.  The three test-coverage items (inbox-worker
  magic/gap pins, hub_vault known_roles L2 round-trip, logger StressLog
  diagnostic) were validated and closed 2026-08-02 — see band 1.  The fourth —
  federation ingress bypass — is parked with #69 and is not counted as open
  work.

---

## Active / next work

> **RANKED PRIORITY — set by the project owner 2026-07-27.**  Work the
> bands in order; inside a band, use leverage.  This ordering overrides
> the "roughly by leverage" heuristic that governed this section before.
>
> | # | Band | What it covers |
> |---|---|---|
> | **1** | **Security items** | **Open:** #89 SMS/vault script surface; #87 privilege audit (now also carries the catch-block re-sweep and, post-observer-retirement, the by-name `collect_shm_info` read); **#121 SEC-Fold** (one module owns libsodium, one HEP owns security); **#122 CTRL ZAP allow-path pin** (the deny pin alone would pass if the broker denied everyone); **#123 role vault `.pub`** (missing production surface that an L4 test currently reaches around).  **#103 admin anti-hijack pin ✅ closed 2026-08-07** — mutation-verified, no production surface added.  Vault hot reload is **CLOSED — decided against 2026-08-07** (#104): restart is the revocation boundary.  The impersonation arc (#83/#95/#96 + inbox sender) is CLOSED on every plane.  **Federation is NOT in this band.** |
> | **2** | ~~**Shared-memory observer feature**~~ — **RETIRED 2026-08-07** | HEP-CORE-0045 is ⛔ retired, not deferred.  The role already holds the SHM counters and already ships `QueueMetrics` to the hub on every heartbeat; joining them is **#117** (seven fields, one X-macro).  Retirement also removes a privilege surface — the broker stops mapping other processes' memory.  Residual work is **#117** → **#119** (abstract the ephemeral grant) → removal of the shipped observer pieces.  **This band is otherwise empty; the ordering below should be re-read with that in mind.** |
> | **3** | **Backlog of smaller polish items** | The P0/P1 batches below (startup log lines, config defaults, per-area subtopic items) — small, independently shippable. |
> | **4** | **Role-program unification (C++ RAII framework)** | #292 collapse of the three role-host files, taken together with the Template-RAII layer (Phase 2b: `TypedInboxClient`, `SimpleRoleHost`) since both reshape the same surface; #55 test re-homing rides along. |
> | **5** | **The rest** | Topology T4/T5 residuals, the Pattern-4 test migration (#52), Windows/CI coverage, and everything else in "Open work by area". |

#### The query layer after the observer retirement (2026-08-07)

The coupling recorded earlier this day — band 2's `collect_shm_info` rewrite
feeding HEP-CORE-0039's `list_shm_blocks` / `get_shm_block` — **dissolved when
the observer was retired.**  What remains is simpler and worth stating so the
old note is not re-derived:

- `collect_shm_info` is no longer being extended.  It is on the way **out**,
  along with the broker's by-name SHM read; the hub gets the same numbers from
  the heartbeat once **#117** lands.
- HEP-CORE-0039's shm query functions were specified (§349-350, §402-403) and
  never built.  With the collector retiring, they should read from `HubState`
  — which the heartbeat already populates — matching the snapshot-based
  signature the HEP gave them in the first place.
- `QUERY_LAYER_TODO`'s retirement row for `query_shm_info` /
  `collect_shm_info_json` is therefore **unblocked**: **#112**'s broker half
  becomes a straight deletion rather than something waiting on band 2.

HEP-0039 still has no band of its own — it appears once, as a clause under
"MessageHub / broker protocol".  With band 2 empty it is the natural candidate
to take that slot, but that is an owner call, not an assumption.

#### Where the 2026-08-07 cleanup findings land

Ten tasks came out of the 2026-08-07 review-verification, TODO cleanup, and
design passes.  Slotted using the band definitions above — **these are
placements, not priority decisions**; move any of them if the band reading is
wrong.  **All of these are decided and actionable** — the owner has ruled on
every open question from the 2026-08-07 session.

| Task | Band | Why there |
|---|---|---|
| **#113** I-CORRELATION-STABLE vs code | **3 — DECIDED, ready** | **Ruled 2026-08-07: the doc is the sole source of truth; the code aligns to it.** Change `pending_requests` to key on `(msg_type, correlation_id)`. The safety argument for id-only does not exist — there is no msg_type check anywhere on the reply path, so type confusion is currently possible and undetected. Also revert the #72 comment edit that justified id-only; it aligned prose to code, the wrong direction. |
| **#111** multi-presence half-built (C5/B4/X5/S4) | **4** | Same surface as #292 role-host collapse — the structures already carry the topology, the operations still take `presences[0]`. Doing it inside band 4 costs little; doing it alone means touching the same files twice. |
| **#112** dead `query_shm_info` + phase labels | **3 — unblocked** | Both halves are now straight deletions. *(This row used to say "check band 2 first, the observer may want that surface." Band 2 was retired later the same day — see the query-layer note above. Nothing is waiting on it.)* |
| **#115** retire `ChannelSnapshotEntry` | **3** | Gate already satisfied — zero test references. Smallest real item on the list. |
| **#110** three doc sites naming retired mechanisms | **3** | Doc-only. Fold the `as_peer_allowlist` comment fixes into the `role_identity_policy.hpp` → `known_role.hpp` rename rather than doing them twice. |
| **#114** regenerate clang-tidy, re-base the lint plan | **3** | Prerequisite for the whole lint backlog; the current plan cannot be actioned. Recipe is in `PLATFORM_TODO.md`. |
| **#116** system lifecycle: hub-initiated shutdown + cold start | **1 — DIRECTED, design-first** | **Owner-directed 2026-08-07** in place of vault hot reload: build hub-initiated system shutdown, and design cold start / deployment for a hub+role network. Restart is now the revocation boundary, so restart must be orchestrated. Today `ADMIN_REQUEST_SHUTDOWN_REQ` stops one process and tells no role anything; roles infer death from a dead socket, so orderly shutdown and crash are indistinguishable; `StopReason` cannot express "planned"; roles never reconnect. Deliverable is a design proposal following "framework is mechanism, not policy" — planned-shutdown notify, readiness marker, honest exit codes; start order and retry belong to the supervisor. Likely home: HEP-0023, which owns the role-side sequence and is missing both bookends. |
| **#117** plumb SHM aggregates into `QueueMetrics` | **2 → now the whole of it** | Seven fields, one X-macro, feeds `api.metrics()` and the heartbeat at once.  Design fully settled; unblocked. |
| **#118** `SharedMemoryHeader` field grouping | **3** (static review) | Does the struct group fields by access pattern and say so in source?  Review + written finding FIRST — it is a core structure, so any change triggers the Core Structure Change Protocol.  Explicitly **not** a benchmark. |
| **#119** ephemeral capability grant abstraction | **before the observer removal** | The kept mechanism needs a home.  Must be a *sibling* to `PubkeyOrigin`, never a third `Kind` — that enum governs which identities a key may speak for, and this key speaks for none. |

#### Where the AUTH_TODO cleanup findings land (2026-08-07, later the same day)

`AUTH_TODO.md` went 850 → ~190 lines, open items only; closure evidence in
`archive/transient-2026-08-07/todo-completions/AUTH_TODO_closed_2026-08-07.md`.
Six claimed-open items proved already closed (the retired reverse-notify path,
the 7 masked `RoleIdentityPolicy` tests deleted in `c7f4f608`, the #275 S2
worker scan, native `allowed_peers`/`producers` parity, the §11.0.4
fire-and-forget tension, and vault hot reload).  **Three of the survivors are
security items, so this pass did add to band 1** — unlike the docs pass above.

| Task | Band | Why there |
|---|---|---|
| **#121** SEC-Fold — one module owns libsodium, one HEP owns security | **1** | Filed after a `sodium_init()` CI failure; the stopgap fix is still the only thing holding.  Scope has shrunk since filing: 7 files include `<sodium.h>` and 5 already sit under `security/`, so re-scope before planning commits.  Docs half first.  Absorbs the old script-crypto item. |
| **#122** CTRL ZAP allow-path pin | **1** | `CtrlZapDenyPath` exists; nothing pins that a *known* key is admitted.  A deny-only pin passes just as well if the broker denies everyone. |
| **#123** role vault publishes no `.pub` | **1** | The documented operator workflow cannot be followed, and the L4 roundtrip test opens the vault programmatically to compensate — a test reaching around a missing production surface. |
| **#127** CLI `--init` bundling | **3** | What makes the shipped auth usable by someone who is not the author.  Natural home for #123 and #124. |
| **#124** 27 demo configs ship `"keyfile": ""` | **3** | Broken since strict CURVE landed in May 2026.  Blocked by #123 + #127 — do it as one wave, with the configs as tool output rather than hand-maintained fixtures. |
| **#125** CURVE-review doc backlog, 11 items | **3** | Doc-only.  One (`known_roles` storage) is known to describe the pre-vault model that was hard-cut-over; do that one first. |
| **#126** HEP-0041 macOS + Windows backends | **5** | SHM channel auth is Linux-only.  Gated on Windows CI existing at all. |
| **#134** test-suite hygiene — Pattern-4 timing discipline | **5 — owner-lowered 2026-08-07** | **Consolidated:** the sleep-to-order in the shared wire base (9 files inherit it), the duplicated helper, the tight Phase 2.4b budget, and the hand-rolled poll loop are ONE item, deliberately parked below product work.  Nothing in it is a product defect and the suite is green.  Two things kept with it so they are not re-derived: Pattern 4 as an *architecture* is the remedy, not the problem — the defect is one helper inside it; and the verification when it runs is a stress run before/after, **not** a green run, since the suite is green today with the defect live. |

#### Where the MESSAGEHUB / API / TOPOLOGY sweep findings land (2026-08-07)

| Task | Band | Why there |
|---|---|---|
| **#130** `expected_schema_owner` — a wire field no canonical schema declares | **3** | Live in three places (`wire_bodies.hpp:402`, `wire_bodies.cpp:293`, `broker_service.cpp:3632`), absent from HEP-0036 §5b.6, and production never sends it — only test helpers do.  Canonicalize under one name or delete the accessor + read.  A field reachable on the wire that no schema owns is what the typed envelope exists to prevent. |
| **#131** five untyped inbound-notify bodies | **3** | Mechanical and uniform once the first is done; the nine REG-family conversions set the pattern.  Do with **#82**'s typed Schema/Metrics bodies — one mechanism. |
| **#132** `with_active_loop` tested and uncalled | **4** | See the theme above; band 4 touches the thread owners. |
| **#133** dynamic peer API tested and uncalled | **4** | Same theme.  Also unblocks a split in topology Phase E, which had it in one deletion row with the load-bearing multi-endpoint PULL loop. |

**And one gate that turned out to be already open:** **#89** (script vault,
band 1) recorded "depends on #104 shipping first". That chain
(`#101 + #102 → #74 → #94 + #103 → #104 → #106`) is fully closed, so #89 has
been sitting behind a satisfied dependency. Still genuinely unbuilt —
`api.vault_save` / `api.vault_load` appear nowhere in `src/`, HEP-0038 is
still 🚧 DRAFT. Sequence it against **#121**: doing the security-module fold
first means the script surface lands on the consolidated shape.

#### A theme worth acting on as one decision: abstractions landing ahead of their adopters

The tracker sweep turned up **three independent abstractions that are defined,
have real test coverage, and have zero production callers.** Not one-offs —
a pattern in how work lands here, and one that normal review misses, because
**the tests are what hide them**: a reviewer skimming for dead code finds
coverage and moves on.

| Symbol | Where | Tests | Live task |
|---|---|---|---|
| `should_continue_loop()` / `should_exit_inner()` | `role_host_core.hpp:516,525` | `test_role_host_core.cpp` | band-4 item in `API_TODO` |
| `with_active_loop()` — HEP-0031 §4.1 shutdown contract | `thread_manager.hpp:92` | whole L2 binary `test_thread_manager_active_loop.cpp` | **#132** |
| `set_producer_peers` / `add_producer_peer` / `remove_producer_peer` | `hub_zmq_queue.hpp:461,466,472` | `test_hub_zmq_queue.cpp:498-545` | **#133** |

Each is the same adopt-or-delete judgement, and each has a real consequence
behind it — an unadopted shutdown contract means shutdown races a critical
region; an undriven peer API means a producer joining mid-channel never
reaches a dialing consumer. **Decide them together.** Answering one in
isolation risks three inconsistent answers to one question, and two of the
three already sit inside band 4, which collapses the surfaces involved.

The generalisable check, now part of the sweep method: **grep for zero-caller
symbols, and do not let test coverage stand in for adoption.**

### Next actions — code-verified 2026-08-07

Each row was checked against source in this pass.  Nothing here rests on a
`✅` marker, a commit message, or a comment.

| Next | Why now | Evidence checked |
|---|---|---|
| ~~**Vault hot reload**~~ → **#116 system lifecycle** | **Superseded within the same day.**  This row led the table as "the biggest open security item"; the owner then **decided against hot reload** (#104) — hot-managing auth invites inconsistency and holes, so restart *is* the revocation boundary.  What replaced it: if restart is the revocation boundary, restart has to be something the system can actually perform.  Today `ADMIN_REQUEST_SHUTDOWN_REQ` stops one process and tells no role anything.  **Design-first, per #116.** | The original evidence still stands and is now the argument *for* the replacement: no reload method exists anywhere, and no orderly system-restart path exists either. |
| ~~**#103 — admin anti-hijack L2 test**~~ **✅ DONE 2026-08-07** | Was the last thing standing between #83 and closed.  Landed as `Console_SessionIdFromAnotherConnection_Rejected`, mutation-verified against `admin_session.cpp:149`. | Prediction held: the L2 fixture already minted consoles with distinct routing ids, so no new harness was needed and no production surface was added.  Limit recorded in `AUTH_TODO.md` — over loopback the routing id is the only discriminating fact. |
| **`origin_uid`: build the scoped origin, or amend §11.0.5** | A doc-vs-code split, and the doc currently promises the larger thing.  Owner's call which way it resolves. | `origin_uid` is hand-threaded into two queue records and emitted on two notifies (`broker_service.cpp:1434`, `:1472`).  The scoped "current actuation origin" §11.0.5 describes — inherited automatically by every log line and NOTIFY in a teardown cascade — does not exist anywhere in `src/`. |
| **#98 — `channel_broadcast` three-engine parity test** | A shipped feature whose per-engine bindings are compile-verified only, against an explicit project rule that each engine is verified directly. | The four L2 dispatcher tests drive a **fake** engine: `test_dispatch_notifications.cpp` defines its own `invoke_on_channel_broadcast` that records calls.  No real Lua/Python/Native binding is exercised anywhere. |
| **Topology Phase E retirement** (T4) | Unblocked since 2026-07-25; the legacy message is still carried. | `CONSUMER_ATTACH_REQ_ZMQ` live in 4 files / 10 references (`broker_service.cpp` ×7, `wire_dispatch.hpp`, `broker_request_comm.cpp`, `plh_version_registry.hpp`). |

**Deliberately not promoted:** #102 (LOW — three branches, two of them the
same loop body at a different index; explicitly *not* via two hub
processes), and #87 / #89, which were not re-verified in this pass and
should be re-scoped against code before anyone starts them.


> **Review findings are not the plan.**  A finding produced by a review pass
> is a CLAIM until someone validates it against code and the owner accepts it.
> Two rules, both learned the hard way on 2026-08-02:
>
> 1. **Findings live in `docs/code_review/REVIEW_*.md` until triaged.**  They
>    do not get copied into this file as work.  This file carries what the
>    owner decided; mixing the two makes a robot's suggestion indistinguishable
>    from a decision, and the suggestion then gets worked on first.
> 2. **Validate before scheduling, not after.**  For any finding of the form
>    "X is untested" or "Y is unchecked": find where X is implemented, find
>    whether that place is already covered, and read the design doc that says
>    what X is supposed to do.  Of the three findings that had sat at the head
>    of band 1 for two weeks, two evaporated under one grep each.

### Detail (open, within the bands above)

**Security (top open surface):**
- **Dead/no-op identity + authority validators (#68) — ✅ CLOSED 2026-07-21,
  re-verified against code 2026-07-27.**  All four are gone or armed: the
  schema-citation validator runs on every joiner path including the consumer
  one (four production call sites in `broker_service.cpp`, reject-counter
  increments pinned in `test_hub_state.cpp`); the key-rotation gate and the
  `RoleIdentityPolicy` string gate were deleted as redundant with the ZAP
  pubkey allowlist; the stale-SHM `producer_uid` cross-check was retired as
  dead wire, superseded by capability-fd attach (HEP-CORE-0013 banner).
  The task sat `pending` after the fix landed — closed on verification, not
  on the tracker's word.
- **"Three security-adjacent coverage gaps" — VALIDATED AGAINST CODE
  2026-08-02.  Two of them do not exist.**  They were carried here verbatim
  from REVIEW_FullSystem and never checked.  What is actually true:
  - **Frame-magic "validation is untested" — MISLEADING; the validation is
    tested.**  Magic is checked in exactly one place,
    `wire_detail::decode_frame` (`zmq_wire_helpers.hpp`), shared by the inbox
    and the data plane, and it is pinned at L1 by
    `ZmqWireFrameTest.RejectsWrongMagic`.  What the review actually
    established is narrower and true: the L3 worker named for it could never
    reach the decoder — a DEALER's single frame arrives as two, and the
    four-frame envelope guard rejects it first — so the INBOX's own
    `!env.valid → count + drop` branch is never entered via a bad magic.
    That branch's siblings (schema-tag mismatch, payload-size mismatch,
    envelope shape) are each pinned by existing workers and all do the same
    thing, so re-entering it through a fourth trigger is low value; it is
    NOT blocked, though — a bad magic short-circuits `decode_frame` before
    the schema-tag check, so no schema tag has to be forged.  Action taken:
    the test was renamed to `WrongFrameCount_Drops` after what it does pin.
    Adding a bad-magic L3 worker remains optional and unscheduled.
  - **"Reset seq mistaken for a replay" — FALSE.**  HEP-0027 §3.6 states
    that `seq` is metrics-only and explicitly must NOT gate replay, because
    it resets on reconnect; replay is defended by nonce + skew.  The receive
    path matches.  There is no code path in which a reset seq could be read
    as a replay, so there is nothing to pin.
  - **`recv_gap_count` untested — TRUE, now CLOSED** (2026-08-02).  An earlier
    pass in this same session called it blocked, reasoning that a test cannot
    forge a frame the receiver accepts because `compute_inbox_schema_tag` is
    file-static.  That was the wrong question: the counter is not reached by
    FORGING a frame, it is reached by CAUSING a loss.  A small `rcvhwm` plus a
    receiver that stops draining makes `InboxClient::send` drop — its
    documented behaviour — and because `send()` consumes a sequence number
    before it attempts the write, every drop burns a seq that never reaches
    the wire, so the next delivered message's gap equals the number lost
    exactly.  `InboxQueueTest.GapCount_TracksDroppedSends`, mutation-checked.
    Also note this is a METRICS counter; "security-adjacent" was mislabelled.
  - **`HubVault` `known_roles` round-trip — TRUE, now closed** (2026-08-02):
    five L2 pins at the vault seam — deny-all bootstrap, in-memory-only
    `set_known_roles`, save/reopen round-trip, keypair+token preserved across
    save, and the roster living inside the ENCRYPTED payload with no
    plaintext sidecar (the one that would catch a re-plaintexted allowlist).
  - **Logger stress diagnostic — TRUE, now closed** (2026-08-02): the line
    had no `{}` placeholder AND counted a different marker than the
    assertion, so it could only ever print nothing.
  **Process note:** review output was written straight into this file as if
  it were decided work.  It is not.  See "Review findings are not the plan".
- **Federation — 🅿 PARKED (owner, 2026-08-02).  NOT security work, NOT
  next work, not to be re-raised.**  Federation is an unactivated
  proposal: no running hub or role uses it, `cfg.peers` is empty in every
  real deployment, and nothing behind the peer path can be reached until
  someone configures a peer.  Every open federation item stays parked
  under #69 until the owner decides what federation is and when it is
  needed — no design pass, no piecemeal patches, no promotion on the
  strength of a latent finding.  Parked scope: the peer-DEALER ingress
  bypassing `receive_and_validate`, the HUB_PEER_HELLO/BYE +
  HUB_TARGETED_MSG control-envelope bypass (task #99 — an admitted role
  can register itself as a configured peer hub; `HUB_RELAY_MSG` is NOT
  affected, it arrives on the dedicated outbound peer DEALER so its
  sender is fixed by the socket), H43 role-disconnect propagation, #75
  HUB_TARGETED_ACK, and the #105/HEP-0037 post-MVP scope + skipped
  federation tests.  Detail: MESSAGEHUB_TODO "Federation — CONSOLIDATED".
  *(Admin-plane CURVE — the former #1 surface — ✅ SHIPPED 2026-07-19.
  Residual: #105 and three L3 stubs under #52 (#103 ✅ 2026-08-07).  The "Line E" section
  it used to point at was removed in the 2026-08-07 AUTH_TODO rewrite.)*
- **FullSystem-review remediation (4 open of 56)** — 3 test-coverage items are
  the live remainder; the fourth (federation ingress) is parked with #69.  See
  "Active code reviews" for the breakdown.

**In-flight arcs:**
- **#98 Channel broadcast — make it reachable from scripts (owner-approved
  2026-08-02, ACTIVE).**  The facility is unreachable in BOTH directions
  today: `BrokerRequestComm::send_broadcast` has no `RoleAPIBase` method and
  no engine binding, so nothing can send it; and the broker's
  `CHANNEL_BROADCAST_DELIVER_NOTIFY` to producer + consumers hits a
  `parse_notification_id` with no arm for that string, so it classifies as
  `Unknown` and nothing can receive it.  Binding one end alone would ship a
  call that goes nowhere.  Named `channel_broadcast` / `on_channel_broadcast`;
  payload stays `message` + `data` (already the wire, and what the #96 typed
  body requires).  Full HEP-CORE-0011 Sync Matrix sweep in one commit — role
  API, both dispatch halves, Lua, Python, Native (ABI 13→14), `.pyi` ×3,
  three-engine parity tests, HEP-0030/0007 docs.
- **#52** HubHostBrokerHandle → Pattern 4 sweep (in progress; ~21 in-process
  co-host workers across ~6 files remain; Round 1 recipe proven).
- **#57** HEP-0046 Phase B — ✅ COMPLETE 2026-07-24 (see "REG protocol
  redesign" above; full record in `MESSAGEHUB_TODO.md`).
- **Topology dynamic residual (consolidated plan T2-T5)** — **T3 = S1–S3
  owner-first contract code catch-up ✅ SHIPPED 2026-07-26** (with the T2/C
  step 7 L3 test flips folded in).  **The R6 broker-pends gate is RETIRED
  2026-07-25 (superseded by the owner-first contract; do NOT build)**; open:
  T4 Phase E retirements (pre-attach `CONSUMER_ATTACH_REQ_ZMQ`,
  `producer_peers` vector + Tier-2 leak, `ProducerEntry.zmq_node_endpoint` —
  now unblocked) and T5 Phase F demos incl. the L4 producer-first-spawn
  keystone + draft retirement.  Detail: `TOPOLOGY_TODO.md`.
- **Line 3 observer remaining** (HEP-0045 §10): C.2.c `PeerDeathWatcher`
  (epoll) → C.2.d broker dial worker + fd cache → D5 opt-out → C.3
  `collect_shm_info` → C.4 L4 tests → C.5 pointer refresh.

**Role-binary unification (Phase 2a — the next major structural arc, #292 + #55):**
- **#292** — collapse the THREE still-separate role-host `.cpp` files
  (`producer_role_host.cpp` 591 LOC + `consumer_role_host.cpp` 506 +
  `processor_role_host.cpp` 716, each `final : public RoleHostFrame`) into one
  canonical `worker_main_()` in `RoleHostFrame`.  (The RoleAPI unification it
  rides on largely landed — CycleOps already unified into
  `src/utils/service/cycle_ops.hpp`, RoleAPIBase is Messenger-free — but the
  host collapse itself is NOT done.)  Design anchor: `raii_layer_redesign.md §2`.
  **New requirement (2026-07-26, from the schema/metrics-query design):** the
  unified host must admit an OBSERVER role kind — control-plane-only (BRC +
  heartbeat, no data channel) — so monitoring/exporter roles can pull metrics
  on the role plane; today every host fatals without channel establishment.
  See the observer-role rationale in `docs/archive/transient-2026-07-27/tech_drafts/DRAFT_schema_metrics_query_integration_2026-07-26.md` (archived 2026-07-27 — arc complete; the requirement itself lives in this entry).
- **#55** — re-home the 4 `role_api_base_*` L3 tests during that unification
  (deferred, `TESTING_TODO` Group B; all 4 confirmed still-valid 2026-07-16).
- **Phase 2b** — Template RAII Phases 2/4/5 (`TypedInboxClient<MsgT>`,
  `TypedBand<EventT>`, `SimpleRoleHost<SlotT>` — verified absent from `src/`;
  Phase 3 MaxRate pacing already shipped in `slot_iterator.hpp`).
- **Phase 3 — now #127** — CLI `--init` one-shot bundling (`--init` mode flag
  parses; bundling incomplete).  Pulls in **#123** (role vault publishes no
  sibling `.pub`, so `--init` has nothing to wire into `known_roles`) and
  **#124** (27 demo configs — recounted 2026-08-07, the old figure was 24 —
  which should become tool output rather than hand-maintained fixtures).

---

## Open work by area (detail in subtopic TODOs)

- **API / ABI / concurrency / lifecycle (`API_TODO.md`)** — #232 engine
  parity-test contract (incl. #235 band-accessor L3 regression tests);
  demo-harness follow-ups #78-#87; Wave-MD1 ThreadManager shutdown-
  contract sweep; #66 `ZmqQueue`+`InboxQueue` → `apply_socket_policy`;
  Connection/Inbox/Band review D2+D3 follow-ups (C2,C4,C5,I1,I3,X1-X6); HEP-0032
  ABI-compat broader impl (fingerprint chain shipped); #86 last-resort trace ✅ SHIPPED 2026-07-29 — buffer moved into the debug
  module (`debug_info.hpp`/`.cpp`) with a set-only dirty latch, freeze-on-print,
  `PLH_DEBUG_TRACE_BYTES` (16384); `panic()` no longer allocates before
  emitting; clients are lifecycle (`LifecycleManager::critical_report`), Logger,
  ZMQContext, ThreadManager, ZapPumpThread and the SIGTERM watcher; lifecycle
  owns no storage.  Residual: release clean-silence branch needs a subprocess
  test; **#85 teardown stall — cause STILL UNKNOWN** (likely the same open bug as
  #93/#242 in HEP-CORE-0004); its diagnosability half is now closed by #86, so
  the next recurrence should name the step that hung;
  #88 thread-spawn resource failure escaping the non-throwing failure channel
  ✅ SHIPPED 2026-07-29 (`std::thread` ctor threw out of
  `ThreadManager::spawn`'s `bool` and out of `timedShutdown` into
  `~LifecycleGuard() noexcept` → `std::terminate` during teardown; now routed
  through the existing `bool` / `ShutdownOutcome` channels, with three
  discarded `spawn()` returns fixed);
  deferred: Python client SDK, script-spawned worker threads, `src/` restructure.
- **Security / vault (`API_TODO.md`, task #89)** — **#89 SMS expansion + vault
  design**: retained vault key in SMS, script vault surface (HEP-CORE-0038 /
  #106, confirmed NOT implemented from `key_store.hpp:215,297`), and runtime
  config reload — one task because all three need the vault openable after
  startup without the password being script-reachable.  Includes the live hole
  that the master password sits in the environment in cleartext while
  `os.getenv` is unsandboxed in Lua and Python has no sandbox at all.
- **MessageHub / broker protocol (`MESSAGEHUB_TODO.md`)** — #92 `_REQ`-frame
  half-mix audit; (H43 federation role-disconnect → folded into #69); Wave-M2 MP4
  residuals; HEP-0039 Hub State Query Layer Phases B+ (Phase A shipped);
  native-engine inbox parity ✅ CLOSED 2026-07-18.
- **Tests / coverage (`TESTING_TODO.md`)** — Pattern-4 ladder rungs 4/5/6/7/8/9/10/12
  pending (classes absent; rungs 5/6/10 now unblocked by Phase 1; rung 11 partial —
  band contract covered in `test_pattern4_channel_group`/`_broker_protocol`; rungs
  2/3 shipped; rung 13 deferred on back-channel-pipe infra); Pattern-4 sweep #52
  (Round 7 = #56 `datahub_broker_workers`); **#58** audit — confirm every L3/L4
  test-worker file is actually in a real ctest run; #296 hub-death L4; N8/N9 bench
  variants; N11 `on_band_message` parity; B8 numpy pin.
- **Windows / MSVC / cross-platform (`PLATFORM_TODO.md`)** — CI is Linux-only vs
  README support claims; MSVC `/W4 /WX` gate + `/Zc:preprocessor` audit;
  clang-tidy quality pass; #86 native-plugin cmake helper.

---

## Pending harness tasks (open only — TaskList is authoritative)

- **P0:** #93 producer validate-path per-step log lines; #95 SCHEMA_REQ +
  METRICS_REQ keep-reserved-vs-delete (survey HEP-0034 §10.3 + federation #105).
- **P1 (batch):** #79 `--init` non-zero SHM secret default; #80 `rx.fz` binding +
  processor flexzone doc; #82 `band_join` from `on_init` failure surface; #85
  native `on_init`/`on_stop` signature + lifecycle cleanup.
- **P2:** #86 `README_NativePlugins.md` + user-oriented cmake helper.
- **P3:** #94 HEP-0021 §16.5 ephemeral-binding production path (unlocks the
  ENDPOINT_UPDATE sync API — incl. port-0 inbox endpoints); #84 NativeEngine
  `build_api_(HubAPI&)` extension; #87 three-engine doc parity; #73 HEP-0033
  Phase 10 doc closure.
- **P4 long tail:** #66, (#75 `HUB_TARGETED_ACK` → folded into #69), #76 script reload (promote
  `SCRIPT_RELOAD_DESIGN` → HEP + impl), #77 Tier-2 dynamic callbacks
  (`engine_callback_tiers.md`), #81, #88, #89.
- **Parallel / post-MVP:** #106 HEP-0038 script-vault keystore (needs #104 +
  HEP-0040 storage); **#105 Federation / HEP-0037 — explicitly post-MVP, consolidated under #69 (design-first)**
  (federation tests skipped in-suite today).

Deferred follow-ups (tracked, non-blocking): topology **P6** version-tagged
membership (replace full-set allowlist copy); **D-3** clang-query build-fail
rule for §I9.1 layer regressions; native-*sender* inbox L4 delivery test
(needs native-L4-role harness; transport already proven via L3 CURVE + L4
Python).  *(AUTH-6 File 10 Suite 2 delete was listed here as housekeeping —
it closed on 2026-07-20 in `c7f4f608`, which deleted the whole
`RoleIdentityPolicy` file and Suite 2 with it.)*

**Doc-debt:** **HEP-0011 D1** — HEP-CORE-0011 is fundamentally stale (documents
the pre-composition inheritance hierarchy + wrong threading model / class
names).  Rewrite tracked here so it isn't lost (was only in a since-deleted
review memory).

**Parked git stashes (2026-07-18):** 5 exist; `stash@{1..4}` MOOT (landed or
superseded).  Only **`stash@{0}`** (AUTH-2 #162 producer-side BRC ZAP-pump
PeriodicTask) may still be wanted — that pump is NOT on the branch (broker ships
per-cycle `pump_one`, not the stash's drain-loop); reconcile before dropping.

---

## Validation infrastructure (closes #44)

Demo framework — `share/demo_framework/runner.py` + 9 demo manifests under
`share/py-demo-*/` — is the L4 data-pipeline reference; clone + tweak for new
scenarios.  Inventory: `TESTING_TODO.md` § "Test infrastructure inventory".

---

## Active code reviews (1 — updated 2026-08-07)

> **Review output is not the plan.** These records are candidate findings,
> not decided work. Nothing here is scheduled until it has been validated
> against code and a HEP, and then chosen.

- `code_review/LINT_FIXES_PLAN.md` — **the only active record, and its input is
  stale.** Built from an April 2026 clang-tidy log that no longer exists;
  re-checked 2026-08-07 and it is *partially* stale, which is the worst kind —
  `actor_vault.cpp` is deleted (1 row in §1, 3 in §2.1) and the `hub_config.cpp`
  rows point at the legacy singleton, while other cited symbols do survive
  (`g_wake_pipe`, `computeUnloadClosure`), lending the dead rows false
  credibility. §2 also asks five owner questions never answered. **Order of
  work: regenerate the log (the recipe lives in `PLATFORM_TODO.md` § "Clang-tidy
  quality pass"), diff against the plan, then ask only the questions that still
  have a subject.** Task #114. Do not action as written.

- `code_review/REVIEW_VALIDATION_2026-08-02.md` — kept as the audit trail for
  how the other records were dispositioned. Of ~26 items carried as open, 12
  were already fixed and never marked, 2 stale, 2 misreads, 5 advisory, 5
  genuinely valid. Three carried forward, all LOW: B-1 (loop helpers, in
  `API_TODO.md`), O1b (`query_shm_info` — now #112), O3 (three forwarders,
  recorded not recommended). **Its standing caveat about the FullSystem
  review's ~50 unverified ✅ notes is discharged** — that verification ran
  2026-08-07 and every note held.

**Archived 2026-08-07 after full verification** (see `DOC_ARCHIVE_LOG.md`
pass 3) — both are now under `docs/archive/transient-2026-08-07/code_review/`:

- `REVIEW_FullSystem_2026-07-20.md` — all ~50 ✅ resolution notes independently
  re-checked against source; every one held. The nine that had been verified
  only by a test *name* were re-run: all nine passed in a full 2780-test sweep.
  Residual doc drifts filed as #110.
- `REVIEW_Connection_Inbox_Band_2026-05-17.md` — 16 of 16 findings re-checked:
  8 resolved, 2 closed as accepted design, 6 open and rehomed to `API_TODO.md`
  plus tasks #111 and #112.

`REVIEW_FullModule_2026-04-06.md` was archived 2026-08-03 — all ten rows
dispositioned against code; its one live finding (B-1) moved to `API_TODO.md`
first. Closed reviews archived per `docs/DOC_ARCHIVE_LOG.md`.

---

## Label hygiene — read before reading any "M*" label

| Label prefix | Means |
|---|---|
| `Wave-B M0..M9` | Sequential phases of Arc B (role-host renovation) |
| `HEP-0033 §15 Phase N` | Sequential phases of Arc A (`plh_hub` renovation) |
| `Wave-M2 / Wave-M2.5 / Wave-M3` | Closed side-arcs (multi-producer + controlled-access) |
| `M1.2 / M1.4 / M1.5 / MD1 / MD1.5` | Closed FSM-consolidation + race-fix side-arcs |

A bare "M3" is almost certainly **Wave-B M3** (RoleHandler skeleton) — verify
against context; NOT `Wave-M3` (RoleEntry controlled-access side-arc).

---

## Quick links

- Build / CMake / staging: `README.md` + `docs/README/README_CMake_Design.md`.
- Running tests + patterns: `docs/README/README_testing.md`.
- Subsystem design contracts: `docs/HEP/HEP-CORE-*.md`.
- Implementation rules + error taxonomies + session hygiene:
  `docs/IMPLEMENTATION_GUIDANCE.md`.
- Doc placement + lifecycle: `docs/DOC_STRUCTURE.md` (incl. §2.1.1 TODO quality
  check).  Archive log: `docs/DOC_ARCHIVE_LOG.md`.

---

## Maintenance rule (see `DOC_STRUCTURE.md §2.1.1`)

Keep this file ≤ 200 lines; subtopic TODOs focused on OPEN items.  At the end of
every sprint / major commit batch, verify completion claims against **code + log
(not commit messages)**, then extract completed content to a dated completions
index.  TODOs are *for what to do, not what has been done.*
