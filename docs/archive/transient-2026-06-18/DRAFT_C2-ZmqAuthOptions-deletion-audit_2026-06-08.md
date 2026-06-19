# Pre-audit: task #158 (C2 — delete `ZmqAuthOptions` + HEP-0040 §8.4 endpoint shape)

**Date:** 2026-06-08
**Author:** transient design note; consume when #158 lands.
**Goal:** every site referencing `ZmqAuthOptions` has a known disposition before any code change starts, so #158 lands as one coherent bundle.

**Cross-refs.** AUTH_TODO §C2 + §C4 (current C-chain spec); HEP-CORE-0040 §8.4 (canonical endpoint signatures); HEP-CORE-0040 §8.6 (struct elimination list).

---

## 1. Current struct definition

`src/include/utils/hub_zmq_queue.hpp` (struct body):

| Field | Type | Used by |
|---|---|---|
| `keystore_name` | `std::string` | PUSH + PULL; KeyStore identity lookup |
| `serverkey_z85` | `std::string` | PULL only; producer's pubkey for `ZMQ_CURVE_SERVERKEY` |
| `zap_domain` | `std::string` | PUSH only; ZapRouter domain registration |
| `initial_allowlist` | `PeerAllowlist` | PUSH only; seed allowlist; empty = deny-all |

---

## 2. Production sites — `src/`

| File:line | What | Post-#158 |
|---|---|---|
| `src/include/utils/hub_zmq_queue.hpp:310` | `pull_from_with_auth(...)` signature param | REPLACE → `pull_from(endpoint, Z85PublicKey server_pubkey, identity_key_name = kRoleIdentityName)` |
| `src/include/utils/hub_zmq_queue.hpp:320` | `push_to_with_auth(...)` signature param | REPLACE → `push_to(endpoint, identity_key_name = kRoleIdentityName, zap_domain = "")` |
| `src/utils/hub/hub_zmq_queue.cpp:142` | `pImpl->auth_opts_` member field on bind side | REPLACE → split into discrete `identity_key_name_`, `server_pubkey_`, `zap_domain_` members or scope-local in `start()` |
| `src/utils/hub/hub_zmq_queue.cpp:576-648` | `validate_auth_options(const ZmqAuthOptions &, bool bind_side)` helper | DELETE; fold inline into the new factory body (keystore_name presence, 40-char Z85 check on serverkey, etc.) |
| `src/utils/hub/hub_zmq_queue.cpp:656, 709` | `*_with_auth` entry calls to `validate_auth_options` | REWRITE to call the inline validators |
| `src/utils/hub/hub_zmq_queue.cpp:799` | `if (!pImpl->auth_opts_.keystore_name.empty())` gating CURVE setup | REPLACE with `if (!identity_key_name.empty())` — and after C2 this should always be true (factory pre-validates) |
| `src/utils/hub/hub_zmq_queue.cpp:908-977` | CURVE setup block (resolve `zap_domain` fallback, set publickey/secretkey via KeyStore, install ZapRouter, handle serverkey) | REFACTOR to consume factory params directly |
| `src/utils/service/role_api_base.cpp:321-330` | `build_tx_queue()` constructs `ZmqAuthOptions auth_opts` + populates 2 fields + calls `push_to_with_auth` | REWRITE → direct factory call `ZmqQueue::push_to_with_auth(endpoint, kRoleIdentityName, uid + ":" + tx_channel + ":tx")`; no struct intermediate |

---

## 3. Test sites — `tests/`

| File:line | What | Post-#158 |
|---|---|---|
| `tests/test_layer2_service/test_hub_zmq_queue.cpp:155, 188, 222` | `ZmqAuthOptions auth{}; auth.keystore_name = "test_identity"` | REWRITE to factory positional/named call |
| `tests/test_layer2_service/test_zmq_queue_auth.cpp:127, 155` | Error-message substring pins (`"connect-side ZmqAuthOptions requires serverkey_z85"`) | REWRITE substring; error text moves to factory body |
| `tests/test_layer2_service/test_zmq_queue_auth.cpp:220-230` | `TEST(ZmqAuthOptionsDefault, KeystoreNameDefaultsToEmpty)` mutation pin | DELETE (struct gone); add factory-default pin instead (e.g. that `push_to_with_auth(...).identity_name_default() == kRoleIdentityName`) |
| `tests/test_layer2_service/workers/zmq_queue_auth_workers.cpp:54` | `using ... ZmqAuthOptions;` | DELETE |
| `tests/test_layer2_service/workers/zmq_queue_auth_workers.cpp:75` | Doc-comment reference to `ZmqAuthOptions::keystore_name` | REWRITE → `identity_key_name` factory param |
| `tests/test_layer2_service/workers/zmq_queue_auth_workers.cpp:~17 scenarios` (lines 153-171, 214-231, 267-299, 395-440, 480, 527-604, 648, 695-763, 784-875) | Each scenario creates `ZmqAuthOptions`, populates 1-4 fields, passes to factory | REWRITE to factory call + optional `set_peer_allowlist()` post-call for `initial_allowlist` cases |
| `tests/test_framework/curve_test_setup.h:176` | Doc-comment reference | REWRITE |

