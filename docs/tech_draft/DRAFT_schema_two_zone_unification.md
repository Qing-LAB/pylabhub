# DRAFT — Schema registry: one record, two un-merged zones (datablock + flexzone)

Status: DESIGN AGREED (2026-07-22), NOT YET IMPLEMENTED. Owner decisions locked
below. This is a schema-registry contract change (HEP-CORE-0034) + a small wire
change; implement as ONE focused pass with the full sweep as the correctness
gate (it touches the security-sensitive citation/matching path).

Related: HEP-CORE-0034 (schema registry), HEP-CORE-0002 §3.2.2 (SHM header keeps
`datablock_schema_hash` + `flexzone_schema_hash` independent). Investigation map
of the current system: see the 2026-07-22 subagent report in this session.

## The problem (root, stated correctly)

A named schema defines ONE channel data protocol built from up to TWO zones —
the **datablock** (slot) and the **flexzone** — either of which may be absent.
They are two halves of ONE record, NOT two separately-cited entities. But today
the record stores only the datablock's fields (`blds`) and a single **folded**
hash `BLAKE2b(canonical(slot, fz))`, throwing the flexzone's fields away. So
half the protocol is lost from the record, `SCHEMA_ACK` can't return a
flexzone, and a flexzone-only record is degenerate (empty `blds`).

## The agreed design

1. **One `SchemaRecord` per `(owner, id)` carrying both zones, un-merged.** Each
   zone has its own `{blds, packing, hash}`; a zone is ABSENT iff its `blds` is
   empty (datablock-only, flexzone-only, or both — at least one present).
2. **Two INDEPENDENT per-zone hashes** — `hash = BLAKE2b(canonical(that zone's
   blds) || packing)`, computed the same (zone-agnostic) way for both zones (so
   the same layout → same hash regardless of zone). The folded
   `canonical(slot, fz)` fingerprint is RETIRED. This mirrors HEP-0002's two SHM
   hashes.
3. **ONE unified construct/validate API** so logic is not patched all around:
   - `make_schema_record(owner, id, db_blds, db_packing, fz_blds, fz_packing)` —
     THE single builder; computes both hashes, un-merged; empty `blds` ⇒ absent
     zone (zero hash). Both `to_hub_schema_record` and the broker path-B builder
     route through it.
   - `schema_records_equivalent(a, b)` — THE single per-zone comparison (packing
     + fingerprint across both zones); used by registration idempotency and
     citation fingerprint-match.
   - `verify_request_fingerprint(blds, packing, claimed_hex)` — PER-ZONE
     self-consistency; callers invoke once per present zone.
4. **Citation matches per-zone** — a joiner is approved only when every present
   zone matches (datablock and flexzone independently; absent matches absent).
5. **`SCHEMA_REQ`/`SCHEMA_ACK` returns the whole record — both zones.** This
   fixes the original flexzone-loss bug. Ownership-aware (`owner`,`id`),
   zone-aware, transport-agnostic.
6. **`SCHEMA_REQ` sender: NOT wired in this change** (owner decision (ii)). The
   handler becomes correct now; wiring a role-side sender (runtime named-schema
   fetch) is a separate follow-up that touches role config/attach.
7. **Registration stays REG_REQ path-B** (no standalone SCHEMA_REG message in
   this change).

## Implementation checklist (sites, from the investigation)

Wire already carries both zones' BLDS (producer `flexzone_blds`/`flexzone_packing`;
consumer `expected_flexzone_*`). Only the HASH is folded today.

- [ ] `schema_record.hpp` `SchemaRecord`: keep `blds`/`packing`/`hash` as the
      DATABLOCK zone; ADD `flexzone_blds`/`flexzone_packing`/`flexzone_hash` +
      `has_datablock()`/`has_flexzone()`.
- [ ] `schema_utils.hpp`: add `make_schema_record(...)` + `schema_records_equivalent(...)`;
      make `verify_request_fingerprint` PER-ZONE (single-zone signature);
      `make_wire_schema_fields` computes per-zone hashes (`schema_hash` =
      datablock, new `flexzone_hash`); `WireSchemaFields` + both `apply_*`
      helpers carry `flexzone_hash` / `expected_flexzone_hash`; refactor
      `to_hub_schema_record` to call `make_schema_record`.
- [ ] `broker_service.cpp`: path-B builder → `make_schema_record` (~2323);
      inbox record → `make_schema_record` (~2428, datablock-only); per-zone
      self-consistency at the two `verify_request_fingerprint` sites (~2244,
      ~3356) incl. the consumer `joiner_hash` per-zone (~3358/3382);
      `SchemaCitationInput`/`ChannelSchemaInvariants` + the caller (~2130) + the
      channel-schema-set site (~2485) gain the flexzone hash; `SCHEMA_ACK`
      (~5424) + DISC_ACK (~2910) + `channel_to_json` (~6892) return both zones.
- [ ] `hub_state.cpp`: idempotency (~2256) → `schema_records_equivalent`;
      `_validate_schema_citation` (~2298) per-zone; channel-schema copy (~1238)
      carries the flexzone hash; `SchemaCitationInput` (hub_state.hpp ~1590) +
      `ChannelSchemaInvariants` (~354) gain `flexzone_hash`.
- [ ] HEP-CORE-0034: §4.1 record model (two zones), §6.3 fingerprint (two
      independent per-zone hashes; retire the folded `canonical(slot,fz)`),
      §10.1/§10.2 wire (`flexzone_hash`/`expected_flexzone_hash`), §10.3
      `SCHEMA_ACK` (both zones).
- [ ] Tests: record carries both zones (datablock-only, flexzone-only, both);
      `SCHEMA_REQ` returns both zones; per-zone citation match + flexzone-mismatch
      rejection; idempotency across both zones. Full sweep.
