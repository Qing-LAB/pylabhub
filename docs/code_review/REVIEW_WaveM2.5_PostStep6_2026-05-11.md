# Wave M2.5 Post-Step-6.5 Audit — Data, API, Logic, Consistency

**Date:** 2026-05-11
**Branch:** `feature/lua-role-support` (post `7038669`)
**Scope:** Full audit of the post-step-6.5 state.  Tests: 1800/1800 passing.

Audits performed:
- Data-shape correctness (`ChannelEntry`, `ProducerEntry`, `ConsumerEntry`, `ChannelMetrics`, `ChannelSnapshotEntry`)
- API completeness + coherence (`ChannelEntry` methods, HubState ops)
- Mixed patterns / obsolete code residue
- Duplicated code / simplification candidates
- Code logic vs HEP-CORE-0007 / 0021 / 0023 / 0033 / 0034 documentation
- Potential errors (ordering, races, lifecycle)

---

## Findings

### G1 — `ChannelMetrics::producer` is single-producer scalar  ❌ BUG (same class as the original M2.5 motivation)

**Severity:** Bug.  Multi-producer correctness regression for the metrics
plane.  Identical shape to the overwrite-class bug M2.5 just
eliminated on `ChannelEntry`.
**Files:**
- `src/utils/ipc/broker_service.cpp:239-243` — `ChannelMetrics` struct.
- `src/utils/ipc/broker_service.cpp:3743-3745` — `update_producer_metrics`
  writes to the single field.
- `src/utils/ipc/broker_service.cpp:3796-3801` — `query_metrics` reads it.

**Problem:** `ChannelMetrics::producer` is a single `ParticipantMetrics`,
not a map keyed by `role_uid`.  The consumer side is correctly keyed:
`std::unordered_map<std::string, ParticipantMetrics> consumers`.  For
Fan-In channels, every producer's metrics report overwrites the previous
one.  `query_metrics` returns the last reporter's blob only.

**Evidence in code (`update_producer_metrics`):**
```cpp
cm.producer.pid         = pid;       // last reporter wins
cm.producer.last_report = now;
cm.producer.data        = metrics;   // last reporter's blob
```

**Effect:** Admin clients reading metrics for a Fan-In channel see a
random producer's metrics (whichever reported most recently).  The
multi-producer admission paths (steps 3-6) work correctly; metrics
reporting silently masks the multi-producer state.

**Fix (proposed):** mirror the consumer side —
```cpp
struct ChannelMetrics {
    std::unordered_map<std::string, ParticipantMetrics> producers;   // keyed by role_uid
    std::unordered_map<std::string, ParticipantMetrics> consumers;
};
```
`update_producer_metrics(channel, uid, metrics, pid)` keyed by uid.
`query_metrics` returns a tree keyed by uid (same shape decided for
DISC_REQ_ACK metadata in design §6.1).

**Tracked under:** `TODO_MASTER.md` M1.4 "retire `metrics_store_` legacy".
M1.4 was scheduled "after M2.5 closes."  This finding documents what
the retirement should look like.

---

### G2 — 17 test files still set obsolete REG_REQ wire keys  ⚠️ TEST CRUFT

**Severity:** Cosmetic.  Tests pass because the broker silently drops
unknown keys; the cruft just clutters the test source.
**Files:** all under `tests/test_layer3_datahub/`:
- `test_hub_lua_integration.cpp:818-820`
- `test_datahub_hub_host_integration.cpp:104-106`
- `test_datahub_channel_access_policy.cpp:116-118`
- `test_datahub_zmq_endpoint_registry.cpp:138-140`
- `test_datahub_broker_admin.cpp:138-140`
- `test_datahub_broker_protocol.cpp:187-189` (+ `make_reg_opts` helper)
- `test_datahub_metrics.cpp:138-140`
- `test_datahub_broker_schema.cpp:158-160`
- `test_datahub_broker_consumer.cpp:142-144`
- `workers/datahub_broker_workers.cpp:302-303` etc.
- `workers/datahub_e2e_workers.cpp:189-190`

**Problem:** Each sets `opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0"; opts["zmq_pubkey"] = "";`
— three placeholder keys that the broker no longer reads after step 2c.

**Fix:** sweep delete the three lines from each test file.  Pure
cosmetic; no behaviour change.  Optionally consolidate any duplicated
test fixture builders (e.g., `make_reg_opts` helpers).

---

### G3 — Direct `producers[]` field access still common (step 7 not done)  ⚠️ DEFERRED

