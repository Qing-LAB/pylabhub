# Demo Framework — Deployment Doc Audit

**Status:** in-progress.  Created 2026-05-20 alongside the demo
framework scaffold (`share/demo_framework/`).

**Purpose:** As we design the single-hub and dual-hub demos by reading
`docs/README/README_Deployment.md` + relevant HEPs, every gap between
the docs and observed code behaviour gets logged here.

**Outcome:** at the end of Phase 4 (iteration), this log seeds either
(a) doc updates to `README_Deployment.md` + HEPs to close the gaps,
or (b) code changes when the doc is correct but the code drifted.

## Gap-log format

```
### G<N> — <short title>
- **Source:** <doc location, e.g. README_Deployment.md §4.2>
- **Observed:** <what the doc says or omits>
- **Reality:** <what the binary actually accepts / emits, with file:line>
- **Resolution candidate:** doc-fix | code-fix | tbd
- **Affects:** <single-hub demo / dual-hub demo / both>
```

## Findings (running list)

### G1 — Stale binary names throughout deployment doc
- **Source:** `README_Deployment.md` §4.3 (line 254), §4.3 (line 260), and §5/§6/§7 invocation sections.
- **Observed:** All examples use `pylabhub-hubshell`, `pylabhub-producer`, `pylabhub-consumer`, `pylabhub-processor`.
- **Reality:** Binaries are now `plh_hub` (broker) and `plh_role` (unified producer/consumer/processor; selects via `--role <tag>`).  Confirmed via `/build/stage-debug/bin/plh_hub --help` + `plh_role --help`.
- **Resolution candidate:** doc-fix — sweep §4.3, §5, §6, §7 + any other invocation examples.  Add a top-of-doc note that Wave-B M0..M8 unified the role binaries.
- **Affects:** documentation accuracy; both demos.

### G2 — §4.4 says hub-script can be Python OR Lua, §4.2 field table says only `"python"`
- **Source:** `README_Deployment.md` §4.2 (line 240) vs §4.4 (lines 269-272).
- **Observed:** §4.2 field reference: `script.type — Script language; "python" is the only supported value`.  §4.4 prose: `<hub-dir>/script/python/__init__.py (Python) or <hub-dir>/script/lua/init.lua (Lua), selected by hub.json:script.type`.
- **Reality:** Hub Lua is wired today (verified via `src/scripting/lua_engine.cpp` `build_api_(HubAPI&)` at line 471).  §4.2 field table is the stale one.
- **Resolution candidate:** doc-fix — update §4.2 field table to list `"python"` / `"lua"`.
- **Affects:** documentation accuracy; no demo impact (demo uses Python).

### G3 — §4.3 mentions `--dev` flag for hub but `plh_hub --help` doesn't list it
- **Source:** `README_Deployment.md` §4.3 (line 260): `pylabhub-hubshell <hub-dir>/ --dev`.
- **Observed:** Doc claims a `--dev` mode for ephemeral keypair / no vault.
- **Reality:** `plh_hub --help` lists only `--init` / `--validate` / `--keygen` modes + run mode.  No `--dev` flag.
- **Resolution candidate:** tbd — try `plh_hub --dev <dir>` empirically.  Either: (a) the flag is silently accepted and works; (b) it was renamed; (c) it was removed and roles now must use keygen + vault for ALL runs; (d) `--help` is incomplete.
- **Affects:** demo running — without a dev mode, the demo needs vault setup, which is friction for a self-contained demo.

### G4 — Channel-name grammar not documented in deployment doc
- **Source:** `README_Deployment.md` §5.1 example uses `"channel": "lab.sensors.temperature"`; no field-level grammar reference.
- **Observed:** Doc gives examples but no grammar spec; doesn't reference `HEP-CORE-0033 §G2.2.0b` from the channel/role_uid field rows.
- **Reality:** Channel names go through `is_valid_identifier` validation at every REG/DEREG/BAND gate (R3.5b shipped 2026-05-19).  Pattern is `<tag>.<dotted-segments>` per HEP-CORE-0033 §G2.2.0b.
- **Resolution candidate:** doc-fix — cross-reference HEP-0033 §G2.2.0b in §5.1 / §6.1 / §7.1 field tables for `channel` / `in_channel` / `out_channel`.
- **Affects:** documentation; configurable demo channel names.

