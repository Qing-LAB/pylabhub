# Unified Action Plan — 2026-05-21

**Status:** consolidates (a) pre-existing TODO_MASTER pending items
(#44, #66, #72, #73, #74, #75, #76, #77), (b) audit-log bug findings
B1-B10 from the 2026-05-20 demo-harness session, and (c) the
systematic code-review findings from the 2026-05-21 Explore-agent
sweep.

**Method:** the code-review agent confirmed the codebase is in good
shape after this session's three shipped fixes (B1 flexzone, B2
worker_main_ ordering, B5 rx-queue shm_name).  The remaining items
are a mix of doc closure, small mechanical cleanups, and substantial
refactor/feature work that's been on the books.

---

## Priority 0 — Immediate, low-risk, high-confidence

These are mechanical or near-mechanical changes that should ship
soon.  Total effort: ~half a day.

| # | Item | Effort | Status |
|---|---|---|---|
| P0.1 | Remove obsolete `#include "utils/broker_request_comm.hpp"` from `producer_role_host.cpp`, `consumer_role_host.cpp`, `processor_role_host.cpp` (BRC is reached transitively via `RoleHandler` since Wave-B M5-M7) | S — 3 one-line deletes | **DONE this session** (2026-05-21 commit) |
| P0.2 | Add `stop_reason()`, `clear_inbox_cache()`, `slot_logical_size()`, `flexzone_logical_size()` to `README_Deployment.md` §8.3 (existing bindings, missing from docs) | S — 4 method entries | **DONE this session** (2026-05-21 commit) |
| P0.3 | HEP-CORE-0011 §"Script Callback Contract" — add per-callback API availability table (closes Audit B10 root cause documentation) | S | **DONE this session** (2026-05-21 commit) |
| P0.4 | README_Deployment.md §8.5 — rewrite to describe HEP-0027 inbox + HEP-0030 band; remove obsolete `api.broadcast`/`api.send`/`api.consumers()` claims | S | **DONE this session** (2026-05-21 commit) |
| P0.5 | Move B9 → audit-log resolution table (eliminated by band-coordinated shutdown demo, not by code fix) | S | **DONE this session** |

## Priority 1 — Code fixes for audit-log findings B1-B10