**Tally:** ~42 test sites. Most are mechanical field→param rewrites; `zmq_queue_auth_workers.cpp` has ~17 distinct scenarios needing per-scenario care.

---

## 4. Doc sites

| File:line | Note |
|---|---|
| `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md:620` | "role_api_base.cpp::build_tx_queue populates `ZmqAuthOptions.my_seckey_z85`..." — stale; the `my_seckey_z85` field already DELETED by #173; update prose |
| `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md:689, 701, 756` | Correctly already document the C2 deletion plan; no rewrite needed |
| `docs/todo/AUTH_TODO.md:167, 396, 416, 418` | Already specify C2/C4 deletion + endpoint shape; no rewrite |

---

## 5. Field-by-field migration

| Old field | New shape |
|---|---|
| `keystore_name` | factory param `std::string_view identity_key_name = kRoleIdentityName` |
| `serverkey_z85` | factory param `Z85PublicKey server_pubkey` (PULL only) |
| `zap_domain` | factory param `std::string_view zap_domain = ""` (PUSH only) |
| `initial_allowlist` | **Removed from factory** — callers invoke `set_peer_allowlist(allowlist)` post-construction. Note the timing risk: broker hasn't pushed the snapshot yet at factory time, so producer defaults to deny-all (correct semantic but easy to misuse). |

---

## 6. Risks / coverage gaps to address as part of #158

1. **`role_api_base.cpp::build_tx_queue` is on the hot path** for every role↔hub producer-side queue. Typos or missing params silently route to wrong identity. Pin with a new L2 test that asserts `build_tx_queue` calls `push_to_with_auth` with exactly `(endpoint, kRoleIdentityName, expected_zap_domain)` (perhaps via a `BrokerStub`-like spy if needed, or via runtime check on socket sockopt).

2. **`validate_auth_options` deletion redistributes its 3 validations** (keystore_name non-empty, 40-char Z85 serverkey, KeyStore presence). Each needs an explicit destination — fold them into the factory body and verify with an L2 negative test per validation point.

3. **`initial_allowlist` orphaning**: the post-#158 caller MUST call `set_peer_allowlist()` separately. Add an L3 test that verifies "producer factory + skipped `set_peer_allowlist` call → consumer cannot connect (handshake-failed)" to pin the secure default.

4. **`ZmqAuthOptionsDefault` mutation pin DELETE** — its replacement should verify the FACTORY default for `identity_key_name` is `kRoleIdentityName` (the wire-canonical name).

5. **Recommended bundle order:**
   1. Introduce new factory signatures alongside old (`*_with_auth` keeps existing struct param).
   2. Inline validators into new factories.
   3. Migrate `role_api_base.cpp::build_tx_queue` to new signature.
   4. Migrate test workers scenario-by-scenario.
   5. Delete `ZmqAuthOptions` struct + the old `*_with_auth` signatures + `ZmqAuthOptionsDefault` test.
   6. Rename `_with_auth` → bare (this is the §C4 / task #160 follow-up; consider bundling with C2).

---

## 7. Out-of-scope (for #158 itself)

- `KnownRolesStore`, `KeyStore`, BrokerService::Config — already aligned with HEP-CORE-0040 round-5 design (no `ZmqAuthOptions` consumption).
- `BRC::Config::keystore_name` — separate slice; deferred per audit decision (HEP-CORE-0036 §I10 enables the eventual cleanup via shared-keypair fixtures).
- L3 broker tests in the masked window (#154) — those need their own re-creation pass; #158 changes will rebreak them again if they're un-masked first.

---

## 8. Recommended #158 PR shape

One PR, four commits, in this order:

1. **C2-prep**: add new factory signatures (`pull_from`, `push_to` taking discrete params); keep old `*_with_auth` + struct alive; new factories delegate to old via on-the-fly struct construction.
2. **C2-validators**: inline the validations from `validate_auth_options` into the new factory bodies; mark the old helper deprecated.
3. **C2-callers**: migrate `role_api_base.cpp::build_tx_queue` + the 17 test worker scenarios + 3 test_hub_zmq_queue sites to the new signatures.
4. **C2-cleanup**: delete `ZmqAuthOptions` struct, the old `*_with_auth` signatures, `validate_auth_options` helper, `ZmqAuthOptionsDefault` test; verify zero references via `grep -r ZmqAuthOptions`.

After commit 4: rename `pull_from_with_auth` → `pull_from`, `push_to_with_auth` → `push_to` (part of #160 — but cheap enough to bundle).
