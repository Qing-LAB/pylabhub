# DRAFT — Schema-validation consolidation (single validator, HEP-0034 I4)

**Status:** design for review. No code yet. When approved, the model here
merges into HEP-CORE-0034 (§9/§10/I4) and this draft is archived.
**Origin:** REVIEW_FullSystem_2026-07-20 finding — `_validate_schema_citation`
is defined but never called (task #68, item 1). Investigation showed the check
is *not* missing (it happens inline) but is **duplicated across several sites
that check different things**, and the dead function is the one the HEP names
as the single validator.

---

## 1. Two kinds of schema (this is the crux)

A channel's data schema arrives in one of two forms:

- **Named schema** — the role gives it a name (`schema_id`) and owner
  (`schema_owner` = the producer, or `"hub"` for a hub-wide schema). The broker
  stores it **in two places**: a registry record keyed by `(owner, id)`, *and*
  the channel's own invariants.
- **Role-provided schema** — the role sends only the raw structure
  (`schema_blds` + `schema_packing`), no name. The broker stores **only a hash
  on the channel** — *nothing goes into the registry* (broker_service.cpp:2313
  skips record creation when `schema_id` is empty).

**Consequence that decides the design:** a role-provided schema has **no
registry record**. So any checker that works by looking up `(owner, id)` in the
registry — which is exactly what the dead `_validate_schema_citation` does
(hub_state.cpp:2320, `schemas.find`) — **cannot validate a role-provided
schema**; it would call every one of them "unknown". The live code already
avoids this by checking against the **channel's stored schema** instead
(broker_service.cpp:3443/3453/3477/3515 all read `channel_entry`, never the
registry).

**Therefore the source of truth for "does a joiner match this channel" must be
the channel's stored invariants, not the registry.** The registry is consulted
only where a *name* is involved (existence + ownership of a named schema, and a
producer adopting a hub-wide schema).

---

## 2. What actually happens today (full site inventory)

Schema validation is spread across **six** code locations (producer main path,
producer early gate, producer inbox path, consumer path, `_on_producer_added`
backstop, and the dead `_validate_schema_citation`) doing **three** distinct
jobs — and the copies of the same job check *different fields*, which is a
latent bug, not just untidiness.

### Job A — request self-consistency: "does your structure hash to your claimed hash?"
Recompute the fingerprint from the wire fields, compare to the caller's own
claimed hash. Pure request sanity, no channel/registry involved.
- producer main path — broker_service.cpp:2341 → `FINGERPRINT_INCONSISTENT`
- consumer Named defense-in-depth — 3477 → `FINGERPRINT_INCONSISTENT`
- consumer Anonymous — 3507 → `FINGERPRINT_INCONSISTENT`
(3 copies. `compute_canonical_hash_from_wire` is already shared; the
compare-and-error wrapper is what's duplicated.)

### Job B — match against the channel: "does your schema match what the channel already has?"
- **consumer** — 3443/3453 (Named: `schema_id` + hash) and 3515 (Anonymous:
  recomputed hash) → `SCHEMA_ID_MISMATCH`, `SCHEMA_CITATION_REJECTED`
- **second producer (fan-in)** — 2239-2270, checks **hash only** →
  `SCHEMA_MISMATCH` (+ `CHANNEL_ERROR_NOTIFY` fan-out)
- **`_on_producer_added` backstop** — hub_state.cpp:1298-1304, checks
  **hash + id + blds + owner** → `RejectedMismatch`
- **⚠ inconsistency:** the producer early gate checks only the hash; the
  backstop checks four fields; the consumer checks id + hash. Same conceptual
  check, three different field sets. A joiner matching on hash but differing on
  `schema_id` slips the early gate and is caught later by the backstop under a
  different code — the copies have already drifted.

### Job C — named-registry check: "does the named (owner,id) record exist and match, and may you cite it?"
- producer hub-adoption (Path C) — broker_service.cpp:2373 (`schema("hub", id)`)
  → `SCHEMA_UNKNOWN`, `FINGERPRINT_INCONSISTENT`
- `_validate_schema_citation` (DEAD) — hub_state.cpp:2320, the cross-citation /
  owner / existence rules of HEP-0034 §9

### Writes (not validation) — already unified, keep as-is
`HubState::_on_schema_registered` is the **sole mutator** (HEP-0034 I1):
producer self-register (2411), inbox (2519), hub-globals loader (5537). No
change.

### Not part of this surface (verified, so we don't chase them)
- `broker_reg_handler.cpp` — the HEP-0046 Phase-B pipeline; does **no** schema
  validation (false hit; it references `_on_schema_registered` only in prose).
- `wire_dispatch.cpp` — `body_schema_violation` / `SCHEMA_REQ` / `SCHEMA_ACK`
  are **wire-envelope** validation and message-type names, a different concern
  from data-channel schema.
- `schema_utils.hpp` — the shared computation home (`compute_canonical_hash_
  from_wire`, `make_wire_fields`, `apply_*_fields`). This is where the new
  `verify_request_fingerprint` helper (Job A) belongs. Not a validation *site*.

### ⚠ The dead validator is NOT untested — it has its own L2 unit tests
`_validate_schema_citation` has **six** dedicated L2 tests
(`test_hub_state.cpp` → `HubStateSchemas.ValidateCitation_*`: SelfOwnedRecord,
HubGlobal, CrossCitation, FingerprintMismatch, UnknownSchema, UnknownOwner).
So the true situation is **two models, each with its own test surface**:
the §9 registry model is unit-tested at L2 against the (dead-in-production)
validator, while the §10.3 channel model is tested at L3 through the wire
(`SCHEMA_MISMATCH`, `SCHEMA_CITATION_REJECTED`, …). Consolidation must make
these two test surfaces describe **one** contract, not two.

---

## 3. The reconciled model

Three helpers, each owning one job. Every current call site routes through one
of them; the inline copies are deleted.

### (1) `verify_request_fingerprint(...)` — Job A (new shared pre-check)
Input: the wire structure fields (`blds`, `packing`, flexzone) + the caller's
claimed hash (may be empty). Recompute via the existing
`compute_canonical_hash_from_wire`; if a claimed hash was given and differs,
fail. Returns the recomputed hash on success.
Wire codes it owns: `MISSING_BLDS*`, `MISSING_PACKING*`, `MISSING_HASH*`,
`FINGERPRINT_INCONSISTENT`. Called by producer, consumer-Named-DiD,
consumer-Anonymous. Not a citation — a request-sanity gate that runs first.

### (2) `_validate_schema_citation(...)` — Jobs B **and** C (the single validator, HEP I4)
This is the function the HEP already names. It becomes the one place a joiner's
schema is checked. **Source of truth = the channel's stored invariants**, with
the registry layered on only for named schemas:

- Always: recomputed/claimed hash must equal `channel.schema_hash`.
- If the channel is **named** (has `schema_id`/`schema_owner`): the joiner's
  `schema_id` must equal `channel.schema_id`; and (for citations of a named
  record) the registry `(owner, id)` must exist, its fingerprint match, and the
  owner be `"hub"` or a producer of the channel (the §9 cross-citation rule).
- If the channel is **role-provided** (no name): hash match is the whole check.

Producers and consumers both call it. The `_on_producer_added` backstop stays
as a defense-in-depth assertion but is no longer the primary check (or is
reduced to an assert, TBD — see open decision D2).

### (3) `_on_schema_registered(...)` — writes, unchanged.

### Outcome → wire-code mapping (the wire contract is FROZEN — every code stays)

| Situation | Reason (extended enum) | Wire code today | Kept |
|---|---|---|---|
| structure ≠ claimed hash | *(pre-check, Job A)* | `FINGERPRINT_INCONSISTENT` | ✅ |
| missing blds/packing/hash | *(pre-check, Job A)* | `MISSING_*` (5 variants) | ✅ |
| joiner hash ≠ channel hash (role-provided or anon) | `kFingerprintMismatch` | `SCHEMA_CITATION_REJECTED` (consumer) / `SCHEMA_MISMATCH` (producer) | ✅ via caller-tag |
| joiner `schema_id` ≠ channel id | `kSchemaIdMismatch` *(NEW)* | `SCHEMA_ID_MISMATCH` | ✅ |
| named record `(owner,id)` absent | `kUnknownSchema` | `SCHEMA_UNKNOWN` | ✅ |
| cited owner not hub/channel-producer | `kCrossCitation` | *(currently unreachable on wire)* | n/a |
| producer claims foreign owner (register) | *(mutator, `_on_schema_registered`)* | `SCHEMA_FORBIDDEN_OWNER` | ✅ |
| second producer, existing differs | `kFingerprintMismatch`/`kSchemaIdMismatch` | `SCHEMA_MISMATCH` | ✅ |

The current `CitationOutcome::Reason` (5 values) can't express all of these — it
lacks `kSchemaIdMismatch`. **The validator returns a neutral reason; each handler
maps it to its own wire code** (the producer handler maps `kFingerprintMismatch`
→ `SCHEMA_MISMATCH`, the consumer handler → `SCHEMA_CITATION_REJECTED`). The
validator stays context-free — it does not know or care who called it; the
direction (D3) lives at the call site. This is cleaner than tagging the outcome.

**`FINGERPRINT_INCONSISTENT` is Job A only (confirmed).** The consumer "named
defense-in-depth" check compares the recomputed hash to the *channel* hash — but
it only runs after the claimed hash has already been shown equal to the channel
hash, so it reduces to recomputed-vs-claimed = self-consistency. Therefore
`compute_canonical_hash_from_wire` stays in the **one** pre-check
(`verify_request_fingerprint`); the validator compares already-computed hashes
only and never recomputes. This keeps the "no bypass" guard exact.

**Processor is not a separate site.** `PROC_REG_REQ` is handled by
`handle_consumer_reg_req` (broker_service.cpp:1680) — consolidating the consumer
path covers consumer **and** processor together.

---

## 4. Open decisions for you (the designer)

- **D1 — field set for the channel match.** → **DECIDED: unify to the superset**
  (hash always; id + owner + blds when the channel is named), applied
  consistently at every site. Fixes the current inconsistency (producer early
  gate checks only hash today); the producer gate now rejects an id-mismatch it
  previously let slip to the backstop.
- **D2 — `_on_producer_added` backstop.** → **DECIDED: keep as a tripwire** — it
  re-asserts the invariants at the point of state mutation and logs ERROR if it
  ever fires (it never should once the front door is authoritative).
- **D3 — `SCHEMA_MISMATCH` vs `SCHEMA_CITATION_REJECTED`.** → **DECIDED: keep
  both** — they name the direction of data flow (producer-in vs consumer-out);
  converging would be a wire change. Mapped at the call site, not inside the
  validator.
- **D4 — check ordering / error precedence.** → **DECIDED: use the clean
  canonical order** (self-consistency → channel → named). The order in which
  errors surface for a request that fails *more than one* check was never part
  of any contract — a well-formed client never sees the difference, and no wire
  spec pins it. The three-question order in §9 is the model. The only mechanical
  check at implementation time: if some existing test happens to assert a
  two-failure ordering, update that test — it was pinning an accident, not a
  contract.

---

## 5. Acceptance criteria (your two requirements)

1. **Docs consistent, and the HEP explains the validation *process* while
   *citing* the schema design — not redefining it.**
   - HEP-0034 §9 (owner/citation rules) and §10.3 (consumer/role-provided
     validation) are rewritten as **one** model: channel invariants are the
     source of truth; registry lookup is layered on for named schemas; request
     self-consistency is a separate pre-check. The module map (I4 "single
     validator") becomes true.
   - HEP-0034 adds a plain-language **"Schema validation process"** section
     walking a REG_REQ / CONSUMER_REG_REQ through the three jobs
     (self-consistency → channel match → named-registry check) in order, so a
     reader sees exactly how a schema is checked.
   - **What a schema *is* is cited, not redefined.** The authoritative source
     for schema structure is **HEP-CORE-0002 (DataHub)** — `SchemaInfo` and the
     BLDS format. HEP-0034 references it (it already `Depends on` HEP-0002) and
     does not restate the layout. HEP-0016 is superseded (already noted) and is
     not a live source.
   - **Two hash forms kept distinct (HEP-0034 §2.4 I6).** Validation operates on
     the **registry/wire canonical form** (`compute_canonical_hash_from_wire`,
     the `attempted_schema` / `expected_schema_hash` wire fields), NOT
     `SchemaInfo::hash` (the SHM-header form, HEP-0002). The rewrite and the new
     `verify_request_fingerprint` helper state this explicitly so the two forms
     are never conflated.
2. **No site bypasses the unified functions.** After the change:
   - `compute_canonical_hash_from_wire` is called **only** inside
     `verify_request_fingerprint` (Job A).
   - the registry (`schemas.find` / `hub_state_->schema(...)`) and
     channel-invariant schema comparisons appear **only** inside
     `_validate_schema_citation` (Jobs B+C) and the mutator.
   Enforced by a grep/clang-query guard in review + a test asserting each wire
   code still fires from its scenario.

---

## 5b. This is ONE job (not producer-fix + consumer-fix)

Producer and consumer are the **same** operation — "does a joiner's schema match
this channel?" — reached from different handlers. They share Job A
(self-consistency), Job B (channel match), and Job C (named-registry). Splitting
the work by caller would just recreate the duplication we're removing. So the
job is defined by the **three shared functions**, and every current site
(consumer REG, producer REG early gate, producer named path, producer adopt,
inbox, `_on_producer_added` backstop) is migrated onto them together, in one
change, verified against one contract.

## 5c. Complete contract surface — code sites × tests

The correctly-designed contract = these behaviors, each with its pinning test.
"Disposition" is what happens to the test when we consolidate.

### Job A — request self-consistency (`verify_request_fingerprint`)
| Behavior | Pinning test(s) | Disposition |
|---|---|---|
| fingerprint includes packing / flexzone; wire-form hash correctness; parse errors; make/apply wire fields | L2 `test_schema_validation.cpp` (~34 tests: `FingerprintIncludesPacking_*`, `WireForm_*`, `MakeWireFields_*`, `ApplyProducer/ConsumerFields_*`, `Parse*`) | **Keep green** — the helper reuses this exact computation; tests unchanged |
| structure ≠ claimed hash → `FINGERPRINT_INCONSISTENT` | L3 (via wire) — e.g. producer/consumer paths; covered indirectly by broker sch tests | **Keep**; add a direct per-code test |

### Job B — channel match (unified `_validate_schema_citation`, channel-first)
| Behavior | Pinning test(s) | Disposition |
|---|---|---|
| 2nd producer, different hash → `SCHEMA_MISMATCH` | L3 `broker_schema_mismatch` / `DatahubBrokerTest.SchemaMismatch`; L3-p4 `DuplicateReg_DifferentSchemaHash_Rejected` | **Keep green** (frozen wire code) |
| consumer id match → success | L3-p4 `ConsumerSchemaIdMatch_Succeeds`; L3 `broker_sch_consumer_citation_match` | **Keep green** |
| consumer id mismatch → `SCHEMA_ID_MISMATCH` | L3-p4 `ConsumerSchemaIdMismatch_Fails` | **Keep green** |
| consumer vs role-provided (empty producer id) | L3-p4 `ConsumerSchemaIdEmptyProducer_Fails` | **Keep green** — this is the role-provided path |
| consumer wrong packing → `SCHEMA_CITATION_REJECTED` | L3 `broker_sch_consumer_citation_mismatch` / `Sch_ConsumerCitationMismatch` | **Keep green** |
| no-packing backward-compat still `SCHEMA_MISMATCH` | L3 `Sch_NoPackingBackwardCompat` | **Keep green** |
| schema_id/owner round-trip stored on channel | L3-p4 `SchemaIdStoredOnReg`, `SchemaHashStoredOnReg`; L3 `Sch_SchemaReq_OwnerIdRoundTrip` | **Keep green** |
| 2nd producer mismatch leaves state unmutated | L2 `HubStateProducerAdmission.SecondProducer_SchemaMismatch_Rejected_NoStateMutation` | **Keep green** — pins the backstop; becomes the assert |

### Job C — named-registry check (the §9 rules, now inside the unified validator)
| Behavior | Pinning test(s) | Disposition |
|---|---|---|
| self-owned record OK; hub-global OK | L2 `ValidateCitation_SelfOwnedRecord_Ok`, `ValidateCitation_HubGlobal_Ok` | **Rework** to the unified entry point; behavior preserved |
| cross-citation rejected | L2 `ValidateCitation_CrossCitation_Rejected` | **Rework**; behavior preserved |
| fingerprint / unknown-schema / unknown-owner rejected | L2 `ValidateCitation_FingerprintMismatch_Rejected`, `_UnknownSchema_Rejected`, `_UnknownOwner_Rejected` | **Rework** + map to wire codes; behavior preserved |
| producer adopt hub-global absent → `SCHEMA_UNKNOWN` | L3 `broker_sch_path_c_unknown_global` | **Keep green** |
| producer foreign owner → `SCHEMA_FORBIDDEN_OWNER` | L3 `broker_sch_path_x_forbidden_owner` | **Keep green** |

### Writes (mutator, unchanged)
| Behavior | Pinning test(s) | Disposition |
|---|---|---|
| create / idempotent / hash-mismatch-self / packing-mismatch / empty-owner / namespace-by-owner / eviction / cascade | L2 `HubStateSchemas.OnSchemaRegistered_*` (6), `OnSchemasEvicted*`, `RoleDisconnect_Cascade`, `ConflictPolicy_*` | **Keep green** — mutator untouched |
| self-register / inbox record → `SCHEMA_HASH_MISMATCH_SELF` | L3 `broker_sch_record_hash_mismatch_self`, `broker_sch_inbox_hash_mismatch_self`, `Sch_RegHashMismatchSelf`, `Sch_InboxHashMismatchSelf`; inbox path A / idempotent | **Keep green** |

**The convergence requirement:** the L2 `ValidateCitation_*` tests (§9 model) and
the L3 wire tests (§10.3 model) must, after consolidation, exercise the **same**
validator and agree. Any L2 case whose expectation contradicts an L3 wire code
is a place the two models disagree today — those are the exact points the
reconciled contract must resolve (tracked as the reason→code mapping in §3).

**No-new-hole guard:** after the change, a test asserts every wire code in §3's
table still fires from its scenario, and a grep/clang-query check proves
`compute_canonical_hash_from_wire`, `schemas.find`, and channel-schema
comparisons appear only inside the three unified functions.

## 6. Implementation order (after this doc is approved)

1. Rewrite HEP-0034 §9/§10/I4 to the reconciled model (design first).
2. Extend `CitationOutcome` + add `verify_request_fingerprint`.
3. Make `_validate_schema_citation` channel-first (Jobs B+C); wire the caller
   tag for the code mapping.
4. Migrate all six sites (consumer path, producer main self-consistency,
   producer early gate, producer adopt Path-C, producer inbox path,
   `_on_producer_added` backstop→assert) onto the shared helpers.
5. Test per wire code + the no-bypass guard. Full build + full ctest.
