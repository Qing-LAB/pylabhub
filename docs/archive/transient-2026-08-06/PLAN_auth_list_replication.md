# Execution plan — auth-list replication (HEP-CORE-0035 §4.9)

**Design authority is `HEP-CORE-0035 §4.9`.** This document does not restate
it and must not contradict it. If the two disagree, the HEP wins and this
file is wrong.

What lives here: the state of the working tree, what is wrong with it, and
the order to fix it in. Delete this file when the work lands.

---

## 1. The goal, in one paragraph

A role decides who may reach its inbox, and it decides alone — no round trip
to the hub while a connection is being made. So it holds a copy of the hub's
key list. Today that copy is written once, at registration, and never
corrected. The goal is that a role's copy tracks the hub's list, that it can
name a sender from the key that sender proved rather than the string the
sender typed, and that both facts come from **one** object rather than
several representations that can disagree.

Nothing about this is a live bug: the hub's list is built once at startup and
runtime reload is deferred, so nothing changes underneath a role today. This
is the missing half of two capabilities that are deferred *because* it is
missing — reload, and sender attribution.

---

## 2. What was wrong, and what fixed it

**Resolved by step 4 and the 2026-08-04 review** — see §2.3, where one item
outlived step 4 despite being listed as fixed. Kept because the failure mode is
the one most likely to recur: a wire was migrated while the surrounding shape
was left alone, so a new type became decoration in front of the old structure.

The defects below no longer reproduce. `adopt_inbox_roster` now parses once
into one published `PeerAuthority` per side, `inbox_known_roles` is gone, and
the ZAP view is derived from the published objects rather than accumulated
beside them. An entry missing either half now throws during the build instead
of being skipped by one pass and admitted by another — there is no second path
into the published object for it to take.

### 2.1 (WAS) The role side was a patch on the old shape, not the design

`RoleAPIBase::adopt_inbox_roster()` built a `PeerAuthority::Builder`,
populated it, handled its exceptions — **and then discarded it**. The state it
actually kept was `inbox_known_roles`, the pre-existing `unordered_set` of
bare key strings.

So the role still held a bag of keys with no names, and the version,
`admits()`, and the `RosterEntry` overload added to `PeerAuthority` for this
purpose were dead on the role side. None of I-ROSTER-ONE-HOLDER, ask-don't-copy,
or attribution-from-proof was in place.

This is the failure worth naming: the wire was migrated and the *shape* was
left alone, so the new type became decoration in front of the old structure.

### 2.2 (WAS) Two loops over one list, disagreeing about what is acceptable

The validating loop skipped an entry with no uid:

```cpp
if (entry.uid.empty() || entry.pubkey_z85.empty()) continue;
```

The mutating loop inserted it anyway, because it only looks at the key:

```cpp
auto pk = e.value("pubkey", std::string{});
if (!pk.empty()) inbox_known_roles.insert(std::move(pk));
```

`{uid: "", pubkey: "<valid>"}` was therefore **refused by validation and
admitted by the gate** — a nameless key in the allowlist. The comment above
claims "publish only on success," which the mutating loop does not honour: it
never throws, and silently accepts what the validator rejected.

This is a direct consequence of 2.1. One list in two representations needs
two passes, and two passes drift.

### 2.3 (WAS) Smaller, same root

- Adopting a reply whose version was `0` set the held version to `0` and
  disarmed the monotonic guard for everything after it.  **This one survived
  step 4 and was still live until the 2026-08-04 review** — §2's blanket
  "resolved by step 4" was wrong about it.  The guard read
  `incoming != 0 && incoming < held`, so a versionless reply skipped the
  comparison and then overwrote the stored version anyway.  Fixed by dropping
  the exemption: the bootstrap case never needed it, because a side holding no
  roster reports `0` and `0 < 0` is false.
- The stale-reply early return logged nothing, so a rejected update was invisible.
- The role stored the version a second time, in `inbox_roster_version_[]`,
  beside the `PeerAuthority` that already carries it — the exact pairing
  I-ROSTER-VERSION-IN-SNAPSHOT exists to forbid, reintroduced one layer up.
  Removed in the same review; the version is now read from the published
  object.
- The roster emission was copy-pasted at both ACK sites in `broker_service.cpp`.
  Fixed by step 5: it is now `roster_ack_block()`, beside `heartbeat_ack_block()`,
  which already existed for exactly this reason.

**The lesson worth keeping:** step 4 rewrote this function and the defect list
was marked resolved wholesale, without re-reading the guard it had just moved.
A claim that a defect is fixed is worth exactly as much as the re-read behind
it.

---

## 3. The pattern to build to

**One published object per side; every consumer asks it; nothing keeps a
second copy.**

`PeerAuthority` already is that object. It is immutable, indexed by key,
published by pointer swap, and answers all three questions a role has — may
this key connect, whose key is this, and what does the ZAP layer need. It
depends on nothing above the security layer, so a role holds the same type
the hub does.

The shape to converge on:

```
   one parse  ──►  one PeerAuthority per side  ──►  published by swap
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
         admits(key)   attribute(key)   zap_allowlist()
```

Everything currently duplicated collapses into that: the string set goes away
because the authority holds the keys; the second loop goes away because there
is one parse; the ordering problem goes away because the socket check asks
instead of holding.

**The test of any change here:** if a second consumer of the list would need
its own copy, or a second list would need its own protocol, the change is
wrong.

---

## 4. Execution order

Each step must build; the wire step must land as one commit across both ends.

