# DRAFT — HEP-CORE-0036 §6.5 amendment: ZMQ pre-attach broker confirmation

| Attribute | Value |
|---|---|
| **Status** | 🟡 DESIGN DRAFT — simplified 2026-07-01 after 10 review passes |
| **Tracker** | [#246](../todo/AUTH_TODO.md#-246-hep-core-0036-amendment) |
| **Sibling** | HEP-CORE-0041 Phase 5 (ZMQ retrofit to symmetric capability semantics) |
| **Drafted** | 2026-06-30 (simplified 2026-07-01) |
| **Chain position** | Follows #275 (1i-cleanup arc complete); precedes REVIEW-D + #262 mutual auth |

---

## Design principles (explicit trade-offs, locked 2026-07-01)

Five principles, locked.  They exist because 10 prior fresh-eye review passes each pressured the doc to spec more corner cases, ballooning it from ~350 → ~1250 lines with cascading regressions (each cleanup introduced new bugs in adjacent sections).  The principles set the boundary: what a reviewer should worry about and what they should leave alone.

**One-sentence mental model.**  Broker tracks a monotonic version per channel and a confirmed version per (channel, producer) pair.  When a consumer needs to attach and the producer isn't caught up, ONE NOTIFY-triggered pull cycle syncs them.  Bad network = timeout + user retry.  Pubkey rotations = one extra sync cycle.  No aging, no debounce, no capability negotiation.

- **P0: Network-stable operating assumption.**  Design assumes broker + producer + consumer are all reachable on the happy path.  When they're not, we degrade to timeout + retry, not to elaborate recovery machinery.  Reviewers should NOT propose defenses against dropped packets, ROUTER hiccups, or NTP jumps — those are P3 territory.
- **P1: Simple over corner-perfect.**  The ONLY asynchrony this design handles is pubkey allowlist changes (admit / revoke / rotate).  Each such change costs at most ONE wait-path attach cycle.  Rare secondary events (broker restart, dropped NOTIFY under bad network, mixed-fleet transition) also cost one cycle each and are handled by the same wait-path mechanism — no dedicated code for them.  Attach is a startup-time cost, not a data-plane cost.
- **P2: Strict contract over hopeful contract.**  All producers on a pre-confirm channel MUST support `CHANNEL_AUTH_APPLIED_REQ`.  Mixed-fleet coexistence is out of scope — Phase 3 (producer upgrade) MUST complete fleet-wide before Phase 4 (consumer pre-confirm).  Deploy discipline replaces protocol negotiation.
- **P3: Retry over recovery.**  User scripts retry on failure.  Framework does not build recovery mechanisms for transient bad-network conditions.  Bad network = degraded attach experience (timeouts), not a supported operating mode.
- **P4: No leak, no security regression.**  Any simplification MUST preserve the correctness invariant: a consumer's CURVE handshake against producer P succeeds if-and-only-if broker has confirmed P's ZAP cache holds the consumer's pubkey.  This is the ONE invariant the design is not allowed to weaken.

### Review guardrails — worry / don't worry list

When reviewing this design, use these as the decision boundary.  If a finding is on the "don't worry" side, it does not warrant a doc change; note it as accepted-degradation instead.

| Worry (in scope) | Don't worry (out of scope, accepted degradation) |
|---|---|
| Wire spec correctness — payload fields, status enum, direction, sequencing. | Byte-level payload efficiency; protocol version bumps for future flexibility. |
| Broker handler flow — the four steps in §5.3.  Missing case → real bug. | Recovery timing after broker restart / producer restart / dropped NOTIFY.  These are acceptable one-cycle wait-path costs per P1. |
| Fast-path correctness — the invariant `confirmed_version[K][P] >= channel_version[K]` implies the current allowlist is in P's cache. | Fast-path OPTIMALITY.  We deliberately trade some fast-path opportunities (e.g., cross-consumer version conflict) for zero per-consumer version tracking. |
| Security invariant P4 — no consumer succeeds a CURVE handshake without broker-confirmed cache-in-sync. | Timing corners of security invariant.  If under a specific race a consumer sees timeout instead of denied, that's degraded UX, not a security regression. |
| Fan-in policy contract — best-effort loop; ≥1 admitted = success; excluded producers not dialed. | Fan-in optimality.  Serial per-producer ATTACH_REQ under a 10-producer channel may take ~30-50 s worst case.  Acceptable. |
| Test-contract-stable log markers (§6.3). | Log line WORDING beyond the marker table.  Prose changes are non-normative. |
| Cross-references — §X.Y MUST point at content that exists AND says what the ref claims. | Section-numbering perfectionism.  §5.X-lettered sub-subsections were retired in favor of numbered ones. |
| Producer capability requirement (§9 rollout ordering). | Runtime capability negotiation.  Deploy discipline replaces per-producer version fields. |
| APPLIED_ACK is `{status="ok"}` on success.  Reply confirms broker received the confirmation. | Enumerated wire status for APPLIED_ACK (stale / unknown_channel / etc).  Producer's local action is the same regardless. |
| Idempotency of the APPLIED_REQ handler under legitimate retries. | Explicit debounce state machines to prevent redundant NOTIFYs.  Fire-every-time is acceptable per P1; wire is cheap. |
| Field-name consistency across §4 / §5 / §10. | Alternate naming schemes (e.g., snapshot_version vs applied_version rename cycles).  Names are locked once specified. |

**Meta-rule for reviewers:** if a finding requires adding a new corner-case-handling mechanism (state, timer, capability negotiation, retry loop, distinct enum value), CHECK the guardrails first.  If the corner case falls under P0/P1/P2/P3, the finding is closed as accepted-degradation with a one-line note in §11 (scope discipline) — NOT with a design change.  ONLY findings that touch P4 (security correctness) or reveal a plain-and-simple mistake (wrong field name, contradiction between sections, broken cross-reference) warrant edits.

**Meta-rule for the author (me):** if 10 prior review passes have not found a specific design bug, and the 11th surfaces a novel one, apply the guardrails BEFORE committing a fix.  Cascading corner-case coverage is what generated the pre-simplification bloat.  Novel corner cases are almost always P0/P1/P3 territory.

---

## 0. Glossary (jargon expanded)

| Term | Meaning |
|---|---|
| CURVE | libzmq's built-in Curve25519 encryption + authentication (RFC 26).  Producer binds as CURVE server; consumer connects as CURVE client. |
| ZAP | libzmq's authentication REP handler (RFC 27).  In-process handler on the CURVE server side (producer) that decides "accept this client pubkey?" per handshake.  Consumer has NO ZAP handler. |
| ROUTER | libzmq socket type used by broker.  Preserves peer identities on inbound, enabling reply-later semantics. |
| PULL / PUSH | libzmq socket types.  Consumer's data-plane is PULL (client, dials); producer's data-plane is PUSH (server, binds). |
| Z85 | ZeroMQ's Base85 encoding for Curve25519 keys (32 bytes → 40 chars). |
| kLive / kDead | Producer lifecycle states on `ChannelEntry[K].producers[P].state`.  Only kLive producers admit consumers. |
| ChannelEntry / ChannelAccessIndex | Broker-side data structures.  ChannelEntry tracks producers per channel.  ChannelAccessIndex tracks the authorized consumer pubkey allowlist per channel. |
| BRC | BrokerRequestComm — the consumer/producer-side sync REQ/REP wrapper for broker's ROUTER. |
| REG_ACK.producers[] | Shorthand for CONSUMER_REG_ACK.producers[] — list of `{role_uid, endpoint, pubkey_z85}`. |

---

## 1. Motivation

Today's ZMQ auth flow has an observed race that skips a fresh consumer's data.  The L4 test `PlhHubCliTest.ZmqE2E_AuthorizedConsumerReceivesAllSlots` skips today per this task, per inline evidence:

> "broker sends `CONSUMER_REG_ACK` (consumer dials) BEFORE the producer's `CHANNEL_AUTH_CHANGED_NOTIFY → GET_CHANNEL_AUTH` chain seeds the producer's ZAP allowlist.  libzmq's initial CURVE handshake fails on the empty allowlist and the consumer never receives data (allowlist update fires ~110ms AFTER consumer PULL goes Active)."

HEP-CORE-0041 §5.5 already solved the symmetric problem on SHM via a pre-attach broker confirmation.  This amendment retrofits ZMQ to the same shape: broker confirms admission before consumer dials producer.  Post-amendment, ZMQ vs SHM difference collapses to just the byte-transport (bind + push vs SCM_RIGHTS handoff).

---

## 2. Design goal

**Symmetric pre-confirm on both transports.**  Consumer sends `CONSUMER_ATTACH_REQ` to broker; broker holds it until producer's cache is confirmed populated; consumer proceeds to dial safely.

**Preserve the discipline established by the 2026-06-04 retraction.**  Broker MUST NOT re-become a "sync-request initiator" on its ROUTER.  Any new pattern that holds a response pending an external event MUST be justified against §6.5's retraction rationale (see §5.3).

**Preserve revocation semantics (HEP-CORE-0035 §I5).**  A revoked consumer stops being able to complete new CURVE handshakes.  The producer's ZAP handler stays the enforcement point; pre-confirm just closes the admit-side race.

---

## 3. Design decisions (locked)

- **D1: Consumer initiates a new `CONSUMER_ATTACH_REQ_ZMQ` wire** after receiving `CONSUMER_REG_ACK`, before dialing producer.  Mirrors SHM's `CONSUMER_ATTACH_REQ` shape.
- **D2: Broker holds the consumer's ATTACH REQ** until producer confirms cache-in-sync via `CHANNEL_AUTH_APPLIED_REQ`.  Adds one broker-side RTT to the consumer-attach path.
- **D3: Cache is the producer's SINGLE reference for allow/deny.**  ZAP handler always reads the cache.  Broker verifies cache-in-sync via bidirectional confirmation (`CHANNEL_AUTH_APPLIED_REQ`).

---

## 4. Wire spec

### 4.1 New wire: `CONSUMER_ATTACH_REQ_ZMQ` / `CONSUMER_ATTACH_ACK_ZMQ`

Direction: consumer → broker.  Sent after `CONSUMER_REG_ACK`, before dialing producer.

**Request:**
```json
{
  "channel_name":       "lab.sensors.temperature",
  "consumer_role_uid":  "cons.logger.uid00000001",
  "producer_role_uid":  "prod.mysensor.uid00000001"
}
```

For fan-in channels the consumer sends one ATTACH_REQ per producer serially (see §6.1).

**ACK payload — status enum:**

| status | reason | Consumer action |
|---|---|---|
| `success` | (empty) | Proceed to dial producer |
| `denied` | `consumer_not_in_channel_allowlist` | Non-retryable — script config error |
| `denied` | `producer_not_live` | Retry may succeed later if producer revives |
| `denied` | `channel_closing` | Non-retryable for this channel |
| `timeout` | `producer_did_not_confirm_within_budget` | Retry may succeed |

### 4.2 New wire: `CHANNEL_AUTH_APPLIED_REQ` / `_ACK`

Direction: producer → broker.  Sent after `ZmqQueue::set_peer_allowlist` succeeds.  The bidirectional confirmation locked in D3.

**Request:**
```json
{
  "channel_name":       "lab.sensors.temperature",
  "producer_role_uid":  "prod.mysensor.uid00000001",
  "instance_id":        7,
  "applied_version":    42
}
```

- `instance_id` — the producer's shift number (see §4.2a).  Broker rejects the message if it doesn't match the current registered instance for this producer, preventing stale-instance APPLIED_REQ from a crashed old producer instance from advancing broker state.
- `applied_version` — the `snapshot_version` from the preceding `GET_CHANNEL_AUTH_ACK`.  Broker uses it to advance `confirmed_version[K][P]`.

**Reply:**
```json
{
  "status":           "ok",
  "channel_name":     "lab.sensors.temperature",
  "applied_version":  42
}
```

Broker always replies `ok` on successful receipt (or silently drops on stale instance_id — producer's local ack timeout handles that path).  Ack is not fire-and-forget — the ack lets the producer detect a network-level broker unreachable event via producer-side timeout (producer's local action is unchanged either way: cache stays applied, log the outcome, continue).

### 4.2a `PRODUCER_REG_ACK` extension — instance_id

Direction: broker → producer.  Existing wire under HEP-CORE-0036; this amendment adds one integer field.

**Payload extension:**
```json
{
  ...existing REG_ACK fields...,
  "instance_id": 7
}
```

- `instance_id` — a monotonic `uint64_t` the broker assigns per registration (first-time or re-registration).  Broker tracks the current instance_id per producer role_uid; producer stores its assigned instance_id and echoes it on every `CHANNEL_AUTH_APPLIED_REQ` (§4.2).  Every re-registration increments the broker-side counter and returns a NEW instance_id.

The instance_id closes the "stale APPLIED_REQ from crashed producer instance" race described in §5.2.  Zero new round-trips: rides on the existing register handshake.

### 4.3 Reused wire — one payload extension

`GET_CHANNEL_AUTH_ACK` payload gains a `snapshot_version` integer field carrying the version the allowlist was snapshotted at.  Producer echoes it back as `applied_version` on `CHANNEL_AUTH_APPLIED_REQ`.  Name changes intentionally: on the ACK it's the SNAPSHOT (nothing applied yet); on the follow-up REQ it's the APPLIED confirmation.

Other reused wires unchanged: `CHANNEL_AUTH_CHANGED_NOTIFY`, `GET_CHANNEL_AUTH_REQ`.

---

## 5. Broker handler

### 5.1 Handler placement

`handle_consumer_attach_req_zmq` — new method on `BrokerServiceImpl`.  Dispatched from ROUTER poll loop parallel to the existing SHM `handle_consumer_attach_req` at `broker_service.cpp:1212`.

`handle_channel_auth_applied_req` — new method for the D3 confirmation wire.

### 5.2 State + notation

Broker tracks three counters:

- `channel_version[K]` — monotonic `uint64_t` per channel, bumped on ANY mutation to `ChannelAccessIndex[K]` (add / remove / any consumer pubkey change).  Shared across all producers on K.  Width MUST be at least 64 bits: at 1 mutation/ms, wraparound is >584 million years — practically infinite.
- `confirmed_version[K][P]` — per-(channel, producer) `uint64_t`; the highest version producer P's current instance has confirmed applying via `CHANNEL_AUTH_APPLIED_REQ`.  Reset to 0 on producer disconnect (ROUTER-observed / kDead heartbeat), producer re-registration, or broker restart.
- `instance[P]` — monotonic `uint64_t` per producer role_uid, incremented on every registration.  Broker returns the current value in `PRODUCER_REG_ACK` (§4.2a); producer echoes it on every `CHANNEL_AUTH_APPLIED_REQ` (§4.2).  Broker rejects any APPLIED_REQ whose `instance_id` doesn't match the current `instance[P]`.

Total broker state: `M + M·N + N` integers for M channels × N producers.  Cost of the instance guard is one extra integer per producer.

**Stale-instance guard (P4).**  The `instance[P]` counter closes the race where a crashed producer's leftover `CHANNEL_AUTH_APPLIED_REQ` (still in flight when the crash happened) advances `confirmed_version` under the NEW producer instance's empty cache.  After re-registration, `instance[P]` bumps; the leftover message from the old instance carries the OLD `instance_id`; broker's handler-flow check (§5.3) drops it.  No leak, no persistent false-success, regardless of message arrival order.  Cost: 1 nanosecond per APPLIED_REQ (integer compare), one integer in state, one integer field on two existing wire messages, no new round-trips.

**Fast-path invariant:** `confirmed_version[K][P] >= channel_version[K]` ⇒ P's ZAP cache reflects the current allowlist (which includes the consumer we're about to admit, since step 2 verified consumer's pubkey IS in the current allowlist).

**Simplification note:** the fast-path can be more conservative than strictly necessary.  If a consumer was admitted at version 10 but a different consumer joined at version 11, this consumer's fast-path check requires producer to be at version 11 (not just 10).  Cost: one extra wait-path cycle for the earlier consumer.  Benefit: no per-consumer version tracking; no pubkey-rotation-aware keying.  We accept this per the "simple over corner-perfect" principle.

### 5.3 Handler flow (normative)

```
On CONSUMER_ATTACH_REQ_ZMQ from consumer C for (K, P):

1. Validate payload shape.
2. If C.pubkey ∉ ChannelAccessIndex[K].authorized_consumer_pubkeys
   → reply {status="denied", reason="consumer_not_in_channel_allowlist"}. Done.
3. If ChannelEntry[K].producers[P].state ≠ kLive
   → reply {status="denied", reason="producer_not_live"}. Done.
4. If confirmed_version[K][P] >= channel_version[K]
   → reply {status="success"}. Done (fast path).
5. Wait path:
   a. Enqueue this REQ into ChannelEntry[K].producers[P].pending_attach_queue
      with target_version = channel_version[K] snapshotted at this moment.
      Start producer_apply_wait_ms timer on the entry.
   b. Fire CHANNEL_AUTH_CHANGED_NOTIFY to P.  Always fire — no debounce.
      Cost: fan-in of N consumers may generate N NOTIFYs.  ZMQ wire is
      cheap; the producer's pull chain coalesces naturally (each pull
      snapshots current state).
   c. Do NOT reply yet. Return to ROUTER loop.

On GET_CHANNEL_AUTH_REQ from P:
   a. Snapshot ChannelAccessIndex[K], stamp version W = channel_version[K].
   b. Reply GET_CHANNEL_AUTH_ACK{allowlist, snapshot_version=W}.

On CHANNEL_AUTH_APPLIED_REQ from P at applied_version=W, instance_id=I:
   a. Stale-instance guard: if I ≠ instance[P] → silently drop (message is from a
      crashed prior instance of P; do not touch broker state).  Done.
   b. Reply {status="ok", channel_name=K, applied_version=W}.
   c. confirmed_version[K][P] = max(confirmed_version[K][P], W).
   d. Walk pending_attach_queue: for entries with target_version ≤ confirmed_version[K][P],
      reply {status="success"} and remove from queue.

On pending-entry timeout (producer_apply_wait_ms elapsed):
   → reply {status="timeout", reason="producer_did_not_confirm_within_budget"}.

On producer P disconnect (ROUTER event / kDead transition):
   → confirmed_version[K][*disconnected P*] = 0.
   → walk P's pending_attach_queue: reply {status="denied", reason="producer_not_live"}
     (unified reason with dispatch-time denial — no timing-boundary distinction).

On producer P registration or re-registration under same role_uid:
   → instance[P] += 1  (assign fresh instance number).
   → confirmed_version[K][P] = 0 for all K covered by P.
   → Reply PRODUCER_REG_ACK with the new instance_id (§4.2a).
   → New instance's ZAP cache is presumed empty; next ATTACH_REQ takes wait-path.
   → Any subsequent APPLIED_REQ carrying the OLD instance_id (from crashed prior instance,
     still in flight) is silently dropped by the stale-instance guard above.

On channel K close:
   → delete ChannelAccessIndex[K], channel_version[K], all confirmed_version[K][*].
   → walk pending_attach_queues for K: reply {status="denied", reason="channel_closing"}.
```

### 5.4 Why this is NOT the retracted "broker as sync-request initiator" pattern

The 2026-06-04 amendment (§6.5) retracted having the broker be a sync-request INITIATOR — broker sends first, then waits for reply on the same ROUTER it responds on.

This amendment has the broker be a sync-response HOLDER for a specific inbound REQ, waiting for a subsequent producer-initiated REQ (`CHANNEL_AUTH_APPLIED_REQ`).  Between the consumer's ATTACH REQ arriving and the producer's APPLIED REQ arriving, the broker's ROUTER poll loop handles the intermediate `GET_CHANNEL_AUTH_REQ/ACK` exchange and any other unrelated traffic normally.

Distinctions from the retracted design:
- Broker never initiates.  Every wire event on broker's ROUTER is inbound REQ or outbound REP.
- Broker's poll loop keeps draining normally; the pending REQ just doesn't get replied-to yet.
- The APPLIED_REQ is a NORMAL producer-initiated REQ (like HEARTBEAT_REQ) — broker just notes arrival.

### 5.5 Timeout + failure taxonomy

| status | reason(s) | source | consumer action |
|---|---|---|---|
| `success` | (empty) | Fast-path or drained-from-queue | Proceed to dial |
| `denied` | `consumer_not_in_channel_allowlist` | ChannelAccessIndex check (step 2) | Non-retryable |
| `denied` | `producer_not_live` | Step 3 OR pending-queue drain on producer disconnect | Retry may succeed later |
| `denied` | `channel_closing` | Channel-close raced with attach | Non-retryable for this channel |
| `timeout` | `producer_did_not_confirm_within_budget` | `producer_apply_wait_ms` elapsed | Retry may succeed |

Wire-level statuses: fixed three (`success` / `denied` / `timeout`).  Reason strings enumerated here.

**Budget defaults + ordering invariant (NORMATIVE):**
- Consumer-side ATTACH_REQ timeout: `attach_ack_wait_ms` = 5000 ms.
- Broker-side producer-apply wait: `producer_apply_wait_ms` = 3000 ms.
- Producer-side APPLIED_ACK timeout: `applied_ack_wait_ms` = 1000 ms.
- Invariant: `attach_ack_wait_ms > producer_apply_wait_ms` (config-validation check at load recommended).

---

## 6. Consumer + producer side flows

### 6.1 Consumer role side (`RoleAPIBase::apply_consumer_reg_ack`, ZMQ branch)

**Fan-in policy:** best-effort — loop always runs to completion; attach fails only when ZERO producers admitted.

```
apply_consumer_reg_ack(REG_ACK ack):
  if ack.data_transport != "zmq": return  # (SHM path unchanged)

  N = ack.producers.size()
  LOGGER_INFO("[{}] channel={} attach loop begin: {} producer(s) ({})",
              short_tag, ack.channel_name, N, N>1?"fan-in":"single")

  per_producer_outcome = {}   # {uid → (status, reason)}
  connected = []
  for i, producer in enumerate(ack.producers):
    attach_ack = brc.request(CONSUMER_ATTACH_REQ_ZMQ{
      channel_name: ack.channel_name,
      consumer_role_uid: pImpl->uid,
      producer_role_uid: producer.role_uid
    }, timeout_ms=attach_ack_wait_ms)
    # If broker never replied (client-side BRC timeout), synthesize the SAME §5.5 reason
    # used for broker-observed timeouts — the two failure modes are indistinguishable from
    # the script's perspective (both mean "producer didn't confirm; retry may help") and
    # the reason string enum is closed to §5.5 taxonomy per §7.6.
    if attach_ack IS NULL:
      attach_ack = {status: "timeout", reason: "producer_did_not_confirm_within_budget"}
    per_producer_outcome[producer.role_uid] = (attach_ack.status, attach_ack.reason ?? "")
    if attach_ack.status == "success":
      LOGGER_INFO("[{}] channel={} producer[{}/{}]={} attach: success",
                  short_tag, ack.channel_name, i+1, N, producer.role_uid)
      connected.push_back(producer)
    else:
      LOGGER_WARN("[{}] channel={} producer[{}/{}]={} attach: {} ({})",
                  short_tag, ack.channel_name, i+1, N,
                  producer.role_uid, attach_ack.status, attach_ack.reason)
      # continue loop — best-effort

  ok = connected.size()
  fail = N - ok
  if ok == 0:
    LOGGER_ERROR("[{}] channel={} attach loop complete: 0/{} admitted, channel unusable",
                 short_tag, ack.channel_name, N)
    return false

  if fail > 0:
    LOGGER_WARN("[{}] channel={} attach loop complete: {}/{} admitted, {} failed (partial success)",
                short_tag, ack.channel_name, ok, N, fail)
  else:
    LOGGER_INFO("[{}] channel={} attach loop complete: {}/{} admitted (full success)",
                short_tag, ack.channel_name, ok, N)

  ConsumerChannelState[ack.channel_name].producer_attach_outcome = per_producer_outcome
  ConsumerChannelState[ack.channel_name].producers_declared = ack.producers   # for observability
  set_producer_peers(connected)                                                # admitted set only
  start()
```

**Rationale for failed producers being excluded from `set_producer_peers`:** consumer is CURVE CLIENT.  Consumer's PULL socket has no ZAP handler; the exclusion means consumer's PULL never has an endpoint for excluded producer P — no CURVE dial toward P is ever initiated.

### 6.2 Producer role side

**Producer-side state (added under this amendment):**
- `pImpl->instance_id` — assigned by broker at registration (echoed from PRODUCER_REG_ACK); echoed on every CHANNEL_AUTH_APPLIED_REQ.  Overwritten on any subsequent re-registration.

```
on PRODUCER_REG_ACK(ack):
  pImpl->instance_id = ack.instance_id   # from §4.2a; store for later APPLIED_REQs
  # ... other existing REG_ACK handling ...

on GET_CHANNEL_AUTH_ACK(ack):
  ok = ZmqQueue::set_peer_allowlist(ack.allowlist)
  if not ok:
    # Queue impl must be atomic on failure: cache stays at pre-call state.
    # Producer logs ERROR; no APPLIED_REQ sent.  Broker will observe as timeout
    # for any pending ATTACH REQs against this producer.
    LOGGER_ERROR("[{}] set_peer_allowlist failed for channel {} — cache unchanged",
                 short_tag, ack.channel_name)
    return

  applied_ack = brc.request(CHANNEL_AUTH_APPLIED_REQ{
    channel_name: ack.channel_name,
    producer_role_uid: pImpl->uid,
    instance_id: pImpl->instance_id,
    applied_version: ack.snapshot_version
  }, timeout_ms=applied_ack_wait_ms)

  # Guard null (client-side timeout — brc.request returned no reply within
  # applied_ack_wait_ms).  Cache STAYS applied locally either way; broker will
  # re-drive on next NOTIFY cycle if needed.  Mirrors §6.1 null-synthesis pattern.
  if applied_ack IS NULL:
    LOGGER_WARN("[{}] pre-attach: no ack from broker for channel {} v{} — cache preserved",
                short_tag, ack.channel_name, ack.snapshot_version)
    return

  if applied_ack.status == "ok":
    LOGGER_INFO("[{}] pre-attach: applied allowlist v{} (size={}, channel={})",
                short_tag, ack.snapshot_version, ack.allowlist.size(), ack.channel_name)
```

### 6.3 Log discipline (normative markers)

The attach-loop lines are test-contract-stable markers.  Format:

| Event | Level | Format |
|---|---|---|
| Loop begin | INFO | `channel={K} attach loop begin: {N} producer(s) ({single\|fan-in})` |
| Per-producer success | INFO | `channel={K} producer[{i}/{N}]={uid} attach: success` |
| Per-producer failure | WARN | `channel={K} producer[{i}/{N}]={uid} attach: {status} ({reason})` |
| Loop complete (partial) | WARN | `channel={K} attach loop complete: {ok}/{N} admitted, {fail} failed (partial success)` |
| Loop complete (full) | INFO | `channel={K} attach loop complete: {ok}/{N} admitted (full success)` |
| Loop complete (all fail) | ERROR | `channel={K} attach loop complete: 0/{N} admitted, channel unusable` |

`producer(s)` renders as `producer` when N=1, `producers` otherwise.

---

## 7. Design decisions — remaining questions for you

Tag `[user]` = your call; tag `[me]` = I resolve at Phase 1 promotion.

### 7.1 D3 ✅ RESOLVED 2026-06-30

Cache is producer's SINGLE reference for allow/deny; admit + revoke both use bidirectional confirmation (§4.2).  ZAP handler stays load-bearing.

### 7.2 Fan-in serialization

Consumer serializes per-producer ATTACH REQ (§6.1).  Worst-case for 10-producer fan-in: ~30 s (broker replies at server-side budget) to ~50 s (silent broker → client-side budget).  Acceptable for MVP?

Recommendation: **MVP serial.**  Concurrent alternatives deferred as follow-on tasks.

### 7.3 Which HEP owns the amendment?

Recommendation: **HEP-CORE-0041 Phase 5** with a §6.5.2 pointer stub in HEP-CORE-0036.  Reason: pre-confirm pattern IS HEP-0041's central design.

### 7.4 Fan-in partial-success policy

Recommendation LOCK:
- Loop always runs to completion.
- Loop-level failure returns `false` ONLY when ZERO producers were admitted.
- Failed producers excluded from `set_producer_peers()` — consumer never dials them.
- No automatic retry within the loop; failed producers retry on next consumer restart.

### 7.5 Per-failure callback

Recommendation: query-only for MVP via `api.producer_attach_status(channel, uid)` — no new callback.

### 7.6 Script observation surface — Shape A (decomposed) chosen

- `api.producers_declared(channel) → list<uid>` — REG_ACK-declared set.
- `api.producers_connected(channel) → list<uid>` — admitted subset.
- `api.producer_attach_status(channel, uid) → enum` — `success` / `denied` / `timeout` / `not_declared`.
- `api.producer_attach_reason(channel, uid) → string` — reason from §5.5 taxonomy.

### 7.7 Log discipline — lock §6.3 as normative

Recommendation: promote §6.3 verbatim to HEP-CORE-0041 §Phase-5.log-format as normative table.  Confirm.

### 7.8 Channel-ready callback wiring

§10's HEP-CORE-0011 bullet assumes `on_channel_ready(channel)` exists.  Resolution:
- **(a)** callback already exists in HEP-CORE-0011 → use as-is.
- **(b)** add new callback in this amendment's Phase 4 (cross-engine parity work).
- **(c)** hook `on_start` + poll `api.is_channel_ready()`.

Recommendation: **(a) → (b) → (c) emergency-only.**  I verify at Phase 1 promotion.

---

## 8. Test plan

**L2 broker unit tests** (extend `test_broker_service.cpp`):

- `AttachReqZmq_FastPath_WhenConfirmedVersionCovers` — `confirmed_version[K][P] >= channel_version[K]` → immediate success.
- `AttachReqZmq_WaitPath_WhenProducerBehind` — cache-behind → fires NOTIFY, holds REQ.
- `AttachReqZmq_DrainsPendingOnAppliedReq` — after producer's APPLIED_REQ, pending consumers drain.
- `AttachReqZmq_TimeoutReplyWhenProducerSilent` — timeout budget elapsed → status="timeout".
- `AttachReqZmq_DeniedWhenNotInAllowlist` — pubkey not in ChannelAccessIndex → status="denied".
- `AttachReqZmq_DeniedWhenProducerNotLive` — producer kDead / disconnected AT DISPATCH TIME (step 3) → status="denied", reason="producer_not_live".
- `AttachReqZmq_DeniedWhenProducerDisconnectsWhilePending` — pins the §5.3 post-enqueue disconnect drain path.  ATTACH_REQ enqueued while P is kLive; P disconnects; verify queued REQ is drained as denied (reason="producer_not_live").
- `AttachReqZmq_PartialDrainOnAppliedReq` — pins the §5.3 partial-drain path.  Multi-entry queue with mixed target_versions W1 < W2 < W3; APPLIED_REQ arrives at W2; verify entries at W1 and W2 drain as success, entry at W3 remains queued (drains on next APPLIED_REQ or times out).
- `AppliedReq_ResetsAfterProducerReRegistration` — pins the §5.3 producer re-registration reset.  Advance confirmed_version[K][P] to N via APPLIED_REQ; P re-registers under same role_uid; verify confirmed_version[K][P] = 0, instance[P] bumped, next ATTACH_REQ takes wait-path.
- `AppliedReq_FromStaleInstance_SilentlyDropped` — pins the §4.2/§5.3 stale-instance guard.  Simulate a producer crash + re-registration; inject an APPLIED_REQ carrying the OLD instance_id after the re-registration; verify: broker does NOT advance confirmed_version, broker does NOT reply to producer (silent drop), no pending ATTACH_REQ drains as spurious success.  This test proves the P4 stale-instance guard closes the race regardless of message ordering.
- `AppliedReq_AdvancesConfirmedVersion` — receives APPLIED_REQ, confirmed_version advances.
- `ChannelClose_DrainsPendingAsDenied` — channel closure while pending → status="denied", reason="channel_closing".

**L3 sequence pin** (extend `test_datahub_broker_protocol.cpp`):

- `PreConfirmSequenceEliminatesRace_ZmqConsumerDialsAfterProducerCacheReady` — admit-path pin.  Verify `set_peer_allowlist` fires BEFORE consumer's `pull_from` dial via log-marker sequencing.
- `RevokePropagation_ConfirmedVersionAdvancesOnAppliedReq` — revoke-path pin.  Confirmed_version advances past the revoke's version only after producer's post-revoke APPLIED_REQ.

**L3 fan-in coverage:**
- `AttachLoop_PartialSuccess_FanIn3Producers1Denied` — verify best-effort loop; 2 admitted, 1 denied; `api.producer_attach_status` returns correct enum per producer.
- `AttachLoop_AllFail_FanIn3ProducersAllDenied` — verify `apply_consumer_reg_ack` returns false; ERROR log line emitted.
- `AttachLoop_LogMarkers_AllPhasesPinned` — assert exact §6.3 log-line strings.

**L4 e2e:**
- Remove `GTEST_SKIP` from `PlhHubCliTest.ZmqE2E_AuthorizedConsumerReceivesAllSlots`.  Verify green.
- `ZmqE2E_FanIn_PartialSuccessDataFlowsFromAdmittedSubset` — 3-producer fan-in, 1 denied; verify data flows from 2 admitted.

---

## 9. Phased implementation

Each phase ships green before the next.

- **Phase 1: HEP promotion.**  Merge this tech_draft into `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` (§Phase-5 section) + cross-ref stub in HEP-CORE-0036 §6.5.2.  No code changes.  Pre-conditions: §7.3, §7.8 resolved (§7.2, §7.4, §7.5, §7.6, §7.7 recommendations accepted or redirected).
- **Phase 2: Broker side.**  Add `handle_consumer_attach_req_zmq` + `handle_channel_auth_applied_req`.  `GET_CHANNEL_AUTH_ACK` gains `snapshot_version`.  L2 tests pin state machine.
- **Phase 3: Producer side.**  `GET_CHANNEL_AUTH_ACK` handler sends `CHANNEL_AUTH_APPLIED_REQ` after `set_peer_allowlist`.  Fleet-wide rollout MUST complete before Phase 4 starts.
- **Phase 4: Consumer side.**  `RoleAPIBase::apply_consumer_reg_ack` ZMQ branch gains pre-confirm loop.
- **Phase 5: L4 unskip + L3 sequence pin.**  Remove `GTEST_SKIP`; add sequence-pin workers.
- **Phase 6: REVIEW-D gate.**  Full systematic review of the pre-confirm arc.

**Rollout constraint (per Design Principles):** Phase 3 (producer) MUST complete before Phase 4 (consumer).  Mixed fleet not supported.  Deploy discipline replaces per-producer capability negotiation.

---

## 10. Sibling HEP updates (Phase 1 promotion enumeration)

Merge plan per HEP.  Paste-ready contract text below — no wordsmithing at Phase 1 execution time.

### 10.1 HEP-CORE-0041 §Phase-5 (authoritative home)

Merge §0, §4, §5, §6 substance verbatim.  §7-§9 do NOT promote (pre-promotion artifacts; open questions become resolved-and-inlined text during Phase 1).  §12 sequence diagram promotes.  §11 (scope discipline) becomes §Phase-5.scope-discipline.  §Design Principles + guardrails table promote to §Phase-5.design-principles as the operational scope for future §Phase-5 amendments.

### 10.2 HEP-CORE-0036 §6.5.2 (cross-reference stub)

New subsection §6.5.2, paste-ready:

> **§6.5.2 Pre-attach admission for ZMQ.**  This section (§6.5) specifies the doorbell-then-pull runtime allowlist synchronization protocol between broker and producers.  A distinct concern — pre-attach admission gating that prevents a fresh consumer from dialing a producer whose ZAP cache does not yet contain the consumer's pubkey — is specified in **HEP-CORE-0041 §Phase-5** (ZMQ pre-attach broker confirmation).  §6.5's `CHANNEL_AUTH_CHANGED_NOTIFY` and `GET_CHANNEL_AUTH_REQ/ACK` wires are reused unchanged by §Phase-5; the additions are `CONSUMER_ATTACH_REQ_ZMQ/_ACK` (consumer↔broker) and `CHANNEL_AUTH_APPLIED_REQ/_ACK` (producer↔broker).  See HEP-CORE-0041 §Phase-5 for the full protocol.

### 10.3 HEP-CORE-0035 §I5 (revocation semantics)

Append this paragraph to §I5, paste-ready:

> **§I5 revocation confirmation under HEP-CORE-0041 §Phase-5 (ZMQ pre-attach).**  For channels using the ZMQ pre-attach protocol, revocation propagation is confirmed via `CHANNEL_AUTH_APPLIED_REQ` (see HEP-CORE-0041 §Phase-5).  Broker maintains `confirmed_version[K][P]` as the highest allowlist snapshot version each producer's current instance has confirmed applying.  A revoke is committed at producer P once `confirmed_version[K][P] >= channel_version[K]` where `channel_version[K]` is the post-revoke bump.  Existing CURVE sessions per §I5 baseline stay up on revoke (per libzmq semantics); new handshakes deny at the producer's ZAP handler because the cache — populated by the confirmed pull — no longer contains the revoked pubkey.  The producer-instance-epoch guard (§Phase-5 §4.2a `instance_id`) prevents stale APPLIED_REQ from a crashed producer instance from falsely committing a revoke.

### 10.4 HEP-CORE-0011 (script callback contract — CONTINGENT on §7.8)

⚠️ **Text depends on §7.8 resolution.**  Three paste-ready alternatives:

**Alternative (a) — `on_channel_ready(channel)` already exists in HEP-0011:**  add this subsection under the existing callback definition:

> **Fan-in channels — script responsibility (HEP-CORE-0041 §Phase-5).**  When `on_channel_ready(channel)` fires for a consumer role on a fan-in ZMQ channel, the channel may have partial producer admission — some producers admitted, others denied or timed out.  The script is RESPONSIBLE for querying `api.producers_declared(channel)` and comparing against `api.producers_connected(channel)` (see HEP-CORE-0028) to decide whether the admitted subset is acceptable.  The framework does NOT encode a policy — a script that requires all-N producers must check and decide (continue with degraded coverage or call `api.channel_stop(channel)`).

**Alternative (b) — new `on_channel_ready` callback added under this amendment:**  same body as (a); the callback definition itself is a new addition to HEP-0011's callback table, with cross-engine binding work landing in Phase 4.

**Alternative (c) — no callback, script uses `on_start` + `api.is_channel_ready()` polling:**

> **Fan-in channels — script responsibility (HEP-CORE-0041 §Phase-5).**  For consumer role scripts on fan-in ZMQ channels, the `on_start` callback should invoke `api.is_channel_ready(channel)` polling until ready (or timeout).  Once ready, query `api.producers_declared(channel)` and `api.producers_connected(channel)` (HEP-CORE-0028) to inspect per-producer admission outcome.  The framework does NOT encode a policy — script decides whether the admitted subset is acceptable.

### 10.5 HEP-CORE-0028 (Native ABI + script-surface parity)

Four new API entries need parity across Lua / Python / Native.  Text for the API contract subsection (add to HEP-0028's "Script API reference"):

> **`api.producers_declared(channel) → list<string>`**  
> Returns the list of producer `role_uid` strings declared in the consumer's most recent `CONSUMER_REG_ACK` for the given channel.  Reflects the broker's view of "producers this consumer was told to expect."  Read-only; no mutation surface.  Available on consumer role hosts only; returns empty list on other roles.
>
> **`api.producers_connected(channel) → list<string>`**  
> Returns the subset of `producers_declared` whose pre-attach handshake succeeded (see HEP-CORE-0041 §Phase-5).  Populated once the channel-ready callback / signal fires.  Read-only.
>
> **`api.producer_attach_status(channel, uid) → string`**  
> Returns the enumerated wire status for the given producer's pre-attach outcome: one of `"success"` / `"denied"` / `"timeout"` (per HEP-CORE-0041 §Phase-5 §4.1).  Returns `"not_declared"` if `uid` was not in `producers_declared`.
>
> **`api.producer_attach_reason(channel, uid) → string`**  
> Returns the enumerated reason string per HEP-CORE-0041 §Phase-5 §5.5 taxonomy: `"consumer_not_in_channel_allowlist"` / `"producer_not_live"` / `"channel_closing"` / `"producer_did_not_confirm_within_budget"` / `""` (empty on success).

Native C ABI version bump.  Impl work: Lua binding + Python binding + Native binding all touched.  Estimated ~1 day cross-engine parity.

### 10.6 docs/todo/AUTH_TODO.md

Task #246 status updates: "amendment retrofit" → "HEP-CORE-0041 §Phase-5 shipped."  Task description points at the merged §Phase-5.

### 10.7 Follow-on tasks (created at Phase 1 promotion)

- Test-infrastructure task: build/extend a broker-side test helper to synthesize `CHANNEL_AUTH_APPLIED_REQ` payloads with arbitrary `instance_id` values.  Required by `AppliedReq_FromStaleInstance_SilentlyDropped` (§8).  Estimated small (~half a day).

---

## 11. What this amendment does NOT do (scope discipline)

- **Does not touch #262 (mutual auth).**  Producer→consumer proof-of-possession is a separate wire (3rd handshake frame).  Orthogonal.
- **Does not add a broker-hosted ZAP handler.**  libzmq supports it; not needed here.
- **Does not eliminate the producer's ZAP allowlist cache.**  Cache remains authoritative under D3; pre-confirm just guarantees it's populated at handshake time.
- **Does not change SHM's pre-attach protocol.**  This amendment mirrors HEP-0041 §5.5 shape onto ZMQ.
- **Does not build mixed-fleet coexistence.**  Deploy discipline handles rollout ordering.
- **Does not handle transient bad-network conditions gracefully.**  Retry (script-level or operator-level) is the recovery mechanism.  Per Design Principles.

---

## 12. Sequence diagram

```mermaid
sequenceDiagram
    autonumber
    participant C as Consumer<br/>(role_host)
    participant B as Broker
    participant P as Producer<br/>(role_host)

    C->>B: CONSUMER_REG_REQ
    B-->>C: CONSUMER_REG_ACK{producers[]}
    Note over C,P: Pre-amendment: C would dial P now (RACE)

    rect rgba(220,240,255,0.5)
        Note over C,B: This amendment
        C->>B: CONSUMER_ATTACH_REQ_ZMQ{channel, C.uid, P.uid}
        alt fast path (confirmed_version >= channel_version)
            B-->>C: CONSUMER_ATTACH_ACK_ZMQ{status="success"}
        else wait path
            Note over B: enqueue REQ,<br/>target_version = channel_version[K]
            B->>P: CHANNEL_AUTH_CHANGED_NOTIFY{channel}
            P->>B: GET_CHANNEL_AUTH_REQ
            B-->>P: GET_CHANNEL_AUTH_ACK{allowlist, snapshot_version=W}
            Note over P: set_peer_allowlist(v=W)
            P->>B: CHANNEL_AUTH_APPLIED_REQ{channel, instance_id=I, applied_version=W}
            Note over B: check instance_id=I matches instance[P]; else drop
            B-->>P: CHANNEL_AUTH_APPLIED_ACK{status="ok", channel_name, applied_version=W}
            Note over B: confirmed_version[K][P] = W;<br/>drain queue
            B-->>C: CONSUMER_ATTACH_ACK_ZMQ{status="success"}
        end
    end

    C->>P: ZMQ PULL dial (CURVE handshake — cache in sync)
    P-->>C: handshake succeeds; data flows

    Note over C,P: --- Fan-in: C repeats §4.1 loop per producer serially ---
    Note over C,P: --- Revoke: same NOTIFY+pull+APPLIED_REQ cycle re-syncs cache ---
```

---

## 13. Open discussion items

**Resolved:**
- ✅ §7.1: D3-corrected LOCKED 2026-06-30.

**Awaiting your call:**
- `[user]` §7.2: fan-in serialization — MVP serial acceptable?
- `[user]` §7.3: HEP-0041 Phase 5 as authoritative home.
- `[user]` §7.4: fan-in partial-success policy — best-effort continue?
- `[user]` §7.5: per-failure callback — query-only for MVP?
- `[user]` §7.6: script observation surface shape.
- `[user]` §7.7: lock §6.3 log format as normative.
- `[user]` §7.8: channel-ready callback wiring — (a) → (b) → (c) order?

**I'll resolve at Phase 1 promotion:**
- `[me]` §4.1 + §4.2: payload field-name choices — align with existing HEP-0036 §6.4 shapes.
- `[me]` §5.5: timeout defaults — align with HEP-0041 SHM prior-art.
- `[me]` §10: contract-text merge plan for each permanent HEP.
