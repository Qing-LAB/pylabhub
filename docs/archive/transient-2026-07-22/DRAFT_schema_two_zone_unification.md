# DRAFT — Schema registry: one record, two un-merged zones (datablock + flexzone)

Status: **IMPLEMENTED + VERIFIED (2026-07-22)** — folded into HEP-CORE-0034
(§2.2, §4.1, §6.3 with diagrams/examples, §9 flow + worked example, §10, §11.4)
and archived. Green at 2639/2639. As-built deviations from the checklist below:
(1) the "inbox record → make_schema_record" item was **superseded** — the
inbox-as-schema-record was removed entirely (HEP-0034 §11.4 / HEP-0027 §4.0);
(2) `flexzone_packing` was intentionally NOT added to the channel structs —
packing folds into the fingerprint, so only `flexzone_blds` is carried;
(3) per-zone rejection-detail labeling was implemented (`hub_state.cpp`
`mismatch_zone`). Original design record below.

Related: HEP-CORE-0034 (schema registry), HEP-CORE-0002 §3.2.2 (SHM header keeps
`datablock_schema_hash` + `flexzone_schema_hash` independent).

## The problem (root, stated correctly)

A named schema defines ONE channel data protocol built from up to TWO zones —
the **datablock** (slot) and the **flexzone** — either of which may be absent.
They are two halves of ONE record, NOT two separately-cited entities. But today
the registry stores only the datablock's fields (`blds`) and a single **folded**
hash `BLAKE2b(canonical(slot, fz))`, throwing the flexzone's fields away. So
half the protocol is lost from the record, `SCHEMA_ACK` can't return a
flexzone, and a flexzone-only record is degenerate (empty `blds`).

## Two hashes, two jobs — do NOT conflate them

The word "schema hash" covers two mechanisms in different layers. This change
touches ONLY Job 2.

- **Job 1 — data-plane `schema_tag` (unchanged, OUT OF SCOPE).** Every ZMQ data
  message carries an 8-byte tag = first 8 bytes of the **folded** whole-protocol
  hash (`hub_zmq_queue.hpp`), a per-message drift tripwire. It is produced by
  `compute_schema_hash(slot, fz)` in `role_api_base` and must stay folded (a
  message is valid only if BOTH zones agree). **Do not touch `compute_schema_hash`,
  `make_schema_tag`, or `role_api_base`.**
- **Job 2 — registry / citation fingerprint (THIS change).** The control-plane
  value stored in `SchemaRecord`, carried on REG_REQ / CONSUMER_REG_REQ as
  `schema_hash` / `expected_schema_hash`, matched at registration and join, and
  returned by `SCHEMA_ACK`. This is where folding loses the flexzone. We split it
  per-zone.

Job 1 and Job 2 already run through separate code paths, so the split is clean.

## The agreed design

1. **One `SchemaRecord` per `(owner, id)` carrying both zones, un-merged.** Keep
   `blds`/`packing` as the DATABLOCK content; ADD `flexzone_blds`/`flexzone_packing`
   as the FLEXZONE content (needed so `SCHEMA_ACK` can reconstruct each zone). A
   zone is ABSENT iff its `blds` is empty. `has_datablock()` / `has_flexzone()`
   key off `blds.empty()`.

