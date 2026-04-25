# HEP-CORE-0033: Hub Character

| Property       | Value                                                                                    |
|----------------|------------------------------------------------------------------------------------------|
| **HEP**        | `HEP-CORE-0033`                                                                          |
| **Title**      | Hub Character — Unified Hub Binary, Structured Admin RPC, Query-Driven Metrics, Scripting Parity |
| **Status**     | Draft (design ratified 2026-04-21; implementation not started)                           |
| **Created**    | 2026-04-21                                                                               |
| **Area**       | `pylabhub-utils`, `pylabhub-scripting`, `plh_hub` binary (new)                           |
| **Depends on** | HEP-CORE-0001 (Lifecycle), HEP-CORE-0007 (DataHub Protocol), HEP-CORE-0021 (ZMQ Registry), HEP-CORE-0022 (Federation), HEP-CORE-0023 (Startup Coordination), HEP-CORE-0024 (Role Directory Service), HEP-CORE-0030 (Band Messaging) |
| **Amends**     | HEP-CORE-0019 §3-4 — the periodic broker-pull metrics model is replaced by a query-driven model. HEP-CORE-0019 §3.2 (live SHM-derived block merge) is retained. |
| **Reference**  | Full design draft: `docs/tech_draft/HUB_CHARACTER_DESIGN.md`                             |

---

## 1. Motivation

The hub binary (`pylabhub-hubshell`) was built before several key abstractions that
have since been established on the role side:

| Capability        | Role side (HEP-0024, modern) | Hub side (`pylabhub-hubshell`, legacy) |
|-------------------|------------------------------|----------------------------------------|
| CLI parsing       | `role_cli::parse_role_args` (no-exit, stream-directed) | inline `argv` walking in `main()` |
| Config            | Composite `RoleConfig` with typed sub-configs + strict key whitelist | monolithic `HubConfig` singleton; whitelist status unverified |
| Env setup         | `scripting::role_lifecycle_modules()` helper | inline `LifecycleGuard` wiring |
| Script engine     | `ScriptEngine` abstraction (Python + Lua + Native) | bespoke `PythonInterpreter` (CPython embed, Python-only) |
| Script API        | Modern pybind11 `ProducerAPI`/`ConsumerAPI`/`ProcessorAPI` | `HubScriptAPI` + `pylabhub_module` (pre-abstraction style) |
| Admin interface   | n/a | `AdminShell` ZMQ REP with "exec arbitrary Python" only |
| Metrics           | `ContextMetrics` hierarchical X-macros | scattered (`metrics_store_`, `RoleStateMetrics`, SHM live merge) |

The hub binary is additionally **currently disabled in the build** (`if(FALSE)` in
`src/CMakeLists.txt`) awaiting the `PythonEngine` migration. This HEP defines the
complete new design the hub binary must implement before re-enabling.

## 2. Design premises (ratified)

1. **`pylabhub-utils` is the primary product.** The hub binary is a thin consumer
   of library code.
2. **Hub is a single-kind automated binary, fully functional without any script.**
   No `HubHostBase`/`HubRegistry`. Extension comes from **config** and **script**,
   never from new binary kinds.
3. **Admin interface is structured RPC** (per-method, typed). Python `exec()` is a
   gated dev-mode backdoor, never the primary path.
4. **Script is a pure customization layer, optional.** Every callback has a
   sensible C++ default (no-op for events, accept for vetoes). Absent script =
   fully functional hub with default policies.
5. **Scripting uses the same `ScriptEngine` abstraction as roles.** `PythonEngine`
   and `LuaEngine` both supported. Bespoke `PythonInterpreter` retired.
6. **Metrics are query-driven.** A global table holds either in-position data
   (broker-internal counters, role-pushed metrics) or pointers to on-demand
   collectors (local SHM blocks). Queries walk the table, collect, format JSON,
   return. Broker never initiates network requests to roles for metrics.
7. **Admin queries never depend on role availability.** Query reads what is
   present; each entry carries a `_collected_at` timestamp; freshness is
   self-describing; no retries, no wait-for-update.

## 3. Hub functions (user-defined decomposition)

The hub binary performs seven functions. This HEP is organised around them.

| # | Function                   | Owner component                                           |
|---|----------------------------|-----------------------------------------------------------|
| 1 | Parse arguments            | `hub_cli::parse_hub_args` (mirrors `role_cli`)            |
| 2 | Config                     | `HubConfig` composite (mirrors `RoleConfig`)              |
| 3 | Set up environment         | `scripting::hub_lifecycle_modules()` + `LifecycleGuard`   |
| 4 | Manage communication       | `BrokerService` + `Messenger` (existing, unchanged)       |
| 5 | Maintain state tables      | `HubState` (new) with public read accessors on `HubHost`  |
| 6 | Maintain metrics           | Global table (part of `HubState`) + query engine          |
| 7 | Answer requests            | `AdminService` RPC + `HubAPI` script binding              |

## 4. `HubHost` class + lifecycle

Single class owning the hub's runtime state and exposing read/write access to
`AdminService` and `HubAPI`.

### 4.0 Component architecture