**Severity:** Tracked deferral.  Not a bug, but a foot-gun for future
contributors who may write code that bypasses the controlled-access API.
**Files:** ~25 sites read or iterate `entry.producers` / `entry_ref.producers`
directly:
- `broker_service.cpp:1139` (CHANNEL_ERROR_NOTIFY fan-out — schema mismatch)
- `broker_service.cpp:1334` (path B inbox schema record creation comment)
- `broker_service.cpp:1570` (DISC_REQ presence resolution scan)
- `broker_service.cpp:1705` (DEREG_REQ producer-by-pid resolution)
- `broker_service.cpp:2072-2090` (heartbeat handler — was_pending derivation)
- `broker_service.cpp:2381-2415` (sweep paths)
- `broker_service.cpp:2452` (consumer iteration — likely)
- `broker_service.cpp:2654` (consumer iteration on dead-consumer fan-out)
- `broker_service.cpp:3122-3158` (admin snapshot serializer)
- `hub_state.cpp:695,723,727` (`_on_channel_registered` legacy)
- `hub_state.cpp:986` (`_on_channel_closed` cascade)
- `hub_state_json.cpp:61` (JSON serializer iteration)

**Status:** This was M2.5 step 7 ("Privatize state-bearing fields").
The previous reviews deferred this as polish — the bug class is
already structurally eliminated (no more channel-scope per-party
fields).  Step 7 would make the wall hard; today it's a strong
convention.

**Recommendation:** Defer until a concrete bug surfaces from a
new direct-access site; the test-driven multi-producer contract
tests will catch any regression in the meantime.

---

### G4 — Design doc §5.1 API methods never delivered (step 2d)  ⚠️ DEFERRED

**Severity:** Tracked deferral.  The methods are syntactic sugar, not
required for correctness.
**Files:** `docs/tech_draft/controlled_access_api_design.md` §5.1 lists:
- `set_invariant_schema` / `set_invariant_transport` — broker's Cat-1
  gate + `_on_producer_added`'s invariant-check both work without them.
- `observable(roles_map)` member wrapper — `compute_channel_observable`
  free template is in use everywhere; the wrapper would just call it.
- `is_alive(roles_map)` — derived from observable; no caller asks for it.
- `producers()` / `consumers()` span accessors — public vectors give
  the same access today.

**Recommendation:** Keep deferred.  Add only when a concrete caller
needs them.

---

### G5 — `_on_channel_registered` legacy path correctness ✅ OK (verified)

**Severity:** None — verified correct post-step-6.5.
**Files:** `src/utils/ipc/hub_state.cpp:677-755` — test-only legacy path.

**Verified:** Reads `entry.producers.front().role_uid` and
`entry.producers.front().zmq_pubkey`; both fields are on `ProducerEntry`
post step 2c/6.5.  Test `HubStateOps.ChannelRegistered_ComposesChannelAndRoleAndShmAndCounter`
sets `ch.producers.front().zmq_pubkey` and verifies propagation; passes.

**Latent edge case (not a bug today):** if a test calls
`HubStateTestAccess::on_channel_registered` with an entry that has
NO producers (`entry.producers.empty()`), the legacy path now passes
an empty `producer_pubkey` to `upsert_role_locked`, which no-ops.
That's correct behavior; the empty-producer-vector case had to be
handled because `ProducerEntry.zmq_pubkey` doesn't exist when there
are no producers.  Code at line 727-729 correctly guards.

---

### G6 — HEP-CORE-0021 §16.3 wire vs broker implementation: `role_uid` field mismatch  ⚠️ DOC-CODE DRIFT

**Severity:** Doc/code reconciliation.  Behaviour today is correct
(more secure); doc describes a wire shape the broker doesn't yet
require.
**Files:**
- `docs/HEP/HEP-CORE-0021-ZMQ-Endpoint-Registry.md` §16.3 lists
  `role_uid` as a wire field on `ENDPOINT_UPDATE_REQ`.
- `src/utils/ipc/broker_service.cpp:handle_endpoint_update_req` does
  not read `req["role_uid"]`; resolves the sender's role_uid by
  matching the ZMQ identity against `entry->producers[].zmq_identity`.

**Why identity-based is BETTER:** A wire `role_uid` could be spoofed
by a producer claiming to be someone else.  The ZMQ identity is bound
to the actual connection by ZMTP and can't be spoofed without
attacking the transport.  HEP-CORE-0021 §16.3 should reflect this:
the wire field is optional/diagnostic; resolution is identity-based.