2. **ONE double-width fingerprint — 64 bytes, `datablock_half ‖ flexzone_half`.**
   Not two parallel fields — the existing 32-byte / 64-hex `schema_hash` is simply
   **widened** to 64 bytes / 128 hex, everywhere it already lives.
   - Each half = `BLAKE2b(zone_blds || "|pack:" || zone_packing)` over ONLY that
     zone's own fields (NO `slot:`/`fz:` prefix — zone-agnostic per half).
   - An ABSENT zone's half is **all-zero** (32 zero bytes). A real zone hashing to
     all-zero is a 1-in-2²⁵⁶ impossibility, so zero unambiguously means "absent".
   - Presence is self-encoded by position: datablock-only = `H(db)‖0`, flexzone-only
     = `0‖H(fz)`, both = `H(db)‖H(fz)`. "Layout L in the datablock" ≠ "layout L in
     the flexzone" (correct — different protocols).
   - **INVARIANT — the full 64-byte fingerprint MUST NOT be all-zero** (at least
     one zone present). Assert this in the builder; reject an all-zero wire claim
     with `INVALID_SCHEMA` (a REG_REQ with a `schema_id` but neither zone).

   Why one widened value beats two parallel fields: a single `fp_a == fp_b` compares
   BOTH zones atomically — it is structurally impossible to forget the flexzone at
   a comparison site. Two parallel fields would require every compare/copy site to
   remember the sibling; one miss = a silent flexzone bypass (the "stale silent
   fallback" contract violation).

3. **Retire the folded fingerprint IN THE REGISTRY/CITATION PATH only.** The Job-2
   folded form (`compute_canonical_hash_from_wire`, and `compute_schema_hash` where
   it feeds the registry/wire) is replaced by the per-half builder below. Job 1's
   data-plane `schema_tag` stays folded (§ above).
   - **Not the same bytes as the SHM-header hash.** This mirrors HEP-0002's two SHM
     hashes only *structurally*. The registry fingerprint's datablock half is over
     `canonical_fields_str`, while the SHM `datablock_schema_hash` is
     `SchemaEntry::slot_info.hash` — a different canonical form. Do NOT unify them;
     that would be a Core-Structure change.

4. **ONE unified construct/validate API** so logic is not patched all around:
   - `compute_zone_hash(blds, packing) -> array<uint8_t,32>` — THE per-zone hasher;
     empty `blds` ⇒ all-zero (absent).
   - `make_schema_record(owner, id, db_blds, db_packing, fz_blds, fz_packing)` — THE
     single builder; fills content + the 64-byte fingerprint; asserts not-all-zero.
     Both `to_hub_schema_record` and the broker path-B builder route through it.
   - `schema_records_equivalent(a, b)` — THE single comparison = fingerprint(64)
     equality (packing folds into each half). Used by registration idempotency and
     citation fingerprint-match.
   - `compute_fingerprint_from_wire(db_blds, db_packing, fz_blds, fz_packing) -> array<uint8_t,64>`
     — recomputes the `db‖fz` fingerprint from wire fields (replaces the folded
     `compute_canonical_hash_from_wire`; same four args, wider return).
   - `verify_request_fingerprint(db_blds, db_packing, fz_blds, fz_packing, claimed_128hex)`
     — KEEPS its wire signature; return widens to `{array<uint8_t,64> fingerprint,
     bool consistent}` and internals call `compute_fingerprint_from_wire`. The
     broker's two call sites barely change (same args, 64-byte result covering both
     zones at once).

5. **Citation matches via the single 64-byte compare.** A joiner is approved only
   when its fingerprint equals the channel's — which inherently requires every
   present zone (and every absent zone) to match. For diagnostics only, slice the
   halves to say "datablock" vs "flexzone" in the rejection detail.

6. **`SCHEMA_REQ`/`SCHEMA_ACK` returns the whole record — both zones' `blds`/`packing`
   + the 128-hex fingerprint.** This fixes the original flexzone-loss bug.
   Ownership-aware (`owner`,`id`), zone-aware, transport-agnostic.

7. **`SCHEMA_REQ` sender: NOT wired in this change** (owner decision (ii)). The
   handler becomes correct now; wiring a role-side sender (runtime named-schema
   fetch) is a separate follow-up.

8. **Registration stays REG_REQ path-B** (no standalone SCHEMA_REG message).

## Wire change (minimal)

No new wire field. The existing `schema_hash` (producer) and `expected_schema_hash`
(consumer) simply widen from 64-hex to **128-hex** (the 64-byte fingerprint). Both
zones' `blds`/`packing` are already on the wire (`schema_blds`/`schema_packing` +
`flexzone_blds`/`flexzone_packing`); the broker recomputes each half from them and
compares to the claimed 128-hex value.

## Implementation checklist (sites, from the investigation)

- [ ] `schema_record.hpp` `SchemaRecord`: keep `blds`/`packing` (datablock); ADD
      `flexzone_blds`/`flexzone_packing` (content); WIDEN `hash` to
      `std::array<uint8_t,64>` (the `db‖fz` fingerprint); add `has_datablock()`
      / `has_flexzone()` (off `blds.empty()`). Update the `SchemaRegOutcome` /
      `CitationOutcome` doc comments (hash is now 64 bytes).
