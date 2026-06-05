# Authentication / PeerAdmission TODO

**Scope:** Open items for HEP-CORE-0035 (Hub-Role auth + federation
trust) and the PeerAdmission feature track that closes out the
data-channel CURVE auth gate.

**Authoritative design lives in:**
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — Layer-1 ZAP + Layer-2 federation trust gate + key-file ACL discipline (§4.6) + runtime key handling (§4.7).
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md` — Three-tier auth (transport / identity / authorization), `ChannelAccessIndex` (§4.1), channel-auth notify+pull wire (§6.5 — `CHANNEL_AUTH_CHANGED_NOTIFY` + `GET_CHANNEL_AUTH_REQ`/`_ACK`, amended 2026-06-04), per-producer pubkey + endpoint (§6.4).
- `docs/HEP/HEP-CORE-0017-Queue-Abstraction.md` §3.3 — `RxQueueOptions::producer_peers` queue auth contract.

**Status source of truth:** `docs/TODO_MASTER.md` (when PeerAdmission
work is the active sprint).

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
(Phase A/B/C; D1+D2+D3; landing-phase §4.6.5 no-bypass cleanup; BRC
monitor investigation; lib-stabilization exclusion procedure;
resolved decisions reference; considered-but-not-pursued).

---

## Current PeerAdmission state

| Phase | Status | Notes |
|---|---|---|
| A — Abstraction (PeerAdmission interface) | shipped | see archive |
| B — KnownRole + CLI | shipped | see archive |
| C — ZapRouter + ZmqQueue CURVE | shipped | see archive |
| D — Broker glue (gate closes) | 🚧 D1+D2+D3 shipped; **D4–D7 open** | see Phase D section below |
| E — Admin loopback enforcement | ⏸ planned | Unblocked once D ships |
| F — Federation peer ZAP parity | ⏸ planned | Depends on E + Federation HEP (#105) |
| G — SHM auth migration | ⏸ planned | Independent of D/E/F; can interleave |
| H — Demo migration | ⏸ planned | Last; needs D shipped end-to-end |
| X — Runtime key hardening | ⏸ planned | HEP-0035 §4.7; task #102 |

---

## Phase D — Broker glue: open steps (D4 → D7)

`HubState` holds the `ChannelAccessIndex` (HEP-CORE-0036 §4.1);
`BrokerServiceImpl` installs the CTRL ROUTER ZAP handler against the
operator-defined allowlist and (per D3) fires
`CHANNEL_AUTH_CHANGED_NOTIFY` whenever consumer membership changes,
prompting producer-initiated `GET_CHANNEL_AUTH_REQ` pulls (§6.5
amended 2026-06-04).  Remaining steps:

4. **D4 — Role-side dispatch.**  `BrokerRequestComm` recognizes
   inbound `CHANNEL_AUTH_CHANGED_NOTIFY` and routes it to a
   role-side handler that fires `GET_CHANNEL_AUTH_REQ`, applies
   the reply via `ZmqQueue::set_peer_allowlist(snapshot)`.
   Coalesce policy: if a query is already in flight for the same
   channel, drop the redundant notify (next reply will reflect the
   latest state).  Note: BRC's `on_notification_cb` hook exists
   (`broker_request_comm.cpp`) but no production code wires it for
   `CHANNEL_AUTH_CHANGED_NOTIFY`.  `ZmqQueue::set_peer_allowlist`
   awaits task #103 (dynamic peer API).
5. **D5 — `CONSUMER_REG_ACK.producers[]` array.**  One entry per
   producer of the channel — supports fan-in (HEP-CORE-0023 §2.1.1).
   Each entry: `{role_uid, pubkey, endpoint}`; SHM transport also
   carries `shm_secret`.  Currently the response body has none of
   these fields, so consumers can't learn the producer pubkey or
   endpoint needed for data-plane CURVE handshake.
6. **D6 — L3 tests.**  Broker pushes allowlist on consumer reg /
   dereg; producer applies; consumer with wrong pubkey rejected;
   revocation propagates within the contract bound.  Tracked
   under task #154 (re-create L3 broker tests against refactored
   lib code; the masking procedure that protected ctest during the
   lib-stabilization window is recorded in the archive).
7. **D7 — L4 test.**  Full dual-hub data flow with auth gates closed.

D4 + D5 land together with task #103 (`RxQueueOptions::producer_peers`
+ `ZmqQueue::add/remove_producer_peer`) — without it the role host
has nowhere to apply the pulled allowlist.

---

## Phase D close-out follow-ons (test + spec gaps surfaced 2026-06-03)

These are tracked here so they survive context resets per CLAUDE.md
§"Session hygiene" — open items must live in a subtopic TODO, not
only in chat history.

- **B1 — `awaiting_endpoint` reason missing in CONSUMER_REG R6 gate.**
  HEP-CORE-0036 §6.6 line 1370 enumerates three valid
  `CHANNEL_NOT_READY` reasons: `awaiting_endpoint`,
  `awaiting_first_heartbeat`, `heartbeat_stalled`.  Current code at
  `src/utils/ipc/broker_service.cpp:2226-2241` returns
  `CHANNEL_NOT_READY` on port-0 with message "has unresolved port
  0" — no `awaiting_endpoint` substring.  Client retry loop only
  matches the other two substrings, so the port-0 case is correctly
  terminal but the §6.6 catalog vocabulary is incomplete in code.
  Fold into the #103 commit batch (small 5-line change).  Effort:
  trivial.

- **B2 — Producer / consumer `zmq_pubkey` read from wire body, not
  ZAP socket identity (HEP-CORE-0036 §I6 violation).**
  HEP-CORE-0036 §I6 + §6.3 (lines 262, 709-712) require the
  identity pubkey to come from `zmq_msg_gets("User-Id")` (CURVE-
  proved, no self-claims).  Current code at
  `src/utils/ipc/broker_service.cpp:1383` (producer REG_REQ) +
  `:2442` (CONSUMER_REG_REQ) reads `req.value("zmq_pubkey", "")`
  from the message body.  Empty/non-40-char is hard-rejected but
  the value itself is still self-claimed.  **Security issue**: a
  compromised consumer can claim any pubkey to drift the channel
  allowlist via `_on_consumer_authorized`.  Fix: replace wire-body
  read with `zmq_msg_gets("User-Id")` recovery; reject with
  INVALID_REQUEST if wire body contains a mismatching `zmq_pubkey`
  (defence-in-depth).  Fold into #103 commit batch.  Effort: S
  (~20 LOC each site).

- **Allow-path L3 pin for D2.**  `DatahubBrokerHealthTest.CtrlZapDenyPath`
  pins the deny path.  Symmetric allow-path L3 needs a BRC client
  whose `client_pubkey` is added to the broker's `known_roles[]`
  before connect; that requires the test infrastructure to thread
  explicit CURVE keys into the worker's broker config (the existing
  L3 worker pattern uses ephemeral BRC keys, which the worker
  process cannot know ahead of time to pre-register).  Smallest
  fix: extend the `BrokerService::Config` test-side construction to
  include a pre-generated `known_roles[]` entry built from the test
  client's keypair.  Effort: S.
- **`plh_role --keygen` does not publish `<vault>.pub`.**
  HEP-CORE-0035 §4.8.3 specifies `plh_hub --add-known-role <role.pub>`
  as the canonical operator workflow; that requires the role binary
  to publish a sibling `.pub` file alongside the vault (the way
  `plh_hub --keygen` publishes `hub.pubkey`).  Currently the L4
  RoundTrip test opens the role vault programmatically to extract
  the pubkey — a test backdoor.  Mirror `HubVault::publish_public_key`
  for `RoleVault` (atomic O_EXCL + O_NOFOLLOW + mode 0644 +
  symlink defense per HEP-CORE-0035 §4.6.4).  Effort: M.
- **Hot-reload of `known_roles.json` on a running hub** (HEP-CORE-0035
  §4.8.5).  `BrokerCtrlAdmission::set_peer_allowlist` exists with
  no caller; the admin RPC (`/admin/reload-known-roles` or
  similar) is the missing wiring.  Operators that run
  `--add-known-role` against a running hub today must restart it
  to pick up the change.  Effort: M.
- **Multi-peer ZAP backlog draining.**  `ZapRouter::pump_one(0ms)`
  is called once per broker poll cycle.  Under N-peer simultaneous
  reconnect (e.g. hub restart) handshake latency is
  ~`kPollTimeout * N`.  Convert to `while (pump_one(0ms)) {}` to
  drain backlog.  Effort: trivial.

---

## CRITICAL — A1+A2 silently shipped zero CURVE coverage (2026-06-05 audit)

This section records the verified findings from the 2026-06-05 audit
so they survive context resets per CLAUDE.md §"Session hygiene".  Each
HARD-BLOCK below was confirmed against actual code (grep + file
read) — NOT against commit messages or docstrings.

### How the silent-fallback hole opened

PeerAdmission **Phase C** (commits `62bda863..47aa0374`, see archive)
introduced the auth-enabled ZmqQueue factories alongside the legacy
plaintext factories.  The migration design accepted a "bridge state":

> *Empty defaults mean plaintext — every field empty produces a
> socket configured exactly like the legacy `pull_from`/`push_to`
> factories.* (`hub_zmq_queue.hpp:144-146`, still present today)

The bridge was supposed to be torn down when HEP-CORE-0035 **§4.6.5
"no-bypass discipline"** landed (2026-06-04, commit `3a64e58c`).
That landing did rip out the `use_curve` field, the
`enforce_ctrl_admission` field, and the legacy `RoleIdentityPolicy`
machinery (3-revised slices A through E).  **It did NOT delete the
legacy `ZmqQueue::pull_from` / `push_to` factories or the
`ZmqAuthOptions` "all-empty = plaintext" carve-out.**

Result: any code path that asks for `push_to_with_auth` with empty
keypair fields silently falls back to plaintext — same behaviour as
the legacy factory.  The validator at `hub_zmq_queue.cpp:587-589`
explicitly endorses this:

```cpp
// All empty: legacy plaintext path, accepted by either side.
if (!has_pub && !has_sec && opts.serverkey_z85.empty())
    return {};   // ← validator says "OK"
