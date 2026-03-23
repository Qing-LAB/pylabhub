# Tech Draft: Unified Config Module — RoleConfig + JsonConfig

**Status**: Draft (2026-03-21, revised)
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0024 (Role Directory Service), SE-14 (script.type validation)

## 1. Problem Statement

The current config system has three independent monolithic config structs
(ProducerConfig, ConsumerConfig, ProcessorConfig) with ~45-70 fields each.
Parsing logic is duplicated across their `from_json_file()` implementations.
Adding a validation requires editing three files identically. No central
place holds the truth of config schema, defaults, or validation rules.

Additionally:
- JSON field names are inconsistent across roles (`hub_dir` vs `in_hub_dir`,
  `zmq_out_endpoint` vs `zmq_in_endpoint`)
- `JsonConfig` (thread-safe transactional file I/O) is not used for role
  config loading — only raw `ifstream + json::parse()`
- No encapsulation — config structs are flat bags of public fields
- No common base — each role reinvents identity, timing, auth, etc.

## 2. Design Principles

1. **RoleConfig is the single config class.** No ProducerConfig, ConsumerConfig,
   ProcessorConfig. One class with a pluggable role-specific extension.

2. **JsonConfig is the file backend.** All config I/O goes through JsonConfig
   for thread-safety, process-safety, reload detection, and transactional writes.

3. **All directional fields use `in_`/`out_` prefixes.** Consistently, in
   both JSON and C++ accessors. No legacy unprefixed forms.

4. **Two slots for everything directional.** Hub, transport, validation,
   SHM, channel — RoleConfig always holds `in_` and `out_` variants.
   Producer populates `out_` only. Consumer populates `in_` only.
   Processor populates both. No special cases in the loader.

5. **Private data, public const accessors.** RoleConfig uses pImpl. External
   code can only read config, never write it.

6. **Role-specific data via factory callback.** Each role defines a fields
   struct and a parser function. RoleConfig stores the result as type-erased
   `std::any` inside its pImpl. Typed access via template convenience wrapper.

## 3. JSON Field Naming Convention (Clean Break)

All directional fields use `in_`/`out_` prefixes consistently:

```json
// Producer — "out" direction
{
  "producer": { "uid": "PROD-TEMPSENS-12345678", "name": "TempSensor" },
  "out_hub_dir": "/var/pylabhub/my_hub",
  "out_channel": "lab.sensors.temperature",
  "out_transport": "shm",
  "out_shm_enabled": true,
  "out_shm_slot_count": 8,
  "out_update_checksum": true,
  "out_slot_schema": { "fields": [{"name": "value", "type": "float32"}] },
  "target_period_ms": 100,
  "script": { "type": "python", "path": "." }
}

// Consumer — "in" direction
{
  "consumer": { "uid": "CONS-DISPLAY-87654321", "name": "Display" },
  "in_hub_dir": "/var/pylabhub/my_hub",
  "in_channel": "lab.sensors.temperature",
  "in_transport": "shm",
  "in_verify_checksum": false,
  "script": { "type": "lua", "path": "." }
}

// Processor — both directions
{
  "processor": { "uid": "PROC-FILTER-11111111", "name": "Filter" },
  "in_hub_dir": "/var/pylabhub/hub_a",
  "out_hub_dir": "/var/pylabhub/hub_b",
  "in_channel": "lab.sensors.raw",
  "out_channel": "lab.sensors.processed",
  "in_transport": "shm",
  "out_transport": "zmq",
  "out_zmq_endpoint": "tcp://0.0.0.0:5590",
  "in_verify_checksum": true,
  "out_update_checksum": true,
  "script": { "type": "python", "path": "." }
}
```

Non-directional fields (identity, timing, script, inbox, startup, monitoring,
auth) use unprefixed names — they apply to the role as a whole.

## 4. Information Flow Design

Every config setting follows one canonical path from JSON file to consumer.
No setting is initialized, transformed, or overridden outside this path.

```
JSON file
  │
  ▼
RoleConfig::load() / load_from_directory()
  │
  ├──► JsonConfig (thread-safe file I/O backend)
  │        │
  │        ▼
  │    lock_for_read() → raw JSON snapshot
  │
  ├──► Categorical parsers (inline, in headers)
  │    Each reads its fields from JSON, validates, returns a value struct.
  │    Parser is the SINGLE SOURCE OF TRUTH for defaults and validation.
  │
  ├──► RoleConfig::Impl (private, in .cpp)
  │    Stores all parsed structs. Single storage location.
  │
  ├──► RoleParser callback (optional)
  │    Returns role-specific fields as std::any → stored in Impl.
  │
  └──► Vault operations (load_keypair / create_keypair)
       Only mutation point: writes auth.client_pubkey/seckey into Impl.
```

