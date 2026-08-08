# AUTH_TODO — closed work, extracted 2026-08-07

`docs/todo/AUTH_TODO.md` was 850 lines, of which the large majority
described work that had finished. This file records **what was removed and
the evidence that justified removing it**, so nobody has to re-derive the
verdicts. The narrative prose itself is in git (`git show
e5de9a53:docs/todo/AUTH_TODO.md`) and in three earlier extractions:

- `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
- `docs/archive/transient-2026-06-09/todo-completions/AUTH_TODO_completions.md`
- `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`

The point of this pass was not compression. It was that the file had begun
to **misreport**: items marked open had shipped, items presented as design
questions had been decided, and two entries contradicted other entries in
the same file. A tracker that misreports is worse than a long one.

---

## 1. Items the file listed as OPEN that are actually closed

Each verified by reading code on 2026-08-07, not by trusting a marker.

| Claimed open | Verdict | Evidence |
|---|---|---|
| **Reverse-notify channel** — "admin console receives no asynchronous notifications; polling only" | **RETIRED BY DESIGN, not missing** | The push reverse path was removed from HEP-CORE-0033 §11 on 2026-07-22 and replaced by a client-polled output buffer. That design shipped: `src/include/utils/console_output_buffer.hpp` exists and `response_query` is live in `admin_service.cpp`. The file listed the *retired* design as an outstanding gap — while a later section of the same file recorded the retirement. Polling is the contract now. |
| **AUTH-6 File-10 Suite-2** — "7 masked broker `TEST_F`s awaiting a delete decision" | **DELETED 2026-07-20** | Commit `c7f4f608` "delete legacy RoleIdentityPolicy string gate (HEP-0035 §8 Phase 6)". `find tests -name '*role_identity_policy*'` returns nothing — the whole file went, Suite 2 with it. `tests/test_layer2_service/CMakeLists.txt:371` carries the tombstone. **This corrects a claim made in this same file's own hazard banner on 2026-08-07**, which listed these 7 as the one thing verified still open. The banner was written from the audit table, not from the tree — the exact failure the banner was warning about. |
| **#275 S2** — "16 L3 datahub workers still to scan for secret params" | **CLOSED** | `datahub_query_engine_workers.cpp` does not exist and never did. The surviving matches in `datahub_producer_consumer_workers.cpp` (lines 26, 85) are tombstone comments describing the retirement, not call sites. No live `shared_secret` parameter remains in `src/include/` — every hit there is a docstring recording the deletion. |
| **G1 / G2 — `api.allowed_peers` and `api.producers` "Native deferred per #84 MVP"** | **SHIPPED** | Both are in the native C ABI: `allowed_peers` at `src/include/utils/native_engine_api.h:323`, `producers` at `:354`. Three-engine parity holds. The deferral note outlived the deferral. |
| **§11.0.4 FINDING** — "control commands are fire-and-forget but `handle_close_channel` does a synchronous existence check; doc↔code tension, unresolved" | **RESOLVED** | The CURVE-integration review applied the doc fix: HEP-0033 §11.0.4 now states the synchronous `not_found` semantics. The file recorded the fix in one row and still called it unresolved in another. |
| **Hot-reload of `known_roles` on a running hub** (HEP-0035 §4.8.5) | **DECIDED AGAINST 2026-08-07** | Owner ruling: restart is the revocation boundary. Hot-managing auth invites inconsistency and holes. `BrokerCtrlAdmission::set_peer_allowlist` having no caller is the correct end state, not a wiring gap. Live task #104. |

## 2. Phases and chains removed as complete

Verified complete; detail in git and the three prior archives.

- **PeerAdmission A / B / C / C-chain (C1..C5)** — abstraction, KnownRole+CLI,
  ZapRouter + ZmqQueue CURVE, strict-CURVE cleanup.
- **HEP-CORE-0040 Locked Key Memory** implementation chain, including the
  §8.5.2 normative raw-32 seckey contract at the security-module boundary.
- **HEP-0036 §5b** canonical wire-schema unification.
- **AUTH-1 / AUTH-2 / AUTH-3** shipped; **AUTH-4** superseded by HEP-0041;
  **AUTH-5** doc sync; **AUTH-6** test revival (batches C0–C6);
  **AUTH-7** L4 end-to-end gate — its three gate tests
  (`ZmqE2E_Authorized`, `_Unauthorized`, `_MultiProducer`) passed in the
  full 2780-test sweep on 2026-08-07.
- **HEP-0041 Phase 1** substeps 1a–1k, the 1i-cleanup chain S1–S5,
  and **#262** mutual auth (default flipped to required).
- **REVIEW-C / REVIEW-D / REVIEW-E** — all closed 2026-07-16/17,
  🟢 Phase 1 production-ready.
- **Line E — admin CURVE + operator console.** REP server, typed admin
  protocol, ROUTER transport, sealed session identity, console output
  buffer, `admin_console_print` across Lua/Python/native, cap sizes wired
  to hub config, in-session replay defence.
- **CURVE-integration review code items (a)(b)(c)** — `known_roles` into the
  encrypted hub vault, inbox replay defence via the shared `ReplayGuard`,
  admin in-session replay. Only the doc backlog survives (live task #125).
- **2026-06-10 design audit** — G1/G2/G3 gaps, R1–R5 race modes, T1–T4 test
  gaps. All closed or folded. Its own closing assessment stands: no
  remaining design gap re-opens HEP-0036 §I11 or §6.5.
- **Deferred decisions P-InboxQueue, P-Admin, P-SHM-Identity, P-Demos,
  P-HEP** — all answered. P-InboxQueue's answer (hub-wide roster, not
  channel-scoped) is implemented and lives in HEP-CORE-0027 §3.5.

## 3. The task-ID collision, and why it is not being mapped

The file cited two incompatible ID spaces. IDs ran `#52`–`#317`; the live
list ended at `#119`, so the ranges overlapped with different meanings:

| ID | meant in AUTH_TODO | means in the live tracker |
|---|---|---|
| `#103` | AUTH-1 closure **and** a HEP-0017 §3.3 item — two things inside one file | admin session-replay L2 test |
| `#104` | AUTH-5 sibling-HEP doc sync | vault hot reload (decided against) |
| `#105` | federation protocol design | the `origin_uid` scoped-cascade doc-vs-code gap |

**No mapping table was built, deliberately.** Almost every legacy ID hangs
off work that closed; an ID that points at finished work does not need a
new name, it needs deleting. The handful that survive were re-anchored to
live tasks in the rewrite. A blind rename would also have been wrong —
many `#NNN` in the text are HEP numbers, PR numbers or line references.

Recovered anchors, for anyone reading an old commit message:
`#276 → #59`, `#277 → #60`, `#242 → #85`, `#283 → #108`,
`#106 → #89`, `#76 → #107`, `#105 → #69` (federation),
`#152` closed by `c7f4f608`, `#247` absorbed into `#121`,
`#155 → #127`.

## 4. Decision log (audit trail, verbatim)

- **2026-06-05 — Strict-mode cleanup must precede A3** (now AUTH-1). C1..C5
  sequenced first to surface dependents as compile/link/run errors instead
  of silent miscompiles. Chain closed 2026-06-09.
- **2026-06-05 PM — Separate STORAGE from API.** HEP-CORE-0040 KeyStore +
  LockedKey owns storage (one owner per process); API exposes OPERATIONS
  (`with_seckey(name, cb)`), not byte exports.
- **2026-06-06 — Use-not-export discipline.** RoleAPIBase + HubAPI lose the
  seckey accessor entirely; no legitimate caller exists. HEP-0040 §8.2.
- **2026-06-09 — Queue-level gate, NOT transport-level.** The broker auth
  gate sits at QUEUE level, before data-channel construction — not at the
  SHM/ZMQ transport level.
- **2026-06-09 — Mechanism enum collapsed 3 → 2.** `Plaintext` was
  structurally unreachable after C1. Later widened by HEP-0041 to add
  `ShmCapability`.
- **2026-06-16 — HEP-0041 supersedes AUTH-4.** Capability transport via
  `memfd_create` + `SCM_RIGHTS` replaces the `shm_secret` model.
- **2026-06-26 — KeyStore seckey representation contract** (HEP-0040 §8.5.2
  NORMATIVE). Raw 32 bytes at the security-module boundary; Z85 only at
  file/wire/display. Closed the SHM consumer-attach handshake failure.
- **2026-06-27 — first AUTH_TODO compression**, 1616 → ~500 lines. Notably,
  that pass caught a line claiming "S3 ✅ shipped" that was false against
  code. The same class of error recurred by 2026-08-07 in the other
  direction — shipped work still marked open.
- **2026-07-17 — inbox admission is HUB-WIDE**, superseding the earlier
  channel-scoped answer. The inbox is a role↔role facility, so it admits any
  authenticated `known_role`, not just the parent channel's allowlist.
- **2026-07-22 — admin reverse-notify retired** in favour of a client-polled
  output buffer. One console at a time; return-and-clear poll; oldest lines
  spill to log with a `dropped_count`. Keeps the plane strict request/reply.
