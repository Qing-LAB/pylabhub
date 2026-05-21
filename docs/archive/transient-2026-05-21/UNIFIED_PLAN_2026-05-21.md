# Unified Action Plan — 2026-05-21 (rev 2)

**Status:** consolidates (a) pre-existing TODO_MASTER pending items
(#44, #66, #72, #73, #74, #75, #76, #77), (b) audit-log bug findings
B1-B13 (B1-B10 from 2026-05-20, B11-B13 from the multi-engine demo
work later in the session), (c) systematic code-review findings from
the 2026-05-21 Explore-agent sweeps (initial + native-engine focus),
(d) test-coverage review findings, and (e) the three-engine
throughput bench foundational data + multi-engine doc-parity work.

**Headline status (close of 2026-05-21):**

* 8 demos shipping clean — Python SHM single + dual hub, Python ZMQ
  single + dual hub, Lua single, Native single, Native bench, Hub-side
  Native (B13 payoff).
* 13 bugs (B1-B13) found by the demo harness; **6 fixed in code**,
  7 documented for follow-up.
* All three script engines (Python / Lua / Native) now exercise both
  role-side AND hub-side via the demo framework.
* Three-engine throughput baseline captured (4 KB slot, 35 s steady-
  state): Lua 139 MiB/s, Native 111 MiB/s, Python 90 MiB/s.

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

## Priority 1 — Code fixes for audit-log findings B1-B13

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
| **B11** | **`setup_infrastructure_` consumer-side + processor-in-side don't copy ZMQ fields** (`data_transport`, `zmq_node_endpoint`, `shm_name`-clear) to `RxQueueOptions` when `in_transport=zmq` — `build_rx_queue` sees default `data_transport="shm"` and dispatches the SHM path with bogus shm_name from in_channel, fails with "Failed to connect consumer to in_channel" | Add the same `if (is_zmq)` branch already present on the tx side: populate `data_transport`, `zmq_node_endpoint`, clear `shm_name` + `shm_shared_secret`.  Same pattern as B5 — config-to-opts translation layer in `setup_infrastructure_` was never exercised by L3 tests (which build opts manually). | S (2 sites) | **✅ FIXED** (commit `ed1e0ba`) |
| **B12** | **Role-host `entry_point` hardcoded for lua/python only**: `(sc.type == "lua") ? "init.lua" : "__init__.py"`.  For `script.type=="native"` the framework tried `<dir>/__init__.py` + `<dir>` as a .so, both failed | Extend the ternary to a 3-way: `(lua → init.lua) : (native → plugin.so) : __init__.py` | S (3 role hosts + 1 hub_script_runner) | **✅ FIXED** (commits `34871de` role hosts, `205f919` hub_script_runner follow-up) |
| **B13** | **NativeEngine had no hub-side `build_api_(HubAPI&)` override**: native could not be a hub script.  Failed at `engine.build_api(HubAPI) failed — engine likely lacks hub-side build_api_ override` | Add `bool NativeEngine::build_api_(HubAPI&)`: allocate NativeContextStorage, populate hub identity strings, call new `wire_hub(HubAPI*)` (minimum surface: log + uid + name + request_stop), invoke `fn_init_`, register lifecycle module.  Add `ctx_request_stop_hub` adapter for HubAPI casting | M | **✅ FIXED** (commit `f6079ec`) |

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

## Priority 2.5 — NEW findings from rev-2 work (2026-05-21 evening)

| # | Item | Effort | Status |
|---|---|---|---|
| N1 | **L3 test for `setup_infrastructure_` config-to-opts translation** — currently zero coverage; B5 + B11 both lived here.  Extract translation into separately-testable function; pin every field that should propagate per transport (SHM, ZMQ).  Would have caught both bugs.  Filed in test_layer3_datahub | M | OPEN — **HIGH PRIORITY** (systemic gap; more bugs likely lurking) |
| N2 | **NativeEngine `build_api_(HubAPI&)` — extend surface beyond MVP** | L | OPEN — B13 closed the gap, but currently exposes only log + uid + name + request_stop.  Missing: HubAPI's read accessors (list_channels, get_channel, list_roles, list_bands, …), metrics + query_metrics, control delegates (close_channel, broadcast_channel, post_event, augment_*), `on_event` hook on the engine for the event-shape callbacks (HEP-0033 §12).  Each needs a `ctx_X_hub` adapter that wraps the HubAPI method as a C function pointer + returns JSON-as-C-string where applicable |
| N3 | **L2 native plugins use wrong on_init/on_stop signature** — all canonical examples (`good_producer_plugin.cpp:96`, `native_multifield_module.cpp:72`) declare `on_init(const char *args_json)` but the documented ABI is `void on_init(void)`.  The framework's `FnVoidNoArgs` calls them no-args; the extra parameter works by accident.  Fix the canonical examples + bench/single-hub native demos to match the ABI | S | OPEN |
| N4 | **Lifecycle module registration with no-op shutdown** in NativeEngine (`native_engine.cpp:566-584`) — misleading design; finalization happens independently in `finalize_engine_()`.  Document or remove the lifecycle module registration | S | OPEN |
| N5 | **`docs/README/README_NativePlugins.md`** — operator-facing native plugin guide.  Discoveries this session: filename convention (`plugin.so`), schema string format (`name:type:count:dim_flag`, `|`-separated, trailing `0` is the dim-marker NOT byte offset), `PLH_DECLARE_SCHEMA` macro, on_init/on_stop signature, `request_stop()` vs `set_critical_error()` distinction, the documented C ABI for each callback (especially `on_band_message(const plh_band_message_args_t *args)` — I had a bug from getting this wrong), compile command via g++, recommended CMake helper | M | OPEN |
| N6 | **`cmake/pylabhubNativePlugin.cmake`** — USER-ORIENTED CMake helper module.  Plugin author drops it into their own project + calls `find_package(pylabhubNativePlugin REQUIRED PATHS /path/to/pylabhub/install)`.  Helper deduces include + lib paths from the install location.  Provides macro `plh_add_native_plugin(<target> SOURCES <files...>)` that handles `-shared -fPIC -std=c++20 -I<include>` plus the right link flags.  Critical: must NOT be the project's internal CMake — it's a deliverable for plugin authors who use pylabhub as a dependency | M | OPEN |
| N7 | **README_Scripting_{Python,Lua,Native}.md three-engine doc parity** — currently Python has full coverage (HEP-0011 + README_Deployment.md §5-8); Lua has just one tiny example; Native has only architectural notes in HEP-0028.  Each engine gets a dedicated doc following the same 1..8 structure (when to use → lifecycle → data structure → API → example → build/setup → pitfalls → cross-refs).  Cross-engine comparison table stays in HEP-0011 | L (~2-3 sessions) | OPEN |
| N8 | **Lua perf bench using max_rate + scalar (no random fill) — pure dispatch overhead** | S | OPEN — would let us compare engine dispatch cost in isolation; current 4 KB-with-random bench measures random-fill cost more than dispatch cost |
| N9 | **Bench with different slot sizes** (16 B, 4 KB, 64 KB, 1 MB) to characterise per-slot-size overhead curve for each engine | M | OPEN |
| N10 | **HEP-CORE-0011 §"Lua" expansion** — current ~10-line subsection; needs Lua-specific notes (`api.X(...)` not `api:X(...)`, FFI access patterns, no-numpy gotcha) | S | OPEN |
| N11 | **PythonEngine + LuaEngine on_band_message C/Lua signature** — for parity with the native `plh_band_message_args_t` discovery, double-check the signatures pin (3 separate args vs single struct ptr) match documentation cleanly | S | OPEN |

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

## Recommended sequencing for next 5 sessions (rev 2)

| Session | Focus | Outcome |
|---|---|---|
| **next** | P2.5-N1 (L3 setup_infrastructure_ test) + N5 (README_NativePlugins.md) + N6 (`cmake/pylabhubNativePlugin.cmake` user-oriented helper) + P1-B3/B4 small code fixes | Test-coverage systemic gap closed; native-plugin authors get full guidance + drop-in CMake; small code-fix cleanups |
| **+1** | N7 (three-engine doc parity — README_Scripting_{Python,Lua,Native}.md) + N10 (HEP-0011 Lua expansion) | Doc parity across engines — each engine gets its own dedicated page following the same 8-section structure |
| **+2** | N2 (NativeEngine `build_api_(HubAPI&)` extension — full HubAPI surface) + N3/N4 native-engine cleanups | Native becomes a real hub-script engine with full HubAPI coverage, not just MVP |
| **+3** | Hub State Query Layer Phase A + B + C (Layer 1 metadata + Layer 2 free functions + HubAPI refactor + script bindings) | Big-ticket API improvement; absorbs Group 2 #2/#3 + B6/B7 in one design |
| **+4** | Wave-B M9 (`RoleHostFrame<HostT>` CRTP) — picks up B2's corrected ordering by construction; OR HEP-CORE-0035 auth Phase 1 (#74) — closes B3 + production-readiness | Eliminates 3-way worker_main_ duplication, OR real auth instead of placeholder |

Tasks #66, #75, #76, #77 are independent and can interleave when
context fits.  N8/N9 (bench variants) are good "warm-up" tasks for
the start of any session.

---

## Health snapshot at session close (2026-05-21, rev 2)

**Code health:**
- **6 real code bugs found + FIXED** via the demo harness this session
  (B1 flexzone, B2 worker_main_, B5 rx-queue shm_name, B11 ZMQ rx-side
  field copy, B12 entry_point ternary (across 4 files), B13
  NativeEngine HubAPI binding).
- Code review found NO new dead code.
- One mechanical cleanup applied (obsolete includes).
- All shipped fixes well-commented with audit cross-references.

**Doc health:**
- HEP-CORE-0011, HEP-CORE-0019, README_Deployment.md substantively
  updated against current code.
- 20+ audit-log gaps drained or scheduled.
- Doc-parity gap surfaced (Python rich, Lua + Native thin) — captured
  in P2.5 N5/N7/N10 for next sessions.
- Multiple session-discovered findings filed in P2.5 (N1-N11).

**Validation infrastructure:**
- Demo framework (`share/demo_framework/runner.py`) is a real
  first-class tool.
- **8 demos shipping clean**: Python single-hub-shm, Python
  single-hub-zmq, Python dual-hub-shm, Python dual-hub-zmq,
  Lua single-hub-shm, Native single-hub-shm, Native bench (4 KB +
  random fill + band-coord shutdown), Hub-side Native (B13 payoff).
- Wave-B M8 dual-hub processor binary-validated.
- Task #44 (L4 pipeline test infra) effectively delivered by the
  framework.
- **Native engine now first-class for both role AND hub scripts** —
  B13 + B12 + B11 closed.

**Foundational throughput data (4 KB slot + random fill, 35s steady-state):**

| Engine | Slots/s | MiB/s |
|---|---|---|
| Lua (LuaJIT FFI loop math.random) | 35,646 | 139.2 |
| Native (C++ std::rand loop)       | 27,079 | 105.8 |
| Python (numpy `_rng.random`)       | 22,933 |  89.6 |

Caveat: Native used `std::rand` (libc, slow); a faster PRNG
(std::mt19937 / SIMD) would likely move native to first.

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