**Consumer access** is always through const typed accessors on RoleConfig:
```
Role host:  config_.script().stop_on_script_error
            config_.out_shm().update_checksum
            config_.role_data<ProducerFields>().out_slot_schema_json
```

**Rules**:
- No config struct is constructed outside its parser.
- No config field is set outside `load_common()` or `load_keypair()`.
- Role hosts never store copies of config fields — they alias via `const auto&`.
- Adding a new field: add to struct → add to parser → it's automatically available.

## 5. Categorical Config Structs

All structs are plain data holders in `src/include/utils/config/`. Each has
an inline `parse_*()` function — single source of truth for validation and
defaults.

### 5.1 Non-directional categories

**IdentityConfig** (`identity_config.hpp`):
- `uid`, `name`, `log_level` — role identity, parsed from `<role_tag>` JSON section.

**AuthConfig** (`auth_config.hpp`):
- `keyfile`, `client_pubkey`, `client_seckey` — CURVE authentication.
- `keyfile` parsed from JSON; pubkey/seckey populated by `RoleConfig::load_keypair()`.

**ScriptConfig** (`script_config.hpp`):
- `type`, `path`, `python_venv`, `type_explicit` — script engine selection.
- `stop_on_script_error` — fatal on script exception in callback.
- Parsed from `"script"` JSON section + top-level `"stop_on_script_error"`.

**TimingConfig** (`timing_config.hpp`):
- `period_us` — resolved from `target_period_ms` or `target_rate_hz` (mutually exclusive JSON inputs).
- `loop_timing` — MaxRate/FixedRate/FixedRateWithCompensation (derived from `period_us`).
- `queue_io_wait_timeout_ratio`, `heartbeat_interval_ms`.
- Note: `target_period_ms` and `target_rate_hz` are parser-local; only `period_us` is stored.

**InboxConfig** (`inbox_config.hpp`):
- `schema_json`, `endpoint`, `buffer_depth`, `overflow_policy`, `has_inbox()`.

**StartupConfig** (`startup_config.hpp`):
- `wait_for_roles` — HEP-0023 startup coordination entries.

**MonitoringConfig** (`monitoring_config.hpp`):
- `ctrl_queue_max_depth`, `peer_dead_timeout_ms`.

### 5.2 Directional categories (two slots: in_ and out_)

**HubConfig** (`hub_config.hpp`):
- `hub_dir`, `broker`, `broker_pubkey` — hub connection.
- Parsed from `<direction>_hub_dir`; broker resolved from hub directory.

**TransportConfig** (`transport_config.hpp`):
- `transport` (Shm/Zmq), `zmq_endpoint`, `zmq_bind`, `zmq_buffer_depth`, `zmq_overflow_policy`, `zmq_packing`.

**ShmConfig** (`shm_config.hpp`):
- `enabled`, `secret`, `slot_count`, `sync_policy` — SHM ring buffer settings.
- `update_checksum` (writer-side), `verify_checksum` (reader-side) — BLAKE2b integrity.

**Channel name** (`in_channel`, `out_channel`): plain strings, no wrapper struct.

### 5.3 Role-specific extensions (via `role_data<T>()`)

| Role | Struct | Fields |
|------|--------|--------|
| Producer | `ProducerFields` | `out_slot_schema_json`, `out_flexzone_schema_json` |
| Consumer | `ConsumerFields` | `in_slot_schema_json`, `in_flexzone_schema_json` |
| Processor | `ProcessorFields` | `in_slot_schema_json`, `out_slot_schema_json`, `out_flexzone_schema_json` |

## 6. RoleConfig Class

### 6.1 Public interface

