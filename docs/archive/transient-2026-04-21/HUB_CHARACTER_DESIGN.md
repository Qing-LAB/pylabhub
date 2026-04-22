# Hub Character — Design

**Status**: ✅ Promoted to HEP-CORE-0033 (2026-04-21). This document is the
working draft used to shape HEP-CORE-0033; the HEP is now the normative spec.
Kept for historical traceability and for elaboration not repeated in the HEP.
**Normative spec**: `docs/HEP/HEP-CORE-0033-Hub-Character.md`.
**Created**: 2026-04-21.
**Author**: Design discussion between user and Claude Code.
**Depends on**: `pylabhub-utils` shared library, `ScriptEngine` abstraction, role-unification work (HEP-CORE-0024 Phases 1-22).
**Supersedes**: HEP-CORE-0019 §3-4 (Metrics Plane broker-pull model — see §7).
**Does NOT modify**: role-side code, role→broker wire protocol, SHM layout.

---

## 0. Design premises (ratified Q1–Q5)

1. **`pylabhub-utils` is the primary product.** The hub binary is a thin consumer of library code. Everything that can live in the shared lib does.
2. **Hub is a single-kind, fully functional automated binary.** No `HubHostBase`/`HubRegistry`; no variant binaries. Extension comes from **config** and **script**, never from new binary kinds.
3. **Admin interface is structured RPC** over ZMQ REP + the same surface bound into pybind11 for the hub script. Python `exec()` backdoor is gated by `--dev` / debug token, never the primary path.
4. **Script is a pure customization layer, optional.** Every callback has a sensible C++ default (no-op for events, accept for vetoes). Absent script = fully functional hub with default policies.
5. **Scripting uses the same `ScriptEngine` abstraction as roles** — `PythonEngine` and `LuaEngine` are both supported. The bespoke `PythonInterpreter` stack is retired.
6. **Metrics are query-driven.** The hub maintains a global table whose entries either hold data in-position (broker-internal counters, role-pushed metrics) or point to an on-demand collector (local SHM blocks). Queries walk the table, collect per filter, format as JSON, return. Broker never initiates network requests to roles for metrics.
7. **Hub admin queries never depend on role availability.** Query reads what's present in the table (pushed by roles via heartbeat piggyback or `METRICS_REPORT_REQ`) with timestamps; freshness is self-describing.

---

## 1. The 7 hub functions (user-defined decomposition)

The hub binary exists to perform these seven functions; everything in this document is subservient to them.

| # | Function | Responsible component |
|---|---|---|
| 1 | Parse arguments | `hub_cli::parse_hub_args` (new, mirrors `role_cli::parse_role_args`) |
| 2 | Config | `HubConfig` (extended, mirrors `RoleConfig` composition) |
| 3 | Set up environment | `scripting::hub_lifecycle_modules()` helper + `LifecycleGuard` |
| 4 | Manage communication | `BrokerService` (existing) + `Messenger` (existing) |
| 5 | Maintain tables | `HubState` (new — aggregates `ChannelRegistry`, `BandRegistry`, role state, peer table, SHM block index) with public read accessors |
| 6 | Maintain metrics | Global table (part of `HubState`) + query engine; heartbeat piggyback + `METRICS_REPORT_REQ` are ingress |
| 7 | Answer requests | Role-facing: `BrokerService` protocol (unchanged). Admin-facing: new `AdminService` structured RPC. Script-facing: `HubAPI` bound into `ScriptEngine`. |

---

## 2. `HubHost` class — shape and lifecycle

