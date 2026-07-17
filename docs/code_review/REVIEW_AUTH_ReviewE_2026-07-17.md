# REVIEW-E (#278) — Phase 1 Production-Readiness Final Gate

**Milestone:** REVIEW-E — the final gate of the Phase 1 CURVE chain.
**Date:** 2026-07-17 · **Status:** ✅ COMPLETE — **Phase 1 (single-hub CURVE data + control chain) is PRODUCTION-READY** for its defined scope, with three surfaces explicitly out of scope + tracked (below).

## Scope

REVIEW-E's charges: (1) confirm the Phase 1 threat model is fully enforced by **live** code; (2) confirm mutual auth closes the impersonation window; (3) declare production-readiness or name the blockers.

Phase 1 scope (per `TODO_MASTER.md` Line 1): **CURVE authentication end-to-end across the ZMQ and SHM data paths + the broker control plane, on a single hub.** Explicitly NOT in scope: admin plane (Line E), inbox CURVE (#191), federation trust (#105).

## Threat-model verification (every row fact-checked against live code)

| Threat | Verdict | Live enforcement (code anchor) |
|---|---|---|
| **Authentication** — connect without proving identity | ✅ enforced | Control plane `curve_server=1` + ZAP domain from `known_roles` (`broker_service.cpp:1026/1083/1085`); ZMQ data plane CURVE; SHM AttachProtocol. A non-allowlisted peer cannot open a session. |
| **Authorization** — authed-but-unpermitted channel access | ✅ enforced | `known_roles` + per-channel `VersionedAdmissionLedger` allowlist; ZAP `is_peer_allowed` (`zap_router.cpp:519`). |
| **Impersonation** — claim another's identity | ✅ enforced | CURVE proves secret-key ownership before ZAP; ZMQ serverkey pinning (`broker_request_comm.cpp:751`); SHM Frame-3 pubkey verification (`MutualAuth_RejectsFrame3PubkeyMismatch`, this session). **Impersonation window closed.** |
| **Replay** — resend a captured REG-family message | ✅ enforced + hardened | Live gate on every REG-family msg (`receive_and_validate → run_reg_family_gates / run_authenticated_reg_family_gates → check_replay_bound → HubState::nonce_seen`). Window≥skew soundness fixed + end-to-end tested this session (`546bc115`). |
| **Envelope tamper** (frame splice) | ✅ enforced | `WireEnvelope::parse` recomputes `envelope_hash` and rejects `envelope_hash_mismatch` (`wire_envelope.cpp:250-259`) — runs in `receive_and_validate` (I-ENVELOPE-BODY-BINDING). |
| **Revocation** — revoked peer still admitted | ✅ enforced | Passive revocation (allowlist removal → new handshakes denied); verified end-to-end in REVIEW-D (`ConsumerAttach_DeniedAfterDereg` #2369). |
| **Downgrade** — force plaintext / anonymous | ✅ resistant | ZAP rejects non-CURVE mechanisms; `shm_require_mutual_auth` defaults `true` (#262); anonymous only behind the explicit `--allow-anonymous-data` operator flag (not a silent path). |
| **Confidentiality / integrity of data** | ✅ enforced | CURVE encrypts the ZMQ data plane; SHM capability handoff via SCM_RIGHTS, fail-closed on `MSG_CTRUNC` (REVIEW-C). |

**No Category-1 gap remains in the Phase 1 CURVE chain.** Every invariant the HEP-0036 / HEP-0046 threat model names (authentication, authorization, pubkey-binding, identity-match, replay, envelope-binding, revocation) is enforced by live, tested code. The earlier REVIEW-E concern that replay/envelope-binding were "islanded" was based on a stale HEP-0046 status note; the fact-check proved the gates run live via `receive_and_validate` (HEP-0046 status corrected this session).

## Out of Phase 1 scope — tracked, not blockers

| Surface | State | Tracking |
|---|---|---|
| **Admin plane** | ⚠ **plaintext + bearer token** (loopback default mitigates; can bind non-loopback). The most notable open surface — but a separate deliverable, not part of the role/broker CURVE data chain. | Line E; design locked HEP-0033 §11.1/§11.3 + `DRAFT_curve_admin_protocol_2026-07-15.md` |
| **Inbox** | ⚠ plaintext, no admission (role↔role, bypasses broker) | #191 / #103 (HEP-0036 Phase 4+) |
| **Federation** | handlers live but every federation test `GTEST_SKIP`'d | #105 (Phase F) |

## Residual low-severity coverage (non-blocking)

Per `COVERAGE_AUDIT_Broker_Queue_CURVE_2026-07-17.md`: some defensive/error-code branches are unit-tested but not broker-wire-tested, and a few positive-path E2E scenarios are timing-deferred (logic covered at L2/L3). None are Cat-1; tracked in `TESTING_TODO`, close opportunistically.

## Outcome

Both charges met. The Phase 1 CURVE chain — ZMQ + SHM data planes and the broker control plane, single hub — enforces its full threat model with live, tested code; mutual auth closes the impersonation window. Full ctest **2557/2557**.

**REVIEW-E ✅ → PHASE 1 PRODUCTION-READY.** Next security work, in order: admin-plane CURVE (Line E — the top open surface), then inbox (#191) and federation (#105).