### G5 — §8.3 claims `api.shm_blocks(channel)` and `api.list_channels()` are available on role API
- **Source:** `README_Deployment.md` §8.3 (lines 882-883).
- **Observed:** Listed as available "all roles" API methods.
- **Reality:** Per Group 2 #2 walkthrough (this same session, candidate `query_shm_info`), the BRC client wrapper for `SHM_BLOCK_QUERY_REQ` exists at `broker_request_comm.cpp:925` but is NOT bound to the role script API (no `.def("shm_blocks", ...)` in `producer_api.cpp` / `consumer_api.cpp` / `processor_api.cpp`).  `list_channels` may or may not be bound — needs verification.
- **Resolution candidate:** code-fix + doc-fix.  Either bind these per the Hub State Query Layer design (`docs/tech_draft/hub_state_query_layer_design.md`) — which is the planned route — or remove the doc claims until the binding lands.
- **Affects:** scripts that try to call `api.shm_blocks()` or `api.list_channels()` will fail.  Our demo doesn't need them, so the demo can sidestep; but the doc is misleading.

### G6 — §8.3 claims `api.notify_channel` / `api.broadcast_channel` on role API
- **Source:** `README_Deployment.md` §8.3 (lines 880-881).
- **Observed:** Listed as broker-messaging methods available to all roles.
- **Reality:** Per the R3.6 finding (2026-05-17, closed), `handle_channel_notify_req` was deleted entirely — the broker no longer handles `CHANNEL_NOTIFY_REQ`.  Also per HEP-0030 §9, `CHANNEL_BROADCAST_REQ` is retired in favour of band-broadcast.  Doc references are stale.
- **Resolution candidate:** doc-fix — remove or replace with current alternatives (band messaging per HEP-CORE-0030).
- **Affects:** scripts using these methods will fail.  Our demo doesn't use them.

### G7 — §8.3 lists `api.broadcast(data)` / `api.send(identity, data)` / `api.consumers()` peer messaging
- **Source:** `README_Deployment.md` §8.3 (lines 931-933) + §8.5.
- **Observed:** Documented as producer/processor → consumer ZMQ peer messaging via the broker's P2C ctrl socket.
- **Reality:** Wave-B M4f deleted `start_ctrl_thread` + the legacy P2C ctrl socket.  The replacement is HEP-CORE-0027 inbox messaging (peer-to-peer ZMQ ROUTER per role).
- **Resolution candidate:** doc-fix — replace §8.5 with the HEP-0027 inbox messaging model, OR verify whether these methods still exist under a different transport.
- **Affects:** scripts using these methods will likely fail.  Our demo doesn't use them.

