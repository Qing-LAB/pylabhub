# Authentication / PeerAdmission TODO

**Scope:** Open items for HEP-CORE-0035 (Hub-Role auth + federation
trust) and the PeerAdmission feature track that closes out the
data-channel CURVE auth gate.

**Authoritative design lives in:**

- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md`
  — Layer-1 ZAP + Layer-2 federation trust gate + key-file ACL
  discipline (§4.6) + runtime key handling (§4.7).
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md`
  — Three-tier auth (transport / identity / authorization),
  `ChannelAccessIndex` (§4.1), channel-auth notify+pull wire (§6.5
  amended 2026-06-04), per-producer pubkey + endpoint (§6.4), **one
  pubkey per role uid (§I10, enforced in `KnownRolesStore` with
  RELEASE-always-on + DEBUG-WITH_TEST bypass)**.
- `docs/HEP/HEP-CORE-0017-Queue-Abstraction.md` §3.3 —
  `RxQueueOptions::producer_peers` queue auth contract.
- `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md` — KeyStore +
  LockedKey + SecureMemorySubsystem framework primitives (use-not-
  export discipline, §8.2 / §8.4 / §8.5.1 / §8.6).

**Status source of truth:** `docs/TODO_MASTER.md` (when PeerAdmission
work is the active sprint).

**Completed-work archives** (verbatim prose retained for context):

