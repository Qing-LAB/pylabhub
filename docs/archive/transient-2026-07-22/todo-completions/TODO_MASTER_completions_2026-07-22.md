# TODO_MASTER completions — extracted 2026-07-22

Completed-status narrative extracted from `docs/TODO_MASTER.md` during the
2026-07-22 reconciliation (the 07-18 status snapshot predated a four-day sprint;
34 commits landed `b0aa0f51..fc08850f`). Per `DOC_STRUCTURE.md §2.1.1` +
TODO_MASTER's maintenance rule: TODOs track what's *to do*, not what's *done* —
the shipped detail lives here, git is the historical record. Verified against
code + `git log`, not commit messages alone.

Prior completions index: `docs/archive/transient-2026-07-18/todo-completions/TODO_MASTER_completions_2026-07-18.md`.

---

## Shipped 2026-07-19 → 2026-07-22 (post-07-18-reconcile sprint)

- **Line 1 — CURVE auth chain: 🟢 PHASE 1 PRODUCTION-READY** (2026-07-17,
  REVIEW-E). Single-hub CURVE end-to-end across ZMQ (libzmq CURVE+ZAP) and SHM
  (`AttachProtocol`, HEP-0044). Follow-on hardening after the reconcile:
  `known_roles` allowlist moved into the encrypted hub vault (HEP-0035 §4.8,
  `4f7a853e`); inbox gained application-level replay defense (HEP-0027 §3.6,
  `062035a7`).
- **Line E — Admin-plane CURVE: ✅ SHIPPED 2026-07-19** — the former "#1 open
  security surface." Commits `132732ca`, `07ca94c9`, `be9d8dfc`, `5cd3be62`,
  `43050a98`, `f54da590`. CURVE-secured admin socket (reuses the broker CURVE
  keypair to `curve_server`; admin token stays a mandatory now-encrypted gate;
  loopback default) + typed operator-console (ROUTER + session + all 11 methods)
  + in-session replay defense (`HubHost::nonce_seen`). Design of record:
  HEP-CORE-0033 §11.1 + §11.3. Draft `DRAFT_curve_admin_protocol_2026-07-15.md`
  archived this batch. Residual polish only (tracked in `AUTH_TODO.md` Line E):
  reverse-notify channel, `origin_uid` stamping, 3 admin-test migrations.
- **Inbox: ✅ complete** — CURVE-authenticated (vault `known_roles`) + full
  cross-engine parity (native send, ABI v10) + replay defense. Schema arc:
  two-zone `SchemaRecord` + single 64-byte `datablock_half ‖ flexzone_half`
  fingerprint; inbox-as-schema-record retired (`fc08850f`, 2026-07-22; merge map
  in `DOC_ARCHIVE_LOG.md` 2026-07-22 batch).
- **Line 2 — SMS consolidation (HEP-0043): ✅ shipped 2026-07-07.** Access is
  `secure().keys()`. Residual: SEC-Fold-1b §8/§10 vault + script-crypto content
  migration (housekeeping).
- **Line 4 — IAttachChannel foundation (HEP-0044): ✅ shipped**; no next-action.

## FullSystem review (`REVIEW_FullSystem_2026-07-20`) — 27 of 56 resolved (07-20→07-22)

Load-bearing items closed (full per-finding detail + evidence lives in the review
doc's `✅` blocks):

- **#67 — `ReplayGuard`** was ALREADY fixed pre-review by `5df0fd1d`; the 07-20
  snapshot was stale. `check_and_record` prunes on its OWN monotonic steady clock
  (no `wall_ts` parameter), so client time structurally cannot reach the dedup
  window; every plane sizes `window_ms = 2 * skew`. L1 pins in
  `test_replay_guard.cpp`.
- **#68 — four dead/no-op identity validators** deleted/retired (items 1–4):
  single-validator consolidation (`2020078a`), redundant key-rotation gate retired
  (`2283cb9d`), legacy `RoleIdentityPolicy` string gate deleted (`c7f4f608`),
  §4.2 stale-SHM cross-check retired (`1d91c753`).
- **#70 — resource-cleanup leaks** fixed: consumer teardown on the 4 FATAL startup
  returns (`28515adb`), BRC never-answered-request reaper + Lua eval stack
  (`d626324d`).
- **#71 — masked suites + untested helpers** (sub-issues 1–4): HubHost lifecycle
  FSM suite un-masked (`273744a3`), ABI classifier BUILD_ONLY-vs-minor fix + tests
  (`7f107950`), synchronous logging path pinned (`f12e6daa`), loop-timing pins
  (`513f4bda`).
- **schema findings** resolved-by-redesign (this session, folded into the two-zone
  arc): `schema_utils.hpp:239` duplication (two-jobs split) + `:331` flexzone-loss
  (two-zone record).
- Plus metrics `_collected_at` per-group stamping + HEP-0021 §16.8 endpoint
  mid-life rules (`ad491e40`), explicit `script.type:"none"` (`8cc13e33`), and
  the low-severity items-1–3 self-review cleanups (`84014d52`).

The 29 still-open findings stay tracked in `TODO_MASTER.md` "Active code reviews"
(by cluster) + the review doc (file-level).

## Stale-listing corrections folded in this reconcile

- #235 Python band-accessor fix + #238 log-format standardization were verified
  SHIPPED 2026-06-27 against code (`consumer_api.cpp:75`; `event=` format
  deployed) — the old master's "HIGH-priority open" listing was stale. Only
  residual: #235 L3 parity regression tests, fold into #232.
