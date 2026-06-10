# AUTH_TODO completions archive — 2026-06-09

This file preserves the verbatim prose for AUTH_TODO entries that were
verified shipped in code as of 2026-06-09.  Companion archive to
`docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
(Phase A/B/C; D1+D2+D3; §4.6.5 no-bypass cleanup; BRC monitor
investigation; lib-stabilization exclusion procedure; resolved
decisions; considered-but-not-pursued).

Source file: `docs/todo/AUTH_TODO.md`.

This archive covers:

- The 2026-06-05 silent-fallback audit (HB-1 .. HB-6 findings) — the
  triage doc that surfaced the C-chain.
- The 2026-06-05 PM REFRAME — HEP-CORE-0040 absorption of the storage
  half of C3 + #102.
- The Strict-CURVE cleanup chain C1..C5 — fallback-era residue
  inventory + per-commit scope table + implementation order.
- The Phase 5 deferred follow-ups closed in the C5 close-out.
- HEP-CORE-0040 (Locked Key Memory) impl chain — #165–#176 + #187.
- Phase D close-out follow-ons that landed alongside the C-chain.

No content summarized; full prose retained for context.

---

## 2026-06-05 audit — A1+A2 silently shipped zero CURVE coverage

This section records the verified findings from the 2026-06-05 audit so
they survive context resets per CLAUDE.md §"Session hygiene".  Each
HARD-BLOCK below was confirmed against actual code (grep + file
read) — NOT against commit messages or docstrings.

### How the silent-fallback hole opened

PeerAdmission **Phase C** (commits `62bda863..47aa0374`, see earlier
archive) introduced the auth-enabled ZmqQueue factories alongside the
legacy plaintext factories.  The migration design accepted a "bridge
state":

> *Empty defaults mean plaintext — every field empty produces a
> socket configured exactly like the legacy `pull_from`/`push_to`
> factories.* (`hub_zmq_queue.hpp:144-146`, removed in C-chain)

The bridge was supposed to be torn down when HEP-CORE-0035 **§4.6.5
"no-bypass discipline"** landed (2026-06-04, commit `3a64e58c`).  That
landing did rip out the `use_curve` field, the `enforce_ctrl_admission`
field, and the legacy `RoleIdentityPolicy` machinery (3-revised slices
A through E).  **It did NOT delete the legacy `ZmqQueue::pull_from` /
`push_to` factories or the `ZmqAuthOptions` "all-empty = plaintext"
carve-out.**

Result: any code path that asks for `push_to_with_auth` with empty
keypair fields silently falls back to plaintext — same behaviour as
the legacy factory.  The validator at `hub_zmq_queue.cpp:587-589`
(pre-C1) explicitly endorses this:

```cpp
// All empty: legacy plaintext path, accepted by either side.
if (!has_pub && !has_sec && opts.serverkey_z85.empty())
    return {};   // ← validator says "OK"
