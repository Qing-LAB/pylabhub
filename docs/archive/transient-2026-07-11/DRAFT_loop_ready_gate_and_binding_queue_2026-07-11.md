# DRAFT — Loop-Ready Gate and Binding-Queue Resolution

**Status:** DRAFT (2026-07-11).  For discussion and approval before
HEP-CORE-0036 / HEP-CORE-0011 amendment and any code work.
**Anchors after landing:** HEP-CORE-0036 §4.3.2, §6.5, §8.3;
HEP-CORE-0011 callback table + new §"Loop-ready gate".
**Session tasks:** harness Tasks #1–#8 (see `TaskList`).

**Self-review 2026-07-11:** verified against code —
`handle_channel_auth_notifies` cast site at `role_api_base.cpp:2258`
(hardcoded `tx_queue`), `is_channel_ready` channel-resolution
pattern at `role_api_base.cpp:2043` (used in §4.1 sketch),
`apply_consumer_reg_ack` populating `allowlist_cache` with
`script_view` (`role_api_base.cpp:1361-1366`), `retry_acquire`
first-cycle spin (`data_loop.cpp:41-48`).  `allowed_peers` +
`admitted_peers_count` distinction: `allowed_peers` returns
`allowlist_cache.snapshot(channel)` and already exists at
`role_api_base.cpp:1978`; `admitted_peers_count` is proposed as a
convenience alias (`return allowed_peers(channel).size()`).

---

## 1. Why this note exists

Two problems, exposed together by the fan-in singular-side migration,
that are actually two independent bugs sharing a fix window:

1. **Fan-in binding-side reader deadlocks on the first cycle of the
   data loop.**  The consumer's `retry_acquire` blocks in
   `src/utils/service/data_loop.cpp:14` waiting for data on its PULL
   socket.  Data can only arrive after producers CURVE-authenticate.
   Producers can only CURVE-authenticate after the consumer's ZAP
   allowlist admits their pubkey.  The allowlist is updated by
   processing `CHANNEL_AUTH_CHANGED_NOTIFY`, which is drained by the
   worker inside `invoke_and_commit` — which the blocked worker never
   reaches.  Circular.

2. **The ZAP allowlist on a binding-side consumer is never populated
   even if the deadlock is bypassed.**  In
   `src/utils/service/role_api_base.cpp:2258`,
   `handle_channel_auth_notifies` dynamic-casts `pImpl->tx_queue` to
   `sec::PeerAdmission` to apply `set_peer_allowlist`.  On a
   binding-side consumer, `tx_queue` is null → cast returns null →
   code falls into the "non-PeerAdmission tx queue (SHM)" branch and
   updates the script-side cache only.  The `rx_queue` (which is the
   real binding side, running ZAP as server) never sees
   `set_peer_allowlist`.

Both bugs share a root cause: the framework was built around implicit
assumptions that held for every previous topology and quietly break
for the new one.

## 2. The implicit contract that was already there

Before the singular-side migration, the framework relied on an
asymmetry between roles that nobody wrote down as a contract:

| Side | `acquire()` blocks? | If no peer yet, why does it work? |
|---|---|---|
| Writer (producer) | No — `write_acquire` returns a slot immediately | Writes to void; ZMQ drops.  Loop runs, drains NOTIFYs each cycle, allowlist grows, next writes are delivered. |
| Reader dialing side | Briefly — `read_acquire` blocks until the dialed peer sends | Peer identity is carried by `REG_ACK.producers[]`; the reader dials to a known target that is already up. |
| Reader binding side | **Yes — blocks forever with no peer** | **Never existed before.** |

The producer got away with binding-with-no-peer because writing is
fire-and-forget.  The reader-dialing case got away with it because
`REG_ACK` carried the peer set.  The reader-that-binds case has
neither escape — it is the first topology in the framework where
"data-plane socket is up" and "there is a peer whose data can reach me"
are not the same fact.

## 3. What is actually new about fan-in binding-side

Two implicit assumptions the framework quietly relied on both break at
once for the reader-that-binds:

- **"Writers can run without peers."** (Still true.)
- **"Readers know their peers at REG_ACK."** (True when readers only
  dialed.  False for the new binding-side reader: `REG_ACK.producers`
  is `[]`; peers are learned later via
  `CHANNEL_AUTH_CHANGED_NOTIFY`.)

`Registered → Authorized` (HEP-CORE-0036 §4.3.2) fires as soon as
`REG_ACK` applies, on the theory that "Layer 3 data plane armed"
means we're ready to loop.  For a reader-that-binds that is not true:
presence being `Authorized` says the socket is up, not that a peer has
been admitted.

## 4. Two decoupled fixes

Fix A is a defect in existing code.  Fix B is a contract addition on
top.  They are correct in either order, but both must land together
because Fix B's default gate reads state that Fix A is responsible for
keeping consistent.

### 4.1 Fix A — `handle_channel_auth_notifies` resolves the binding queue by channel

Replace the hardcoded `dynamic_cast<sec::PeerAdmission*>(pImpl->tx_queue.get())`
with a per-channel lookup:

```
// Resolve the channel to whichever queue on THIS role holds it.
// Same pattern as is_channel_ready (role_api_base.cpp:2043):
//   pImpl->channel      is the primary channel; single-side roles
//                       have it on rx_queue OR tx_queue (never both).
//   pImpl->out_channel  is the processor's tx-side channel when
//                       distinct from the primary.
hub::Queue *q = nullptr;
if (!pImpl->out_channel.empty() && channel == pImpl->out_channel)
    q = pImpl->tx_queue.get();
else if (channel == pImpl->channel)
    q = pImpl->rx_queue ? pImpl->rx_queue.get() : pImpl->tx_queue.get();

sec::PeerAdmission *binding = nullptr;
if (q && q->is_binding_side())
    binding = dynamic_cast<sec::PeerAdmission*>(q);

if (binding) binding->set_peer_allowlist(allowlist);
else         /* non-binding side for this channel — nothing to seed; ZAP is not our concern */
```

The channel-to-queue resolution follows the existing pattern in
`is_channel_ready` — no new accessor on `hub::Queue` needed.
Exactly one of `rx_queue` / `tx_queue` is the binding side for a
given channel, because a role never binds twice for the same
channel.  This is a role-local invariant paired with the
broker-side "at most one binding-side role per channel" rule.

Downstream cache update (`allowlist_cache.put`) and the
`invoke_on_allowlist_changed` callback fire regardless of which
side found the binding queue — those are pure observability and
belong on both sides.

### 4.2 Fix B — `invoke_on_init` returns a status; the loop re-enters it each cycle until Ready

Change `ScriptEngine::invoke_on_init` from `void` to
`InitStatus { NotReady, Ready }`.  Move the invocation from the
one-shot call in each role host into the top of every cycle in
`run_data_loop`:

```
while (base_gates) {
    if (!init_done) {
        bool default_ready = ops.default_init_ready(api);
        bool script_ready  = engine.has_callback("on_init")
                           ? (engine.invoke_on_init() == Ready)
                           : true;
        init_done = default_ready && script_ready;
        if (!init_done && elapsed > init_timeout_ms)
            { fatal("event=LoopInitTimeout"); break; }
    }

    if (init_done) { Step A: acquire();  Step B: deadline_sleep; }
    Step B': shutdown_check;
    Step C:  drain_messages;  drain_inbox_sync;
    Step D+E: invoke_and_commit;  // NOTIFY apply still runs; user
                                  // callback skipped when data==null
    Step F/G: metrics; next_deadline;
}
```

Cycle-by-cycle trace for the fan-in binding-side reader:

- Cycle 1: `on_init` sees `admitted_peers==0` → NotReady.  Step A
  skipped.  Step C drains any queued NOTIFYs.  Step D+E's
  `handle_channel_auth_notifies` applies them → allowlist grows.
- Cycle 2: `on_init` sees `admitted_peers==1` → Ready → `init_done`.
  Step A runs; first data arrives; cycle completes normally.
- Cycle 3+: `init_done` true; on_init not called again.

## 5. AND composition — default is an invariant, not a fallback

```
init_done = Ops::default_init_ready(api) && script_hook_result
```