```cpp
class PYLABHUB_UTILS_EXPORT RoleConfig
{
public:
    using RoleParser = std::function<std::any(const nlohmann::json&,
                                              const RoleConfig&)>;

    // ── Factory ───────────────────────────────────────────────────────

    static RoleConfig load(const std::string& path,
                           const char* role_tag,
                           RoleParser role_parser = nullptr);

    static RoleConfig load_from_directory(const std::string& dir,
                                          const char* role_tag,
                                          RoleParser role_parser = nullptr);

    // ── Non-directional accessors ────────────────────────────────────

    const config::IdentityConfig&   identity()   const;
    const config::AuthConfig&       auth()       const;
    const config::ScriptConfig&     script()     const;  // includes stop_on_script_error
    const config::TimingConfig&     timing()     const;
    const config::InboxConfig&      inbox()      const;
    const config::StartupConfig&    startup()    const;
    const config::MonitoringConfig& monitoring() const;

    // ── Directional accessors (two slots each) ───────────────────────

    const config::HubConfig&        in_hub()        const;
    const config::HubConfig&        out_hub()       const;
    const config::TransportConfig&  in_transport()  const;
    const config::TransportConfig&  out_transport() const;
    const config::ShmConfig&        in_shm()        const;  // includes update/verify_checksum
    const config::ShmConfig&        out_shm()       const;
    const std::string&              in_channel()    const;
    const std::string&              out_channel()   const;

    // ── Mutable auth (post-parse vault decryption) ────────────────────

    config::AuthConfig& mutable_auth();

    // ── JsonConfig-backed operations ──────────────────────────────────

    const nlohmann::json& raw() const;
    bool reload_if_changed();

    // ── Role-specific typed access ────────────────────────────────────

    /// Returns true if role-specific data was loaded.
    bool has_role_data() const;

    /// Typed access to role-specific data. Throws std::bad_any_cast on
    /// type mismatch. Template is header-only; storage is in pImpl.
    template<typename T>
    const T& role_data() const
    {
        return std::any_cast<const T&>(role_data_any_());
    }

    template<typename T>
    T& mutable_role_data()
    {
        return std::any_cast<T&>(mutable_role_data_any_());
    }

    // ── Role tag ──────────────────────────────────────────────────────

    const std::string& role_tag() const;
    const std::filesystem::path& base_dir() const;

    // ── Special members ───────────────────────────────────────────────

    ~RoleConfig();
    RoleConfig(RoleConfig&&) noexcept;
    RoleConfig& operator=(RoleConfig&&) noexcept;

private:
    RoleConfig();  // private — use factory methods

    // Non-template bridges to pImpl (compiled in .cpp)
    const std::any& role_data_any_() const;
    std::any& mutable_role_data_any_();

    struct Impl;
    std::unique_ptr<Impl> impl_;   // ONLY member — pure pImpl, ABI-safe
};
```

### 5.2 Impl structure

```cpp
struct RoleConfig::Impl
{
    // JsonConfig backend
    JsonConfig jcfg;

    // Role metadata
    std::string role_tag;
    std::filesystem::path base_dir;

    // Non-directional categories
    config::IdentityConfig   identity;
    config::AuthConfig       auth;
    config::ScriptConfig     script;
    config::TimingConfig     timing;
    config::InboxConfig      inbox;
    config::ValidationConfig validation;
    config::StartupConfig    startup;
    config::MonitoringConfig monitoring;

    // Directional categories (two slots each)
    config::HubConfig        in_hub;
    config::HubConfig        out_hub;
    config::TransportConfig  in_transport;
    config::TransportConfig  out_transport;
    config::ShmConfig        in_shm;
    config::ShmConfig        out_shm;
    config::DirectionalValidationConfig in_validation;
    config::DirectionalValidationConfig out_validation;
    std::string              in_channel;
    std::string              out_channel;

    // Role-specific extension (type-erased)
    std::any                 role_data;
};
```

### 5.3 Factory implementation

```cpp
RoleConfig RoleConfig::load(const std::string& path,
                             const char* role_tag,
                             RoleParser role_parser)
{
    RoleConfig cfg;
    cfg.impl_ = std::make_unique<Impl>();
    auto& s = *cfg.impl_;

    s.role_tag = role_tag;
    s.base_dir = std::filesystem::path(path).parent_path();

    // JsonConfig as backend — thread-safe, process-safe
    s.jcfg.init(path);
    const auto& j = s.jcfg.data();

    // Determine default period: producer=100ms, others=0
    const double default_period =
        (std::string_view(role_tag) == "producer") ? 100.0 : 0.0;

    // ── Non-directional categories ───────────────────────────────────
    s.identity   = config::parse_identity_config(j, role_tag);
    s.auth       = config::parse_auth_config(j, role_tag);
    s.script     = config::parse_script_config(j, s.base_dir, role_tag);
    s.timing     = config::parse_timing_config(j, role_tag, default_period);
    s.inbox      = config::parse_inbox_config(j, role_tag);
    s.validation = config::parse_validation_config(j);
    s.startup    = config::parse_startup_config(j, role_tag);
    s.monitoring = config::parse_monitoring_config(j);

    // ── Directional categories (always load both slots) ──────────────
    s.in_hub        = config::parse_hub_config(j, s.base_dir, "in");
    s.out_hub       = config::parse_hub_config(j, s.base_dir, "out");
    s.in_transport  = config::parse_transport_config(j, "in", role_tag);
    s.out_transport = config::parse_transport_config(j, "out", role_tag);
    s.in_shm        = config::parse_shm_config(j, "in");
    s.out_shm       = config::parse_shm_config(j, "out");
    s.in_validation  = config::parse_directional_validation(j, "in");
    s.out_validation = config::parse_directional_validation(j, "out");
    s.in_channel    = j.value("in_channel", std::string{});
    s.out_channel   = j.value("out_channel", std::string{});

    // ── Security check ───────────────────────────────────────────────
    RoleDirectory::warn_if_keyfile_in_role_dir(s.base_dir, s.auth.keyfile);

    // ── Role-specific extension ──────────────────────────────────────
    if (role_parser)
        s.role_data = role_parser(j, cfg);

    return cfg;
}
```

