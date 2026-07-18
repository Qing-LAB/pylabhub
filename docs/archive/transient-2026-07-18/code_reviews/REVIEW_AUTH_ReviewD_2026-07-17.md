# REVIEW-D (#277) — Allowlist + Revocation Cycle

**Milestone:** REVIEW-D — the penultimate gate of the Phase 1 CURVE chain; gates REVIEW-E (production-ready final gate).
**Date:** 2026-07-17 · **Status:** ✅ COMPLETE — all charges met; one L4 item explicitly deferred (logic covered at L3).

## Scope

REVIEW-D had two charges (per `docs/todo/AUTH_TODO.md`):
1. **Test contract pins reality, not just outcome** — auth/admission tests must assert the design contract (path, side-effects), not merely a pass/fail outcome.
2. **Allowlist + revocation cycle exercised end-to-end** — the admit → flow → revoke → deny lifecycle must be pinned against the real broker + roles, not only at the ledger primitive.

## Findings

| # | Item | Verdict | Resolution |
|---|------|---------|------------|
| 1 | Consumer PID-liveness sweep (`check_dead_consumers` / `process_dead`) | 🔴 REAL BUG — broken by design in distributed deployment | **REMOVED** (`a00a4188`). A PID is only meaningful on the broker's own host; the sweep silently failed cross-host. Heartbeat is now the **sole** consumer-liveness mechanism (HEP-CORE-0023 §2.1); revocation moved into the heartbeat-timeout reclaim path (`_on_consumer_revoked` + `fire_channel_auth_changed_notify(phase="left")`). HEP-CORE-0023 revoke/close protocol updated; the retired design scrubbed from the docs so it cannot be re-hallucinated. |
| 2 | Allowlist + revocation cycle end-to-end | ✅ COVERED (L3 + L2 + unit) | Pinned deterministically with real components — see "Coverage map" below. The revoke → DENY entry-gate transition is `Pattern4BrokerConsumerTest.ConsumerAttach_DeniedAfterDereg` (#2369): real broker + `BrokerWireClient`, `register → attach(success) → dereg → attach(denied)`. |
| 3 | Test-pins-reality audit | ✅ DONE | `docs/code_review/COVERAGE_AUDIT_Broker_Queue_CURVE_2026-07-17.md` — a 6-dimension systematic pass over the auth/admission path (REQ/ACK, queue ops, SHM/ZMQ CURVE, inbox/band). Found + fixed 2 security-test gaps + several doc-hygiene divergences; confirmed **no hidden critical gap** in the core path. |
| 4 | Passive-revocation contract correctness | ✅ CONFIRMED IN CODE | `zap_router.cpp:485-523`: libzmq's CURVE handshake proves the client owns the secret key behind the presented pubkey; ZAP then does pure **authorization** (`is_peer_allowed` against the allowlist). Revocation is **passive** (HEP-CORE-0036 §I5) — it denies NEW connections; existing sessions are trusted for their lifetime. **No impersonation risk.** The revoke→deny assertions correctly pin "new handshake denied + allowlist mutated," NOT "live session stops" (which the design intentionally does not do). |
| 5 | L4 data-plane revocation cycle (`MixedAdmitDeny`) | ⏸ **DEFERRED** (intentional) | The end-to-end data-plane variant stays deferred (`test_plh_hub_role_zmq_e2e.cpp:855-866`). Its **logic is fully covered at L3** (#2369) and its gate at L2 (`Swap_BlocksOldPeer_PinsData`); the only delta is live CURVE data-plane *timing*, which is fragile (the single-producer positive-flow scenario is skipped for the same reason). Low marginal value; tracked in `TESTING_TODO`. |

## Coverage map — the revoke → deny cycle

Every link of `admit → flow → revoke → deny` is pinned with **real code, no mocks**:

| Link | Test | Layer | What it proves |
|---|---|---|---|
| revoke removes the entry; same pubkey now denied | `ConsumerAttach_DeniedAfterDereg` (#2369) | L3 Pattern 4 (real broker + wire client) | `register → attach(success) → dereg → attach(denied)` with `denial_reason`; the gate reads the same `VersionedAdmissionLedger` that `_on_consumer_revoked → ledger.revoke` mutated. |
| dereg empties the allowlist | `GetChannelAuth_ReturnsAllowlist` (#2365) | L3 Pattern 4 | The pulled allowlist shrinks on dereg. |
| reclaim revokes + announces | `ConsumerHeartbeatTimeout_NotifyBodyShape` (#2396) | L3 Pattern 4 | Heartbeat-timeout reclaim fires `CHANNEL_AUTH_CHANGED_NOTIFY(phase="left")`. |
| the ZAP gate blocks a was-allowed peer | `Swap_BlocksOldPeer_PinsData` | L2 (real `ZmqQueue` + ZAP + CURVE) | After the allowlist swaps, the previously-admitted peer is denied at the transport gate. |
| ledger revoke/version + #2480 clamp | `test_versioned_admission_ledger.cpp` | L1/L2 unit | `admit`/`revoke`/`confirm` version semantics; the over-confirmation clamp. |

The L3 test is the composition guard: it threads broker `revoke` → ledger mutation → attach-gate refusal through the real wire, deterministically (no data plane). This is where the #2480 propagation bug would resurface.

## Why not an L4 data-plane cycle

Given passive revocation (§I5), "deny after revoke" is only observable on a **new** handshake, never as a live session stopping. That observable — new-connection-denied + allowlist-mutated — is exactly what #2369 pins at L3, without inheriting the CURVE data-plane timing fragility that keeps `ZmqE2E_AuthorizedConsumerReceivesAllSlots` skipped. Adding an L4 variant would largely duplicate #2369's logic while re-importing that flakiness. It stays deferred by choice, not omission.

## Outcome

Both charges met: the real production bug REVIEW-D surfaced (PID-liveness sweep) is removed with heartbeat as the sole liveness path; the allowlist+revocation cycle is regression-proofed end-to-end at L3 with real components; the test-pins-reality audit is complete. The passive-revocation contract is confirmed correct with no impersonation risk. Full ctest 2555/2555 (post security-test-gap commit `38414078`).

**REVIEW-D ✅ → REVIEW-E (#278) unblocked.**
