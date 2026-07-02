# HEP-CORE-0042: Channel Attach Coordination Protocol

## 1. Status banner

**Adopted:** 2026-07-01 (promoted from `docs/tech_draft/DRAFT_HEP-0036_zmq-pre-confirm_2026-06-30.md`, task #246 Phase 1).

**Scope.**  Transport-agnostic coordination protocol for channel attach.  Specifies how the broker mediates a channel attach between a producer and a consumer so that the consumer's data-plane handshake (CURVE for ZMQ, `crypto_box` for SHM) succeeds against a producer whose per-connection auth cache reflects the broker's current allowlist.

Concrete transport instantiations of the coordination protocol live under §6 "Bindings":
- **§6.1 Bindings.SHM** — HEP-CORE-0041 `CONSUMER_ATTACH_REQ_SHM` (memfd + `SCM_RIGHTS` capability fd via Unix socket).
- **§6.2 Bindings.ZMQ** — this HEP's original substance: `CONSUMER_ATTACH_REQ_ZMQ` + `CHANNEL_AUTH_APPLIED_REQ` (bidirectional broker↔producer confirmation) + producer-instance-epoch guard.

**Relationship to sibling HEPs.**

| HEP | Owns | This HEP |
|---|---|---|
| HEP-CORE-0036 | Auth framework foundation: invariants (§I1-§I12), REG/DEREG wire schemas, `CHANNEL_AUTH_CHANGED_NOTIFY` + `GET_CHANNEL_AUTH_REQ` mechanism (§6.5).  `PeerAllowlist` / `ChannelAccessIndex` definition + mutation rules. | Reads.  Reuses §6.5 doorbell-then-pull as the substrate the pre-confirm layer builds on. |
| HEP-CORE-0041 | SHM transport specifics: memfd + `SCM_RIGHTS` capability transport, `crypto_box` challenge-response (L2 attach-time handshake, §5.5), per-platform backends (§6.5). | Cross-linked.  HEP-0041 §5.4 CONSUMER_ATTACH_REQ_SHM moved here as §6.1 Bindings.SHM.  HEP-0041 §5.5 `crypto_box` stays there (SHM-transport-specific crypto). |
| HEP-CORE-0035 | Hub-role auth + federation trust; key-file storage. | Reads.  §I5 revocation semantics get a paragraph pointing at this HEP for confirmation timing. |
| HEP-CORE-0011 | Script host abstraction + callback contract. | Adds `on_channel_ready(channel)` callback (§10.4 of promotion source; landed under this HEP's §8 script-facing surface). |
| HEP-CORE-0028 | Native plugin engine + script API reference. | Adds four accessors (`producers_declared`, `producers_connected`, `producer_attach_status`, `producer_attach_reason`) — §8 documents them here; HEP-0028 documents the C ABI shape. |

---

## 2. Motivation

Consumer role dials producer role over the data plane (ZMQ CURVE / SHM Unix socket).  The producer's data-plane socket enforces authorization via a per-connection cache — for ZMQ, the ZAP handler consults the cache; for SHM, the accept-thread validates via `crypto_box` against the cached peer allowlist.

The broker knows the authoritative allowlist (`ChannelAccessIndex[K].authorized_consumer_pubkeys`).  The producer's cache lags: consumers can register (and get admitted to the allowlist at the broker) FASTER than the producer can pull the update via the existing `CHANNEL_AUTH_CHANGED_NOTIFY` doorbell-then-pull mechanism (HEP-CORE-0036 §6.5).

Without a coordination step, the consumer's data-plane handshake can lose to a producer cache-miss race:
- Consumer dials producer.
- Producer's data-plane socket does auth check against its cache.
- Consumer is in the broker's allowlist but not yet in the producer's cache.
- Handshake fails.  Consumer sees "auth denied" for a consumer that IS authorized.

This HEP specifies the coordination step: before the consumer dials, the broker CONFIRMS the producer's cache reflects the current allowlist, waiting one doorbell-then-pull cycle if not.

---

## 3. Design goal

Consumer NEVER dials until the broker confirms the producer's cache is caught up.  If the cache is not caught up, the broker holds the consumer's attach request, fires a `CHANNEL_AUTH_CHANGED_NOTIFY` doorbell to the producer, waits for the producer's `CHANNEL_AUTH_APPLIED_REQ` acknowledgment, then replies to the consumer with "go ahead."

Failure modes:
- Producer is dead → broker denies immediately.
- Producer doesn't confirm within budget → broker replies "timeout"; consumer retries.

No new state machines on either role; no debounce; no capability negotiation.

---

## 4. Design principles (guardrails for future amendments)

Any amendment to this HEP MUST be checked against these principles.  A finding that "corner case X isn't handled" is CLOSED as accepted-degradation unless it touches P4 or is a plain-and-simple mistake.

- **P0 — Network stability assumption.**  The protocol assumes a stable local-network deployment (single-host or trusted LAN).  Under bad network, the protocol degrades to timeouts + user-visible retry.  Adding retry loops / debouncing / aging is out-of-scope.
- **P1 — One wait-path cycle per rare event.**  Rare state changes (fresh producer, pubkey rotation, revoke) cost EXACTLY ONE additional doorbell-then-pull cycle before consumer attach succeeds.  Amendments that shave this to zero via caching are OUT of scope UNLESS they preserve P4.
- **P2 — Simple over corner-perfect.**  Given a choice between adding state to handle a rare corner and accepting a bounded degradation, prefer the accepted degradation.
- **P3 — Bad network = degraded attach experience.**  Timeouts on producer confirmation, message loss, etc., result in `status="timeout"` on the consumer's attach ACK and expected user-script retry (existing retry conventions).  Not a supported operating mode.
- **P4 — Security correctness invariant (the ONE invariant not weakened).**  A consumer's data-plane handshake against producer P succeeds if-and-only-if the broker has confirmed P's per-connection cache holds the consumer's pubkey.  Direction A ("succeeds → confirmed") is the security direction and is invariant.  Direction B ("confirmed → succeeds") is a liveness property; transient violations under exotic network reordering are acceptable per P3.

### Review guardrails — worry / don't worry list

| DO worry about (Category A: real bugs) | DON'T worry about (Category B: accepted-degradation) |
|---|---|
| Fast-path admits a consumer whose pubkey ISN'T in producer's cache (P4 leak) | Multiple pending consumers for same (K, P) — serialize acceptable |
| Handler flow contradicts itself between §4.3 and §6 bindings | NOTIFY debounce, aging, monotonic-clock discipline |
| Sequence diagram vs prose disagreement | Producer-restart contract beyond §4.2 reset rules |
| Cross-section drift (§5 wire fields vs §4.3 handler references) | Consumer-restart contract beyond §7.1 retry semantics |
| P4 violation under any timing scenario (excluding P3-accepted degradation) | Mixed-version fleet during Phase 2/3 rollout (single framework, single upgrade) |
| Timeout budget invariants (`attach_ack_wait_ms > producer_apply_wait_ms`) | Field-level backward compatibility on new wires |
| Missing test coverage for a §4.3-specified path | Optimization of fan-in serialization (accepted MVP-serial cost) |

**Meta-rule.**  If a reviewer's finding requires adding new state (counter, timer, capability negotiation, retry loop, distinct enum), CHECK guardrails first.  If the case falls under P0/P1/P2/P3, close as accepted-degradation.  Only P4 or plain-and-simple mistakes warrant edits.

---

## 5. Abstract protocol

### 5.0 Scope of the abstract protocol

The abstract protocol below (state model §5.2, handler flow §5.4, wire spec §5.5, timeout budgets §5.6) describes the case where the **producer maintains a per-connection auth cache that can lag** the broker's authoritative allowlist.  This is the ZMQ instantiation (§6.2 Bindings.ZMQ) — the producer's ZAP handler consults the cache on every CURVE handshake, and the cache must be re-synced via `CHANNEL_AUTH_CHANGED_NOTIFY` + `GET_CHANNEL_AUTH_REQ` after each mutation.  The `channel_version` / `confirmed_version` / `instance_id` state exists to close the race between broker mutation and producer cache update.

The SHM instantiation (§6.1 Bindings.SHM) is a **stateless degenerate case**: SHM's `CONSUMER_ATTACH_REQ_SHM` is producer-initiated on EVERY attach attempt, and the broker's reply reflects the current `ChannelAccessIndex[K]` at query time.  No producer-side cache lags; therefore no `confirmed_version` state and no `CHANNEL_AUTH_APPLIED_REQ` handshake are needed.  What SHM shares with ZMQ is the *coordination principle* — the broker mediates the attach, not the producer's own local check — plus the transport-agnostic wire shape (`consumer_pubkey` / `producer_role_uid` / `channel_name`, common reason strings on denial).

Downstream sections in §5 apply to the ZMQ instantiation.  §6.1 documents the SHM instantiation as a compact per-attempt query without redundantly restating the state model.

### 5.1 One-sentence mental model

Broker tracks a monotonic version per channel and a confirmed version per (channel, producer) pair.  When a consumer needs to attach and the producer isn't caught up, ONE `CHANNEL_AUTH_CHANGED_NOTIFY`-triggered pull cycle syncs them.  Bad network = timeout + user retry.  Producer restart = new instance epoch (no cross-instance leak).

### 5.2 State model

Broker tracks three counters:

- `channel_version[K]` — monotonic `uint64_t` per channel, bumped on ANY mutation to `ChannelAccessIndex[K]` (add / remove / any consumer pubkey change).  Shared across all producers on channel K.  Width MUST be at least 64 bits: at 1 mutation/ms, wraparound is >584 million years — practically infinite.
- `confirmed_version[K][P]` — per-(channel, producer) `uint64_t`; the highest allowlist snapshot version producer P's current instance has confirmed applying via `CHANNEL_AUTH_APPLIED_REQ`.  Reset to 0 on producer P disconnect (ROUTER-observed / kDead heartbeat), producer re-registration, or broker restart.
- `instance[P]` — monotonic `uint64_t` per producer role_uid, incremented on every registration.  Broker returns the current value in `PRODUCER_REG_ACK` (§5.5.3); producer echoes it on every `CHANNEL_AUTH_APPLIED_REQ` (§5.5.2).  Broker rejects any APPLIED_REQ whose `instance_id` doesn't match the current `instance[P]`.

**Total broker state:** `M + M·N + N` integers for M channels × N producers.  The instance guard adds one integer per producer.

**Stale-instance guard (P4).**  The `instance[P]` counter closes the race where a crashed producer's leftover `CHANNEL_AUTH_APPLIED_REQ` (still in flight at crash time) advances `confirmed_version` under the NEW producer instance's empty cache.  After re-registration, `instance[P]` bumps; the leftover message from the old instance carries the OLD `instance_id`; broker's handler-flow check (§5.4) drops it.  No leak, no persistent false-success, regardless of message arrival order.  Cost: 1 nanosecond per APPLIED_REQ (integer compare), one integer in state, one integer field on two existing wire messages, no new round-trips.

**Fast-path invariant.**  `confirmed_version[K][P] >= channel_version[K]` ⇒ P's per-connection auth cache reflects the current allowlist (which includes the consumer we're about to admit, since step 2 of §5.4 verified consumer's pubkey IS in the current allowlist).

**Simplification note.**  The fast-path can be more conservative than strictly necessary.  If consumer A was admitted at version 10 but consumer B joined at version 11, A's fast-path check requires producer at version 11 (not just 10).  Cost: one extra wait-path cycle for the earlier consumer.  Benefit: no per-consumer version tracking; no pubkey-rotation-aware keying.  Accepted per P2.

### 5.3 Design decisions

- **D1** — Fast-path OK only when broker has POSITIVELY confirmed producer's cache reflects current allowlist.  No optimistic pre-confirms.
- **D2** — On wait-path miss, always fire NOTIFY.  No debounce / batching.
- **D3** — APPLIED_REQ is bidirectional (broker replies).  Producer's ACK timeout is the detector for "broker unreachable" — see §5.5.2.

### 5.4 Handler flow (normative)

**Implementation status (task #246 impl chain):**

| Step | Phase | Status |
|---|---|---|
| §5.4 steps 1-4 fast-path (validate + allowlist + producer-live + confirmed check) | 2.2 | ✅ shipped 2026-07-01 |
| §5.4 step 5a enqueue + step 5b NOTIFY fire + deferred-reply sentinel | 2.3a | ✅ shipped 2026-07-01 |
| §5.4 step d APPLIED_REQ drain (walk queue, reply success) | 2.3b | ✅ shipped 2026-07-01 |
| Producer-disconnect drain (§5.4 producer disconnect) | 2.3b | ✅ shipped 2026-07-01 |
| Channel-close drain (§5.4 channel K close) | 2.3b | ✅ shipped 2026-07-01 |
| §5.4 pending-entry timeout + §5.6 producer_apply_wait_ms sweep | 2.3c | ✅ shipped 2026-07-01 |
| §5.4 stale-instance guard (APPLIED_REQ step a) | 2.2 | ✅ shipped 2026-07-01 |
| §5.4 confirmed_version reset on re-registration / disconnect / kDead | 2.2 close-out | ✅ shipped 2026-07-01 |
| §5.5.3 `PRODUCER_REG_ACK.instance_id` echo (broker → producer) | 3a.1 | ✅ shipped 2026-07-02 |
| §5.5.4 `snapshot_version` echo on `PRODUCER_REG_ACK` + `GET_CHANNEL_AUTH_ACK` | 3a.3a | ✅ shipped 2026-07-02 |
| §5.5.2 producer captures + emits `CHANNEL_AUTH_APPLIED_REQ` (initial REG + NOTIFY-triggered paths) | 3a.3b | ✅ shipped 2026-07-02 |
| Phase 3a L3 close-out: error-path pin via real idle producer (timeout drain) | 3a.4 | ✅ shipped 2026-07-02 |
| Phase 3a L4 close-out: happy-path + wider error scenarios via real cycle-driving producer | 3a follow-up | ⏳ deferred to L4 (see `docs/todo/TESTING_TODO.md`) |
| §7.1 BRC `consumer_attach_zmq` method (declared + impl) | 3b.1 | ✅ shipped 2026-07-02 |
| §7.1 consumer pre-attach loop in `apply_consumer_reg_ack` (Option A placement) + L3 rung 4 retirement | 3b.2 | ✅ shipped 2026-07-02 |
| Phase 3b coverage | L3 wire + L4 e2e | ✅ complete — L3 attach coordination tests pin broker wire decisions; L4 `test_plh_hub_role_zmq_e2e.cpp` pins full-stack consumer loop.  No dedicated L2 layer — the feature has no L2-scope surface (pure decision helpers would be tautologies of the impl).  Multi-producer L4 augmentation lives with the Phase 3a L4 follow-up in `docs/todo/TESTING_TODO.md`. |

**Deferred-reply wire contract (Phase 2.3a).**  When the broker enters the wait-path (step 5), it does NOT send a reply immediately.  The consumer will receive its `CONSUMER_ATTACH_ACK_ZMQ` reply later, via one of four drain paths:
- `CHANNEL_AUTH_APPLIED_REQ` from producer P advances `confirmed_version[K][P]` past the enqueued `target_version` → reply `{status="success"}`.
- Producer P disconnects (ROUTER-observed or kDead heartbeat) → reply `{status="denied", reason="producer_not_live"}`.
- Channel K closes → reply `{status="denied", reason="channel_closing"}`.
- `producer_apply_wait_ms` budget elapses → reply `{status="timeout", reason="producer_did_not_confirm_within_budget"}`.

Consumer-side `attach_ack_wait_ms` (§5.6) must be > `producer_apply_wait_ms` so a broker-observed timeout arrives before a client-synthesized one.  All four drain paths use the ROUTER identity + correlation_id captured at enqueue time.

```
On CONSUMER_ATTACH_REQ_SHM from consumer C for (K, P):

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
   → Reply PRODUCER_REG_ACK with the new instance_id (§5.5.3).
   → New instance's cache is presumed empty; next ATTACH_REQ takes wait-path.
   → Any subsequent APPLIED_REQ carrying the OLD instance_id (from crashed prior instance,
     still in flight) is silently dropped by the stale-instance guard above.

On channel K close:
   → delete ChannelAccessIndex[K], channel_version[K], all confirmed_version[K][*].
   → walk pending_attach_queues for K: reply {status="denied", reason="channel_closing"}.
```

### 5.5 Wire spec

#### 5.5.1 `CONSUMER_ATTACH_REQ_ZMQ` / `_ACK_ZMQ` (consumer → broker)

**Scope:** ZMQ instantiation only, per §5.0.  SHM's `CONSUMER_ATTACH_REQ_SHM` is producer-initiated with a distinct payload (see §6.1).

Direction: consumer → broker.

**Request:**
```json
{
  "envelope":         "CONSUMER_ATTACH_REQ_ZMQ",
  "channel_name":     "lab.sensors.temperature",
  "consumer_pubkey":  "<40 Z85 chars>",
  "producer_role_uid":"prod.mysensor.uid00000001"
}
```

**Reply (per §5.4 outcomes):**
```json
{ "envelope": "CONSUMER_ATTACH_ACK_ZMQ", "status": "success" }
{ "envelope": "CONSUMER_ATTACH_ACK_ZMQ", "status": "denied",  "reason": "consumer_not_in_channel_allowlist" | "producer_not_live" | "channel_closing" }
{ "envelope": "CONSUMER_ATTACH_ACK_ZMQ", "status": "timeout", "reason": "producer_did_not_confirm_within_budget" }
```

§6.2.1 restates this shape in the ZMQ binding context (with `consumer_role_uid` for the fan-in tracker).

#### 5.5.2 `CHANNEL_AUTH_APPLIED_REQ` / `_ACK` (producer → broker)

Direction: producer → broker.  Sent after producer's per-connection cache successfully receives the pulled allowlist snapshot.

**Request:**
```json
{
  "channel_name":       "lab.sensors.temperature",
  "producer_role_uid":  "prod.mysensor.uid00000001",
  "instance_id":        7,
  "applied_version":    42
}
```

- `instance_id` — producer's shift number (§5.5.3).  Broker rejects if it doesn't match `instance[P]`.
- `applied_version` — the `snapshot_version` from the preceding `GET_CHANNEL_AUTH_ACK`.

**Reply:**
```json
{ "status": "ok", "channel_name": "...", "applied_version": 42 }
```

Silent drop on stale `instance_id` — producer's local ack timeout handles the missing reply path.

#### 5.5.3 `PRODUCER_REG_ACK` extension — `instance_id`

Existing wire (HEP-CORE-0036 §6.2).  This HEP adds one integer field:

```json
{
  ...existing REG_ACK fields...,
  "instance_id": 7
}
```

Broker assigns per registration (first-time or re-registration).  Producer stores + echoes on every APPLIED_REQ.  Every re-registration increments and returns a NEW `instance_id`.

Zero new round-trips: rides on the existing register handshake.

#### 5.5.4 Reused wires from HEP-CORE-0036

- `CHANNEL_AUTH_CHANGED_NOTIFY` (broker → producer, §6.5) — the doorbell.  This HEP fires it during wait-path (§5.4 step 5b).  No shape change.
- `GET_CHANNEL_AUTH_REQ` / `_ACK` (producer ↔ broker, §6.5) — the pull.  Producer pulls after NOTIFY.  Handler unchanged; `snapshot_version` field is the `channel_version[K]` value at snapshot time, and becomes the `applied_version` in the subsequent APPLIED_REQ.

### 5.6 Timeout + failure taxonomy

| Budget | Value (default) | Purpose |
|---|---|---|
| `producer_apply_wait_ms` | 3000 | Broker's wait for producer's APPLIED_REQ after firing NOTIFY.  On elapse: reply `{status="timeout", reason="producer_did_not_confirm_within_budget"}`. |
| `attach_ack_wait_ms` (client) | 5000 | Consumer's BRC-side timeout for the ATTACH_REQ round-trip.  MUST be > `producer_apply_wait_ms` so broker-observed timeout wins over client-observed timeout. |
| `applied_ack_wait_ms` | 1000 | Producer's wait for broker's APPLIED_REQ ACK.  On elapse: log WARN, cache stays applied (broker may re-drive on next NOTIFY). |

**Invariant:** `attach_ack_wait_ms > producer_apply_wait_ms`.  Violation would mean the consumer sees a client-synthesized timeout while the broker is still holding the entry — the broker's later reply would arrive after the consumer has moved on.

**Reason strings (enumerated, closed set):**
- `consumer_not_in_channel_allowlist`
- `producer_not_live`
- `channel_closing`
- `producer_did_not_confirm_within_budget`

Empty on success.  Script-facing `producer_attach_reason(channel, uid)` returns one of these values.

---

## 6. Bindings — per-transport instantiations

### 6.1 Bindings.SHM — HEP-CORE-0041 CONSUMER_ATTACH_REQ_SHM

Per §5.4's pre-confirm pattern, the producer queries the broker on every attach attempt before sending the SHM capability fd.  Shipped under HEP-CORE-0041 Phase 1 substep 1d (#251); handler at `broker_service.cpp::handle_consumer_attach_req`.

Request from producer to broker:
```json
{
  "channel_name":      "lab.raw",
  "consumer_pubkey":   "<40 Z85 chars>",
  "consumer_role_uid": "consumer.daq01.uid0042",
  "role_uid":          "<producer's own role_uid>",
  "correlation_id":    "<optional>"
}
```

Reply (auth decision; `CONSUMER_ATTACH_ACK_SHM` envelope):
```json
{ "status": "success",  "channel_name": "...", "consumer_pubkey": "..." }
{ "status": "denied",   "channel_name": "...", "consumer_pubkey": "...",
  "denial_reason": "consumer_pubkey not in channel allowlist" }
```

Reply (protocol-level errors, `ERROR` envelope):
- `INVALID_REQUEST` — missing field.
- `CHANNEL_NOT_FOUND` — channel doesn't exist on the broker.
- `PRODUCER_NOT_AUTHORIZED` — caller `role_uid` is not a registered producer of the channel (defence in depth — never disclose another channel's auth state to a non-producer).
- `INTERNAL_ERROR` — HubState invariant broken (broker bug).

**"denied" is distinct from "error"**: producer-side cache-divergence WARN logic (HEP-CORE-0041 substep 1e) needs to distinguish a clean broker "no" from a wire-level transport failure.  Dispatcher special-case maps `(status=success | denied)` → `CONSUMER_ATTACH_ACK_SHM`, others → `ERROR`.

**Note on SHM's pre-confirm shape.**  SHM's `CONSUMER_ATTACH_REQ_SHM` is producer-initiated (producer asks broker "should I hand this consumer the capability fd?") whereas ZMQ's `CONSUMER_ATTACH_REQ_ZMQ` is consumer-initiated (consumer asks broker "will producer's ZAP accept me?").  Both fall under the same coordination pattern: BROKER is the arbiter, and the data-plane handshake proceeds ONLY after broker confirmation.  SHM's L2 `crypto_box` challenge-response (HEP-CORE-0041 §5.5) is orthogonal — it proves the consumer holds the seckey after the broker has authorized the handshake.

### 6.2 Bindings.ZMQ — CONSUMER_ATTACH_REQ_ZMQ + CHANNEL_AUTH_APPLIED_REQ

#### 6.2.1 `CONSUMER_ATTACH_REQ_ZMQ` / `CONSUMER_ATTACH_ACK_ZMQ`

Direction: consumer → broker.  Sent by the consumer for each declared producer during `apply_consumer_reg_ack` (§7.1).

**Request:**
```json
{
  "envelope":            "CONSUMER_ATTACH_REQ_ZMQ",
  "channel_name":        "lab.sensors.temperature",
  "consumer_role_uid":   "consumer.daq01.uid0042",
  "consumer_pubkey":     "<40 Z85 chars>",
  "producer_role_uid":   "prod.mysensor.uid00000001"
}
```

**Reply — success:**
```json
{ "envelope": "CONSUMER_ATTACH_ACK_ZMQ",
  "status":   "success",
  "channel_name": "...",
  "producer_role_uid": "..." }
```

**Reply — denied / timeout (§5.6 reason strings):**
```json
{ "envelope": "CONSUMER_ATTACH_ACK_ZMQ",
  "status":   "denied" | "timeout",
  "reason":   "<enumerated per §5.6>",
  "channel_name": "...",
  "producer_role_uid": "..." }
```

#### 6.2.2 `CHANNEL_AUTH_APPLIED_REQ` — see §5.5.2

Fully documented in §5.5.2; ZMQ producer sends after `ZmqQueue::set_peer_allowlist` succeeds.  This is the D3 bidirectional confirmation.

---

## 7. Producer + consumer role flows

### 7.1 Consumer role side (ZMQ; `RoleAPIBase::apply_consumer_reg_ack`)

Runs during `apply_consumer_reg_ack`; between `register_consumer` returning REG_ACK and the consumer dialing producers.

**Placement decision (design-locked 2026-07-02, Phase 3b.1).**  The `CONSUMER_ATTACH_REQ_ZMQ` loop runs BEFORE `rx_queue->apply_master_approval(reg_ack)` (Option A), not after via a `set_producer_peers(admitted)` override (Option B).  Rationale:

- **Consumer never dials denied producers.**  Under A the queue's `apply_master_approval` receives an ACK whose `producers[]` has ALREADY been filtered to the admitted subset — no ZAP handshake ever runs against a producer the broker rejected.  Under B the queue would briefly dial ALL producers, then get overwritten; even the transient dial state costs measurable ZAP work + a potential race window where a denied producer's handshake completes just before `set_producer_peers` reduces the peer set.
- **`apply_master_approval` sees the truth-of-record.**  HEP-CORE-0036 §6.7 defines `apply_master_approval` as the single Standby→Active mutator on the queue.  Feeding it the filtered ACK keeps the "one source of truth" invariant clean — the ACK bytes it processes MATCH the ACK bytes the queue-level tests observe.  Under B, the queue would briefly hold a state that no ACK ever wrote.
- **Failure mode is a no-op, not a rollback.**  Under A, if the broker denies all N producers the ACK's `producers[]` becomes empty and `apply_master_approval` sees `producers=[]` — the queue stays in Standby.  Under B we'd have to un-dial after the fact, which requires a queue-level "retract peer" API that doesn't exist today.

The consumer's `pImpl->uid` / pubkey come from the KeyStore (HEP-CORE-0040), NOT from `reg_ack` — the ACK carries the consumer's uid only for the broker's authority-of-record; the role's own identity is the trusted source.

**Log markers (per §I11 observability).**  Emit these INFO/WARN lines from the loop for L3 test contracts + operator visibility:
- `[<tag>] attach:begin channel={} producers={N}` — one per `apply_consumer_reg_ack` entry.
- `[<tag>] attach:success channel={} producer={}` — one per admitted producer.
- `[<tag>] attach:denied channel={} producer={} reason={}` — one per denied.
- `[<tag>] attach:timeout channel={} producer={} reason={}` — one per timeout (both broker-observed AND client-synthesized §7.1 null path).
- `[<tag>] attach:complete channel={} admitted={}/{}` — one at end.


```
apply_consumer_reg_ack(reg_ack):
  connected = []
  attach_results = {}   # uid → (status, reason)

  LOGGER_INFO("[{}] attach:begin channel={} producers={}",
              short_tag, reg_ack.channel_name, len(reg_ack.producers))

  for producer_uid in reg_ack.producers:
    attach_ack = brc.request(CONSUMER_ATTACH_REQ_ZMQ{
      channel_name: reg_ack.channel_name,
      consumer_role_uid: pImpl->uid,
      consumer_pubkey: pImpl->pubkey_z85,
      producer_role_uid: producer_uid
    }, timeout_ms=attach_ack_wait_ms)

    # If broker never replied (client-side BRC timeout), synthesize the SAME §5.6 reason
    # used for broker-observed timeouts — the two failure modes are indistinguishable from
    # the script's perspective (both mean "producer didn't confirm; retry may help") and
    # the reason string enum is closed to §5.6 taxonomy per §8.
    if attach_ack IS NULL:
      attach_ack = {status: "timeout", reason: "producer_did_not_confirm_within_budget"}

    attach_results[producer_uid] = (attach_ack.status, attach_ack.reason)
    if attach_ack.status == "success":
      connected.append(producer_uid)
      LOGGER_INFO("[{}] attach:success channel={} producer={}",
                  short_tag, reg_ack.channel_name, producer_uid)
    else:
      LOGGER_WARN("[{}] attach:{} channel={} producer={} reason={}",
                  short_tag, attach_ack.status, reg_ack.channel_name,
                  producer_uid, attach_ack.reason)

  ZmqQueue::set_producer_peers(connected)   # ONLY dial admitted producers
  publish_attach_results(attach_results)    # feeds §8 accessors + on_channel_ready

  LOGGER_INFO("[{}] attach:complete channel={} admitted={}/{}",
              short_tag, reg_ack.channel_name, len(connected), len(reg_ack.producers))

  return (len(connected) > 0)   # per §5 fan-in partial-success policy
```

**Fan-in partial-success (locked policy).**  The loop always runs to completion.  Loop-level failure returns `false` ONLY when ZERO producers were admitted.  Failed producers are excluded from `set_producer_peers()` — consumer never dials them.  No automatic retry within the loop; failed producers retry on next consumer restart.

### 7.2 Producer role side (ZMQ)

**Producer-side state (added under this HEP):**
- `pImpl->instance_id` — assigned by broker at registration (echoed from `PRODUCER_REG_ACK`); echoed on every `CHANNEL_AUTH_APPLIED_REQ`.  Overwritten on any subsequent re-registration.

```
on PRODUCER_REG_ACK(ack):
  pImpl->instance_id = ack.instance_id   # from §5.5.3; store for later APPLIED_REQs
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
  # re-drive on next NOTIFY cycle if needed.  Mirrors §7.1 null-synthesis pattern.
  if applied_ack IS NULL:
    LOGGER_WARN("[{}] pre-attach: no ack from broker for channel {} v{} — cache preserved",
                short_tag, ack.channel_name, ack.snapshot_version)
    return

  if applied_ack.status == "ok":
    LOGGER_INFO("[{}] pre-attach: applied allowlist v{} (size={}, channel={})",
                short_tag, ack.snapshot_version, ack.allowlist.size(), ack.channel_name)
```

### 7.3 Log discipline (NORMATIVE markers)

The attach-loop lines are test-contract-stable markers.  Format:

| Marker | Emitted | Format |
|---|---|---|
| `[<tag>] attach:begin channel={ch} producers={N}` | Consumer, start of §7.1 loop | one per apply_consumer_reg_ack call |
| `[<tag>] attach:success channel={ch} producer={uid}` | Consumer, per successful producer | one per admitted producer |
| `[<tag>] attach:{denied\|timeout} channel={ch} producer={uid} reason={r}` | Consumer, per failed producer | one per failed producer |
| `[<tag>] attach:complete channel={ch} admitted={M}/{N}` | Consumer, end of loop | one per apply_consumer_reg_ack call |
| `[<tag>] pre-attach: applied allowlist v{V} (size={sz}, channel={ch})` | Producer, on APPLIED_REQ success | one per applied version |
| `[<tag>] pre-attach: no ack from broker for channel {ch} v{V} — cache preserved` | Producer, on APPLIED_REQ timeout | one per timeout |

Changes require a HEP amendment, not a code commit.  Downstream test contracts (`AttachLoop_LogMarkers_*`) depend on the exact strings.

---

## 8. Script-facing surface

Four accessors, all read-only, cross-engine parity required (Lua / Python / Native — see HEP-CORE-0028 for C ABI details):

- `api.producers_declared(channel) → list<uid>` — returns the list of producer `role_uid` strings declared in the consumer's most recent `CONSUMER_REG_ACK` for the given channel.  Reflects the broker's view of "producers this consumer was told to expect."  Available on consumer role hosts only; returns empty list on other roles.
- `api.producers_connected(channel) → list<uid>` — returns the subset of `producers_declared` whose pre-attach handshake succeeded.  Populated once the channel-ready callback / signal fires.
- `api.producer_attach_status(channel, uid) → enum` — returns the enumerated wire status for the given producer's pre-attach outcome: one of `"success"` / `"denied"` / `"timeout"` / `"not_declared"`.
- `api.producer_attach_reason(channel, uid) → string` — returns the enumerated reason string per §5.6 taxonomy: `"consumer_not_in_channel_allowlist"` / `"producer_not_live"` / `"channel_closing"` / `"producer_did_not_confirm_within_budget"` / `""` (empty on success).

**Channel-ready callback.**  Consumer role scripts receive `on_channel_ready(channel)` when §7.1's attach loop completes.  Script is RESPONSIBLE for querying the four accessors above to decide whether the admitted subset is acceptable (framework encodes no policy — a script that requires all-N producers must check and decide: continue with degraded coverage or call `api.channel_stop(channel)`).  Callback contract lands under HEP-CORE-0011 script callback surface.

---

## 9. Test plan

**L2 broker unit tests** (extend `test_broker_service.cpp`):

- `AttachReqZmq_FastPath_WhenConfirmedVersionCovers` — `confirmed_version[K][P] >= channel_version[K]` → immediate success.
- `AttachReqZmq_WaitPath_WhenProducerBehind` — cache-behind → fires NOTIFY, holds REQ.
- `AttachReqZmq_DrainsPendingOnAppliedReq` — after producer's APPLIED_REQ, pending consumers drain.
- `AttachReqZmq_TimeoutReplyWhenProducerSilent` — timeout budget elapsed → status="timeout".
- `AttachReqZmq_DeniedWhenNotInAllowlist` — pubkey not in ChannelAccessIndex → status="denied".
- `AttachReqZmq_DeniedWhenProducerNotLive` — producer kDead / disconnected AT DISPATCH TIME (step 3) → status="denied", reason="producer_not_live".
- `AttachReqZmq_DeniedWhenProducerDisconnectsWhilePending` — pins the §5.4 post-enqueue disconnect drain path.  ATTACH_REQ enqueued while P is kLive; P disconnects; verify queued REQ is drained as denied (reason="producer_not_live").
- `AttachReqZmq_PartialDrainOnAppliedReq` — pins the §5.4 partial-drain path.  Multi-entry queue with mixed target_versions W1 < W2 < W3; APPLIED_REQ arrives at W2; verify entries at W1 and W2 drain as success, entry at W3 remains queued (drains on next APPLIED_REQ or times out).
- `AppliedReq_ResetsAfterProducerReRegistration` — pins the §5.4 producer re-registration reset.  Advance confirmed_version[K][P] to N via APPLIED_REQ; P re-registers under same role_uid; verify confirmed_version[K][P] = 0, instance[P] bumped, next ATTACH_REQ takes wait-path.
- `AppliedReq_FromStaleInstance_SilentlyDropped` — pins the §5.5.2/§5.4 stale-instance guard.  Simulate a producer crash + re-registration; inject an APPLIED_REQ carrying the OLD instance_id after the re-registration; verify: broker does NOT advance confirmed_version, broker does NOT reply to producer (silent drop), no pending ATTACH_REQ drains as spurious success.  This test proves the P4 stale-instance guard closes the race regardless of message ordering.
- `AppliedReq_AdvancesConfirmedVersion` — receives APPLIED_REQ, confirmed_version advances.
- `ChannelClose_DrainsPendingAsDenied` — channel closure while pending → status="denied", reason="channel_closing".

**L3 broker–role integration tests** (extend Pattern4 ladder under `docs/todo/TESTING_TODO.md`):

- Fan-in wait-path e2e: 3 producers, one consumer, one producer's cache is behind at consumer attach time; verify NOTIFY fires, producer pulls, APPLIED_REQ acks, consumer's attach succeeds.
- Fan-in partial-success e2e: 3 producers, 2 admitted, 1 timed out; consumer's `set_producer_peers` receives only the admitted 2.

**L4 end-to-end tests** (extend demo framework):

- Auth-gated ZMQ e2e mirror of HEP-CORE-0041's `test_l4_shm_auth_e2e.cpp` — consumer + producer + broker in separate processes; verify data flows end-to-end.

---

## 10. Scope discipline (what this HEP does NOT do)

- **NOT the mechanism for producer's cache update.**  That is HEP-CORE-0036 §6.5 (doorbell-then-pull).  This HEP coordinates the timing of a consumer attach around that mechanism.
- **NOT the transport-specific auth crypto.**  ZMQ CURVE handshake and SHM's `crypto_box` L2 challenge remain owned by their respective sockets/backends (HEP-CORE-0021 for ZMQ; HEP-CORE-0041 §5.5 for SHM).
- **NOT federation.**  Cross-hub attach coordination is HEP-CORE-0037's concern.
- **NOT producer-→-consumer mutual auth.**  Tracked under task #262; extends this HEP's protocol without changing its shape.
- **NOT a substitute for connection-lifecycle events.**  On-connect / on-disconnect notifications remain HEP-CORE-0021 concerns; this HEP's `on_channel_ready` fires ONCE per channel at attach-loop completion.
- **NOT a replacement for revocation propagation semantics.**  HEP-CORE-0036 §I5 owns revocation; this HEP's `confirmed_version` mechanism becomes the confirmation timing for revoke committals — see §11 cross-references.

---

## 11. Cross-references + sibling HEP updates

- **HEP-CORE-0036 §I5 (revocation semantics)** — this HEP's `confirmed_version[K][P]` is the confirmation mechanism.  A revoke is committed at producer P when `confirmed_version[K][P] >= channel_version[K]` after the revoke bump.  Existing sessions per §I5 baseline stay up; new handshakes deny at the producer's per-connection cache.  Stale-instance guard (§5.2) prevents crashed-producer leftover APPLIED_REQ from falsely committing a revoke.
- **HEP-CORE-0036 §6.5 (notify-then-pull)** — reused as the substrate for this HEP's wait-path.  §6.5 owns the mechanism; this HEP owns the coordination that hangs off it.
- **HEP-CORE-0041 §5.4 (SHM CONSUMER_ATTACH_REQ_SHM)** — relocated to §6.1 Bindings.SHM of this HEP as the SHM instantiation of the coordination protocol.  HEP-0041 §5.4 becomes a 3-line pointer.
- **HEP-CORE-0041 §8 (What stays in HEP-0036)** — updated: ZMQ pre-attach content is in HEP-CORE-0042, not HEP-0036.
- **HEP-CORE-0035** — no direct edit; HEP-0035 focuses on hub-role auth foundation; this HEP is transport-agnostic coordination.
- **HEP-CORE-0011** — adds `on_channel_ready(channel)` callback contract.
- **HEP-CORE-0028** — adds four script-facing accessors' C ABI shape.  Cross-engine parity (Lua / Python / Native) — task #232 tracks the parity sweep.

---

## 12. Sequence diagram

```mermaid
sequenceDiagram
    participant C as Consumer role
    participant B as Broker
    participant P as Producer role

    Note over C,P: Prior state: producer P registered<br/>with instance_id=I; confirmed_version[K][P] behind channel_version[K]

    C->>B: CONSUMER_ATTACH_REQ_SHM (K, P, C.pubkey)
    Note over B: Step 2: pubkey ∈ allowlist? ✓<br/>Step 3: P.state == kLive? ✓<br/>Step 4: confirmed < channel_version → wait path
    B->>B: enqueue with target_version=channel_version[K]

    B->>P: CHANNEL_AUTH_CHANGED_NOTIFY (K)
    P->>B: GET_CHANNEL_AUTH_REQ (K)
    B->>P: GET_CHANNEL_AUTH_ACK (K, allowlist, snapshot_version=W)

    Note over P: set_peer_allowlist(allowlist) OK

    P->>B: CHANNEL_AUTH_APPLIED_REQ{channel, instance_id=I, applied_version=W}
    Note over B: check instance_id=I matches instance[P]; else drop
    B->>P: {status="ok"}
    Note over B: confirmed_version[K][P] = W<br/>drain pending_attach_queue

    B->>C: CONSUMER_ATTACH_ACK_SHM {status="success"}

    Note over C: dial producer P (CURVE / crypto_box handshake)<br/>succeeds against caught-up cache
```

---

## 13. History

- **2026-07-01** — Adopted (task #246 Phase 1 promotion).  Draft substance from `docs/tech_draft/DRAFT_HEP-0036_zmq-pre-confirm_2026-06-30.md` promoted verbatim; SHM binding relocated from HEP-CORE-0041 §5.4.

---