- `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
  — Phase A/B/C; D1+D2+D3; landing-phase §4.6.5 no-bypass cleanup; BRC
  monitor investigation; lib-stabilization exclusion procedure;
  resolved decisions; considered-but-not-pursued.
- `docs/archive/transient-2026-06-09/todo-completions/AUTH_TODO_completions.md`
  — 2026-06-05 HB-1..HB-6 audit + 2026-06-05 PM REFRAME + C1..C5
  cleanup chain (full inventory + per-commit scope) + HEP-CORE-0040
  impl chain (#165–#176 + #187) + Phase D close-out follow-ons.

---

## Design principles — single source of truth for AUTH-N execution

**Every entry in this doc — current or future — anchors to these HEP
sections.  Implementations that contradict them are wrong; design
discussions that re-litigate them are wasted.**

1. **HEP-CORE-0036 §I11 — Framework provides protocol; scripts
   provide coordination.**  The framework guarantees validated
   identity, asynchronous notification of membership changes, atomic
   allowlist updates, and observable list state.  It does NOT
   synchronise roles' decisions.  Cross-role coordination (when to
   start, who's ready) is the script's job, implemented via the
   observable allowlist + band + inbox.
2. **HEP-CORE-0036 §6.5 producer-side handler flow (normative).**
   The notify-then-pull mechanism uses the existing BRC →
   `IncomingMessage` queue → worker-thread drain pattern.  BRC poll
   thread enqueues; worker thread drains, fires sync
   `GET_CHANNEL_AUTH_REQ` via the BRC, and applies the result via
   `ZmqQueue::set_peer_allowlist` (atomic under the queue's internal
   mutex).  On pull failure: log + return; recovery is via the next
   notify or via the existing hub-dead → re-register path.  No
   priority dispatch, no critical-error escalation, no new threads.
3. **HEP-CORE-0036 §I3 + §I5 + §8.2 — Race-window behaviour is
   PROTOCOL.**  Between broker decision and producer cache update,
   the producer's PUSH socket continues to serve handshakes using
   its current ZAP cache.  A consumer that joins during the gap
   retries CURVE until the cache converges; existing CURVE sessions
   are trusted for their lifetime.  This is the contract — not a
   bug to be plugged.  Scripts that want stricter "ready-to-emit"
   semantics gate on the observable allowlist themselves.
4. **HEP-CORE-0036 §I9 — Three-tier separation.**  Scripts never
   touch transport primitives.  Broker emits events; framework
   reacts via queue APIs; queue handles transport plumbing; script
   sees membership state, not sockets.

When extending AUTH-N work: cite these sections instead of
re-deriving.  If a proposed change appears to require new threading,
priority dispatch, or critical-error escalation around the auth
flow, re-read §I11 + §6.5 first — almost always it doesn't.

---

## Current PeerAdmission state

| Phase | Status | Notes |
|---|---|---|
| A — Abstraction (PeerAdmission interface) | shipped | see 2026-06-05 archive |
| B — KnownRole + CLI | shipped | see 2026-06-05 archive |
| C — ZapRouter + ZmqQueue CURVE | shipped | see 2026-06-05 archive |
| C-chain — Strict-CURVE cleanup (C1..C5) | shipped 2026-06-09 | see 2026-06-09 archive |
| HEP-0040 — Locked Key Memory impl chain | shipped 2026-06-09 | see 2026-06-09 archive |
| D — Broker glue (gate closes) | 🚧 D1+D2+D3 shipped; **D4–D7 open** | tracked as AUTH-1..6 below |
| E — Admin loopback enforcement | ⏸ planned | Unblocked once D ships |
| F — Federation peer ZAP parity | ⏸ planned | Depends on E + Federation HEP (#105) |
| G — SHM auth migration | ⏸ planned | AUTH-4 below; can interleave |
| H — Demo migration | ⏸ planned | Last; needs D shipped end-to-end |

---

## Critical path — AUTH-1 .. AUTH-7

Numbered renumbering of the live execution chain.  Each entry cites
its task-system ID + the historical label (D-step / HB-number) it
replaces so prior commits / docs cross-reference cleanly.

### AUTH-1 — Role-side dispatch + producer pubkey emission

> **Tracker:** task **#103** (in-progress).
> **Old labels:** A3 / D4 / D5 / HB-3 / B1 / B2.
> **Closes:** HEP-0036 §6.5 amended notify-then-pull wire; HB-3
> (`set_peer_allowlist` zero-callers); HEP-0036 §6.6
> `awaiting_endpoint` vocabulary gap; HEP-0036 §6.1 / §6.3 Layer-2
> identity-verification gap.

The single largest auth gate close-out — combines D4, D5, and the B1
+ B2 wire-shape fixes that surfaced during the 2026-06-05 audit.
Sub-deliverables:

1. **D5 — `CONSUMER_REG_ACK.producers[]`** ✅ shipped 2026-06-10
   (commit-1 of AUTH-1 chain).  Broker emits one entry per producer
   `{role_uid, pubkey, endpoint}` (HEP-CORE-0023 §2.1.1 fan-in); ZMQ
   transport only — SHM uses `shm_secret` (AUTH-4).  L3 pin:
   `DatahubBrokerConsumerTest.ConsumerRegAckEmitsProducersZmq` asserts
   the array shape on the consumer's ACK.
2. **B2 — Layer-2 identity verification** ✅ shipped 2026-06-10
   (same commit).  REVISED FROM ORIGINAL SCOPE: the audit's
   `zmq_msg_gets("User-Id")` plan was replaced with a **verification
   model** (per user decision 2026-06-10) — body carries declared
   `(role_uid, zmq_pubkey)` and broker verifies the pair against
   `cfg.known_roles[]`.  Two new error codes: `UNKNOWN_ROLE`
   (role_uid not registered) and `PUBKEY_MISMATCH` (uid registered but
   wrong pubkey).  Helper `verify_known_role_binding` called in both
   `handle_reg_req` and `handle_consumer_reg_req`.  HEP-CORE-0036
   §6.1 / §6.3 / §6.4 / §6.6 doc updated to match.  L3 pins:
   `ConsumerRegUnknownRole` + `ConsumerRegPubkeyMismatch`.  Sequence
   diagrams in §5 + handler-step tables in §8 retain stale
   `User-Id`-recovery wording — to be updated under AUTH-5 / #104.
3. **D4 — BRC notify dispatch** ✅ shipped 2026-06-10.  Implements
   HEP-CORE-0036 §6.5 producer-side handler flow normative spec
   exactly.  Closes **HB-3** (set_peer_allowlist now has live callers).
   - `NotificationId::ChannelAuthChanged = 8` + parse mapping in
     `role_host_core.hpp`.  BRC's generic `on_notification` callback
     in `role_api_base.cpp:1094` already enqueues every wire notify
     by string → IncomingMessage; no callback changes needed.
   - Simpler than the planned dispatch-table extension: handler runs
     BEFORE `dispatch_notifications` in each `CycleOps::invoke_and_commit`
     (3 sites — Producer / Consumer / Processor — defensive on
     non-producer side).  Strips ChannelAuthChanged out of msgs so
     the script dispatcher never sees them.  Audit finding G3 fix:
     no dispatch-table contract change required.
   - `RoleAPIBase::handle_channel_auth_notifies(msgs)` is the worker-
     thread handler: routes per-channel via `resolve_bc_for_channel`,
     calls `BrokerRequestComm::get_channel_auth(channel, role_uid,
     5000)`, dynamic_casts `tx_queue` to `PeerAdmission`, calls
     `set_peer_allowlist` on success.  All wrapped in try/catch per
     audit finding R5.  Logs WARN on every failure path (timeout,
     CHANNEL_NOT_FOUND, PRODUCER_NOT_AUTHORIZED, malformed reply,
     no tx queue, no BRC for channel) — no critical-error escalation
     per §I11.
   - New `BrokerRequestComm::get_channel_auth(channel, role_uid,
     timeout_ms=5000)` sync REQ/REP API.  Mirrors `register_channel`
     pattern.  L3 pins: `GetChannelAuthReturnsAllowlist` (allowlist
     reflects consumer joins) + `GetChannelAuthRejectsNonProducer`
     (defence-in-depth check).
   - Audit finding R1 (queue overflow) confirmed BENIGN: `enqueue_message`
     drops on full + logs WARN (`role_host_core.cpp:23`).  Operators can
     grep.  No additional counter needed.
4. **Consumer-side switch + script-side observability surface (engine
   parity)** ⏳ pending.  Two-part: (a) production code wiring +
   (b) script API exposure that obeys §I11 separation.  Direction
   confirmed 2026-06-10 (option B — polling + callback, both
   read-only).

   (a) **Wire consumer-side `pull_from` to the authed factory.**
   *Larger than initially scoped (discovered 2026-06-10); decomposed
   into Stages 1A / 1B / 1C after design discussion (2026-06-10).*
   The factory call site at `role_api_base.cpp:421-433` ALREADY uses
   `kRoleIdentityName` + `Z85PublicKey serverkey` from
   `producer_peers.front().pubkey_z85` and ALREADY hard-errors when
   `producer_peers` is empty.  Under HEP-CORE-0036 §3.5 the rx queue
   is INTENTIONALLY built in Standby at S1 before `register_consumer`
   runs (no PULL connect, no thread).  `apply_master_approval(CONSUMER_REG_ACK)`
   at S3 then connects per-producer + spawns the PULL worker.  This
   is the canonical S1/S3 ordering per §3.5.5.

   **Approach (chosen 2026-06-10): option β + state machine** — build
   rx queue with empty `producer_peers` initially, then apply
   `set_producer_peers(ACK.producers[])` after CONSUMER_REG_ACK
   arrives.  This generalizes to a queue-level state machine
   (Standby → Configured → Active) formalized in HEP-CORE-0036 §6.7
   that enforces §I12 "master approval before authority application."
   Option α (re-order setup) was rejected because (1) it doesn't
   compose with future dynamic-producer-join via §6.5.1 notify-then-
   pull (the queue needs the Standby state for hub-dead recovery
   anyway), and (2) it forces SHM and ZMQ down divergent code paths.

   Decomposed into:
     - **Stage 1A (#188)** — `ZmqQueue` Standby state machine:
       relax `pull_from()` to accept empty `Z85PublicKey`; add
       `set_producer_peers(list)` snapshot mutator; gate `start()`
       on Configured (both server_pubkey + peers populated);
       `is_running()` returns true iff Active.  Single-peer support
       initially (existing tests have 1:1 producer:consumer);
       multi-producer fan-in piggybacks on §6.5.1 pull path
       (separate scope).  HEP §6.7 normative spec.
     - **Stage 1B (#189)** — cycle_ops Standby guard: skip
       `write_acquire`/`read_acquire` when `!api_.is_*_active()`;
       no drop counter increment for Standby skip (preserves
       counter meaning); DEBUG log on Standby↔Active edge.  User
       callbacks (on_step / on_consume) don't fire when their
       queue isn't Active — that IS the script-visible "queue
       not ready" signal.
     - **Stage 1C (#190)** — `api.is_channel_ready(channel) →
       bool` script binding (Lua + Python + Native parity).
       Read-only state query for scripts that need to know channel
       state from callbacks OTHER than the channel's own data-loop
       callback (e.g., `on_init`, `on_band_message`).

   (b) **Broker wire-shape for `GET_CHANNEL_AUTH_ACK.allowlist`**
   per HEP §6.5 (locked 2026-06-12): array entries are **bare Z85
   pubkey strings** — symmetric with §6.2 `REG_ACK.initial_allowlist`
   and §7.2 producer-side cache `unordered_set<string>`.  The
   `role_uid` is operator-side metadata and is not carried on the
   wire.  ✅ shipped 2026-06-12 (Follow-up 6.1):
     - `BrokerServiceImpl::handle_get_channel_auth_req` enumerates
       the channel's authorized pubkeys and emits `array<string>`.
     - `RoleAPIBase::handle_channel_auth_notifies` reads each entry
       as `entry.get<std::string>()` and builds
       `PeerIdentity{"curve", pk}` (kind matches production
       convention; role_uid kept empty in the script-view because
       it's not on the wire).
     - L3 `GetChannelAuthReturnsAllowlist` worker asserts
       `al[0].is_string()` + Z85 pubkey value.

   (c) **Script API exposure — polling accessors (audit G1+G2).**
     - `api.allowed_peers(channel)` (producer-side) — returns a
       snapshot of the current `set_peer_allowlist` state as a list
       of `{role_uid, pubkey}`.  ✅ shipped 2026-06-10 in **Lua** +
       **Python** (via `ProducerAPI`, `ConsumerAPI`, `ProcessorAPI`
       engine-parity bindings).  Native deferred (MVP per #84).
     - `api.producers(channel)` (consumer-side) — ⏳ pending; gated
       on sub-task (a) for the producer-peer source data.
     - Both read-only.  Both return a COPY (script cannot retain a
       reference that the framework later mutates under it).

   (d) **Script API exposure — event callback (option B).**
     - New `ScriptEngine::invoke_on_allowlist_changed(channel,
       allowlist, reason)` virtual.  Default implementation: no-op.
     - Each engine implementation (`LuaEngine`, `PythonEngine`,
       `NativeEngine`) looks up the script-side function named
       `on_allowlist_changed` and invokes it with arguments shaped
       per HEP §6.5 producer-side flow step 5: `channel` (string),
       `allowlist` (host-natural list-of-record shape), `reason`
       (string).
     - `RoleAPIBase::handle_channel_auth_notifies` invokes the
       callback AFTER `set_peer_allowlist` returns success.
       Exceptions from the script callback are logged but do NOT
       roll back the cache (cache stays correct regardless).
     - Callback fires ONLY on the framework-applied transitions —
       no synthetic firing on connect/disconnect of the broker; no
       firing during process startup before the first pull
       completes.

   (e) **Engine-parity discipline.**  Audit pass after impl: verify
   the SAME callback name + signature + shape works identically in
   Lua, Python, and Native plugins.  Add a parity test
   (`tests/test_layer4_plh_role/...` or similar) that loads the
   SAME on_allowlist_changed callback in all three engines and
   asserts identical observed behavior.  Maintains the engine-parity
   invariant from #89 (N11 cross-engine signature audit).

   (f) **Safety guardrail (audit S3).**  ✅ shipped 2026-06-10 —
   `tools/check_auth_guardrail.sh` greps the script-binding surface
   (`src/scripting/`, `producer_api.*`, `consumer_api.*`,
   `processor_api.*`) for any `set_(allowed_peers|peer_allowlist|
   allowlist|authorized_peers)` symbol.  Wired into ctest as
   `AuthGuardrail_NoScriptAllowlistMutator` (layer0 / hep0036 /
   guardrail labels).  Currently green (0 hits).  ZmqQueue's
   framework-internal `set_peer_allowlist` is intentionally
   out-of-scope — it's NOT a script binding.

   (g) **Tests.**
     - L3 (broker side): broker emits new wire shape +
       reg/dereg propagates through the BRC pull path.  Verified
       2026-06-10 (object shape) → flipped 2026-06-12 to the
       locked Z85-string-array shape via Follow-up 6.1.  The
       `GetChannelAuthReturnsAllowlist` worker now asserts
       pre-reg empty, post-reg single-entry pubkey string,
       post-dereg empty.  ✅ shipped.
     - L3 script-driven callback firing test — needs a
       producer-role-host worker with a Lua script defining
       `on_allowlist_changed` and recording call sites.  The
       existing `IsolatedProcessTest` pattern in
       `broker_consumer_workers.cpp` uses BRC handles only — no
       script engine.  Build cost is higher than other L3 tests.
       **Disposition**: fold into AUTH-7 (L4 end-to-end gate
       close) where the demo framework hosts script-driven
       producer + consumer in real `plh_role` binaries.  Tracking
       under AUTH-7 scope below.
     - L2 unit test for `handle_channel_auth_notifies` with a
       fake-BRC double — possible but the existing L3 path
       already pins the broker side; adding L2 buys little.
       **Disposition**: skip unless a regression motivates it.
     - Engine parity (L4): same script logic running under Lua /
       Python observes the same callback args + same polling
       results.  Belongs in AUTH-7.
5. **B1 — `awaiting_endpoint` vocabulary fix** ✅ shipped 2026-06-10.
   Both port-0 sites (`broker_service.cpp:2063-2069` DISC_REQ and
   `:2273-2279` CONSUMER_REG_REQ) now include `(awaiting_endpoint)`
   in the WARN log + the error message, completing the §6.6 catalog
   vocabulary.  Client retry loops can now match the substring
   alongside `awaiting_first_heartbeat` and `heartbeat_stalled`.

6. **Producer-side S3 activation** ✅ shipped 2026-06-12.  *Discovered
   missing from the AUTH-1 plan during the 2026-06-12 design re-read*
   — the original 2026-06-10 plan (sub-deliverables 1-5) is entirely
   consumer-side because the pre-§3.5 design had the producer's
   PUSH socket bound at S1 inside `setup_infrastructure_`.  The
   §3.5.1 Option-α decision (locked 2026-06-12) moved producer PUSH
   bind to S3 inside `apply_master_approval`, leaving the
   producer-side counterpart of the consumer-side Stages 1A/1B/1C
   unplanned and unimplemented.  This entry closes that gap:
   - `hub_zmq_queue.cpp::set_producer_peers` no longer
     auto-promotes peer[0] into transport-artifact fields — under
     §6.7 Option B bare `set_*` mutators on Standby BUFFER args
     only; `apply_master_approval` is the single Standby → Active
     driver.
   - `hub_zmq_queue.cpp::apply_master_approval` now actually does
     work on the PUSH side (was a `return true` no-op): extracts
     `REG_ACK.initial_allowlist` (per HEP §6.2 — array of Z85
     pubkey strings), builds a `PeerAllowlist` with
     `PeerIdentity{"curve", pk}`, seeds the ZAP cache, then calls
     the queue's private `start()` to bind + arm + spawn worker.
     PULL side adopts the same shape: extracts `producers[]`,
     buffers via `set_producer_peers`, promotes peer[0] into
     `server_pubkey_z85_` + `endpoint`, then `start()`.
   - `role_api_base.cpp::build_tx_queue` no longer calls
     `writer->start()` inline — the tx queue is built in Standby
     symmetrically with the rx queue.
   - `role_api_base.{cpp,hpp}`: new `apply_producer_reg_ack(ack)`
     mirror of the existing `apply_consumer_reg_ack`.
   - `role_host_frame.cpp::setup_infrastructure_` no longer calls
     `start_rx_queue()` / `start_tx_queue()` — both calls deleted.
     Queues stay Standby through S1+S2.
   - `producer_role_host.cpp` + `processor_role_host.cpp` worker
     bodies: call `apply_producer_reg_ack(*reg_result)` after
     successful registration, before `install_heartbeat`.  Failure
     aborts startup with `promise_ref.set_value(false)`.
   - `tests/.../workers/zmq_queue_auth_workers.cpp` —
     `auth_standby_state_transitions` worker rewritten to assert
     the §6.7 Option B contract (bare `set_producer_peers` buffers
     without transitioning; `start()` refuses on Standby; only
     `apply_master_approval` drives Standby → Active).
   Full L2+L3 sweep green (1734/1734) post-commit.

**Open follow-ups under AUTH-1**, in execution priority order
(out of scope for the 72021c54 commit; each is its own
follow-up commit):

**Follow-up 6.1 — `GET_CHANNEL_AUTH_ACK.allowlist` wire shape
flip.**  ✅ shipped 2026-06-12.  Flipped both emit + read +
test-assertion sites to the locked Z85-string-array shape:
- `broker_service.cpp::handle_get_channel_auth_req` emits
  `array<string>` of authorized pubkeys.
- `role_api_base.cpp::handle_channel_auth_notifies` reads each
  entry as `entry.get<std::string>()`, builds
  `PeerIdentity{"curve", pk}` for the queue allowlist, and
  feeds the script-view `AllowedPeer` with empty `role_uid`
  (not on the wire) + `pubkey`.
- `tests/.../broker_consumer_workers.cpp::
  get_channel_auth_returns_allowlist` worker asserts
  `al[0].is_string()` and the Z85 pubkey value.
- Sub-deliverable 4(b) text updated above to cite the locked
  2026-06-12 §6.5 shape (replaces the 2026-06-10 amendment).
Full L2+L3 sweep green post-flip (1734/1734).

**Follow-up 6.2 — Fatal-on-failure for bare registration
branches.**  MEDIUM severity (completes §3.5.1 contract).
Currently the producer / consumer / processor role hosts log
the failure and fall through (in-file comments document the
deferred intent).  Add `promise_ref.set_value(false); return`
to the failure branch in each:

- `producer_role_host.cpp:367-386` (REG_REQ failure)
- `consumer_role_host.cpp:328-360` (CONSUMER_REG_REQ failure)
- `processor_role_host.cpp:405-446` (BOTH prod_result + cons_result
  failure paths)

Three small edits.  Build + full L2/L3 + L4 sweep required to
catch any test that relied on the half-state.

**Follow-up 6.3 — Migrate 10 test sites off the legacy
direct-activation path.**  MEDIUM severity (false confidence —
tests pass but exercise the wrong code path).
`tests/test_layer3_datahub/workers/role_api_flexzone_workers.cpp`
lines 206, 233, 363, 434, 788, 825, 930, 972, 1056, 1098 use
`start_rx_queue()` / `start_tx_queue()` directly.  Each call
site needs a stub `REG_ACK` / `CONSUMER_REG_ACK` constructed
and invoked via `apply_consumer_reg_ack(stub)` /
`apply_producer_reg_ack(stub)`.  Grep across `tests/`
confirmed no other test file is affected.

**Follow-up 6.4 — Delete `RoleAPIBase::start_rx_queue()` /
`start_tx_queue()` from the public API.**  LOW severity (cleanup
once 6.3 is done).  No production caller remains; the public
surface still invites legacy patterns.  Make private or delete.
Depends on 6.3.

**Blocks:** 6.1 ✅ shipped 2026-06-12; AUTH-2 unblocked on the
wire-shape side.  AUTH-3 still waits on 6.2 (fatal-on-failure
needs to land before RegistrationState::Authorized is wired).
6.3 + 6.4 can land in parallel with AUTH-2/3.

### AUTH-2 — Producer-side ZAP pump on BRC poll thread

> **Tracker:** task **#162**.
> **Old labels:** HB-2.
> **HEP anchors:** §7.1 (producer-side ZAP cache responsibility) +
> §I11 (framework provides atomic update mechanism).

**Goal.**  Make the producer's data ROUTER's ZAP handler responsive
so CURVE handshakes can complete.  The producer's ZAP cache (the
allowlist set by AUTH-1's pull) is meaningless if the handshake
never gets to consult it.

**§3.5 alignment (2026-06-12).**  ZAP arm + `ZapRouter::pump_one`
wiring occurs inside `apply_master_approval(REG_ACK)` (S3), not in
`setup_infrastructure_` (S1).  Symmetric with consumer-side
`apply_master_approval(CONSUMER_REG_ACK)`.

**Scope (do not expand).**
- Wire `ZapRouter::pump_one(0ms)` into the BRC poll thread's existing
  poll loop, alongside the broker-side dispatch (`broker_service.cpp:811`
  is the pattern reference).
- Multi-peer backlog drain: convert to `while (pump_one(0ms)) {}` —
  one extra line, fold into this commit, no separate task.

**Out of scope (per §I11; do not re-litigate).**
- Coordinating ZAP responses with allowlist updates: not needed —
  `set_peer_allowlist` already atomic per §6.5 producer-side flow.
- Backpressure on the BRC poll thread: not needed — pumping is
  non-blocking, pure local work.
- Dedicated ZAP thread: not needed — HEP-0036 §7.1 explicitly says
  pump from an existing poll thread.

**Depends on:** AUTH-1 (without producer pubkey emission in D5, no
consumer attempts CURVE handshake against the producer ROUTER, so
the pump has nothing to service).

### AUTH-3 — Authorized state + data-loop outer guard

> **Tracker:** task **#163**.
> **Old labels:** HB-4 + HB-5.
> **HEP anchors:** §4.3.2 (Authorized entry conditions, synchronous
> trigger) + §8.2 (data-loop outer guard) + §I3 (control gates
> data) + §I11 (framework guarantees outer gate; script decides
> per-cycle send policy).

**Goal.**  Add the FSM state + outer-loop guard so the data loop
does not run before the role's auth flow is in place.

**Scope (do not expand).**
- `RegistrationState::Authorized` added to `role_presence.hpp:95-101`
  + transitions per §4.3.2 (producer: after PUSH bind + REG_ACK ok;
  consumer: after CONSUMER_REG_ACK with `producers[]` + rx queue
  built).  Synchronous, no async re-entry.
- `any_presence_authorized()` predicate.
- Data-loop outer guard at `data_loop.hpp:129-131` extended:
  ```cpp
  while (core.is_running() && !core.is_shutdown_requested()
         && !core.is_critical_error() && any_presence_authorized())
  ```

**Out of scope (per §I11; do not re-litigate).**
- Per-cycle "do we have N peers" check inside the loop — script
  decision, gates emission, not the loop.
- Allowlist non-emptiness as guard condition — Authorized is FSM
  state, not "have peers"; per §I11 emission policy is script-level.
- Conditional re-entry semantics on transient failures — covered by
  existing hub-dead path (§4.3.3); do not add new escalation tiers.

**Depends on:** AUTH-1 (the Authorized transition for consumer-side
fires synchronously upon CONSUMER_REG_ACK arrival with `producers[]`,
which D5 emits).  Producer-side Authorized fires at the END of
`apply_master_approval(REG_ACK)` (S3 — PUSH bound + ZAP armed +
allowlist seeded + worker spawned) per HEP-CORE-0036 §3.5.4 INV1.
Broker fires `CHANNEL_PRODUCERS_CHANGED_NOTIFY {reason=producer_joined}`
to consumers on the producer's Pending → Ready transition (first HB),
not on REG_REQ accept (§3.5.4 INV2).

### AUTH-4 — SHM broker-issued secret end-to-end

> **Tracker:** tasks **#164** + **#79** (CLI side).
> **Old labels:** HB-6 + B4.
> **HEP anchors:** §5.6 (broker generates per-channel random uint64)
> + §I6 (SHM secret is broker-minted, unlike CURVE keys which are
> role-owned) + §I11 (framework provides token; script doesn't
> participate in secret negotiation).

**Goal.**  Apply the same broker-as-source-of-truth + atomic-update
pattern to SHM transport that AUTH-1 applies to ZMQ.  Broker mints
per-channel `shm_secret`; CONSUMER_REG_ACK delivers it; SHM consumer
enforces it on attach.

**Scope (do not expand).**
1. Broker generates per-channel random `shm_secret` (replaces
   hardcoded zero at `broker_service.cpp:1894`).
2. `CONSUMER_REG_ACK.shm_secret` field emitted for SHM transport
   (sibling to ZMQ's `producers[]` per §6.4).
3. SHM consumer applies as DataBlock guard secret per HEP-CORE-0002.
4. **#79** — `plh_role --init` provisions a non-zero seed in role
   config (avoids zero-default that pre-dates this work).

**Out of scope (per §I11).**
- Secret rotation while channel live: out of scope; secret lifetime
  is channel lifetime, restart-on-rotate is acceptable for MVP.
- Cross-channel secret reuse: forbidden — every channel mints fresh.
- Script-visible secret API: scripts must NEVER see the secret;
  framework consumes it inside the SHM consumer setup.

**Independent of AUTH-1..3** (SHM transport is orthogonal to ZMQ
CURVE).  Can interleave or land after.  Order vs AUTH-1: if AUTH-1
D5 schema lands first, ACK emits `shm_secret=0` until AUTH-4 lands
the generator — acceptable as long as SHM consumers don't enforce
until AUTH-4 (3) ships (single landing window per Phase D close-out).

### AUTH-5 — Sibling HEP doc sync

> **Tracker:** task **#104**.
> **HEP anchors:** §14 sibling-HEP update list + §I11 (the principle
> the sibling HEPs adopt by cross-reference, not by re-derivation).

**Goal.**  Make every sibling HEP consistent with HEP-0036's
shipped contract.  Pure doc work for 7 HEPs; ~10 LOC code for
HEP-0023's `Authorized` enum already folded into AUTH-3.

**Scope (do not expand).**
- HEP-0017 §3.3 — document the shipped `ProducerPeer` +
  `add_producer_peer` / `remove_producer_peer` API.
- HEP-0021 §16 — pubkey REQUIRED text.
- HEP-0027 / HEP-0030 / HEP-0007 — record the wire-version transition.
- HEP-0033 §G — cross-references for `ChannelAccessEntry`.
- HEP-0036 §5 sequence diagrams + §8 handler-step tables — replace
  stale `zmq_msg_gets("User-Id")` wording with §6.1 / §6.3
  verification-model wording (deferred from AUTH-1's HEP touch).

**Out of scope (per §I11).**
- Re-litigating the verification model in sibling HEPs — each
  sibling cross-refs HEP-0036; no parallel restatement.
- Sibling HEPs proposing their own coordination layers — coordination
  lives in script per §I11; sibling HEPs document the channels
  (band / inbox / queue) the framework provides, not policies.

**Depends on:** AUTH-1, AUTH-2, AUTH-3 shipped (so the docs match
code).

### AUTH-6 — L3 broker test revival

> **Tracker:** task **#154** (in-progress).
> **Old labels:** D6.
> **HEP anchors:** §6.5 producer-side handler flow + §I11
> (observable allowlist as test fixture surface).

**Goal.**  Unmask the 7 worker files masked under task #153
(lib-stabilization exclusion procedure; see 2026-06-05 archive).
Per-file commit with mutation-sweep on each restored TEST_F.

**Scope (do not expand).**
- Pins per §I11: broker pushes allowlist on consumer reg / dereg;
  producer applies via the normative handler flow; consumer with
  wrong pubkey rejected; revocation propagates within the contract
  bound.
- Test surface = observable allowlist state (e.g. `api.allowed_peers`
  or `tx_queue.peer_allowlist_snapshot()`).  Assertions pin the
  observable, not the implementation thread / dispatcher internals.

**Out of scope (per §I11).**
- Tests that pin priority dispatch, queue-blocking semantics, or
  critical-error escalation — none of these are in the contract;
  asserting them locks future implementations into an arbitrary
  shape.
- Tests for script-level coordination policies — those are application
  concerns; if a sample app uses one, it gets an L4 demo test, not
  an L3 framework pin.

**Depends on:** AUTH-1 shipped.

### AUTH-7 — L4 end-to-end gate close

> **Old labels:** D7.
> **HEP anchors:** §I11 (script-level coordination demonstrated on
> top of framework's membership contract) + §5.2 sequence (end-to-
> end auth flow).

**Goal.**  Full dual-hub auth-gated data flow under the demo
framework.  Final proof that the data-plane CURVE gate is closed
end-to-end and that scripts can build coordination on top.

**Scope (do not expand).**
- Demo manifest with producer + consumer roles, real `plh_role`
  binaries, real CURVE handshakes, end-to-end data flow.
- Assertion: data flows iff consumer is in producer's allowlist;
  data stops iff consumer dereg.
- Optional second demo: a script-side "wait until N peers ready"
  pattern using `api.allowed_peers` — illustrative of §I11.

**Out of scope (per §I11).**
- Framework changes — by AUTH-7, all framework work has shipped;
  AUTH-7 is observation, not implementation.

**Depends on:** AUTH-1 + AUTH-2 + AUTH-3 + AUTH-6 shipped.  AUTH-4
landing first is nice-to-have (SHM gate closed) but not strictly
required (ZMQ-only L4 test is meaningful).

---

## Design audit 2026-06-10 — issues found in §I11 / §6.5 contract

Honest investigation against the contract just landed.  Each entry
classifies severity and disposition; nothing here is hand-waving.

### Gaps in the framework surface (must address before AUTH-7 closes)

- **G1 — `api.allowed_peers(channel)` doesn't exist.**  Referenced in
  HEP §I11 examples + AUTH-6/AUTH-7.  Scripts cannot poll the
  allowlist today.  **Disposition**: add as explicit sub-task to
  **AUTH-1.consumer-switch** (it's the same C5-style "expose
  framework state to scripts" pattern as #186).  Producer-side and
  consumer-side accessors both needed.
- **G2 — `api.producers(channel)` doesn't exist on consumer side.**
  Same situation as G1; symmetric API for §6.4 `producers[]` array.
  **Disposition**: bundle with G1 under the same AUTH-1 sub-task.
- **G3 — Internal-only `NotificationId` pattern unestablished.**
  AUTH-1 D4 will be the first user.  Risk: future code
  copies-without-understanding and creates non-internal events that
  silently consume infrastructure events.  **Disposition**: document
  the "callback_name == nullptr ⇒ internal" rule directly in
  `cycle_ops.hpp` next to the dispatch loop change.  Trivial.

### Race conditions / failure modes worth pinning

- **R1 — Queue overflow.**  `kMaxIncomingQueue=64`.  Verify
  `enqueue_message` behavior on full: block, drop, throw?  If silent
  drop, an auth NOTIFY can be lost and the cache never converges.
  **Severity**: HIGH if drop; medium if block (BRC poll thread
  blocking is itself a deadlock risk, see analysis above).
  **Disposition**: investigate as part of AUTH-1 D4 implementation;
  if drop, add a queue-full counter that scripts/ops can observe and
  document the recovery via BRC reconnect → REG_ACK.initial_allowlist.
- **R2 — Worker stuck in long script → queue fills → notify dropped.**
  Direct consequence of R1.  Operational mitigation: bounded script
  callback time.  **Disposition**: add operator-facing doc note in
  README_Scripting_*.md (under AUTH-5 sibling doc sweep).
- **R3 — GET pull timeout.**  `BrokerRequestComm::get_channel_auth`
  needs a timeout parameter matching the other sync APIs (5000ms
  default like `register_channel`).  **Disposition**: enforce in
  AUTH-1 D4 implementation.
- **R4 — GET returns `CHANNEL_NOT_FOUND` or `PRODUCER_NOT_AUTHORIZED`.**
  Race: notify fired, then producer deregistered or channel deleted
  before pull arrives.  Handler MUST treat as "no allowlist to apply,
  log + continue" — not as a fatal error.  **Disposition**: explicit
  handling in AUTH-1 D4 native handler; L3 test pinning the
  graceful-skip behavior.
- **R5 — `set_peer_allowlist` exception safety.**  If the apply
  throws, worker thread dies.  **Disposition**: verify exception
  spec in implementation; wrap in try/catch in the dispatcher's
  native-handler call site so a malformed allowlist doesn't crash
  the role.

### Acknowledged trade-offs (not bugs; document for future readers)

- **A1 — Revocation race window (HEP §I5).**  Between broker
  revocation and producer cache update, a revoked consumer can
  complete a NEW CURVE handshake.  HEP §I5 acknowledges this.
  **Disposition**: documented; no action.
- **A2 — Stale-cache during long script callback.**  Cache update
  delayed by current script's wall-clock.  Scripts that block for
  seconds defer auth convergence for seconds.  **Disposition**:
  documented in §I11 + above; operator's responsibility to bound
  script callback time.
- **A3 — Allowlist size scalability.**  `set_peer_allowlist` holds a
  mutex during update; concurrent CURVE handshakes wait.  Fine for
  tens to low-hundreds of consumers per channel.  Beyond that, the
  contract still holds but latency grows.  **Disposition**: documented
  as a non-MVP concern; revisit if a channel ever needs >1000
  consumers.

### Federation (HEP-0037, post-MVP)

- **F1 — Cross-hub `CHANNEL_AUTH_CHANGED_NOTIFY` propagation.**  When
  producer is on hub A and consumer registers on hub B (federated),
  the notify path must traverse the inter-hub link.  HEP-0036 §13.1
  defers this.  **Disposition**: AUTH-N work does NOT handle
  federation; HEP-0037 will absorb.  Confirmed not blocking.
- **F2 — `allowed_peers` semantics in federation.**  Per-hub view or
  union view?  **Disposition**: HEP-0037's call when it lands.

### Test gaps

- **T1 — L4 demos for §I11 examples.**  Examples A/B/C/D should each
  exist as runnable demos under `share/py-demo-auth-*/` (or similar).
  **Disposition**: add to AUTH-7 scope.
- **T2 — Queue-overflow stress test.**  Drive the BRC notify rate
  above worker drain rate; confirm the documented behavior (drop or
  block) actually happens + the observability fires.  **Disposition**:
  add to AUTH-6 scope.
- **T3 — Pull failure paths (CHANNEL_NOT_FOUND / PRODUCER_NOT_AUTHORIZED
  / timeout).**  Each path needs an L3 worker.  **Disposition**: AUTH-6.
- **T4 — libzmq CURVE auto-retry timing.**  §I3 relies on libzmq
  retrying handshakes until producer's cache converges.  Pin the
  retry interval is bounded (no infinite-backoff hazard).
  **Disposition**: AUTH-6 (single L3 test with a sync barrier).

### Honest assessment

The contract is sound for the threat model HEP-0036 specifies
(operator-trusted roles + manual key distribution + accepted
revocation-race window).  The audit found 5 medium/high concrete
issues (G1, G2, R1, R3, R4) that MUST land before AUTH-7 close —
each has an explicit AUTH-N entry pointing to it now.  No design
gaps that re-open the §I11 / §6.5 discussion.

If a future request looks like it wants to revisit "should worker
prioritise auth notifies" / "should pull failure be critical" /
"should the queue block" — it's hitting issues this audit already
considered.  Re-read this section and §I11 + §6.5 first.

---

## Backlog — open items NOT on the AUTH-1..7 critical path

These are tracked here so they survive context resets per CLAUDE.md
§"Session hygiene" — open items must live in a subtopic TODO, not only
in chat history.

### Test infrastructure / coverage gaps

- **Allow-path L3 pin for D2.**
  `DatahubBrokerHealthTest.CtrlZapDenyPath` pins the deny path.
  Symmetric allow-path L3 needs a BRC client whose `client_pubkey` is
  added to the broker's `known_roles[]` before connect; that requires
  the test infrastructure to thread explicit CURVE keys into the
  worker's broker config (the existing L3 worker pattern uses
  ephemeral BRC keys, which the worker process cannot know ahead of
  time to pre-register).  Smallest fix: extend the
  `BrokerService::Config` test-side construction to include a
  pre-generated `known_roles[]` entry built from the test client's
  keypair.  Effort: S.

### Operator workflow gaps

- **`plh_role --keygen` does not publish `<vault>.pub`.**
  HEP-CORE-0035 §4.8.3 specifies `plh_hub --add-known-role <role.pub>`
  as the canonical operator workflow; that requires the role binary
  to publish a sibling `.pub` file alongside the vault (the way
  `plh_hub --keygen` publishes `hub.pubkey`).  Currently the L4
  RoundTrip test opens the role vault programmatically to extract the
  pubkey — a test backdoor.  Mirror `HubVault::publish_public_key`
  for `RoleVault` (atomic O_EXCL + O_NOFOLLOW + mode 0644 + symlink
  defense per HEP-CORE-0035 §4.6.4).  Effort: M.

- **Hot-reload of `known_roles.json` on a running hub** (HEP-CORE-0035
  §4.8.5).  `BrokerCtrlAdmission::set_peer_allowlist` exists with no
  caller; the admin RPC (`/admin/reload-known-roles` or similar) is
  the missing wiring.  Operators that run `--add-known-role` against
  a running hub today must restart it to pick up the change.
  Effort: M.

### Phase E/F/G/H staging

- **Phase E — Admin loopback enforcement.**  AdminService refuses
  non-loopback bind for v1; CURVE-wrap is HEP-CORE-0035 §5 future
  work.  Unblocked once AUTH-1..7 ship.
- **Phase F — Federation peer ZAP parity.**  Depends on Phase E +
  Federation HEP (task #105).
- **Phase G — SHM auth migration.**  AUTH-4 is the foundation;
  Phase G is the broader migration (existing SHM consumers gain
  `shm_secret` enforcement).
- **Phase H — Demo migration.**  24 role configs across 11 demo dirs
  ship `"auth": { "keyfile": "" }` (pre-dates Phase C, broken since
  early May 2026; see 2026-06-09 archive for full disposition).
  Belongs alongside #79 (AUTH-4 step 4) and #155 (CLI --init
  bundling) as a coordinated refresh wave, not as a Phase C close-out.

---

## Deferred decisions (each tied to its phase)

| # | Decision | Affects | Tentative direction |
|---|---|---|---|
| P-InboxQueue | InboxQueue admission policy location | Phase E | **REVISED 2026-06-10:** InboxQueue inherits the parent data channel's allowlist + reuses the role-wide ZAP handler per HEP-0036 §9.3.  No separate per-inbox `PeerAdmission`.  Inbox state follows the parent ZmqQueue's Standby/Configured/Active state machine (HEP §6.7).  Implementation deferred — tracked as task **#191**, picked up after AUTH-7.  HEP-0036 §12 Phase 8's "verification only" wording is inaccurate; CURVE wiring is genuinely missing in `InboxQueue::start()` today (audited 2026-06-10). |
| P-Admin | AdminService — CURVE-wrap or loopback-only? | Phase E (task #127) | Hard loopback-only enforce (refuse non-loopback bind) for v1; CURVE-wrap is HEP-CORE-0035 §5 future work |
| P-SHM-Identity | What is a PeerIdentity for SHM? | Phase G + AUTH-4 (task #129) | Broker-issued `shm_secret` primary; optional uid guard if operator sets it; broker controls the gate via secret issuance |
| P-Demos | How existing demos migrate | Phase H (task #130) | Transitional `--allow-anonymous-data` flag, gated to refuse-bind on non-loopback endpoints; demos updated incrementally |
| P-HEP | When to sync HEPs vs hold tech_draft | Close-out (task #131) | Tech_draft was archived 2026-06-02; HEPs are now the authoritative source.  No further sync needed unless H surfaces gaps |

---

## Parallel / adjacent tracks (don't sequence into the AUTH-1..7 chain by mistake)

These have their own task IDs but touch the same surface:

- **#74** — HEP-CORE-0035 auth implementation tracker (umbrella for
  Phase D close + AUTH-1..7; treat as the strategic tracker).
- **#102** — HEP-CORE-0035 §4.7 runtime key handling — **SUPERSEDED
  2026-06-05** by the HEP-CORE-0040 chain (#165–#176 all shipped);
  closes when stragglers reach steady state.
- **#103** — HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation;
  carries AUTH-1.
- **#104** — Sibling HEP updates per HEP-CORE-0036 §14; carries
  AUTH-5.
- **#105** — Federation protocol design + cross-hub reg/comm
  verification; explicitly post-MVP per HEP-CORE-0036 §13.1.
- **#106** — HEP-CORE-0038 + impl: script-accessible vault keystore
  (`api.vault_save/load`); blocked on HEP-0040 storage layer (now
  shipped).
- **#120** — Windows pathway hardening for HEP-CORE-0035 §4.6 floor.
- **#152** — Delete legacy `RoleIdentityPolicy` (hygiene; independent).
- **#154** — Re-create L3 broker tests against refactored lib code;
  carries AUTH-6.
- **#162** — HB-2 wiring; carries AUTH-2.
- **#163** — HB-4+5 wiring; carries AUTH-3.
- **#164** — HB-6 broker-side shm_secret; carries AUTH-4 steps 1-3.
- **#79** — `plh_role --init` SHM secret; carries AUTH-4 step 4.

### Items NOT on this critical path

- **#75** HUB_TARGETED_ACK — scope ambiguous; no HEP section, no
  tech_draft.  Needs design-first work.
- **#76** Script reload — independent feature; tech_draft exists,
  HEP not yet numbered.
- **#77** Tier 2 dynamic callbacks — independent feature.
- **#94** HEP-0021 §16.5 ephemeral binding — paired with AUTH-1 per
  HEP-0036 §14.1 wire-shape coupling but the production-caller
  wiring is about multi-hub processor, not auth gating.  Can land in
  the AUTH-1 commit as a §14.1 deliverable but doesn't itself gate
  the goal.
- **#155** Phase 3 (`--init` one-shot bundling) — CLI UX, auth-
  adjacent but not auth-gating.  Phases 1+2 shipped per commits
  `3215e5aa` and `c684776a`.

---

## Decision log — key calls captured for audit trail

- **2026-06-05 — Strict-mode cleanup must precede A3** (now AUTH-1).
  The 2026-06-05 audit (see 2026-06-09 archive §"How the silent-fallback
  hole opened") showed A2 (commit `badfaed1`) shipped zero CURVE
  coverage due to a `validate_auth_options` fallback + a `set_auth`
  ordering bug.  Strict-mode cleanup chain (C1..C5) sequenced before
  AUTH-1 to surface dependents as compile/link/run errors instead of
  silent miscompiles.  Chain closed 2026-06-09.
- **2026-06-05 PM — Separate STORAGE from API.**  The "keypair as
  ctor arg" shape creates a second copy of the seckey in process
  memory.  Decision: storage is one owner per process (HEP-CORE-0040
  KeyStore + LockedKey); API design exposes OPERATIONS on secret
  material (`with_seckey(name, cb)`), not byte exports.  Memory rule
  added: separate STORAGE design from API design when asked "where
  should X live".  See 2026-06-09 archive §"2026-06-05 PM REFRAME".
- **2026-06-06 — Use-not-export discipline (round-5).**
  RoleAPIBase + HubAPI lose the seckey accessor entirely; no
  legitimate caller exists (tracing every reader of seckey shows
  they're all libzmq socket-option setters that call `with_seckey`
  directly).  See HEP-CORE-0040 §8.2 and 2026-06-09 archive
  §"Strict-CURVE cleanup chain — C1..C5".
- **2026-06-09 — Queue-level gate, NOT transport-level.**  The broker
  auth gate sits at QUEUE level (before data channel construction),
  not at SHM/ZMQ transport level (CURVE handshake / shm_secret
  memcmp).  HEP-CORE-0036 lines 1546, 1797, 1906, 1911 explicitly
  state this.  Captured here because it surfaced multiple times
  during the audit.
- **2026-06-09 — Mechanism enum collapsed from 3 to 2 states.**
  `Plaintext` was structurally unreachable after C1 (validator rejects
  empty keys).  Collapsed to `Uninitialized` / `Curve`.  See 2026-06-09
  archive §"Phase C fresh-eye review".
- **2026-06-09 — Demo refresh deferred.**  Phase C did NOT introduce
  new demo breakage; the `auth.keyfile = ""` breakage pre-dates
  Phase C (#78 / B3 in early May 2026).  Demo refresh belongs in
  Phase H alongside #79 + #155, not as a Phase C close-out.  See
  2026-06-09 archive §"Strict-CURVE cleanup chain — C1..C5".

---

## Memory rules adopted during the auth track

These survive in the user-level MEMORY.md; included here for the
audit trail.

- **Audit stale silent-fallback patterns whenever a contract changes.**
  `if (X.empty()) return /* skip security */` is a contract-violation
  candidate, not a clean default.  Source: 2026-06-05 audit found A2
  shipped ZERO CURVE because the validator endorsed all-empty as
  legacy plaintext.
- **Separate STORAGE design from API design.**  When asked "where
  should X live", split into (a) where the bytes live (lowest
  reasonable level, single owner) and (b) what API exposes access
  (grouped logically per consumer).  Source: 2026-06-05 PM REFRAME on
  HEP-CORE-0040.
- **Refresh against persistent docs at the moment work begins.**  Do
  not re-derive plans from HEPs when AUTH_TODO already has the agreed
  plan.  Source: 2026-06-09 user correction — "your memory is burnt
  about what we should do?" — caught me re-deriving HB-1..6 from
  HEPs instead of reading the AUTH_TODO entries that already
  captured the plan.