```mermaid
graph TB
    subgraph Binary["plh_hub binary"]
        Main[plh_hub_main.cpp]
        CLI[hub_cli::parse_hub_args]
        Main --> CLI
        Main --> Host[HubHost]
    end

    subgraph Subsystems["HubHost-owned subsystems"]
        Host --> Broker[BrokerService]
        Host --> Admin[AdminService]
        Host --> Script[HubScriptRunner<br/>optional]
        Host --> State[HubState]
    end

    subgraph External["External actors"]
        Roles[Roles<br/>plh_role instances]
        AdminClient[Admin client<br/>CLI / dashboard]
        Peers[Peer hubs<br/>federation]
    end

    Roles -->|REG / HEARTBEAT / METRICS_REPORT| Broker
    AdminClient -->|ZMQ REP<br/>structured RPC| Admin
    Peers -->|HUB_PEER_HELLO / BYE| Broker

    Broker -->|updates| State
    Admin -->|reads/controls| State
    Admin -.->|event push| Script
    Broker -.->|event push| Script
    Script -.->|vetoes + control ops| Admin
```



```cpp
namespace pylabhub::hub_host {

class PYLABHUB_UTILS_EXPORT HubHost
{
public:
    explicit HubHost(config::HubConfig cfg,
                     std::unique_ptr<scripting::ScriptEngine> engine,  // optional
                     std::atomic<bool> *shutdown_flag);
    ~HubHost();

    void startup_();
    void run_main_loop();
    void shutdown_();

    // Read accessors (snapshots; thread-safe via internal state mutex).
    HubStateSnapshot     state_snapshot() const;
    nlohmann::json       query_metrics(const MetricsFilter &f) const;
    ChannelInfo          get_channel(std::string_view name) const;
    std::vector<RoleInfo> list_roles(const RoleFilter &f = {}) const;

    // Control operations (thread-safe).
    Result<void, Error>  close_channel(std::string_view name);
    Result<void, Error>  broadcast_channel(std::string_view name,
                                           const nlohmann::json &msg);
    Result<void, Error>  revoke_role(std::string_view uid,
                                     std::string_view reason);

    const config::HubConfig &config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::hub_host
```

**Lifecycle module ordering** (driven by config toggles — disabled subsystems
are not constructed):

```mermaid
graph LR
    Logger --> CryptoUtils --> FileLock --> HubConfig --> ZMQContext
    ZMQContext --> HubVault
    ZMQContext --> BrokerService
    HubVault -.->|if auth| BrokerService
    BrokerService --> FederationManager
    BrokerService --> AdminService
    BrokerService --> HubScriptRunner
    FederationManager -.-> SignalHandler
    AdminService --> SignalHandler
    HubScriptRunner --> SignalHandler

    style HubVault stroke-dasharray: 5 5
    style FederationManager stroke-dasharray: 5 5
    style AdminService stroke-dasharray: 5 5
    style HubScriptRunner stroke-dasharray: 5 5
```

Dashed modules are config-gated; a minimal hub (admin off, federation off, no
script, no auth) runs with only the solid-edge nodes.

## 5. CLI (`plh_hub`)

Mirrors `plh_role` CLI shape and parser contract.

```
Usage:
  plh_hub --init [<hub_dir>]            # Create hub directory + template config
  plh_hub <hub_dir>                     # Run from directory
  plh_hub --config <path.json> [--validate | --keygen]
  plh_hub --dev                         # Dev/test mode; ephemeral keys, no vault
  plh_hub --help | -h

Init-only options:
  --name <name>      Hub name for --init
  --log-maxsize <MB> Rotate at (default 10)
  --log-backups <N>  Keep N (default 5; -1 = keep all)
```

**Parser contract** (identical to `role_cli::parse_role_args`):
- Returns `ParseResult { HubArgs args; int exit_code = -1; }`; never `std::exit`.
- `--help`/`-h` prints usage to stdout, returns `exit_code = 0`.
- Errors print to stderr, return `exit_code = 1`.
- Mode-exclusion and init-only-flag post-loop guards.

**Files**: `src/include/utils/hub_cli.hpp` (inline, mirrors `role_cli.hpp`).

## 6. Config — `hub.json`

### 6.1 Composite `HubConfig` (mirrors `RoleConfig`)

```cpp
class PYLABHUB_UTILS_EXPORT HubConfig
{
public:
    static HubConfig load(const std::string &path);
    static HubConfig load_from_directory(const std::string &dir);

    const HubIdentityConfig   &identity()   const;
    const AuthConfig          &auth()       const;   // reused from roles
    const ScriptConfig        &script()     const;   // reused; optional
    const LoggingConfig       &logging()    const;   // reused
    const HubNetworkConfig    &network()    const;
    const HubAdminConfig      &admin()      const;
    const HubBrokerConfig     &broker()     const;
    const HubFederationConfig &federation() const;
    const HubStateConfig      &state()      const;

    bool        load_keypair(const std::string &password);
    std::string create_keypair(const std::string &password);

    const nlohmann::json         &raw() const;
    bool                          reload_if_changed();
    const std::filesystem::path  &base_dir() const;
};
```

pImpl + `JsonConfig` backend (thread-safe, process-safe I/O + `reload_if_changed()`),
exactly as `RoleConfig` today. No directional `in_`/`out_` slots — the hub has no
asymmetric sides.

