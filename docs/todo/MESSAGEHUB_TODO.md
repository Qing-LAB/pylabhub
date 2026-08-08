# Messenger / Broker TODO

**Scope:** MessageHub / broker protocol / federation / hub-side connection
lifecycle. Open items only — 865 → this, on 2026-08-07. Closure evidence:
`docs/archive/transient-2026-08-07/todo-completions/MESSAGEHUB_TODO_closed_2026-08-07.md`.
Strategic status lives in `docs/TODO_MASTER.md`.

**Task IDs here are live IDs.** Two sections used to be titled `#95` and
`#92`, which mean different things in the live list; both described work
that is now closed and both are gone.

---

## Where the broker stands

| Track | State |
|---|---|
| **HEP-CORE-0046 REG redesign** | ✅ Phase B complete — all nine REG-family handlers typed, `to_legacy` bridge retired. Follow-on: **#131** (five untyped inbound notifies) + **#82** (typed Schema/Metrics request bodies). |
| **Schema / metrics query integration** | ✅ All four slices shipped 2026-07-26; design lives in HEP-0034 + HEP-0007 §12.3/§12.4a + HEP-0036 §5b.7. |
| **HEP-CORE-0035 auth** | 🟢 Single-hub CURVE production-ready (REVIEW-E). Remainder is federation (**#69**) + the items in `AUTH_TODO.md`. |
| **Arc A — `plh_hub` renovation** (HEP-0033 §15) | Phases 1-9 shipped; Phase 10 doc-amendment recorded as partial. **Not re-verified in the 2026-08-07 pass** — check it against HEP-0033 before treating it as either open or done. |
| **Arc B — role-host renovation** (Wave-B M0..M9) | ✅ Closed 2026-05-26. |
| **`HUB_TARGETED_ACK`** (HEP-0033 §12.3.6) | ⏸ Folded into **#69**; never lands standalone. |

---

## ⚠️ Do not delete `CHANNEL_BROADCAST_REQ`

An archived draft (`DRAFT_reg_wire_alignment_cleanup_2026-07-13.md`, groups
C2 and D1) calls `CHANNEL_BROADCAST_REQ` "retired" and plans to delete the
dispatch-table row and `BrokerRequestComm::send_broadcast()`.

**That premise was overturned four days after it was written.** Finding
B6/T3 of the Connection/Inbox/Band review established that channel-bound
broadcast (broker fans out to a channel's producers + consumers) and
band-bound broadcast (broker fans out to a band's members) are
**complementary, not one superseding the other** — HEP-CORE-0030 §9 was
amended and §9.1 added saying so. `CHANNEL_BROADCAST_REQ` has live callers:
the hub API, the admin RPC, and the broker's own synthetic path.

Executing that draft group today would delete shipping protocol. This
warning lives here because the draft sits in `docs/archive/` where a reader
would have no way to know it was overturned.

---

## Open — wire shape

- **`expected_schema_owner` — HEP-0036 §5b.6's catalog is missing a row.
  ⚠ DO NOT DELETE THE FIELD.** An earlier version of this entry said
  production never sends it and offered "delete the accessor and the read"
  as an option. **That was wrong.** The field is required and enforced:
  `broker_service.cpp:3633` rejects an owner claim without a schema id
  (`INVALID_REQUEST`), and `:3760` rejects a named fan-in open with no
  owner (`SCHEMA_OWNER_REQUIRED`, with a WARN). HEP-0007's error table
  documents that code operator-facing, and HEP-0034 §Owner axis names the
  field with this exact spelling. The behaviour was ruled 2026-07-26 to
  close a stale-silent-fallback; deleting it would re-open it.
  What actually remains: HEP-0036 §5b.6 lists `expected_schema_id` /
  `_hash` / `_blds` / `_packing` and omits `_owner`. Add the row.
  Doc-only. **Task #130.**

- **Five inbound-notify bodies are still untyped** — `CHANNEL_COUNT_NOTIFY`,
  `CHANNEL_EVENT_NOTIFY`, `CHANNEL_ERROR_NOTIFY`,
  `CHANNEL_BROADCAST_DELIVER_NOTIFY`, `BAND_BROADCAST_DELIVER_NOTIFY`. Each
  needs a `wire_bodies` class + a BRC shape-table arm. These are messages a
  role *receives*, so untyped means per-handler `body["field"]` extraction —
  the scatter the typed envelope exists to remove. **Task #131.**

- **A locked HEP invariant contradicts shipping code.** HEP-CORE-0046
  **I-CORRELATION-STABLE** states without qualification that BRC's
  `pending_requests` keys on `(msg_type, correlation_id)`; the code keys on
  correlation_id alone (`broker_request_comm.cpp:281`). The #72 pass made it
  worse by rewriting BRC's echo-check *comments* to justify the single-key
  shape — prose aligned to code while the governing invariant says the
  opposite. **Owner ruled 2026-08-07: the doc is the source of truth; change
  the map key and revert that comment edit.** **Task #113.**

- **`consumer_attach_zmq()` / `CONSUMER_ATTACH_REQ_ZMQ` still live** (16
  references in `src/`). Owned by the topology Phase-E retirement — see
  `TOPOLOGY_TODO.md`. Noted here only so the archived draft's version of it
  is not read as a separate item.

## Open — federation (all of it parked under #69)

**Ruling, ratified 2026-07-24: federation gets a full top-down design — hub↔hub
trust model, peer lifecycle and discovery, wire, security — BEFORE any further
protocol work. No piecemeal patches to federation paths.**

`#69` is the single umbrella and consolidates: (1) the **security ingress
bypass** — peer-DEALER traffic skips the `receive_and_validate` gate chain,
the last unvalidated broker ingress; (2) the **control-envelope bypass** —
`HUB_PEER_HELLO` (`broker_service.cpp:1095`) and `HUB_PEER_BYE` (`:1338`)
hand-roll a divergent 3-frame layout with no correlation_id via raw
`socket.send`, and `HUB_TARGETED_MSG` / `HUB_RELAY_MSG` are likewise
off-envelope (HEP-0047 §3.0 records this as today's lone exception); (3) H43
role-disconnect propagation; (4) `HUB_TARGETED_ACK`; (5) the skipped
`BrokerFederationTest.*` suite, whose test strategy the design must define.
Anchors: HEP-0022, HEP-0037, HEP-0035 federation-trust, HEP-0033 §12.3,
HEP-0047 §3.0.

**A shape not to carry forward — recorded as design input, NOT a live hole.**
A federation peer configured without a pubkey is wired PLAIN and connected
anyway: setting none of the CURVE options leaves libzmq on the NULL
mechanism, so the socket does not fail closed — it connects and sends in the
clear. The stated rationale is diagnostics-over-silence (the remote ROUTER
rejects the unauthenticated handshake, so the operator sees
`HANDSHAKE_FAILED_*` rather than a silent no-op).

Why that is a gap and not a trade-off: it delegates a security property to a
machine we do not control. It holds only while the far end enforces. A
permissive or older peer means both ends talk plaintext indefinitely with
nothing failing; a wrong endpoint — typo, recycled address, hostile listener
— means we connect and begin sending to whoever answered, with no key to
check them against. `FederationPeer::pubkey_z85` and
`FederationPeerEntry::pubkey` both *document* the empty value as "no CURVE"
and nothing validates it at config load, so it reads as a supported mode.
That is the same shape as the two backdoors closed in #90/#91: unreachable
in practice, documented as legitimate, therefore unwatched.

It is left in place deliberately. Nothing an operator can configure today
reaches this code — there is no federation app and no federation config
surface — and hardening scaffolding whose contract does not exist would bake
in an assumption the design has not made. When the design lands: a peer with
an endpoint and no key cannot federate under any circumstances, so refusing
it at config load beats connecting and hoping someone is watching a monitor;
and "trusted peer hub" must resolve to real entries in `peers` from an
authority, because the blanket-admit primitive is gone (#91) and is not
coming back.

## Open — smaller items

- **`IncomingMessage::sender` means two different things.**
  `HubScriptRunner::worker_main_()` reuses `scripting::IncomingMessage` from
  `role_host_core.hpp` as the cross-thread queue payload. Role-side `sender`
  is the broker peer; hub-side it should be the originating role's uid (HEP-0033
  §12). Audit the field-use sites and document the split, or introduce a
  sibling type.
- **Phantom script API in HEP-0022 §8.2** — `api.notify_hub` does not exist;
  no script API sends `HUB_TARGETED_MSG` (only a broker augment hook). Left
  flagged in the doc pending a decision on whether to add the surface.
  (`api.notify_channel` in the same section was a naming error for
  `api.broadcast_channel` and is already fixed.)
- **HEP-CORE-0039 query layer, phases B+** — see `QUERY_LAYER_TODO.md`.
- **HEP-CORE-0047 drift-guard** — the master wire registry landed
  2026-07-17 with a drift-guard spec; the guard itself is not built.
- **Stale-comment scrub, ~15 sites** — references to deleted
  `set_broker_comm` / `start_ctrl_thread` / `pImpl->broker_channel` in
  `role_api_base.{hpp,cpp}`, `hub_script_runner.cpp`, the three role hosts,
  `engine_host.hpp`, `role_host_helpers.hpp`, `role_host_core.hpp`. Plus
  three obsolete `#include "utils/broker_request_comm.hpp"` in the role hosts
  (BRC reaches them via `role_handler.hpp` now). Mechanical.
- **Dead-code candidates:** `ChannelSnapshot::count_by_observable()`
  (`broker_service.hpp:77`) — verified 2026-08-07 as having no other
  reference in `src/` or `tests/`; folds into **#115**.
  `query_shm_info` / `collect_shm_info_json` / `SHM_INFO_REQ` fold into
  **#112**. *(`RoleAPIBase::close_all_inbox_clients()` was listed here too —
  it no longer exists anywhere in the tree.)*
- **Doc debt:** two HEPs describe mechanisms their own later sections retire
  (HEP-0021 §16 line 608, HEP-0018 lines 422/470) — **#110**. Four
  `REVIEW_*WaveM3*.md` files from 2026-05-11 are archive candidates.

---

## Reference — kept because it is short and settled

**Error taxonomy.** Cat 1 = protocol/contract violations, surfaced as
errors. Cat 2 = operational events, surfaced as notifications. Full rules in
`docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy — Broker, Producer, and
Consumer".

**Key design decisions.** Per-presence FSM on `RoleEntry` (Connected /
Pending / Disconnected) replaced the Pending / Ready / Closing channel-level
model. Channel-bound and band-bound broadcast are complementary (HEP-0030
§9.1). Heartbeat is fire-and-forget `HEARTBEAT_NOTIFY` with no ack.

## Related

`docs/todo/API_TODO.md` (API-layer view of the same renovation),
`docs/todo/TESTING_TODO.md` (broker-protocol test gaps),
`docs/todo/TOPOLOGY_TODO.md` (Phase E retirements),
`docs/todo/QUERY_LAYER_TODO.md` (HEP-0039).
