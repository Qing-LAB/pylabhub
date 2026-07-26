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
1. **Broker — gate at the TOP of each handler, before ANY state mutation.**
   **CRITICAL REVIEW FINDING (2026-07-26, code+doc): gating "before
   `_on_producer_added`" is TOO LATE.** `handle_reg_req` FILES A SCHEMA into
   `HubState.schemas` (Path A adoption / Path B self-registration, `~:2330-2400`)
   *before* `_on_producer_added` (`:2462`) — and a later reject "CAN leave an
   orphan schema record" (existing code comment). So a gated (`AWAITING_OWNER`)
   fan-in producer would leave an orphan schema for a not-yet-existent channel and
   pre-file a schema the consumer-owner never chose — a real consistency bug.
   Contract C2 ("a dialer never causes the hub to open a book") ⇒ the dialer must
   **mutate NO hub state**.
   - **Producer (`handle_reg_req`)** — gate at the TOP, right after the ABI-check
     block (`~:1996`, which the code marks "BEFORE any state mutation"), before
     the schema-filing step. All fields are available there (`channel_name:1976`,
     `body.channel_topology()`, `hub_state_->channel(name)`). If
     `parse(topology)==FanIn && !hub_state_->channel(name).has_value()` (the fan-in
     producer is the dialer, owner absent) → `make_error(corr_id,"AWAITING_OWNER",
     …)`; mutates nothing (mirrors the ABI-check reject pattern).
   - **Consumer (`handle_consumer_reg_req`)** — the existing `CHANNEL_NOT_FOUND`
     site (`:3069`) is ALREADY before any mutation (the `:3055` snapshot is
     read-only; `_on_consumer_joined` mutates only after). So just change that
     `CHANNEL_NOT_FOUND` → `AWAITING_OWNER` (fan-out / one-to-one consumer dialer).
   - Owner REG (+ no book) and dialer REG (+ book exists) → unchanged (fall
     through to `_on_producer_added` / `_on_consumer_joined` and validate as today).

   > The producer gate must NOT rely on `declared_topology` parsed at `:2440`
   > (that is after the schema step) — read `body.channel_topology()` directly at
   > the top. Empty-topology hazard still applies (defaults `OneToOne` ⇒ producer
   > treated as owner); pin with the L2 test below.
2. **Wire — `AWAITING_OWNER` is an ERROR `error_code`, NOT a new status tier**
   (verified 2026-07-26). The broker dispatch maps any non-`success` status to an
   `ERROR` frame (`:1505` / `:1529`), and `do_request` surfaces that ERROR
   reply's json (with `error_code`) to the caller — the same path
   `CHANNEL_NOT_FOUND` already rides to reach the role. So emit
   `make_error(corr_id, "AWAITING_OWNER", …)`; **no dispatch / `do_request` /
   new-status change**. The role branches on `error_code == "AWAITING_OWNER"`
   (retry) vs other codes (fatal), exactly as it already distinguishes
   `CHANNEL_NOT_FOUND`. Trade-off (acceptable per C3): a genuinely wrong channel
   name now fails after `init_timeout` (retries) rather than instantly — the
   dialer cannot distinguish "owner not up yet" from "never" at REG time.
3. **Role host — Tier-2 retry loop (C3), NEW.** `BrokerRequestComm::register_channel`
   is a one-shot `do_request("REG_REQ","REG_ACK",…)` — **verified: no existing
   REG-level retry** (the `finalize_channel_connect` poll is a *different*,
   post-REG_ACK loop). So S1 adds a retry loop **at the role host's REG call
   site** (the `build_tx`/`build_rx` establishment path): on a REG_ACK with
   `status=awaiting_owner`, re-call `register_channel`. The script never sees it
   (`on_init` has not run). **The loop MUST be hard-bounded — three stop
   conditions (no dead-loop is possible):**
   - **Time cap** — total elapsed ≤ `init_timeout_ms` (the establishment budget).
     On exhaustion → **fatal-abort** with a misconfiguration diagnostic; the role
     never hangs. (Contract C3.)
   - **Backoff interval** — a bounded wait between attempts so it never
     tight-spins / hammers the broker (reuse the finalize-connect poll cadence).
   - **Cancellation / hub-dead** — honors the `is_cancelled` token (shutdown /
     SIGTERM during startup breaks it *immediately*) and aborts if the broker
     connection is lost (hub-dead).

   Note: `CHANNEL_CLOSING_NOTIFY` does NOT gate this loop — during `awaiting_owner`
   no channel exists yet (the owner has not bound), so there is nothing to close;
   the bound is time + cancellation, consistent with the broker being a pure
   responder that never pushes "give up". (Once established, `CHANNEL_CLOSING_NOTIFY`
   is the stop signal — the S2/T2 post-establishment path.)  Owner-comes-then-dies
   mid-retry churns `awaiting_owner` until the time cap → clean abort (bounded,
   not instant; acceptable failure path).

### Conflicts / risks to verify at code time
- **The dialer MUST declare `channel_topology` on a fresh channel.** Empty wire
  topology defaults to `OneToOne` for a fresh channel (`:2436`), which would
  mis-classify a fan-in producer as the *owner* and open a book. The gate is only
  correct if the dialer's REG carries `channel_topology=fan-in` (it should — the
  role config declares each side's channel topology). Pin this in the gate + an
  L2 test: a fan-in producer REG with empty topology must NOT silently become a
  `OneToOne` owner.
