# CURVE-secured Admin Protocol — implementation plan

**Design authority:** `docs/HEP/HEP-CORE-0033-Hub-Character.md` §11.1
(Transport) + §11.3 (Authorization).  The design is finalized there;
this file is only the code-migration checklist and does not restate or
compete with it.

**Scope:** the *simple* first version — CURVE-wrap the admin socket with
the existing broker keypair, keep the init-generated admin token as a
mandatory + now-encrypted auth gate.  The admin plane is rarely used
today; per-operator identity (a `known_admins` allowlist), streaming
admin, and any ROUTER/DEALER move are explicitly **future expansion**
(noted in §11.1 / §11.3) and out of scope here.

**Status:** design documented (HEP-0033 §11); implementation not started.

---

## Why (one line)

The admin socket is the one privileged hub interface still plaintext +
bearer token; the ctor even permits a network bind that sends the token
in the clear.  HEP-0033 §11 now specifies CURVE-secured admin; this is the
code to match it.

## Implementation checklist (composes with existing infra)

1. **`AdminService::run()`** — before `bind()`, set `curve_server = 1` +
   `curve_secretkey = vault.broker_curve_secret_key()`.  No `zap_domain`
   (encrypted-but-not-key-gated; keeps the admin socket off the broker's
   single inproc ZAP handler, §7.4).  Reuses the `curve_server` pattern
   already in `broker_service.cpp`.
2. **Token gate mandatory** — delete the `token_required=false` branch and
   the "loopback-required-when-token-off" enforcement in the
   `AdminService` ctor; keep the existing constant-time token comparison.
   Loopback becomes the *default* endpoint, not a security requirement.
3. **`HubAdminConfig`** — remove the `token_required` field
   (`hub_admin_config.hpp`); admin is always token-gated when enabled.
4. **Admin client / CLI** — connect with an ephemeral CURVE keypair +
   `curve_serverkey = <hub broker pubkey>`; keep sending `token` in the
   request.  (A test-side `AdminWireClient` here is the near-identical
   sibling of `BrokerWireClient` — it unblocks the 3 admin-triggered
   sweep tests: `close_channel` ×2, `broadcast_hub_queue`.)
5. **Retire the plaintext path atomically** with the hub / `broker_proto`
   version bump — no dual-accept window (project frame: hard-enforce).
6. **Tests** — an admin-plane wire test suite (auth accept/deny,
   encryption, each §11.4 method) against the CURVE socket; then migrate
   the 3 sweep tests onto the `AdminWireClient`.

## Verification the design is grounded (already confirmed)

- Broker keypair reusable: `HubVault::broker_curve_secret_key()` /
  `_public_key()` (`hub_vault.hpp`).
- Admin token already minted at init: `generate_admin_token()`
  (`hub_vault.cpp:94`, at vault create :135) + constant-time check
  (`admin_service.cpp:255+`).
- `curve_server` + `zap_domain` mechanism: `broker_service.cpp:1029+`,
  `security/zap_router.hpp`.

Tracked in `docs/todo/AUTH_TODO.md`.