No role-specific branching. No `if (role_tag == "processor")`. Every role
gets both slots parsed. Empty fields stay at defaults.

### 5.4 RTTI safety of std::any

The `std::any` is:
- **Constructed** by the `RoleParser` callback (binary-side code)
- **Stored** in `Impl` (shared lib memory, but opaque — shared lib never
  touches the `std::any_cast`)
- **Cast** by `role_data<T>()` template (binary-side code, instantiated
  in the binary)

Both construction and cast use the same binary's `typeid(T)`. The shared
lib only stores the raw bytes. No RTTI mismatch possible.

## 7. Role-Specific Field Structs

Each role defines a lightweight struct for its unique fields. These live
in the binary source, not the shared lib.

### 6.1 ProducerFields

```cpp
// src/producer/producer_fields.hpp
struct ProducerFields
{
    nlohmann::json slot_schema_json;
    nlohmann::json flexzone_schema_json;
};

inline std::any parse_producer_fields(const nlohmann::json& j,
                                       const RoleConfig& /*cfg*/)
{
    ProducerFields pf;
    pf.slot_schema_json     = j.value("out_slot_schema", nlohmann::json{});
    pf.flexzone_schema_json = j.value("out_flexzone_schema", nlohmann::json{});
    if (pf.slot_schema_json.is_null() || pf.slot_schema_json.empty())
        throw std::runtime_error("producer: 'out_slot_schema' is required");
    return pf;
}
```

### 6.2 ConsumerFields

```cpp
// src/consumer/consumer_fields.hpp
struct ConsumerFields
{
    // Consumer has no unique fields beyond what RoleConfig provides.
    // This struct exists for future extensibility.
};

inline std::any parse_consumer_fields(const nlohmann::json& /*j*/,
                                       const RoleConfig& /*cfg*/)
{
    return ConsumerFields{};
}
```

### 6.3 ProcessorFields

```cpp
// src/processor/processor_fields.hpp
struct ProcessorFields
{
    nlohmann::json in_slot_schema_json;
    nlohmann::json out_slot_schema_json;
    nlohmann::json out_flexzone_schema_json;
};

inline std::any parse_processor_fields(const nlohmann::json& j,
                                        const RoleConfig& /*cfg*/)
{
    ProcessorFields pf;
    pf.in_slot_schema_json      = j.value("in_slot_schema", nlohmann::json{});
    pf.out_slot_schema_json     = j.value("out_slot_schema", nlohmann::json{});
    pf.out_flexzone_schema_json = j.value("out_flexzone_schema", nlohmann::json{});
    // Validation ...
    return pf;
}
```

## 8. Usage in main.cpp

```cpp
// producer_main.cpp
int run_producer(int argc, char** argv)
{
    auto args = role_cli::parse_role_args(argc, argv, "producer");

    auto config = RoleConfig::load_from_directory(
        args.role_dir, "producer", parse_producer_fields);

    // Auth
    if (!config.auth().keyfile.empty())
    {
        auto pw = scripting::get_role_password("producer", "Vault password: ");
        config.mutable_auth().load_keypair(
            config.identity().uid, *pw, "prod");
    }

    // Engine selection
    auto engine = (config.script().type == "lua")
        ? std::unique_ptr<ScriptEngine>(std::make_unique<LuaEngine>())
        : std::unique_ptr<ScriptEngine>(std::make_unique<PythonEngine>());

    if (config.script().type == "python" && !config.script().python_venv.empty())
        static_cast<PythonEngine*>(engine.get())->set_python_venv(
            config.script().python_venv);

    // Host
    ProducerRoleHost host;
    host.set_engine(std::move(engine));
    host.set_config(std::move(config));    // RoleConfig, not ProducerConfig
    host.set_shutdown_flag(&g_shutdown);
    host.startup_();
    // ...
}
```

## 9. Usage in role host

