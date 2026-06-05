# API_TODO completions archive — 2026-06-05

This file preserves verbatim prose for API_TODO entries that were verified
shipped in code as of 2026-06-05.  Moved here so the active API_TODO can
focus on open items.  No content summarized; full prose retained for
context.

Source file: `docs/todo/API_TODO.md`.

---

## 2026-05-31 — Task #78 final closure via E′-1 + E′-2a + E′-2b + E′-2c

Multi-step landing (commits ffbe6a9c → 2e730fa6 → E′-2c):

- **E′-1** (commit `ffbe6a9c`) rolled back C′-2's `--vault-mode`
  flag + `vault_path_resolve` module + 5-mode taxonomy as
  invented terminology not grounded in an operator-facing decision
  the project needed.  Restored pre-C′-2 baseline + C′-1.
- **E′-2a** (commit `51f76d55`) strengthened `parse_auth_config`
  to reject empty `auth.keyfile` (extending C′-1's missing-field
  rejection).  pylabhub is a vault — no in-memory CURVE mode.  L4
  fixtures refactored to use placeholder paths instead of `""`; L4
  tests hardened to verify actual file artifacts (mode 0600, exact
  path, no-vault-on-failed-keygen) instead of relying on exit code.
- **E′-2b** (commit `2e730fa6`) shipped the hub UID-keyed vault
  filename (`<hub_uid>.vault` — HEP-CORE-0033 §6.5 revised; closes
  the silent-collision footgun), the `--keygen` no-silent-overwrite
  check on both binaries (with byte-for-byte content-unchanged L4
  verification), and the symmetric
  `HubDirectory::warn_if_keyfile_in_hub_dir` helper (closes the
  hub-vs-role asymmetry from the holistic review's Finding #6).
- **E′-2c** finalized the HEP docs: HEP-CORE-0024 §3.4 + §3.4.1
  rewritten with the design intent (script-write attack vector,
  load-bearing warning), HEP-CORE-0033 §6.5 + §7.1 + §7.2 mirror
  the role side, HEP-CORE-0035 §4.6.3 past-tense the #78 closure,
  HEP-CORE-0038 §2.2 corrected, DRAFT_HEP-0036 §5.1.1 annotated
  CLOSED.  README_Deployment.md rewritten: `hub.auth.keyfile` and
  role-side `*.auth.keyfile` are marked REQUIRED (was "no");
  description cites the relevant HEP and the security-warning rule;
  the §4.3 "running the hub" walkthrough is updated for the
  no-overwrite contract and the UID-keyed filename.

### Final shipped contract (Task #78)

`auth.keyfile` is REQUIRED and must be non-empty (no in-memory
CURVE mode); hub vault filename is UID-keyed (`<hub_uid>.vault`);
`--keygen` refuses to overwrite an existing vault file; symmetric
`warn_if_keyfile_in_hub_dir` closes hub-vs-role asymmetry.

---

## 2026-05-30 — Commit C′-1 (A1 of #78 / #101 sub-phase 1D)

`parse_auth_config` (`auth_config.hpp`) now throws on missing
`<section>.auth` object OR missing `auth.keyfile` field — silent
default to ephemeral is no longer possible.  Operator MUST opt
explicitly: `"keyfile": "<path>"` for vault mode or `"keyfile": ""`
for ephemeral mode.  Hub-side and role-side share the same parser
(`hub_identity_config.hpp` delegates auth to `parse_auth_config`).
Error messages cite HEP-CORE-0024 §3.4 + HEP-CORE-0033 §7.1.  L2
fixtures updated (3 role + 1 hub minimal JSON helpers).  Tests: 9
new (5 role + 3 hub + 1 uid-autogen fixture refresh); all 1445 L1+L2
+ 408 L3 + 92 L4 tests pass.

---

## Task #101 — HEP-CORE-0035 §4.6 key-file ACL discipline

**Verified shipped** — `src/utils/security/key_file_acl.{hpp,cpp}`
exists with both `verify_keyfile_acl` and `set_keyfile_mode`
exported; `--keygen` / `--init` SETS modes (0600 for `*.sec`, 0700
for keystore dirs, 0750 for `known_roles/`); binary startup
VERIFIES modes and refuses to start with OpenSSH-style actionable
error (path + observed mode + required mode + exact chmod command).

Mechanically independent of #74 ZAP plumbing.  Layered on B3 (#78)
+ B4 (#79).  Spec: HEP-CORE-0035 §4.6.

---

## Code-ahead removals from D2 / D3 drift batches (already shipped in code as of 2026-06-05)

These items were carried in the API_TODO drift / polish lists as
"open" but the underlying code change has already shipped.  Moved
here for archive context; removed from the active API_TODO.

### C2 — Heartbeat tick branches on `role_tag` string (CLOSED)

Doc claim: `role_api_base.cpp:806-823` branches on `role_tag` string;
HEP-0033 §19.3 says iterate `handler_->presences()`.

Code reality: `role_api_base.cpp` now iterates `handler_->presences()`
per HEP-0033 §19.3 step 3.  Inline audit comment: "Closes audit H2
(2026-05-15) + audit C2 (`REVIEW_Connection_Inbox_Band_2026-05-17.md`)."

### X1 — Delete `BrokerRequestComm::send_notify` (CLOSED)

Doc claim: `broker_request_comm.cpp:691` has zero callers; delete.

Code reality: `broker_request_comm.cpp` audit comment already
records "Audit O1 (2026-05-17): `BrokerRequestComm::send_notify`
removed".  Symbol no longer in the public BRC API.