```

That branch is the contract violation.  It is the design that needs
to die before anything else in the data-plane CURVE chain ships.

### Verified HARD-BLOCKS (2026-06-05)

Numbered HB-1 .. HB-6.  Each cites code; each must be fixed before
the data-plane CURVE contract can be claimed live.

**HB-1.  `build_tx_queue` reads `auth_client_pubkey` BEFORE `set_auth`
runs.**  In `producer_role_host.cpp:223` `setup_infrastructure_`
internally calls `build_tx_queue`; `set_auth(...)` is at line 313 —
90 lines later.  Same ordering in `consumer_role_host.cpp:214 → 289`
and `processor_role_host.cpp:245 → 346`.  Net effect: the
`push_to_with_auth` factory ALWAYS sees `pImpl->auth_client_pubkey
== ""`.  Combined with the silent-fallback in `validate_auth_options`,
the producer's PUSH socket binds plaintext.  A2 (commit `badfaed1`)
appears to have shipped CURVE but actually achieved zero CURVE
coverage.  The 1998/1998 ctest passes because every code path took
the plaintext branch.

**HB-2.  `ZapRouter::pump_one` is never called from any role-side
poll thread.**  Verified by grep across `src/`: only two call sites
exist — `broker_service.cpp:811` (broker poll thread) and
`zap_router.cpp:504` (ZapRouter's own optional poll-loop helper).
HEP-0036 §7.1 lines 1411-1436 explicitly require the producer-side
data ROUTER's ZAP handler to be pumped from a poll thread on the
role process.  If HB-1 were fixed, every consumer CURVE handshake
would hang indefinitely waiting for a ZAP REP.

**HB-3.  `set_peer_allowlist` has zero callers.**  Verified by grep
across `src/`: interface declared in `peer_admission.hpp:180`,
impl at `hub_zmq_queue.cpp:719`.  No production code anywhere calls
it for the producer's PUSH queue.  So even if HB-1 + HB-2 were
fixed, the producer's allowlist would remain the empty
`initial_allowlist` forever → every consumer's pubkey denied at the
ZAP cache → no data flows.  This is the missing **D4** wiring
(broker `CHANNEL_AUTH_CHANGED_NOTIFY` → producer pull →
`set_peer_allowlist`).

**HB-4.  `RegistrationState::Authorized` does not exist.**  Verified
at `role_presence.hpp:95-101`: enum has only `Unregistered /
RegRequestPending / Registered / Deregistered`.  HEP-CORE-0036 §14.3
+ §8 require an `Authorized` value plus the transitions into it.
Currently nothing in the role-side FSM bridges control to data.

**HB-5.  `any_presence_authorized()` + data-loop outer guard do not
exist.**  `data_loop.hpp:129-131` outer guard is:

```cpp
while (core.is_running() && !core.is_shutdown_requested() && !core.is_critical_error())
```

No `any_presence_authorized()` call.  Per HEP-CORE-0036 §8.2 this
predicate is the load-bearing gate that prevents the data loop from
entering BEFORE the producer's ZAP cache is populated.

**HB-6.  SHM `shm_secret` hardcoded to zero.**  `broker_service.cpp:1879`:

```cpp
hub_state_->_on_channel_access_opened(channel_name, /*shm_secret=*/0);
```

HEP-CORE-0036 §5.6 specifies the broker generates a per-channel
random `uint64`.  `CONSUMER_REG_ACK` also doesn't carry `shm_secret`.
SHM data plane has zero authorization today — any process that
knows the channel name attaches successfully.

### Test blind spots (why 1998/1998 ctest doesn't catch any of this)

No test in the suite asserts that CURVE is actually engaged on a
producer's socket.  Every "auth" test asserts a symmetric property:

| Existing test | What it asserts | Why it passes with CURVE off |
|---|---|---|
| `ZmqQueueAuthTest.AllowedPeer_DeliversRoundTrip` | data flows when both sides authed | also passes when both sides plaintext |
| `ZmqQueueAuthTest.UnallowedPeer_BlockedFromDelivery` | wrong pubkey denied | only meaningful if CURVE engaged to begin with |
| `ZmqQueueAuthTest.AdmissionIsEnforced_Lifecycle` | `admission_is_enforced()` true after start | `admission_is_enforced()` is itself silent-false on empty keys (`hub_zmq_queue.cpp:770`) |
| `test_layer4_plh_role` demos | end-to-end data flow | both sides plaintext = data flows = test passes |

The assertions that would have caught the no-op:
- **L2 ZmqQueue**: after `push_to_with_auth(...)` succeeds, `zmq_getsockopt(socket, ZMQ_MECHANISM, ...)` returns `ZMQ_CURVE`, not `ZMQ_NULL`.
- **L2 RoleAPIBase**: constructor REQUIRES a `CurveKeypair`; trying to construct without is a compile error.
- **L3 broker**: spin up a CURVE-enforced producer; a NULL-mech client cannot connect (handshake-failed monitor event observed).
- **L4 plh_role**: with deliberately empty / wrong `auth.keyfile` path, the role aborts at startup BEFORE binding any data socket.

These four assertions are tracked as part of cleanup commit C5 below.

---

## Strict-CURVE cleanup chain — C1..C5 (replaces naive ordering patch)

The audit above shows the right fix is not "move `set_auth` before
`setup_infrastructure_`" — that's still a workaround on top of the
silent-fallback design.  The correct fix removes the obsolete bridge
entirely.  Five sequential commits, each fails loudly if the wrong
thing happens.  Tracked as harness tasks #157..#163 (see below).

### Inventory of fallback-era residue (what dies / changes per commit)

A. **`validate_auth_options` all-empty branch** (`hub_zmq_queue.cpp:587-589`) — the smoking gun.  Dies in C1.

B. **Five `if (empty)` skip-CURVE conditionals in ZmqQueue impl**:

   | Line | What | After strict-mode |
   |---|---|---|
   | `:584-590` | validator returns OK on all-empty | DELETE — empty must error (C1) |
   | `:770` | `admission_is_enforced()` returns false if `my_pubkey_z85.empty()` | method becomes constant true → deleted from interface (C4) |
   | `:875` | `if (!my_pubkey_z85.empty())` — CURVE-setup block skipped if false | DELETE the conditional; CURVE setup is unconditional (C1) |
   | `:894` | `if (resolved_zap_domain_.empty())` — derive default name | KEEP — that's derivation, not auth bypass |
   | `:930` | `if (serverkey_z85.empty())` — skip `ZMQ_CURVE_SERVERKEY` set on connect side | DELETE — connect side must have serverkey (C1) |

C. **Two parallel factory pairs** (4 functions, should be 2):
   - `pull_from()` plaintext → DELETE (C4)
   - `push_to()` plaintext → DELETE (C4)
   - `pull_from_with_auth()` → RENAME to `pull_from` (C4)
   - `push_to_with_auth()` → RENAME to `push_to` (C4)

D. **`ZmqAuthOptions` overdesigned options bag**.  5 stringly-typed fields, each independently nullable.  Dies in C2 — replaced by `CurveKeypair` + optional `Z85PublicKey serverkey` + `zap_domain` as named factory args.

E. **`RoleAPIBase` auth surface — three overdesigned shapes**:
   - `set_auth(client_pubkey, client_seckey)` setter — exists ONLY because auth was optional → delete in C3
   - `auth_client_pubkey()` / `auth_client_seckey()` getters — exist for "did we set auth?" checks → return `CurveKeypair const&` in C3 (or drop entirely)
   - `pImpl->auth_client_pubkey/_seckey` storage as default-empty `std::string` → `CurveKeypair` member, no empty default state possible

F. **`admission_is_enforced()` interface method** is dead after C1 (always true).  Deleted from the `PeerAdmission` interface in C4.

G. **103 test sites in `tests/` call legacy plaintext factories** (grep `ZmqQueue::pull_from\b\|ZmqQueue::push_to\b`).  Migration burden in C4 — most use `curve_test_setup.h`; some delete-only.

H. **Stale documentation** (`role_api_base.hpp:55-58`, `plh_datahub.hpp` cross-refs, hub_shm_queue.hpp:60) references the plaintext factories as canonical examples.  Updated in C4.

I. **`hub_zmq_queue.cpp:617` "bind side: serverkey is meaningless. Don't fail if the caller set it by mistake — silently ignored"** — same silent-ignore anti-pattern.  Make it reject in C1.

### C1..C5 sequence (target design per commit)

| # | Commit | Scope | Why this order |
|---|---|---|---|
| **C1** | `validate_auth_options` strict-mode | Delete the "all-empty = OK" early-return at `hub_zmq_queue.cpp:587-589`.  Require both keypair fields non-empty + 40-char Z85.  Delete the silent-ignore at line 617.  Delete the "if empty skip CURVE" conditional at line 875.  Delete the "if empty skip serverkey" conditional at line 930.  Update `validate_auth_options` L2 tests for the new contract (rejection cases, not acceptance). | Smallest defensive change.  After this, every existing call site that relied on the silent path FAILS LOUDLY at the factory boundary.  All subsequent commits surface their dependents through real compile/link/run errors instead of silent miscompiles. |
| **C2** | Strong-type the keypair | Reuse / extend the existing `pylabhub::crypto::CurveKeypair` struct (`security/curve_keypair.hpp:55`).  Define `Z85PublicKey` strong type (40-char Z85 invariant).  Replace `std::string my_pubkey_z85 + std::string my_seckey_z85` with `CurveKeypair` everywhere it appears: `ZmqAuthOptions`, `AuthConfig`, `BrokerRequestComm::Config`, `RoleAPIBase::pImpl`, `HubHost::Config`.  Replace `std::string serverkey_z85`, `std::string ProducerPeer::pubkey_z85`, `std::string ProducerEntry::zmq_pubkey`, `std::string ConsumerEntry::zmq_pubkey` with `Z85PublicKey`.  Constructors validate at construction time; misuse becomes a compile error. | Forces every key-bearing struct through the same invariant.  Wrong-length / empty becomes a compile error at every site, not a runtime silent-pass. |
| **C3** | `RoleAPIBase` keypair as constructor arg | Add `CurveKeypair` to the RoleAPIBase ctor signature.  Delete `set_auth()` method.  Replace `auth_client_pubkey()` / `auth_client_seckey()` getters with `keypair() const & → CurveKeypair const&` (or drop entirely if internal-only).  Update producer / consumer / processor role hosts to construct `RoleAPIBase` with the keypair already loaded (vault open must precede ctor).  The HB-1 ordering bug becomes IMPOSSIBLE by construction — a RoleAPIBase instance cannot exist without a valid keypair. | Removes the silent-empty failure mode at the role-host layer.  The "ordering bug" the audit found can no longer happen. |
| **C4** | Delete legacy plaintext factories + rename + migrate tests | Delete `ZmqQueue::pull_from()` and `push_to()`.  Rename `pull_from_with_auth` → `pull_from`, `push_to_with_auth` → `push_to`.  Delete `ZmqAuthOptions` struct; pass `CurveKeypair`, optional `Z85PublicKey serverkey`, `zap_domain` as named factory args directly.  Delete `admission_is_enforced()` from the `PeerAdmission` interface.  Migrate the 103 test sites in `tests/`: most pick up the per-test `curve_test_setup.h` fixture; a handful are deleted as "only tested the legacy plaintext path".  Update `role_api_base.cpp:412` (consumer-side pull_from) to use the auth signature with `CurveKeypair` from RoleAPIBase + `Z85PublicKey serverkey` from `producer_peers.front().pubkey_z85`. | Closes the "two factories" fork.  Every test engages real CURVE.  Stale plaintext-as-default docstrings die alongside the code. |
| **C5** | CURVE-engagement test assertions | Add four classes of test that catch "CURVE accidentally off":<br>• **L2 ZmqQueue**: after `push_to` succeeds, query `zmq_getsockopt(ZMQ_MECHANISM)` and assert `== ZMQ_CURVE`.<br>• **L2 RoleAPIBase**: construct with valid keypair → pass; trying to construct without is a compile error (covered automatically by C3) — add a smoke test that verifies ctor signature requires the keypair (e.g. `static_assert(!std::is_constructible_v<RoleAPIBase, RoleHostCore&, std::string, std::string>)`).<br>• **L3 broker**: spin up a CURVE-enforced producer; have a NULL-mech `zmq::socket_t` try to connect to it; assert handshake-failed event observed via socket monitor.<br>• **L4 plh_role**: with a deliberately broken `auth.keyfile` (file missing, empty, or wrong-length contents), the role aborts at startup BEFORE binding any data socket. | Anti-recursion.  Once C1-C4 land, the next time someone forgets CURVE the C5 assertions trip immediately.  This is the test layer of §4.6.5 cleanup that the previous landing deferred. |

### After C5: what HB-1..HB-6 still need

| HB | Fix track | Notes |
|---|---|---|
| HB-1 (set_auth ordering) | Closed by **C3** | Cannot recur once keypair is a ctor arg. |
| HB-2 (ZAP pump on role thread) | New task #161 — folds into **A3** (consumer-side) and a **new producer-side commit** | Per HEP-0036 §7.1 the BRC poll thread is the natural pumper; needs explicit wiring. |
| HB-3 (broker `set_peer_allowlist` call) | The ORIGINAL **D4** scope — folds into A3 | Was always part of D4; the audit just confirmed it's still missing. |
| HB-4 + HB-5 (Authorized state + outer-loop guard) | New task #162 — separate commit after A3 | Touches HEP-CORE-0023 sibling-HEP update too (task #104 §14.3). |
| HB-6 (shm_secret generation) | New task #163 — separate commit; depends on **Phase G** | Broker generates per-channel random uint64; CONSUMER_REG_ACK carries it; SHM consumer applies as guard. |

### Items NOT on this cleanup chain (don't sequence in by mistake)

- #75 HUB_TARGETED_ACK, #76 script reload, #77 Tier 2 callbacks, #94
  ephemeral binding, #105 federation, #155 phase 3, #120 Windows ACL,
  #152 RoleIdentityPolicy delete — all independent.
- A1 (commit `164b805c`) stays as the wire-shape schema even though
  the producer_peers field is unused until A3.

### Memory rule to add

**"Audit stale silent-fallback patterns whenever a contract changes."**
Whenever code contains `if (X.empty()) return /* skip security */` or
similar early-return-on-missing-state, the pattern is a contract
violation candidate, not a clean default.  Before respecting it,
audit whether it survives the current HEP contract.  This commit
chain (C1-C5) is the precedent for what the cleanup looks like.

---

## Critical-path execution plan — #103 closes data-plane CURVE

**2026-06-05 REFRAME (post-audit):** the original A1/A2/A3 plan
assumed the existing `*_with_auth` factories would do the CURVE
wiring once called with non-empty keys.  The audit (see "CRITICAL —
A1+A2 silently shipped zero CURVE coverage" above) shows that
assumption was wrong: the silent-fallback in `validate_auth_options`
+ the `set_auth` ordering bug means A2 (commit `badfaed1`) shipped
zero CURVE coverage and 1998/1998 ctest didn't catch it.  Strict-mode
cleanup (C1-C5) **must run before A3 can meaningfully ship**.

### Updated commit chain (depends-on order)

| # | Commit | Status | Folded-in HBs |
|---|---|---|---|
| **A1** (`164b805c`) | Schema field `RxQueueOptions::producer_peers` + `ProducerPeer` struct | shipped — keeps as-is | — |
| **A2** (`badfaed1`) | `add_producer_peer` / `remove_producer_peer` interface + producer-side `push_to_with_auth` call site + `producer_peers[0]` consumed by `build_rx_queue` | shipped **but no-op due to silent fallback** | (uncovered HB-1, HB-2, HB-3 — see above) |
| **C1** | strict-mode `validate_auth_options` + delete the empty-skip conditionals in `ZmqQueue` impl | next | — |
| **C2** | `CurveKeypair` + `Z85PublicKey` strong types | after C1 | — |
| **C3** | `RoleAPIBase` keypair as ctor arg; delete `set_auth` | after C2 | closes **HB-1** by construction |
| **C4** | Delete legacy `pull_from`/`push_to`; rename `_with_auth` → bare; delete `ZmqAuthOptions` + `admission_is_enforced`; migrate 103 test sites | after C3 | — |
| **C5** | CURVE-engagement test assertions (L2 / L3 / L4) | after C4 | — |
| **A3** | **D5** `CONSUMER_REG_ACK.producers[]` emission + **D4** BRC notify dispatch + `set_peer_allowlist` wire from broker + consumer-side switch to authed factory + B1 (`awaiting_endpoint`) + B2 (`zmq_msg_gets("User-Id")`) | after C5 | closes **HB-3** (D4 IS this); contributes to **HB-2** (consumer-side pump) |
| **HB-2 producer-side pump** | Wire `ZapRouter::pump_one` on the BRC poll thread for the producer's data ROUTER per HEP-0036 §7.1 | after A3 | closes **HB-2** |
| **HB-4+5** | Add `RegistrationState::Authorized` + transitions + `any_presence_authorized()` + data-loop outer guard | after A3; touches HEP-CORE-0023 sibling-HEP per task #104 §14.3 | closes **HB-4**, **HB-5** |
| **HB-6** | Broker generates per-channel random `shm_secret`; CONSUMER_REG_ACK carries it; SHM consumer applies as guard | independent / Phase G | closes **HB-6** |

After **A3 + HB-2 + HB-4+5** land, the data-plane CURVE auth gate is
closed in code AND verified by tests.  **HB-6** closes the SHM gate.

### Parallel work (no dependency on #103)

- **#102** — HEP-CORE-0035 §4.7 runtime key handling (mlock +
  `disable_core_dumps()` + `SecureKeyBuffer` libsodium wrapper).
  Independent commit; ship anytime.

### After A3 (sequential)

1. **#104** sibling HEP doc sync — 7 of 8 sibling HEPs are pure doc
   edits (HEP-0017 §3.3 documents the shipped API; HEP-0021 §16
   pubkey REQUIRED text; HEP-0027/0030/0007 record the wire-version
   transition; HEP-0033 §G cross-references `ChannelAccessEntry`).
   HEP-0023 needs ~10 LOC code addition for the `Authorized` FSM
   state on `role_presence.hpp`.
2. **#154** L3 broker test revival — unmask the 7 worker files
   masked under task #153; per-file commit with mutation-sweep on
   each restored TEST_F.  Closes D6.
3. **D7** L4 end-to-end — dual-hub auth-gated data flow under demo
   framework.  Closes Phase D.

### Items NOT on this critical path (do not sequence into auth)

- **#75** HUB_TARGETED_ACK — scope ambiguous; no HEP section, no
  tech_draft.  Needs design-first work.
- **#76** Script reload — independent feature; tech_draft exists,
  HEP not yet numbered.
- **#77** Tier 2 dynamic callbacks — independent feature.
- **#94** HEP-0021 §16.5 ephemeral binding — paired with #103 per
  §14.1 wire-shape coupling but the production-caller wiring is
  about multi-hub processor, not auth gating.  Can land in the A2
  or A3 commit as a §14.1 deliverable but doesn't itself gate the
  goal.
- **#105** Federation HEP-0037 — explicitly post-MVP per HEP-0036
  §13.1.
- **#155** Phase 3 (`--init` one-shot bundling) — CLI UX, auth-
  adjacent but not auth-gating.  Phases 1+2 shipped per commits
  `3215e5aa` and `c684776a`.
- **#120** Windows §4.6 hardening — compliance gap; independent.
- **#152** Delete legacy `RoleIdentityPolicy` — hygiene; independent.

---

## Deferred decisions (each tied to its phase)

| # | Decision | Affects | Tentative direction |
|---|---|---|---|
| P-InboxQueue | InboxQueue admission policy location | Phase E | InboxQueue implements `PeerAdmission` directly, no queue inheritance — preserves REQ/REP nature |
| P-Admin | AdminService — CURVE-wrap or loopback-only? | Phase E (task #127) | Hard loopback-only enforce (refuse non-loopback bind) for v1; CURVE-wrap is HEP-CORE-0035 §5 future work |
| P-SHM-Identity | What is a PeerIdentity for SHM? | Phase G (task #129) | Broker-issued `shm_secret` primary; optional uid guard if operator sets it; broker controls the gate via secret issuance |
| P-Demos | How existing demos migrate | Phase H (task #130) | Transitional `--allow-anonymous-data` flag, gated to refuse-bind on non-loopback endpoints; demos updated incrementally |
| P-HEP | When to sync HEPs vs hold tech_draft | Close-out (task #131) | Tech_draft was archived 2026-06-02; HEPs are now the authoritative source.  No further sync needed unless H surfaces gaps |

---

## Parallel / adjacent tracks

These have their own task IDs but touch the same surface:

- **Task #102** — HEP-CORE-0035 §4.7 runtime key handling (mlock + no-core-dump + zeroing, cross-platform).
- **Task #103** — HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation: `RxQueueOptions::producer_peers` + ZmqQueue dynamic peer API.  **Blocks D4 + D5.**
- **Task #104** — Sibling HEP updates: schema / FSM / CURVE-wiring per HEP-CORE-0036 §14.  This IS the "auth contract in design documents" deliverable.
- **Task #105** — Federation protocol design + cross-hub reg/comm verification.
- **Task #106** — HEP-CORE-0038 + impl: script-accessible vault keystore (`api.vault_save/load`).
- **Task #120** — Windows pathway hardening for HEP-CORE-0035 §4.6 floor.
- **Task #154** — Re-create L3 broker tests against the refactored lib code.  **Closes D6.**