| # | Bug | Fix candidate | Effort | Status |
|---|---|---|---|---|
| B1 | `role_api_base.cpp:258` empty-flexzone crash | Gate `schema_spec_to_zmq_fields` call on `fz_spec.fields.empty()`; pass empty vector if true | S | **✅ FIXED** (commit `2be5156`) |
| B2 | Three role-hosts: api state wired AFTER `setup_infrastructure_` | Split worker_main_ Step 2 into Step 2a (api state, unconditional) + Step 2b (`setup_infrastructure_`, gated) | M (3 sites) | **✅ FIXED** (commit `2be5156`) |
| B3 | `auth.keyfile=""` half-state (broker uses ephemeral CURVE but doesn't publish hub.pubkey → role-side handshake fails silently) | Make empty keyfile a hard config-load error: "Hub requires a vault for CURVE keypair — run `plh_hub --keygen` first" | S | OPEN |
| B4 | `--init` SHM secret defaults to 0 (silent fall-through to ZMQ on SHM-transport pipelines) | `plh_role --init` should generate a non-zero random secret default for `out_shm_secret` / `in_shm_secret` | S | OPEN |
| B5 | Consumer/processor `RxQueueOptions::shm_name` never populated | Set `opts.shm_name = ch` where `ch` is the in_channel | S (2 sites) | **✅ FIXED** (commit `59ad703`) |
| B6 | Consumer `rx.fz` claimed in README §8.4 but ConsumerAPI's RxChannel pybind binding only exposes `.slot` | Either bind `.fz` in `consumer_api.cpp:191` OR update §8.4 to say "use `api.flexzone()` instead" — pick one canonical surface | S | DOC-UPDATED §8.3; binding still missing |
| B7 | Processor `api.flexzone()` requires `side` argument (dual flexzones); README example omits the arg | README §8.4 + HEP-0011 — note processor needs `api.flexzone(api.Tx)` | S | DOC-UPDATED in §8.3; §8.4 still has the old example |
| B8 | numpy not in bundled python (`pip install numpy` works ad hoc; demo setup should chain `plh_pyenv install` with a `requirements.txt`) | Add `requirements.txt` to demo dirs that need numpy; have setup_commands run `plh_pyenv install --requirements <demo>/requirements.txt` | M | OPEN |
| B9 | SIGTERM-cascade teardown race (`Socket operation on non-socket` from BRC ctrl thread when broker dies first) | ELIMINATED by demo's band-coordinated shutdown (consumer → drain → producer + processor → stop) — race never triggers when hub stays alive while roles deregister | — | **✅ ELIMINATED** (commit `d45311a8`) |
| B10 | `api.band_join` in `on_init` silently fails (handler not yet up at Step 5; needs Step 6) | Two options: (a) defer in C++ (queue band_join, replay after handler ready); (b) surface `std::runtime_error("api.band_join: handler not yet up")` — option (b) is smaller | S | DOC-UPDATED HEP-0011 §"API availability per callback"; code change pending |

## Priority 2 — Code review findings (2026-05-21 Explore agent)

| # | Item | Effort | Notes |
|---|---|---|---|
| R.A | Category A (dead code): NO new items beyond Group 2 #1-#5 already decided | — | Clean |
| R.B1 | Stale comments referencing deleted subsystems (`start_ctrl_thread`, `set_broker_comm`, P2C ctrl socket, `role_host_lifecycle.cpp`) | M (scrub ~12 sites) | LOW priority — comments are accurate archaeology |
| R.B2 | Obsolete `#include "utils/broker_request_comm.hpp"` in 3 role hosts | S | **DONE** as P0.1 |
| R.C1 | 4 methods missing from README_Deployment.md §8.3 | S | **DONE** as P0.2 |
| R.C2 | §8.5 ZMQ-messaging section stale | S | **DONE** as P0.4 |
| R.D1 | L4 binary-pipeline integration test coverage (Steps 5-6, 8 of worker_main_ have no L4 test) | L | **EFFECTIVELY DONE** via demo framework (the two demos in `share/py-demo-*-shm/` and `py-demo-dual-hub-processor/` exercise the full pipeline at the binary level — they ARE Task #44 reimagined) |
| R.D2 | `--validate` mode internal-state correctness | S | LOW priority |
| R.D3 | `api.is_in_band(band)` has zero test coverage | S | LOW priority |

## Priority 3 — Substantial refactor / feature work (TODO_MASTER)

| Task # | Item | Effort | Critical-path? |
|---|---|---|---|
| #44 | L4 processor + consumer test infrastructure (Wave-D) | **EFFECTIVELY DONE** by demo framework | YES — was the dual-hub validation blocker; now closed by `share/py-demo-dual-hub-processor/` |
| #66 | S1 Phase B: migrate ZmqQueue + InboxQueue to `apply_socket_policy` | L (multi-day) | NO — same risk profile BRC had pre-S1 |
| #72 | Wave-B M9: `RoleHostFrame<HostT>` CRTP template | L (multi-day refactor) | NO — lifts the duplicated worker_main_ across 3 role hosts into one template; will inherit B2's corrected phase ordering |
| #73 | HEP-CORE-0033 Phase 10 doc closure | M | NO — HEP-CORE-0019 §9 per-producer metrics tree + cross-reference survey |
| #74 | HEP-CORE-0035 auth implementation | L (7 phases) | YES for production — CURVE keys are mandatory but admission policy is placeholder |
| #75 | HUB_TARGETED_ACK wire frame | M | NO — federation-only; C++ surface in place |
| #76 | Script reload (api.request_reload) | M | NO — design captured in `docs/tech_draft/SCRIPT_RELOAD_DESIGN_2026-05-20.md` |
| #77 | Tier 2 dynamic callbacks | M | NO |
| **NEW** | Hub State Query Layer (`hub.snapshot()` + Layer 2 free fns) | L (~3-4 days) | NO — design in `docs/tech_draft/hub_state_query_layer_design.md`; absorbs Group 2 #2/#3 |

## Priority 4 — Documentation: drain audit log

| Item | Effort | Status |
|---|---|---|
| HEP-CORE-0011 §"Init Protocol" Step 2a/2b ordering | S | **DONE** (commit `089e63e`) |
| HEP-CORE-0019 §5.4.1-5.4.3 metrics reference tables | M | **DONE** (commit `089e63e`) |
| HEP-CORE-0011 §"API availability per callback" (B10) | S | **DONE** (commit pending this commit) |
| README_Deployment.md §4.2 hub.json schema sync | M | **DONE** (commit `089e63e`) |
| README_Deployment.md §4.3 binary names + `--keygen` flow | S | **DONE** (commit `089e63e`) |
| README_Deployment.md §5.1 / §6.1 / §7.1 role configs `in_*`/`out_*` renames | M | **DONE** (commit `089e63e`) |
| README_Deployment.md §8.3 API methods sweep | M | **DONE** (commits `089e63e` + pending) |
| README_Deployment.md §8.5 rewrite (band + inbox replacement for retired peer messaging) | S | **DONE** (commit pending) |
| **PENDING:** §5.2 / §6.2 / §7.2 Python script examples (still use old field names) | M | OPEN |
| **PENDING:** §8.4 Flexzone — note `side` arg for processor (B7) | S | OPEN |
| **PENDING:** §10 Multi-Hub Pipelines — predates Wave-B refactor; use dual-hub demo as canonical example | L | OPEN |
| **PENDING:** Top-level README.md + GettingStarted.md staleness sweep | M | OPEN |
| **PENDING:** Native plugin doc (HEP-CORE-0028) — not touched this session | M | OPEN |
| **PENDING:** `share/demo_framework/README.md` polish — describe the harness as a first-class validation tool | S | OPEN |

## Recommended sequencing for next 5 sessions

| Session | Focus | Outcome |
|---|---|---|
| **next** | P1-B3 + P1-B4 code fixes (small, high-clarity) + remaining doc updates (§5.2/6.2/7.2 + §8.4 + §10) | Audit log fully drained at the code level; deployment doc internally consistent |
| **+1** | Hub State Query Layer Phase A (Layer 1 metadata + Layer 2 functions); absorbs Group 2 #2/#3 | Big-ticket API improvement; sets up scripts to query coherent hub state |
| **+2** | Hub State Query Layer Phase B + C (HubAPI refactor + script bindings) | Completes the layered query API |
| **+3** | Wave-B M9 (`RoleHostFrame<HostT>` CRTP) — picks up B2's corrected ordering by construction | Eliminates 3-way worker_main_ duplication |
| **+4** | HEP-CORE-0035 auth implementation (Phase 1 of 7) — closes B3 properly + production-readiness | Real auth instead of placeholder |

Tasks #66, #75, #76, #77 are independent and can interleave when context fits.

---

## Health snapshot at session close (2026-05-21)

**Code health:**
- 3 real code bugs found + FIXED via demo harness this session.
- Code review found NO new dead code (Group 2 sweep + this session's fixes have it clean).
- One mechanical cleanup applied (obsolete includes).
- All shipped fixes well-commented with audit cross-references.

**Doc health:**
- HEP-CORE-0011, HEP-CORE-0019, README_Deployment.md substantively updated against current code.
- 17+ audit-log gaps drained or scheduled.
- Some sections still pending (script-example bodies, §10 multi-hub).

**Validation infrastructure:**
- Demo framework (`share/demo_framework/runner.py`) is a real first-class tool.
- Two demos shipping clean (single-hub max-rate throughput; dual-hub cross-hub cascade).
- Wave-B M8 dual-hub processor binary-validated for the first time.
- Task #44 (L4 pipeline test infra) effectively delivered by the framework.

**Open critical-path items for "shippable":**
- HEP-CORE-0035 auth implementation (#74) — only true production-readiness gap.
- Hub State Query Layer (NEW item; design captured) — API improvement.
- Wave-B M9 (#72) — code-quality, not capability.
- HEP-CORE-0033 Phase 10 doc closure (#73) — doc hygiene.

**This session ships substantial value:**
- 14+ commits (demo framework + 3 code fixes + 2 demos + comprehensive doc updates + unified plan).
- Dual-hub binary validation, the previously-named #1 blocker.
- Bottleneck identification methodology (per-role metrics in on_stop).
- Coordinated-shutdown pattern using band facility.