- **Gate at the TOP, before schema filing** (change #1): the producer gate sits
  before `HubState.schemas` is touched (`~:2330`), so a dialer-before-owner leaves
  ZERO broker state — no schema record, no producer record, no book. Gating merely
  before `_on_producer_added` is INSUFFICIENT (schema filing precedes it → orphan
  schema). L2 pin: after a fan-in producer's `AWAITING_OWNER`, `HubState` has no
  channel AND no orphan schema record for it.
- **HEP-0042 admission ledger**: gating book-open to the owner does not touch
  ledger seeding (INVARIANT-BIND-CONFIRM-1..3) — the dialer admits into the
  owner's *existing* ledger, unchanged.
- **Processor (C7)**: the gate is per channel / per REG, so an
  owner-on-one-side / dialer-on-the-other processor needs no special case.

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

### Current code (verified 2026-07-26 — broker side is ALREADY correct)
- `handle_check_peer_ready_req` (`:4399`) **already returns `CHANNEL_NOT_FOUND`
  when `!ch.has_value()`** (`:4416`) — the owner-died / book-gone case. The
  `not_ready` reply (`:4480`) is the *channel-exists-but-no-owner-present-yet*
  case (correct). **So the broker half of S3 is DONE.**
- The remaining gap is ROLE-SIDE: does the dialer's `finalize_channel_connect` /
  CHECK_PEER_READY poll treat a `CHANNEL_NOT_FOUND` reply as a **terminal
  fast-fail** (abort establishment) vs keep polling until `init_timeout`?

### The change
- Role-side only: on a `CHECK_PEER_READY_ACK` / ERROR carrying `CHANNEL_NOT_FOUND`,
  the poll aborts establishment fast with a clean diagnostic instead of burning
  the `init_timeout` budget. No broker change. S3 is a small role-side reaction
  that falls out of S2 (S2 makes the book disappear on owner death).

### Conflicts / risks
- Distinguish the two "no book" causes at the poll: **owner-not-yet-up**
  (should be the S1 `awaiting_owner` retry path, before the dial) vs
  **owner-died-after-establishment** (terminal here). At `CHECK_PEER_READY` time
  the dialer already got its `REG_ACK` (owner existed), so a later missing book
  is unambiguously owner-death → terminal. Confirm the ordering.

---

## Slice order + tests

> **Integration-order finding (2026-07-26, verified by building S1's gate).**
> The gate alone reds ≥3 fan-in L3 wire tests that register a producer FIRST with
> no consumer-owner (`Pattern4BrokerConsumer.ConsumerReg_ChannelNotFound`,
> `Pattern4Metrics.FanInTwoProducersMetricsDoNotOverwrite`,
> `Pattern4AttachCoordination.WaitPathDrainOnProducerDisconnect`). These are the
> **integration point** and MUST be migrated *after* S1–S3, not during S1:
> - `ConsumerReg_ChannelNotFound` → re-pin to `AWAITING_OWNER` (pure S1).
> - The two fan-in tests need consumer-first ordering (S1's gate) **and** the
>   owner-aware teardown behavior — `WaitPathDrainOnProducerDisconnect` tests
>   *producer-disconnect on a fan-in channel*, which is **exactly what S2
>   changes** (dialer-drop must not close). Migrating it before S2 would pin the
>   superseded producer-centric teardown.
>
> **Therefore: land S1 (gate + retry) + S2 (teardown) + S3 as one coherent unit,
> THEN migrate the fan-in tests, THEN one green commit. Do NOT commit the gate
> standalone.**

1. **S1** — gate (`AWAITING_OWNER`) + role-host retry.
2. **S2** — owner-aware teardown (`hub_state.cpp:1507/1540/1875`).
3. **S3** — role-side fast-fail (broker half already done).
4. **Test migration (last):** consumer-first ordering + owner-aware-teardown
   expectations on the fan-in L3 tests, plus the new L2/L4 pins below.

**Tests (L2 broker + L4 e2e):**
- L2: REG owner-vs-dialer gate (dialer-before-owner → `awaiting_owner`, not book-open / not `CHANNEL_NOT_FOUND`); owner-death closes book, dialer-death does not; `CHECK_PEER_READY` on missing book → `CHANNEL_NOT_FOUND`.
- L4: fan-in **producer-first** spawn (dialer races ahead) → producer retries `awaiting_owner`, consumer comes up, channel establishes, data flows (the case R6 was meant to handle, now via the retry). fan-in consumer(owner) kill → producers get `CHANNEL_CLOSING`; fan-in producer kill → channel lives, consumer keeps serving the rest.

## Doc fold (on completion)
- HEP-CORE-0017 §4.7.0.1/§4.7.0.2 already normative — mark code-complete.
- HEP-CORE-0007 — add `awaiting_owner` REG status.
- HEP-CORE-0023 §2.1.1 — reconcile the teardown comment to owner-aware.
- TOPOLOGY_TODO Phase D/E — S1–S3 done unblocks Phase E retirements.
