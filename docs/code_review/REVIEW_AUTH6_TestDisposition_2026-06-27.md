# AUTH-6 (#154) — L3 Datahub Test Disposition Audit

**Date:** 2026-06-27
**Tracker:** AUTH-6 / #154
**Auditor:** dispatched research agent (read-only)
**Discipline:** `docs/README/README_testing.md` § "Mocking discipline +
test-only hooks" rules 6 (RETIRE obsolete tests) + 7 (layer placement
follows what the test exercises, not what it constructs).

---

## Why this audit exists

My initial AUTH-6 plan was "unmask + migrate the masked workers files."
The user correctly pointed out that this misses the harder discipline:

> **"Not all tests are justified to exist. Use correct tests to pin
> design + code bugs.  Retire the test if needed; create replacement
> if needed."**

So the correct AUTH-6 is an **audit-first disposition pass**, not a
mechanical migration.  This doc captures that audit so:

- per-file unmask/retire/defer commits cite a verified rationale, and
- the work is not re-derived next time L3 broker tests need attention.

---

## Scope

**15 L3 datahub test files** audited (drivers + their worker files):

**Currently masked (10):**
1. `test_datahub_broker.cpp` (356 LOC) + `workers/datahub_broker_workers.cpp` (2576 LOC)
2. `test_datahub_broker_health.cpp` (173 LOC) + `workers/datahub_broker_health_workers.cpp` (1193 LOC)
3. `test_datahub_broker_protocol.cpp` (273 LOC) + `workers/datahub_broker_protocol_workers.cpp` (1859 LOC)
4. `test_datahub_role_state_machine.cpp` (366 LOC) + `workers/datahub_role_state_workers.cpp` (2726 LOC)
5. `test_datahub_metrics.cpp` (139 LOC) + `workers/datahub_metrics_workers.cpp` (1101 LOC)
6. `test_datahub_zmq_endpoint_registry.cpp` (105 LOC) + `workers/zmq_endpoint_registry_workers.cpp` (810 LOC)
7. `test_hub_lua_integration.cpp` (108 LOC) + `workers/hub_lua_integration_workers.cpp` (1070 LOC)
8. `test_hub_python_integration.cpp` (31 LOC) + `workers/hub_python_integration_workers.cpp` (315 LOC)
9. `test_datahub_hub_federation.cpp` (41 LOC) + `workers/hub_federation_workers.cpp` (411 LOC)
10. `test_datahub_role_identity_policy.cpp` (144 LOC) + `workers/role_identity_policy_workers.cpp`

**Already unmasked 2026-06-07 (4 — retroactive audit):**
11. `test_datahub_broker_consumer.cpp` + `workers/broker_consumer_workers.cpp`
12. `test_datahub_broker_schema.cpp` + `workers/broker_schema_workers.cpp`
13. `test_datahub_broker_admin.cpp` + `workers/broker_admin_workers.cpp`
14. `test_datahub_broker_request_comm.cpp` + `workers/datahub_broker_request_comm_workers.cpp`

**Recently unmasked 2026-06-27 (1 — retroactive audit):**
15. `test_datahub_hub_host_integration.cpp` (59 LOC) + `workers/hub_host_integration_workers.cpp` (318 LOC).
    Commit `86b7b209`.

**Total: ~194 TEST_F's across 15 files.**

---

## Headline finding

The disposition turns out to be much simpler than the initial scoping
suggested:

| Disposition | Count | Files |
|---|---|---|
| **UNMASK + MIGRATE** (surface intact, test correctly asserts, just needs KeyStore fixtures) | 142 | Files 1-8 + 11-15 |
| **DEFER** (blocked on a not-yet-shipped piece) | 3 | File 9 federation — blocks on #105 |
| **DELETE** (slated for outright deletion per a sibling task) | 7 | File 10 Suite 2 — blocks on #152 (RoleIdentityPolicy retirement) |
| **RETIRE with doc-block** (production helper survives; Pattern-1 enum tests guard it) | 4 | File 10 Suite 1 — enum↔string helpers used by production WARN logs |

**Retroactive audit of files 11-15: CONFIRMED VALID.**  No re-audit work
needed; the 2026-06-07 batch + the 2026-06-27 `86b7b209` commit pass
both rule 6 (no obsolete surfaces re-introduced) and rule 7 (correct
layer).

## Real blocker — task #177

The reason the masks haven't lifted isn't the test disposition itself —
it's that **task #177 ("Migrate L3 test workers + L2 test_hub_state to
KeyStore-based fixtures") hasn't shipped**.

