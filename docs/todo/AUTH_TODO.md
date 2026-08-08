# Authentication / PeerAdmission TODO

**Open items only.** Everything the CURVE chain has finished was removed on
2026-08-07 and the evidence for each removal recorded in
`docs/archive/transient-2026-08-07/todo-completions/AUTH_TODO_closed_2026-08-07.md`.
Task IDs here are **live** task-list IDs. The old `#52`–`#317` numbering is
gone; the archive explains why it was not mapped.

**Where the auth chain stands.** Single-hub CURVE is production-ready.
Every plane decides on the key the peer proved: registration, the REG
family, the control tier, channel broadcast, the inbox, and the admin
console. SHM channel auth ships on Linux with mutual auth on by default.
What is left is listed below — none of it re-opens the chain.

**Design of record:**

- `HEP-CORE-0035` Hub/Role authentication and federation trust
- `HEP-CORE-0036` Authenticated connection establishment
- `HEP-CORE-0040` Locked key memory (incl. §8.5.2 NORMATIVE seckey
  representation at the security-module boundary)
- `HEP-CORE-0041` SHM channel auth
- `HEP-CORE-0042` Channel attach coordination
- `HEP-CORE-0044` AttachProtocol primitive
- `HEP-CORE-0033` §11 admin plane
- `HEP-CORE-0017` §3.3 queue abstraction

---

## Do not re-litigate these

Five settled points. A proposal that appears to need new threading,
priority dispatch, or critical-error escalation around the auth flow is
almost certainly re-deriving one of them.

1. **Framework provides protocol; scripts provide coordination**
   (HEP-0036 §I11). The framework guarantees validated identity, async
   notification of membership change, atomic allowlist updates, and
   observable list state. It does **not** synchronise roles' decisions.
   "When to start, who's ready" is the script's job.
2. **Binding-side handler flow is notify-then-pull** (HEP-0036 §6.5).
   Broker fires a notify; the role pulls via `GET_CHANNEL_AUTH_REQ` and
   applies through `set_peer_allowlist`. On failure: log and return —
   recovery comes from the next notify or from hub-dead re-registration.
   No new threads, no priority dispatch.
3. **The race window is protocol, not a bug** (HEP-0036 §I3/§I5/§8.2).
   Between broker decision and cache update the producer keeps serving
   handshakes from its current ZAP cache; a consumer joining in the gap
   retries CURVE until it converges. Existing sessions are trusted for
   their lifetime — revocation is passive.
4. **Three-tier separation** (HEP-0036 §I9). Scripts see membership state,
   never sockets.
5. **Seckey representation is normative** (HEP-0040 §8.5.2). Raw 32 bytes
   inside the security module; Z85 only at file, wire, or display.

---

## Open — admin plane

- **`origin_uid` reaches two paths, not the cascade the HEP describes.**
  An admin close and an admin broadcast each carry `origin_uid` into their
  queue record and out on the resulting notify
  (`broker_service.cpp:1434`, `:1472`). HEP-0033 §11.0.5 additionally
  specifies a *scoped* "current actuation origin" that every log line and
  NOTIFY in the resulting teardown inherits automatically — no such
  mechanism exists in `src/`. So the audit trail the HEP promises for a
  cascade (pending-attach denials, the producer disconnect) is not there,
  and the `CHANNEL_CLOSING_NOTIFY` sender stamp still falls back to
  `self_hub_uid`. Either build the scoped origin or amend §11.0.5 to
  describe the two explicit paths that exist. **Task #105.**