```cpp
namespace pylabhub::hub_host {

class PYLABHUB_UTILS_EXPORT HubHost
{
public:
    explicit HubHost(config::HubConfig cfg,
                     std::unique_ptr<scripting::ScriptEngine> engine, // optional
                     std::atomic<bool> *shutdown_flag);
    ~HubHost();

    // Non-copyable, movable.
    HubHost(HubHost &&) noexcept;
    HubHost &operator=(HubHost &&) noexcept;

    // Lifecycle — called from plh_hub_main.cpp.
    void startup_();          // wires enabled subsystems per config
    void run_main_loop();     // blocks until shutdown_flag set
    void shutdown_();

    // State accessors (thread-safe, internal mutex). Used by AdminService
    // and HubAPI. Return snapshots, not live views.
    [[nodiscard]] HubStateSnapshot     state_snapshot() const;
    [[nodiscard]] nlohmann::json       query_metrics(const MetricsFilter &f) const;
    [[nodiscard]] ChannelInfo          get_channel(std::string_view name) const;
    [[nodiscard]] std::vector<RoleInfo> list_roles(const RoleFilter &f = {}) const;
    // ... etc per Admin RPC surface (§8)

    // Control operations (thread-safe).
    Result<void, Error> close_channel(std::string_view name);
    Result<void, Error> broadcast_channel(std::string_view name,
                                          const nlohmann::json &msg);
    // ... etc

    // Config accessor.
    const config::HubConfig &config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::hub_host
```

**Lifecycle ordering** (driven by config toggles):

```
Logger
  → CryptoUtils
  → FileLock
  → HubConfig           (lifecycle module — exposes the loaded HubConfig singleton
                          for modules that need early access; cf. RoleHostCore pattern)
  → ZMQContext
  → HubVault            (if auth enabled)
  → BrokerService       (always — the core service)
  → FederationManager   (if federation_enabled)
  → AdminService        (if admin_enabled — replaces AdminShell)
  → HubScriptRunner     (if script.path is set AND script loads successfully)
  → SignalHandler
```

Subsystems disabled in config are not constructed. `HubHost::run_main_loop()` blocks on the shutdown flag and performs periodic bookkeeping (state-table eviction of disconnected roles after grace period, etc.). Actual I/O runs on the subsystem threads (broker loop thread, admin REP thread, script engine thread).

---

## 3. CLI (`plh_hub`)

Mirrors `plh_role` CLI in shape and naming conventions.

```
Usage:
  plh_hub --init [<hub_dir>]            # Create hub directory + template config
  plh_hub <hub_dir>                     # Run from directory
  plh_hub --config <path.json> [--validate | --keygen]
  plh_hub --dev                         # Dev/test mode, no hub_dir, ephemeral keypair
  plh_hub --help | -h

Modes (at most one; default is run):
  --init [dir]       Create hub directory with config template; exit 0
  --validate         Validate hub.json + (optional) script; exit 0 on success
  --keygen           Generate broker CURVE keypair + hub vault; exit 0
  --dev              Development: built-in defaults, ephemeral keys, no vault

Common options:
  <hub_dir>          Hub directory containing hub.json
  --config <path>    Explicit path to hub.json
  --name <name>      Hub name for --init (skips interactive prompt)

Init-only options (write into generated logging config):
  --log-maxsize <MB> Rotate when a log file reaches this size (default 10)
  --log-backups <N>  Keep N rotated files (default 5; -1 = keep all)
```

**Parser contract** (per `role_cli::parse_role_args` pattern):
- Returns `ParseResult { HubArgs args; int exit_code = -1; }`.
- Never calls `std::exit`.
- `--help`/`-h` prints usage to `out_stream` (stdout by default) and returns `exit_code = 0`.
- Parse errors print to `err_stream` and return `exit_code = 1`.
- Mode-exclusion and init-only-flag post-loop guards, same as role CLI.

**Files**: `src/include/utils/hub_cli.hpp` (header-only, inline — mirrors `role_cli.hpp`).

---

## 4. Config — `hub.json`

Mirrors the role-config composite pattern: one `HubConfig` class composed of categorical sub-configs, each in its own header, each with a strict key whitelist enforced at parse time.

### 4.1 Composite shape