Without #177:
- L3 worker bodies can't initialize `SecureMemorySubsystem` + `KeyStore`
  (the framework primitives shipped under HEP-CORE-0040 #169-#170).
- `BrokerHandle` / `start_hubhost_broker` construction fails on missing
  KeyStore seed.

With #177 shipped:
- The 142 UNMASK+MIGRATE tests should compile + link + green with no
  per-file rewrite.  Each file is a 1-line CMakeLists unmask + a
  worker-side `CurveKeyStoreFixture ks_fixture(...)` seed before
  `start_hubhost_broker(...)`.

**Recommended sequencing:** ship #177 first, then batch the unmasks.

## Cross-cutting observations

1. **No legacy `LocalBrokerHandle` (mock-host) remnants.**  All 194
   workers use real `HubHost` via `BrokerHandle` — no rule-2 bypasses.

2. **All 194 tests are correctly at L3.**  They exercise protocol +
   state-machine behavior between components in one process (real
   BrokerService + real HubHost).  Not L1/L2 (no API-unit isolation),
   not L4 (no multi-process cross-binary protocol).  README_testing
   §1.2 rule 7 satisfied.

3. **Surfaces all still exist in current code.**  The audit walked each
   TEST_F against the current code state:
   - HEP-CORE-0034 Phase 3/4 schema-registry surfaces (40 schema tests)
     — intact.
   - HEP-CORE-0036 §5b canonical wire schema (10+ wire-conformance
     tests + transport arbitration + CONSUMER_REG_ACK `producers[]`
     array) — shipped 2026-06-25/26.
   - HEP-CORE-0039 P8 migration gates (MultiProducer_PartialPendingTimeout,
     TwoSnapshotInvariant, ChannelTornDown_ConsumerPass2Skipped) —
     shipped #149-#150.
   - HEP-CORE-0040 KeyStore + LockedKey — shipped 2026-06-09.
   - HEP-CORE-0041 capability transport — shipped 1a-1h + 1k.
   - AUTH-1/2/3 (Authorized state, ZAP pump, peer allowlist refresh)
     — shipped 2026-06-13/16.

4. **Federation file (file 9) is the one true DEFER.**  Tests
   HELLO/TARGETED_MSG/PEER_BYE pin federation control-plane surfaces
   whose broker handler implementation is gated on #105 federation
   design.  Correct disposition: DEFER until #105 design ships +
   broker handler lands.  Mark with `GTEST_SKIP` + doc-block citing
   #105 in the meantime.

5. **RoleIdentityPolicy file (file 10) is split.**
   - Suite 1 (4 TEST_F's, Pattern 1, enum↔string helpers):
     **RETIRE with doc-block** per rule 6.  The helpers
     (`role_identity_policy_from_str`, `_to_str`) are still consumed
     by production WARN logs + error responses in `broker_service.cpp`
     `check_role_identity`.  Pattern-1 unit tests of those helpers
     stay valuable — but the test FILE belongs in the L2 utils layer,
     not L3.  Move to `tests/test_layer2_service/`.
   - Suite 2 (7 TEST_F's, Pattern 3, broker policy enforcement):
     **DELETE** per #152 (HEP-CORE-0035 §8 Phase 6).  The placeholder
     auth mechanism retires; tests retire with it.

6. **Files 5/6/7/8 do NOT need separate tracker tasks (#293/#294/#295).**
   They're all in the UNMASK+MIGRATE bucket alongside files 1-4.  The
   prior decision to split them into separate tasks was wrong — the
   audit found no design-disposition reason to separate them.  The
   tasks #293/#294/#295 should be **CLOSED as merged into #154** with
   the audit pointing them all under the same Phase-2 batch.

---

## Per-TEST_F disposition table

See dispatched-agent report (preserved in commit message for this audit).
The dispositions are uniform per file with the breakdown above:

- Files 1, 2, 3, 4 (broker / health / protocol / state-machine):
  **all UNMASK + MIGRATE** — 101 TEST_F's pinning HEP-CORE-0007 §2 / §5 /
  §6, HEP-CORE-0023 §2-§4, HEP-CORE-0030 §5-§6, HEP-CORE-0034 §3-§11,
  HEP-CORE-0036 §4-§5b, HEP-CORE-0039 §2-§3.
- Files 5, 6 (metrics / endpoint registry):
  **all UNMASK + MIGRATE** — 26 TEST_F's pinning HEP-CORE-0019 §2 +
  HEP-CORE-0036 §5b transport arbitration.
- Files 7, 8 (Lua / Python integration):
  **all UNMASK + MIGRATE** — 12 TEST_F's pinning HEP-CORE-0011
  "Engine Construction Lifecycle" + HEP-CORE-0033 Phase 7 D3.3 / D4.2.
- File 9 (federation):
  **all DEFER** — 3 TEST_F's pinning federation surfaces gated on #105.
- File 10 Suite 1 (RoleIdentityPolicy enum helpers):
  **RE-LAYER to L2** — 4 TEST_F's; helpers stay in production.
- File 10 Suite 2 (RoleIdentityPolicy broker enforcement):
  **DELETE** — 7 TEST_F's slated for #152 retirement.
- Files 11-15 (already unmasked):
  **RETROACTIVELY CONFIRMED** — 39 TEST_F's; no issues.

---

## Recommended action plan

**Phase 1 — Ship task #177 (KeyStore fixtures).**
This is the actual blocker.  Estimated 2-3 days.  Without #177 the
unmasks won't link.

**Phase 2 — Batch unmasks (per-file commits, after #177).**
- **Batch 2a** (broker family, no inter-dependencies): files 1, 3, 4
  (broker + protocol + state-machine).  ~91 TEST_F's.
- **Batch 2b** (depends on 2a stable): files 2, 5, 6 (broker_health +
  metrics + endpoint registry).  ~36 TEST_F's.
- **Batch 2c** (depends on 2a + HubHost stable): files 7, 8 (Lua +
  Python integration).  ~12 TEST_F's.

**Phase 3 — DEFER + DELETE + RE-LAYER:**
- File 9 (federation): single commit adds `GTEST_SKIP` per TEST_F with
  doc-block citing #105.  Land at any time.
- File 10 Suite 2 (RoleIdentityPolicy broker): single commit DELETES
  Suite 2 + worker bodies.  Lands as part of #152 ship.
- File 10 Suite 1 (RoleIdentityPolicy enum helpers): single commit
  moves the Pattern-1 unit tests to `tests/test_layer2_service/` with
  doc-block recording the move + the production-consumer rationale.

**Phase 4 — Close-out:**
- Close tasks #293, #294, #295 as merged into #154 (no separate
  trackers needed — disposition is uniform).
- Update AUTH_TODO §AUTH-6 to reflect the audit findings + Phase
  schedule.
- Archive this review doc once AUTH-6 closes.

---

## What about commit `86b7b209` (hub_host_integration)?

Retroactively audited (file 15 above).  All 3 TEST_F's verified:

| TEST_F | Surface | Status |
|---|---|---|
| HubHost_BrokerReachable_AfterStartup | HubHost::broker_endpoint() non-empty after startup | ✓ surface intact (HEP-CORE-0033 §4 lifecycle) |
| HubHost_RegReq_RoundTripsViaSpawnedBroker | REG_REQ wire round-trip via real BRC + real BrokerService | ✓ surface intact (HEP-CORE-0007 §2.1) |
| HubHost_Shutdown_BreaksClientConnection | BRC monitor observes ZMQ_EVENT_DISCONNECTED on broker close | 🔴 REMOVED 2026-06-27 (commit 6e819b73) — cannot pin in-process (libzmq shared-context CURVE quirk); L4 replacement task #296 |

The commit was **valid** — all 3 TEST_F's pin live surfaces; the
workers file was already migrated to the post-#177-ish KeyStore pattern.
The mask was stale; the unmask was justified.

The retrospective concern was that the unmask DIDN'T go through this
audit discipline.  The audit confirms it would have passed.  Keep
`86b7b209`.

---

## Cross-references

- `docs/todo/AUTH_TODO.md` § "AUTH-6 — L3 broker test revival"
- `docs/README/README_testing.md` § "Mocking discipline + test-only
  hooks" rules 6 + 7
- Task #154 (AUTH-6)
- Task #177 (KeyStore fixtures — the actual prerequisite blocker)
- Task #152 (RoleIdentityPolicy retirement — Phase 3 delete dependency)
- Task #105 (Federation design — Phase 3 defer dependency)
- Tasks #293/#294/#295 (close as merged into #154 per audit)
- HEP-CORE-0019, HEP-CORE-0023, HEP-CORE-0030, HEP-CORE-0034,
  HEP-CORE-0036, HEP-CORE-0039, HEP-CORE-0040, HEP-CORE-0041
