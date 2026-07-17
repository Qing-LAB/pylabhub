# HEP-CORE-0047: Messaging & Communication Master Reference

| Property | Value |
|---|---|
| **HEP** | `HEP-CORE-0047` |
| **Title** | Messaging & Communication Master Reference — Wire-Message Registry + Cluster Map |
| **Status** | 📖 **REFERENCE.**  This HEP owns no protocol of its own.  It is the navigational index and the single canonical registry of every wire message, cross-referencing the protocol HEPs that define each one. |
| **Created** | 2026-07-17 |
| **Scope** | Index + registry only.  Cross-cutting rules (the message-type suffix taxonomy, the Cat 1 / Cat 2 error taxonomy) remain owned by their source documents and are *referenced* here, never redefined. |
| **References** | HEP-CORE-0002, -0007, -0013, -0019, -0021, -0022, -0023, -0024, -0027, -0030, -0035, -0036, -0039, -0041, -0042, -0044, -0045, -0046; `docs/IMPLEMENTATION_GUIDANCE.md § "Error Taxonomy"`; `src/utils/network_comm/wire_dispatch.cpp` (`kDispatchTable`). |

---

## 1. Why this document exists

The messaging and communication design is spread across roughly sixteen HEPs.
Any one wire message — say a broadcast — has its behaviour described in one
HEP, its naming convention set by another, its error category defined in a
third, and its handler living in the code.  When one of those pieces is renamed
or retired, the others drift.  A real example: the channel broadcast was
renamed from `CHANNEL_BROADCAST_REQ` to `CHANNEL_BROADCAST_SEND_NOTIFY`, but
several HEPs kept striking the old name out as "removed / superseded by the
band protocol" — which was simply wrong, because the message is alive under a
new name.  A reviewer reading only the stale strike-through would conclude the
feature was gone.

This document is the one place a reviewer or implementer can start to answer
three questions without reading all sixteen HEPs:

1. **Which HEP owns the behaviour I care about?** → §2 cluster map.
2. **Is this wire message real, what does it do, and where is its code?** →
   §3 canonical registry.
3. **Am I about to confuse two similar-looking messages?** → §5 do-not-confuse
   pairs, and §6 rename/retirement ledger.

It deliberately does **not** re-specify any protocol.  Each registry row points
at the HEP that is authoritative for that message; if this document and that
HEP disagree, the protocol HEP wins and this document has a bug to fix.

## 2. The communication HEP cluster

Grouped by plane.  "Owns" is the behaviour a reviewer should expect to find
authoritatively specified there; "does NOT own" names the most common
misattribution.

| Plane | HEP | Owns | Does NOT own |
|---|---|---|---|
| **Data hub core** | HEP-CORE-0002 | The DataHub model, slots, producer/consumer roles | The wire protocol catalog (that is -0007) |
| **Protocol catalog** | HEP-CORE-0007 | The reference catalog of DataHub wire messages + Cat 1/Cat 2 broker notifications | The REG-family redesign (that is -0046); band pub/sub (that is -0030) |
| **REG redesign** | HEP-CORE-0046 | The typed `WireEnvelope`, admission-gate pipeline, REG/CONSUMER_REG/DEREG shapes, and the **message-type suffix taxonomy** (`_REQ`/`_ACK`/`_NOTIFY`) | The catalog prose (still in -0007 §12 until migrated) |
| **Endpoint registry** | HEP-CORE-0021 | `ENDPOINT_UPDATE_REQ`, the ZMQ endpoint lifecycle | Channel admission (that is -0042/-0036) |
| **Channel attach** | HEP-CORE-0042 | `CHANNEL_AUTH_APPLIED_REQ`, `CHANNEL_AUTH_CHANGED_NOTIFY`, the attach-coordination sequence | Peer authentication cryptography (that is -0035/-0036/-0044) |
| **Startup coordination** | HEP-CORE-0023 | Role liveness FSM, `HEARTBEAT_NOTIFY`, `CHECK_PEER_READY_REQ`, reclaim → `CONSUMER_DIED_NOTIFY` | Endpoint publication (that is -0021) |
| **Band pub/sub** | HEP-CORE-0030 | The `BAND_*` family; §9.1 channel-bound vs band-bound broadcast coexistence | The channel-bound broadcast (that is -0007; -0030 §9.1 only asserts coexistence) |
| **Federation** | HEP-CORE-0022 | `HUB_PEER_HELLO`/`_ACK`/`_BYE`, `HUB_RELAY_MSG`, `HUB_TARGETED_MSG`, cross-hub relay | Intra-hub broadcast (that is -0007/-0030) |
| **Inbox** | HEP-CORE-0027 | Point-to-point role→role inbox delivery | Channel/band broadcast |
| **Auth overlay** | HEP-CORE-0035, -0036 | Hub-role CURVE identity, ZAP admission, authenticated connection establishment | The application-layer SHM attach handshake (that is -0044/-0041) |
| **SHM attach** | HEP-CORE-0041, -0044, -0045 | The SHM AttachProtocol binary frames + SCM_RIGHTS capability + broker SHM observer | JSON control-plane messages (all the rows in §3) |
| **Query / directory / metrics** | HEP-CORE-0024, -0039, -0019, -0016/-0034 | Role directory, hub-state query layer, metrics plane, schema registry | See the attribution caveat in §3 |
| **Channel identity** | HEP-CORE-0013 | Channel naming, provenance, role_uid grammar | Message dispatch |