```cpp
namespace pylabhub::config {

class PYLABHUB_UTILS_EXPORT HubConfig
{
public:
    // ── Factory methods (parallel to RoleConfig) ──────────────────────
    static HubConfig load(const std::string &path);
    static HubConfig load_from_directory(const std::string &dir);
    // No role_parser — hub is single-kind.

    // ── Non-directional accessors ─────────────────────────────────────
    // (No directional in_/out_ — hub doesn't have asymmetric sides like roles.)
    const HubIdentityConfig  &identity()   const;
    const AuthConfig         &auth()       const;  // reused from roles
    const ScriptConfig       &script()     const;  // reused from roles (optional)
    const LoggingConfig      &logging()    const;  // reused from roles
    const HubNetworkConfig   &network()    const;  // new
    const HubAdminConfig     &admin()      const;  // new
    const HubBrokerConfig    &broker()     const;  // new — limits, policies
    const HubFederationConfig&federation() const;  // new
    const HubStateConfig     &state()      const;  // new — table retention, grace

    // ── Vault operations (parallel to RoleConfig) ────────────────────
    bool        load_keypair(const std::string &password);
    std::string create_keypair(const std::string &password);

    // ── Raw JSON + metadata ──────────────────────────────────────────
    const nlohmann::json            &raw() const;
    bool                             reload_if_changed();
    const std::filesystem::path     &base_dir() const;

    ~HubConfig();
    HubConfig(HubConfig &&) noexcept;
    HubConfig &operator=(HubConfig &&) noexcept;

private:
    HubConfig();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::config
```

Same pImpl + JsonConfig backend as `RoleConfig` for ABI stability + thread-safe file I/O + `reload_if_changed()`.

### 4.2 `hub.json` schema

```jsonc
{
  // ── Identity (analogous to role's "producer"/"consumer"/"processor" block) ──
  "hub": {
    "uid":       "HUB-MAIN-12345678",     // optional; auto-generated if absent
                                          //   (prefix HUB-, with "HUB-" warning if present but wrong format)
    "name":      "MainHub",               // required (interactive prompt in --init)
    "log_level": "info",                  // "debug" | "info" | "warn" | "error"
    "auth": {
      "keyfile": "vault/hub.vault"        // path (absolute or <hub_dir>-relative);
                                          //   empty = no CURVE auth, broker runs in plain mode
    }
  },

  // ── Script (optional — reused from roles) ──────────────────────────
  "script": {
    "type":     "python",                 // "python" | "lua" | "native"
    "path":     ".",                      // parent of script/<type>/ package
    "checksum": "..."                     // BLAKE2b-256 hex of native .so (native only)
  },
  "python_venv":          "",             // venv name (Python only); empty = base
  "stop_on_script_error": false,          // fatal on script exception

  // ── Logging (reused from roles) ────────────────────────────────────
  "logging": {
    "file_path":    "",                   // empty → <hub_dir>/logs/<uid>.log
    "max_size_mb":  10,
    "backups":      5,                    // -1 = keep all
    "timestamped":  true
  },

  // ── Network (new) ──────────────────────────────────────────────────
  "network": {
    "broker_endpoint": "tcp://0.0.0.0:5570",   // ROUTER for roles
    "broker_bind":     true,                    // true = bind, false = connect (future)
    "zmq_io_threads":  1                        // ZMQ context io_threads
  },

  // ── Admin service (new) ────────────────────────────────────────────
  "admin": {
    "enabled":          true,
    "endpoint":         "tcp://127.0.0.1:5600", // ZMQ REP for structured RPC
    "dev_mode":         false,                   // exposes exec_python RPC when true
    "token_required":   true                     // if true, vault must provide admin_token;
                                                 //   if false, local-only connections are trusted
  },

  // ── Broker runtime (new; pulled out of BrokerService::Config) ──────
  "broker": {
    "heartbeat_timeout_ms":    15000,           // role missing heartbeats → dead
    "heartbeat_multiplier":    5,               // HEP-CORE-0023 Phase 2
    "default_channel_policy":  "open",          // "open" | "tracked" | "required" | "verified"
    "known_roles": [                            // for "required"/"verified" policies
      { "uid": "PROD-SRC-...", "name": "Source", "pubkey": "z85..." }
    ]
  },

  // ── Federation (new) ───────────────────────────────────────────────
  "federation": {
    "enabled":  false,
    "peers": [
      { "uid": "HUB-PEER-...", "endpoint": "tcp://other.lab:5570",
        "pubkey": "z85..." }
    ],
    "forward_timeout_ms": 2000
  },

  // ── State table retention (new) ────────────────────────────────────
  "state": {
    "disconnected_grace_ms": 60000,   // keep last metrics for 60s after role exits
    "max_disconnected_entries": 1000  // LRU cap on retained-after-disconnect entries
  }
}
```