- ✅ Admin session-id replay from a second connection — pinned and
  mutation-verified 2026-08-07 (#103). Detail in `todo-completions/`.

- **Three admin-triggered `HubHostBrokerHandle` tests** still sit on L3
  KEEP-rationale stubs (`broker_admin_workers.cpp:130`, `:163`, plus
  `broadcast_hub_queue`). Both things they waited on — the admin CURVE
  socket and `AdminWireClient` — have landed, so the migration is now
  mechanical. **Task #52.**

- **Deferred with federation:** operator identity derived from the client
  CURVE pubkey needs admin to have its own ZAP domain plus a
  `known_admins` allowlist (HEP-0033 §11.0.6). Verified 2026-08-07:
  `known_admins` appears in the HEP and nowhere in `src/`. The admin
  ROUTER runs with no ZAP domain deliberately — `admin_service.cpp:170`
  says why. **Task #69.**

- **Optional coverage, low value:** the command → broker-actuation →
  completion path is unit- and compile-verified but has no single e2e
  (would need channel registration in the admin harness), and
  `admin_console_print` has direct behaviour tests only via Lua. Python
  and native are thin marshalling onto the same HubAPI method that the Lua
  e2e already proves. Record, do not schedule.

## Open — security posture

- ✅ **SEC-Fold closed 2026-08-08 — do not re-open the HEP fold.** The C++
  half was already done (6 `<sodium.h>` includes, all inside
  `src/*/security/`; `KeyStore` already a gated member) and is now guarded
  by `SecurityGuardrail_SodiumConfinedToModule`. The **document fold is
  withdrawn**: its trigger, the `sodium_init` triangle, was fixed by
  HEP-0043 existing, and the vault already has *finalized* owners
  (HEP-0024 §3.4, HEP-0033 §7.1) that a merge would strip. Evidence and
  the three rejected structures: **#121** and `todo-completions/`.

- ❌ **"CTRL ZAP has a deny pin and no allow pin" — WITHDRAWN, no gap. Do
  not re-file.** Unknown-key refusal is pinned by `CtrlZapDenyPath`;
  known-key admission is proven by every L3 CURVE test that registers a
  role. Together those are the discrimination proof, and allow-branch
  counter pins already exist elsewhere. Was #122; filed off a grep for a
  test NAME that did not exist, read as absent coverage.

- **Federation peer admission has no active end-to-end pin.** The three
  `BrokerFederationTest` cases are `GTEST_SKIP`-ed, so removing peer keys
  from the broker allowlist entirely would still leave the suite green.
  The projection *is* pinned at index level
  (`AllowlistProjectsEveryKeyOfBothKinds`), and the exclusion half — a peer
  key never leaking into the role-to-role inbox roster — is pinnable today
  from config alone. What must wait is the admission half: a real peer hub
  completing a handshake against the CTRL ROUTER. **Do not read a green
  sweep as evidence the peer path works.** **Tasks #69, #99** (parked).

## Open — operator workflow and deployment

- ❌ **Role `.pub` sidecar — INVERTED. Do not build
  `RoleVault::publish_public_key`.** HEP-0035 §4.8.3 takes a Z85 *string*,
  and §4.8.4 rules that roles publish no `.pub`; stdout at keygen is the
  shipped path. The real defect was the L4 test decrypting the vault
  instead of reading that stdout — fixed and mutation-verified. Was #123.

- **27 demo role configs still ship `"keyfile": ""`** — broken since strict
  CURVE landed in May 2026. **Task #124.**
- **CLI `--init` one-shot provisioning** — the thing that makes the shipped
  auth usable by someone who is not the author. **Task #127.**
- **Hub-initiated shutdown and cold start** of a hub+role network — the
  deployment story the three items above feed into. **Task #116.**

## Open — platform reach

- **HEP-0041 Phase 2 (macOS) and Phase 3 (Windows)** capability-transport
  backends. SHM channel auth is Linux-only today. **Task #126.**
- Windows pathway hardening for HEP-0035 §4.6 remains blocked on there
  being any Windows CI.

## Open — doc reconciliation

- **CURVE-review doc backlog, 11 HEP-against-HEP items**, one of which
  (`known_roles` storage) is known to describe the pre-vault plaintext
  model that was hard-cut-over on 2026-07-19. **Task #125.**
- Three doc sites describing retired mechanisms. **Task #110.**

## Adjacent — same surface, not on the auth path

Do not sequence these into auth work.

| Item | Task |
|---|---|
| SMS expansion + vault design: retained key, script vault surface, config reload (HEP-0038) | #89 |
| Ephemeral capability grant — a home for the preserved observer-key mechanism | #119 |
| `spawn_bounded` bounded-thread primitive (DRAFT_HEP-0031) | #108 |
| Script hot-reload — design exists, nothing in tree | #107 |
| HEP-0042 impl phases 2–4 (ZMQ retrofit of the pre-attach pattern) | topology/messagehub trackers |
| Teardown stall — instrumented, parked until it recurs | #85 |

---

## Retrieving what was removed

```bash
# The 850-line version, immediately before this rewrite:
git show e5de9a53:docs/todo/AUTH_TODO.md | less

# The 1616-line version, before the 2026-06-27 compression:
git show dfe86a61:docs/todo/AUTH_TODO.md | less
```

Extraction records, newest first:
`docs/archive/transient-2026-08-07/todo-completions/AUTH_TODO_closed_2026-08-07.md`
(closure evidence + decision log + ID-collision explanation), then the
`2026-06-27`, `2026-06-09` and `2026-06-05` completions files under
`docs/archive/`.
