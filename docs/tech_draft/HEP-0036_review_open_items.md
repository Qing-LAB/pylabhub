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
**Status:** ✅ RESOLVED (2026-05-28).
**Decision:** SYMMETRIC design — both producer and consumer use
their identity keypairs (from `--keygen`) on the data plane.
Broker mints NO data-plane keypairs; it tracks allowlists and
caches each channel's authorized producer identity pubkey.  The
`consumer_pubkey` wire field is DROPPED entirely — broker recovers
the CURVE-proved consumer identity via `zmq_msg_gets("User-Id")`
on the BRC socket (no self-claims; SSH `authorized_keys` model).
SHM keeps its broker-generated `shm_secret` (unrelated to CURVE).
**Supporting work:**
- HEP-0035 §4.6 file ACL discipline (commit d8591e73, task #101).
- HEP-0035 §4.7 runtime key handling (commit a919938c, task #102).
**HEP-0036 sections updated:** §3 I6, §3 I8, §4.1 ChannelAccessEntry,
§4.2 diagram, §5.1 / §5.2 / §5.3 / §5.4 / §5.5 / §5.6 / §5.7,
§6.1-6.4 + 6.6 (wire format + error codes), §10 lifetime, §12
phases.

### T2 — `Authorized` state + role/broker FSM coupling
**Status:** 🟡 DISCUSSED-NOT-LOCKED.
**Question:** how does the role transition into a state that gates
data flow, and how does the broker know the producer is ready to
admit consumers?
**Resolution path discussed:** first heartbeat (existing
`first_heartbeat_seen` mechanism, `hub_state.hpp:107`) is the
broker-visible "producer ready" signal; role-side `Authorized` is
the local gate that controls when `install_heartbeat` is called.
**Where it appears in HEP-0036:** §4.3 FSM, §5.1 + §5.2 + §10
sequences, §12 phases 0.5 / 0.7 / 0.8 — all written into commit
d9f7d218.
**Open sub-questions (asked but not formally answered):**
- **T2-Q1:** consumer's `Authorized` trigger via ZMQ socket monitor
  (`HANDSHAKE_SUCCEEDED` / `HANDSHAKE_FAILED_AUTH`) — yes/no?
  Diagram assumes yes.
- **T2-Q2:** `Authorized → Unregistered` trigger threshold = the
  existing `hub_dead_grace`, or shorter? Diagram assumes
  `hub_dead_grace`.
**To close:** explicit yes/no on Q1 + Q2.

### T3 — Broker restart / per-channel key staleness
**Status:** 🔄 OPEN — not yet discussed.
**Question:** when the broker crashes + restarts, the role's PUSH
socket is still bound with broker-minted per-channel keys that the
new broker doesn't know about.  Same problem applies to producer
restart (broker keys still alive on a now-dead producer).
**Why it matters:** HEP-0036 §10 lifecycle is silent on this; T2's
hub-dead path partly handles it (role-side teardown of stale data
sockets) but the broker-side recovery semantics aren't documented.

### T4 — Inbox CURVE wiring (§9.3 claim)
**Status:** 🔄 OPEN — not yet discussed.
**Question:** §9.3 currently claims inbox inherits the channel's
ZAP cache + per-channel keypair.  Code reality: inbox uses
control-plane CURVE keys (BRC's identity keypair), zero CURVE refs
in `hub_inbox_queue.cpp`.  Three options:
1. Inbox shares the data channel's per-channel keypair + cache.
2. Inbox gets its own per-channel keypair.
3. Inbox stays on control-plane (= role identity) CURVE.
**Why it matters:** the §9.3 wording is currently a design claim,
not a verified design.  Pick → write code-grounded §9.3.

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
**Status:** ✅ RESOLVED — committed d8591e73.  Task #101 filed.

### S2 — HEP-0036 Phase 0 reconciliation with HEP-0035 §4.5
**Status:** ✅ RESOLVED — Phase 0 = "prerequisite from HEP-0035
Layer-1 ZAP" (commit d8591e73).

### S3 — Runtime key handling (HEP-0035 §4.7)
**Status:** ✅ RESOLVED — mlock + no-core-dump + zeroing,
cross-platform via libsodium (Linux, macOS, Windows).  Doc:
HEP-0035 §4.7.  Task #102 filed.  Rejected "random session key
in-memory encryption" as obfuscation that doesn't increase
attacker work.

---

## Order of discussion (proposed, user can override)

1. T1 — formally lock Option A (or B).
2. T2-Q1 — socket monitor for consumer `Authorized` trigger.
3. T2-Q2 — `hub_dead_grace` as `Authorized → Unregistered` trigger.
4. T3 — broker restart key staleness policy.
5. T4 — inbox CURVE wiring.
6. T5 — federation allowlist propagation.
7. Sweep M3 + M4 + M5 + M6.

## Commits referenced in this doc

- `a2e68057` — HEP-0036 draft
- `4773f4c7` — HEP-0036 revision (two-conditions; passive revocation)
- `d8591e73` — HEP-0035 §4.6 file ACL + HEP-0036 Phase 0 reconciliation
- `a75f0fbb` — task #101 filed in API_TODO
- `d9f7d218` — HEP-0036 T2 FSM + sequence diagram updates (NOT YET
  LOCKED — Q1 + Q2 baked in without explicit user agreement; may
  need partial revert depending on T2 close-out)