> **Known duplication (flagged, not resolved here).**  Two message catalogs
> exist today: HEP-CORE-0007 §12 and the message table in HEP-CORE-0033 (Hub
> Character).  Most control/query messages appear in both.  Consolidating them
> — deciding which is authoritative per message — is a reorganization decision
> for the designer, tracked in `docs/todo/MESSAGEHUB_TODO.md`.  Until then the
> "Primary HEP" column in §3 names the *protocol* HEP where the message's
> semantics are defined, not necessarily the only place it is described.

## 3. Canonical wire-message registry

**Provenance of columns.**  *Message name*, *Direction*, and *Code anchor* are
extracted directly from the source tree (`broker_service.cpp`,
`wire_dispatch.cpp`) as of 2026-07-17 and are the load-bearing, drift-tested
facts (see §7).  *Category* is from the error/shape taxonomy where it applies.
*Primary HEP* is a best assignment; rows marked ⟳ have ambiguous ownership
pending the §2 catalog-consolidation decision and MUST be verified before being
cited as authoritative.

**Direction legend:** `→B` role/hub → broker (inbound, broker handles);
`B→` broker → role/hub (outbound, broker emits); `B↔B` broker ↔ broker
(federation).

**Category legend:** `RA` = request/ack (synchronous, has a wire `_ACK`);
`FF` = fire-and-forget (`_NOTIFY`, no `_ACK`, caller must not depend on
acceptance — HEP-CORE-0007 §12.2.1); `C1`/`C2` = Cat 1 / Cat 2 broker
notification (`IMPLEMENTATION_GUIDANCE § Error Taxonomy`).

### 3.1 Registration & channel lifecycle

> **Registration is channel creation.**  There is no dedicated
> "create channel" wire message (`CREATE_CHANNEL_REQ` never existed — it was a
> phantom in HEP-CORE-0018).  A channel is created as a side-effect of the
> **first registration** on a channel name: the first `REG_REQ` (producer-
> binding topologies — fan-out / one-to-one) or, under fan-in, the binding-side
> consumer's first `CONSUMER_REG_REQ`.  The broker signals first-vs-subsequent
> with `admission.channel_opened` (`broker_service.cpp:2666`/`:3649`).  This is
> the entry point for every channel.

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `REG_REQ` / `REG_ACK` | →B / B→ | RA | HEP-CORE-0046 (catalog -0007 §12) | `wire_dispatch.cpp` Tier::RegReq; `broker_service.cpp` process_message |
| `CONSUMER_REG_REQ` / `CONSUMER_REG_ACK` | →B / B→ | RA | HEP-CORE-0046 | Tier::ConsumerRegReq |
| `DEREG_REQ` / `DEREG_ACK` | →B / B→ | RA | HEP-CORE-0007 §12 | Tier::AuthReg_Dereg |
| `CONSUMER_DEREG_REQ` / `CONSUMER_DEREG_ACK` | →B / B→ | RA | HEP-CORE-0007 §12 | Tier::AuthReg_ConsumerDereg |
| `ENDPOINT_UPDATE_REQ` / `ENDPOINT_UPDATE_ACK` | →B / B→ | RA | HEP-CORE-0021 §16 | Tier::AuthReg_EndpointUpdate |
| `CHANNEL_CLOSING_NOTIFY` | B→ | FF | HEP-CORE-0007 §12 | `send_closing_notify` (`broker_service.cpp:6009`) |
| `ROLE_REGISTERED_NOTIFY` — **planned, NOT implemented** | B→ | FF | HEP-CORE-0007 §12 | none (no wire literal in `src`); planned for federation role-presence propagation |
| `ROLE_DEREGISTERED_NOTIFY` — **planned, NOT implemented** | B→ | FF | HEP-CORE-0007 §12 | none (planned; sibling of `ROLE_REGISTERED_NOTIFY`) |

