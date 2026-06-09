# Authentication / PeerAdmission TODO

**Scope:** Open items for HEP-CORE-0035 (Hub-Role auth + federation
trust) and the PeerAdmission feature track that closes out the
data-channel CURVE auth gate.

**Authoritative design lives in:**
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — Layer-1 ZAP + Layer-2 federation trust gate + key-file ACL discipline (§4.6) + runtime key handling (§4.7).
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md` — Three-tier auth (transport / identity / authorization), `ChannelAccessIndex` (§4.1), channel-auth notify+pull wire (§6.5 — `CHANNEL_AUTH_CHANGED_NOTIFY` + `GET_CHANNEL_AUTH_REQ`/`_ACK`, amended 2026-06-04), per-producer pubkey + endpoint (§6.4), **one pubkey per role uid (separation of duties) §I10 — added 2026-06-08, enforced in `KnownRolesStore` with RELEASE-always-on + DEBUG-WITH_TEST bypass**.
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
- **L2 RoleAPIBase**: constructor PRE-CHECKS `key_store().has(kRoleIdentityName)` (HEP-CORE-0040 round-5 use-not-export — keys live in KeyStore, not in the ctor signature); absence is a loud throw, not silent fallback.
- **L3 broker**: spin up a CURVE-enforced producer; a NULL-mech client cannot connect (handshake-failed monitor event observed).
- **L4 plh_role**: with deliberately empty / wrong `auth.keyfile` path, the role aborts at startup BEFORE binding any data socket.

These four assertions are tracked as part of cleanup commit C5 below.

---

## 2026-06-05 PM REFRAME — HEP-CORE-0040 (Locked Key Memory) absorbs the storage half of C3 + #102

Mid-session discussion on the C3 in-flight changes surfaced that the
"keypair as ctor arg" shape (value-copy into `RoleAPIBase`) creates a
**second copy of the seckey** in process memory.  Under §4.7 mlock /
zero-on-destruct that means a second mlock region + second zero path
to get right — and a hub-side analog (`BrokerService::Config` already
value-copies from `HubConfig::auth()`) makes it a third copy.

**Decision**: storage and API design are separate concerns.  Storage
is **one owner per process, in locked memory**.  Round-5 refinement
(2026-06-06): the security module exposes **OPERATIONS** on secret
material, not byte exports.  Public-half keys returned as
`std::string_view`; secret-half keys accessed only via
`with_seckey(name, callback)` — bytes never leave the LockedKey
region.  RoleAPIBase + HubAPI lose the `auth_client_seckey()`
accessor entirely (no legitimate caller); BrokerService bind / BRC
connect / ZmqQueue factories apply the seckey on-site via
`key_store().with_seckey(name, cb)` at the libzmq use point.  No
keypair threading through ctors, no keypair fields in Config, no
keypair in BrokerService::Config.  HEP-CORE-0035 §4.7's flat-utility
sketch (`SecureKeyBuffer` + `disable_core_dumps()`) is lifted into a
new framework primitive HEP — **HEP-CORE-0040 (Locked Key Memory)** —
which defines:

- a **STATIC** `SecureMemorySubsystem` lifecycle module (process-init:
  `setrlimit(RLIMIT_CORE,0)` / `prctl(PR_SET_DUMPABLE,0)` /
  `SetErrorMode` / RLIMIT_MEMLOCK inspection / Windows
  `SeLockMemoryPrivilege`).  Registered at `main()` BEFORE vault open.
- a **DYNAMIC** `KeyStore` lifecycle module (one per process; mirrors
  ThreadManager auto-register pattern) owning N `LockedKey` RAII
  instances backed by `sodium_malloc` (mlock + guard pages + canary +
  auto-wipe).  Use-not-export API: `pubkey(name) → std::string_view`
  (non-secret view into LockedKey bytes) and `with_seckey(name, cb)`
  (callback-scoped seckey access; bytes never leave LockedKey
  region).  `lookup_raw(name) → span` for HEP-0038 scripted secrets.
  All reads take shared lock (parallel); add/remove take exclusive
  lock.  Canonical names: `"hub_identity"` (hub process),
  `"role_identity"` (role process), `"vault:<script-name>"`
  (HEP-0038 scripted secrets).

Consumers:
- HEP-CORE-0035 §4.7 (identity keypair storage) — §4.7 becomes a
  one-line cross-ref; framework primitives move into HEP-0040.
- HEP-CORE-0038 / task #106 (script vault keystore) — runtime-saved
  scripted secrets land in the same dynamic KeyStore.

Task tracking (created 2026-06-05):
- **#165** draft HEP-0040 in tech_draft (✅ landed
  `docs/tech_draft/HEP-CORE-0040-Locked-Key-Memory-DRAFT.md`)
- **#166** fresh-eye review of draft
- **#167** promote tech_draft → `docs/HEP/`
- **#168** HEP-0035 §4.7 cross-reference update
- **#169** impl: `SecureMemorySubsystem` static module
- **#170** impl: `KeyStore` dynamic module + `LockedKey` RAII
- **#171** impl: migrate HubConfig + RoleConfig to LockedKey storage
- **#172** impl: BrokerService::Config to reference pattern (drops
  the value-copy at `hub_host.cpp:183-184`)
- **#173** impl: RoleAPIBase loses `auth_client_pubkey()` /
  `auth_client_seckey()` entirely; ctor signature UNCHANGED;
  `set_auth` + 3 role-host call sites deleted;
  `Impl::auth_client_pubkey_/_seckey_` strings deleted; use sites
  migrate to `key_store().with_seckey` / `pubkey` at the libzmq
  socket-option point (per round-5 use-not-export design).
  **Supersedes the in-flight #159 value-copy C3** (which was reverted
  2026-06-05) AND the round-4 `lookup() → CurveKeypair&` plan.
- **#174** impl: HubAPI does NOT gain accessors (round-5 deletes the
  symmetric-accessor plan — no legitimate caller exists; tracing every
  reader of seckey shows they're all libzmq socket-option setters that
  call `with_seckey` directly).
- **#102** (HEP-0035 §4.7 utility-only) — SUPERSEDED; blocked on
  #167–#171; closes when the chain lands.
- **#159** (original C3) — SUPERSEDED; blocked on #173; closes when
  #173 lands.
- **#106** (HEP-0038 script vault keystore) — gains blockedBy on
  #167 + #170 so the storage layer exists before consumer impl.

**In-flight state**: working tree has the WRONG-DIRECTION C3 changes
(value copy of `CurveKeypair` into `RoleAPIBase::Impl`, `if constexpr`
branch in `engine_host.cpp:143`, `set_auth` removals across the three
role hosts).  Decision: do NOT revert.  Leave the tree as-is until
#173 reshapes it to the reference pattern.  The HB-1 ordering fix
concept survives; only the storage shape changes.

**Sequencing impact on C-chain below**: C3 row in the implementation
order table (and §"C1..C5 sequence" rows) now executes per HEP-0040
design — RoleAPIBase + HubAPI accessors query `key_store().lookup` by
canonical name; NO keypair fields in Config or BrokerService::Config;
NO ctor signature changes anywhere.  Other rows (C1, C2, C4, C5) are
unchanged.

**Memory rule that fell out of this**: separate STORAGE design from
API design.  When asked "where should X live", split into (a) where
the bytes live (lowest reasonable level, single owner) and (b) what
API exposes access (grouped logically per consumer).  Conflating the
two pushed the discussion in circles for an hour.

---

## Strict-CURVE cleanup chain — C1..C5 (replaces naive ordering patch)

**STATUS — CLOSED 2026-06-09.**  All five steps shipped:
C1 (#157), C2 (#158), C3 SHIPPED via #173, C4 (#160), C5 (#161
Phase 1–5).  Closed under commits `9f9b3ede`/`47bf6fb6`/`7ff98d60`/`233933eb`/`96469812` + the deferred-followup commit (#186 binding + L3 NULL-mech test).  Phase C
fresh-eye review (2026-06-09) + script-binding + concurrency
audits completed; documentation drift fixed across HEPs 0015/0017/0021/0040.

**Deferred follow-ups addressed in Phase 5 close-out:**
- ✅ **#186 binding gap** — shipped.  `RoleAPIBase::queue_mechanism(ChannelSide)`
  accessor + `hub::mechanism_name()` string conversion + threaded into
  `snapshot_metrics_json()` (Python `api.metrics()`) and Lua
  `api.metrics()`.  L3 worker `ZmqTxNull` pins
  `queue_mechanism(Tx) == Curve` after start + `Uninitialized` after
  close_queues.
- ✅ **L3 NULL-mech handshake-fail test** — shipped.
  `ZmqQueueAuthTest::NullMechClient_HandshakeFails` uses
  `zmq_socket_monitor` on a raw NULL-mech PULL socket connecting to
  a CURVE-enforced producer; asserts a `ZMQ_EVENT_HANDSHAKE_FAILED_*`
  event fires within 3s.  Pins the bidirectional contract that the
  L2 `Mechanism::Curve` invariant + start() guard alone don't
  cover — server CURVE-enforced + client NULL ⇒ no data session.

**Deferred follow-up explicitly left out of Phase C — Demo refresh wave.**
Demos under `share/py-demo-*` ship with `"auth": { "keyfile": "" }` in
24 role configs across 11 demo dirs.  Per the 2026-06-09 audit:
1. The breakage PRE-DATES Phase C — B3 (#78) made empty `auth.keyfile`
   a config-load hard error in early May 2026.  No demo has been
   refreshed since.
2. No demo file constructs `ZmqQueue` directly; all transport
   construction goes through `RoleAPIBase::build_tx_queue` /
   `build_rx_queue`, which were correctly migrated to the post-C4
   CURVE-only factories.  Phase C did NOT introduce any new demo
   breakage.
3. Fixing the demos requires structural changes orthogonal to the
   C-chain: provisioning per-role vaults via `plh_role --keygen` in
   each `demo.manifest.json` `setup.commands` array, populating
   `known_roles` in each `hub.json` with the resulting pubkeys, and
   end-to-end re-validation of each demo flow.  That's a coordinated
   refresh wave that belongs alongside #79 (B4 plh_role --init SHM
   secret) and #155 (CLI --init bundling), not as a Phase C close-out.
Tracking: keep as a known-broken state until the next demo refresh
wave; #79 + #155 are the right home.

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

D. **`ZmqAuthOptions` overdesigned options bag**.  5 stringly-typed fields, each independently nullable.  Dies in C2 — replaced per HEP-CORE-0040 §8.4 by `identity_key_name` (the KeyStore name STRING) + optional `Z85PublicKey serverkey` + `zap_domain` as named factory args.  No `CurveKeypair` parameter survives on the public factory API; keys live only in `key_store()` and are looked up on-site via `with_seckey(name, cb)` + `pubkey(name)`.

E. **`RoleAPIBase` auth surface — three overdesigned shapes** (all CLOSED by #173 round-5):
   - `set_auth(client_pubkey, client_seckey)` setter — deleted #173.
   - `auth_client_pubkey()` / `auth_client_seckey()` getters — deleted #173 (round-5 use-not-export; HubAPI doesn't gain symmetric accessors either).
   - `pImpl->auth_client_pubkey/_seckey` storage — deleted #173; replaced by on-site `key_store().with_seckey(kRoleIdentityName, cb)` at libzmq use sites.

F. **`admission_is_enforced()` interface method** is dead after C1 (always true).  Deleted from the `PeerAdmission` interface in C4.

G. **103 test sites in `tests/` call legacy plaintext factories** (grep `ZmqQueue::pull_from\b\|ZmqQueue::push_to\b`).  Migration burden in C4 — most use `curve_test_setup.h`; some delete-only.

H. **Stale documentation** (`role_api_base.hpp:55-58`, `plh_datahub.hpp` cross-refs, hub_shm_queue.hpp:60) references the plaintext factories as canonical examples.  Updated in C4.

I. **`hub_zmq_queue.cpp:617` "bind side: serverkey is meaningless. Don't fail if the caller set it by mistake — silently ignored"** — same silent-ignore anti-pattern.  Make it reject in C1.

### C1..C5 sequence (target design per commit)

| # | Commit | Scope | Why this order |
|---|---|---|---|
| **C1** | `validate_auth_options` strict-mode | Delete the "all-empty = OK" early-return at `hub_zmq_queue.cpp:587-589`.  Require both keypair fields non-empty + 40-char Z85.  Delete the silent-ignore at line 617.  Delete the "if empty skip CURVE" conditional at line 875.  Delete the "if empty skip serverkey" conditional at line 930.  Update `validate_auth_options` L2 tests for the new contract (rejection cases, not acceptance). | Smallest defensive change.  After this, every existing call site that relied on the silent path FAILS LOUDLY at the factory boundary.  All subsequent commits surface their dependents through real compile/link/run errors instead of silent miscompiles. |
| **C2** | Strong-type non-keypair pubkeys + endpoint shape per HEP-0040 §8.4 | **Note**: original C2 ("replace `my_pubkey_z85` / `my_seckey_z85` with `CurveKeypair` in `ZmqAuthOptions`, `AuthConfig`, `BrokerRequestComm::Config`, `RoleAPIBase::pImpl`, `HubHost::Config`") is OBSOLETE — those fields were DELETED by #171-#173 (HEP-CORE-0040 round-5 use-not-export design).  The structs no longer carry a keypair at all; keys live only in `key_store()`.  C2 now means: (a) define `Z85PublicKey` strong type (40-char Z85 invariant) for the remaining pubkey-only fields (`ProducerPeer::pubkey_z85`, `ProducerEntry::zmq_pubkey`, `ConsumerEntry::zmq_pubkey`, `BrokerRequestComm::Config::broker_pubkey`, BRC's `serverkey`); (b) align ZmqQueue factories to HEP-CORE-0040 §8.4 endpoint shape — `push_to(endpoint, identity_key_name, zap_domain)` and `pull_from(endpoint, Z85PublicKey server_pubkey, identity_key_name)`; (c) kill the `ZmqAuthOptions` options bag (its fields become factory parameters).  No `CurveKeypair` parameter on the public API (keys live in KeyStore). | Forces remaining pubkey fields through one invariant; eliminates `ZmqAuthOptions` "all-empty = OK" residue at the type level; matches HEP-CORE-0040 §8.4 + §8.6 exactly. |
| **C3** | RoleAPIBase + HubAPI lose the seckey accessor entirely; on-site `with_seckey` at libzmq use points (per HEP-CORE-0040 §8.2 round-5) | RoleAPIBase ctor signature UNCHANGED.  DELETE: `set_auth()` method + three role-host call sites; `Impl::auth_client_pubkey_` / `_seckey_` string members; `auth_client_pubkey()` accessor; `auth_client_seckey()` accessor; `CurveKeypair::empty()` method.  HubAPI does NOT gain symmetric accessors (round-5 deletes the symmetric-accessor plan — no legitimate caller).  Sites that previously read these (role_handler BRC connect, build_tx_queue, build_rx_queue) migrate to `key_store().with_seckey(name, cb)` at the actual libzmq socket-option site.  No `if constexpr` in EngineHost.  No keypair fields in HubConfig / RoleConfig / BrokerService::Config / BRC::Config.  HB-1 closes because `with_seckey` / `pubkey` throw std::out_of_range on missing key — loud, not silent.  | Zero unlocked seckey copies in pylabhub source.  Symmetric absence (neither RoleAPIBase nor HubAPI exposes the seckey as data).  No threading of keypair through ctor chains. |
| **C4** | Delete legacy plaintext factories + adopt HEP-0040 §8.4 endpoint shape + migrate tests | Delete `ZmqQueue::pull_from()` and `push_to()` (plaintext variants).  Rename `pull_from_with_auth` → `pull_from`, `push_to_with_auth` → `push_to`.  Final signatures per HEP-CORE-0040 §8.4: `push_to(endpoint, identity_key_name = kRoleIdentityName, zap_domain = "")` and `pull_from(endpoint, Z85PublicKey server_pubkey, identity_key_name = kRoleIdentityName)` — name lookups via `key_store().with_seckey(name, cb)` + `key_store().pubkey(name)` inside the factory body; no keypair parameter on the public API.  Delete `ZmqAuthOptions` struct entirely (its fields become factory parameters).  Delete `admission_is_enforced()` from the `PeerAdmission` interface.  Migrate the 103 test sites in `tests/`: most pick up `CurveKeyStoreFixture` from `curve_test_setup.h` (which seeds KeyStore); a handful are deleted as "only tested the legacy plaintext path".  Update `role_api_base.cpp:412` (consumer-side pull_from) to pass `kRoleIdentityName` + `Z85PublicKey serverkey` from `producer_peers.front().pubkey_z85`. | Closes the "two factories" fork.  Every test engages real CURVE.  Single point where (name → bytes) lookup happens (`key_store()`).  Matches HEP-CORE-0040 §8.4 + §8.6 exactly. |
| **C5** | CURVE-engagement test assertions + `Mechanism` invariant + start() guard | Added the `Mechanism` enum (`Uninitialized`/`Plaintext`/`Curve`) on `ZmqQueue` (HEP-CORE-0035 §2 enforcement point), a thread-safe atomic `mechanism_` field on `ZmqQueueImpl`, a public const `ZmqQueue::mechanism()` accessor, and a hard guard inside `start()` that queries libzmq via `zmq_getsockopt(ZMQ_MECHANISM)`; if the answer is not `ZMQ_CURVE` the start fails.  Anti-recursion test coverage across four tiers:<br>• **L2 ZmqQueue invariant** ✅ `Mechanism_BeforeStart_IsUninitialized` + `Mechanism_AfterPushBind_IsCurve` — pins the observable contract; `Mechanism::Plaintext` is structurally unreachable now that the guard is in place.<br>• **L2 RoleAPIBase static_assert** ✅ Three compile-time pins on the ctor signature: constructible with (RoleHostCore&, role_tag, uid), NOT default-constructible, NOT constructible with (RoleHostCore&) alone.  Catches any regression that adds default args or a `set_auth` accessor.<br>• **L2 loader contract** ✅ `LoadKeypair_RejectsCorruptVaultContents` — vault present with correct 0600 ACL but garbage contents: `load_keypair` throws AND `key_store()` for `kRoleIdentityName` is left untouched.  Pins the "no partial state on failure" gate.<br>• **L4 gate enforcement** ✅ `CurveGate_CorruptVault_AbortsBeforeBind` (all 3 roles) — same corruption applied to a real `plh_role` binary; `--validate` exits non-zero before any socket-bind code runs.  Mirrors the L2 loader contract at the binary tier.<br>• **L3 NULL-mech handshake-fail** ⏳ DEFERRED — would require building socket-monitor test infrastructure (none in repo today) just to pin a property the L2 `Mechanism::Curve` invariant + the start() guard + the keystore-validator chain together structurally guarantee.  Tracked as future work if defense-in-depth becomes warranted. | shipped (#161 Phase 1–4).  L3 NULL-mech handshake-fail explicitly deferred — see entry. |

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

### Implementation order (to keep ctest green throughout the chain)

Doc-review 2026-06-05 caught a sequencing issue: C1 (strict
`validate_auth_options`) alone would break ctest because the
ordering bug (HB-1) means `build_tx_queue` passes empty keys at run
time.  Practical order:

1. **C0 — HB-1 ordering fix via HEP-CORE-0040 use-not-export KeyStore**
   (round-5 2026-06-06: BrokerService bind / BRC connect / ZmqQueue
   factory call `key_store().with_seckey(name, cb)` + `pubkey(name)` at
   the libzmq socket-option site; RoleAPIBase + HubAPI lose `auth_client_seckey()`
   entirely; `set_auth` setter deleted; NO keypair passed through any
   ctor; NO keypair fields in Config or BrokerService::Config; NO `lookup() → CurveKeypair&` accessor on KeyStore).  Lands as part of
   the HEP-0040 impl chain (tasks #167–#175).  Earlier "ctor takes
   value", "ctor takes const ref", and "lookup() returns CurveKeypair&"
   plans all REJECTED — the first two threaded keypair through ctors,
   the third forced a second std::string copy via Entry::view cache.
   After C0, any read of an identity key before `key_store().add_identity`
   has been called throws std::out_of_range → loud, not silent.  No
   latent consumer-side plaintext fallback survives the migration (it
   cannot compile against the new use-not-export API).
2. **C1 — strict `validate_auth_options`**.  Safe now that HB-1 is
   closed; the strict-mode rejection cannot fire on production
   paths.  L2 tests for `validate_auth_options` flip from acceptance
   to rejection cases.
3. **C2 — strong-type pubkeys + endpoint shape** (`Z85PublicKey` for
   `ProducerPeer::pubkey_z85` / `ProducerEntry::zmq_pubkey` /
   `ConsumerEntry::zmq_pubkey` / `BRC::Config::broker_pubkey`; ZmqQueue
   factories adopt HEP-CORE-0040 §8.4 endpoint shape — `push_to(endpoint,
   identity_key_name, zap_domain)` and `pull_from(endpoint,
   Z85PublicKey server_pubkey, identity_key_name)`; `ZmqAuthOptions`
   dies in this step too).  Pure refactor — keys still live only in
   KeyStore.  **NOT a CurveKeypair-into-struct rewrite** — that
   pre-round-5 plan is OBSOLETE per HEP-CORE-0040 §8.6.
4. **C3b — finalize**: SHIPPED via #173 (deleted `set_auth`,
   `auth_client_pubkey()`, `auth_client_seckey()` + storage entirely;
   round-5 use-not-export, no `CurveKeypair const&` accessor added).
5. **C4 — delete legacy factories** + rename + migrate 103 test sites
   (factories take `identity_key_name` STRING per HEP §8.4, not
   keypair).
6. **C5 — asymmetric assertions** + unmask any tests masked in C0.

Tasks #157-#161 already exist for C1-C5; C0 absorbed into #159
(the original "C3" task) since it's the same structural change.

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
| **C1** | strict-mode `validate_auth_options` + delete the empty-skip conditionals in `ZmqQueue` impl | shipped (#157) | — |
| **C2** | `Z85PublicKey` strong type (pubkey-only fields) + HEP-0040 §8.4 endpoint shape on ZmqQueue factories + delete `ZmqAuthOptions` | shipped (#158) | — |
| **C3** | SHIPPED via #173 (round-5 use-not-export — `set_auth` + accessors deleted entirely; keys via `key_store().with_seckey`) | — | closed **HB-1** by construction |
| **C4** | Delete legacy `pull_from`/`push_to`; rename `_with_auth` → bare; final signatures take `identity_key_name` per HEP §8.4; migrate 103 test sites + flip 13 roundtrip tests to production-mirror orientation (PUSH+bind / PULL+connect with serverkey).  `admission_is_enforced` was already deleted in close-out commit 1.  Test fixture: `ZmqQueueTestEnvironment` seeds `kRoleIdentityName` once; per-test `ZapPumpThread` services CURVE handshakes; `seed_self_allowlist(*push)` on the bind side after start. | shipped (#160) | — |
| **C5** | CURVE-engagement test assertions (L2 / L3 / L4) | shipped (#161 Phase 1–4) | — |
| **A3** | **D5** `CONSUMER_REG_ACK.producers[]` emission + **D4** BRC notify dispatch + `set_peer_allowlist` wire from broker + consumer-side switch to authed factory + B1 (`awaiting_endpoint`) + B2 (`zmq_msg_gets("User-Id")`) | after C5 | closes **HB-3** (D4 IS this); contributes to **HB-2** (consumer-side pump) |
| **HB-2 producer-side pump** | Wire `ZapRouter::pump_one` on the BRC poll thread for the producer's data ROUTER per HEP-0036 §7.1 | after A3 | closes **HB-2** |
| **HB-4+5** | Add `RegistrationState::Authorized` + transitions + `any_presence_authorized()` + data-loop outer guard | after A3; touches HEP-CORE-0023 sibling-HEP per task #104 §14.3 | closes **HB-4**, **HB-5** |
| **HB-6** | Broker generates per-channel random `shm_secret`; CONSUMER_REG_ACK carries it; SHM consumer applies as guard | independent / Phase G | closes **HB-6** |

After **A3 + HB-2 + HB-4+5** land, the data-plane CURVE auth gate is
closed in code AND verified by tests.  **HB-6** closes the SHM gate.

### Parallel work (no dependency on #103)

- **#102** — HEP-CORE-0035 §4.7 runtime key handling.  **SUPERSEDED
  2026-06-05 by the HEP-CORE-0040 chain (tasks #167–#174)**.  The
  flat-utility plan (`SecureKeyBuffer` + `disable_core_dumps()`) is
  lifted into a registered lifecycle subsystem; see the
  "2026-06-05 PM REFRAME" section above and the HEP-0040 draft at
  `docs/tech_draft/HEP-CORE-0040-Locked-Key-Memory-DRAFT.md`.  #102
  stays open as the tracker until #167–#171 land, then closes.

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