| # | Step | Done? |
|---|------|-------|
| 1 | `PeerAuthority`: version inside the object, `admits()`, `Builder::add_local_role(RosterEntry)` | ✅ |
| 2 | Broker: hub-scoped `roster_ledger_`, snapshot stamped with its version | ✅ |
| 3 | Wire: `{uid, pubkey}` pairs + `known_roles_version` on both ACKs; role replaces rather than merges | ✅ |
| 4 | Role holds a published `PeerAuthority` **per side**; `inbox_known_roles` deleted; ZAP view derived from the authorities; one parse; `roster_admits()` ORs across sides | ✅ |
| 5 | Collapse the duplicated roster emission into a `roster_ack_block()` beside `heartbeat_ack_block()` | ✅ |
| 6 | `InboxQueue::is_peer_allowed` delegates to the role's check instead of consulting a stored allowlist | ✅ |
| 7 | Review fixes inside `adopt_inbox_roster`: lock scope, version read inside the try, the hand-off comment that points at a check nobody wrote | ✅ |
| 8 | **Reachable inbox — I-ROSTER-PRESENT + I-INBOX-REACHABLE, ONE commit.** Layers L1–L5 below | 🔄 L1–L4 done; full sweeps green in BOTH configs (Debug 2774/2774, Release 2771/2771); L5 has 6 of 10 cases |
| 9 | Inbox attribution: sender from `AttestedKey::from_message` + `attribute_sender`; replay guard and per-sender sequence keyed off the proven uid, not the routing frame | ❌ |
| 10 | Tests, then a full unfiltered sweep in **both** configs before commit | ❌ |

### Step 8 as five layers

Organised by idea, not by file.  Each layer is a complete statement about
the system; the code in it follows from that statement.  A layer that is
half-done leaves the system describable, just less capable — none of them
is a patch on another.

**L1 — one ledger answers both questions, and only from wire evidence.**

- ✅ keyed on role uids, starts empty, moved by `subscribe_role_registered` /
  `subscribe_role_disconnected` under `roster_mu_`
- ✅ `roster_ack_block()` = vault entries filtered by the admitted set
- ❌ hub calls `confirm(role_uid, version)` when a `ROSTER_CHECK_NOTIFY`
  arrives.  This is the ONLY way the confirmation map may move.
- ❌ the primitive's doc names its first parameter "the admitted peer's
  identity (z85 pubkey in production)"; this instance holds uids in both
  slots.  Correct the doc or introduce a named alias — a doc that lies is
  a defect whether or not the code is right.