### 6.2 `hub.json` schema

```jsonc
{
  "hub": {
    "uid":       "HUB-MAIN-12345678",
    "name":      "MainHub",
    "log_level": "info",
    "auth":      { "keyfile": "vault/hub.vault" }
  },

  "script":              { "type": "python", "path": "." },
  "python_venv":         "",
  "stop_on_script_error": false,

  "logging": {
    "file_path":    "",
    "max_size_mb":  10,
    "backups":      5,
    "timestamped":  true
  },

  "network": {
    "broker_endpoint": "tcp://0.0.0.0:5570",
    "broker_bind":     true,
    "zmq_io_threads":  1
  },

  "admin": {
    "enabled":        true,
    "endpoint":       "tcp://127.0.0.1:5600",
    "dev_mode":       false,
    "token_required": true
  },

  "broker": {
    "heartbeat_timeout_ms":   15000,
    "heartbeat_multiplier":   5,
    "default_channel_policy": "open",
    "known_roles": [
      { "uid": "PROD-SRC-...", "name": "Source", "pubkey": "z85..." }
    ]
  },

  "federation": {
    "enabled":           false,
    "peers":             [ { "uid": "...", "endpoint": "...", "pubkey": "..." } ],
    "forward_timeout_ms": 2000
  },

  "state": {
    "disconnected_grace_ms":   60000,
    "max_disconnected_entries": 1000
  }
}
```

### 6.3 Parsing rules

- **Strict key whitelist.** Unknown top-level keys and unknown keys inside every
  sub-object throw `std::runtime_error("<hub>: unknown config key '<name>'")` at
  parse time. Verbatim from role-side pattern.
- **Sub-object type enforcement.** `"logging"`/`"network"`/... if present must be
  JSON objects, else throw.
- **Sentinels documented in code.** `"backups": -1` → `kKeepAllBackups`
  (`numeric_limits<size_t>::max()`). `"disconnected_grace_ms": -1` → infinite.
- **Absent sections take defaults.** No `"federation"` → `enabled=false`; no
  `"script"` → hub runs without one.
- **UID auto-generation with diagnostic.** Absent `hub.uid` → generate
  `HUB-<NAME>-<hex8>`, print warning with the generated UID so the operator can
  paste it back for stability (same pattern as role identity).

### 6.4 New categorical sub-config headers (`src/include/utils/config/`)

- `hub_identity_config.hpp` — `HubIdentityConfig { uid, name, log_level }`.
- `hub_network_config.hpp` — broker endpoint/bind/io_threads.
- `hub_admin_config.hpp` — endpoint/enabled/dev_mode/token_required.
- `hub_broker_config.hpp` — heartbeat timeouts/policy/known_roles.
- `hub_federation_config.hpp` — enabled/peers/forward_timeout.
- `hub_state_config.hpp` — disconnected_grace / max_disconnected_entries.

Reused from role-side: `auth_config.hpp`, `script_config.hpp`, `logging_config.hpp`.

**Rename required**: the existing `src/include/utils/config/hub_config.hpp` is
role-facing (for `in_hub_dir`/`out_hub_dir` references). Renamed to
`hub_ref_config.hpp` to free the `HubConfig` name for the hub-side config here.

### 6.5 Vault + keygen (three-mode semantics)

Same overall pattern as the role side (HEP-CORE-0024 §11). The vault
stores the broker's stable CURVE keypair plus, optionally, the admin
token.

- **`plh_hub --keygen`** generates a CURVE25519 keypair, writes the
  secret half into `hub.auth.keyfile` encrypted with `PYLABHUB_HUB_PASSWORD`
  (env var → interactive prompt fallback per the same source-chain as
  role-side), prints the public key (for roles to pin via
  `in_hub_pubkey` / `out_hub_pubkey` in their own configs) and the
  `hub_uid`. Idempotent against the same vault file via the existing
  `HubVault` API.
- **Run mode** (without `--dev`) reads the vault, unlocks it via the
  same password source chain, loads the broker's stable CURVE keypair
  for the ROUTER socket. If `admin.token_required: true`, the vault
  also carries the admin token (separate slot, same KDF domain) — the
  `AdminService` (§11.3) compares against this token on each request.
  See `src/include/utils/hub_vault.hpp` for the existing storage shape;
  it already supports both keypair and token slots — no vault extension
  needed.
- **`plh_hub --dev`** generates an ephemeral CURVE keypair at startup,
  skips vault entirely, skips admin token. The `AdminService` endpoint
  is enforced to bind only to `127.0.0.1` in this mode, since there is
  no token to gate access. Use for local development and tests only.

## 7. Hub directory layout (`--init` output)

```
<hub_dir>/
├── hub.json
├── script/                     # OPTIONAL — absent = hub runs without scripting
│   └── python/                 #   (or lua/ or native/)
│       └── __init__.py
├── vault/
│   └── hub.vault               # CURVE keypair + (optional) admin token
├── logs/
│   └── <hub_uid>-<ts>.log
└── run/
```

Mirrors HEP-CORE-0024 role layout. `HubDirectory` helper (new) mirrors
`RoleDirectory` accessors (`base_dir/vault/logs/script/run`,
`create_standard_layout()`, `has_standard_layout()`).