### 3.2 Channel admission / auth-changed

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `CHANNEL_AUTH_APPLIED_REQ` / `CHANNEL_AUTH_APPLIED_ACK` | →B / B→ | RA | HEP-CORE-0042 §5.5 | Tier::AuthReg_ChanAuthApplied |
| `CHANNEL_AUTH_CHANGED_NOTIFY` | B→ | FF | HEP-CORE-0042 | emitted on admit/revoke (`fire_channel_auth_changed_notify`) |
| `GET_CHANNEL_AUTH_REQ` / `GET_CHANNEL_AUTH_ACK` | →B / B→ | RA | HEP-CORE-0042 / -0036 ⟳ | Tier::Control_GetChannelAuth |

### 3.3 Liveness & control

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `HEARTBEAT_NOTIFY` (was `HEARTBEAT_REQ`) | →B | FF | HEP-CORE-0023 §2.1 | Tier::Control_HeartbeatNotify |
| `DISC_REQ` / `DISC_ACK` | →B / B→ | RA | HEP-CORE-0007 §12 | Tier::Control_Disc |
| `CHECK_PEER_READY_REQ` / `CHECK_PEER_READY_ACK` | →B / B→ | RA | HEP-CORE-0042 / -0023 ⟳ | Tier::Control_EnvelopeWithRoleUid |
| `CONSUMER_DIED_NOTIFY` | B→ | FF | HEP-CORE-0023 (reclaim) ⟳ | emitted from heartbeat-timeout reclaim |

### 3.4 Channel notifications (Cat 1 vs Cat 2)

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `CHANNEL_ERROR_NOTIFY` | B→ | **C1** | HEP-CORE-0007 §"Broker Notifications" | `broker_service.cpp:2248` (schema-mismatch on re-reg → fan-out to existing producers) |
| `CHANNEL_EVENT_NOTIFY` | B→ | **C2** | HEP-CORE-0007 / -0030 §9.1 | `:6159`/`:6167` (checksum NotifyOnly); `:7377` (federation relay via `handle_hub_relay_msg`) |
| `CHECKSUM_ERROR_REPORT` | →B | FF | HEP-CORE-0007 / -0006 | `handle_checksum_error_report` (`:6138`, dispatched `:1811`) |

### 3.5 Broadcast — channel-bound (renamed, NOT superseded)

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `CHANNEL_BROADCAST_SEND_NOTIFY` (was `CHANNEL_BROADCAST_REQ`) | →B | FF | HEP-CORE-0007 / -0030 §9.1 | `handle_channel_broadcast_req` (`:6197`, dispatched `:1826`) |
| `CHANNEL_BROADCAST_DELIVER_NOTIFY` (was `CHANNEL_BROADCAST_NOTIFY`) | B→ | FF | HEP-CORE-0007 / -0030 §9.1 | fan-out at `:6234` (consumers) / `:6250` (producer) |

### 3.6 Broadcast — band-bound (pub/sub, HEP-CORE-0030)

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `BAND_JOIN_REQ` / `BAND_JOIN_ACK` | →B / B→ | RA | HEP-CORE-0030 §5 | Tier::Control_EnvelopeWithRoleUid |
| `BAND_JOIN_NOTIFY` | B→ | FF | HEP-CORE-0030 §5 | emitted to band members on join |
| `BAND_LEAVE_REQ` / `BAND_LEAVE_ACK` | →B / B→ | RA | HEP-CORE-0030 §5 | Tier::Control_EnvelopeWithRoleUid |
| `BAND_LEAVE_NOTIFY` | B→ | FF | HEP-CORE-0030 §5 | `send_band_leave_notify` (`:6102`) |
| `BAND_BROADCAST_SEND_NOTIFY` | →B | FF | HEP-CORE-0030 §5 | Tier::Control_EnvelopeWithRoleUid |
| `BAND_BROADCAST_DELIVER_NOTIFY` | B→ | FF | HEP-CORE-0030 §5 | broker fan-out to band members |
| `BAND_MEMBERS_REQ` / `BAND_MEMBERS_ACK` | →B / B→ | RA | HEP-CORE-0030 §5 | Tier::EnvelopeOnly |