- [ ] `schema_utils.hpp`: add `compute_zone_hash(...)`, replace
      `compute_canonical_hash_from_wire` with `compute_fingerprint_from_wire(...)`
      (`db‖fz`, array<64>), add `make_schema_record(...)` (asserts fingerprint ≠
      all-zero), `schema_records_equivalent(...)`; `verify_request_fingerprint`
      keeps its 4-arg-wire signature, return widens to a 64-byte fingerprint;
      `make_wire_schema_fields` emits the 128-hex fingerprint in `schema_hash`
      (no separate flexzone hash field); `WireSchemaFields.schema_hash` widens;
      refactor `to_hub_schema_record` to call `make_schema_record`. Leave
      `compute_schema_hash` in place — it still serves Job 1's `schema_tag`.
- [ ] `broker_service.cpp` helper `hex_to_hash_array` returns `array<32>`; add a
      64-byte sibling (`hex_to_fingerprint_array`) for the widened
      `channel_hash`/`expected_hash`/`joiner_hash` sites.
- [ ] Structs (hub_state.hpp): WIDEN the fingerprint types — `SchemaCitationInput`
      `channel_hash` + `expected_hash` → `array<uint8_t,64>` (~1590);
      `ChannelSchemaInvariants.schema_hash` (~352) + `ChannelEntry.schema_hash`
      (~583) are hex `std::string` → now 128-char. `ChannelEntry` also gains
      `flexzone_blds`/`flexzone_packing` (content, for the SCHEMA_ACK/DISC_ACK
      channel-name branch). NO new `flexzone_hash` field anywhere — the widened
      single fingerprint carries both zones.
- [ ] `broker_service.cpp`: REG_REQ self-consistency (~2244) recomputes the 64-byte
      fingerprint from both zones' wire fields and checks it equals the claimed
      128-hex `schema_hash` (reject all-zero → `INVALID_SCHEMA`); path-B builder
      (~2323) → `make_schema_record`; inbox record (~2428) → `make_schema_record`
      (datablock-only, so `H(inbox)‖0`); Path-C adoption (~2280) uses the 64-byte
      `h_recomputed`; citation caller (~2130) sets the 64-byte `channel_hash` /
      `expected_hash`; consumer citation (~3356) recomputes the 64-byte fingerprint
      incl. `expected_flexzone_*`, sets 64-byte `joiner_hash` (~3363/3368/3380);
      channel-schema-set (~2485) copies the 128-hex fingerprint (+ flexzone content)
      into `ChannelSchemaInvariants`; `SCHEMA_ACK` BOTH branches — owner+id (~5424)
      AND channel_name (~5454-5460) — return both zones' content + the 128-hex
      fingerprint; DISC_ACK (~2910) returns the 128-hex fingerprint; the schema
      **registry** snapshot serializer (`mc::kSchema` block, ~6892 — NOT
      `channel_to_json`) emits both zones' `blds`/`packing` + 128-hex hash.
      Observability: channels-list dump (~6538) + `ChannelSnapshotEntry` (~6559)
      read `entry.schema_hash` — now 128-hex automatically.
- [ ] `hub_state.cpp`: idempotency (~2256) → `schema_records_equivalent`;
      `_validate_schema_citation` (~2298) compares the 64-byte `expected_hash` vs
      `channel_hash` (step-2a, ~2321) and the registry record's 64-byte hash
      (step-3, ~2379) — single compares now cover both zones; slice halves only for
      the rejection detail; channel-schema copy (~1238) copies the 128-hex
      fingerprint (+ flexzone content) into `ChannelEntry`.
- [ ] HEP-CORE-0034: §4.1 record model (two zones + one 64-byte fingerprint), §6.3
      fingerprint (per-half `BLAKE2b(blds||"|pack:"||packing)`, `db‖fz`, absent=zero,
      not-all-zero invariant; retire the folded Job-2 form; note Job-1 `schema_tag`
      stays folded), §10.1/§10.2 wire (`schema_hash`/`expected_schema_hash` widen to
      128-hex), §10.3 `SCHEMA_ACK` (both zones).
- [ ] Tests: record carries both zones (datablock-only, flexzone-only, both);
      fingerprint halves correct + absent-zone half is zero + all-zero rejected;
      `SCHEMA_REQ` returns both zones; citation match + flexzone-mismatch rejection
      (wrong flexzone with right datablock must reject); idempotency across both
      zones. Update L2 `test_schema_validation.cpp` + L3 workers pinning the old
      folded form. Full sweep.
