# DRAFT: Versioned Admission Ledger (unified binding-side confirmation)

**Date:** 2026-07-13
**Status:** SHIPPED (2026-07-13).  Retirement of the pre-2026-07-13 set-
snapshot mechanism (`binding_side_confirmed_allowlist`,
`_on_binding_confirmed`, `is_pubkey_in_binding_confirmed`).
**Scope:** broker admission book-keeping only.  No wire changes.
**Authoritative section:** HEP-CORE-0042 §5.5.2 amendment 2026-07-13
+ §5.5.2.1 INVARIANT-BIND-CONFIRM-1..3.
**Regression pinned by:** test #2480
(`PlhHubCliTest.ZmqE2E_MultiProducer_TwoAuthorized`), L1 unit tests in
`tests/test_layer1_base/test_versioned_admission_ledger.cpp`,
L2 hub_state tests in `tests/test_layer2_service/test_hub_state.cpp`.

## 1. What the bug was

Broker's `_on_binding_confirmed(channel)` at `hub_state.cpp:2112`
snapshotted the CURRENT `authorized_consumer_pubkeys` into
`binding_side_confirmed_allowlist` on every consumer-branch
`CHANNEL_AUTH_APPLIED_REQ`.  The wire's `applied_version` was captured
in the handler (`broker_service.cpp:4377`) but IGNORED when populating
the confirmed state.  If a new admission landed at broker between the
consumer sending APPLIED_REQ(V) and broker processing it, broker
over-confirmed — its snapshot included pubkeys the consumer had NOT
installed at version V.

## 2. Reproduction — test #2480 timeline (PID 710828, 2026-07-12)

Reconstructed from broker + consumer + producer-B logs at
`build/stage-debug/test_artifacts/plh_hub_l4/plh_hub_l4_zmqe2e_fanin_*_710828_*/logs`:

| Time | Actor | Event | Broker `authorized` | Broker `confirmed_allowlist` (bugged) |
|---|---|---|---|---|
| 17.527830 | broker | NOTIFY(admitted, prod A) fired | {A} | {} |
| 17.626799 | consumer | applied allowlist size=1 [A] locally | {A} | {} |
| 17.627148 | broker | producer B REG_REQ → `_on_consumer_authorized(B)` → NOTIFY(admitted, B) fired | **{A, B}** | {} |
| **17.627341** | **broker** | **`ChannelAuthAppliedConsumer applied_version=1`** — `_on_binding_confirmed` snapshots CURRENT | **{A, B}** | **{A, B}** ← BUG |
| 17.628282 | producer B | polls `CHECK_PEER_READY_REQ` → broker says **ready** (B ∈ confirmed_allowlist, incorrectly) | {A, B} | {A, B} |
| **17.629914** | **consumer** | **ZAP DENY pubkey=B** (actual local allowlist is only {A}) | {A, B} | {A, B} |
| 17.679166 | consumer | applied allowlist size=2 [A, B] — **51ms too late for B** | {A, B} | {A, B} |
| ...258 iterations later... | producer B | `prod_test: stop iter=258` — 258 slots sent into a dead PUSH socket | | |
| 32.959321 | consumer | `cons_test: stop offsets_seen=0` — offset=100 (B's data) never arrived | | |

**Pre-2026-07-13 failure rate under isolation:** ~14% (empirical).

## 3. Why fan-out was already correct

Fan-out uses `CONSUMER_ATTACH_REQ_ZMQ` with a `target_version` field.
Broker checks `confirmed_version[K][producer_uid] >= target_version`.
This is version-based, hence race-free: `channel_version` is monotonic,
and `confirmed_version >= V` genuinely means "producer installed all
admissions at version ≤ V."  The fan-in path invented a parallel
mechanism (set-snapshot) instead of reusing this primitive.

## 4. Unified fix: `VersionedAdmissionLedger`

New primitive at `src/include/utils/versioned_admission_ledger.hpp`
(header-only template).  Replaces the pre-2026-07-13 quartet on
`ChannelAccessEntry`:
- `authorized_consumer_pubkeys` (set)
- `channel_version` (uint64_t)
- `confirmed_version_per_producer` (map)
- `binding_side_confirmed_allowlist` (set) ← the buggy one

Ledger API (concise):
- `admit(pk) -> uint64_t` — assigns/returns admission_version; idempotent (no bump on re-admit).
- `revoke(pk) -> uint64_t` — removes admission; bumps iff pubkey was actually present.
- `confirm(role_uid, applied_version) -> uint64_t` — advances role's confirmed_version monotonically.
- `is_visible_to(role_uid, pk) -> optional<bool>` — nullopt if role never confirmed; false if pk not admitted OR admission_version > confirmed_version; true otherwise.

## 5. What the code changes were

Files modified:
- `src/include/utils/versioned_admission_ledger.hpp` (NEW) — the class template.
- `src/include/utils/hub_state.hpp` — `ChannelAccessEntry` field replaced with `ledger`; `_on_producer_confirmed` renamed to `_on_role_confirmed`; `_on_binding_confirmed` and `is_pubkey_in_binding_confirmed` removed; `is_pubkey_visible_to` added.
- `src/utils/ipc/hub_state.cpp` — `_on_consumer_authorized` / `_on_consumer_revoked` delegate to `ledger.admit` / `ledger.revoke`; `_on_role_confirmed` uses `ledger.confirm`; per-producer confirmation reset uses `ledger.reset_role_confirmation`.
- `src/utils/ipc/broker_service.cpp` — 8 reader sites migrated to ledger accessors; consumer-branch APPLIED_REQ handler calls `_on_role_confirmed` with the wire's `applied_version`; `handle_check_peer_ready_req` resolves the binding-side role_uid from topology and calls `is_pubkey_visible_to`.
- `tests/test_framework/hub_state_test_access.h` — retired `on_binding_confirmed` helper; added `on_role_confirmed`.
- `tests/test_layer1_base/test_versioned_admission_ledger.cpp` (NEW) — 21 L1 tests including race regression at primitive level.
- `tests/test_layer1_base/CMakeLists.txt` — registered the new L1 target.
- `tests/test_layer2_service/test_hub_state.cpp` — rewrote confirmation tests to pin INVARIANT-BIND-CONFIRM-1..3 via the ledger API.

## 6. Verification

- L1 (`test_layer1_versioned_admission_ledger`): 21/21 pass.  Includes `Test2480_RaceRegression_TwoProducerAdmissionSequence` at primitive level.
- L2 (`test_layer2_hub_state`): all hub-state ledger tests pass.
- L4 (`PlhHubCliTest.ZmqE2E_MultiProducer_TwoAuthorized`): 30/30 iterations pass under 4x CPU stress (wall-clock 8–14s per run, vs. 3s unstressed baseline).  Probability of 30 clean iterations at the pre-fix 14% failure rate: ≈1%.

## 7. Retirement of this doc

Merge lasting insight into HEP-CORE-0042 §5.5.2 (done) then archive
this doc to `docs/archive/transient-YYYY-MM-DD/` per DOC_STRUCTURE.md
§2.2.  Retire after: (a) test #2480 has run in the CI regression
matrix for 2 weeks with zero flakes, AND (b) no residual `_on_binding_
confirmed` / `binding_side_confirmed_allowlist` references survive
anywhere in `src/` (doc-comment references in `plh_version_registry.hpp`,
`hub_queue.hpp`, and `broker_request_comm.hpp` are cosmetic —
harmless but noted for a future comment-refresh sweep).