### 4.3 Parsing + validation rules (copied verbatim from role pattern)

- **Strict key whitelist.** Unknown top-level keys and unknown keys inside every sub-object throw `std::runtime_error("<hub>: unknown config key '<name>'")` at parse time.
- **Sub-object type enforcement.** If `"logging"` is present, it must be a JSON object, else throw.
- **Sentinel values documented in code.** `kKeepAllBackups = numeric_limits<size_t>::max()` with JSON `"backups": -1`. `disconnected_grace_ms: -1` for infinite retention.
- **Empty sections take defaults.** Absent `"federation"` means `federation.enabled: false`; absent `"script"` means no script (hub runs without one).
- **UID auto-generation with warning.** If `hub.uid` is absent, generate `HUB-<NAME>-<hex8>` and print a diagnostic with the generated UID so the user can paste it back in for stability.
- **Type coercion errors surface `Config error: ...`** as plain-text stderr, consistent with `plh_role`.

### 4.4 New categorical config headers

Under `src/include/utils/config/`:
- `hub_identity_config.hpp` — `HubIdentityConfig { uid, name, log_level }` (parallel to `IdentityConfig`, different prefix).
- `hub_network_config.hpp` — `broker_endpoint`, `broker_bind`, `zmq_io_threads`.
- `hub_admin_config.hpp` — `enabled`, `endpoint`, `dev_mode`, `token_required`.
- `hub_broker_config.hpp` — `heartbeat_timeout_ms`, `heartbeat_multiplier`, `default_channel_policy`, `known_roles[]`.
- `hub_federation_config.hpp` — `enabled`, `peers[]`, `forward_timeout_ms`.
- `hub_state_config.hpp` — `disconnected_grace_ms`, `max_disconnected_entries`.

Reused from role headers:
- `auth_config.hpp`, `script_config.hpp`, `logging_config.hpp`.

The existing `src/include/utils/config/hub_config.hpp` (role-facing hub-reference for `in_hub_dir`/`out_hub_dir`) is **renamed** to `hub_ref_config.hpp` to avoid the name collision with this new hub-side `HubConfig`.

### 4.5 Vault + keygen

Same three-mode semantics as role-side:
- **`--keygen`**: generates a broker CURVE25519 keypair, writes the secret half to `hub.auth.keyfile` encrypted with `PYLABHUB_HUB_PASSWORD` (or interactive prompt), prints the public key (for roles to pin via `in_hub_pubkey` / `out_hub_pubkey`) and the `role_uid` (or `hub_uid`).
- **Run-mode without `--dev`**: reads the vault, unlocks with the same password source chain (env var → TTY prompt), loads the broker's stable CURVE keypair. If `admin.token_required: true`, the vault also carries the admin token (same KDF domain, different slot).
- **`--dev`**: generates an ephemeral keypair at start, skips vault entirely, skips admin token; only bind-to-`127.0.0.1` + local admin allowed.

---

## 5. Hub directory layout (`--init` output)

Mirrors the role directory layout (`docs/HEP/HEP-CORE-0024`):

```
<hub_dir>/
├── hub.json                    # config (§4)
├── script/                     # OPTIONAL — absent = no scripting, hub still works
│   └── python/                 #   (or lua/ or native/)
│       ├── __init__.py
│       └── callbacks.py        # on_start / on_tick / on_role_registered / ...
├── vault/
│   └── hub.vault               # encrypted CURVE keypair + (optional) admin token
├── logs/
│   └── <hub_uid>-<ts>.log      # timestamped rotating logs
└── run/                        # runtime state (PID files, etc.)
```

Path resolution uses the same `RoleDirectory` helper pattern — likely `HubDirectory` (new) with the same `base_dir/vault/logs/script/run` accessors and `create_standard_layout()` / `has_standard_layout()`.

---

## 6. State tables — what the hub tracks (`HubState`)

