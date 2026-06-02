# Authentication / PeerAdmission TODO

**Scope:** Open items for HEP-CORE-0035 (Hub-Role auth + federation
trust) and the PeerAdmission feature track that closes out the
data-channel CURVE auth gate.

**Authoritative design lives in:**
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — Layer-1 ZAP + Layer-2 federation trust gate + key-file ACL discipline (§4.6) + runtime key handling (§4.7).
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md` — Three-tier auth (transport / identity / authorization), `ChannelAccessIndex` (§4.1), `CHANNEL_AUTH_UPDATE` snapshot wire (§6.5), per-producer pubkey + endpoint (§6.4).
- `docs/HEP/HEP-CORE-0017-Queue-Abstraction.md` §3.3 — `RxQueueOptions::producer_peers` queue auth contract.

**Status source of truth:** `docs/TODO_MASTER.md` (when PeerAdmission
work is the active sprint).

---

## Current PeerAdmission state (2026-06-02)

| Phase | Status | Notes |
|---|---|---|
| A — Abstraction (PeerAdmission interface) | ✅ shipped | commit `d5a90f29` |
| B — KnownRole + CLI | ✅ shipped | commit `a6b44ff8`; HEP-0035 §4.8.3/§4.8.4 |
| C — ZapRouter + ZmqQueue CURVE | ✅ shipped + closed | Phase C close-out commits `62bda863..47aa0374` |
| D — Broker glue (gate closes) | ⏳ ready to implement | All blockers resolved (see "Resolved decisions" below) |
| E — Admin loopback enforcement | ⏸ planned | Unblocked once D ships |
| F — Federation peer ZAP parity | ⏸ planned | Depends on E + Federation HEP (#105) |
| G — SHM auth migration | ⏸ planned | Independent of D/E/F; can interleave |
| H — Demo migration | ⏸ planned | Last; needs D shipped end-to-end |
| X — Runtime key hardening | ⏸ planned | HEP-0035 §4.7; task #102 |

---

## Phase D — Broker glue (next up)

`BrokerServiceImpl` holds the `ChannelAccessIndex`, runs ZAP for the
CTRL ROUTER, and pushes `CHANNEL_AUTH_UPDATE` snapshots when consumer
membership changes.  Steps:

1. **D1 — `ChannelAccessIndex` in `HubState`** (per HEP-CORE-0036 §4.1
   line 388: `channel_access_index_` lives in HubState, NOT in
   BrokerServiceImpl).  Two fields per entry
   (`authorized_consumer_pubkeys` + `shm_secret`).  Producer pubkey
   + endpoint live on `ChannelEntry::producers[i]` (existing fields
   `zmq_pubkey` + `zmq_node_endpoint`, hub_state.hpp:184/194) — no
   duplication.  Mutator API on HubState: `_on_producer_registered`
   creates entry + generates `shm_secret` if transport=shm;
   `_on_consumer_authorized` writes pubkey to allowlist;
   `_on_consumer_revoked` removes pubkey; entry deleted by the
   existing last-producer atomic-teardown path
   (`_on_channel_closed`).  Read accessor: `find_channel_access`.
   L2 tests for the mutators (HubStateOps pattern).
2. **D2 — Broker CTRL ROUTER ZAP handler.**  Installed against the
   broker-side `KnownRoleAllowlist` (already loaded by Phase B from
   `<hub_dir>/vault/known_roles.json`).  Refuses every CTRL hello
   whose User-Id is not in the allowlist.
3. **D3 — `CHANNEL_AUTH_UPDATE` wire frame.**  broker_proto 5 → 6.
   Snapshot semantics per HEP-0036 §6.5 (locked 2026-06-02): single
   `allowlist[]` array of plain Z85 strings; receiver REPLACES cache.
4. **D4 — Role-side dispatch.**  `BrokerRequestComm` recognizes the
   inbound message type; `RoleAPIBase` looks up the matching tx_queue
   by channel name and calls `set_peer_allowlist(snapshot)`.
5. **D5 — `CONSUMER_REG_ACK.producers[]` array.**  One entry per
   producer of the channel — supports fan-in (HEP-CORE-0023 §2.1.1).
   Each entry: `{role_uid, pubkey, endpoint}`; SHM transport also
   carries `shm_secret`.
6. **D6 — L3 tests.**  Broker pushes allowlist on consumer reg /
   dereg; producer applies; consumer with wrong pubkey rejected;
   revocation propagates within the contract bound.
7. **D7 — L4 test.**  Full dual-hub data flow with auth gates closed.

Tracked as task #126.  Sub-tasks (D1–D7) are commits inside the Phase
D landing window.

## Resolved decisions (for reference)

| # | Decision | Where it landed |
|---|---|---|
| P-API | ZmqQueue auth shape — additive `*_with_auth` overloads | Phase C `7b7944e8` |
| P-Wire | CHANNEL_AUTH_UPDATE semantics — snapshot, not delta | HEP-0036 §6.5 (locked 2026-06-02) |
| P-Vault | Where known roles live — separate `<hub_dir>/vault/known_roles.json` file mode 0600 | Phase B `a6b44ff8`; HEP-0035 §4.8 |
| P-Threading | ZapRouter — caller-pumped from BRC poll thread, no internal thread | HEP-0036 §7.1; Phase C `28a06046` + `827474f0` |
| P-S3 | `current_allowlist_` atomic primitive — `PortableAtomicSharedPtr` | Phase C `7b7944e8` |
| P-Schema | `ChannelAccessEntry` shape — two fields, per-producer info per-producer | HEP-0036 §4.1 (locked) |
| P-Push | How broker pushes update — reuse CTRL DEALER/ROUTER in reverse direction | HEP-0036 §6.5 |
| P-Default | Empty allowlist semantics — deny-all | HEP-0036 §6.5 |
| P-Migration | Operators with pre-auth vaults — auto-derive pubkey; absent file = deny-all + admit-none until populated by CLI | HEP-0035 §4.8.4 |

## Deferred decisions (each tied to its phase)

| # | Decision | Affects | Tentative direction |
|---|---|---|---|
| P-InboxQueue | InboxQueue admission policy location | Phase E | InboxQueue implements `PeerAdmission` directly, no queue inheritance — preserves REQ/REP nature |
| P-Admin | AdminService — CURVE-wrap or loopback-only? | Phase E (task #127) | Hard loopback-only enforce (refuse non-loopback bind) for v1; CURVE-wrap is HEP-CORE-0035 §5 future work |
| P-SHM-Identity | What is a PeerIdentity for SHM? | Phase G (task #129) | Broker-issued `shm_secret` primary; optional uid guard if operator sets it; broker controls the gate via secret issuance |
| P-Demos | How existing demos migrate | Phase H (task #130) | Transitional `--allow-anonymous-data` flag, gated to refuse-bind on non-loopback endpoints; demos updated incrementally |
| P-HEP | When to sync HEPs vs hold tech_draft | Close-out (task #131) | Tech_draft was archived 2026-06-02; HEPs are now the authoritative source.  No further sync needed unless H surfaces gaps |

## Considered-but-not-pursued

| Idea | Source | Reason |
|---|---|---|
| `--allow-anonymous-roles` flag (S6 option c) | tech_draft §12.5 S6 | Empty `known_roles.json` already maps to deny-all per HEP-0035 §4.8.4; the friendly-bootstrap need is satisfied by the `--add-known-role` CLI + clear deny-all diagnostic.  Revisit only if Phase H demo migration finds it necessary |

---

## Parallel / adjacent tracks

These have their own task IDs but touch the same surface:

- **Task #102** — HEP-CORE-0035 §4.7 runtime key handling (mlock + no-core-dump + zeroing, cross-platform).
- **Task #103** — HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation: `RxQueueOptions::producer_peers` + ZmqQueue dynamic peer API.
- **Task #104** — Sibling HEP updates: schema / FSM / CURVE-wiring per HEP-CORE-0036 §14.
- **Task #105** — Federation protocol design + cross-hub reg/comm verification.
- **Task #106** — HEP-CORE-0038 + impl: script-accessible vault keystore (`api.vault_save/load`).
- **Task #120** — Windows pathway hardening for HEP-CORE-0035 §4.6 floor.