**Fix:** update HEP-CORE-0021 §16.3 — the `role_uid` field becomes
"optional, ignored by the server; identity-based resolution is
authoritative."  Or remove the field from the wire schema entirely.
(Reading it as optional preserves any client that still sends it.)

---

### G7 — Stale "STEP 2a ADDITIVE" comments on `ProducerEntry`  ⚠️ STALE

**Severity:** Comment drift.
**Files:** `src/include/utils/hub_state.hpp:165-175`.

**Problem:** The comment block on `ProducerEntry` says:
> // STEP 2a ADDITIVE: these fields are added here; the matching
> // fields on `ChannelEntry` remain until step 2b/3 migrates the
> // callers (REG_REQ handler, DISC_REQ_ACK, ENDPOINT_UPDATE_REQ).
> // After step 2b/3 those channel-scope copies are deleted.

Step 6.5 deleted those channel-scope copies.  The "ADDITIVE" wording is
now wrong — these fields are THE STORAGE, not additive.

**Fix:** rewrite the comment block to describe the final state.

---

### G8 — Missing L2 test for `ChannelMetrics` multi-producer bug  ⚠️ COVERAGE GAP

**Severity:** Couples with G1.  If/when G1 is fixed, a test should
pin the contract.
**Suggested test:** broker REG_REQ admits two distinct ZMQ producers
on a Fan-In channel; each sends metrics; broker returns a metrics
tree keyed by uid with both producers' data present (not last-writer-
wins).

---

### G9 — HEP-CORE-0023 §2.6 `ChannelEntry` schematic placeholder  ⚠️ DOC PRECISION

**Severity:** Documentation precision.  Not a bug, but the schematic
has a hand-wavy placeholder that's worth tightening.
**Files:** `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` §2.6 —
`ChannelEntry` schematic ends with:
> // ... data-plane endpoint / schema metadata

**Problem:** Post step 6.5 there is NO data-plane endpoint or
schema metadata at channel scope — those moved to `ProducerEntry`.
The schematic should list exactly what's left: `name`, `shm_name`,
schema invariants (hash / version / id / blds / owner),
`has_shared_memory`, `data_transport`, `pattern`, `created_at`,
and the two party lists.

**Fix:** rewrite the schematic with the exact field list, or
replace the placeholder with "// channel-wide invariants only —
all per-party fields are on ProducerEntry / ConsumerEntry".

---

### G10 — `compute_channel_observable<RolesMap>` template direct-field access ✅ OK BY DESIGN

**Severity:** None.
**Files:** `src/include/utils/hub_state.hpp:911-942` — the templated
free function reads `ch.producers` directly.

This is intentional — the template needs to traverse the producer
list to compute the channel-wide observable.  Wrapping it in an
accessor adds nothing; the function is the canonical reader.

---

### G11 — RoleEntry.disconnected_fired 🚧 PATCH still present  ⚠️ TRACKED FOR M3

**Severity:** Tracked.  Step 7 → Wave M3 closure.
**Files:** `src/include/utils/hub_state.hpp:733-757`,
`src/utils/ipc/hub_state.cpp:408,659,1133`.

Already tracked for Wave M3 (RoleEntry controlled-access API)
per the design doc §5.2.  Comments retargeted to "Wave M3" in
commit `7038669`.  No M2.5 action.

---

### G12 — `metrics_store_` overall is "legacy"; mixed pattern  ⚠️ TRACKED FOR M1.4

**Severity:** Tracked.
**Files:** `src/utils/ipc/broker_service.cpp:245-252,1990` etc.

The whole `metrics_store_` lives outside HubState — historical
artifact.  TODO_MASTER.md M1.4 ("retire `metrics_store_` legacy")
tracks the migration.  Wave M2.5 deliberately did not retire it;
G1 is the most pressing piece (a real multi-producer bug).

---

### G13 — `ChannelSnapshotEntry` — clean ✅ OK

**Verified** (post-step-2c deletion): no back-compat scalars remain.
Only `producer_uids` + `producer_pids` parallel vectors carry the
multi-producer truth.  No producers map for metrics here — that
lives on `ChannelMetrics` per G1.

---

### G14 — DISC_REQ_ACK echo precision: first-producer transitional shape  ✅ OK (DESIGN INTENT)

**Severity:** None.  Logged for completeness.
**Files:** `broker_service.cpp:1666-1672`.

