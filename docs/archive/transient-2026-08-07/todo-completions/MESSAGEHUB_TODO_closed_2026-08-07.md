# MESSAGEHUB_TODO — closed work, extracted 2026-08-07

`docs/todo/MESSAGEHUB_TODO.md` was 865 lines. Two sections accounted for 438
of them and both were **landed narrative** — 24 `✅` markers between them and
two residual `⏳` items, both of which turned out to be closed as well.

Prose is in git (`git show fd773e97:docs/todo/MESSAGEHUB_TODO.md`). This file
records the verdicts and the evidence.

---

## 1. Schema / metrics query integration — SHIPPED, design has a permanent home

The 2026-07-26 wire-inventory audit found `SCHEMA_REQ`/`SCHEMA_ACK` and
`METRICS_REQ`/`METRICS_ACK` were complete broker-side mechanisms with no
client half — the only two HEP-0033 Class-C queries whose client plumbing
was never built. Owner ruling: integrate, don't delete.

All four slices shipped 2026-07-26. The design draft was archived to
`transient-2026-07-27/tech_drafts/` and its permanent home is HEP-0034
(§2.4, §6.4, §10.2, §10.3a, §11.1) + HEP-0007 §12.3/§12.4a + HEP-0036 §5b.7.

What shipped, in one line each: open-row validation (`SCHEMA_REQUIRED` on
material-free fan-in open, owner-citation self-consistency, anonymous
producer structure⇒hash); query plumbing (`get_schema` /
`get_channel_schema` / `get_channel_metrics` on the BRC, RoleAPI
pass-throughs, both messages moved to the `Control_EnvelopeWithRoleUid`
tier so the caller `role_uid` is identity-bound, channel-form member
gating, the all-channels METRICS wire branch retired); plus slices 1c and
2–4.

**Residual, still open:** typed `SchemaReqBody` / `MetricsReqBody` were left
on the HEP-0046 EnvelopeOnly→typed follow-on list. Verified 2026-08-07 —
neither exists in `wire_bodies.hpp` (only `AdminQueryMetricsReqBody` does).
Tracked under **#82**, and worth doing in the same pass as **#131**.

## 2. HEP-CORE-0046 REG protocol redesign — Phase B COMPLETE

`WireEnvelope` + typed body classes, 46 L1 tests, shared gate runners, all
nine REG-family handlers converted, the `to_legacy` bridge and
`BrokerRegHandler` skeleton retired, `inbox_schema_json` replaced by a typed
`SchemaSpec` boundary-parse. Task #57.

**The one `⏳` residue in 265 lines is closed.** It said the BRC's
`channel_auth_applied` still wrote a duplicate `producer_role_uid = role_uid`
"for pre-amendment brokers" with no reader left. Verified against
`broker_request_comm.cpp:1215-1231`: the function now writes exactly five
fields and the comment states *"The historical `producer_role_uid` alias is
retired — neither written here nor read by the broker."* Retired per the
HEP-CORE-0042 §5.5.2 strict-wire amendment of 2026-07-24.

(`consumer_attach_zmq` at `:1298` does still set `producer_role_uid`, but
that is `CONSUMER_ATTACH_REQ_ZMQ`, a different message, itself slated for
retirement under topology Phase E. Not the same item.)

## 3. Notify / broadcast doc-consistency — DONE 2026-07-17

`CHANNEL_ERROR_NOTIFY` (Cat 1) vs `CHANNEL_EVENT_NOTIFY` (Cat 2) unified in
HEP-0007; channel-bound broadcast documented as **renamed, not removed**
(`CHANNEL_BROADCAST_REQ`→`_SEND_NOTIFY`,
`CHANNEL_BROADCAST_NOTIFY`→`_DELIVER_NOTIFY`); HEP-0030 §9.1 coexistence
table corrected. The wider rename sweep (`HEARTBEAT_REQ`→`HEARTBEAT_NOTIFY`,
`BAND_BROADCAST_REQ`→`_SEND_NOTIFY`) swept eleven HEPs plus
`IMPLEMENTATION_GUIDANCE.md`, with historical quotes deliberately left
intact. Dead `HeartbeatAckBody` deleted.

Two findings from that sweep survive and moved to the live file: the phantom
`api.notify_hub` in HEP-0022 §8.2, and the untyped notify bodies (**#131**).

## 4. `_REQ` frame half-mix audit — suspects verified clean, sweep overtaken

Filed 2026-05-21 after `ENDPOINT_UPDATE_REQ` was found in the prohibited
half-mix shape (broker emitted an `_ACK`, client dropped it). The item asked
for a scan of every `_REQ` frame, naming three suspects: `send_broadcast`,
`send_checksum_error`, `send_heartbeat`.

**Verified 2026-08-07: none of `BROADCAST_ACK`, `CHECKSUM_ERROR_ACK`, or
`HEARTBEAT_ACK` exists anywhere in `src/`.** All three are genuinely
fire-and-forget on both ends.

The general sweep has also been largely overtaken: the 2026-07-17 rename pass
converted the misleadingly-named fire-and-forget `_REQ` frames to `_NOTIFY`,
which is the shape contract expressed in the name, and HEP-0046 Phase B typed
the whole REG family. Not carried forward as standing work — a specific
half-mix, if one is found, is a bug report on the spot.

## 5. Other closed items

- **D2 drift B1** — empty `correlation_id` in BAND_JOIN/LEAVE validator
  errors. Fixed; verified 2026-06-27 at `broker_service.cpp:5540`/`:5651`.
- **`RoleAPIBase::close_all_inbox_clients()`** — listed as a dead-code
  candidate with zero callers. Verified 2026-08-07: the symbol does not
  exist anywhere in `src/` or `tests/`. Already deleted.
- **Wave-M2 / Wave-M2.5 / Wave-M3 side-arcs, M1.2 / M1.4 / M1.5 / MD1 /
  MD1.5** — all closed.
- **Arc B role-host renovation (Wave-B M0..M9)** — closed 2026-05-26.