## 8. State tables — `HubState`

Read-mostly aggregate owned by `HubHost`. `BrokerService` updates as messages
arrive; `AdminService` and `HubAPI` read via `HubHost` accessors.

```mermaid
graph TB
    HubState[HubState<br/>single mutex]
    HubState --> Channels["channels<br/>map&lt;name, ChannelEntry&gt;"]
    HubState --> Roles["roles<br/>map&lt;uid, RoleEntry&gt;"]
    HubState --> Bands["bands<br/>map&lt;name, BandEntry&gt;"]
    HubState --> Peers["peers<br/>map&lt;uid, PeerEntry&gt;"]
    HubState --> ShmBlocks["shm_blocks<br/>map&lt;name, ShmBlockRef&gt;"]
    HubState --> Counters["BrokerCounters"]

    Channels -.in-position.-> M1["role metrics blobs<br/>_collected_at"]
    Roles -.in-position.-> M2["ContextMetrics snapshots<br/>_collected_at"]
    Counters -.in-position.-> M3["RoleStateMetrics<br/>broker counters"]
    ShmBlocks -.pointer-to-collect.-> M4["hub::DataBlock::get_metrics<br/>invoked at query time"]

    style M4 stroke:#f90,stroke-width:2px
```

```cpp
struct HubState
{
    std::unordered_map<std::string, ChannelEntry>  channels;
    std::unordered_map<std::string, RoleEntry>     roles;
    std::unordered_map<std::string, BandEntry>     bands;
    std::unordered_map<std::string, PeerEntry>     peers;
    std::unordered_map<std::string, ShmBlockRef>   shm_blocks;  // pointer-to-collect
    BrokerCounters                                  counters;   // in-position
};
```

| Entry | Updated by | Holds |
|---|---|---|
| `ChannelEntry` | REG_REQ / DISC_REQ / CHANNEL_CLOSING / broker-internal | name, schema, producer PID, consumers, created_at, status |
| `RoleEntry` | REG/HEARTBEAT/METRICS_REPORT/DISC/timeout | uid, name, role_tag, channels, state, first_seen, last_heartbeat, latest metrics, `metrics_collected_at`, pubkey |
| `BandEntry` | BAND_JOIN / BAND_LEAVE | name, members[], last_activity |
| `PeerEntry` | HUB_PEER_HELLO / HUB_PEER_BYE / federation heartbeat | uid, endpoint, state, last_seen |
| `ShmBlockRef` | channel registration with SHM transport | channel name, block path; metrics collected via `collect_shm_info(channel)` at query time |
| `BrokerCounters` | broker internal | `RoleStateMetrics` (HEP-0023 §2.5), ctrl queue depth, byte counts, per-msg-type counts |

**Retention**: disconnected roles linger `state.disconnected_grace_ms` (default
60s) with `status: "disconnected"` before eviction; LRU cap
`state.max_disconnected_entries` prevents unbounded growth. Closed channels
evict immediately.

**Consistency**: single internal mutex; accessors return snapshot structs. No
cross-field consistency guarantee; each metric entry carries `_collected_at`.

## 9. Message-Processing Contract

The broker's `process_message()` and the per-handler dispatch operate
under a fixed contract that defines: (i) what counter goes up when,
(ii) what happens on error/exception, (iii) what reply (if any) the
client receives, and (iv) where in the pipeline each side-effect
occurs.  This contract is the seam between the wire (ZMQ frames) and
the model (`HubState` mutations).  It is normative for any handler
added to `process_message()` and for any code that reads counters via
`HubState::counters()`.

### 9.1 Pipeline stages

Every inbound message traverses these stages in order:

```
S0  Socket recv         zmq::recv_multipart returns frames
S1  Frame validation    frame count, size cap (1 MB)
S2  Body parse          nlohmann::json::parse(frames[3])
S3  Dispatch            msg_type → handler branch in process_message()
S4  Handler execution   broker validation → HubState op → reply build
S5  Reply send          send_reply() (request-reply types only)
S6  Post-process        counters, hook
```

Stages S0–S2 happen in the recv loop, before `process_message()` is
invoked.  S3–S6 happen inside `process_message()`.

### 9.2 Reply-shape classification (per msg_type)

Each msg_type is **statically classified** at code-time as one of:

- **request-reply** — client always expects exactly one reply frame
  (`*_ACK` on success, `ERROR` on failure).  The protocol shape is
  fixed; the server's outcome does not change it.
- **fire-and-forget** — client never expects a reply.  Errors are
  handled entirely on the server side (counter + hook + log).  The
  client cannot distinguish "delivered" from "error" — that is the
  intentional contract of fire-and-forget messages.

Current classification (single source of truth: the dispatcher branch
that either calls `send_reply()` or does not):

