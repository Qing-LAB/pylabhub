# Objective peer counts — locked design (#74)

| | |
|---|---|
| **Status** | SHIPPED — 2026-07-26. Implemented (role + broker + `CHANNEL_COUNT_NOTIFY` wire + L4 e2e fan-in). Folded into HEP-CORE-0028 §6a.2/§6a.3, HEP-CORE-0007 (catalog), HEP-CORE-0017 §3.3.2, HEP-CORE-0036 §I11. Ready to archive per DOC_STRUCTURE §2.2. |
| **Task** | #74 — "Complete objective peer-count accessors (currently binding-side-only)". |
| **Folds into** | HEP-CORE-0028 §6a (accessor semantics), HEP-CORE-0007 (wire catalog), HEP-CORE-0036 §I11 (notification fan-out), HEP-CORE-0017 §3.3.2 (dialing side). Merge on completion, then archive. |

---

## 1. The principle (non-negotiable)

`producer_count(channel)` and `consumer_count(channel)` return the **objective total**
number of live producers / consumers on the channel — the true fact of the
channel's membership. It is:

- **the same number for every role that asks**, on either side, in every topology,
- **self-inclusive** (a member is part of the total),
- **not** role-relative, **not** derived differently per side, **not** a config guess.

There is nothing to hide or bend. The count is a fact; the only job is to deliver
that fact to every role.