### 3.7 Query / directory / metrics / schema

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `ROLE_PRESENCE_REQ` / `ROLE_PRESENCE_ACK` | →B / B→ | RA | HEP-CORE-0039 / -0033 ⟳ | Tier::Control_EnvelopeWithQueryRoleUid |
| `ROLE_INFO_REQ` / `ROLE_INFO_ACK` | →B / B→ | RA | HEP-CORE-0039 / -0033 ⟳ | Tier::Control_EnvelopeWithQueryRoleUid |
| `SCHEMA_REQ` / `SCHEMA_ACK` | →B / B→ | RA | HEP-CORE-0016 / -0034 ⟳ | Tier::EnvelopeOnly |
| `CHANNEL_LIST_REQ` / `CHANNEL_LIST_ACK` | →B / B→ | RA | HEP-CORE-0039 / -0007 ⟳ | Tier::EnvelopeOnly |
| `METRICS_REQ` / `METRICS_ACK` | →B / B→ | RA | HEP-CORE-0019 | Tier::EnvelopeOnly |
| `SHM_BLOCK_QUERY_REQ` / `SHM_BLOCK_QUERY_ACK` | →B / B→ | RA | HEP-CORE-0007 / -0033 ⟳ | Tier::EnvelopeOnly |

### 3.8 Federation (broker ↔ broker, HEP-CORE-0022)

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| `HUB_PEER_HELLO` / `HUB_PEER_HELLO_ACK` | B↔B | RA | HEP-CORE-0022 | `handle_hub_peer_hello` (`:7209`) |
| `HUB_PEER_BYE` | B↔B | FF | HEP-CORE-0022 | `handle_hub_peer_bye` (`:7289`) |
| `HUB_RELAY_MSG` | B↔B | FF | HEP-CORE-0022 | `handle_hub_relay_msg` (`:7325`) → local `CHANNEL_EVENT_NOTIFY` |
| `HUB_TARGETED_MSG` | B↔B | FF | HEP-CORE-0022 | `handle_hub_targeted_msg` (`:7383`) |

### 3.9 Non-JSON and sentinel

| Message | Dir | Cat | Primary HEP | Code anchor |
|---|---|---|---|---|
| SHM AttachProtocol Frame 1/2/3 + SCM_RIGHTS capability | peer↔peer | binary frames | HEP-CORE-0044 / -0041 | `attach_protocol.cpp`, `shm_capability_channel.cpp` — **not** JSON wire msg_types |
| `UNKNOWN_MSG_TYPE` | B→ | reply | HEP-CORE-0007 | reply to any unrecognized or retired msg_type |

> **Inbox (HEP-CORE-0027).**  The point-to-point inbox family is not yet
> enumerated here; its wire surface is tracked under tasks #191/#103.  Add its
> rows when that plumbing lands.

## 4. Keyword & suffix glossary (referenced, not redefined)

These conventions are **owned elsewhere**; this section only collects the
vocabulary so every messaging HEP uses the same words.

- **Message-type suffix taxonomy** (HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY):
  - `_REQ` + `_ACK` — synchronous request/reply.  A `_REQ` with a wire `_ACK`
    MUST have a client API that observes the ack (HEP-CORE-0007 §12.2.1).
  - `_NOTIFY` — fire-and-forget.  No `_ACK`; the caller must proceed correctly
    regardless of acceptance.  The suffix is *why* several `_REQ` messages were
    renamed to `_SEND_NOTIFY` (e.g. `HEARTBEAT_REQ`→`HEARTBEAT_NOTIFY`,
    `CHANNEL_BROADCAST_REQ`→`CHANNEL_BROADCAST_SEND_NOTIFY`).
  - `_SEND_NOTIFY` vs `_DELIVER_NOTIFY` — for broadcasts: `_SEND_NOTIFY` is
    sender→broker; `_DELIVER_NOTIFY` is broker→each recipient (disambiguates
    submission from fan-out).
- **Admission planes** (HEP-CORE-0036 / -0041):
  - Plane 1 (control) — BRC CURVE + ZAP, REG_REQ/ACK.
  - Plane 2 (channel-scope) — broker ACL / `VersionedAdmissionLedger`.
  - Plane 3 (data) — ZMQ per-peer CURVE allowlist; SHM AttachProtocol +
    SCM_RIGHTS capability.
- **Error categories** (`IMPLEMENTATION_GUIDANCE § Error Taxonomy`):
  - **Cat 1** — invariant violation → `CHANNEL_ERROR_NOTIFY` → role expected to
    stop; broker never repairs.
  - **Cat 2** — application-dependent → `CHANNEL_EVENT_NOTIFY` → informational;
    channel keeps running.