```
Request-reply:
  REG_REQ, DISC_REQ, DEREG_REQ,
  CONSUMER_REG_REQ, CONSUMER_DEREG_REQ, CONSUMER_DEREG_ACK,
  ENDPOINT_UPDATE_REQ, SCHEMA_REQ,
  CHANNEL_LIST_REQ, METRICS_REQ, SHM_BLOCK_QUERY_REQ,
  ROLE_PRESENCE_REQ, ROLE_INFO_REQ,
  BAND_JOIN_REQ, BAND_LEAVE_REQ, BAND_MEMBERS_REQ,
  HUB_PEER_HELLO         (HUB_PEER_HELLO_ACK)

Fire-and-forget:
  HEARTBEAT_REQ, METRICS_REPORT_REQ,
  CHECKSUM_ERROR_REPORT,
  CHANNEL_NOTIFY_REQ, CHANNEL_BROADCAST_REQ,
  BAND_BROADCAST_REQ,
  HUB_PEER_BYE,
  HUB_RELAY_MSG (peer-DEALER inbound),
  HUB_TARGETED_MSG (peer-DEALER inbound)
```

When adding a new msg_type, the contributor MUST classify it
explicitly in the dispatcher, and the classification table above
MUST be updated.

### 9.3 Failure-mode disposition (per stage)

