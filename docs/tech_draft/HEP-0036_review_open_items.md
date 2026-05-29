# HEP-0036 Review — Open Items Tracker

Persistent record of items raised during the 2026-05-27/28 HEP-0036
design review.  Lock-in flow: **discuss → agree → mark RESOLVED with
one-line decision + reference to the HEP section / commit that holds
the agreed wording**.

This doc lives under `docs/tech_draft/` per `DOC_STRUCTURE.md` §1.7
(in-progress design work).  When all items are resolved, fold
decisions into HEP-0036 and archive this file per
`docs/IMPLEMENTATION_GUIDANCE.md` § Session Hygiene step 5.

## Status legend

- 🔄 **OPEN** — not yet discussed.
- 🟡 **DISCUSSED-NOT-LOCKED** — design surfaced, possibly baked into
  HEP draft, but no formal yes/no from user.  Don't treat as
  decided.
- ✅ **RESOLVED** — explicitly agreed; HEP-0036 (or sibling HEP)
  reflects the decision.
- 🗂️ **DEFERRED** — out of MVP scope; tracked but not blocking.

---

## Tier 1 — Architectural blockers (must close before any Phase 1 impl)

### T1 — Consumer's data-side pubkey provenance
**Status:** ✅ RESOLVED (locked 2026-05-28; symmetric-array
migration committed `5dc1c65c`; §5.7 fan-in split committed
`87784a8f`).
**Decision:** SYMMETRIC design — both producer and consumer use
their identity keypairs (from `--keygen`) on the data plane.
Broker mints NO data-plane keypairs; it tracks allowlists.  The
`consumer_pubkey` wire field is DROPPED entirely — broker recovers
the CURVE-proved consumer identity via `zmq_msg_gets("User-Id")`
on the BRC socket (no self-claims; SSH `authorized_keys` model).
The legacy single `data_server_pubkey` field is REPLACED with a
`producers[]` array in CONSUMER_REG_ACK (one element per
registered producer; single-producer = length 1; fan-in = length
N — uniform wire shape, no special cases).  `ChannelAccessEntry`
holds NO producer-pubkey field — producer pubkeys live on
`ChannelEntry::producers[i].zmq_pubkey` (the existing Wave-M2.5
per-producer field).  SHM keeps its broker-generated `shm_secret`
(unrelated to CURVE).
**Supporting work:**
- HEP-0035 §4.6 file ACL discipline (commit `d8591e73`, task #101).
- HEP-0035 §4.7 runtime key handling (commit `a919938c`, task #102).
- Task #94 / HEP-0021 §16.5 — DISC_REQ_ACK array migration must
  land coordinated with HEP-0036's CONSUMER_REG_ACK array (same
  wire family).
- Task #103 — ZmqQueue dynamic peer API (HEP-0017 §3.3); the
  framework consumer of `producers[]` per I9.
**HEP-0036 sections updated:** §3 I6, §3 I8, §4.1
ChannelAccessEntry, §4.2 diagram, §5.1 / §5.2 / §5.3 / §5.4 /
§5.5 / §5.6 / §5.7 (later split into §5.7.1 + §5.7.2 — see Q4
below), §6.1-6.4 + §6.5 + §6.6 (wire format + error codes),
§8.3, §9.1 (per-producer fan-in), §10 lifetime, §12 phases,
§14.1 (HEP-0021 cross-update), §14.4 (HEP-0017 cross-update added
post-I9), §15 references.

### T2 — `Authorized` state + role/broker FSM coupling
**Status:** ✅ RESOLVED (locked 2026-05-28; committed across
`d9f7d218`, `06798daa`).
**Decision:** Role-side `RegistrationState` gains a new
`Authorized` state between `Registered` and `Deregistered`.
- **Producer's Authorized** fires SYNCHRONOUSLY at end of
  `setup_infrastructure_` (PUSH bound + ZAP installed +
  `ENDPOINT_UPDATE_REQ` ACK'd).  `install_heartbeat` is called
  next; the first heartbeat tick fires; broker's existing
  `first_heartbeat_seen` mechanism (`hub_state.hpp:107`) flips
  channel from `kRegistering → kLive`.  Consumer admission is
  gated on `kLive` (R6 — small broker-side fix at
  `broker_service.cpp:1959` extending the existing check to match
  DISC_REQ's behavior).
- **Consumer's Authorized** fires SYNCHRONOUSLY when
  `CONSUMER_REG_ACK.producers[]` is received and the framework
  constructs the rx queue.  Authorization was completed at REG
  (control plane: I1 cond 1 + cond 2 + Q1 allowlist push to each
  producer); the endpoints in the ACK ARE the proof of
  authorization (no endpoint disclosure on rejection — §5.2).
  No socket monitor; data-plane CURVE handshake is
  transport-internal per I9.
- **`Authorized → Unregistered`** (recovery on sustained broker
  loss): existing `hub_dead_grace` threshold, no new knob.
- Data loop's outer guard adds `any_presence_authorized()` —
  HEP-0036 §8.2.
**Sub-questions:**
- **T2-Q1** ✅: Consumer Authorized is SYNCHRONOUS at queue
  construction (corrected from earlier socket-monitor framing;
  commit `06798daa`).
- **T2-Q2** ✅: Reuse existing `hub_dead_grace`; no benefit to
  a new knob.
**Where in HEP-0036:** §3 (new `Authorized` between Registered
and Deregistered), §4.3 FSM, §5.1 + §5.2 + §10 sequences, §8.2
gating, §8.3 per-presence, §12 phases 0.7 + 0.8 + 4 + 6.

### T3 — Broker restart / per-channel key staleness
**Status:** 🔄 OPEN — not yet discussed.
**Question:** when the broker crashes + restarts, the role's PUSH
socket is still bound with broker-minted per-channel keys that the
new broker doesn't know about.  Same problem applies to producer
restart (broker keys still alive on a now-dead producer).
**Why it matters:** HEP-0036 §10 lifecycle is silent on this; T2's
hub-dead path partly handles it (role-side teardown of stale data
sockets) but the broker-side recovery semantics aren't documented.

### T4 — Inbox CURVE wiring (§9.3)
**Status:** 🟡 PARTIAL — SIMPLIFIED by T1; §9.3 wording committed
in `5dc1c65c` (uses identity keypair per I6; shares data
channel's ZAP allowlist as MVP default).  Remaining question is
narrow: should inbox have a DISTINCT allowlist scope from the
data channel?
**Original 3-option framing was:** (1) inbox shares data
channel's per-channel keypair + cache, (2) inbox gets its own
per-channel keypair, (3) inbox stays on control-plane (role
identity) CURVE.
**After T1:** options (1) and (2) DISSOLVE (no per-channel
keypairs exist anywhere) and (3) IS what T1 mandates (identity
keypair used on inbox sockets too).
**Remaining question:** distinct allowlist scope?  Code reality:
`hub_inbox_queue.cpp` has zero CURVE refs today; needs
implementation regardless of scope choice.
**Why it matters:** the §9.3 wording locks "shared allowlist
scope" as the MVP default but reviews could revisit if a use
case emerges.

### T5 — Federation CHANNEL_AUTH_UPDATE propagation (§13.1 Q1)
**Status:** 🔄 OPEN — listed as "open question 1" in HEP-0036
§13.1; not yet substantively discussed in review.
**Question:** when Hub-A's consumer joins a channel hosted on Hub-B,
how does Hub-B's producer's ZAP cache learn the consumer's pubkey?
Hub-A relays via `HUB_RELAY_MSG`, or each hub maintains its own
allowlist (consumer's pubkey must be in BOTH hubs' `known_roles[]`)?
**Why it matters:** dual-hub processor (already in production) is
the blocking use case.

---

## Tier 2 — Medium (resolve during the relevant phase)

### M1 — Legacy `check_role_identity()` is dead code under HEP-0035 §4.5
**Status:** ✅ RESOLVED — HEP-0035 §4.5 drops the string-based
machinery; HEP-0036 §3 I1 note updated to reflect this (commit
d8591e73).  Implementation will REMOVE the function in Phase 11
cleanup.

### M2 — `channel_policy_overrides` (HEP-0035 §4.5)
**Status:** ✅ RESOLVED — HEP-0035 §4.5 drops the concept; HEP-0036
inherits hub-wide admission.  Revisit only if a concrete use case
emerges.

### M3 — Multi-hub same-role coordination (operator surface)
**Status:** 🔄 OPEN.  Role has ONE keypair from `--keygen`; must
appear in EVERY hub's `known_roles[]` it connects to.  HEP-0036
§11.3 mentions this obliquely; should be a clear operational note.

### M4 — ZAP handler health detection
**Status:** 🔄 OPEN.  What if the ZAP handler thread dies but the
BRC poll thread is alive?  CHANNEL_AUTH_UPDATE_ACK succeeds (BRC
handles it) but the cache update never lands (ZAP thread is dead).
All future handshakes silently DENY by libzmq timeout.  Need:
heartbeat between threads; on missed heartbeat, BRC reports critical
error.

### M5 — `endpoint_hint_range` validation (§6.1)
**Status:** 🔄 OPEN.  Field accepted on the wire but no code
validates "bound port falls within range."  Either spec the
validator or drop the field.

### M6 — `initial_allowlist` for pre-REG consumers (§6.2)
**Status:** 🔄 OPEN.  Field mentioned in §6.2 wire format but no
broker-side buffering exists; will always be empty.  Either spec
the buffering (consumer-before-producer scenario) or drop the
field.

---

## Tier 3 — Lower (future hardening)

### L1 — Rate-limiting on CONSUMER_REG_REQ
**Status:** 🗂️ DEFERRED.  Authenticated-but-malicious role could
DoS a producer's BRC via spam.  Add per-role token bucket later.

### L2 — Endpoint / pubkey logging policy
**Status:** 🗂️ DEFERRED.  Spec: never log at INFO or above; treat
as DEBUG-only.  Documentation item.

### L3 — Config-file integrity
**Status:** ✅ RESOLVED — HEP-0035 §4.6 addresses file mode;
operator responsibility for content integrity.

### L4 — `ChannelAccessIndex` synchronization model
**Status:** 🟡 PARTIAL.  Broker is single-threaded handler dispatch
today; will document the index as "owned by broker handler thread;
no external sync needed" once HEP-0036 §4.1 implementation lands.

### L5 — Replay attack analysis
**Status:** 🗂️ DEFERRED.  CURVE handles in-session replay
protection; cross-session requires seckey (insider threat).

---

## Side-discussion items (already closed)

### S1 — File-ACL discipline (HEP-0035 §4.6)
**Status:** ✅ RESOLVED — committed `d8591e73`.  Task #101 filed.

### S2 — HEP-0036 Phase 0 reconciliation with HEP-0035 §4.5
**Status:** ✅ RESOLVED — Phase 0 = "prerequisite from HEP-0035
Layer-1 ZAP" (commit `d8591e73`).

### S3 — Runtime key handling (HEP-0035 §4.7)
**Status:** ✅ RESOLVED — mlock + no-core-dump + zeroing,
cross-platform via libsodium (Linux, macOS, Windows).  Doc:
HEP-0035 §4.7.  Task #102 filed.

### S4 — I9 three-tier separation (architectural principle)
**Status:** ✅ RESOLVED (locked 2026-05-28; committed `0ade2394`
HEP-0017 §3.3 + §4.6.1 + HEP-0036 I9; propagated through HEP-0036
in `87784a8f` + `a4af9f98`).
**Decision:** Wire protocol does NOT expose transport-level
operations to roles.  Architecture is four-tier:
- **Broker** (HubState): authoritative state + channel-event
  broadcasts via HEP-0033 §12 (no new wire messages added by
  HEP-0036).
- **Role-host framework**: consumes broadcasts via BRC; calls
  `queue.add_producer_peer` / `remove_producer_peer` on ZmqQueue
  (HEP-0017 §3.3).
- **Queue (ZmqQueue / ShmQueue)**: all transport plumbing —
  sockets, bind/connect direction, ZAP cache, fair-queue.
  Conceals N-producer fan-in.
- **Script**: sees only queue API (`api.rx.acquire()` /
  commit, band callbacks, inbox via `api.list_producers(channel)`).
**Implications:**
- HEP-0036 adds NO new wire messages for membership-change
  reactions; existing channel-event family covers it.
- Bind/connect direction in ZmqQueue is internal — HEP-0036
  doesn't specify.
- Tracks under task #103 (HEP-0017 §3.3 implementation).

### DP-Q1 — Fan-in CHANNEL_AUTH_UPDATE failure semantics
**Status:** ✅ RESOLVED (locked 2026-05-28; committed `87784a8f`).
**Decision:** Option B "skip-disconnected" — broker pushes
`CHANNEL_AUTH_UPDATE add` only to currently-kLive producers;
waits up to `push_ack_timeout_ms` per ACK (default 2000 ms;
configurable via `hub.broker.push_ack_timeout_ms` in hub.json).
Producers that don't ACK within the timeout are SKIPPED; they
re-sync via `REG_ACK.initial_allowlist` when they next REG_REQ
after reconnect.  If zero producers ACK (catastrophic-no-producer
branch): broker returns
`CHANNEL_NOT_READY{reason="no_live_producer"}`.  Otherwise:
CONSUMER_REG_ACK with `producers[]` = ACK'd subset.
`ALLOWLIST_PUSH_FAILED` error code RETIRED — partial success is
the normal case under skip-disconnected.
**Where in HEP-0036:** §6.5 (CHANNEL_AUTH_UPDATE spec), §6.6
(error codes), §13.2 (resolved summary).

### DP-Q2 — New producer joins existing channel
**Status:** ✅ RESOLVED (dissolved by I9).
**Decision:** No new wire protocol needed.  Broker emits the
existing channel-event broadcast (HEP-0033 §12) when a new
producer REGs an existing channel; consumer-side frameworks call
`rx_queue.add_producer_peer(new_producer)` per HEP-0017 §3.3 /
HEP-0036 I9.  `REG_ACK.initial_allowlist` covers the
joining-producer's ZAP cache.

### DP-Q3 — Channel-scope vs per-producer ACL
**Status:** 🔄 OPEN — needs explicit non-goal in HEP-0036 §2.1.
**Question:** Should the doc explicitly declare per-producer ACL
out of MVP scope?  Today's design is channel-scope only:
authorization for channel X allows the consumer to connect to ANY
producer of X.  Per-producer ACL (consumer C may subscribe to P1
but not P2 within X) is NOT supported — operators with per-producer
gating needs should split into separate channels.
**To close:** one-line addition to §2.1 non-goals.

### DP-Q4 — Per-producer DEREG cascade vs last-producer teardown
**Status:** ✅ RESOLVED (committed `87784a8f`).
**Decision:** §5.7 split into two cases per HEP-CORE-0023 §2.1.1
atomic-teardown semantics:
- **§5.7.1 Per-producer DEREG (channel survives):** broker
  removes from `ChannelEntry::producers[]`; no
  CHANNEL_CLOSING_NOTIFY; emits channel-event broadcast (HEP-0033
  §12); consumer-side framework calls
  `rx_queue.remove_producer_peer(P.uid)`.  No new wire messages.
- **§5.7.2 Last-producer DEREG (channel teardown):** atomic
  teardown of ChannelEntry; existing `CHANNEL_CLOSING_NOTIFY`
  fires to all consumers; consumer-side rx queue tears down at
  the framework level (per I9).

### S3 — Runtime key handling (HEP-0035 §4.7)
**Status:** ✅ RESOLVED — mlock + no-core-dump + zeroing,
cross-platform via libsodium (Linux, macOS, Windows).  Doc:
HEP-0035 §4.7.  Task #102 filed.  Rejected "random session key
in-memory encryption" as obfuscation that doesn't increase
attacker work.

---

## Order of discussion (remaining)

1. **DP-Q3** — channel-scope vs per-producer ACL non-goal (one-line
   addition to §2.1).  Trivial.
2. **T3** — broker restart key staleness policy.
3. **T5** — federation allowlist propagation (cross-hub).
4. **Sweep** M3 + M4 + M5 + M6.

(T1 ✅, T2 ✅, T4 🟡 partial, I9 ✅, DP-Q1 ✅, DP-Q2 ✅, DP-Q4 ✅.)

## Commits referenced in this doc

- `a2e68057` — HEP-0036 draft
- `4773f4c7` — HEP-0036 revision (two-conditions; passive revocation)
- `d8591e73` — HEP-0035 §4.6 file ACL + HEP-0036 Phase 0 reconciliation
- `a919938c` — HEP-0035 §4.7 runtime key handling (cross-platform)
- `a75f0fbb` — task #101 filed in API_TODO
- `d9f7d218` — HEP-0036 T2 FSM + sequence diagram updates (NOT YET
  LOCKED — Q1 + Q2 baked in without explicit user agreement; may
  need partial revert depending on T2 close-out)
- `5dc1c65c` — T1 symmetric design + producers[] array migration
- `0ade2394` — HEP-0017 §3.3 / §4.6.1 + HEP-0036 I9 three-tier
  separation
- `87784a8f` — HEP-0036 Set A + B consistency sweep: §6.5
  fan-in plural framing + Q1 lock-in (skip-disconnected) + §5.7
  split (§5.7.1 per-producer / §5.7.2 last-producer-cascade) +
  Set B PULL → queue propagation
- `a4af9f98` — HEP-0036 §13/§14/§15 propagation: §14.4 (HEP-0017)
  added; §13.2 Q1 lock-in summary; §15 references updated
- `06798daa` — T2-Q1 LOCKED: consumer Authorized is SYNCHRONOUS at
  queue construction (auth was completed at REG / control plane);
  removed socket-monitor framing from §4.3.2 / §5.2 / §8.3 / §10 /
  §12 phases 0.7+4
- `46e27acd` — tracker T2 RESOLVED + reorganized "order of
  discussion remaining" list
- `a4f8e623` — post-T2-lock sloppiness sweep: §4.3.1 FSM REG_ACK
  annotation (producers[] array); §4.3.2 heading "both sides
  synchronous"; dropped T2 R# internal labels in §6.6 + §12;
  §10 lifetime intro updated to T2 LOCKED
- `2b20e7fb` — sibling-HEP sync: propagated HEP-0036 T1/T2/I9/Q1
  locks across HEP-0017, 0021, 0023, 0027, 0030, 0035 (added §14.5
  HEP-0027 + §14.6 HEP-0030 to HEP-0036)
- `0f82dc70` — post-sync sloppiness sweep: §14 "four → six sibling
  HEPs"; §14.4 commit hash split; HEP-0021 §16.4 + HEP-0035
  §4.1+§4.2 cross-ref clarifications

### S5 — Sibling-HEP cross-reference audit
**Status:** ✅ RESOLVED — coordinated cross-reference + body-text
sweep across HEP-CORE-0007 §12 (CONSUMER_REG_ACK producers[]
array), HEP-CORE-0017 §3.2 + §3.3 + §4.6.1, HEP-CORE-0021 §5.1
+ §5.2 + §16.4, HEP-CORE-0023 header + §2.1, HEP-CORE-0027
header + §3.5, HEP-CORE-0030 header, HEP-CORE-0033 §8,
HEP-CORE-0035 §4.1 + §4.2 + §6.  Task #104 tracks the code-
level implementation work that follows the doc updates.