DISC_REQ_ACK emits `zmq_pubkey` + `zmq_node_endpoint` from
`first_producer()` only.  Design §7.5.3 step 6 explicitly defers
the per-producer wire shape to step 5 (where the broker handlers
migrated).  Today the wire is still single-producer-shaped for
backwards compat; admin clients needing per-producer data can use
`query_channel_snapshot` which already emits per-producer endpoints
(`hub_state_json.cpp:65-68`).

---

### G15 — Test-helper `make_reg_opts` central place  ⚠️ SIMPLIFICATION

**Severity:** Low.  Couples with G2.
**Files:** `tests/test_layer3_datahub/test_datahub_broker_protocol.cpp:180-193`.

**Problem:** `make_reg_opts` is the only test fixture builder I found
that includes the three obsolete keys (`zmq_ctrl_endpoint` /
`zmq_data_endpoint` / `zmq_pubkey`).  Many other test files duplicate
the same three lines manually (per G2).  An opportunity to consolidate:
make `make_reg_opts` (or a moved helper in a shared test header) the
single source of truth and drop the obsolete keys from it.  Other test
files import the helper.

---

## Status Summary

| ID | Title | Severity | Status |
|---|---|---|---|
| G1 | `ChannelMetrics::producer` single-producer scalar (overwrite-class bug) | Bug | ❌ OPEN |
| G2 | 17 test files set obsolete REG_REQ wire keys | Cruft | ✅ FIXED 2026-05-11 (14 files swept, 51 lines deleted) |
| G3 | Direct `producers[]` access still common (step 7 deferred) | Deferred | ⚠️ TRACKED |
| G4 | Design doc §5.1 API methods never delivered (step 2d) | Deferred | ⚠️ TRACKED |
| G5 | `_on_channel_registered` legacy path correctness | Verified | ✅ OK |
| G6 | HEP-CORE-0021 §16.3 `role_uid` wire vs identity-based resolution | Doc drift | ✅ FIXED 2026-05-11 |
| G7 | Stale "STEP 2a ADDITIVE" comments on `ProducerEntry` | Stale | ✅ FIXED 2026-05-11 |
| G8 | Missing L2 test for `ChannelMetrics` multi-producer | Coverage | ❌ OPEN (couples with G1) |
| G9 | HEP-CORE-0023 §2.6 schematic placeholder for `ChannelEntry` | Doc precision | ✅ FIXED 2026-05-11 |
| G10 | `compute_channel_observable` template direct-field access | OK by design | ✅ OK |
| G11 | `RoleEntry.disconnected_fired` 🚧 PATCH | Tracked | ⚠️ DEFERRED to M3 |
| G12 | `metrics_store_` overall is "legacy"; mixed pattern | Tracked | ⚠️ DEFERRED to M1.4 |
| G13 | `ChannelSnapshotEntry` post-step-2c | Verified | ✅ OK |
| G14 | DISC_REQ_ACK first-producer transitional shape | OK by design | ✅ OK |
| G15 | `make_reg_opts` consolidation opportunity | Cruft | ✅ FIXED 2026-05-11 (3 obsolete keys removed from helper) |

## Fix-in-this-pass set (proposed)

Land NOW (small, mostly cosmetic):
- **G7** — rewrite the `ProducerEntry` "STEP 2a ADDITIVE" comment block.
- **G6** — update HEP-CORE-0021 §16.3 to say identity-based resolution is authoritative.
- **G9** — tighten HEP-CORE-0023 §2.6 `ChannelEntry` schematic.

Land as a small follow-up commit:
- **G2 + G15** — sweep delete obsolete `zmq_ctrl_endpoint` / `zmq_data_endpoint` / `zmq_pubkey` keys from the 17 test files; consolidate `make_reg_opts` if shared.

Land as the next behavior-changing commit (G1):
- **G1 + G8** — refactor `ChannelMetrics` to be per-producer keyed by uid; add L2 test pinning the contract.  This is the same bug class M2.5 eliminated on ChannelEntry, applied to the metrics plane.  Belongs in M1.4 retire-`metrics_store_` work; could also land standalone now while the M2.5 context is fresh.

Defer:
- **G3** (step 7 privatize fields) — bug class structurally gone; concrete-driven.
- **G4** (step 2d API gaps) — sugar, no current need.
- **G11** (M3 RoleEntry) — already on the wave plan.
- **G12** (metrics_store retirement) — broader than G1.