> **Do not let the hub self-confirm at REG_ACK.**  It knows it just handed
> the role version N, so recording the confirmation itself looks free and
> saves a message.  It is exactly the over-confirmation class the ledger
> exists to prevent (test #2480), and `confirm()`'s contract forbids
> inferring confirmation from anything but wire evidence.  Noted here
> because it will look like an optimisation to someone later.

**L2 — every adoption is acknowledged.**

One helper, three callers — not three code paths:

- ✅ on the periodic tick, once per side
- ❌ immediately on adopting a pushed `ROSTER_UPDATE_NOTIFY`
- ❌ immediately on adopting the REG_ACK roster

The third is not optional.  Without it the hub cannot know a freshly
registered role has a roster at all, so the FIRST sender to any role is
told "not reachable" and a push is sent — even when the role's REG_ACK
roster already named that sender.

**L3 — reachability is decided where the address is disclosed.**

In the `ROLE_INFO_REQ` handler, and only after the target is established
to have an inbox (otherwise the hub pushes rosters on behalf of senders
trying to reach roles that have no mailbox):

- target confirmed a version that admitted the asker → answer normally
- not confirmed → send the target the current roster, answer "not yet"
- **asker not admitted at all** (not registered on this hub) → answer
  "not reachable", and do NOT push.  No version can ever satisfy the
  test, so pushing would be a livelock rather than a delay.  A distinct,
  permanent condition and it must read as one.

Compute under `roster_mu_`, send outside it.  Address the target by its
`role_uid`: I-DEALER-IDENTITY (HEP-CORE-0046) makes that the control
DEALER's routing id and the broker verifies it at REG admission, so no
new state is needed to find a role.

A role that stops and restarts is handled with no special case: it
receives a NEW, higher admission version, so every prior confirmation
stops covering it and the gate re-runs.

**L4 — the contract is visible to the caller.**

`open_inbox` must distinguish "not reachable yet" from "no such role" and
from "that role has no inbox".  It already returns nothing on several
paths; what is missing is which.  The failure is now an ordinary,
clearing condition and a caller cannot act sensibly on an undifferentiated
empty answer.

**L5 — proof.**

*Three existing L3 tests fail on L3's landing, all from one cause.*
`Pattern4BrokerProtocolTest.RoleInfoReq_WithInbox_ReturnsInfo`,
`Pattern4BrokerProtocolTest.WireConformance_RoleInfoAck_Shape`, and
`DatahubBrokerTest.Sch_InboxSchemaDiscoveryRoundTrip` each ask
`ROLE_INFO_REQ` from a client that **never registered** — so the gate
answers `sender_not_registered` and withholds the coordinates.  They pin
the pre-gate contract, in which anyone holding a vault key could learn any
role's inbox address.

They are not worked around.  Each becomes a faithful exercise of the whole
protocol: the asker registers, the target confirms its roster by sending
`ROSTER_CHECK_NOTIFY` with the `known_roles_version` from its own REG_ACK,
and only then does `ROLE_INFO_REQ` disclose.  That turns three tests that
asserted an ungated read into three that pin the sequence the design
requires — and yields the positive reachability case for free.

*Checked: gating this message breaks no directory feature.*  The only
production caller of `query_role_info` is `open_inbox_client`
(`role_api_base.cpp`), so `ROLE_INFO_REQ` **is** inbox discovery rather
than a general directory query.  `ROLE_PRESENCE_REQ` is the liveness probe
and is untouched.  The "probes legitimately ask about other roles" note on
`Tier::Control_EnvelopeWithQueryRoleUid` explains why `identity_match` is
skipped when the body names a third party; it does not describe a live
probe consumer of this message.

*Done.*  All three were rewritten as protocol exercises rather than worked
around, and the shared sequence exists once at each layer rather than three
times: `confirm_roster()` in `pattern4_wire_test_base.h`, and
`raw_roster_confirm()` in the datahub workers — the latter obtained by
factoring `raw_heartbeat`'s dealer setup into a shared `raw_notify()`
instead of copying it a twelfth time.  `RoleInfoReq_WithInbox_ReturnsInfo`
now also asserts the WITHHELD state before confirmation, so the sequence is
pinned from both sides.

*A wire overreach the tests caught.*  The first cut of the gate dropped the
`inbox_*` keys from the no-inbox reply — a change the design never asked
for, which `WireConformance_RoleInfoAck_Shape` refused.  `ROLE_INFO_ACK` now
has ONE key set for every outcome, with `found` and `reason` carrying the
difference; three outcomes emitting three key sets would have been three
wire messages wearing one name.

**Version-guard coverage — where it can and cannot go.**

The behaviour to pin: an offered version `<= held` re-publishes nothing but
STILL sends a confirmation.  The second half is the livelock fix — without
it a lost confirmation leaves the hub pushing and the sender refused until
the next tick.

- **L1** — impossible without extracting the decision out of
  `adopt_inbox_roster`, which was considered and rejected.
- **L2 — investigated, NOT cheaply reachable.**  `apply_consumer_reg_ack`
  is public and takes a hand-built ACK, so no test seam is needed.  But
  adoption sits ~128 lines in, behind channel resolution, producer
  handling and transport setup; an in-process role must satisfy all of it
  first.  The test would be mostly about constructing a plausible ACK, and
  would pass or fail for reasons unrelated to the guard.
- **L4 — attempted, and it does NOT cover the fix.**
  `ZmqE2E_InboxTwoSendersOneUnconvergedReceiver` was written and kept, but
  **verified against the defect and found not to catch it**: with the
  confirmation-on-decline disabled the test still passes, because the
  periodic version report restores the hub's view within a tick — well
  inside any delivery budget a subprocess test can use.  Both senders
  arrive either way, so an assertion on arrival cannot tell the two
  implementations apart.  The test is retained for what it genuinely
  covers (fan-in through the gate: two roles reaching one unconverged
  target, otherwise unexercised) and its comment now says so explicitly.
  **The confirmation-on-decline behaviour remains UNCOVERED**, and the
  periodic tick is what makes it hard to cover — the safety net masks the
  fault it is a net for.
  *Method note:* the disable-and-rerun check is what caught this.  Applied
  to the restart test it confirmed real coverage; applied here it disproved
  a claim I had already written into the test's own comment.

- **L4 — original reasoning, superseded by the result above.**  The natural scenario is TWO senders opening an
  inbox to one receiver before it has confirmed: each `ROLE_INFO_REQ`
  triggers a push, so the second lands on a version the receiver already
  holds.  Realistic (any fan-in messaging pattern), no backdoor, logs as
  the observable.  Caveat to design around: whether the second push lands
  before the confirmation is timing-dependent, so the deterministic
  assertion is that BOTH senders deliver — which is exactly what the
  livelock would break — with `event=InboxRosterNotNewer` asserted as
  supporting evidence rather than as the gate.

- **L3 — where the duplicate IS deterministic, and now pinned.**
  `Pattern4BrokerProtocolTest.UnconfirmedTargetIsRePushedTheSameRoster`.
  The reason L4 cannot force the duplicate is that a live role confirms the
  first push in microseconds; a raw wire client simply never confirms, so
  the duplicate becomes the only possible outcome instead of a race.  Two
  `ROLE_INFO_REQ`s about one unconfirmed target, each followed by a
  `drain_for` on the target's DEALER: both pushes arrive, both carry the
  version the target's own REG_ACK carried, and the coordinates stay
  withheld throughout.  Every step is a reply on the connection that
  carried its request — no sleep, no poll for an event that may not come.
  Verified load-bearing by disable-and-rerun (push suppressed → FAILS).

  This pins the *precondition* — the hub re-sends an identical roster —
  not the role's response to it.  The split is honest and worth stating:
  the hub side is deterministic because the hub is driven by requests; the
  role side is not, because the role's response is immediate by design.

**Reachability of the role side, corrected.**  An earlier note here
proposed that a role with two channels on one hub would receive two
REG_ACKs at the same version and hit the guard deterministically.  **That
is wrong, and reading the code is what caught it.**  A producer holds one
`tx_queue` and one channel, so there is no second same-side registration;
a processor registers twice, but its two registrations land on *different*
sides (`kInputSide` / `kOutputSide`), and each side's held version starts
at 0.  No natural duplicate-ACK path exists.  The guard's live trigger is
the pushed duplicate above, and that arrives only inside the window
between a push and its confirmation.

**No backdoor.**  `role_api_base_test_access.h` was extended with three
accessors during this work and REVERTED: two duplicated observability the
log already provides, and the third answered "this is hard to test" with
access instead of asking why it was hard.

**The §4a backlog, with the approach for each.**  §4a names these as "how
the version-0 hole shipped and survived a step that rewrote the function
around it".  This session found three bugs by reading and none by testing,
so they close on evidence rather than on trust.

| Case | Where | Shape |
|---|---|---|
| No-authority-bound deny | L3 `hub_inbox_queue_workers` | Bind an `InboxQueue`, never call `set_admission_authority`; a CURVE peer with a valid key is still refused.  Reachable with existing worker infrastructure — nothing role-level needed. |
| Stale-version rejection | role-level | Adopt version N, then offer N−1; held roster AND version unchanged. |
| Malformed entry → whole-roster refusal | role-level | Offer a roster valid but for one entry; the previously held roster survives INTACT.  Half-applied is the dangerous outcome — it admits a prefix of a list the role rejected. |
| Two-sided OR | role-level, dual-hub | A key either side vouches for is admitted, and replacing one side leaves the other untouched. |

The three role-level cases drive `adopt_inbox_roster` with hand-built
acknowledgements.  It is private to `RoleAPIBase::Impl` — but **the seam
decision is already made and does not need re-deciding**:
`tests/test_framework/role_api_base_test_access.h` is a friend shim,
catalogued in README_testing as "reach private role/hub state from a test
without loosening production visibility".  It currently exposes only
`install_handler`; extending it with an adoption entry point is TEST
surface, not production surface, which is the distinction the no-test-
surface rule actually turns on.

So the work is: extend the existing shim, then write three focused tests
against it.  No new mechanism, no production change.

**The two fixes found by reading are now pinned** — the gap that mattered
most, because a fix nothing exercises survives only until the next
refactor, which is exactly how the version-0 hole shipped.

- ✅ **Stale confirmation on restart** —
  `RestartedRoleIsNotReachableOnAStaleConfirmation`.  Full stop/restart
  cycle: register → confirm → reachable → deregister → re-register → **must
  be `not_reachable_yet`** → confirm → reachable again.  *Verified to
  catch the bug*: with `reset_role_confirmation` commented out the test
  FAILS, and passes with it restored.  A regression test that has not been
  seen to fail on the defect is an assumption, not evidence.
- ✅ **§4.9.9 no-poll** — the L4 delivery test now also asserts the sender's
  log contains NO `InboxRosterAdopted` on any side, checked at the END of
  the test so several heartbeat ticks have elapsed.  Holding no roster is
  exactly what stops the periodic report, so this pins the precondition
  positively; the marker pins that the decision was reached, and this pins
  that nothing adopted afterwards by another path.

Cases closed by this arc:

- ✅ the **decrease** case, unreachable before I-ROSTER-PRESENT —
  `RosterShrinksWhenARoleDeregisters`.  A registers, B registers, B
  deregisters, C registers; C's REG_ACK shows B gone and a higher version.
  Observed through successive REG_ACKs rather than a pushed notify: that is
  the same evidence a role acts on and needs no unsolicited receive.
  Membership asserted as a SET — the hub filters an unordered set of present
  uids, so pinning wire order would pin an implementation detail.
  **This is what the version machinery was built for and had never
  exercised**: while the roster was every configured role it could only grow
  with the vault, so `revoke` and the monotonic guard were dead weight that
  still passed review.
- ✅ the **ordering** case that started all of this — `ZmqE2E_InboxDelivery`
  passes in 2.05s where it previously failed at 18.9s.  NOT rewritten: the
  script already loops at `open_inbox`, which is exactly where the design
  puts the retry, so the fix made the existing test pass rather than needing
  a new one.
- ✅ the **permanent** case —
  `InboxWithheldPermanentlyFromUnregisteredAsker`.  A caller holding a vault
  key (so it authenticates to the broker fine) but no registration gets
  `sender_not_registered` and an empty address, rather than a "retry" it
  would spin on forever.
- ✅ the **withheld** case — asserted inside
  `RoleInfoReq_WithInbox_ReturnsInfo` before confirmation, so the sequence is
  pinned where it refuses as well as where it succeeds.

Then the full unfiltered sweep in both configurations.

### What step 7 did

Three findings from the 2026-08-05 review of steps 1-6, all inside
`adopt_inbox_roster`:

- **The version read moved inside the try.**  A wrong-typed
  `known_roles_version` threw past the function and failed the whole
  registration, while a wrong-typed *entry* kept the previous roster and
  carried on.  Two halves of one block, two policies, neither stated.
- **The `gate_keys` count moved out of the lock.**  It built two full key
  projections while holding the mutex that the ZAP pump thread takes for
  every handshake.  Both side pointers are now copied out under the lock
  and counted with it released.
- **The hand-off comment pointed at a check that does not exist.**  It
  claimed enforcement of "an inbox role has a non-empty roster" lived at
  the inbox-arm site; that site (`consumer_role_host.cpp`, and the
  producer/processor equivalents) does no such check, and could not — it
  runs before registration, when no roster exists yet.

The third became real enforcement rather than a corrected comment.
`adopt_inbox_roster` now returns a bool and both ACK handlers fail the
registration on false.  The test is on the OUTCOME — *does this role end
the attempt able to gate this side* — not on any single way of failing,
because an absent field, a malformed entry and a stale version are three
faults with one consequence.  A role with no inbox has nothing to gate
and always passes.

**Assumption to confirm:** hard-fail was chosen over log-and-continue
under the project's hard-enforce default.  A role that owns a mailbox and
holds no roster would otherwise report a successful start while denying
every sender, leaving the sender with a connection that never completes
and the receiver with no log line at all.  Softening this to a warning is
a one-line change if the owner prefers it.

### BLOCKER found by step 8: a ZAP denial is terminal, so the window does not heal

**HEP-CORE-0035 §4.9.7 currently claims the opposite and is wrong.**  It says
a refused sender "costs one connection attempt", that "the socket reconnects
on its own", and that the sender is "admitted on a later reconnect once that
peer's next tick lands."  That was derived from the socket's reconnect
*policy*.  The deny *semantics* are different, and the codebase already said
so in two places before this work started:

- `src/utils/security/zap_router.cpp:608` — the DENY log line reads
  "peer's CURVE handshake will be terminal per libzmq; no client-side retry".
- `src/utils/hub/hub_zmq_queue.cpp:1583` — "treats ZAP DENY as terminal
  (no client-side retry)".

`PlhHubCliTest.ZmqE2E_InboxDelivery` then demonstrated it.  Sequence from the
role logs (2026-08-05 run):

```
recv  28.768  InboxRosterAdopted side=output entries=1 version=1   <- only itself
send  28.971  InboxRosterAdopted side=output entries=2 version=2   <- registered later
send  28.972  opens inbox to recv, sends
recv  28.973  ZapRouter DENY pubkey='...send...' — not in allowlist
recv  30.771  ROSTER_UPDATE_NOTIFY arrives
recv  30.818  InboxRosterAdopted side=output entries=2 version=2   <- now admissible
send  33.976  InboxClient ACK timeout (seq=0) — rc=255
```

The replication mechanism worked exactly as designed: the receiver's roster
converged 2s after the sender appeared.  The sender's connection was already
dead and did not come back.

**Why this could not happen before.**  A static roster is the complete vault
list from the moment a role starts, so a peer that appears later is already
admissible.  I-ROSTER-PRESENT is what introduces an ordering window, and the
window does not close by itself.

**Second, smaller finding.**  The test's sender script sets `_sent = True`
after `h.send()` regardless of the return code, so it is one-shot and would
not retry even if retrying worked.  Whatever is decided below, that script
needs to react to `rc`.

### The full sequence, both levels

Traced against source, not reconstructed.  Reconstructing this is expensive
and three design passes went wrong for want of it, so it is written down.

**Phase 1 — receiver R comes up.**  R registers; REG_ACK carries the roster
(`entries=1 version=1` — R only; S has not registered).  R adopts it into
`inbox_roster_[output]`.  `InboxQueue::start()` (`hub_inbox_queue.cpp:379`)
then, IN THIS ORDER: ROUTER created → `apply_socket_policy(TcpBind)` →
`arm_curve_server` + `zap_domain="<R.uid>:inbox"` → **`register_domain`
BEFORE `bind()`** so no handshake arrives un-gated → `bind()`.
`set_admission_authority` binds `Impl::roster_admits`.  Periodic tick installs.

**Phase 2 — S opens the inbox.  This path ALREADY makes a hub round trip.**
`api.open_inbox(R)` → `RoleHostCore::open_inbox` (cache miss) → for each of
S's connections `bc->query_role_info(R, 1000)`:

- **ROLE_INFO_REQ** → hub (synchronous; blocks the script thread up to 1s)
- **ROLE_INFO_ACK** ← hub, carrying `inbox_endpoint`, `inbox_schema`,
  `inbox_packing`, `inbox_checksum`, and **`inbox_receiver_pubkey_z85`**
  (R's identity key, used as the DEALER's `curve_serverkey`)

`InboxClient::connect_to` creates NO socket — layout and buffers only.  Then
`set_curve_client_identity`, then `start()` (`:876`): DEALER created →
`apply_socket_policy(TcpConnect)` (linger 0, sndtimeo 500ms, heartbeat 5s/30s,
`reconnect_ivl=100/max=1000`, and **`reconnect_stop = CONN_REFUSED |
HANDSHAKE_FAILED | AFTER_DISCONNECT`**) → `routing_id=S.uid` →
`arm_curve_client` → `connect()`, which returns immediately.  The cache now
holds the entry with `running_=true`.

**Phase 3 — handshake, libzmq I/O thread.**  TCP connect; **a pipe to the
session is created at connect time** (`ZMQ_IMMEDIATE` is not set), so S's
DEALER has a writable peer BEFORE the handshake completes.  ZMTP greeting →
CURVE → HELLO / WELCOME / INITIATE → R's `curve_server_t::process_initiate`
extracts S's long-term key → `send_zap_request(key)` on
`inproc://zeromq.zap.01`.

**Phase 4 — the decision, on the process-wide ZAP pump thread.**
`ZapRouter::pump_one` → credential to z85 → `routing.with_admission` (shared
lock + RecursionGuard) → `InboxQueue::is_peer_allowed` → mechanism is CURVE ✓,
`admits_` bound ✓, `roster_admits(z85)` takes `inbox_roster_mu` and asks each
side's `PeerAuthority` → **S is not in R's version-1 roster → false** →
`send_zap_reply("400", "Not in allowlist")` + WARN DENY.

**Phase 5 — what libzmq does with the 400.**

On R (server side): `zap_client_t::handle_zap_status_code()` fires
**`ZMQ_EVENT_HANDSHAKE_FAILED_AUTH(400)` on R's own ROUTER socket**
(`zap_client.cpp:227`) — nobody monitors it.  State → `sending_error` →
`produce_error()` sends a ZMTP **ERROR command whose body is the three bytes
`400`** (`curve_server.cpp:460`).  Connection torn down.

On S (client side): `curve_client_t::process_error` → `handle_error_reason` →
**`ZMQ_EVENT_HANDSHAKE_FAILED_AUTH(400)` fires on S's DEALER**
(`mechanism_base.cpp:41`).  **The event is emitted; nothing in pylabhub
listens.**  `_state = error_received` → engine `error(protocol_error)` →
`session_base_t::engine_error(handshaked=false, protocol_error)` →
**`clean_pipes()` discards the queued message** → `terminate()`, NOT
`reconnect()` (`session_base.cpp:463-471`).

**Phase 6 — the send, concurrent with the above.**  `h.acquire()` → write →
`h.send()` → `InboxClient::send(5000ms)`: pack frame + checksum →
**`send_seq_.fetch_add(1)`, seq consumed and deliberately never rolled back**
→ fresh nonce + wall_ts → `send_multipart_atomic` with **`dontwait`**, which
**succeeds** because the doomed pipe exists → ACK wait loop, deadline +5000ms.
The pipe dies 1ms later.  Five seconds on, `ACK timeout (seq=0)` → **255**.

**Phase 7 — what never happened.**  R's ROUTER never saw a message, so
`recv_one` returned nullptr and `send_ack` was never called — **there is no
transport-level ACK anywhere; the ACK is application-level**.  No reconnect
(twice determined).  No application retry.  The `InboxClient` remains in
`inbox_cache_` with `running_=true` forever.  R adopts version 2 two seconds
later and `roster_admits(S)` becomes true; nothing knocks again.

### What the trace changed

1. **The 5-second wait is our socket configuration, not fate.**  The send
   succeeded only because libzmq queues into a pipe created at `connect()`.
   `ZMQ_IMMEDIATE` makes a socket refuse to queue with no established
   connection — turning this into an immediate `send:blocked` instead of a
   5s ACK timeout.  Trade-off: a legitimate send issued mid-handshake would
   also fail fast.  A decision, not a reflex.
2. **"Never retry a failed handshake" is a project decision, in writing.**
   `zmq_socket_policy.hpp:145-155` sets `RECONNECT_STOP_HANDSHAKE_FAILED`
   unconditionally, reasoning "a failed handshake means we are not
   authorised… retrying is pure noise."  True when the roster was static and
   a denial was permanent.  Under I-ROSTER-PRESENT a denial is TEMPORARY, so
   that premise no longer holds for the inbox.  The same comment says
   connection-lifecycle policy belongs ABOVE the socket, on the monitor.
3. **The receiver can learn of a denial without touching the ZAP gate.**  R's
   own ROUTER fires `HANDSHAKE_FAILED_AUTH` too — on the monitor, off the pump
   thread, with no reentrance concern and no work inside the admission
   decision.  Strictly better than the in-gate flag drafted and deleted
   earlier.  It carries the status code only, not the pubkey: enough to mean
   "refresh my roster", and it discloses nothing.
4. **None of this displaces the primary fix.**  The above is about recovering
   from a knock.  The gate below stops the knock happening.

### The design: gate at ROLE_INFO, monitor as the safety net

**The rule.**  The hub does not hand S the address of R's inbox until R can
admit S.  S already makes a synchronous hub round trip immediately before
dialling (Phase 2), so this costs no new message.

**It uses the ledger half that is currently dead.**  `is_visible_to(R, S)`
asks exactly "has R confirmed a roster version at or after the one that
admitted S".  §4.9.4's note that "the confirmation map has no consumer yet"
stops being true.

**The confirmation is a message that already exists.**  `ROSTER_CHECK_NOTIFY`
carries `{role_uid, known_roles_version}` — the hub calls `confirm()` with it.
No new msg_type.

**Flow.**  S asks about R → hub tests `is_visible_to(R, S)`:
- **yes** → answer normally; S dials and is admitted on the FIRST attempt
- **no** → push `ROSTER_UPDATE_NOTIFY` to R now, answer S "not ready"

**Logic hole found in review, and its fix.**  A role only reported its version
on its periodic tick, so the hub could not learn R had converged for up to a
full heartbeat interval — reintroducing the latency the gate exists to remove.
Fix: **R sends `ROSTER_CHECK_NOTIFY` immediately on adopting a pushed roster.**
That is an acknowledgement of the push, not new machinery.  Loop becomes
ask → push → adopt → confirm → next ask succeeds.

**Why the rate limiter drafted earlier is not needed.**  The push fires only
when a *connected role* asks about a target AND that target is unconfirmed.
Once confirmed there is no push, so spamming the request yields at most one
push per real roster change.  Self-limiting by construction rather than by a
counter.

**Redundancy check — everything still earns its place.**

| Piece | Job |
|---|---|
| Presence filter (I-ROSTER-PRESENT) | the security surface; nothing else does this |
| Periodic tick | bounds REVOCATION — nobody asks about a role that stopped, so no push fires for it |
| Roster on REG_ACK | bootstrap before the first tick; seeds the first confirmation |
| Push on demand | bounds ADMISSION latency |
| `ROSTER_CHECK_NOTIFY` | doubles as the confirmation — removes a message rather than adding one |

**Risks, checked.**

- **The asker's identity is NOT verified on ROLE_INFO_REQ.**
  `envelope_with_query_role_uid` (`wire_dispatch.cpp:463`) runs grammar +
  role-tag policy only — no identity match, no attested-ownership gate, by
  design, because its `role_uid` names the QUERIED subject.  Acceptable
  ONLY because this gate is a reliability mechanism; the security control
  remains the receiver's ZAP gate on the proven key.  A caller that
  misidentifies itself gets a wrong readiness answer and then a denied dial —
  i.e. degrades to today's behaviour, gaining nothing.  **The guarantee must
  be stated as "the first dial succeeds if the caller identified itself
  honestly", and the code must not be written as though this were a security
  check.**
- **Cross-hub — self-consistent, no break.**  `open_inbox` iterates S's own
  connections, and a connection exists only where S has a presence.  So if S
  can reach the hub that knows R, S is registered there and is in R's roster.
  Reads like a bug until traced; hence written down.
- **A role with no inbox never confirms**, so `is_visible_to` is permanently
  unanswered for it.  Also self-consistent: such a role has no `inbox_schema`,
  so `open_inbox` fails earlier and the gate is never reached.
- **Naming defect introduced by step 8.**  `roster_ledger_` is now
  `VersionedAdmissionLedger<std::string, std::string>` with BOTH parameters
  holding uids, while the primitive documents its first parameter as "the
  admitted peer's identity (z85 pubkey in production)".  The usage is right;
  the doc now lies.  Needs a corrected doc on the primitive or a named alias
  at the use site.
- **Locking — clean.**  HubState fires presence handlers AFTER releasing its
  lock (`hub_state.cpp:793`, `:837`); the handlers take only `roster_mu_`.
  No ordering hazard with `roster_ack_block`.

**Open choice (owner's).**  When R is unconfirmed, does the hub
(i) answer "not ready" and let the caller ask again — no broker state,
`open_inbox` already returns nothing on several paths and callers already
handle it; or (ii) pend the request and answer when R confirms — no caller
retry, precedent in `CONSUMER_ATTACH_REQ_ZMQ`'s `{status:pending}` + the
existing timeout sweep, at the cost of pending-queue state.  Leaning (i):
a pending queue is somewhere for entries to leak, and the caller-side loop
runs against a contract that already exists.

**Rejected, and why.**  Retry counters, reason-coded failures, redial
semantics, and a rate-limited in-gate refresh trigger were all drafted and
dropped: each is machinery for recovering from a knock that the gate prevents,
and correctness would depend on all of them working together.

**Doc corrections owed** (see §"Doc corrections" below).

### Cross-check review: code against document

Run after L1–L4 were green (2774/2774 Debug).  **Every finding below was
invisible to the test suite** — the sweep passed with the first one live.

**1. §4.9.9 violated by this work, silently.**  `adopt_inbox_roster` never
checked whether the role owns an inbox, so a role with none adopted a roster
anyway; `report_roster_version_` skips only a NULL side, so that role then
reported its version to its hub on every tick, forever, for a list it never
reads.  That is verbatim what §4.9.9 exists to prevent ("poll forever for
data it never reads… unexplained traffic rather than anything
identifiable"), and the comment in `send_roster_checks_` asserted the
opposite of what the code did.  Introduced when the tick was added; nothing
tested it, because holding an unused roster breaks nothing observable.
Fixed at the single adoption point, which is also the only place that can
know.

**2. Four comments still describing the pre-I-ROSTER-PRESENT roster** as
"known_roles membership" — `role_api_base.cpp` (the member doc and the
CURVE-arm site), `hub_inbox_queue.hpp` (both the ROUTER and DEALER arming
docs).  Membership is the vault INTERSECTED with current registrations; a
reader following any of these would have had the old model.

**3. The L4 test asserted the old design in prose and in code.**  Its
comment said the roster "is projected from the static operator roster, not
from who has registered so far, so both roles see the same numbers
regardless of registration order" — the exact claim I-ROSTER-PRESENT
retires.  Its sender-side assertion required a roster that role has no
mailbox to gate.  Replaced with a positive pin on the skip, because "no
adoption line" and "adoption silently broken" look the same.

**4. `PeerAuthority` now serves two roles and documented one.**  A hub's
vault index (gates its own door, names senders, replicated to nobody) and a
role's roster snapshot (replicated, versioned) are both built from it.
`version()` has exactly ONE reader, on the role side.  The `= 0` default on
`build` is therefore a deliberate "unversioned", which nothing said.

**5. A false justification I wrote this session.**  The new decrease test
claimed wire order is unspecified.  `local_role_roster()` returns a
`std::set` in uid order and documents that order as deliberate; filtering
preserves it.  Asserting membership as a set is still right, but not for
that reason.

**6. Log level hid the evidence.**  The skip marker was DEBUG while its
sibling adoption line is INFO, so in a role log "declined a roster" and
"declined nothing, the level hid it" were indistinguishable.

**Second pass — what the first pass's own fix created.**

**7. A dead branch, immediately.**  `gated_or_refuse_` opened with
`if (inbox_queue == nullptr) return true;`.  Once `adopt_inbox_roster`
returns early on that same condition, all three of its callers are
unreachable with a null inbox — so the test became a copy of a rule
enforced upstream, and the copy that never executes is the one that rots.
Removed, with the reachability stated where the reader will ask.

**8. Two docs left describing the removed path.**  `gated_or_refuse_`'s
docstring still said "a role with no inbox has nothing to gate and always
passes", and `adopt_inbox_roster`'s said such a role "does not care whether
a roster arrived".  Neither is true now: it declines outright and never
reaches either.  A fix that leaves its own documentation behind is half a
fix.

*Verified clean in this pass:* `set_inbox_queue` already handles a null
queue correctly (binds nothing, gate never armed); HEP-CORE-0027 §3.5's
roster bullet already says "knows and currently has registered";
HEP-CORE-0035 §4.9.6's bootstrap rule ("no list yet == DENY") is unaffected
and its pseudocode already reads "for side ... where side exists".

### Doc corrections owed to HEP-CORE-0035 §4.9

Four statements in the governing HEP are now false.  Three were written during
this arc, on reasoning the trace above disproves.

- **§4.9.7** — "a refusal costs the sender one connection attempt", "the socket
  reconnects on its own", "admitted on a later reconnect once that peer's next
  tick lands".  All false.  The session is terminated, not reconnected, and the
  project's own socket policy independently forbids the retry.
- **§4.9.8** — "The roster withholds nothing", "Confirmation means: this role
  has converged; nothing waits on it", and the direction row "Role **asks** on
  its own schedule".  Under I-ROSTER-PRESENT the inbox opener DOES wait, and
  the hub pushes on demand.  The correction makes the design more consistent,
  not less: both lists now withhold until the far side confirms, under one
  rule, instead of the roster being an exception to it.
- **§4.9.4** — "The confirmation map it also maintains has no consumer yet."
  It has one: the ROLE_INFO gate.
- **§4.9.7's "no second trigger"** — the reasoning holds for a flag consumed by
  the next tick (genuinely redundant) but must not be read as forbidding the
  ROLE_INFO-triggered push, which is a different trigger with a different
  justification and no unauthenticated lever.

New material §4.9 must carry: the ROLE_INFO gate rule, confirm-on-adopt, and
the honest scope of the guarantee ("first dial succeeds if the caller
identified itself honestly" — the asker's identity is unverified on that
message tier by design).

### Why step 8 cannot be split

The design change (HEP-CORE-0035 §4.9.2, I-ROSTER-PRESENT) makes the
roster mean "known AND currently registered" instead of "known".  The
broker-side filter and the role-side periodic check must land together,
because **the filter alone is a regression.**

Today the roster is static, so a role that registers first still
receives every configured role — complete, merely containing entries
for roles that are not running.  Filter by presence without a refresh
path and that role receives only whoever happened to be registered at
its own registration instant, and never hears about anyone who starts
later.  Reachability would get strictly worse than before the change.

The filter shrinks what a role is told; the periodic check is what
keeps telling it.  Neither half is shippable alone.

Steps 1-6 are done, build clean, and pass 96/96 on
`-R 'InboxQueue|InboxDelivery|KnownRoles|PubkeyOrigin|PeerAuthority|Inbox'`
plus 6/6 on `DatahubBrokerTest.Sch_Inbox*`.  Nothing is committed.

### What step 6 did

`InboxQueue` no longer stores an allowlist.  `set_admission_authority`
binds one `std::function<bool(const std::string&)>`, held in the same
atomic-shared_ptr slot the allowlist used, and `is_peer_allowed` calls it
after checking the peer arrived over CURVE.  `RoleAPIBase::set_inbox_queue`
binds `Impl::roster_admits` — the wiring moment, not the first adoption, so
the gate is never up with nobody to ask.

`set_peer_allowlist` on an inbox is now **inert and returns false**, with
`peer_allowlist_snapshot` returning `nullopt`.  Refusing beats
accepting-and-ignoring: a caller that expects the push to gate something
finds out.  There is precedent — `ZmqQueue`'s dialing side is inert the same
way.  The four test sites that pushed a hand-built allowlist now bind an
authority instead, which pins the gate rather than the storage.

Two things fell out of it that were not the goal:

- **A teardown window closed.**  `adopt_inbox_roster` used to dereference
  `inbox_queue` on the BRC poll thread, and `teardown_infrastructure_`
  destroys `inbox_queue_` at step 2 while the API's threads are still
  running (step 3).  Nothing pushes to the queue any more, so that
  dereference is gone.  The callback runs the other direction, and the
  direction is the safe one: the queue is destroyed first, and stopping it
  unregisters its ZAP domain, so no handshake can be inside the callback
  afterwards.
- **The `entries=`/`total=` naming from §4a is resolved.**  `total=` is now
  `gate_keys=`, and the log comment states what each counts.

---

## 4a. Carried out of the 2026-08-04 second review

Checked and cleared — do not re-litigate:

- `Z85PublicKey::validate` throws `std::invalid_argument`; `roster_admits`
  catches exactly that.
- No lost-update race on `set_peer_allowlist` (computed under the lock,
  applied outside it). `processor_role_host.cpp:474/502` applies both ACKs in
  straight-line code on one thread, so the two sides never adopt concurrently.
- ~~The version guard admits EQUAL versions on purpose. `<` is required: a hub
  with no known_roles sends version 0 and a side holding nothing reports 0, so
  an `<=` early-return would make the first adoption never happen.~~
  **WRONG, and re-litigated for good reason.**  The premise was a collision I
  created: "I hold nothing" and "I hold revision zero" both reported 0, and
  rather than separating the two states I weakened the comparison so equal
  would slip through — which is the guard admitting what it exists to reject.
  Accepting equal also made every repeated push reparse, rebuild, republish
  and report back to produce a duplicate of what was already held, on a path
  where repeated pushes are ordinary (several senders asking about one
  unconfirmed target).
  **Resolution: versions start at 1 and 0 means "no roster"**
  (I-ROSTER-VERSION).  The sentinel IS the version, so nothing can drift out
  of step with it; the guard is `incoming == 0 → refuse`,
  `incoming <= held → nothing to do`, else adopt.  The bootstrap case the
  exemption existed for is unreachable: a registration answer is assembled
  after the role it answers has been admitted, so the version on it is never
  0.
  **Lesson worth keeping:** a comparison that needs an exemption is usually
  a signal that two states are sharing one value.

Still open:

- ~~`entries=` and `total=` mean different things~~ — resolved by step 6:
  `total=` is now `gate_keys=`.
- **Nothing tests the guard or the split.** The L4 test pins one adoption on
  one side. Stale-version rejection, whole-roster refusal on a malformed
  entry, and the two-sided OR are all uncovered. This is how the version-0
  hole in §2.3 shipped and survived a step that rewrote the function around
  it. Step 9 owns these; they are listed here so the step is not read as
  "add a test for the happy path."
- **Step 6 added a fourth uncovered case.** The gate denies when no authority
  is bound and when the peer is not a CURVE peer. Both are deny paths that a
  passing delivery test cannot distinguish from an admitting gate, which is
  the shape of bug this whole arc keeps finding.

## 5. Traps already paid for

- **The heartbeat has no reply.** `send_heartbeat` is `void`,
  `handle_heartbeat_req` returns nothing, and `heartbeat_ack_block()` rides
  the *registration* ACK. Step 8 needs a real request/reply; only the timer
  is reused.
- **The periodic task is one per role**, installed on the master control
  thread, fanning out per presence internally. Fan out per side the same way —
  do not add a timer.
- **`known_roles` names two different things.** The operator's vault roster
  (~37 files: vault, CLI, config, L4 vault tests) and the wire field (3 sites).
  Only the wire field is in scope. Consider renaming it while it is already
  breaking.
- **Versions are per hub.** Never compare across sides. A hub restart resets
  its count, which is safe only because losing a hub is terminal for the role;
  if in-place reconnect is ever added, this needs a hub-instance identity.
- **Do not merge `KnownRole` and `RosterEntry`.** The subset relation is the
  privilege boundary — two types make the narrowing compiler-checked, so a
  field added to the operator's record cannot reach every role in a diff that
  looks like vault bookkeeping.