```

That branch is the contract violation.  It is the design that needed
to die before anything else in the data-plane CURVE chain shipped.

### Verified HARD-BLOCKS (2026-06-05)

Numbered HB-1 .. HB-6.  Each cites code; each had to be fixed before
the data-plane CURVE contract could be claimed live.

**HB-1.  `build_tx_queue` reads `auth_client_pubkey` BEFORE `set_auth`
runs.**  In `producer_role_host.cpp:223` `setup_infrastructure_`
internally calls `build_tx_queue`; `set_auth(...)` was at line 313 —
90 lines later.  Same ordering in `consumer_role_host.cpp:214 → 289`
and `processor_role_host.cpp:245 → 346`.  Net effect: the
`push_to_with_auth` factory ALWAYS saw `pImpl->auth_client_pubkey
== ""`.  Combined with the silent-fallback in `validate_auth_options`,
the producer's PUSH socket bound plaintext.  A2 (commit `badfaed1`)
appeared to have shipped CURVE but actually achieved zero CURVE
coverage.  The 1998/1998 ctest passed because every code path took
the plaintext branch.

**HB-2.  `ZapRouter::pump_one` is never called from any role-side poll
thread.**  Verified by grep across `src/`: only two call sites existed —
`broker_service.cpp:811` (broker poll thread) and `zap_router.cpp:504`
(ZapRouter's own optional poll-loop helper).  HEP-0036 §7.1 lines
1411-1436 explicitly require the producer-side data ROUTER's ZAP
handler to be pumped from a poll thread on the role process.  If HB-1
were fixed, every consumer CURVE handshake would hang indefinitely
waiting for a ZAP REP.

**HB-3.  `set_peer_allowlist` has zero callers.**  Verified by grep
across `src/`: interface declared in `peer_admission.hpp:180`, impl at
`hub_zmq_queue.cpp:719`.  No production code anywhere called it for
the producer's PUSH queue.  So even if HB-1 + HB-2 were fixed, the
producer's allowlist would remain the empty `initial_allowlist`
forever → every consumer's pubkey denied at the ZAP cache → no data
flows.  This is the missing **D4** wiring (broker
`CHANNEL_AUTH_CHANGED_NOTIFY` → producer pull → `set_peer_allowlist`).

**HB-4.  `RegistrationState::Authorized` does not exist.**  Verified
at `role_presence.hpp:95-101`: enum has only `Unregistered /
RegRequestPending / Registered / Deregistered`.  HEP-CORE-0036 §14.3 +
§8 require an `Authorized` value plus the transitions into it.
Currently nothing in the role-side FSM bridges control to data.

**HB-5.  `any_presence_authorized()` + data-loop outer guard do not
exist.**  `data_loop.hpp:129-131` outer guard:

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

HEP-CORE-0036 §5.6 specifies the broker generates a per-channel random
`uint64`.  `CONSUMER_REG_ACK` also doesn't carry `shm_secret`.  SHM
data plane has zero authorization today — any process that knows the
channel name attaches successfully.

### Test blind spots (why 1998/1998 ctest didn't catch any of this)

No test in the suite asserted that CURVE was actually engaged on a
producer's socket.  Every "auth" test asserted a symmetric property:

| Existing test | What it asserts | Why it passes with CURVE off |
|---|---|---|
| `ZmqQueueAuthTest.AllowedPeer_DeliversRoundTrip` | data flows when both sides authed | also passes when both sides plaintext |
| `ZmqQueueAuthTest.UnallowedPeer_BlockedFromDelivery` | wrong pubkey denied | only meaningful if CURVE engaged to begin with |
| `ZmqQueueAuthTest.AdmissionIsEnforced_Lifecycle` | `admission_is_enforced()` true after start | `admission_is_enforced()` is itself silent-false on empty keys (`hub_zmq_queue.cpp:770`) |
| `test_layer4_plh_role` demos | end-to-end data flow | both sides plaintext = data flows = test passes |

The assertions that would have caught the no-op (all shipped in C5):
- **L2 ZmqQueue**: after `push_to_with_auth(...)` succeeds,
  `zmq_getsockopt(socket, ZMQ_MECHANISM, ...)` returns `ZMQ_CURVE`,
  not `ZMQ_NULL`.
- **L2 RoleAPIBase**: constructor PRE-CHECKS
  `key_store().has(kRoleIdentityName)` (HEP-CORE-0040 round-5
  use-not-export — keys live in KeyStore, not in the ctor signature);
  absence is a loud throw, not silent fallback.
- **L3 broker**: spin up a CURVE-enforced producer; a NULL-mech client
  cannot connect (handshake-failed monitor event observed).
- **L4 plh_role**: with deliberately empty / wrong `auth.keyfile`
  path, the role aborts at startup BEFORE binding any data socket.

---

## 2026-06-05 PM REFRAME — HEP-CORE-0040 absorbs storage half of C3 + #102

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
region.  RoleAPIBase + HubAPI lose the `auth_client_seckey()` accessor
entirely (no legitimate caller); BrokerService bind / BRC connect /
ZmqQueue factories apply the seckey on-site via
`key_store().with_seckey(name, cb)` at the libzmq use point.  No
keypair threading through ctors, no keypair fields in Config, no
keypair in BrokerService::Config.  HEP-CORE-0035 §4.7's flat-utility
sketch (`SecureKeyBuffer` + `disable_core_dumps()`) was lifted into a
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
  (callback-scoped seckey access; bytes never leave LockedKey region).
  `lookup_raw(name) → span` for HEP-0038 scripted secrets.  All reads
  take shared lock (parallel); add/remove take exclusive lock.
  Canonical names: `"hub_identity"` (hub process), `"role_identity"`
  (role process), `"vault:<script-name>"` (HEP-0038 scripted secrets).

Consumers:
- HEP-CORE-0035 §4.7 (identity keypair storage) — §4.7 became a
  one-line cross-ref; framework primitives moved into HEP-0040.
- HEP-CORE-0038 / task #106 (script vault keystore) — runtime-saved
  scripted secrets land in the same dynamic KeyStore.

Task tracking (created 2026-06-05; all closed by 2026-06-09):
- **#165** draft HEP-0040 in tech_draft (✅ landed
  `docs/tech_draft/HEP-CORE-0040-Locked-Key-Memory-DRAFT.md`)
- **#166** fresh-eye review of draft (✅)
- **#167** promote tech_draft → `docs/HEP/` (✅)
- **#168** HEP-0035 §4.7 cross-reference update (✅)
- **#169** impl: `SecureMemorySubsystem` static module (✅)
- **#170** impl: `KeyStore` dynamic module + `LockedKey` RAII (✅)
- **#171** impl: drop keypair fields from HubConfig + RoleConfig
  AuthConfig (✅)
- **#172** impl: drop keypair fields from BrokerService::Config +
  BRC::Config (✅)
- **#173** impl: RoleAPIBase loses `auth_client_pubkey()` /
  `auth_client_seckey()` entirely; ctor signature UNCHANGED;
  `set_auth` + 3 role-host call sites deleted;
  `Impl::auth_client_pubkey_/_seckey_` strings deleted; use sites
  migrate to `key_store().with_seckey` / `pubkey` at the libzmq
  socket-option point (per round-5 use-not-export design).
  **Superseded the in-flight #159 value-copy C3** (which was reverted
  2026-06-05) AND the round-4 `lookup() → CurveKeypair&` plan.
- **#174** impl: HubAPI does NOT gain accessors (round-5 deletes the
  symmetric-accessor plan — no legitimate caller exists; tracing every
  reader of seckey shows they're all libzmq socket-option setters that
  call `with_seckey` directly).
- **#175** vault hardening — `vault_read_secure` + HubVault/RoleVault
  plaintext-retention fix.
- **#176** wire `SecureMemorySubsystem` + `KeyStore` in
  `plh_hub_main` + `plh_role_main`.
- **#187** vault load-path tightening — `get_ref<std::string&>` +
  `sodium_memzero` WipeGuard on json-owned seckey copy (closes
  freed-heap exposure window during `load_keypair`).
- **#102** (HEP-0035 §4.7 utility-only) — SUPERSEDED; closed when
  #167–#171 landed.
- **#159** (original C3) — SUPERSEDED; closed via #173.

**Memory rule that fell out of this**: separate STORAGE design from
API design.  When asked "where should X live", split into (a) where
the bytes live (lowest reasonable level, single owner) and (b) what
API exposes access (grouped logically per consumer).  Conflating the
two pushed the discussion in circles for an hour.

---

## Strict-CURVE cleanup chain — C1..C5 (closed 2026-06-09)

**STATUS — CLOSED 2026-06-09.**  All five steps shipped:
C1 (#157), C2 (#158), C3 SHIPPED via #173, C4 (#160), C5 (#161
Phase 1–5).  Closed under commits
`9f9b3ede`/`47bf6fb6`/`7ff98d60`/`233933eb`/`96469812` + the
deferred-followup commit (#186 binding + L3 NULL-mech test).  Phase C
fresh-eye review (2026-06-09) + script-binding + concurrency audits
completed; documentation drift fixed across HEPs 0015/0017/0021/0040.

**Deferred follow-ups addressed in Phase 5 close-out:**

- ✅ **#186 binding gap** — shipped.
  `RoleAPIBase::queue_mechanism(ChannelSide)` accessor +
  `hub::mechanism_name()` string conversion + threaded into
  `snapshot_metrics_json()` (Python `api.metrics()`) and Lua
  `api.metrics()`.  L3 worker `ZmqTxNull` pins
  `queue_mechanism(Tx) == Curve` after start + `Uninitialized` after
  close_queues.
- ✅ **L3 NULL-mech handshake-fail test** — shipped.
  `ZmqQueueAuthTest::NullMechClient_HandshakeFails` uses
  `zmq_socket_monitor` on a raw NULL-mech PULL socket connecting to a
  CURVE-enforced producer; asserts a
  `ZMQ_EVENT_HANDSHAKE_FAILED_*` event fires within 3s.  Pins the
  bidirectional contract that the L2 `Mechanism::Curve` invariant +
  start() guard alone don't cover — server CURVE-enforced + client
  NULL ⇒ no data session.

**Deferred follow-up explicitly left out of Phase C — Demo refresh wave.**
Demos under `share/py-demo-*` ship with `"auth": { "keyfile": "" }` in
24 role configs across 11 demo dirs.  Per the 2026-06-09 audit:

1. The breakage PRE-DATES Phase C — B3 (#78) made empty
   `auth.keyfile` a config-load hard error in early May 2026.  No demo
   has been refreshed since.
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
   secret) and #155 (CLI --init bundling), not as a Phase C
   close-out.

Tracking: kept as a known-broken state until the next demo refresh
wave; #79 + #155 are the right home.

### Inventory of fallback-era residue (what died / changed per commit)

A. **`validate_auth_options` all-empty branch** (`hub_zmq_queue.cpp:587-589`) — the smoking gun.  Died in C1.

B. **Five `if (empty)` skip-CURVE conditionals in ZmqQueue impl**:

   | Line | What | After strict-mode |
   |---|---|---|
   | `:584-590` | validator returns OK on all-empty | DELETED — empty errors (C1) |
   | `:770` | `admission_is_enforced()` returns false if `my_pubkey_z85.empty()` | method became constant true → deleted from interface (C4) |
   | `:875` | `if (!my_pubkey_z85.empty())` — CURVE-setup block skipped if false | DELETED the conditional; CURVE setup is unconditional (C1) |
   | `:894` | `if (resolved_zap_domain_.empty())` — derive default name | KEPT — that's derivation, not auth bypass |
   | `:930` | `if (serverkey_z85.empty())` — skip `ZMQ_CURVE_SERVERKEY` set on connect side | DELETED — connect side must have serverkey (C1) |

C. **Two parallel factory pairs** (4 functions, now 2):
   - `pull_from()` plaintext → DELETED (C4)
   - `push_to()` plaintext → DELETED (C4)
   - `pull_from_with_auth()` → RENAMED to `pull_from` (C4)
   - `push_to_with_auth()` → RENAMED to `push_to` (C4)

D. **`ZmqAuthOptions` overdesigned options bag**.  5 stringly-typed
   fields, each independently nullable.  Died in C2 — replaced per
   HEP-CORE-0040 §8.4 by `identity_key_name` (the KeyStore name
   STRING) + optional `Z85PublicKey serverkey` + `zap_domain` as named
   factory args.  No `CurveKeypair` parameter survived on the public
   factory API; keys live only in `key_store()` and are looked up
   on-site via `with_seckey(name, cb)` + `pubkey(name)`.

E. **`RoleAPIBase` auth surface — three overdesigned shapes** (all
   CLOSED by #173 round-5):
   - `set_auth(client_pubkey, client_seckey)` setter — deleted #173.
   - `auth_client_pubkey()` / `auth_client_seckey()` getters — deleted
     #173 (round-5 use-not-export; HubAPI doesn't gain symmetric
     accessors either).
   - `pImpl->auth_client_pubkey/_seckey` storage — deleted #173;
     replaced by on-site
     `key_store().with_seckey(kRoleIdentityName, cb)` at libzmq use
     sites.

F. **`admission_is_enforced()` interface method** is dead after C1
   (always true).  Deleted from the `PeerAdmission` interface in C4.

G. **103 test sites in `tests/` call legacy plaintext factories**
   (grep `ZmqQueue::pull_from\b\|ZmqQueue::push_to\b`).  Migration
   burden in C4 — most use `curve_test_setup.h`; some delete-only.

H. **Stale documentation** (`role_api_base.hpp:55-58`, `plh_datahub.hpp`
   cross-refs, hub_shm_queue.hpp:60) referenced the plaintext factories
   as canonical examples.  Updated in C4.

I. **`hub_zmq_queue.cpp:617` "bind side: serverkey is meaningless. Don't
   fail if the caller set it by mistake — silently ignored"** — same
   silent-ignore anti-pattern.  Made it reject in C1.

### C1..C5 sequence (target design per commit)

| # | Commit | Scope |
|---|---|---|
| **C1** (#157) | `validate_auth_options` strict-mode | Deleted the "all-empty = OK" early-return at `hub_zmq_queue.cpp:587-589`.  Required both keypair fields non-empty + 40-char Z85.  Deleted the silent-ignore at line 617.  Deleted the "if empty skip CURVE" conditional at line 875.  Deleted the "if empty skip serverkey" conditional at line 930.  Updated `validate_auth_options` L2 tests for the new contract (rejection cases, not acceptance). |
| **C2** (#158) | Strong-type non-keypair pubkeys + endpoint shape per HEP-0040 §8.4 | Defined `Z85PublicKey` strong type (40-char Z85 invariant) for the remaining pubkey-only fields (`ProducerPeer::pubkey_z85`, `ProducerEntry::zmq_pubkey`, `ConsumerEntry::zmq_pubkey`, `BrokerRequestComm::Config::broker_pubkey`, BRC's `serverkey`); aligned ZmqQueue factories to HEP-CORE-0040 §8.4 endpoint shape — `push_to(endpoint, identity_key_name, zap_domain)` and `pull_from(endpoint, Z85PublicKey server_pubkey, identity_key_name)`; killed the `ZmqAuthOptions` options bag (its fields became factory parameters).  No `CurveKeypair` parameter on the public API (keys live in KeyStore). |
| **C3** | SHIPPED via #173 (round-5 use-not-export) | RoleAPIBase ctor signature UNCHANGED.  DELETED: `set_auth()` method + three role-host call sites; `Impl::auth_client_pubkey_` / `_seckey_` string members; `auth_client_pubkey()` accessor; `auth_client_seckey()` accessor; `CurveKeypair::empty()` method.  HubAPI does NOT gain symmetric accessors (round-5 deletes the symmetric-accessor plan — no legitimate caller).  Sites that previously read these (role_handler BRC connect, build_tx_queue, build_rx_queue) migrated to `key_store().with_seckey(name, cb)` at the actual libzmq socket-option site.  No `if constexpr` in EngineHost.  No keypair fields in HubConfig / RoleConfig / BrokerService::Config / BRC::Config.  HB-1 closed because `with_seckey` / `pubkey` throw std::out_of_range on missing key — loud, not silent. |
| **C4** (#160) | Delete legacy plaintext factories + adopt HEP-0040 §8.4 endpoint shape + migrate tests | Deleted `ZmqQueue::pull_from()` and `push_to()` (plaintext variants).  Renamed `pull_from_with_auth` → `pull_from`, `push_to_with_auth` → `push_to`.  Final signatures per HEP-CORE-0040 §8.4: `push_to(endpoint, identity_key_name = kRoleIdentityName, zap_domain = "")` and `pull_from(endpoint, Z85PublicKey server_pubkey, identity_key_name = kRoleIdentityName)` — name lookups via `key_store().with_seckey(name, cb)` + `key_store().pubkey(name)` inside the factory body; no keypair parameter on the public API.  Deleted `ZmqAuthOptions` struct entirely (its fields became factory parameters).  Deleted `admission_is_enforced()` from the `PeerAdmission` interface.  Migrated the 103 test sites + flipped 13 roundtrip tests to production-mirror orientation (PUSH+bind / PULL+connect with serverkey).  `admission_is_enforced` was already deleted in close-out commit 1.  Test fixture: `ZmqQueueTestEnvironment` seeds `kRoleIdentityName` once; per-test `ZapPumpThread` services CURVE handshakes; `seed_self_allowlist(*push)` on the bind side after start. |
| **C5** (#161) | CURVE-engagement test assertions + `Mechanism` invariant + start() guard | Added the `Mechanism` enum (`Uninitialized`/`Curve`; 2-state collapse per Phase C review) on `ZmqQueue` (HEP-CORE-0035 §2 enforcement point), a thread-safe atomic `mechanism_` field on `ZmqQueueImpl`, a public const `ZmqQueue::mechanism()` accessor, and a hard guard inside `start()` that queries libzmq via `zmq_getsockopt(ZMQ_MECHANISM)`; if the answer is not `ZMQ_CURVE` the start fails.  Anti-recursion test coverage across four tiers: L2 ZmqQueue invariant (`Mechanism_BeforeStart_IsUninitialized` + `Mechanism_AfterPushBind_IsCurve`); L2 RoleAPIBase static_assert (three compile-time pins on the ctor signature); L2 loader contract (`LoadKeypair_RejectsCorruptVaultContents`); L4 gate enforcement (`CurveGate_CorruptVault_AbortsBeforeBind` all 3 roles); L3 NULL-mech handshake-fail (shipped in close-out per #186 follow-up). |

### Items NOT on the cleanup chain (didn't sequence in by mistake)

- #75 HUB_TARGETED_ACK, #76 script reload, #77 Tier 2 callbacks, #94
  ephemeral binding, #105 federation, #155 phase 3, #120 Windows ACL,
  #152 RoleIdentityPolicy delete — all independent.
- A1 (commit `164b805c`) stayed as the wire-shape schema even though
  the producer_peers field was unused until A3.

### Implementation order that kept ctest green throughout the chain

Doc-review 2026-06-05 caught a sequencing issue: C1 (strict
`validate_auth_options`) alone would break ctest because the ordering
bug (HB-1) means `build_tx_queue` passes empty keys at run time.
Practical order shipped:

1. **C0 — HB-1 ordering fix via HEP-CORE-0040 use-not-export KeyStore**
   (round-5 2026-06-06: BrokerService bind / BRC connect / ZmqQueue
   factory call `key_store().with_seckey(name, cb)` + `pubkey(name)` at
   the libzmq socket-option site; RoleAPIBase + HubAPI lose
   `auth_client_seckey()` entirely; `set_auth` setter deleted; NO
   keypair passed through any ctor; NO keypair fields in Config or
   BrokerService::Config; NO `lookup() → CurveKeypair&` accessor on
   KeyStore).  Landed as part of the HEP-0040 impl chain (tasks
   #167–#175).  Earlier "ctor takes value", "ctor takes const ref",
   and "lookup() returns CurveKeypair&" plans all REJECTED — the first
   two threaded keypair through ctors, the third forced a second
   std::string copy via Entry::view cache.  After C0, any read of an
   identity key before `key_store().add_identity` has been called
   throws std::out_of_range → loud, not silent.  No latent
   consumer-side plaintext fallback survives the migration (it cannot
   compile against the new use-not-export API).
2. **C1 — strict `validate_auth_options`**.  Safe now that HB-1 is
   closed; the strict-mode rejection cannot fire on production paths.
   L2 tests for `validate_auth_options` flipped from acceptance to
   rejection cases.
3. **C2 — strong-type pubkeys + endpoint shape** (`Z85PublicKey` for
   `ProducerPeer::pubkey_z85` / `ProducerEntry::zmq_pubkey` /
   `ConsumerEntry::zmq_pubkey` / `BRC::Config::broker_pubkey`;
   ZmqQueue factories adopt HEP-CORE-0040 §8.4 endpoint shape;
   `ZmqAuthOptions` dies in this step too).  Pure refactor — keys
   still live only in KeyStore.  **NOT a CurveKeypair-into-struct
   rewrite** — that pre-round-5 plan is OBSOLETE per HEP-CORE-0040
   §8.6.
4. **C3 — finalize via #173** (deleted `set_auth`,
   `auth_client_pubkey()`, `auth_client_seckey()` + storage entirely;
   round-5 use-not-export, no `CurveKeypair const&` accessor added).
5. **C4 — delete legacy factories** + rename + migrate 103 test sites
   (factories take `identity_key_name` STRING per HEP §8.4, not
   keypair).
6. **C5 — asymmetric assertions** + unmask any tests masked in C0.

### Memory rule added

**"Audit stale silent-fallback patterns whenever a contract changes."**
Whenever code contains `if (X.empty()) return /* skip security */` or
similar early-return-on-missing-state, the pattern is a contract
violation candidate, not a clean default.  Before respecting it, audit
whether it survives the current HEP contract.  This commit chain
(C1-C5) is the precedent for what the cleanup looks like.

---

## Phase 5 close-out (#161 Phase 5 — concurrency + script binding + doc audits)

Three sub-investigations bundled into the #161 close-out and shipped
together:

- **Script-binding audit** — exposed `ZmqQueue::mechanism()` to scripts
  via `RoleAPIBase::queue_mechanism(ChannelSide)` accessor +
  `hub::mechanism_name()` string conversion + threaded into
  `snapshot_metrics_json()` (Python `api.metrics()`) and Lua
  `api.metrics()`.  Closed #186.
- **Concurrency audit** — `Mechanism` atomic field is the single
  contended cross-thread variable; producer-side writer (factory
  thread) + reader (any script callback thread) ordering verified
  against `std::memory_order_seq_cst`.  No race.  Dropped from open
  items.
- **Doc drift sweep** — HEPs 0015 / 0017 / 0021 / 0040 had stale
  references to `pull_from_with_auth` / `push_to_with_auth` /
  `ZmqAuthOptions`.  All references updated to the post-C4 signatures.

---

## Phase C fresh-eye review (2026-06-09) — design residue cleanup

Performed after C5 shipped.  Findings + dispositions:

- **Mechanism enum should collapse from 3 states to 2** — `Plaintext`
  was structurally unreachable after C1 (validator rejects empty
  keys).  Collapsed to `Uninitialized` / `Curve`.  Applied.
- **`admission_is_enforced()` interface method** — deleted earlier in
  C4 per the original plan.  Mistakenly kept (with a justification
  comment) in an interim pass; deleted again per the original C4
  plan after user caught the regression.
- **Dead defensive throw in ZmqQueue start path** — removed; the
  KeyStore lookup throws std::out_of_range on missing key, so the
  defensive recheck is dead.
- **Doc-tense fix** — `hub_zmq_queue.hpp:144-146` "Empty defaults
  mean plaintext" comment block (the bridge-state quote that opened
  the silent-fallback hole) removed.
- **ProducerPeer + add_producer_peer / remove_producer_peer** —
  initially flagged as dead but HEP-CORE-0017 §3.3 lines 259-298
  mandate this API.  Kept.

---

## Phase D close-out follow-ons that landed alongside the C-chain

- **#187 — vault load-path tightening (post-#175)**.  HEP-CORE-0040
  §175 originally accepted a brief seckey-in-freed-heap exposure
  window during `RoleVault::open` (json node + `.get<std::string>()`
  temporary).  Patch shipped 2026-06-09:
  ```cpp
  auto &pub_ref = j.at("public_key").get_ref<std::string &>();
  auto &sec_ref = j.at("secret_key").get_ref<std::string &>();
  struct WipeGuard {
      std::string &p;
      std::string &s;
      ~WipeGuard() noexcept {
          sodium_memzero(p.data(), p.size());
          sodium_memzero(s.data(), s.size());
      }
  } wipe_on_exit{pub_ref, sec_ref};
  ```
  Closes both freed-heap windows (json string + temporary) in one
  shot.  Documented in HEP-CORE-0040 §8.5.1.

---

## Cross-references — where lasting design content moved

- **HEP-CORE-0035 §2** — Mechanism enum invariant doc.
- **HEP-CORE-0036 §4.1 / §5.6 / §6.5 / §7.1 / §8.2 / §14.3** — queue-
  level gate, shm_secret generation, notify-then-pull wire, ZAP pump,
  data-loop outer guard, Authorized FSM state.
- **HEP-CORE-0040 §8.2 / §8.4 / §8.5.1 / §8.6** — use-not-export API,
  factory endpoint shape, vault load-path hardening, KeyStore name
  canonical names.
- **HEP-CORE-0017 §3.3** — ProducerPeer + RxQueueOptions::producer_peers
  + ZmqQueue dynamic peer API contract.

Anything not captured here that is still load-bearing lives in the
HEPs cited above; the active `docs/todo/AUTH_TODO.md` carries only the
open work plan after this archive sweep.
