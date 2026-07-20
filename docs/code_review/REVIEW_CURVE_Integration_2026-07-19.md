# REVIEW — CURVE Integration (data handling + communication)

**Status:** ✅ REVIEW COMPLETE — Phase A (document review) + Phase B
(code-vs-doc) done. Verdict: **the CURVE integration is soundly implemented;
the findings are overwhelmingly documentation drift** plus a few concrete
code cleanups. No security bypass exists.
**Date:** 2026-07-19.
**Scope:** every CURVE-touching surface of the hub — broker↔role control
plane, role DEALER, inbox, SHM channel-auth (AttachProtocol), admin console,
key management, ZAP, and the wire integrity/replay layer.
**Method:** Phase A = docs-only consistency review of the CURVE HEPs (4
parallel reviewers). Phase B = check the code against the confirmed
documented contract for inconsistencies, gaps, obsolete/duplicated
patterns.

> This is a transient review doc (`docs/DOC_STRUCTURE.md §1.7`). Open items
> also feed `docs/todo/AUTH_TODO.md`. When resolved, fold lasting design
> corrections into the owning HEPs and archive.

---

## Phase A — executive summary

The CURVE *mechanism* is sound and consistent across the HEPs; the problems
are **documentation drift** — the same concept described several ways after
successive migrations, stale "open questions" already decided, and large
superseded blocks left un-fenced. A reader cannot always tell which text is
normative. Highest-impact items:

- **MAJOR — `§7.4` is a dangling reference in HEP-0033.** The "single-pumper"
  ZAP handler — the load-bearing authority for both the broker ZAP model and
  the admin plane's crypto-only isolation — is cited as "§7.4" **8×** in
  HEP-0033 §11, but HEP-0033 §7 has only §7.1/§7.2. The concept lives in
  **HEP-0036 §7.4**. *Every* `§7.4` in HEP-0033 (including text I just added
  for the console) must be repointed to HEP-0036. **Touches the just-shipped
  console — fixable now.**
- **MAJOR — dev-mode escape hatch contradicts the "no CURVE bypass"
  invariant.** HEP-0036 §11.2 documents `hub.dev_mode=true` disabling
  Layer-1 ZAP + data-plane CURVE (NULL fallback), which HEP-0035 §2/§4.6.5
  forbid and HEP-0035 §7 Q5 records as **removed 2026-06-04**. Self-contradicts
  HEP-0036 §6.1 ("no bypass"). Either the hatch exists (security hole) or the
  text is stale (must delete). **Phase B must confirm which.**
- **`known_roles` storage described three incompatible ways** — vault-internal
  map (HEP-0035 §4.8, which *rejects* a plaintext dir) vs a plaintext
  `known_roles/` `.pub` directory (HEP-0036 §11.3) vs `known_roles.json`
  (HEP-0036 §6.1). One source of truth must win.
