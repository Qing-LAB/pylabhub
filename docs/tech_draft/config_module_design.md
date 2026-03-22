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

## 4. Categorical Config Structs

All structs are plain data holders in `src/include/utils/config/`. Each has
an inline `parse_*()` function — single source of truth for validation and
defaults.

### 4.1 IdentityConfig

```cpp
struct IdentityConfig
{
    std::string uid;
    std::string name;
    std::string log_level{"info"};
};
// parse_identity_config(json, role_tag) — reads <role_tag>.uid, .name, .log_level
```

### 4.2 HubConfig

```cpp
struct HubConfig
{
    std::string hub_dir;          // resolved absolute path (or empty)
    std::string broker;           // "tcp://127.0.0.1:5570"
    std::string broker_pubkey;    // Z85 CURVE pubkey (or empty)
};
// parse_hub_config(json, base_dir, direction) — reads <direction>_hub_dir,
// resolves broker endpoint + pubkey from hub directory
```

### 4.3 TransportConfig

```cpp
struct TransportConfig
{
    Transport   transport{Transport::Shm};
    std::string zmq_endpoint;
    bool        zmq_bind{true};
    size_t      zmq_buffer_depth{64};
    std::string zmq_overflow_policy{"drop"};
    std::string zmq_packing{"aligned"};
};
// parse_transport_config(json, direction, tag) — reads <direction>_transport,
// <direction>_zmq_endpoint, etc.
```

### 4.4 ValidationConfig (per-direction)

```cpp
struct DirectionalValidationConfig
{
    bool update_checksum{true};    // writer side
    bool verify_checksum{false};   // reader side
};
// parse_directional_validation(json, direction) — reads <direction>_update_checksum,
// <direction>_verify_checksum
```

Shared (non-directional) validation:

```cpp
struct ValidationConfig
{
    bool stop_on_script_error{false};
};
```

### 4.5 ShmConfig

```cpp
struct ShmConfig
{
    bool        enabled{true};
    uint64_t    secret{0};
    uint32_t    slot_count{8};
    hub::ConsumerSyncPolicy sync_policy{hub::ConsumerSyncPolicy::Sequential};
};
// parse_shm_config(json, direction) — reads <direction>_shm_enabled, etc.
```

### 4.6 ScriptConfig

```cpp
struct ScriptConfig
{
    std::string type{"python"};      // "python", "lua", "native" (future)
    std::string path{"."};           // resolved absolute
    std::string python_venv;
    bool        type_explicit{false};
};
// parse_script_config(json, base_dir, tag) — validates type ∈ {"python","lua"}
```

### 4.7 TimingConfig

```cpp
struct TimingConfig
{
    double           target_period_ms{0.0};
    double           target_rate_hz{0.0};
    int64_t          period_us{0};
    LoopTimingPolicy loop_timing{LoopTimingPolicy::MaxRate};
    double           queue_io_wait_timeout_ratio{kDefaultQueueIoWaitRatio};
    int              heartbeat_interval_ms{0};
};
// parse_timing_config(json, tag, default_period)
```

### 4.8 InboxConfig

```cpp
struct InboxConfig
{
    nlohmann::json schema_json;
    std::string    endpoint;
    size_t         buffer_depth{64};
    std::string    overflow_policy{"drop"};

    bool has_inbox() const noexcept;
};
```

### 4.9 StartupConfig

```cpp
struct StartupConfig
{
    std::vector<WaitForRole> wait_for_roles;
};
```

### 4.10 MonitoringConfig

```cpp
struct MonitoringConfig
{
    size_t ctrl_queue_max_depth{256};
    int    peer_dead_timeout_ms{30000};
};
```

### 4.11 AuthConfig

```cpp
struct PYLABHUB_UTILS_EXPORT AuthConfig
{
    std::string keyfile;
    std::string client_pubkey;
    std::string client_seckey;

    bool load_keypair(const std::string& uid, const std::string& password,
                      const char* role_tag);
};
```

## 5. RoleConfig Class

### 5.1 Public interface

```cpp
class PYLABHUB_UTILS_EXPORT RoleConfig
{
public:
    /// Role-specific parser callback.
    /// Receives the raw JSON and a reference to the partially-loaded RoleConfig
    /// (common fields already populated). Returns role-specific data as std::any.
    using RoleParser = std::function<std::any(const nlohmann::json&,
                                              const RoleConfig&)>;

    // ── Factory ───────────────────────────────────────────────────────

    /// Load from file path. Uses JsonConfig as backend.
    static RoleConfig load(const std::string& path,
                           const char* role_tag,
                           RoleParser role_parser = nullptr);

    /// Load from role directory.
    static RoleConfig load_from_directory(const std::string& dir,
                                          const char* role_tag,
                                          RoleParser role_parser = nullptr);

    // ── Common accessors (always available) ───────────────────────────

    const config::IdentityConfig&   identity()   const;
    const config::AuthConfig&       auth()       const;
    const config::ScriptConfig&     script()     const;
    const config::TimingConfig&     timing()     const;
    const config::InboxConfig&      inbox()      const;
    const config::ValidationConfig& validation() const;  // non-directional
    const config::StartupConfig&    startup()    const;
    const config::MonitoringConfig& monitoring() const;

    // ── Directional accessors (two slots each) ───────────────────────

    const config::HubConfig&        in_hub()        const;
    const config::HubConfig&        out_hub()       const;
    const config::TransportConfig&  in_transport()  const;
    const config::TransportConfig&  out_transport() const;
    const config::ShmConfig&        in_shm()        const;
    const config::ShmConfig&        out_shm()       const;
    const config::DirectionalValidationConfig& in_validation()  const;
    const config::DirectionalValidationConfig& out_validation() const;
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

## 6. Role-Specific Field Structs

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

## 7. Usage in main.cpp

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

## 8. Usage in role host

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

## 9. ActorVault → RoleVault Rename

`ActorVault` is a legacy name from the deleted `pylabhub-actor` binary.
Rename to `RoleVault` as part of this migration:

- `src/include/utils/actor_vault.hpp` → `src/include/utils/role_vault.hpp`
- `src/utils/security/actor_vault.cpp` → `src/utils/security/role_vault.cpp`
- Class: `pylabhub::utils::ActorVault` → `pylabhub::utils::RoleVault`
- All callers: `auth_config.cpp`, 3× `*_main.cpp`

## 10. Implementation Phases

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

## 11. Compatibility with Future Designs

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