`HubState` is a read-mostly in-memory aggregate owned by `HubHost`. `BrokerService` updates it as messages arrive; `AdminService` and `HubAPI` read from it via the public `HubHost` accessors.

### 6.1 Table schema

```cpp
struct HubState
{
    // Channels (from ChannelRegistry; §2 of existing broker code).
    std::unordered_map<std::string, ChannelEntry>  channels;

    // Roles (aggregated across channels, keyed by uid).
    std::unordered_map<std::string, RoleEntry>     roles;

    // Bands (HEP-CORE-0030).
    std::unordered_map<std::string, BandEntry>     bands;

    // Federation peers.
    std::unordered_map<std::string, PeerEntry>     peers;

    // SHM block index — "pointer-to-collect" entries; data is fetched on
    // demand via hub::DataBlock::get_metrics(channel) at query time.
    std::unordered_map<std::string, ShmBlockRef>   shm_blocks;

    // Broker-internal counters (in-position data).
    BrokerCounters                                  counters;
};
```

### 6.2 Entry kinds

| Entry | Updated by | Holds |
|---|---|---|
| `ChannelEntry` | REG_REQ / DISC_REQ / CHANNEL_CLOSING / broker internal | name, schema, producer PID, consumers list, created_at, status, last-seen timestamps |
| `RoleEntry` | REG_REQ / HEARTBEAT_REQ / METRICS_REPORT_REQ / DISC_REQ / timeout | uid, name, role_tag, channel(s), state (Pending/Ready/Closing/Disconnected), first-seen, last-heartbeat, latest `metrics` blob, `metrics_collected_at` timestamp, pubkey |
| `BandEntry` | BAND_JOIN / BAND_LEAVE | name, members[], last-activity |
| `PeerEntry` | HUB_PEER_HELLO / HUB_PEER_BYE / federation heartbeat | uid, endpoint, state, last-seen |
| `ShmBlockRef` | channel registration with SHM transport | channel name, block path; metrics collected via `collect_shm_info(channel)` at query time (reference, not cache) |
| `BrokerCounters` | broker internal (lock-free atomics where possible) | role state transitions (HEP-CORE-0023 `RoleStateMetrics`), ctrl queue depth, bytes in/out, per-msg-type counts |

### 6.3 Retention

- **Active roles/channels/bands/peers**: kept until explicit close / timeout / deregister.
- **Disconnected roles**: entry lingers for `state.disconnected_grace_ms` (default 60s) with `status: "disconnected"` and last known metrics + `disconnected_at` timestamp, then evicted. LRU cap `state.max_disconnected_entries` prevents unbounded growth.
- **Closed channels**: evicted immediately from `channels`; closing-history can be exposed via a bounded ring buffer in `BrokerCounters` if useful (not in v1).

### 6.4 Consistency

- Single internal mutex (`HubState::m_`) guarding the maps.
- Each accessor takes the lock, reads the requested subset, unlocks, returns a **snapshot struct** (not a reference into the live map).
- Query does not hold the lock across network I/O (SHM metrics collection drops the lock, reads block, reacquires lock if further processing needed — but v1 may just read SHM while holding the lock since `datablock_get_metrics` is fast in-process).

Per the ratified metrics premise: **no cross-field consistency is promised**; each metric entry has its own `collected_at` timestamp.

---

## 7. Metrics model (supersedes HEP-0019 §3-4)

### 7.1 Ingress — role→hub push only