### G8 — Auth/access fields documented as not-yet-parsed (honest)
- **Source:** `README_Deployment.md` §4.2 banner (lines 188-195).
- **Observed:** Doc honestly states `hub.connection_policy` / `hub.channel_policies` / `hub.known_roles` are accepted but not parsed (HEP-CORE-0035 placeholder).
- **Reality:** Matches the doc.
- **Resolution candidate:** none — this is correctly documented; closes when HEP-CORE-0035 lands (Task #74).
- **Affects:** demo can set `connection_policy: "open"` safely (or omit it).

### G9 — `plh_hub --dev` flag is GONE (RESOLVED + new bug found)
- **Source:** `README_Deployment.md` §4.3 line 260: `pylabhub-hubshell <hub-dir>/ --dev`.
- **Observed:** Doc documents `--dev` as the "no vault, ephemeral key" mode.
- **Reality:** Verified empirically — `plh_hub --dev <dir>` returns `Unknown argument: --dev`.  The flag is not in `--help` and is not accepted by the parser.
- **Replacement workflow (verified via `test_plh_hub_role_roundtrip.cpp`):** set `PYLABHUB_HUB_PASSWORD` env var, run `plh_hub --config <hub.json> --keygen`.  This generates `vault/hub.vault` AND writes `hub.pubkey` (per F1 fix in `HubConfig::create_keypair`).  THEN run normally.
- **Reframed (B3) — `auth.keyfile=""` is a half-state that violates the deployment design.**  Initial framing called this a bug to patch.  Stepping back to the system-level deployment design — informed by user direction "CURVE is always required" — the right fix is to REMOVE this path, not patch it.

  **Design principles for the deployment:**
  1. CURVE is mandatory on every hub↔role link.  No plaintext mode.
  2. Each hub has a stable identity (vault-encrypted CurveZMQ keypair); hub.pubkey is the broker's public key, copied to each role's `hub_dir`.
  3. Role identities are optional (HEP-CORE-0035 future) — by default roles generate an ephemeral CurveZMQ keypair at connect time (`broker_request_comm.cpp:456`).
  4. Setup is one-shot per hub: `plh_hub --init <dir>` then `PYLABHUB_HUB_PASSWORD=… plh_hub --keygen <dir>` writes both vault AND hub.pubkey (F1 fix verified working).
  5. Every restart of hub+role uses the same vault → no re-handshake / no rekey churn.

  **Current code state:**
  - The `--keygen` path is correct and consistent (F1 fix wired `HubVault::publish_public_key` into `HubConfig::create_keypair`).  L4 binary roundtrip (`test_plh_hub_role_roundtrip.cpp`) validates this end-to-end.
  - The `auth.keyfile=""` path is the half-state:
    - `HubConfig::load_keypair` returns false ("operator opt-out").
    - `BrokerService` falls through to the ephemeral-keypair branch (`broker_service.cpp:3691`, comment "Generate ephemeral keypair (--dev mode; no vault)") — broker now uses CURVE.
    - But `hub.pubkey` is NEVER published in this branch (publication is only triggered from `--keygen` / `HubConfig::create_keypair`).
    - Role-side reads `<hub_dir>/hub.pubkey` for CURVE pin; finding nothing, connects plaintext.
    - **Plaintext role ⇄ CURVE broker = silent handshake failure** → REG_REQ times out, then `Socket operation on non-socket` when the broken socket is reused.
  - Half-state code path probably survived from when `--dev` was a real CLI flag (now deleted) and the empty-keyfile branch was the dev-mode plaintext path.  Someone changed the broker side to ephemeral-CURVE without updating role-side pubkey discovery.

  **Resolution per design-first framing:** code-fix — make `auth.keyfile=""` a hard config-load error: "Hub requires a vault for CURVE keypair — run `plh_hub --keygen` first."  Removes the half-state.  Single canonical setup path.  Demo / dev / CI use scripted `--keygen` as part of setup, no different from any other first-run.  The demo framework's `setup_commands` manifest block provides the automation hook.

  **Affects:** the empty-keyfile branch in `hub_config.cpp:178-179` + `broker_service.cpp:3680-3697` + the stale "Generate ephemeral keypair (--dev mode; no vault)" comment.  Demo manifest gets a `setup` block that runs `plh_hub --keygen` with `PYLABHUB_HUB_PASSWORD` before launching processes.

### G17 — `--init` template defaults `_comment` to no comments, but binary REJECTS unknown keys (resolved as note)
- **Source:** Discovery while writing hub.json from `--init`.
- **Observed:** hub.json/role.json validators reject `_comment` field with `Config error: hub: unknown config key '_comment'`.
- **Reality:** Strict-mode rejection is the policy.  No JSON-comment dialect support.  Operators who add `_comment` keys (a common convention) will get config-load failures.
- **Resolution candidate:** doc-fix — README_Deployment.md should mention this strict-key policy explicitly.  Existing demo .json files (now stale) used `_comment` and would fail today.
- **Affects:** any operator copy-pasting from the existing demo files.

### G18 — `_unused` placeholder for empty flexzone was the wrong workaround (FIXED IN CODE)
- **Source:** Initial demo design.
- **Observed:** Producer crashes immediately on startup with `thread 'worker' body threw: fields must not be empty`.
- **Reality:** `RoleAPIBase::build_tx_queue` (role_api_base.cpp:258 pre-fix) unconditionally called `schema_spec_to_zmq_fields(opts.fz_spec)` on the SHM writer path.  When `out_flexzone_schema` was null (the `--init` template default), `fz_spec.fields` was empty, and the call threw.
- **Resolution:** code-fix applied this session.  Gate the conversion: empty fields → pass empty `SchemaFieldDesc` vector to `ShmQueue::create_writer`.  Working in producer + processor SHM writer paths.

### G19 — worker_main_ phase ordering bug — `set_channel` called AFTER `setup_infrastructure_` (FIXED IN CODE)
- **Source:** Surfaced after G18 fix unblocked the producer.
- **Observed:** Producer now crashes with `shm_create failed for ''. Error: 22` (EINVAL on empty SHM name).
- **Reality:** All three role hosts (producer / consumer / processor) had `setup_infrastructure_` (Step 2) running BEFORE `api_ref.set_channel(...)` (Step 3).  `setup_infrastructure_` calls `build_tx_queue` → `ShmQueue::create_writer(tx_channel, ...)` → `DataBlock(tx_channel, ...)` → `shm_create(tx_channel, ...)`.  `tx_channel = pImpl->out_channel.empty() ? pImpl->channel : pImpl->out_channel` reads pImpl state that hadn't been set yet.
- **Resolution:** code-fix applied this session in all three role hosts.  Step 2 split into Step 2a (api state wiring, unconditional) + Step 2b (setup_infrastructure_, gated on !validate_only, contains set_inbox_queue which DEPENDS on infrastructure being up).
- **Architectural note:** the duplication of `worker_main_` across three role hosts is exactly what Task #72 (Wave-B M9 `RoleHostFrame<HostT>` CRTP template) is meant to fix.  When M9 lands, the lifted template should inherit this corrected phase ordering.
- **Test gap discovered:** the L3 `RoleAPIBase_StartHandlerThreads_DualHub_E2E` test exercises `build_tx_queue` directly via the API and bypasses `worker_main_` — so it didn't catch this.  L4 binary tests only cover `--init` / `--keygen` / `--validate`, not the data pipeline.  A binary-level pipeline test (Task #44) would catch this entire class of bug.

### G20 — `--init` SHM secret defaults to 0, causing processor input to silently fall through to ZMQ
- **Source:** Surfaced after G19 fix.
- **Observed:** Processor's worker fails with `[proc] Failed to connect consumer to in_channel 'lab.demo.counter'` (`build_rx_queue` returns false).
- **Reality:** `plh_role --init --role processor` template emits `in_shm_enabled: true` but no `in_shm_secret`.  Default value is 0.  `processor_role_host.cpp:452` reads `in_opts.shm_shared_secret = config_.in_shm().enabled ? config_.in_shm().secret : 0u;`.  `RoleAPIBase::build_rx_queue` then gates on `if (!opts.shm_name.empty() && opts.shm_shared_secret != 0 && opts.slot_spec.has_schema)` — with secret=0, the SHM path is SKIPPED and the consumer attempts ZMQ.  But the producer is publishing SHM-only, so the ZMQ attempt also fails → `Failed to connect consumer to in_channel`.
- **Resolution candidate:** demo config-fix (set matching `out_shm_secret` / `in_shm_secret`).  OR code-fix: the `--init` template should generate a sensible default secret (e.g. a random non-zero uint64).
- **Affects:** Any demo / deployment that uses `--init` templates without manually adding secrets to the SHM blocks.

### G10 — Hub.json schema fundamentally reorganised vs doc §4.2
- **Source:** `README_Deployment.md` §4.2 (lines 197-244).
- **Observed:** Doc shows the canonical hub.json as:
  ```
  hub.{name,uid,description,broker_endpoint,admin_endpoint,connection_policy}
  broker.{heartbeat_interval_ms, ready_miss_heartbeats, pending_miss_heartbeats, consumer_liveness_check_s}
  script.{type, path, tick_interval_ms, health_log_interval_ms}
  peers
  ```
- **Reality:** `plh_hub --init` emits a fundamentally different schema:
  ```
  admin.{enabled, endpoint, token_required}
  broker.{heartbeat_interval_ms, pending_miss_heartbeats, ready_miss_heartbeats}      [no consumer_liveness_check_s]
  federation.{enabled, forward_timeout_ms, peers}                                       [replaces top-level peers]
  hub.{auth.keyfile, log_level, name, uid}                                              [no description, no endpoints, no connection_policy]
  logging.{backups, file_path, max_size_mb, timestamped}                                [new block]
  loop_timing                                                                            [new top-level — was script-only in doc]
  network.{broker_bind, broker_endpoint, zmq_io_threads}                                 [endpoint moved here from hub.*]
  python_venv                                                                            [new top-level]
  script.{path, type}                                                                    [no tick_interval_ms, no health_log_interval_ms]
  state.{disconnected_grace_ms, max_disconnected_entries}                                [new block]
  stop_on_script_error                                                                   [new top-level]
  target_period_ms                                                                       [new top-level]
  ```
- **Resolution candidate:** doc-fix (massive rewrite of §4.2).  Field-by-field reconciliation needed.  Code IS the truth here per Wave-B + Arc-A renovation — doc never caught up.
- **Affects:** every demo's hub config; anyone deploying from the doc would write a non-functional hub.json.
- **Severity:** HIGH — this is the central reference for hub deployment.

### G11 — UID format changed: dotted-lowercase, not UPPER-HEX-DASH
- **Source:** `README_Deployment.md` §4.2 line 201 and §5/§6/§7 field tables (e.g. line 531: `producer.name — used in UID (PROD-{NAME}-{HEX})`).
- **Observed:** Doc says UIDs are `HUB-MAIN-3A7F2B1C`, `PROD-MYSENSOR-A1B2C3D4`, `CONS-{NAME}-{HEX}`, `PROC-{NAME}-{HEX}` — all uppercase with dashes.
- **Reality:** `plh_hub --init` produces `hub.audithub.uid33c1cda4`; `plh_role --init --role producer --name AuditProd` produces `prod.auditprod.uid25ee42f2`.  Lowercase dotted, `uid` literal prefix on the suffix.  Matches HEP-CORE-0033 §G2.2.0b grammar (`<tag>.<name>.<unique-suffix>`).
- **Resolution candidate:** doc-fix.  Update every UID example + the field-table descriptions.
- **Affects:** documentation accuracy; demo authors who copy UID format from doc.

### G12 — Producer config fields renamed `*` → `out_*`
- **Source:** `README_Deployment.md` §5.1 (lines 482-563).
- **Observed:** Doc shows producer.json fields: `hub_dir`, `channel`, `transport`, `shm.{enabled,slot_count,secret}`, `slot_schema`, `flexzone_schema`, `zmq_out_endpoint`, `zmq_buffer_depth`.
- **Reality:** `plh_role --init --role producer` emits: `out_hub_dir`, `out_channel`, `out_transport`, `out_shm_enabled`, `out_shm_slot_count`, `out_slot_schema`, `out_flexzone_schema`.  All fields gained `out_*` prefix.  No nested `shm` object — flattened to `out_shm_<field>`.
- **Resolution candidate:** doc-fix (rewrite §5.1).  Code is the truth (Wave-B M-series renovation).
- **Affects:** producer config in every demo; any deployment.
- **Severity:** HIGH.

### G13 — Consumer config fields renamed `*` → `in_*`
- **Source:** `README_Deployment.md` §6.1 (lines 593-664).
- **Observed:** Doc shows: `hub_dir`, `channel`, `queue_type`, `slot_schema`, `shm.{enabled,secret}`.
- **Reality:** `plh_role --init --role consumer` emits: `in_hub_dir`, `in_channel`, `in_transport` (no `queue_type`!), `in_shm_enabled`.  Flat namespace, no nested `shm`.
- **Resolution candidate:** doc-fix (rewrite §6.1).
- **Affects:** consumer config in every demo.
- **Severity:** HIGH.

### G14 — Processor `shm.in` / `shm.out` nested → flat `in_shm_*` / `out_shm_*`
- **Source:** `README_Deployment.md` §7.1 (lines 690-789).
- **Observed:** Doc shows `shm: {in: {enabled, secret}, out: {enabled, slot_count, secret}}` nested form.
- **Reality:** `plh_role --init --role processor` flattens to `in_shm_enabled`, `out_shm_enabled`, `out_shm_slot_count`.
- **Resolution candidate:** doc-fix (rewrite §7.1).
- **Affects:** processor config.

### G15 — Producer `--init` template uses `target_period_ms: 100` and `loop_timing: "fixed_rate"` as defaults
- **Observed:** Doc says `loop_timing` is required.  `--init` provides a sensible default.
- **Reality:** Both are present in template; matches doc requirement but not its example for max_rate cases.
- **Resolution candidate:** none — this is fine.

### G16 — Consumer `queue_type` field documented but does not exist
- **Source:** `README_Deployment.md` §6.1 line 643: `queue_type — "shm" or "zmq"`.
- **Reality:** Consumer template emits `in_transport`, not `queue_type`.  Likely the field rename from `queue_type` → `in_transport` happened as part of the consumer-side `*` → `in_*` rename (G13).
- **Resolution candidate:** doc-fix.
- **Affects:** any consumer config copied from doc will fail to parse.

---

## Summary as of doc audit checkpoint (2026-05-20)

The deployment doc (`README_Deployment.md`, dated 2026-03-15) is comprehensively stale relative to current binaries:

- **EVERY field name pattern in producer / consumer / processor** has changed via the `*` → `in_*` / `out_*` rename (G12 / G13 / G14).
- **Hub.json** has been fundamentally reorganised (G10) — top-level keys moved, nested blocks added.
- **UID format changed** (G11).
- **`--dev` flag removed** (G9) — demo running has a friction issue.
- **Script API claims** (`api.shm_blocks`, `api.notify_channel`, `api.broadcast_channel`, peer-messaging methods) are partially-or-fully obsolete (G5/G6/G7).

**Conclusion:** Designing the demo configs from doc-only knowledge is not viable — the doc would produce a non-parsing config.  The pragmatic path is to use `plh_hub --init` and `plh_role --init` templates as the truth, run the demo, then drain this gap log into a focused doc-update task (likely an expansion of Task #73 — HEP-0033 Phase 10 doc closure).

**Strategic question for user (open):** continue Phase 2 of demo work using `--init` templates as truth, OR block on doc reconciliation first.

---

## Reference doc state at session start

- `docs/README/README_Deployment.md` — 1386 lines, dated 2026-03-15.
  **Two months stale** as of this audit; covers single-hub deployment
  in detail (§4-§7) plus a §10 "Multi-Hub Pipelines" section that
  predates the Wave-B M0..M8 per-presence FSM refactor.
- `docs/README/README_DirectoryLayout.md` — 470 lines, architecture-level
  reference for hub + role directory shape.
- `docs/README/README_GettingStarted.md` — 283 lines.

## Code state at session start

- `plh_hub` and `plh_role` binaries staged at
  `build/stage-debug/bin/`.
- Both accept `--init`, `--validate`, `--keygen`, `--config`, or
  `<directory>` positional-arg run mode (verified via `--help`).
- `plh_role` requires `--role <tag>` in run mode (verified via help).
- broker_proto 5 (R3.5b, 2026-05-19) — `consumer_uid`/`uid`/`sender_uid`
  unified to `role_uid` on the wire; grammar validation +
  side-aware tag policy at every gate (HEP-CORE-0033 §G2.2.0b).
- Per-presence registration FSM on `Presence` rows (S1+O4 closed
  2026-05-17); dual-hub processor has 2 presences.
- Band authority + typed callbacks (S4 expanded, shipped) — broker
  is authoritative on band membership; role-side bookkeeping
  mirrors on success+error.

## Design plan

1. **Single-hub demo** (`py-demo-single-processor-shm/`) — 4
   processes (hub + producer + processor + consumer).
   - Read §4.1, §4.2, §5.1, §5.2, §6.1, §6.2, §7.1, §7.2, §8 of the
     deployment doc.
   - Produce hub.json, producer.json, processor.json, consumer.json
     + Python scripts from doc-derived knowledge only.
   - Write the manifest.
   - Run the harness; iterate.
2. **Dual-hub demo** (`py-demo-dual-processor-bridge/`) — 6
   processes; same doc-driven approach plus §10 Multi-Hub.
3. **Doc updates / code fixes** — drained from this log at the end.
