# Owner-first establishment — S1–S3 implementation plan (T3)

| | |
|---|---|
| **Status** | DESIGN — 2026-07-26. Implementation plan for the code catch-up to the pinned contract. Review + user sign-off before coding each slice. |
| **Target contract (normative, already pinned)** | HEP-CORE-0017 §4.7.0.1 (C1–C7 establishment) + §4.7.0.2 (T1–T7 teardown). This draft does NOT re-design — it maps the contract onto the current code and specifies the changes. |
| **Supersedes** | The retired R6 broker-pends gate (do NOT build). |
| **Scope** | S1 owner-locked `ChannelEntry` + `awaiting_owner`; S2 owner-aware teardown; S3 dialer fast-fail. (S4 peer-join callbacks shipped 2026-07-25.) |

---

## The one idea

**The channel book (`ChannelEntry` in `HubState`) exists iff its binding *owner*
exists** — fan-in → the consumer owns; fan-out / 1-to-1 → the producer owns
(C1/C2). Everything in S1–S3 is a consequence:

- **S1** — a *dialer* arriving before its owner must not open a book, and must
  not hard-fail; it gets a retryable `awaiting_owner` and the role host retries
  (C3).
- **S2** — when the *owner* leaves, the book dies; a *dialer* leaving never
  closes it (C2/T2).
- **S3** — once the book is gone, the dialer's readiness poll fast-fails instead
  of burning `init_timeout` (C4).

The broker **never pends a request** — it is a pure responder. `awaiting_owner`
and `CHANNEL_NOT_FOUND` are *immediate* replies; the retry lives in the dialer's
role host.

---

## S1 — owner-locked book + `awaiting_owner`  (C2 open-half, C3)

### Current code (the gap) — verified 2026-07-26
- **The owner-side book-open is already correct** (do NOT re-add it): the
  fan-out / 1-to-1 producer-owner opens via `handle_reg_req` (`:2524-2527`,
  `admission.channel_opened`); the fan-in consumer-owner opens via the HEP-0036
  §6.6.1 consumer-side fix (2026-07-11). S1 must NOT touch these.
- **The gap is that the book-open is topology-BLIND, so a DIALER can open it.**
  `handle_reg_req` opens on `admission.channel_opened` regardless of role/topology
  — so a **fan-in producer (dialer) arriving first opens a book with no owner**
  (`:2524-2527`; violates C2, the whole-first-producer-opens `_on_producer_added`
  fresh-channel path).