Unchanged from current implementation (confirmed by code inspection):
- **Heartbeat piggyback**: role sends `HEARTBEAT_REQ` periodically (cadence `cfg.heartbeat_interval_ms`), *iteration-gated* (stops if role's loop stalls). If `metrics` field present, broker calls `update_producer_metrics(channel, metrics, pid)`.
- **Standalone report**: role sends `METRICS_REPORT_REQ` periodically (same cadence, *time-only* — fires even when role is idle/stuck), gated by `cfg.report_metrics`. Broker calls `update_consumer_metrics(channel, uid, metrics)`.

No broker-initiated metrics pull exists or will exist.

### 7.2 Global table entry types

- **In-position** (data lives directly in the entry):
  - Role-pushed `metrics` blob per channel per role (keyed `channels[ch].{producer, consumers[uid]}`).
  - `BrokerCounters` (broker-internal transitions and rates).
  - Federation peer states.
- **Pointer-to-collect** (entry holds a reference; collector invoked at query time):
  - `ShmBlockRef` → `hub::DataBlock::get_metrics(channel)` (in-process, microsecond-scale).
  - Future extension points (e.g., system-level metrics: CPU/RSS via OS call) use the same pattern.

### 7.3 Query flow (`HubHost::query_metrics(filter)`)

1. Accept a `MetricsFilter` — sets of role_uids, channel names, band names, peer uids, category tags (`"channel"`, `"role"`, `"band"`, `"peer"`, `"broker"`, `"shm"`, or `"all"`).
2. Walk `HubState`, selecting entries matching the filter.
3. For each entry: read in-position data directly; for pointer-to-collect entries, invoke the collector.
4. Build a single JSON response structure:
   ```jsonc
   {
     "status": "ok",
     "queried_at": "2026-04-21T20:00:00.000Z",
     "filter":    { ... },
     "channels":  { "ch_name": {"producer": {..., "_collected_at": "..."},
                                 "consumers": {"uid": {..., "_collected_at": "..."}},
                                 "shm":      {..., "_collected_at": "..."}} },
     "roles":     { "uid": { ..., "_collected_at": "...", "_status": "ready" } },
     "bands":     { ... },
     "peers":     { ... },
     "broker":    { ... }
   }
   ```
5. Return. No retry, no wait-for-update.

Every entry carries a `_collected_at` timestamp (ISO-8601); stale entries are visible at the per-entry level. No "query succeeded but data was old" ambiguity.

### 7.4 Relationship to existing `BrokerService::query_metrics_json_str(channel)`

The existing implementation (`broker_service.cpp:2336`, `2523`, `2534`) already implements the shape described here for `channels/shm_blocks`:
- `query_metrics()` reads from `metrics_store_` (in-position, role-pushed).
- `collect_shm_info(channel)` is called on-demand.

The refactor extends this to:
- Add role/band/peer/broker categories.
- Add `_collected_at` per entry.
- Expose via `HubHost::query_metrics(filter)` rather than `BrokerService` directly.
- Plumb through `AdminService` RPC and `HubAPI` script binding.

---

## 8. Admin RPC surface (`AdminService`)

Replaces the existing `AdminShell` that only offers `{token, code}` → `exec(code)`.

### 8.1 Transport

- ZMQ REP socket at `admin.endpoint` (default `tcp://127.0.0.1:5600`).
- Request envelope: `{ "method": "<name>", "token": "<admin_token>", "params": { ... } }`.
- Response envelope: `{ "status": "ok|error", "result": ..., "error": {"code": ..., "message": ...} }`.
- One method per operation; parameters typed per-method.

### 8.2 Method list (v1)

**Query methods** (read-only):
| Method | Params | Returns |
|---|---|---|
| `list_channels` | `{ "name_prefix": "..." (optional) }` | `{ "channels": [ChannelInfo] }` |
| `get_channel` | `{ "name": "..." }` | `ChannelInfo` |
| `list_roles` | `{ "status": "ready|disconnected|..." (optional) }` | `{ "roles": [RoleInfo] }` |
| `get_role` | `{ "uid": "..." }` | `RoleInfo` |
| `list_bands` | `{}` | `{ "bands": [BandInfo] }` |
| `list_peers` | `{}` | `{ "peers": [PeerInfo] }` |
| `query_metrics` | `MetricsFilter` | see §7.3 |
| `list_known_roles` | `{}` | `{ "known_roles": [KnownRole] }` |

**Control methods** (write):
| Method | Params | Effect |
|---|---|---|
| `close_channel` | `{ "name": "...", "reason": "..." }` | Broadcast CHANNEL_CLOSING_NOTIFY, remove |
| `broadcast_channel` | `{ "name": "...", "message": {...} }` | CHANNEL_BROADCAST_NOTIFY to members |
| `revoke_role` | `{ "uid": "...", "reason": "..." }` | FORCE_SHUTDOWN to role |
| `add_known_role` | `KnownRole` | Update allowlist (persists to `hub.json`? — see §12) |
| `remove_known_role` | `{ "uid": "..." }` | Remove from allowlist |
| `reload_config` | `{}` | `HubConfig::reload_if_changed()` + apply runtime-tunables |
| `request_shutdown` | `{ "graceful": true }` | Set shutdown flag |

**Dev-only methods** (gated by `admin.dev_mode: true`):
| Method | Params | Effect |
|---|---|---|
| `exec_python` | `{ "code": "..." }` | Runs in the hub script engine's namespace (via `ScriptEngine::eval`); returns captured stdout + result |

### 8.3 Authorization

- If `admin.token_required: true`: request `"token"` must equal the admin token stored in the vault. Mismatch → `{"status": "error", "error": {"code": "unauthorized"}}`.
- If `admin.token_required: false`: token field ignored; endpoint must be local (bind to `127.0.0.1` enforced).
- `exec_python` always requires token when gated, plus `dev_mode: true` flag.

### 8.4 Files

- `src/include/utils/admin_service.hpp` — public `AdminService` class + method schemas.
- `src/utils/service/admin_service.cpp` — impl.
- Method handler dispatch lives in `AdminService`; each method is a C++ function that reads/writes `HubHost` via its public accessors.

---

## 9. Script callbacks + `HubAPI`

### 9.1 Engine integration

`HubHost` owns a `std::unique_ptr<scripting::ScriptEngine>` (null if no script configured). The engine runs on its own thread via the same cross-thread dispatch pattern as roles (`ThreadEngineGuard`, request queue). Engine factory is `scripting::make_engine_from_script_config(cfg.script())` — the exact call role hosts use.

### 9.2 Callback surface (all default to no-op / accept)

**Lifecycle**:
- `on_start(api)` — called after startup, before the main loop.
- `on_tick(api)` — periodic (cadence TBD — default ~1 Hz, configurable via `script.tick_interval_ms` in a future extension).
- `on_stop(api)` — called during shutdown, before tear-down.

**Role events** (push-notified):
- `on_role_registered(api, role_info)`
- `on_role_closed(api, uid, reason)`

**Channel events**:
- `on_channel_opened(api, channel_info)`
- `on_channel_closed(api, name)`

**Band events** (HEP-CORE-0030):
- `on_band_joined(api, band, member_uid)`
- `on_band_left(api, band, member_uid)`

**Federation events**:
- `on_peer_connected(api, peer_uid)`
- `on_peer_disconnected(api, peer_uid, reason)`

**Veto hooks** (synchronous; return `bool`; default accept):
- `on_channel_close_request(api, channel) → bool` — return `false` to refuse admin-initiated close.
- `on_role_register_request(api, info) → bool` — return `false` to reject registration.

Every callback is **optional**; if absent in the script, the C++ default runs. Script exceptions are caught; `stop_on_script_error: true` promotes them to fatal (same pattern as roles).

### 9.3 `HubAPI` surface (bound into both Python and Lua)

All methods resolve via `HubHost` accessors; no direct `BrokerService` touch from scripts.

```
# Read
api.list_channels() -> [ChannelInfo]
api.get_channel(name) -> ChannelInfo
api.list_roles(status="*") -> [RoleInfo]
api.get_role(uid) -> RoleInfo
api.list_bands() -> [BandInfo]
api.list_peers() -> [PeerInfo]
api.query_metrics(filter) -> dict

# Control (subject to same security as admin RPC)
api.close_channel(name, reason="")
api.broadcast_channel(name, message)
api.revoke_role(uid, reason="")
api.add_known_role(uid, name, pubkey)
api.remove_known_role(uid)
api.request_shutdown(graceful=True)

# Introspection
api.config() -> dict      # snapshot of hub.json as loaded
api.uid -> str
api.name -> str
```

Pybind11 bindings live in `src/scripting/hub/hub_api.cpp` (and Lua bindings wherever `LuaEngine` places them today).

---

## 10. Protocol — additions / extensions / non-changes

### 10.1 No role-side changes

- Role→broker protocol (REG_REQ, DISC_REQ, HEARTBEAT_REQ, METRICS_REPORT_REQ, notifies) is **unchanged**.
- Role-side headers, config, lifecycle — unchanged.

### 10.2 Broker-side: new message types for admin RPC

These are on the admin ZMQ REP socket, not the role ROUTER socket. Protocol spec in §8.

### 10.3 Broker-side: new internal interfaces

- `HubHost` public accessor API (state snapshots + control ops).
- `BrokerService` gains internal callbacks that push events into `HubHost` (which fans out to script event hooks). This replaces the ad-hoc `pylabhub_module` callback wiring.

### 10.4 Heartbeat multiplier (HEP-CORE-0023 Phase 2)

Already implemented; no change.

---

## 11. Cross-references — HEPs affected

| HEP | Title | Relationship |
|---|---|---|
| HEP-CORE-0001 | Hybrid Lifecycle Model | Used verbatim for module ordering. |
| HEP-CORE-0007 | DataHub Protocol & Policy | Used verbatim for channel-level semantics. |
| HEP-CORE-0019 | Metrics Plane | §3-4 **superseded** by query-driven model (§7). §3.2 (live SHM merge) retained. Requires HEP amendment. |
| HEP-CORE-0021 | ZMQ Endpoint Registry | Used verbatim. |
| HEP-CORE-0022 | Hub Federation Broadcast | Used verbatim; federation config pulled out into `HubFederationConfig`. |
| HEP-CORE-0023 | Startup Coordination (incl. Phase 2 heartbeat multiplier) | Used verbatim. |
| HEP-CORE-0024 | Role Directory Service | Parallels mirrored: hub gets `HubDirectory`, `hub_cli`, `--init`/`--validate`/`--keygen`, same directory layout. |
| HEP-CORE-0030 | Band Messaging | Band events surface via script callbacks; admin RPC includes `list_bands`. |

A new HEP (provisionally **HEP-CORE-0031 — Hub Character**) promotes this document once reviewed.

---

## 12. Open items to resolve during implementation

These are intentionally deferred — design does not commit yet.

1. **`add_known_role` / `remove_known_role` persistence.** Do these modify `hub.json` on disk (via `JsonConfig` write), or only the in-memory allowlist? User-visible behavior differs. Likely: in-memory only by default; add a separate `persist_known_role_changes: false` config option (when `true`, `JsonConfig::save` is called).
2. **`script.tick_interval_ms`** — add to `ScriptConfig` (roles don't need it) or to a new `HubScriptConfig` subconfig? Hub-only knob; leans toward the latter.
3. **Band events on the hub**: `BandRegistry` must emit `on_band_joined`/`on_band_left` callbacks. Current `BandRegistry` is internal to broker thread; needs a hook point.
4. **Graceful-vs-fast shutdown semantics for `request_shutdown`**: match `plh_role` signal conventions (SIGINT = graceful, 2nd SIGINT = fast-exit).
5. **Metrics snapshot time-precision**: ISO-8601 with microsecond precision, or nanosecond? Role-side uses `std::chrono::steady_clock` internally but reports wall-clock — need to align on one clock source for timestamps.
6. **`_collected_at` semantics for pointer-to-collect entries**: is it the wall-clock at collector invocation, or the steady-clock delta from some epoch? Wall-clock is more user-useful.
7. **Script engine thread lifetime relative to `BrokerService` thread**: ensure clean teardown order (engine stops *before* broker, so last-minute event pushes don't dereference a dead engine).
8. **Dev-mode admin token**: `--dev` mode skips vault → what's the admin token behavior? Options: (a) no token, local-only enforced; (b) ephemeral token generated at startup and printed to stderr for the operator to use. Leaning (a).

---

## 13. Out of scope for this design

- Hub side-by-side replication / HA. Single-instance hub only.
- Hub script hot-reload mid-run (stop + restart cycle only).
- `--kind` binary variants. (Q3 decision — enabled/disabled subsystems via config only.)
- Rewriting role-side to mirror any hub-side change.
- HEP-CORE-0019 Phase 2 periodic broker-pull (**explicitly replaced** by query-driven model; §7).
