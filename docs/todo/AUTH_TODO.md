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

### 🔨 Named-key operations — stop callers carrying secrets around

**Plan: `tech_draft/DRAFT_named_key_operations_2026-08.md`. Design:
HEP-CORE-0043 §2.5. Task #137. Prerequisite for the file/dir vault below.**

A review on 2026-08-08 found secret bytes held outside the security
module in three places. They are one API gap with three symptoms, not
three defects: the module can encrypt *for* you under a named key on the
public-key side (`box_*_using`), but there is no symmetric equivalent —
so every caller needing symmetric encryption must fetch the key first.

- **The vault key is a plain stack array** (`vault_crypto.cpp`). Wiped
  afterwards, but not page-locked, so the OS can write it to swap while
  live. Also returned by value, and NRVO is not guaranteed, so a copy may
  survive unwiped in the callee frame.
  *Why it is written that way:* `SecureBuffer` has a deleted move
  constructor, so it cannot be returned — the safe type could not express
  "produce a key and hand it back." That is the API's fault, not the
  author's, and it is the clearest argument for the fix.
- **Private keys pass through `std::string`** in both vault create paths
  and the hub save path. `std::string` does not wipe. This is the exact
  pattern whose removal from the `Impl` members is commented in both
  files — it survived in the functions either side.
- **The two config loaders are couriers** — `vault.secret_key()` →
  `string_view` → KeyStore, for no reason the vault could not do itself.

**Already right, do not "fix":** socket arming already takes a name and
never exposes a key; wiping discipline is good everywhere (the gap is
locking, not wiping); `box_*_using` is the model to copy.

**Fix:** nine operations defined in HEP-0043 §2.5 — three to admit a key,
two symmetric jobs, three whole-file jobs, one vault deposit. Settled and
not open: no nonce parameter on the symmetric jobs (generated inside, so
it cannot be reused); process-lifetime keys; `add_` throws on duplicate
while `replace_` is explicit; no new guard types.

**Seven steps, in order, each leaving the tree green** — see the plan.

**Step 6 is the one that decides whether any of it holds.** Steps 1-5
move every caller off the fetch-a-key pattern; they do not remove the
*ability*. The raw-key `secretbox_encrypt` / `secretbox_decrypt` stay
public unless step 6 makes them private, and then the next person writes
a new courier with the API inviting them to. The asymmetric side is the
proof: it exposes **no** raw-key `box_encrypt`, only `box_*_using`, which
is exactly why it never grew this problem. `lookup_raw` is the harder
call in the same step — no production consumer afterwards, but retiring
it is a contract handoff, not a deletion.

**Verification is *absence*, not a green suite** — these changes are
invisible to behaviour, so a passing test proves nothing about whether a
secret stopped being copied. Each step must also *add* coverage for what
it introduces; two assertions worth naming, because they are the ones
that catch the dangerous bugs: two encryptions of the same plaintext must
**differ** (the nonce is genuinely fresh), and the same password under a
**different scope** must give a different key (one vault's password does
not open another's). Step 4 additionally needs a committed fixture vault
— a round-trip inside one build passes even if both sides changed
together.

### 🔨 CARRY FORWARD — design and finish the file/dir vault for user scripts

**Owner-requested 2026-08-08 as a standing follow-up. Task #136.** This
entry is the durable record; the task list does not survive a context
reset.

**What it is.** A place a user script can persist a secret across role
restarts — a token, a credential for some instrument the script talks to.
The owner's framing is a **file/directory** store, held on disk the way
the identity vault is, but as its own thing.

**Why it is not a small feature, and why every prior attempt understated
it.** The long-standing framing (HEP-0038, and #89 before the split) was
"extend the `RoleVault` payload with a `scripts` map." That reads as a
payload change. It is not. **`RoleVault` is write-once** — its whole
surface is `create`, `open`, `public_key()`, `secret_key()`, `role_uid()`.
There is no `save`, no setter (contrast `HubVault`, which has
`set_known_roles` + `save`). Storing script data in the role's vault
therefore needs a write path that does not exist, operating on the file
that holds the role's CURVE identity key.

**Design questions, none of them answered anywhere today:**

1. **Re-encryption without the password.** The vault is Argon2id +
   secretbox. Writing back means having the key again. Retain the
   password for the process lifetime (a new standing secret)? Re-prompt
   (impossible for a daemon)? Or give the store its own key material?

   **Owner input 2026-08-08 — the intended direction: keep the DERIVED
   KEY in the security module's protected memory after the password is
   validated once, rather than retaining the password at all.** SMS was
   built for exactly this. `KeyStore` holds keys in `LockedKey`
   (`sodium_malloc` — mlock, guard pages, canary) under the
   use-not-export contract, and it already has a raw-symmetric-key path:
   `add_raw` to store, `lookup_raw` to use the bytes in place without
   copying them into an owning buffer.
   **Working precedent to read first:** the admin console's session-seal
   key (`admin.session.seal`) does precisely this shape — minted once,
   held in `KeyStore`, and passed straight into `secretbox_encrypt` /
   `_decrypt` within a single statement so the bytes never leave the
   module. See `admin_session.cpp` and HEP-CORE-0033 §11.0.5.
   **The difference that needs investigating, and it is the whole
   question:** that key is *randomly minted per instance*; a vault key is
   *derived from an operator password*. So: is `add_raw` the right home
   for a KDF-derived key, what is its lifetime (process lifetime? until
   first write? re-derive on password change?), and what happens on
   rotation. **Owner direction: investigate when this item is picked up,
   not before.** Recorded here so the investigation starts from the
   existing facility instead of inventing a second one.
2. **Atomicity.** A torn write to `<uid>.vault` destroys the role's
   identity, not just its script data.
3. **Crash mid-save.** Same file, same blast radius.
4. **Authority.** May user script code compel repeated rewrites of the
   file holding its own identity key? That is a write-amplification and
   DoS surface driven by user code.
5. **Quota.** An unbounded script store is a disk-fill vector.
6. **Naming.** Settled: see the ruling below.

**Strong prior — a separate file, not the identity vault.** Its own
failure domain, own key lifetime, own permissions. Removes questions 2,
3 and 4 outright. The only argument for sharing the identity vault file
is "one file to provision," which the CLI can solve. The owner's
"file/dir vault" phrasing points the same way, but the shape is still
the design's to settle.

**Naming — ruled by the owner 2026-08-08.** The on-disk container keeps
the word *vault*: 1,891 references across `src/`, `tests/`, configs and
the operator CLI, and that meaning is load-bearing. The script-facing
store needs its own name; `api.vault_save` / `api.vault_load` as drafted
in HEP-0038 must not ship under those names. Nothing is built, so the
rename is free now and expensive later.

**Already settled — do not redesign.** Sandboxing lives in the LANGUAGE
BINDING LAYER, not in `SecureSubsystem`. Bindings namespace a
script-supplied name (`"mykey"` → `"script.<role_uid>.mykey"`) before
calling `secure().keys().*`; the module sees fully-qualified names and
applies no per-caller policy. Teaching the module about roles so it can
sandbox centrally would put policy in a mechanism module and give it a
reason to know who is calling it. Recorded in HEP-CORE-0043 §10.

**When it ships:** three-engine parity — Python, Lua and Native each
verified directly, no compile-only parity.

**Document state.** HEP-CORE-0038 is a 200-line draft that ends at §5 and
now carries a banner naming its two wrong premises (the name and the
storage model); do not implement from it. HEP-CORE-0043 §10 records the
open questions and marks the store not designed and not built.

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