| Stage | Failure | Action | Counter | Hook fires? |
|---|---|---|---|---|
| S0 | recv error (`EAGAIN`, `EINTR`) | log at TRACE; continue loop | none | no |
| S1 | wrong frame count, oversized payload | `LOGGER_WARN`; drop; continue | `sys.malformed_frame` | yes (msg_type empty) |
| S2 | `nlohmann::json::parse_error` | `LOGGER_WARN`; drop; continue | `sys.malformed_json` | yes (msg_type empty) |
| S3 | unknown msg_type | `LOGGER_WARN`; ERROR reply (request-reply shape per the unknown name's lack of classification — falls to fire-and-forget by default; current dispatcher always replies ERROR for unknowns); continue | **only `sys.unknown_msg_type`** — the unknown msg_type string is NOT inserted into `msg_type_counts` (cardinality-attack mitigation, R1) | yes |
| S4 | broker-level validation rejected (e.g. missing required field) | request-reply: ERROR reply; fire-and-forget: log + counter only | `msg_type_counts[type]` | no — this is a normal protocol error, surfaced via the ERROR reply for request-reply types |
| S4 | HubState validator silent-drop (e.g. invalid uid) | reply built from POST-state read; client sees actual outcome | `msg_type_counts[type]` only; `sys.invalid_identifier_rejected` bumped inside HubState | no — HubState handles internally |
| S4 | unexpected exception (e.g. `zmq::error_t`, `std::bad_alloc`) | request-reply: best-effort ERROR reply (inner try/catch); fire-and-forget: NO reply (never confuse the client); always continue loop | `msg_type_counts[type]` + `msg_type_errors[type]` + `sys.handler_exception` | yes |
| S5 | reply send fails (DEALER closed, HWM hit) | inner try/catch; `LOGGER_WARN`; continue | none (this is a transport-layer issue, not a processing one) | no |
| S6 | counter bump | atomic increment; never fails | n/a | n/a |

### 9.4 Counter taxonomy

Three independent kinds; all live in `BrokerCounters` and are exposed
via `HubState::counters()`:

**Wire metrics** (broker dispatcher; bump policy: post-processing):

```
msg_type_counts[<known_msg_type>]   bumped at S6 if S0–S3 succeeded;
                                    counts every dispatch-completed message
                                    (success OR error) of a known type.

bytes_in_total                      bumped at S6 with frames[3].size().
bytes_out_total                     deferred — multi-target fan-out
                                    (broadcast/relay) makes per-message
                                    accounting ambiguous.

msg_type_errors[<known_msg_type>]   bumped at S6 alongside msg_type_counts
                                    if S4 hit an exception or validation
                                    rejection (request-reply types only).
```

**Operational metrics** (failure-point bump):

```
sys.malformed_frame             — S1 failure (count, size limits).
sys.malformed_json              — S2 failure (JSON parse).
sys.unknown_msg_type            — S3 failure (no handler matched).
sys.handler_exception           — S4 uncaught exception.
sys.invalid_identifier_rejected — bumped INSIDE HubState capability ops
                                  when an _on_* op silent-drops on validator.
                                  (Independent of the dispatcher; counts
                                  state-mutation rejects, not message arrivals.)
```

**Semantic metrics** (HubState capability op; bump atomic with state change):

```
ready_to_pending_total          HEP-CORE-0023 §2.5
pending_to_ready_total
pending_to_deregistered_total
close:<reason>                  per ChannelCloseReason enum value
```

These are triggered by **state transitions**, which include but are
not limited to message arrivals (the heartbeat-timeout sweep fires
`ready_to_pending_total` from a timer, no inbound message).  They
remain inside the capability ops because the dispatcher cannot detect
a transition without state-diff introspection.

### 9.5 Exception-safety contract

> `process_message()` MUST NOT propagate exceptions to its caller (the
> recv loop).  Any `std::exception` from any stage S3–S5 is caught,
> logged at ERROR, the appropriate counters and hook are fired, a
> best-effort ERROR reply is sent **only for request-reply types**, and
> the broker continues to the next message.

Implementation: outer try/catch in `process_message()`; inner try/catch
around the best-effort ERROR reply send (R11); inner try/catch around
the hook invocation (R2 — the user-supplied hook may throw, broker
must survive).

The recv loop's existing outer `try/catch (nlohmann::json::exception)`
remains as-is for S2 protection; the new contract removes the broader
exception class from escaping past `process_message()`.

### 9.6 `on_processing_error` hook

Opt-in callback on `BrokerService::Config`:

```cpp
struct ProcessingError {
    std::string                msg_type;       // empty for S1/S2 errors
    std::string                error_kind;     // "malformed_frame" | "malformed_json" |
                                               // "unknown_msg_type"  | "exception"
    std::string                detail;         // exception what() / parse error / etc.
    std::optional<std::string> peer_identity;  // ROUTER routing identity if available
};

std::function<void(const ProcessingError &)> on_processing_error;
```

Fired AFTER counter bumps (so a hook handler reading
`HubState::counters()` sees fresh state).  Invoked under broker thread,
synchronously — handler must be fast; for slow work, enqueue and
return.  Hook may throw; broker swallows.

The struct is **append-only** for ABI stability: future fields may be
added at the end; existing fields may not be removed or reordered.
(R6.)

### 9.7 Mutation-vs-reply ordering (request-reply handlers)

```
1. Parse + broker-level field validation
       if invalid → ERROR reply, return

2. Call HubState capability op
       op may silent-drop on internal validator failure; that is fine

3. Build reply from POST-state read
       e.g. `auto entry = hub_state_.channel(name);`
       if op silent-dropped, the entry is absent → reply reflects reality

4. send_reply(...)
```

This rule eliminates the bug class where the broker reports success
but HubState silently dropped the mutation: because the reply is
constructed from observed state, a silent drop surfaces as a NOT_FOUND
or empty-payload reply rather than a misleading ACK.

### 9.8 Cross-reference

This contract is exercised in:
- `src/utils/ipc/broker_service.cpp` — `process_message()` is the
  single dispatch entrypoint that conforms to this contract.
- `src/utils/ipc/hub_state.cpp` — capability ops bump only **semantic**
  counters; **wire** counter bumps were moved to the dispatcher in
  HEP-CORE-0033 G2.2.4.

## 10. Metrics model (supersedes HEP-CORE-0019 §3-4)

### 10.1 Ingress — role→hub push only (unchanged from today)

- `HEARTBEAT_REQ` with `metrics` field, iteration-gated, stops when role stalls.
- `METRICS_REPORT_REQ`, time-only, configurable via role's `cfg.report_metrics`.

No broker-initiated metrics pull exists or will exist.

```mermaid
sequenceDiagram
    participant Role as plh_role
    participant Broker as BrokerService
    participant State as HubState

    loop Heartbeat tick (iteration-gated — stops if role stalls)
        Role->>Broker: HEARTBEAT_REQ{channel, metrics, pid}
        Broker->>State: update_producer_metrics(ch, metrics, pid)
    end

    loop Metrics report tick (time-only; if cfg.report_metrics)
        Role->>Broker: METRICS_REPORT_REQ{channel, uid, metrics}
        Broker->>State: update_consumer_metrics(ch, uid, metrics)
    end

    Note over Broker,Role: Broker NEVER sends METRICS_REQ to Role
```


### 10.2 Entry types

- **In-position**: role-pushed metrics, broker counters, federation peer states.
- **Pointer-to-collect**: SHM block metrics (invoke `hub::DataBlock::get_metrics(channel)` at query time). Future extensions (system CPU/RSS, etc.) use this pattern.

### 10.3 Query flow

```mermaid
sequenceDiagram
    participant Client as Admin/Script
    participant API as AdminService / HubAPI
    participant Host as HubHost
    participant State as HubState
    participant Collect as SHM collector<br/>(hub::DataBlock::get_metrics)

    Client->>API: query_metrics(filter)
    API->>Host: query_metrics(filter)
    Host->>State: lock + walk tables
    State-->>Host: matching in-position entries
    loop per ShmBlockRef in filter
        Host->>Collect: collect_shm_info(channel)
        Collect-->>Host: live SHM metrics
    end
    Host->>Host: assemble JSON + _collected_at per entry
    Host-->>API: JSON response
    API-->>Client: JSON response
```

1. Accept a `MetricsFilter` (role uids, channel names, band names, peer uids,
   category tags: `"channel"`, `"role"`, `"band"`, `"peer"`, `"broker"`, `"shm"`,
   `"all"`).
2. Walk `HubState` under the state mutex; select matching entries.
3. Read in-position data directly; invoke pointer-to-collect callbacks.
4. Build single JSON response:
   ```jsonc
   {
     "status": "ok",
     "queried_at": "ISO-8601",
     "filter":    { ... },
     "channels":  { "<ch>": { "producer": {..., "_collected_at": "..."},
                               "consumers": {...},
                               "shm":      {..., "_collected_at": "..."} } },
     "roles":     { "<uid>": { ..., "_collected_at": "...", "_status": "ready" } },
     "bands":     { ... },
     "peers":     { ... },
     "broker":    { ... }
   }
   ```
5. Return. No retry, no wait-for-update.

### 10.4 Relation to existing `BrokerService::query_metrics_json_str`

Existing impl (`broker_service.cpp:2336`, `2523`, `2534`) already has this shape
for channels + shm_blocks. Refactor extends it with role/band/peer/broker
categories, `_collected_at` per entry, and plumbs through `HubHost` (not
directly on `BrokerService`).

## 11. Admin RPC surface — `AdminService`

Replaces legacy `AdminShell` which only offered `{token, code}` → `exec(code)`.

### 11.1 Transport

- ZMQ REP socket at `admin.endpoint` (default `tcp://127.0.0.1:5600`).
- Request: `{ "method": "<name>", "token": "<admin_token>", "params": { ... } }`.
- Response: `{ "status": "ok|error", "result": ..., "error": {"code": ..., "message": ...} }`.

```mermaid
sequenceDiagram
    participant Client as Admin Client
    participant Admin as AdminService (REP)
    participant Host as HubHost
    participant Engine as ScriptEngine<br/>(dev-only path)

    Client->>Admin: {method, token, params}

    alt invalid token (token_required)
        Admin-->>Client: {status:error, code:unauthorized}
    else method = query_*
        Admin->>Host: accessor call
        Host-->>Admin: snapshot data
        Admin-->>Client: {status:ok, result}
    else method = control_*
        Admin->>Host: control operation
        Host-->>Admin: Result<void, Error>
        Admin-->>Client: {status, result/error}
    else method = exec_python (dev_mode)
        Admin->>Engine: eval(code)
        Engine-->>Admin: captured stdout + result
        Admin-->>Client: {status, result}
    end
```


### 11.2 Methods (v1)

**Query**: `list_channels`, `get_channel`, `list_roles`, `get_role`,
`list_bands`, `list_peers`, `query_metrics`, `list_known_roles`.

**Control**: `close_channel`, `broadcast_channel`, `revoke_role`,
`add_known_role`, `remove_known_role`, `reload_config`, `request_shutdown`.

**Dev-only** (gated by `admin.dev_mode: true`): `exec_python` — runs in the
hub script engine's namespace via `ScriptEngine::eval`.

### 11.3 Authorization

- `admin.token_required: true` → request `"token"` must match vault's
  admin token (KDF-derived, same-vault different slot). Mismatch →
  `{"status": "error", "error": {"code": "unauthorized"}}`.
- `admin.token_required: false` → token ignored; endpoint MUST bind to
  `127.0.0.1` (enforced at construction).
- `exec_python` always requires token when gated, plus `dev_mode: true`.

### 11.4 Files

- `src/include/utils/admin_service.hpp`, `src/utils/service/admin_service.cpp`.

## 12. Script callbacks + `HubAPI`

### 12.1 Engine

`HubHost` owns `std::unique_ptr<scripting::ScriptEngine>` (null if no script).
Runs on its own thread with the cross-thread dispatch pattern roles use.
Engine factory: `scripting::make_engine_from_script_config(cfg.script())`.

### 12.2 Callbacks (all optional; C++ defaults = no-op / accept)

**Lifecycle**: `on_start(api)`, `on_tick(api)`, `on_stop(api)`.

**Role events**: `on_role_registered(api, role_info)`,
`on_role_closed(api, uid, reason)`.

**Channel events**: `on_channel_opened(api, channel_info)`,
`on_channel_closed(api, name)`.

**Band events** (HEP-0030): `on_band_joined(api, band, member_uid)`,
`on_band_left(api, band, member_uid)`.

**Federation events**: `on_peer_connected(api, peer_uid)`,
`on_peer_disconnected(api, peer_uid, reason)`.

**Veto hooks** (sync, bool return, default accept):
`on_channel_close_request(api, channel) → bool`,
`on_role_register_request(api, info) → bool`.

Script exceptions caught; `stop_on_script_error: true` promotes to fatal (same
as role side).

### 12.3 `HubAPI` surface (bound into Python + Lua)

Read: `list_channels`, `get_channel`, `list_roles`, `get_role`, `list_bands`,
`list_peers`, `query_metrics`, `config`, `uid`, `name`.

Control: `close_channel`, `broadcast_channel`, `revoke_role`, `add_known_role`,
`remove_known_role`, `request_shutdown`.

All methods resolve via `HubHost` accessors; scripts never touch
`BrokerService` directly. Pybind11 bindings live in
`src/scripting/hub/hub_api.cpp`.

## 13. Protocol — additions / unchanged

- **Role→broker protocol**: unchanged. REG_REQ, DISC_REQ, HEARTBEAT_REQ,
  METRICS_REPORT_REQ, notifies, etc. No wire-format break.
- **Role-side headers, config, lifecycle**: unchanged.
- **New admin RPC on the admin socket**: methods above (§11).
- **Internal callbacks**: `BrokerService` gains event hooks into `HubHost`
  (which fans out to script events). Replaces ad-hoc `pylabhub_module`
  callback wiring.

## 14. Relationship to existing HEPs

```mermaid
graph TB
    H33[HEP-0033<br/>Hub Character]
    H19[HEP-0019<br/>Metrics Plane]
    H01[HEP-0001<br/>Lifecycle]
    H07[HEP-0007<br/>DataHub Protocol]
    H21[HEP-0021<br/>ZMQ Registry]
    H22[HEP-0022<br/>Federation]
    H23[HEP-0023<br/>Startup Coord]
    H24[HEP-0024<br/>Role Directory]
    H30[HEP-0030<br/>Band Messaging]

    H33 -.supersedes §3-4.-> H19
    H33 -->|uses| H01
    H33 -->|uses| H07
    H33 -->|uses| H21
    H33 -->|uses| H22
    H33 -->|uses| H23
    H33 -->|shape mirrors| H24
    H33 -->|exposes events| H30

    style H33 fill:#d4f1d4,stroke:#2a2
    style H19 stroke-dasharray: 5 5
```

| HEP | Relationship |
|---|---|
| HEP-CORE-0001 | Used verbatim for lifecycle ordering. |
| HEP-CORE-0007 | Used verbatim for channel-level semantics. |
| HEP-CORE-0019 | **This HEP supersedes §3-4** (periodic broker-pull). §3.2 (live SHM merge) retained. |
| HEP-CORE-0021 | Used verbatim. |
| HEP-CORE-0022 | Used verbatim; federation config is factored into `HubFederationConfig`. |
| HEP-CORE-0023 | Used verbatim (Phase 2 multiplier included). |
| HEP-CORE-0024 | Shape mirrored: hub gets `HubDirectory`, `hub_cli`, `--init/--validate/--keygen`, directory layout. |
| HEP-CORE-0030 | Band events surface via script callbacks; admin RPC includes `list_bands`. |

## 15. Implementation phases

Phases should each land in a build-green, test-green checkpoint.

```mermaid
graph LR
    P1[Phase 1<br/>Config]
    P2[Phase 2<br/>hub_cli]
    P3[Phase 3<br/>HubDirectory + --init]
    P4[Phase 4<br/>HubState + HubHost]
    P5[Phase 5<br/>Query engine]
    P6[Phase 6<br/>AdminService]
    P7[Phase 7<br/>HubScriptRunner]
    P8[Phase 8<br/>HubAPI bindings]
    P9[Phase 9<br/>plh_hub binary<br/>delete hub_python/]
    P10[Phase 10<br/>Docs + HEP amend]

    P1 --> P2
    P1 --> P3
    P1 --> P4
    P4 --> P5
    P4 --> P6
    P1 --> P7
    P7 --> P8
    P2 --> P9
    P3 --> P9
    P5 --> P9
    P6 --> P9
    P8 --> P9
    P9 --> P10
```

Phase numbering reflects dependency order; independent phases (e.g. P2, P3, P4)
can land in parallel once P1 lands.


- **Phase 1** — Config: rename role-facing `hub_config.hpp` → `hub_ref_config.hpp`;
  create new hub-side sub-configs (§6.4); create `HubConfig` composite class.
  No behavior change (hub binary still disabled). Covered by L2 tests.
- **Phase 2** — `hub_cli::parse_hub_args` (§5). Covered by L2 tests mirroring
  `role_cli` test shape.
- **Phase 3** — `HubDirectory` + `--init` template output. L2 tests for dir
  layout + template validation.
- **Phase 4** — `HubState` struct + accessors on a new `HubHost` class (wraps
  existing `BrokerService` without changing broker behavior). L2 tests for
  snapshot accessors.
- **Phase 5** — Query engine (`HubHost::query_metrics`) over `HubState` +
  existing `collect_shm_info`. L2 tests for filter coverage + `_collected_at`.
- **Phase 6** — `AdminService` structured RPC (§11); retire `AdminShell`
  dependency. L3 tests for each RPC method.
- **Phase 7** — `scripting::hub_lifecycle_modules()` + `HubScriptRunner` using
  `ScriptEngine`; retire `PythonInterpreter`/`HubScript`/`hub_script_api`/
  `pylabhub_module`. L3 tests for each callback + default no-op behavior.
- **Phase 8** — `HubAPI` pybind11 + Lua bindings (§12.3). L3 tests via each
  engine.
- **Phase 9** — `plh_hub` binary; re-enable build; delete `src/hubshell.cpp` +
  `src/hub_python/*`. L4 no-hub tier tests (parallel `test_layer4_plh_role/`).
- **Phase 10** — HEP-0019 amendment finalised; README + deployment docs
  updated.

## 16. Open items (deferred to implementation)

1. `add_known_role`/`remove_known_role` persistence — in-memory only by default;
   `persist_known_role_changes: false` toggle for opt-in disk write.
2. `script.tick_interval_ms` location — `ScriptConfig` vs new `HubScriptConfig`.
3. Band event hooks — `BandRegistry` currently broker-thread-internal; needs
   hook point for `HubState` updates.
4. Graceful-vs-fast shutdown semantics for `request_shutdown`.
5. Timestamp precision (ms vs µs) and clock source alignment.
6. `_collected_at` semantics for pointer-to-collect entries.
7. Script engine thread lifetime ordering vs `BrokerService` teardown.
8. Dev-mode admin token behaviour.

## 17. Out of scope

- Hub-side HA / replication.
- Hub script hot-reload mid-run.
- Binary variants (`--kind`) — subsystems toggled via config only.
- Role-side rewrites.
- HEP-CORE-0019 periodic broker-pull (explicitly replaced by query-driven
  model; §10).