- **Fan-out / 1-to-1 consumer (a DIALER) hard-fails.**
  `handle_consumer_reg_req` returns `CHANNEL_NOT_FOUND` (a caller error) when the
  channel does not exist. Under fan-out/1-to-1 the consumer is the dialer, so an
  early consumer should get a *retryable* `awaiting_owner`, not a hard error
  (tracker #13).
- **`awaiting_owner` does not exist** (grep: 0 hits) — new wire status + new
  role-host retry.

> Net: the owner-open half is done; **S1 = gate out the *dialer* open + turn the
> dialer's early arrival into a retryable `awaiting_owner`.**  Two dialer sites
> only: fan-in producer (`handle_reg_req` book-open) and fan-out/1-to-1 consumer
> (`handle_consumer_reg_req` `CHANNEL_NOT_FOUND`).

### The change
1. **Broker — topology-gate the book-open.** In the REG handlers, compute
   `role_is_owner = Queue::{writer,reader}_is_binding_side(topology, this_role)`.
   - Owner REG + no book → open the `ChannelEntry` (as today).
   - **Dialer REG + no book → reply `awaiting_owner`** (retryable transient); do
     NOT call `_on_channel_access_opened`. This replaces (a) the fan-in
     producer's book-open and (b) the fan-out consumer's `CHANNEL_NOT_FOUND`.
   - Dialer REG + book exists → proceed as today (admit into the owner's book).
2. **Wire — new status `awaiting_owner`** on `REG_ACK`/`CONSUMER_REG_ACK`
   (HEP-CORE-0007 catalog). Distinct from `CHANNEL_NOT_FOUND` (caller error) and
   `success`. Add to the typed body / status enum used by
   `receive_and_validate`.
3. **Role host — Tier-2 retry (C3).** On `awaiting_owner`, the dialing role
   host re-sends REG after a bounded backoff, capped by `init_timeout_ms`,
   honoring cancellation; on budget-exhaustion it fails startup cleanly (fatal
   abort, misconfiguration diagnostic). The script never sees it (`on_init` has
   not run). Reuses the existing backoff/`is_cancelled` machinery already in the
   role host's establishment path (same place `finalize_channel_connect` polls).

### Conflicts / risks to verify at code time
- **HEP-0042 admission ledger**: the owner's book carries the
  `VersionedAdmissionLedger`; opening it only for the owner must not disturb the
  ledger seeding (INVARIANT-BIND-CONFIRM-1..3). The dialer is admitted into the
  owner's existing ledger — unchanged.
- **`_on_producer_added` double-duty**: it both creates the record and
  (via `_on_channel_access_opened`) opens the book. Split: record-append still
  happens for a fan-out producer-owner; for a fan-in producer (dialer) it must
  admit-into-existing-book, never open. Check the `_on_producer_added` fresh-
  channel path does not implicitly create.
- **Processor (C7)**: per-side ownership — the gate is per channel, so a
  processor that is owner on one side and dialer on the other is handled by the
  same per-REG gate. No special case.

---

## S2 — owner-aware teardown  (C2 close-half, T2)

### Current code (the gap)
- **Producer-centric teardown.** `hub_state.cpp` (~1874, consumer-presence
  drop) states: *"consumer-presence Pending → Disconnected does NOT tear down
  the channel — only the producer side controls channel observability +
  teardown."* That is correct for fan-out/1-to-1 (producer = owner) but **wrong
  for fan-in**, where the consumer is the owner: a fan-in consumer's death must
  close the channel, and last-producer death must NOT.

### The change
- Make the teardown decision **owner-aware, not producer-hardcoded**: on a
  presence drop (DEREG / heartbeat-timeout / crash), close the `ChannelEntry`
  **iff the leaving presence is the channel's binding owner** for that
  topology; a dialer drop only erases its slot + fans nothing channel-wide.
  - fan-out / 1-to-1: producer(owner) death → close (unchanged);
    consumer(dialer) death → slot erase (unchanged).
  - fan-in: **consumer(owner) death → close (NEW)**; producer(dialer) death →
    slot erase, channel lives (NEW — today last-producer may wrongly close).
- Fix the stale `§2.1.1 "only the producer controls teardown"` comment +
  the `hub_state.cpp:1874` citation to the owner-aware rule.

### Conflicts / risks
- **HEP-0023 §2.1.1** already carries a 2026-07-08 amendment for owner-aware
  teardown (per the tracker) — this is *code* catch-up to that doc, so verify
  the doc wording and cite it rather than re-deciding.
- **Channel-observable FSM**: the Pending/Live observable is derived from
  *producer* presences today. If the consumer becomes the owner (fan-in), does
  the observable need a consumer-driven variant? Verify — may be orthogonal
  (observable is about data-readiness, teardown is about ownership).
- **`CHANNEL_CLOSING_NOTIFY` fan-out target**: on owner death the notify goes to
  the *dialers* (T2). Confirm the fan-out iterates the dialing side, not a
  hardcoded producer/consumer list.

---

## S3 — dialer fast-fail on owner death  (C4)

### Current code (the gap)
- `handle_check_peer_ready_req` (`broker_service.cpp:4399`) answers
  `ready` / `not_ready`. Verify its behavior when the channel/book is **gone**
  (owner died): today it may return `not_ready` (dialer keeps polling until
  `init_timeout`) rather than a terminal `CHANNEL_NOT_FOUND`.

### The change
- When `CHECK_PEER_READY_REQ` finds **no book** for the channel, return a
  terminal `CHANNEL_NOT_FOUND` (not `not_ready`). The dialer's role host treats
  it as fatal-fast: abort establishment with a clean diagnostic instead of
  burning the full `init_timeout` budget. This *falls out of* S2 (S2 makes the
  book disappear on owner death; S3 is the reader-side reaction).

### Conflicts / risks
- Distinguish the two "no book" causes at the poll: **owner-not-yet-up**
  (should be the S1 `awaiting_owner` retry path, before the dial) vs
  **owner-died-after-establishment** (terminal here). At `CHECK_PEER_READY` time
  the dialer already got its `REG_ACK` (owner existed), so a later missing book
  is unambiguously owner-death → terminal. Confirm the ordering.

---

## Slice order + tests

1. **S1** first (establishment) — it introduces `awaiting_owner` + the retry;
   the highest-leverage, most-tested piece.
2. **S2** (teardown) — owner-aware close.
3. **S3** (fast-fail) — small, falls out of S2.

**Tests (L2 broker + L4 e2e):**
- L2: REG owner-vs-dialer gate (dialer-before-owner → `awaiting_owner`, not book-open / not `CHANNEL_NOT_FOUND`); owner-death closes book, dialer-death does not; `CHECK_PEER_READY` on missing book → `CHANNEL_NOT_FOUND`.
- L4: fan-in **producer-first** spawn (dialer races ahead) → producer retries `awaiting_owner`, consumer comes up, channel establishes, data flows (the case R6 was meant to handle, now via the retry). fan-in consumer(owner) kill → producers get `CHANNEL_CLOSING`; fan-in producer kill → channel lives, consumer keeps serving the rest.

## Doc fold (on completion)
- HEP-CORE-0017 §4.7.0.1/§4.7.0.2 already normative — mark code-complete.
- HEP-CORE-0007 — add `awaiting_owner` REG status.
- HEP-CORE-0023 §2.1.1 — reconcile the teardown comment to owner-aware.
- TOPOLOGY_TODO Phase D/E — S1–S3 done unblocks Phase E retirements.