- **Admin control semantics contradict within HEP-0033 §11** — §11.0.4 says
  control commands are fire-and-forget ("closing an unregistered channel is a
  silent no-op, not an error"), but §11.5 assigns `not_found` and the code
  (`handle_close_channel`) does a synchronous existence check. **Design
  decision needed.**
- **Admin per-instance session-sealing key is "unhomed"** — §11.0.5 says it
  lives "in the security subsystem," but HEP-0043 §7 gives it no canonical
  name, it's absent from the ephemeral-key examples, and `secretbox_encrypt`
  takes a *raw* key (unlike `box_*_using`'s name-citation), so its
  use-not-export story is unspecified. **Touches the just-shipped console.**

---

## Findings touching the just-shipped console (actionable now)

| # | Finding | Fix |
|---|---|---|
| C-1 | `§7.4` dangling in HEP-0033 §11 (8×, incl. my console text) + the same ref in `admin_service.cpp`/`curve_socket.hpp`/`admin_session.hpp` comments | Repoint to **HEP-0036 §7.4** (or add a real §7.4 to HEP-0033); fix code comments too |
| C-2 | HEP-0033 §11.0.2 table row still labels admin "message form" as **JSON** | Change to typed `WireEnvelope` (§11.1 already decided this) |
| C-3 | §11.0.4 fire-and-forget vs §11.5 `not_found` vs `handle_close_channel` existence check | Decide: keep synchronous `not_found` (update §11.0.4) OR make close fire-and-forget (update handler + test) |
| C-4 | Admin session-sealing key unhomed in HEP-0043 §7 (no canonical name; use-not-export unspecified for a raw-key secretbox) | Add the `admin.session.seal` name + its ephemeral/use-not-export contract to HEP-0043 §7 + §11.0.5; Phase B: confirm `lookup_raw` span is used synchronously and never copied |
| C-5 | Admin typed bodies (ADMIN_HELLO/PING/… + which fields feed envelope_hash) not in HEP-0046 §14.3 body catalog | Enumerate the admin bodies in §14.3 (or a §11 appendix) |
| C-6 | Admin has **no in-session replay window** — bodies carry envelope_hash but not the security triple; only cross-connection replay is defended (§11.0.5) | Document whether admin control methods are idempotent under in-session replay, or add a nonce/ts guard |

---

## Area 1 — Auth chain (HEP-0035 / HEP-0036)

**Inconsistencies:** dev-mode hatch (§11.2 vs §2/§4.6.5, MAJOR); `known_roles`
storage 3 ways (§4.8 vs §11.3 vs §6.1); `UNKNOWN_ROLE` local-check vs
federation `peer_delegated` acceptance unreconciled (§6.1 vs §4.3);
`ENDPOINT_UPDATE_REQ` "retired" (§11.1) vs re-introduced (§I7);
inbox `known_roles` roster used (§9.3) but absent from REG_ACK schema (§6.2/6.4);
producer-pubkey field named 4 ways (`zmq_pubkey`/`pubkey_z85`/`pubkey`/`data_pubkey`);
`initial_allowlist` object-shape (§6.2) vs bare-string cache (§7.2);
teardown readiness vocab (`Authorized` §8 vs `Registered/RegRequestPending` §3.5.6).
**Gaps:** ZAP DENY status-code contract; control-link revocation timing (I5 is
data-plane only); key rotation grace-vs-hardcut (§7 Q1 open vs §2.1 "hard-cut");
hot-reload depends on the deferred admin RPC; reconnect re-seed sequencing;
audit-log levels (§7 Q4).
**Obsolete:** dev-mode §11.2; `shm_secret` broker-mints §11.3 (no banner);
`ENDPOINT_UPDATE_REQ` retired §11.1; stale open Q2 (already decided §7.1/§7.4);
§3 "no ZAP handler" gap-analysis (contradicts shipped banner); §6 wire block
self-declared migration-history (§5b wins, Task #286).

## Area 2 — Key management (HEP-0040 / 0043 / 0038)

**Inconsistencies:** `with_seckey` Z85 (§5.2) vs raw-32 (§8.5.2); `add_identity`
80-byte Z85 (§5.2/§8.1) vs 64-byte raw (§8.5.2 / 0043 §7); "§7 mirrors §5.2"
false (§7 adds 4 methods); obsolete `lookup()` still cited as consumer call
(§5.2/§10/§11 + §3 diagram); `box_seal_using` vs `box_encrypt_using` /
`secretbox_seal` vs `secretbox_encrypt` (0043 §1.4/§8 vs §2/§5/§6); `plh-cli
keygen` (0043 §1.5) vs real `plh_hub/plh_role --keygen`; `kHubIdentityName`/
`kRoleIdentityName` consumed everywhere but never specified in the owning
§5.3/§7; **HEP-0038 banner cites §5–§9 that don't exist** (ends at §5).
**Gaps:** online key rotation (none); per-key revocation (only allowlist +
token rotation; `remove()` unwired); **admin session-sealing key unhomed
(§3.3)**; federation key distribution/pinning; at-rest constant single-home
(pwhash preset/salt/AEAD not pinned in one normative section).
**Obsolete:** whole `SecureMemorySubsystem` §4; §5.1/§5.4/§5.6 (singularity,
dynamic KeyStore module, `key_store()` accessor); §3 diagram; §8 code using
deleted `key_store()`; §10 task table; 0038 whole body is open-questions draft
cited as authoritative.

## Area 3 — SHM channel auth (HEP-0041 / 0044 / 0045)

**Inconsistencies:** `ConsumerAuthMaterial` name-based (0044 §7.3) vs
`SeckeyAccessor` (0041 §6.4); acceptor ctor missing `ObserverPubkeyAccessor`
(0041 §6.4); mutual-auth default flip = shipped/true (0044 §8.4) vs
blocked/pending (0041 §D4.5) vs NOT-done (0042 §10); version-axis broker_proto
required (0041 §D4.5) vs wrong-axis (0044 §8.4); Frame-3 pubkey source field
rename half-applied (0044 §3.4 vs §8.3); `CONSUMER_REG_ACK`
`data_endpoint`/`data_pubkey` rename **not propagated** to 0041/0045 despite
"loop closed" claim; 0042 points at moved content (0041 §5.5 → 0044).
**Gaps:** observer initiator can't emit `role_type="observer"` yet (0044 §7.6)
though 0045 marks it shipped; attach-retry policy only in a task note;
handshake replay property implicit/undocumented; **role_uid not
cryptographically bound to pubkey** (0044 §4.2); `expected_uid` vs multi-role
acceptor (observer at different uid); **post-`SCM_RIGHTS` consumer revocation
impossible** (capability held indefinitely); observer header-only fd is
convention-only (no memfd seal); no stable error enum (grep `what()` strings).
**Obsolete:** `SeckeyAccessor` block + Diagram-A (0041 §6.4/§9); `set_shm_secret`
/ plaintext-secret ladder (0041 §1.2/§4/§5); one-way-auth "gap" prose (0041
§9 D4); signed-nonce residue; bearer-token `consumer_authorization_token`
(0041 §5.2/D5).

## Area 4 — Messaging surfaces + wire (HEP-0027 / 0033 §11 / 0046)

**Inconsistencies:** `§7.4` dangling in HEP-0033 (8×, MAJOR — see C-1); admin
control fire-and-forget vs `not_found` (§11.0.4 vs §11.5 vs impl-note, C-3);
admin "message form=JSON" table row stale (§11.0.2, C-2); inbox header
depends-on cites per-channel-allowlist model that §3.5 supersedes (hub-wide
known_roles); replay nonce-window formula `2*pending_budget_ms` (I-REPLAY-BOUND)
vs live `30 s` config; inbox status "deferred" vs §4.1 present-tense narration.
**Gaps:** **inbox has no replay/anti-splice story** (msgpack frame: checksum +
schema_tag only; no envelope_hash/nonce/ts; seq-gap is diagnostic);
**admin no in-session replay window** (C-6); admin reverse-notify + provenance
DESIGN complete but CODE pending (tracked); admin typed bodies not in §14.3
catalog (C-5).
**Obsolete:** HEP-0046 §14.3 `RegReqBody`/`RegAckBody` unified entries
(superseded by Producer/Consumer split, ⚠ erratum-pending); retired
`broker_proto`/`schema_version` fields still in the catalog; AdminShell +
`exec_python` tombstones (§11 preamble/§11.2).

---

## Fixes applied (2026-07-19)

**Bucket A — code cleanups (done; pending full-build verify):**
- `arm_curve_server` dedup — broker (`broker_service.cpp`) + inbox
  (`hub_inbox_queue.cpp`) converted to the helper; all 3 sites consolidated.
- `wire_envelope.cpp` local `bytes_to_hex` → `format_tools::bytes_to_hex`.
- 2 bare `§7.4` code comments + 8 HEP-0033 `§7.4` refs → `HEP-CORE-0036 §7.4`.
- 2 stale `SeckeyAccessor` comments (`role_host_frame.cpp`, `key_store.cpp`).
- HEP-0033 §11.0.2 "message form" JSON → typed `WireEnvelope`.
- (Prereq CI fix: removed 6 stale `token_required` refs missed when the field
  was dropped — full-sweep + full build now the gate.)

**Bucket B — doc reconciliation (partial):**
- ✅ HEP-0036 §11.2 `dev_mode` → tombstone (code has NO bypass; verified).
- ✅ HEP-0040 §5.2 `with_seckey` "Z85" → raw-32 (aligned to §8.5.2 + code).
- ✅ HEP-0033 §11.0.5 — homed the admin session-sealing key
  (`admin.session.seal`, `add_raw`/`lookup_raw`, use-not-export note).
- ⏳ Remaining: `known_roles` §11.3 `.pub`-dir → `known_roles.json`;
  `CONSUMER_REG_ACK` field names in HEP-0041/0044/0045 →
  `producers[].endpoint`+`pubkey_z85`; HEP-0038 banner (§5–§9 don't exist);
  HEP-0041 §6.4/§D4.5 stale-fencing; enumerate admin bodies in HEP-0046 §14.3;
  fence HEP-0040 §4/§5 + HEP-0036 §11.3 obsolete blocks.

**Maintainer decisions (2026-07-19) — RESOLVED:**
1. **`known_roles` storage → ENCRYPT (code is the gap).** The §4.8
   encrypted-vault design is intended; the shipped plaintext `known_roles.json`
   is a gap. **NEW WORK ITEM:** encrypt the allowlist into the hub vault (docs
   unchanged — code must catch up). Tracked in AUTH_TODO.
2. **Control-command semantics → KEEP synchronous `not_found`.** ✅ DONE —
   §11.0.4 updated (validate synchronously; `ok`=accepted-not-completed;
   `not_found` for unknown), §11.1 impl-note + console-test comment updated;
   handler + test unchanged.
3. **Inbox replay → ADD defense.** Integrity-only is NOT acceptable.
   **NEW WORK ITEM:** add a replay/anti-splice gate to the inbox msgpack frame
   (nonce/ts or seq-reject, cf. broker I-REPLAY-BOUND). Tracked in AUTH_TODO.
4. **Admin in-session replay → ADD per-command nonce/ts.** **NEW WORK ITEM:**
   extend admin bodies with the replay triple + nonce/skew check like the REG
   plane (§11.0.5 currently defends only cross-connection). Tracked in AUTH_TODO.

**Verification (2026-07-19):** full build green (all targets); **full ctest
`100% passed, 0 failed out of 2592`** with all fixes applied (incl. the
guardrail-fixture fix for the two admin test blocks, which had been missing
`FIXTURES_REQUIRED Guardrails` and were cascading the whole suite to Not-Run).

## Phase B — results (code-vs-doc)

**Security all-clear + code-is-correct.** Almost every Phase-A doc finding
turned out to be **stale documentation, not a code defect**:

| Phase-A concern | Code reality (file:line) | Verdict |
|---|---|---|
| `dev_mode` CURVE bypass | **none exists** — 0 matches for `dev_mode`/`ZMQ_NULL`; broker arms `curve_server` unconditionally (`broker_service.cpp:1026`) | ✅ secure; HEP-0036 §11.2 STALE |
| `known_roles` source | `known_roles.json` (ACL-verified plaintext) via `KnownRolesStore::load_from_file` (`known_roles.cpp:146`) | code right; HEP-0035 §4.8 + HEP-0036 §11.3 WRONG; §6.1 right |
| producer pubkey field | `zmq_pubkey` (`wire_bodies.hpp:172`, `broker_reg_handler.cpp:94`) | code consistent; 4-way naming is doc drift |
| `with_seckey` encoding | RAW 32 bytes (`key_store.cpp:406`) | HEP-0040 §8.5.2 right, §5.2 WRONG |
| deleted accessors gone | `key_store()`/`lookup()`/`secure_memory_subsystem()` — 0 live hits | ✅ clean |
| `kHub/RoleIdentityName` | defined `key_store.hpp:133-134` | doc-narration gap only |
| **admin session key use-not-export** | `lookup_raw` span → `secretbox_*` synchronously, never copied (`admin_session.cpp:85,128`) | ✅ HONORED |
| `ConsumerAuthMaterial` | `own_seckey_name` name citation (`attach_protocol.hpp:116`) | HEP-0044 §7.3 right; HEP-0041 §6.4 STALE |
| acceptor `ObserverPubkeyAccessor` | present (`attach_protocol.hpp:165`) | ✅ matches |
| mutual-auth default | `true` (`startup_config.hpp:46`, `role_api_base.cpp:356`) | HEP-0044 §8.4 right; HEP-0041 §D4.5 STALE |
| observer initiator | hardcodes `role_type="consumer"` (`attach_protocol.cpp:722`) | real GAP (send-side unshipped) |
| `CONSUMER_REG_ACK` keys | `producers[].endpoint` + `pubkey_z85` (`role_api_base.cpp:1118`) | BOTH doc phrasings WRONG |
| obsolete SHM auth code | `set_shm_secret`/signed-nonce/`SeckeyAccessor` — none live (2 stale comments) | ✅ clean |
| inbox CURVE | fully implemented + ZAP (`hub_inbox_queue.cpp:286-310`) | HEP-0027 §3.5 "zero CURVE" STALE |
| replay gates | broker LIVE (`admission_gates.cpp:205-241`); **inbox NONE** | inbox = real GAP |
| envelope_hash | parse recomputes+rejects (`wire_envelope.cpp:246`); all 13 admin bodies require it | ✅ confirmed |

### Real code cleanups (small, actionable)
1. **`arm_curve_server` duplication** — only `admin_service.cpp:180` uses it;
   `broker_service.cpp:1026` + `hub_inbox_queue.cpp:290,652` still inline-arm.
   The `curve_socket.hpp:7-9` docblock **over-claims** it consolidated all
   three (my error). Fix: convert broker+inbox onto the helper (real dedup).
2. **2 bare `§7.4` refs** → qualify as HEP-CORE-0036 §7.4: `curve_socket.hpp:19`,
   `admin_service.cpp:178`.
3. **3rd hex encoder** — promote the local `wire_envelope.cpp:25` `bytes_to_hex`
   (self-flagged) to `format_tools::bytes_to_hex`.
4. **2 stale `SeckeyAccessor` comments** (`role_host_frame.cpp:23`,
   `key_store.cpp:402`).
5. **HEP-0033 §11.0.2 table** still labels admin "message form" JSON → typed.

### Real design GAPS (not bugs — track, don't fix inline)
- Inbox has **no replay/anti-splice defense** (msgpack frame: checksum only).
- Observer **initiator send-side** not implemented (receive-side ready).
- Admin has **no in-session replay window** (only cross-connection via §11.0.5).
- **Post-`SCM_RIGHTS` consumer revocation** impossible (capability held).
- Online **key rotation** / per-key **revocation** unspecified + unimplemented.

### Doc reconciliation backlog (owning HEPs)
Delete HEP-0036 §11.2 (dev_mode); fix HEP-0040 §5.2 (`with_seckey` raw);
reconcile `known_roles` storage to `known_roles.json` (HEP-0035 §4.8 /
HEP-0036 §11.3); fix HEP-0038 banner (§5–§9 don't exist); mark HEP-0041 §6.4 /
§D4.5 stale; correct `CONSUMER_REG_ACK` field names in HEP-0041/0044/0045 to
`producers[].endpoint`+`pubkey_z85`; resolve §11.0.4-vs-§11.5-vs-handler
(fire-and-forget vs `not_found`); home the admin session key in HEP-0043 §7;
enumerate admin bodies in HEP-0046 §14.3; fence the large obsolete blocks
(HEP-0040 §4/§5, HEP-0036 §11.1/§11.3).
</content>
