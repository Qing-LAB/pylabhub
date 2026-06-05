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

## Critical-path execution plan — #103 closes data-plane CURVE

Verified 2026-06-05 against code: `#103` is the single blocker for
AUTH Phase D4 + D5.  No `producer_peers` references exist in `src/`;
`ZmqQueue::add_producer_peer` / `remove_producer_peer` do not exist.
Without that API, D4 has nothing to call and D5's emitted array has
no consumer-side apply path.  Splits into three sequential commits:

| # | Commit | Files | Scope | Size |
|---|---|---|---|---|
| A1 | `#103` schema | `src/include/utils/hub_rx_queue_options.hpp` | Add `struct ProducerPeer{role_uid, endpoint, pubkey}` + `std::vector<ProducerPeer> producer_peers` field per HEP-0017 §3.3.  Wire-shape only; no runtime logic. | ~40 LOC |
| A2 | `#103` dynamic peer API + producer-side ZAP | `src/include/utils/hub_zmq_queue.hpp` + `src/utils/hub/hub_zmq_queue.cpp` | Implement `add_producer_peer(peer)` / `remove_producer_peer(uid)` + update PeerAdmission allowlist atomically.  Install producer-side ZAP handler on PUSH bind per HEP-0036 §7.  Switch `role_api_base.cpp` `pull_from()` / `push_to()` factories to the `_with_auth()` variants. | ~150-200 LOC |
| A3 | D5 + D4 + folded drifts | `src/utils/ipc/broker_service.cpp` (`handle_consumer_reg_req` response builder ~2521-2530) + `src/utils/network_comm/broker_request_comm.cpp` (`on_notification_cb` dispatch) + role-host coalescing refresh worker | **D5**: emit `resp["producers"] = [...{role_uid, pubkey, endpoint}...]` from `channel_entry.producers`; SHM transport also emits `shm_secret` field (zero today; populated when Phase G ships).  **D4**: BRC routes `CHANNEL_AUTH_CHANGED_NOTIFY` → fires `GET_CHANNEL_AUTH_REQ` → applies via `ZmqQueue::set_peer_allowlist(snapshot)`; coalesce drops redundant notifies when a query is already in flight.  Folds in **B1** (awaiting_endpoint reason) + **B2** (`zmq_msg_gets("User-Id")` for pubkey recovery). | ~200-250 LOC |

After A3 lands, the data-plane CURVE auth gate is closed in code.

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