Consequences:

- Script that returns `Ready` cannot start the loop while
  `admitted_peers < 1` on a reader.  The framework's minimum-peer
  guarantee is a floor the script cannot lower.
- Script that returns `NotReady` can hold the loop even after the
  framework minimum is met — e.g. waiting for a specific peer UID
  in the allowlist, or `admitted_peers >= 3`.
- No script hook → default alone decides.  A pre-existing script
  that returns void / `None` / nil is treated as `Ready`; nothing
  breaks.

## 6. Per-role default gate — same loop, per-role predicate

The main loop is one template (`run_data_loop`).  The `Ops` types
already differ by direction (`ProducerCycleOps`,
`ConsumerCycleOps`, `ProcessorCycleOps`).  The only new per-role
piece is `Ops::default_init_ready(api)`:

| Role      | Default returns true when |
|-----------|---------------------------|
| Producer  | `true` (fire-and-forget stays the default; script may override) |
| Consumer  | `api.admitted_peers_count(ch) >= 1` for every rx-side channel (dialing or binding) |
| Processor | Consumer rule on rx side; tx side always `true`; AND both |

The predicate reads `allowed_peers(channel).size()` (the
`allowlist_cache` snapshot) — this is the admitted count from
the broker's perspective, which reflects what ZAP will actually
enforce once Fix A wires the cache write to the correct queue.
It is NOT `producer_count` / `consumer_count` (those read
`live_peers`, populated only on `phase=live` NOTIFYs — a peer can
be admitted but not yet heartbeating; making the gate wait for
`live` adds a round-trip on top of what CURVE already requires).

## 7. Impact matrix — roles × topologies