```cpp
void ProducerRoleHost::setup_infrastructure_()
{
    // Common accessors
    const auto& id   = config_.identity();
    const auto& hub  = config_.out_hub();
    const auto& tc   = config_.out_transport();
    const auto& shm  = config_.out_shm();
    const auto& val  = config_.out_validation();

    // Role-specific
    const auto& pf   = config_.role_data<ProducerFields>();

    // Use them
    out_messenger_.connect(hub.broker, id.uid, hub.broker_pubkey, ...);
    if (tc.transport == Transport::Shm)
        queue_ = make_shm_queue(shm.slot_count, shm.secret, ...);
    else
        queue_ = make_zmq_queue(tc.zmq_endpoint, tc.zmq_bind, ...);
    // ...
}
```

## 10. RoleVault Integration

`ActorVault` is a legacy name from the deleted `pylabhub-actor` binary.
Rename to `RoleVault` as part of this migration:

- `src/include/utils/actor_vault.hpp` → `src/include/utils/role_vault.hpp`
- `src/utils/security/actor_vault.cpp` → `src/utils/security/role_vault.cpp`
- Class: `pylabhub::utils::ActorVault` → `pylabhub::utils::RoleVault`
- All callers: `auth_config.cpp`, 3× `*_main.cpp`

## 11. Implementation Phases

### Phase 1 (DONE): Categorical config headers + shared parsers
- 7 headers in `src/include/utils/config/`
- Role configs delegate to shared parsers
- `config::AuthConfig::load_keypair()` shared impl

### Phase 2 (DONE): RoleDirectory::load_config() + typed accessors
- pImpl-backed ConfigState, categorical accessors
- 1273/1273 tests passing

### Phase 3: RoleConfig class
- Create `src/include/utils/config/role_config.hpp` (public header)
- Create `src/utils/config/role_config.cpp` (Impl, factory, accessors)
- Add to `pylabhub-utils` build
- Add `HubConfig`, `TransportConfig`, `ShmConfig`, `DirectionalValidationConfig`
  categorical headers (new directional parsers)
- Wire JsonConfig as backend (replace raw ifstream everywhere)
- L2 tests for RoleConfig::load() with all 3 role types

### Phase 4: Migrate role hosts + mains
- Role hosts take `RoleConfig` instead of ProducerConfig/etc.
- Access via `config_.identity().uid`, `config_.out_hub().broker`, etc.
- Mains use `RoleConfig::load()/load_from_directory()` with role parser
- `--init` templates updated to `in_`/`out_` field names

### Phase 5: Cleanup
- Remove `ProducerConfig`, `ConsumerConfig`, `ProcessorConfig` structs
- Remove `producer_config.cpp`, `consumer_config.cpp`, `processor_config.cpp`
- Remove `ProducerAuthConfig`, `ConsumerAuthConfig`, `ProcessorAuthConfig`
- `ActorVault` → `RoleVault` rename
- Update HEP-0024, HEP-0018, HEP-0015, README_Deployment.md

### Phase 6: RoleDirectory integration
- `RoleDirectory::load_config()` delegates to `RoleConfig::load()`
- Or: `RoleConfig::load_from_directory()` uses `RoleDirectory` internally
- Resolve the relationship — avoid two parallel config paths

## 12. Future Direction: Unified Role Binary

Since RoleConfig is now the single config class for all roles and most role
infrastructure (ScriptEngine, RoleHostCore, hub::Producer/Consumer, Messenger)
lives in `pylabhub-utils`, the three separate binaries (pylabhub-producer,
pylabhub-consumer, pylabhub-processor) could be unified into a single binary
that determines its role from config. The role_tag in RoleConfig already
selects the correct parsing and behavior. This would simplify deployment,
reduce build artifacts, and make the system more configuration-driven.

Prerequisites: ScriptEngine cleanup, config Phase 3-4 completion, RoleHostCore
encapsulation (CR-03). Evaluate after those are done.

## 13. Compatibility with Future Designs

### 11.1 NativeEngine
`ScriptConfig.type` expands to `{"python", "lua", "native"}`. Single
validation point in `parse_script_config()`.

### 11.2 Runtime config reload
`RoleConfig` uses `JsonConfig` internally. `reload_if_changed()` re-reads
the file, re-runs all parsers, updates cached state. Thread-safe.

### 11.3 Config-from-broker
`RoleConfig::load_from_json(json, role_tag, parser)` — same categorical
parsers, no file I/O. Enables broker-pushed config.

### 11.4 Build info / plugin ABI verification
`plh::build_info` from `engine_thread_model.md` §11 can be stored in
RoleConfig for native plugin compatibility checks at load time.