## 5. Do-not-confuse pairs

| A | B | The trap |
|---|---|---|
| `CHANNEL_ERROR_NOTIFY` (Cat 1) | `CHANNEL_EVENT_NOTIFY` (Cat 2) | Both are broker→participant notifications, but one means "stop" and the other means "FYI". A slot-checksum report is Cat 2, not Cat 1. |
| **Channel-bound** broadcast (`CHANNEL_BROADCAST_SEND_NOTIFY`) | **Band-bound** broadcast (`BAND_BROADCAST_SEND_NOTIFY`) | Different membership axes: "everyone registered on this data channel" vs "everyone who opted into this band". Neither superseded the other (HEP-CORE-0030 §9.1). |
| `_SEND_NOTIFY` (sender→broker) | `_DELIVER_NOTIFY` (broker→recipient) | Same broadcast, two hops. Asserting a test on the wrong one silently passes. |
| `disconnect()` | `stop()` | Connection axis vs lifecycle axis — see `feedback_api_conflation_connection_vs_lifecycle`. |
| `CHANNEL_NOTIFY_REQ` (retired) | `HUB_RELAY_MSG` (live) | Federation channel-event relay moved from the former to the latter (audit R3.6). The old handler is deleted; the old name now returns `UNKNOWN_MSG_TYPE`. |

## 6. Rename / retirement ledger

Every entry here is a place where a stale strike-through or old name could
mislead a reviewer.  Date = when the change was recorded.

| Old name | New name / state | Date | Reason |
|---|---|---|---|
| `HEARTBEAT_REQ` | `HEARTBEAT_NOTIFY` | 2026 (C13) | fire-and-forget → `_NOTIFY` suffix |
| `CHANNEL_BROADCAST_REQ` | `CHANNEL_BROADCAST_SEND_NOTIFY` | 2026-07-14 | suffix taxonomy; **NOT** superseded by bands |
| `CHANNEL_BROADCAST_NOTIFY` | `CHANNEL_BROADCAST_DELIVER_NOTIFY` | 2026-07-14 | disambiguate send vs deliver |
| `CHANNEL_NOTIFY_REQ` | **retired** → `UNKNOWN_MSG_TYPE` | 2026-05-17 (R3.6) | handler deleted; federation relay via `HUB_RELAY_MSG` |
| `ENDPOINT_ALREADY_SET` | **retired** | 2026-06-12 | post-bind ENDPOINT_UPDATE uses idempotent-if-same / `ENDPOINT_CHANGE_FORBIDDEN` |
| P2C Peer protocol (HELLO/BYE) | **removed** | — | eliminated; use `BAND_JOIN_NOTIFY`/`BAND_LEAVE_NOTIFY` (HEP-CORE-0030) |

**Residual old-name occurrences awaiting a sweep** (found 2026-07-17; these
still show pre-rename names as if current): `HEP-CORE-0022` (federation) lines
17, 51; `HEP-CORE-0033` (Hub Character) message catalog lines 1260, 3093, 3112;
`HEP-CORE-0015` line 524; `HEP-CORE-0023` line 713.

## 7. Keeping the registry honest — drift guard

The registry in §3 is prose and will rot the moment code changes, exactly like
the strike-throughs that motivated this HEP.  The guard is a **test**, not
discipline:

- **Source of truth for inbound messages:** `wire_dispatch.cpp kDispatchTable`
  (the gated set) plus the handful of handlers dispatched directly in
  `process_message` outside the tier table (`CHECKSUM_ERROR_REPORT`,
  `HUB_PEER_HELLO`/`_BYE`/`_HELLO_ACK`, `HUB_RELAY_MSG`, `HUB_TARGETED_MSG`).
- **The test** enumerates the msg_type literals the broker dispatches and
  asserts:
  1. every dispatched inbound msg_type has a §3 registry row (no undocumented
     message);
  2. every §3 inbound row names a msg_type the code actually dispatches (no
     phantom / stale-name row);
  3. renamed messages do NOT appear under their old name anywhere in the
     registry except the §6 ledger.
- On drift the test fails and points at the offending name, so a rename cannot
  land without updating this document.

Outbound (`B→`) and federation (`B↔B`) messages are emitted, not dispatched, so
they are covered by a companion assertion over the `send_to_identity` /
`send_reply` emission sites rather than `kDispatchTable`.

> **Implementation status:** the drift test is specified here and tracked in
> `docs/todo/MESSAGEHUB_TODO.md`; it is not yet written.  Until it lands, §3 is
> maintained by hand and the §2 caveat applies.
