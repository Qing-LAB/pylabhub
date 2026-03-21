# Tech Draft: Unified Config Module via RoleDirectory

**Status**: Draft (2026-03-20)
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0024 (Role Directory Service), SE-14 (script.type validation)

## 1. Problem Statement

The current config system has three independent monolithic config structs
(ProducerConfig, ConsumerConfig, ProcessorConfig) with ~45-70 fields each.
Parsing logic is duplicated across their `from_json_file()` implementations:

| Duplicated block | Lines per copy | Copies |
|-----------------|---------------|--------|
| Script section (type, path, venv) | ~10 | 3 |
| Inbox section (schema, endpoint, depth, overflow) | ~18 | 3 |
| Startup wait_for_roles | ~28 | 3 |
| Timing/period resolution | ~35 | 3 |
| from_directory() flow | ~25 | 3 |
| Auth keyfile loading (main.cpp) | ~8 | 3 |

Adding a validation (e.g., SE-14's `script.type` check) requires editing
three files identically. No central place holds the truth of config schema,
defaults, or validation rules.

Meanwhile, `RoleDirectory` already owns path resolution, hub discovery, and
script entry-point logic — but stops short of JSON parsing. `JsonConfig`
provides thread-safe transactional file I/O — but is only used by `HubConfig`,
not by role configs.

## 2. Design: RoleDirectory as Unified Config Manager

### 2.1 Current architecture

```
role.json (on disk)
    ↓
std::ifstream + nlohmann::json::parse()    ← raw file read
    ↓
ProducerConfig::from_json_file()           ← monolithic parser (300+ lines)
    ↓
ProducerConfig struct                       ← flat bag of 50+ fields
    ↓
ProducerConfig::from_directory()           ← bolt-on: RoleDirectory path fixes
    ↓
main.cpp: host.set_config(std::move(cfg))  ← consumed once
```

### 2.2 Proposed architecture

```
RoleDirectory::open(base) or ::from_config_file(path)
    ↓
RoleDirectory::load_config("producer.json", "producer")
    ↓
    ├── JsonConfig (thread-safe, process-safe, transactional file IO)
    │   ├── FileLock (cross-process exclusive access during load)
    │   ├── std::shared_mutex (concurrent in-memory reads)
    │   └── Atomic writes (temp file + rename on persist)
    │       ↓
    │   raw nlohmann::json (cached in JsonConfig)
    │       ↓
    ├── Categorical parsers (shared, validated, single source of truth)
    │   ├── parse_identity_config(j, "producer")  → IdentityConfig
    │   ├── parse_hub_config(j, prefix, dir)      → RoleHubConfig
    │   │   └── RoleDirectory::resolve_hub_dir()  (integrated, not bolt-on)
    │   ├── parse_script_config(j, base_path)     → ScriptConfig
    │   ├── parse_timing_config(j)                → TimingConfig
    │   ├── parse_transport_config(j, prefix)     → TransportConfig
    │   ├── parse_shm_config(j, prefix)           → ShmConfig
    │   ├── parse_inbox_config(j)                 → InboxConfig
    │   ├── parse_validation_config(j)            → ValidationConfig
    │   ├── parse_startup_config(j)               → StartupConfig
    │   ├── parse_monitoring_config(j)            → MonitoringConfig
    │   └── parse_auth_config(j)                  → AuthConfig
    │       ↓
    └── Stored in RoleDirectory::ConfigState (pImpl)
            ↓
        Any component: dir.script(), dir.timing(), dir.hub(), ...
        Runtime update: dir.update<TimingConfig>([](auto& t) { ... })
            → JsonConfig::transaction(FullSync).write(...)
            → re-parse + re-validate category
```

**Key**: `JsonConfig` replaces raw `ifstream + json::parse()`. This gives
role configs the same thread-safety, process-safety, and transactional
guarantees that `HubConfig` already has. Runtime config reload and
persistence become available without additional plumbing.

### 2.3 Core principle

**RoleDirectory is the single source of truth for all role configuration.**
It owns the directory, the config file, the parsed state, and the path
resolution. No separate config struct exists. Components query the directory
for what they need.

## 3. Categorical Config Structs

Each struct is a plain data holder with no methods beyond convenience
queries. All parsing, validation, and defaulting happens in the
corresponding `parse_*()` inline function.

### 3.1 IdentityConfig

```cpp
struct IdentityConfig
{
    std::string uid;            // e.g., "PROD-TEMPSENS-12345678"
    std::string name;           // human-readable name
    std::string log_level;      // "info", "debug", "warn", "error"
};
```

Parser takes a role tag ("producer", "consumer", "processor") to derive
the JSON field names (`producer_uid`, `consumer_uid`, etc.).

### 3.2 RoleHubConfig

```cpp
struct RoleHubConfig
{
    std::string hub_dir;          // resolved absolute path (or empty)
    std::string broker;           // "tcp://127.0.0.1:5570"
    std::string broker_pubkey;    // Z85-encoded CURVE public key (or empty)
};
```

Parser integrates hub resolution: if `hub_dir` is set, automatically calls
`RoleDirectory::resolve_hub_dir()`, `hub_broker_endpoint()`, and
`hub_broker_pubkey()`. No separate "bolt-on" step needed.

For Processor dual-broker, the directory stores two instances:
`hub(Direction::In)` and `hub(Direction::Out)`.

### 3.3 ScriptConfig

```cpp
struct ScriptConfig
{
    std::string type{"python"};     // "python", "lua", or "native" (future)
    std::string path{"."};          // resolved absolute path to script dir
    std::string python_venv;        // venv name (Python only)
    bool        type_explicit{false}; // true if "type" was in JSON
};
```

Parser validates `type ∈ {"python", "lua"}` (SE-14 fix — single location).
Path resolution integrated: relative paths resolved against directory base.

### 3.4 TimingConfig

```cpp
struct TimingConfig
{
    double              target_period_ms{0.0};
    double              target_rate_hz{0.0};
    int64_t             period_us{0};           // resolved from above
    LoopTimingPolicy    loop_timing{LoopTimingPolicy::MaxRate};
    double              queue_io_wait_timeout_ratio{kDefaultQueueIoWaitRatio};
    int                 slot_acquire_timeout_ms{-1};  // deprecated alias
    int                 heartbeat_interval_ms{5000};
};
```

Parser calls existing shared helpers: `resolve_period_us()`,
`parse_loop_timing_policy()`, `default_loop_timing_policy()`.

### 3.5 TransportConfig

```cpp
struct TransportConfig
{
    Transport   transport{Transport::Shm};
    std::string zmq_endpoint;
    bool        zmq_bind{true};
    uint32_t    zmq_buffer_depth{hub::kZmqDefaultBufferDepth};
    std::string zmq_packing{"aligned"};
    std::string zmq_overflow_policy{"drop"};
};
```

Parser is parameterized by direction prefix for JSON field names.
Producer uses no prefix (flat fields). Processor uses `"in_"` / `"out_"`
prefixes. Consumer uses `queue_type` as the transport field name.

### 3.6 ShmConfig

```cpp
struct ShmConfig
{
    bool        enabled{true};
    uint64_t    secret{0};
    uint32_t    slot_count{8};              // writer-side only
    ConsumerSyncPolicy sync_policy{ConsumerSyncPolicy::Sequential};  // writer-side only
};
```

Parser handles flat (producer/consumer) and nested (processor `shm.in`/`shm.out`).

### 3.7 InboxConfig

```cpp
struct InboxConfig
{
    nlohmann::json schema_json;              // object or string (named ref)
    std::string    endpoint;
    uint32_t       buffer_depth{hub::kZmqDefaultBufferDepth};
    std::string    overflow_policy{"drop"};

    bool has_inbox() const { return !schema_json.is_null(); }
};
```

Single parser, used by all 3 roles identically.

### 3.8 ValidationConfig

```cpp
struct ValidationConfig
{
    bool update_checksum{true};           // writer-side only
    bool verify_checksum{false};          // reader-side only
    bool stop_on_script_error{false};
};
```

Parser reads all fields; each role uses only the relevant subset.

### 3.9 StartupConfig

```cpp
struct StartupConfig
{
    std::vector<WaitForRole> wait_for_roles;
};
```

Parser extracted from the identical 28-line block duplicated 3×.

### 3.10 MonitoringConfig

```cpp
struct MonitoringConfig
{
    uint32_t ctrl_queue_max_depth{1024};
    int      peer_dead_timeout_ms{30000};
};
```

### 3.11 AuthConfig

```cpp
struct AuthConfig
{
    std::string keyfile;
    std::string client_pubkey;   // populated by load_keypair()
    std::string client_seckey;   // populated by load_keypair()

    void load_keypair(const std::string& uid, const std::string& password);
};
```

## 4. RoleDirectory Extended Interface

```cpp
class RoleDirectory
{
public:
    // ── Existing (unchanged) ──────────────────────────────────────────
    static RoleDirectory open(const std::filesystem::path& base);
    static RoleDirectory from_config_file(const std::filesystem::path& config_path);
    static RoleDirectory create(const std::filesystem::path& base);

    const std::filesystem::path& base() const noexcept;
    std::filesystem::path logs() const;
    std::filesystem::path run() const;
    std::filesystem::path vault() const;
    std::filesystem::path config_file(std::string_view filename) const;
    std::filesystem::path script_entry(std::string_view script_path,
                                        std::string_view type) const;
    // ... hub resolution, security helpers, layout inspection ...

    // ── New: config loading ───────────────────────────────────────────

    /// Load and parse a role config file. Calls all categorical parsers,
    /// integrates hub resolution and path normalization.
    /// @param filename  Config file name, e.g. "producer.json".
    /// @param role_tag  Role type: "producer", "consumer", "processor".
    /// @throws std::runtime_error on parse/validation error.
    void load_config(std::string_view filename, std::string_view role_tag);

    /// True after load_config() succeeds.
    bool config_loaded() const noexcept;

    // ── New: typed config accessors ───────────────────────────────────

    const IdentityConfig&    identity()    const;
    const ScriptConfig&      script()      const;
    const TimingConfig&      timing()      const;
    const InboxConfig&       inbox()       const;
    const ValidationConfig&  validation()  const;
    const StartupConfig&     startup()     const;
    const MonitoringConfig&  monitoring()  const;
    const AuthConfig&        auth()        const;

    /// Hub config. For processor dual-broker, use hub(Direction).
    const RoleHubConfig&     hub() const;
    const RoleHubConfig&     hub(Direction d) const;

    /// Transport config. For processor, use transport(Direction).
    const TransportConfig&   transport() const;
    const TransportConfig&   transport(Direction d) const;

    /// SHM config. For processor, use shm(Direction).
    const ShmConfig&         shm() const;
    const ShmConfig&         shm(Direction d) const;

    /// Channel name(s). Producer/consumer have one, processor has two.
    const std::string&       channel() const;           // producer/consumer
    const std::string&       channel(Direction d) const; // processor

    /// Raw JSON access for extension/custom fields.
    const nlohmann::json&    raw() const;

private:
    std::filesystem::path base_;

    // JsonConfig backend — thread-safe, process-safe, transactional
    JsonConfig json_config_;

    // Parsed config state (populated by load_config)
    struct ConfigState;
    std::unique_ptr<ConfigState> config_;
};
```

### 4.1 ConfigState (pImpl)

```cpp
struct RoleDirectory::ConfigState
{
    nlohmann::json    raw;

    IdentityConfig    identity;
    RoleHubConfig     hub;
    RoleHubConfig     in_hub;      // processor only
    RoleHubConfig     out_hub;     // processor only
    ScriptConfig      script;
    TimingConfig      timing;
    TransportConfig   transport;
    TransportConfig   in_transport;  // processor only
    TransportConfig   out_transport; // processor only
    ShmConfig         shm;
    ShmConfig         in_shm;        // processor only
    ShmConfig         out_shm;       // processor only
    InboxConfig       inbox;
    ValidationConfig  validation;
    StartupConfig     startup;
    MonitoringConfig  monitoring;
    AuthConfig        auth;

    std::string       channel;
    std::string       in_channel;    // processor only
    std::string       out_channel;   // processor only

    std::string       role_tag;      // "producer", "consumer", "processor"
};
```

### 4.2 load_config() implementation

```cpp
void RoleDirectory::load_config(std::string_view filename, std::string_view role_tag)
{
    namespace fs = std::filesystem;

    // Use JsonConfig for thread-safe, process-safe file access.
    // ReloadFirst ensures we read the latest on-disk state.
    json_config_ = JsonConfig(config_file(filename), /*createIfMissing=*/false);

    auto state = std::make_unique<ConfigState>();
    state->role_tag = std::string(role_tag);

    json_config_.transaction(AccessFlags::ReloadFirst).read([&](const json& j) {
    state->raw = j;

    // Parse all categories (single source of truth for each)
    state->identity   = parse_identity_config(j, role_tag);
    state->script     = parse_script_config(j, base_);   // resolves path against base
    state->timing     = parse_timing_config(j);
    state->inbox      = parse_inbox_config(j);
    state->validation = parse_validation_config(j);
    state->startup    = parse_startup_config(j);
    state->monitoring = parse_monitoring_config(j);
    state->auth       = parse_auth_config(j);

    // Hub resolution (integrated — no bolt-on step)
    if (role_tag == "processor")
    {
        state->in_hub  = parse_hub_config(j, "in_", *this);
        state->out_hub = parse_hub_config(j, "out_", *this);
        state->hub     = parse_hub_config(j, "", *this);  // fallback
        state->in_channel  = j.value("in_channel", std::string{});
        state->out_channel = j.value("out_channel", std::string{});
        state->in_transport  = parse_transport_config(j, "in_");
        state->out_transport = parse_transport_config(j, "out_");
        state->in_shm  = parse_shm_config(j, "in_");
        state->out_shm = parse_shm_config(j, "out_");
    }
    else
    {
        state->hub       = parse_hub_config(j, "", *this);
        state->channel   = j.value("channel", std::string{});
        state->transport = parse_transport_config(j, "");
        state->shm       = parse_shm_config(j, "");
    }

    // Security check
    warn_if_keyfile_in_role_dir(base_, state->auth.keyfile);

    }); // end transaction

    config_ = std::move(state);
}
```

Runtime updates use the transactional write path:

```cpp
template <typename T, typename Fn>
void RoleDirectory::update(Fn&& fn)
{
    json_config_.transaction(AccessFlags::FullSync).write([&](json& j) {
        // Re-parse the category from current JSON, apply user mutation, validate
        T category = parse_category<T>(j);
        fn(category);
        // Write mutated category back to JSON
        serialize_category(j, category);
        // Update cached state
        set_cached<T>(std::move(category));
    });
}
```

## 5. Categorical Parsers — Header Location

Following the existing pattern (inline functions in headers next to their
struct/enum definitions):

| Parser | Header | Struct defined in |
|--------|--------|-------------------|
| `parse_identity_config()` | `config/identity_config.hpp` | same |
| `parse_hub_config()` | `config/hub_config.hpp` (role-level, not broker-level) | same |
| `parse_script_config()` | `config/script_config.hpp` | same |
| `parse_timing_config()` | `loop_timing_policy.hpp` (extends existing) | same |
| `parse_transport_config()` | `config/transport_config.hpp` | same |
| `parse_shm_config()` | `config/shm_config.hpp` | same |
| `parse_inbox_config()` | `config/inbox_config.hpp` | same |
| `parse_validation_config()` | `config/validation_config.hpp` | same |
| `parse_startup_config()` | `startup_wait.hpp` (extends existing) | same |
| `parse_monitoring_config()` | `config/monitoring_config.hpp` | same |
| `parse_auth_config()` | `config/auth_config.hpp` | same |

All new headers go in `src/include/utils/config/` alongside the existing
`role_directory.hpp`.

## 6. Migration Path

### Phase 1: Extract categorical structs + parsers (no behavior change)
- Create `config/*.hpp` headers with structs and inline parsers
- Each parser is tested independently (L2 unit tests)
- Existing role configs still work — no callers changed yet

### Phase 2: Wire RoleDirectory::load_config()
- Add `load_config()`, `ConfigState`, and typed accessors to RoleDirectory
- Add L2 tests for `load_config()` with each role type

### Phase 3: Migrate role hosts to use RoleDirectory
- Role hosts take `const RoleDirectory&` instead of `RoleConfig`
- Access fields via `dir.script().type`, `dir.timing().period_us`, etc.
- Old config structs become thin wrappers (or removed)

### Phase 4: Migrate main.cpp
- `main()` creates `RoleDirectory`, calls `load_config()`, passes to host
- Remove `ProducerConfig::from_json_file()` / `from_directory()`
- Remove monolithic config structs

### Phase 5: Cleanup
- Remove old config .cpp files
- Update HEP-0024 to reflect expanded RoleDirectory scope
- Update README_Deployment.md field reference tables

## 7. Compatibility with Future Designs

### 7.1 NativeEngine (§11 of engine_thread_model.md)
`ScriptConfig.type` expands to `{"python", "lua", "native"}`. Single
validation point in `parse_script_config()`.

### 7.2 Runtime config updates
`RoleDirectory` uses `JsonConfig` internally as its storage backend.
This means runtime config updates are already supported through JsonConfig's
transactional API. The typed accessors hide this — callers don't know or
care about the storage backend.

### 7.3 Config-from-broker (remote config)
A future feature where the broker pushes config to roles. `RoleDirectory`
could accept a JSON blob via `load_config_from_json(json)` using the same
categorical parsers. No separate code path needed.

### 7.4 Build info integration
`plh::build_info` (from `engine_thread_model.md` §11.11) can be included in
`RoleDirectory` for native plugin ABI verification. The directory already
knows the script type — it can check plugin compatibility at load time.

## 8. Impact on Existing Codebase

### Files eliminated (Phase 4)
- `src/producer/producer_config.cpp` (~330 lines)
- `src/consumer/consumer_config.cpp` (~285 lines)
- `src/processor/processor_config.cpp` (~450 lines)
- Total: ~1065 lines of duplicated parsing removed

### Files created (Phase 1)
- `src/include/utils/config/identity_config.hpp`
- `src/include/utils/config/hub_role_config.hpp`
- `src/include/utils/config/script_config.hpp`
- `src/include/utils/config/transport_config.hpp`
- `src/include/utils/config/shm_config.hpp`
- `src/include/utils/config/inbox_config.hpp`
- `src/include/utils/config/validation_config.hpp`
- `src/include/utils/config/monitoring_config.hpp`
- `src/include/utils/config/auth_config.hpp`

### Files modified
- `src/include/utils/role_directory.hpp` — add `load_config()`, `ConfigState`, accessors
- `src/utils/config/role_directory.cpp` — implement `load_config()`
- `src/include/utils/startup_wait.hpp` — add `parse_startup_config()` inline
- `src/include/utils/loop_timing_policy.hpp` — add `parse_timing_config()` inline
- All 3 `*_role_host.cpp` — read from `dir.script()` etc. instead of `config_.*`
- All 3 `*_main.cpp` — simplified: create dir, load config, pass dir to host
- Test files: new L2 tests for each parser, update existing config tests

### HEP documents
- **HEP-CORE-0024**: Update to document expanded RoleDirectory scope (config
  loading, typed accessors, categorical parsing)
- **HEP-CORE-0018**: Update config section references
- **HEP-CORE-0015**: Update config section references
- **README_Deployment.md**: Update field reference tables to use categorical
  section names

## 9. Test Strategy

### L2 unit tests (per category)
Each `parse_*_config()` gets its own test file:
- Valid defaults, valid explicit values, missing optional fields
- Invalid values (wrong type, out of range, unknown enum)
- Edge cases (empty strings, zero values, negative timeouts)

### L2 integration tests (RoleDirectory::load_config)
- Load a minimal producer.json → verify all categories populated
- Load a full processor.json with dual-broker → verify direction routing
- Invalid JSON → verify clear error messages
- Missing required fields → verify throws

### Regression
- Existing L4 integration tests continue to pass (behavior unchanged)
- Config test suites verify same field values as before migration