- **2026-08-07 — no vault hot reload.** Restart is the revocation boundary.
- **2026-08-07 — SHM observer retired** (HEP-CORE-0045). Its metrics reach
  the hub through the ordinary presence/heartbeat path; the separate
  observer attach was solving a problem that had gone away. The ephemeral
  key-grant mechanism it used is worth keeping and needs a proper home —
  live task #119.

## 5. Memory rules that came out of this track

Already carried in the user-level `MEMORY.md`; listed here only so the
audit trail is complete. Audit stale silent-fallback patterns when a
contract changes. Separate storage design from API design. Refresh against
persistent docs at the moment work begins. No flake explanations — a
different test failing each run is a race signature. Read the log before
the rerun. Seckey representation at the module boundary is normative.

---

# Closed 2026-08-08 — four items, three of which were never real

Moved out of `AUTH_TODO.md` so the open file stops being a changelog.
One-line pointers remain there under each section.

## ✅ #103 — admin session id replayed from a second connection (REAL, fixed)

`AdminServiceTest.Console_SessionIdFromAnotherConnection_Rejected`. Alice
establishes and pings; mallory connects on her own routing id, never
authenticates, presents alice's exact sealed id with a well-formed replay
triple; hub answers `unauthorized`; alice's session survives.

Mutation-verified: removing the fact comparison at `admin_session.cpp:149`
makes mallory succeed and the test fails on the `is_error()` assertion,
while the sibling establish/ping test stays green.

**Recorded limit:** both consoles connect over loopback, so `Peer-Address`
is identical and the routing id is the only discriminator. A regression
dropping *only* the `peer_address` comparison would not fail this test.

## ❌ #122 — "CTRL ZAP has a deny pin and no allow pin" (NOT REAL)

Premise was that a deny-only pin would pass even if the broker denied
everyone. It would not: every passing L3 CURVE test registers a role
through that same door. Unknown-key refusal is pinned by
`CtrlZapDenyPath`; known-key admission is proven by every role that
registers. Allow-branch counter pins already existed
(`zap_router::handshake_allow_increments_allowed_counter`, plus a full
deny→allow swap on one keypair in `zmq_queue_auth`).

Filed off `grep CtrlZapAllowPath tests/` — a search for a test NAME,
reported as absent COVERAGE.

## ❌ #123 — "role vault publishes no `.pub`" (INVERTED)

Claimed HEP-0035 §4.8.3 requires `--add-known-role <role.pub>` and so
`RoleVault` needs a `publish_public_key`. §4.8.3 takes a Z85 **string**,
and §4.8.3/§4.8.4 rule explicitly that roles publish **no** `.pub` sidecar
— stdout capture at keygen is the shipped path, with a `.pub` named only
as a candidate convenience *if that flow proves brittle*.

Real defect was the opposite end: the L4 roundtrip decrypted the vault to
get the pubkey, so the only workflow an operator can follow had zero
coverage. Now parses `--keygen` stdout. Mutation-verified — a well-formed
but wrong 40-char pubkey satisfies both the length assert and the
`--list-known-roles` check, yet still fails, because the role is denied at
the ZAP gate and never registers.

## ✅/❌ #121 — SEC-Fold (half already done, half withdrawn)

C++ half was complete before work started: 6 `<sodium.h>` includes, all
inside `src/*/security/`; the alleged outlier `vault_crypto` routes through
`secure()`; `KeyStore` already a gated member. Now guarded by
`SecurityGuardrail_SodiumConfinedToModule`, mutation-verified.

Doc fold **withdrawn**. Three structures proposed, each killed by evidence:
~12,000 lines in one file; the vault already has *finalized* owners
(HEP-0024 §3.4, HEP-0033 §7.1) a merge would strip; and the citation matrix
puts the densest coupling in the wire protocols (0036↔0041 = 46), nowhere
near any proposed grouping. Decisive: the `sodium_init` triangle that
justified the fold was already fixed by HEP-0043 existing.

## The pattern across all four

Three of the four were false, and all three shared one method: **an
absence asserted from counting search hits rather than reading them** — a
guessed test name, a guessed file path, a hit-count split by directory.
Every claim verified by reading the artifact held.

Rule: to show a behaviour is untested or a field unused, grep the
BEHAVIOUR — the accessor, the config field, the wire type — repo-wide, and
read every hit. Never a hypothetical name, never a single guessed path.
And a comment enumerating things is not evidence of their absence: read
what the comment is attached to before quoting it as proof.