| Role      | Topology (this role's side) | Binding queue | Fix A applies allowlist to | Fix B default gate |
|-----------|-----------------------------|---------------|----------------------------|--------------------|
| Producer  | fan-out (binds)             | `tx_queue`    | `tx_queue` (unchanged from today) | `true` (writer default) |
| Producer  | fan-in (dials)              | none          | none — non-binding side; script cache only | `true` |
| Producer  | 1-to-1 (binds)              | `tx_queue`    | `tx_queue`                 | `true` |
| Producer  | 1-to-1 (dials)              | none          | none                       | `true` |
| Consumer  | fan-in (binds)              | `rx_queue`    | **`rx_queue` (NEW — Fix A fixes)** | `admitted_peers_count(ch) >= 1` |
| Consumer  | fan-out (dials)             | none          | none                       | `admitted_peers_count(ch) >= 1` (peer is known from REG_ACK so this is met at cycle 1) |
| Consumer  | 1-to-1 (binds)              | `rx_queue`    | `rx_queue` (NEW)           | `admitted_peers_count(ch) >= 1` |
| Consumer  | 1-to-1 (dials)              | none          | none                       | `admitted_peers_count(ch) >= 1` |
| Processor | fan-in input + fan-out output | rx_queue (input) + tx_queue (output) | Fix A per channel: input NOTIFY → rx_queue; output NOTIFY → tx_queue | rx side: `>=1` on input channel; tx side: `true` |
| Processor | fan-out input + fan-in output | none input + none output | none (both sides dial) | rx side: `>=1` (peer from REG_ACK); tx side: `true` |
| Processor | mixed 1-to-1                | Whichever side binds | Fix A per channel | rx side: `>=1`; tx side: `true` |

Points to note:

- Producer with `default_init_ready = true` preserves today's
  behaviour — the producer that "accidentally worked" continues to
  work identically.  Its data loop enters at cycle 1 exactly like
  today.
- Consumer-that-dials (fan-out) meets the default at cycle 1 because
  `REG_ACK.producers[]` populates the allowlist_cache directly in
  `apply_consumer_reg_ack`, before the loop starts.
- Processor with mixed sides is not a special case — each channel is
  evaluated by its own side using the same rule.

## 8. What does NOT change

- **Thread model.**  Worker still owns the data loop and admin
  apply.  Ctrl thread still receives NOTIFYs on the BRC and enqueues
  onto the core message queue.  No new synchronization primitives.
- **FSM semantics.**  `Registered → Authorized` still fires at
  `REG_ACK` apply; §4.3.2 is amended only to *distinguish* this
  FSM-level fact from loop-ready, not to change when it fires.
- **NOTIFY dispatch table.**  `kNotificationTable` (`cycle_ops.hpp`)
  is unchanged.  Every existing script callback contract holds.
- **Callback surface.**  Only `invoke_on_init`'s return type is
  new.  No new hooks, no new native ABI beyond the mirror struct
  for `InitStatus`.
- **NOTIFY apply latency.**  Applying `ChannelAuthChanged` on the
  ctrl thread instead of the worker is a separate robustness idea,
  worth considering later.  It is not required to fix either bug.

## 9. User callback contract for `on_init` — graceful handling

A user script that defines `on_init` must:

- **Return promptly.**  `on_init` is called every cycle until it
  returns `Ready`.  A blocking or long-running call in `on_init`
  stalls the loop.  If the script needs to wait on external state,
  it should sample and return `NotReady`, not sleep.
- **Be idempotent under repeated invocation.**  Any side effect
  (e.g., caching a discovered peer UID into a script-local) must
  survive being run N times.
- **Consume state, not mutate framework state.**  Safe to call from
  `on_init`: `api.allowed_peers(channel)`,
  `api.admitted_peers_count(channel)`, `api.producers(channel)`,
  `api.consumers(channel)`, `api.is_channel_ready(channel)`.  Not
  safe: `api.write_acquire`, `api.read_acquire`, anything that
  mutates the data plane before the loop has decided data can
  flow.
- **Respect the startup timeout.**  The framework enforces a wall-
  clock budget (same shape as `wait_for_roles` — HEP-CORE-0023).
  A `NotReady` that never turns `Ready` within the budget is a
  fatal role-startup error; the framework tears down cleanly and
  logs `event=LoopInitTimeout` with the elapsed time and unmet
  gates.
- **Handle exceptions defensively.**  An exception thrown from
  `on_init` is caught and logged; the framework treats the cycle
  as `NotReady`.  Scripts should not rely on exceptions for
  control flow — return `NotReady` explicitly.

Return-value coercion (per engine):

- **Python:** `True` / `1` → Ready; `False` / `0` → NotReady;
  `None` / return-nothing → Ready (back-compat).  Any other value
  is coerced with `bool(...)`.
- **Lua:** `true` → Ready; `false` → NotReady; return-nothing →
  Ready (back-compat).
- **Native:** `plh_init_status_t` enum with `PLH_INIT_READY` /
  `PLH_INIT_NOT_READY`.  **Native C ABI bumped v3 → v4.**  Existing
  plugins compiled against the pre-amendment header (void-returning
  `on_init`) MUST rebuild against `native_engine_api.h` v4 — calling
  a void-return function through the amended status-return pointer
  is undefined behavior on some target ABIs.  `NativeEngine::load_plugin`
  refuses v3 plugins at load time with an actionable error surfaced
  via `plh_plugin_abi_version`.  Plugins with no `on_init` symbol at
  all continue to be treated as `Ready` (missing hook path is
  ABI-independent).

## 10. Follow-ups — not in scope for this arc

- **NOTIFY apply on ctrl thread.**  `ChannelAuthChanged` and
  `ConsumerDied` mutate framework state that gates data flow;
  applying them on the ctrl thread instead of the worker would
  reduce first-admission latency by ~1 cycle.  Independent
  robustness improvement.
- **Consolidation with `wait_for_roles` (HEP-CORE-0023).**  Both
  gate loop entry on peer visibility; the difference is `wait_for_roles`
  polls broker for role registration while `on_init` reads the
  local admitted-peer view.  Long-term, one unified pre/per-loop
  mechanism may be cleaner.  Not decided here.
- **Overlap with `on_channel_ready` (HEP-CORE-0042 §7.1).**
  Consumer's pre-attach loop fires `on_channel_ready` once per
  channel when the attach completes.  It is a per-channel
  observation callback, not a gate.  Loop-ready is a per-cycle
  gate.  They do not conflict but the doc should say so
  explicitly when §7.1 is amended.