"Live" = the broker has received the peer's first heartbeat
(HEP-CORE-0036 §3.5.2 — the peer's data-plane wire is ready).

**Worked truth** — fan-in, 3 producers → 1 consumer. *Every* role — the consumer
and each of the three producers — reads the same two numbers:

| channel fact | `producer_count` | `consumer_count` |
|---|---|---|
| 3 producers, 1 consumer live | **3** | **1** |

Fan-out 1P → 3C: everyone reads `producer_count=1`, `consumer_count=3`.
One-to-one: everyone reads `1` / `1`. A peer death drops the relevant number by one
for everyone.

---

## 2. Why it is broken today

The count is backed by `live_peers`, which is populated only by the `phase=live`
notification. The broker sends `phase=live` to the channel's **binding (controlling)**
side only, about the **dialing** peers. Result: the controlling side sees the *other*
side's count; its own side and the entire dialing side read **0**
(HEP-CORE-0028 §6a.2 "current limitation").

---

## 3. Two notifications, two purposes — do not conflate

```mermaid
flowchart TD
    B[Broker: authoritative HubState<br/>liveness = Connected + first_heartbeat_seen]
    B -- "per-peer IDENTITY stream<br/>CHANNEL_AUTH_CHANGED_NOTIFY<br/>(admitted / live / left, role_uid+role_type+pubkey)" --> C[Binding / controlling side ONLY<br/>manages admission + identity lists<br/>producers() / consumers()]
    B -- "channel-level COUNT<br/>CHANNEL_COUNT_NOTIFY<br/>{producer_count, consumer_count}" --> ALL[EVERY member, both sides<br/>backs producer_count() / consumer_count()]
```

1. **Per-peer identity stream** — `CHANNEL_AUTH_CHANGED_NOTIFY`
   (`phase = admitted | live | left`), carrying one peer's **identity**.
   *Purpose:* the controlling (binding) side learns **which** role on the other
   side joined/left, to manage admission (allowlist) and expose the identity
   lists `producers()` / `consumers()`.
   **Recipient: the binding side only. UNCHANGED by #74.**

2. **Channel-level count** — the objective `{producer_count, consumer_count}`.
   *Purpose:* every role knows the true membership count.
   **Recipient: every member of the channel, both sides.**
   This is channel-level *status* (a number, not an identity) — the same class of
   information the dialing side already receives as ready / closing. The dialing
   side, which controls nothing, receives only channel-level status, never the
   per-peer identity stream.

---

## 4. Mechanism

### 4.1 Broker — the single authoritative source

- **Live, not registered.** The count is of *live* members (`RoleState` kLive =
  `Connected` + `first_heartbeat_seen`), from `HubState::ChannelEntry.producers` /
  `.consumers`. This is DISTINCT from the existing `HubState::producer_count()` /
  `consumer_count()`, which return `.size()` — the *registered* count. Add a
  clearly-named live counter (e.g. `HubState::live_producer_count(channel)` /
  `live_consumer_count(channel)`) so the two are never confused; the broker's
  `compute_channel_live_counts` reads those.
- `fire_channel_count_notify(channel)` sends `CHANNEL_COUNT_NOTIFY` with the two
  live numbers to **all** members of the channel that have a captured
  `zmq_identity`.
- **Firing sites.** The count changes on per-member *liveness* transitions, so it
  fires at those sites — the existing `subscribe_channel_status_changed` hook is
  **insufficient** (it fires only on the aggregate `ChannelObservable` Pending↔Live
  transition, not when a 2nd producer joins an already-Live channel). Fire at:
  - **join-live** — `handle_heartbeat_req`, on the one-shot gate that flips
    `first_heartbeat_seen` (`eff.presence_found && !eff.was_first_heartbeat_seen`),
    for *any* role (binding or dialing);
  - **leave** — every path that drops a live member: `handle_disc_req`,
    `check_heartbeat_timeouts` → `_on_heartbeat_timeout` → `on_consumer_closed`
    (and the producer-side equivalents / channel-close cascade).
  Enumerate and cover ALL leave paths (INV-4); a missed one leaks a stale count.
- **Compose, don't parallel-plumb.** Where the broker already subscribes to
  HubState events (`hub_script_runner.cpp` / broker registration) to fan wire
  notifications, add the count-notify emission alongside the existing fan-out for
  the same event rather than opening a second traversal.
- `fire_channel_auth_changed_notify` (the per-peer identity stream) is **unchanged**:
  still binding-side only. Note: on a *dialing* peer's first heartbeat the broker now
  fires **both** — `phase=live` to the binding side (identity, unchanged) *and*
  `CHANNEL_COUNT_NOTIFY` to all members (count). Two messages, two purposes; accepted.

### 4.2 Role — store the number, return it

- New `NotificationId::ChannelCount` mapped from wire `"CHANNEL_COUNT_NOTIFY"`.
- `pImpl` gains `channel_counts: channel_name -> {producers, consumers}`, guarded by
  a mutex (broker-poll thread writes; script thread reads).
- The dispatch handler for `CHANNEL_COUNT_NOTIFY` stores the numbers and consumes
  the message. **Infrastructure-only — no user callback** (identical treatment to
  the `phase=live` `live_peers` update: framework bookkeeping, not a script event).
- `producer_count()` / `consumer_count()` return the stored `channel_counts` value
  (0 if the channel is unknown or no count has arrived yet).
- `producers()` / `consumers()` (identity lists) keep reading `live_peers` —
  binding-side only, as before. The dialing side has the number, not identities,
  by design.

> Note: the count source moves from `live_peers.size()` to the broker's
> authoritative `CHANNEL_COUNT_NOTIFY` number **for every role including the
> binding side** — one source of truth, no parallel count plumbing. `live_peers`
> remains solely for the identity-list accessors.
>
> **Consequence (by design, not a bug):** `producers().size()` /
> `consumers().size()` will NOT equal `producer_count()` / `consumer_count()`.
> The identity lists are the binding side's view of the *other* side's live peers
> (never self, never the dialing side); the counts are the *objective total*
> (self-inclusive, every side). A lone fan-out producer sees `producers()` empty
> but `producer_count() == 1`. Documented so the divergence is not mistaken for a
> defect. (If a future need arises for objective identity lists on all sides, that
> is a separate broker change — out of scope for #74.)

---

## 5. Invariants (test against these)

- **INV-1** — a role's count for a channel equals the broker's authoritative live
  count at the last `CHANNEL_COUNT_NOTIFY` it received.
- **INV-2** — the count is identical across all roles of a channel (modulo
  notification propagation latency).
- **INV-3** — the count is self-inclusive: a live member is part of the total.
- **INV-4** — a peer leaving (death / timeout / DISC) decrements every member's
  count.
- **INV-5** — the per-peer identity stream (`CHANNEL_AUTH_CHANGED_NOTIFY`) routing
  is unchanged: binding side only. `producers()`/`consumers()` and the join
  callbacks are unaffected.

---

## 6. Wire — `CHANNEL_COUNT_NOTIFY` (new, HEP-CORE-0007 catalog)

- Direction: broker → role, fire-and-forget (same class as `CHANNEL_CLOSING_NOTIFY`).
- Body: `{ channel_name, producer_count: uint, consumer_count: uint }`. No
  `channel_version` — the count is not an admission-version event.
- Delivered to every member of the channel with a captured `zmq_identity`.
- **This is channel-level *status* — a number, carrying no peer identity.** It is
  the same *class* of information the dialing side already receives (ready /
  closing): the dialing side never receives the per-peer identity stream, only
  channel-level status, of which the count is one kind. It fires on every
  membership change because the count must stay live-correct (INV-1/INV-2), not
  only at ready/closing edges.
- Verified 2026-07-26: no existing count/status wire notification to extend — the
  HubState event framework is C++-internal (`subscribe_*`), not a wire type; the
  only channel-status wire notify today is `CHANNEL_CLOSING_NOTIFY`. A new type is
  justified.

---

## 7. Code touch-points

**Broker (`src/utils/ipc/broker_service.cpp`)**
- add `compute_channel_live_counts(channel)`,
- add `fire_channel_count_notify(socket, channel)` → fan to all members,
- call it on first-heartbeat (`handle_heartbeat_req`) and on every peer-leave path
  (DISC handler, death detection, `check_heartbeat_timeouts`),
- leave `fire_channel_auth_changed_notify` binding-only.

**Role (`src/include/utils/role_host_core.hpp`, `src/utils/service/role_api_base.cpp`)**
- `NotificationId::ChannelCount` + `"CHANNEL_COUNT_NOTIFY"` mapping,
- `pImpl.channel_counts` + guarded handler that stores the numbers,
- `producer_count()`/`consumer_count()` return the stored number.

---

## 8. Tests

- **L2 broker** — live-count query correct across topologies (live vs registered);
  `CHANNEL_COUNT_NOTIFY` fans to all members with updated numbers on join-live and
  on each leave path.
- **L4 e2e** — fan-in 3P → 1C: every role (the consumer and each producer) reads
  `producer_count=3, consumer_count=1`; kill one producer → every role reads `2`.
- **Existing-assertion handoff (mandatory).** Audit every current
  `producer_count`/`consumer_count` assertion before changing the source. Two
  distinct accessors share the name:
  - `HubState::producer_count()/consumer_count()` = registered `.size()` —
    `test_hub_state.cpp` etc. — **unaffected** (not the script accessor).
  - `RoleAPIBase::producer_count()/consumer_count()` = the script accessor whose
    backing moves from `live_peers.size()` to `CHANNEL_COUNT_NOTIFY`. Any test
    pinning it (e.g. `test_dispatch_notifications`, datahub/pattern4 workers) must
    be re-pinned to the new source and to the objective (self-inclusive) numbers —
    per the test-retirement-is-a-tracked-handoff rule, update the assertion, don't
    silently drop it.

---

## 9. HEP folding (on completion, then archive this draft)

- **HEP-CORE-0028 §6a.2** — replace the "current limitation" block: counts are now
  objective for every role; count source is `CHANNEL_COUNT_NOTIFY`; identity lists
  remain binding-side.
- **HEP-CORE-0007** — add `CHANNEL_COUNT_NOTIFY` to the wire catalog.
- **HEP-CORE-0036 §I11** — document the channel-count fan-out to all members,
  distinct from the binding-only per-peer identity stream.
- **HEP-CORE-0017 §3.3.2** — the dialing side's count is delivered as channel-level
  status.
