# DRAFT — HEP-CORE-0036 §6.5 amendment: ZMQ pre-attach broker confirmation

| Attribute | Value |
|---|---|
| **Status** | 🟡 DESIGN DRAFT — under designer review; no code changes yet |
| **Tracker** | [#246](../todo/AUTH_TODO.md#-246-hep-core-0036-amendment) |
| **Sibling** | HEP-CORE-0041 Phase 5 (ZMQ retrofit to symmetric capability semantics) |
| **Drafted** | 2026-06-30 |
| **Chain position** | Follows #275 (1i-cleanup arc complete); precedes REVIEW-D + #262 mutual auth |

---

## 1. Motivation

Today's ZMQ auth flow has an observed race that skips a fresh consumer's data. The L4 test `PlhHubCliTest.ZmqE2E_AuthorizedConsumerReceivesAllSlots` skips under this task, with the following captured evidence (inline test-file comment):

> "the broker sends `CONSUMER_REG_ACK` to the consumer (consumer dials producer) BEFORE the producer's `CHANNEL_AUTH_CHANGED_NOTIFY` → `GET_CHANNEL_AUTH` chain seeds the producer's PUSH allowlist with the new consumer's pubkey. libzmq's initial CURVE handshake fails on the empty allowlist and the consumer never receives data (observed: producer allowlist update fires ~110 ms AFTER consumer PULL goes Active)."

The race is a real production-readiness gap. HEP-CORE-0041 §5.5 already solved the symmetric problem on SHM via a pre-attach broker confirmation (the `CONSUMER_ATTACH_REQ`/`_ACK` wire, `broker_service.cpp:1212`). This amendment retrofits ZMQ to the same shape so both transports have deterministic pre-authorization: broker confirms admission before consumer dials producer. Post-amendment, the difference between ZMQ and SHM collapses to just the byte-transport (bind + push vs SCM_RIGHTS handoff).

---

## 2. Design goal

**Symmetric pre-confirm on both transports.** The wire pattern SHM uses (consumer sends `CONSUMER_ATTACH_REQ` to broker, broker confirms admission, consumer proceeds) applies to ZMQ verbatim. Any residual asymmetry lives in the byte-transport layer, not the auth layer.

**Preserve the retracted-2026-06-04 discipline.** The broker MUST NOT re-become a "sync-request initiator" on its ROUTER. Any new pattern that holds a response pending an external event must be justified against §6.5's retraction rationale.

**Preserve revocation semantics.** HEP-CORE-0035 §I5 requires that a revoked consumer stop being able to complete new CURVE handshakes. The producer's ZAP handler stays the enforcement point for both admission and revocation; the pre-confirm just closes the admit-side race.

---

## 3. Design decisions (locked)

- **D1-A: Consumer initiates a new `CONSUMER_ATTACH_REQ_ZMQ` wire** after receiving `CONSUMER_REG_ACK`, before dialing producer.  This mirrors SHM's `CONSUMER_ATTACH_REQ` shape exactly.
- **D2-X: Broker synchronously holds the consumer's ATTACH REQ** until the producer's `GET_CHANNEL_AUTH_REQ` arrives (proof-of-cache-populated).  Adds one broker-side RTT to the consumer-attach path.  See §5 for how this differs from the 2026-06-04 retracted pattern.
- **D3-corrected: Producer's ZAP handler stays load-bearing** for enforcement (cache is the source of truth at handshake time).  The pre-confirm eliminates the admit-side race; the cache is still enforcement-authoritative because revocation propagates through the same cache path.  This is a correction from the initial "cache as observability" framing — see §7 for why.

---

## 4. Wire spec

### 4.1 New wire: `CONSUMER_ATTACH_REQ_ZMQ` / `CONSUMER_ATTACH_ACK_ZMQ`

Direction: consumer → broker.  Sent by the consumer after successful `CONSUMER_REG_ACK`, before dialing producer's endpoint.

**Request payload (`CONSUMER_ATTACH_REQ_ZMQ`):**

```json
{
  "channel_name":       "lab.sensors.temperature",
  "consumer_role_uid":  "cons.logger.uid00000001",
  "producer_role_uid":  "prod.mysensor.uid00000001"
}
```

- `channel_name` — Same string carried on prior `CONSUMER_REG_REQ`; disambiguates in the fan-in case.
- `consumer_role_uid` — Consumer's own role_uid (already known to broker via `CONSUMER_REG`; redundant but included for symmetry with existing broker request payloads that echo identity).
- `producer_role_uid` — Which producer the consumer intends to dial.  For fan-in channels the consumer picks one of `CONSUMER_REG_ACK.producers[]`; the broker gates admission per-producer since each producer runs its own ZAP handler with a channel-scoped allowlist (HEP-CORE-0036 §I3).

**Success reply (`CONSUMER_ATTACH_ACK_ZMQ`) — `status="success"`:**

```json
{
  "status":            "success",
  "channel_name":      "lab.sensors.temperature",
  "producer_role_uid": "prod.mysensor.uid00000001"
}
```

Broker has confirmed the producer's allowlist has been populated with the consumer's pubkey; consumer may now dial safely.

**Denied reply — `status="denied"`:**

```json
{
  "status":            "denied",
  "channel_name":      "lab.sensors.temperature",
  "producer_role_uid": "prod.mysensor.uid00000001",
  "reason":            "consumer_not_in_channel_allowlist"
}
```

Same pattern as HEP-CORE-0041 SHM `CONSUMER_ATTACH_ACK.status="denied"` (`broker_service.cpp:1216-1226`) — "denied" is a normal auth decision, not a wire error.

**Timeout reply — `status="timeout"`:**

```json
{
  "status":            "timeout",
  "channel_name":      "lab.sensors.temperature",
  "producer_role_uid": "prod.mysensor.uid00000001",
  "reason":            "producer_did_not_pull_within_budget"
}
```

Producer did not send `GET_CHANNEL_AUTH_REQ` within the timeout budget (see §6.3).  Consumer treats as attach failure; can retry.

### 4.2 Reused wires (no changes)

- `CHANNEL_AUTH_CHANGED_NOTIFY` — HEP-0036 §6.5.0.  Broker → producer fire-and-forget doorbell.
- `GET_CHANNEL_AUTH_REQ` / `GET_CHANNEL_AUTH_ACK` — HEP-0036 §6.5.  Producer → broker request-reply, producer applies allowlist via `ZmqQueue::set_peer_allowlist`.

The pre-confirm reuses these existing surfaces for the producer-side flow; only the consumer-broker leg is new.

---

## 5. Broker handler shape (async response holding)

### 5.1 Handler placement

`handle_consumer_attach_req_zmq` — new method on `BrokerServiceImpl`.  Dispatched from the ROUTER poll loop parallel to the existing SHM `handle_consumer_attach_req` at `broker_service.cpp:1212`.

### 5.2 Handler flow (normative)

```
CONSUMER_ATTACH_REQ_ZMQ arrives from consumer C for channel K + producer P:

1. Validate payload shape + identity.
2. Look up ChannelAccessIndex[K].authorized_consumer_pubkeys.
   - If C.pubkey ∉ allowlist → reply CONSUMER_ATTACH_ACK_ZMQ{status="denied", reason=...}. Handler complete.
3. Look up ChannelEntry[K].producers to find P.
   - If P not present or not kLive → reply {status="denied", reason="producer_not_live"}. Handler complete.
4. If C.pubkey is already known to be in P's cache (broker-side tracking; see §5.4):
   → reply CONSUMER_ATTACH_ACK_ZMQ{status="success"} immediately. Fast path.
5. Otherwise:
   a. Enqueue this ATTACH REQ (consumer C, channel K, producer P, request identity) into a
      per-producer "pending attach" queue on ChannelEntry[K].producer[P].pending_attach_queue.
   b. Fire CHANNEL_AUTH_CHANGED_NOTIFY to P (fire-and-forget, existing wire).
      This nudges P to send GET_CHANNEL_AUTH_REQ if it hasn't already.
   c. Do NOT send an ATTACH_ACK yet.  Handler returns to the ROUTER loop without replying.

When P's GET_CHANNEL_AUTH_REQ subsequently arrives:
   a. Reply GET_CHANNEL_AUTH_ACK to P normally (existing §6.5 flow).
   b. Broker-side marker: after replying, walk ChannelEntry[K].producer[P].pending_attach_queue
      and reply CONSUMER_ATTACH_ACK_ZMQ{status="success"} to each pending consumer whose
      pubkey is in the allowlist just returned to P.  Drain the queue.
```

### 5.3 Why this is NOT the retracted "broker as sync-request initiator" pattern

The 2026-06-04 amendment (§6.5) retracted having the broker be a sync-request INITIATOR — i.e., broker sends first, then waits for reply on the same ROUTER it responds on.  That created "how do we serve inbound during outbound wait" complications with no precedent.

This amendment has the broker be a sync-response HOLDER for a specific inbound REQ, waiting for a DIFFERENT normal inbound REQ from a specific expected sender (the producer's `GET_CHANNEL_AUTH_REQ`, which is a first-class existing wire pattern).  Distinctions from the retracted design:

- Broker never initiates.  Every wire event on the broker's ROUTER is either an inbound REQ or an outbound REP.
- Broker doesn't fan-out or wait for fan-in.  ONE consumer's REQ waits for ONE producer's REQ.
- Broker's poll loop keeps draining normally — the pending REQ just doesn't get replied-to yet.  No new demultiplexing logic; the ROUTER's socket identity/routing already lets the reply-later step work via the recorded identity.
- Existing dispatch machinery.  `GET_CHANNEL_AUTH_REQ` handler already exists; the only NEW thing is the post-reply drain step that ACKs pending consumers.

### 5.4 Broker-side cache: which pubkeys does each producer's ZAP already know?

For the fast-path check at §5.2 step 4, the broker needs to know which consumer pubkeys the producer's cache already contains.  Options:

- **Option A (chosen)**: Track the last-served snapshot per producer inside `BrokerService`.  After every `GET_CHANNEL_AUTH_ACK` reply to producer P for channel K, the broker records `served_cache[K][P] = {pubkeys just replied with}`.  On a new ATTACH REQ, the broker compares C.pubkey against `served_cache[K][P]` to decide fast-path vs wait.  Memory cost: O(channels × producers × pubkeys).  Same order as `ChannelAccessIndex` itself.
- **Option B (rejected)**: Producer reports its cache state to broker.  Adds a new wire; wastes bandwidth; broker already knows what it served.

The broker's tracked `served_cache` is best-effort — it's advisory for the fast-path decision.  If the producer dropped a NOTIFY or restarted, `served_cache` may over-estimate what the producer has cached.  The wait-path then fires an extra NOTIFY and gets a fresh pull — self-corrects on next round-trip.

### 5.5 Timeout + failure modes

- **Timeout budget**: producer_pull_wait_ms (config, default proposed: 3000 ms).  This is a broker-side timer on each pending ATTACH REQ.
- **On timeout**: broker replies to the pending consumer REQ with `status="timeout"`; consumer treats as attach failure.
- **On producer disconnect while REQ pending**: broker replies `status="denied", reason="producer_disconnected"`.  Consumer can retry with a fresh REG_ACK.
- **On channel closing while REQ pending**: broker replies `status="denied", reason="channel_closing"`.

---

## 6. Consumer + producer side flows

### 6.1 Consumer role side (`RoleAPIBase::apply_consumer_reg_ack`, ZMQ branch)

Current shape (pseudo):
```
apply_consumer_reg_ack(REG_ACK ack):
  if ack.data_transport == "zmq":
    ZmqQueue::pull_from(ack.producers[0].endpoint, ack.producers[0].pubkey_z85)   ← dials immediately (RACE)
    set_producer_peers(ack.producers)
    start()
```

Post-amendment shape:
```
apply_consumer_reg_ack(REG_ACK ack):
  if ack.data_transport == "zmq":
    for producer in ack.producers:                                                 ← multi-producer fan-in
      attach_ack = brc.request(CONSUMER_ATTACH_REQ_ZMQ{
        channel_name: ack.channel_name,
        consumer_role_uid: pImpl->uid,
        producer_role_uid: producer.role_uid
      }, timeout_ms=budget)
      if attach_ack.status != "success":
        LOGGER_ERROR("[{}] pre-attach denied by broker: {}", short_tag, attach_ack.reason)
        return false
    ZmqQueue::pull_from(ack.producers[0].endpoint, ack.producers[0].pubkey_z85)   ← dials safely now
    set_producer_peers(ack.producers)
    start()
```

The pre-confirm is per-producer.  For fan-in channels the consumer serializes the ATTACH REQ across producers; for MVP-simplicity this is fine (attach is a startup-time cost, not a data-plane cost).  Concurrent pre-confirm across producers is a possible optimization deferred to a follow-on.

### 6.2 Producer role side

**No code change required for MVP.**  The producer already runs the `CHANNEL_AUTH_CHANGED_NOTIFY → GET_CHANNEL_AUTH_REQ → set_peer_allowlist` chain from HEP-0036 §6.5 (shipped 2026-06-03 as part of D1).  The pre-confirm just makes the consumer wait for that chain to reach `set_peer_allowlist` before dialing.

**Optional producer-side observability**: producer logs `[{}] pre-attach: served allowlist to broker (size={}, channel={})` on every `GET_CHANNEL_AUTH_REQ` handled.  Operator-visible confirmation that the ZAP handler is now ready.  Non-normative.

### 6.3 Budget shape

- Consumer's `CONSUMER_ATTACH_REQ_ZMQ` timeout (client-side): `attach_ack_wait_ms`, default 5000 ms.
- Broker's producer-pull wait (server-side): `producer_pull_wait_ms`, default 3000 ms.
- Budget ordering: consumer's timeout MUST be > broker's timeout so the consumer always gets a real ACK (success/denied/timeout) rather than transport-level timeout.  Default (5000 > 3000) satisfies this.

---

## 7. Design decisions — remaining questions for you

These are the items I flagged where the initial three-decision framing needs refinement.  Please confirm or redirect.

### 7.1 D3 revocation semantics

**Original framing (needs correction):** "Producer's ZAP becomes cache-only observability."

**Problem I found while drafting:** If ZAP always admits (observability-only), a revoked consumer can still complete a fresh CURVE handshake — breaks HEP-CORE-0035 §I5.  Revocation needs an enforcement gate somewhere, and the ZAP handler is the natural place.

**Corrected D3 (proposed):** Producer's ZAP handler stays load-bearing for enforcement — cache is the source of truth at handshake time.  Pre-confirm eliminates the admit-side race.  Revocation propagates through the same `CHANNEL_AUTH_CHANGED_NOTIFY → GET_CHANNEL_AUTH → set_peer_allowlist` chain that already exists.  Cache is BOTH load-bearing AND observable (via `api.allowed_peers()`).

Please confirm this correction OR redirect if you want a different revocation shape.

### 7.2 Broker-side `served_cache` size

Memory cost is O(channels × producers × pubkeys) — same order as `ChannelAccessIndex` — but adds a per-producer dimension on top.  For the target scale (dozens of channels, ≤ few producers per channel, ≤ few dozen consumers), that's <1 MB total.  Fine for MVP.  Do you want a hard cap / eviction policy, or is "same lifecycle as ChannelAccessIndex" sufficient?

### 7.3 Fan-in serialization

Consumer serializes per-producer ATTACH REQ (§6.1).  For a 10-producer fan-in channel with 3s producer_pull_wait_ms each, worst-case consumer-attach becomes 30 s.  Acceptable for MVP?  Or concurrency an MVP requirement?

### 7.4 Which HEP owns the amendment?

Two options:
- **HEP-0036 §6.5 amendment** (this doc's default).  Extends the doorbell-then-pull framework with a new pre-confirm.
- **HEP-0041 Phase 5 spec**, cross-referenced from HEP-0036.  HEP-0041 is where the SHM version already lives; putting the ZMQ retrofit in HEP-0041 keeps both transports' pre-confirm designs in one document.

My recommendation: **HEP-0041 Phase 5** with a §6.5.2 pointer from HEP-0036.  Reason: the pre-confirm pattern IS HEP-0041's central design; HEP-0036 §6.5 is about post-attach runtime sync (doorbell-then-pull), not pre-attach admission.

---

## 8. Test plan

**L2 broker unit tests** (extend `test_broker_service.cpp` or add new file):
- `HandlesConsumerAttachReqZmq_FastPath_WhenCacheHasPubkey` — cache-hit → immediate ACK.
- `HandlesConsumerAttachReqZmq_WaitPath_WhenProducerNeedsPull` — cache-miss → fires NOTIFY, holds REQ.
- `HandlesConsumerAttachReqZmq_DrainsPendingOnProducerPull` — after producer's GET_CHANNEL_AUTH_REQ, pending consumers drain.
- `HandlesConsumerAttachReqZmq_TimeoutReplyWhenProducerSilent` — timeout budget → status="timeout".
- `HandlesConsumerAttachReqZmq_DeniedWhenNotInAllowlist` — pubkey not in ChannelAccessIndex → status="denied".
- `HandlesConsumerAttachReqZmq_DeniedWhenProducerDeadWhileWaiting` — producer disconnect during wait → status="denied".

**L3 sequence pin** (extend `test_datahub_broker_protocol.cpp` or add new worker):
- `PreConfirmSequenceEliminatesRace_ZmqConsumerDialsAfterProducerCacheReady` — full sequence pin.  Verify producer's `set_peer_allowlist` fires BEFORE consumer's `pull_from` dial via log-marker sequencing.

**L4 e2e unskip:**
- Remove `GTEST_SKIP` from `PlhHubCliTest.ZmqE2E_AuthorizedConsumerReceivesAllSlots`.  Verify the deferred scenario passes green.

**L4 e2e negative:**
- Existing `ZmqE2E_UnauthorizedConsumerDeniedByBroker` continues to pass (denial path unchanged from consumer's perspective — broker just denies earlier).

---

## 9. Phased implementation

Each phase is a single commit; ships green before the next starts.

- **Phase 1: HEP promotion.** Merge this tech_draft into `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` (§Phase 5 section) + cross-ref stub in HEP-CORE-0036 §6.5.2.  No code changes.  Reviewers can gate here.
- **Phase 2: Broker side.** Add `handle_consumer_attach_req_zmq` + the `served_cache` + the pending-attach queue on `ChannelEntry::producer`.  L2 tests pin the state machine.
- **Phase 3: Consumer side.** Extend `RoleAPIBase::apply_consumer_reg_ack` ZMQ branch with the per-producer pre-confirm loop.  BRC layer gains the `CONSUMER_ATTACH_REQ_ZMQ` helper if not already present.
- **Phase 4: Test unskip + L3 sequence pin.** Remove `GTEST_SKIP` in AUTH-7 L4 ZMQ test; add L3 sequence-pin worker.
- **Phase 5: REVIEW-D gate.** Full systematic review of the pre-confirm arc.  Any residual §6.5 amendment doc-drift closes here.

---

## 10. Sibling doc updates (which HEPs cite this)

At Phase 1 promotion (or Phase 5 close-out):

- **HEP-CORE-0036 §6.5.2** — new subsection: "Pre-attach admission (ZMQ) — see HEP-CORE-0041 §Phase 5."
- **HEP-CORE-0035 §I5** — cross-ref: revocation still enforces via ZAP-with-cache; pre-confirm eliminates admit-side race.
- **HEP-CORE-0023 §7** — no change (BRC path already handles the new REQ shape transparently).
- **HEP-CORE-0017 §4.6** — no change (fan-in semantic unchanged).

---

## 11. What this amendment does NOT do (scope discipline)

- **Does not touch #262 (mutual auth).** Producer→consumer proof-of-possession is a separate wire (3rd handshake frame) on the SHM capability path.  Orthogonal.
- **Does not add a broker-hosted ZAP handler.** libzmq supports it but the re-arch cost is unjustified — cache-based ZAP with pre-confirm covers the race without new architecture.
- **Does not eliminate the producer's ZAP allowlist cache.** Cache remains load-bearing for both admit and revoke; pre-confirm just makes the cache populated at handshake time.
- **Does not change SHM's pre-attach protocol.** SHM's `CONSUMER_ATTACH_REQ` stays exactly as HEP-0041 §5.5 specifies.  This amendment mirrors the shape onto ZMQ.

---

## 12. Sequence diagram (Mermaid)

```mermaid
sequenceDiagram
    autonumber
    participant C as Consumer<br/>(role_host)
    participant B as Broker
    participant P as Producer<br/>(role_host)

    C->>B: CONSUMER_REG_REQ
    B-->>C: CONSUMER_REG_ACK{producers[]}
    Note over C,P: Pre-amendment: consumer would dial P now (RACE)

    rect rgba(220,240,255,0.5)
        Note over C,B: This amendment (§4.1)
        C->>B: CONSUMER_ATTACH_REQ_ZMQ{channel, C.uid, P.uid}
        alt fast path (broker knows P has C.pubkey)
            B-->>C: CONSUMER_ATTACH_ACK_ZMQ{status="success"}
        else wait path
            B->>P: CHANNEL_AUTH_CHANGED_NOTIFY{channel, reason="consumer_joined"}
            Note over B: holds C's REQ in per-producer pending queue
            P->>B: GET_CHANNEL_AUTH_REQ
            B-->>P: GET_CHANNEL_AUTH_ACK{allowlist}
            Note over P: applies via set_peer_allowlist
            Note over B: post-reply drain: walks pending queue,<br/>ACKs waiting consumers whose pubkey is in the just-served allowlist
            B-->>C: CONSUMER_ATTACH_ACK_ZMQ{status="success"}
        end
    end

    C->>P: ZMQ PULL dial (CURVE handshake — cache is populated)
    P-->>C: CURVE handshake succeeds; data flows
```

---

## 13. Open discussion items

Tag `[user]` = your call; tag `[me]` = I'll resolve during Phase 1 promotion.

- `[user]` §7.1: confirm D3-corrected (revocation via ZAP-with-cache, not observability-only).
- `[user]` §7.2: `served_cache` memory bound — accept "same lifecycle as ChannelAccessIndex" or want an explicit cap?
- `[user]` §7.3: fan-in serialization acceptable for MVP, or want concurrent-per-producer?
- `[user]` §7.4: HEP-0041 Phase 5 vs HEP-0036 §6.5 amendment as authoritative home.
- `[me]` §5.5: timeout defaults are proposed; if you have prior-art values from HEP-0041 SHM, I'll align.
- `[me]` §4.1: payload field-name choices (`consumer_role_uid` vs `role_uid` etc.) — I'll align with the existing HEP-0036 §6.4 CONSUMER_REG_REQ shape at promotion time.
