Below is a focused, critical review of the C/C++ under `cpp/src`, with per‑module summaries, API/design notes, duplicated/obsolete/unused elements, and HEP consistency findings. I’ve cited concrete code locations for each issue.

## Findings (ordered by severity)

- **Actor broker config is parsed but never used.** Each role has a `broker` field, yet neither producer nor consumer options use it, so multi‑broker configs silently collapse to the default broker.  
```142:149:cpp/src/actor/actor_config.hpp
struct RoleConfig
{
    enum class Kind { Producer, Consumer };

    Kind        kind{Kind::Producer};
    std::string channel;
    std::string broker{"tcp://127.0.0.1:5570"};
```
```415:419:cpp/src/actor/actor_host.cpp
    hub::ProducerOptions opts;
    opts.channel_name = role_cfg_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = role_cfg_.has_shm;
```
```835:839:cpp/src/actor/actor_host.cpp
    hub::ConsumerOptions opts;
    opts.channel_name      = role_cfg_.channel;
    opts.shm_shared_secret = role_cfg_.has_shm ? role_cfg_.shm_secret : 0U;
```

- **`--validate` and `--keygen` are documented but effectively stubs.** `--validate` prints “Validation passed” without calling the layout printer/validator; `--keygen` is a placeholder.  
```123:134:cpp/src/actor/actor_main.cpp
    std::cout
        << "Usage:\n"
        << "  " << prog << " --config <path.json> [--validate | --list-roles | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --config <path>   Path to actor JSON config (required)\n"
        << "  --validate        Validate script and print layout; exit 0 on success\n"
        << "  --list-roles      Show configured roles and activation status; exit 0\n"
        << "  --keygen          Generate actor NaCl keypair at auth.keyfile path; exit 0\n"
```
```263:267:cpp/src/actor/actor_main.cpp
    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        return 0;
    }
```
```218:224:cpp/src/actor/actor_main.cpp
        std::cout << "Keypair generation for actor '" << config.actor_uid << "'\n"
                  << "Target file: " << config.auth.keyfile << "\n"
                  << "(keypair generation not yet implemented — "
                     "see SECURITY_TODO Phase 5)\n";
```

- **HEP‑CORE‑0005 script abstraction is not implemented.** The HEP specifies `IScriptEngine`, but the current system exposes only a singleton `PythonInterpreter::exec()` with no `IScriptEngine`/`IScriptContext` abstraction or lifecycle methods.  
```58:127:cpp/docs/HEP/HEP-CORE-0005-script-interface-framework.md
### 1. `IScriptEngine` Interface
...
class IScriptEngine {
public:
    virtual ~IScriptEngine() = default;
    virtual bool initialize(std::shared_ptr<IScriptContext> context) = 0;
    virtual bool load_initialization_script(const std::string& script_content, const std::string& script_name = "init_script") = 0;
    ...
    virtual ScriptValue call_function(const std::string& function_name, const std::vector<ScriptValue>& args) = 0;
    virtual bool register_cpp_function(const std::string& script_func_name, std::function<ScriptValue(const std::vector<ScriptValue>&)> cpp_func) = 0;
```
```90:101:cpp/src/hub_python/python_interpreter.hpp
    /**
     * @brief Executes Python source code in the persistent namespace.
     */
    PyExecResult exec(const std::string& code);
```

- **AdminShell executes arbitrary code with minimal input hardening.** The token is validated only after JSON parsing and there is no `code` size limit.  
```137:163:cpp/src/hub_python/admin_shell.cpp
        req = nlohmann::json::parse(raw);
...
        if (!token.empty())
        {
            if (req.value("token", "") != token)
            {
                LOGGER_WARN("AdminShell: rejected request — invalid token");
                return error_reply("unauthorized");
            }
        }
...
        const std::string code = req["code"].get<std::string>();
```

- **DataBlock “structure remap” APIs are public but unimplemented** (runtime errors), which is risky API surface.  
```832:868:cpp/src/include/utils/data_block.hpp
    // ─── Structure Re-Mapping API (Placeholder - Future Feature) ───
    [[nodiscard]] uint64_t request_structure_remap(
        const std::optional<schema::SchemaInfo> &new_flexzone_schema,
        const std::optional<schema::SchemaInfo> &new_datablock_schema
    );
...
    void commit_structure_remap(
        uint64_t request_id,
        const std::optional<schema::SchemaInfo> &new_flexzone_schema,
        const std::optional<schema::SchemaInfo> &new_datablock_schema
    );
```
```2009:2036:cpp/src/utils/data_block.cpp
uint64_t DataBlockProducer::request_structure_remap(...)
{
    ...
    throw std::runtime_error(
        "DataBlockProducer::request_structure_remap: "
        "Structure remapping requires broker coordination - not yet implemented. ");
}
...
void DataBlockProducer::commit_structure_remap(...)
{
    ...
    throw std::runtime_error(
        "DataBlockProducer::commit_structure_remap: "
        "Structure remapping requires broker coordination - not yet implemented. ");
}
```

- **HEP‑CORE‑0002 is out of date in at least one key place.** It states `register_consumer` is a stub, but Messenger implements it with request/response.  
```32:36:cpp/docs/HEP/HEP-CORE-0002-DataHub-FINAL.md
- **Implemented:** ... `register_producer`; synchronous `discover_producer` ...; `register_consumer` stub pending protocol.
```
```639:688:cpp/src/utils/messenger.cpp
bool handle_command(RegisterConsumerCmd &cmd,
                    std::optional<zmq::socket_t> &socket) const
{
    ...
    const std::string msg_type   = "CONSUMER_REG_REQ";
    ...
    if (response.value("status", "") != "success")
    {
        LOGGER_ERROR("Messenger: register_consumer('{}') failed: {}", cmd.channel,
                     response.value("message", std::string("unknown")));
    }
}
```

- **Duplicated logic likely to drift:** ChannelPattern string conversion is implemented twice in different modules.  
```93:109:cpp/src/utils/messenger.cpp
constexpr const char *pattern_to_wire(ChannelPattern p) noexcept
{
    switch (p)
    {
    case ChannelPattern::Pipeline: return "Pipeline";
    case ChannelPattern::Bidir:    return "Bidir";
    default:                       return "PubSub";
    }
}
...
ChannelPattern pattern_from_wire(const std::string &s) noexcept
```
```32:49:cpp/src/utils/broker_service.cpp
constexpr const char* pattern_to_str(ChannelPattern p) noexcept
{
    switch (p)
    {
    case ChannelPattern::Pipeline: return "Pipeline";
    case ChannelPattern::Bidir:    return "Bidir";
    default:                       return "PubSub";
    }
}
...
ChannelPattern pattern_from_str(const std::string& s) noexcept
```

- **Unused / misleading fields and APIs:**  
  - `PyExecResult::result_repr` is declared but never set.  
```45:50:cpp/src/hub_python/python_interpreter.hpp
struct PyExecResult
{
    bool        success{false};
    std::string output;
    std::string error;
    std::string result_repr;     ///< repr() of the last expression value, if any.
};
```
  - `g_py_initialized` is only written, never read.  
```26:28:cpp/src/hub_python/python_interpreter.cpp
static std::atomic<bool>    g_py_initialized{false};
```
```201:208:cpp/src/hub_python/python_interpreter.cpp
    g_py_initialized.store(true, std::memory_order_release);
...
    g_py_initialized.store(false, std::memory_order_release);
```
  - `_registered_roles()` omits `on_stop` / `on_stop_c`, so it under‑reports registered handlers.  
```182:193:cpp/src/actor/actor_module.cpp
    m.def("_registered_roles",
          [](const std::string &event) -> py::list
          {
              ...
              if (event == "on_write")   map = &tbl.on_write;
              else if (event == "on_read")    map = &tbl.on_read;
              else if (event == "on_init")    map = &tbl.on_init;
              else if (event == "on_data")    map = &tbl.on_data;
              else if (event == "on_message") map = &tbl.on_message;
```

- **Schema validation in actor JSON is shallow.** The `numpy_array` dtype is accepted as an arbitrary string and shapes allow any integer (including negative or 0).  
```20:44:cpp/src/actor/actor_schema.cpp
    if (expose_as == "numpy_array")
    {
        spec.exposure = SlotExposure::NumpyArray;
        ...
        spec.numpy_dtype = schema_obj["dtype"].get<std::string>();
        if (schema_obj.contains("shape") && schema_obj["shape"].is_array())
        {
            for (const auto &dim : schema_obj["shape"])
            {
                if (!dim.is_number_integer())
                {
                    throw std::runtime_error(
                        "Schema: 'shape' entries must be integers");
                }
                spec.numpy_shape.push_back(dim.get<int64_t>());
            }
        }
```

- **Duplicate control‑flow logic in actor write loops.** The trigger/interval handling block is repeated in SHM and ZMQ loops.  
```665:675:cpp/src/actor/actor_host.cpp
        if (role_cfg_.interval_ms == -1)
        {
            std::unique_lock<std::mutex> lock(trigger_mu_);
            trigger_cv_.wait(lock, [this]
            {
                return trigger_pending_ || !running_.load() || shutdown_.load();
            });
            trigger_pending_ = false;
            if (!running_.load() || shutdown_.load())
                break;
        }
```
```722:731:cpp/src/actor/actor_host.cpp
        if (role_cfg_.interval_ms == -1)
        {
            std::unique_lock<std::mutex> lock(trigger_mu_);
            trigger_cv_.wait(lock, [this]
            {
                return trigger_pending_ || !running_.load() || shutdown_.load();
            });
            trigger_pending_ = false;
            if (!running_.load() || shutdown_.load())
                break;
        }
```

## Module summaries (API design + code quality)

### `actor/*`
- **Design:** Clean separation of config parsing, schema handling, and runtime workers. API surface is reasonable for Python callbacks, but critical config (`broker`) isn’t wired into runtime options.  
- **Quality:** Good error messages and use of pybind; however `--validate` and `--keygen` are UX traps (documented but no‑op) and duplicated wait logic suggests consolidation opportunities.  
- **Obsolete/unused:** legacy flat format remains (deprecated) and `--validate` does not call layout printing.  
```150:155:cpp/src/actor/actor_config.cpp
/// Parse the legacy single-role flat format, wrapping it as a single-entry
/// roles map. The role name is "<kind>:<channel>".
ActorConfig parse_legacy_flat(const nlohmann::json &j, const std::string &path)
{
    LOGGER_WARN("[actor] config '{}': flat single-role format is deprecated. "
```

### `hub_python/*` + `hubshell.cpp`
- **Design:** Provides a simple embedded Python shell and admin RPC. However it diverges from the HEP‑0005 script abstraction (no `IScriptEngine`, no `IScriptContext`, no structured calls).  
- **Quality:** Threading and GIL usage are generally careful, but there are several missed safeguards: unbounded `code` input, token checked after parse, unused fields (`result_repr`, `g_py_initialized`), and no wrapper around `channels` callback exceptions.  
```162:169:cpp/src/hub_python/pylabhub_module.cpp
    m.def("channels", []() -> py::list
    {
        py::list result;
        if (pylabhub::hub_python::g_channels_cb)
        {
            for (auto& d : pylabhub::hub_python::g_channels_cb())
                result.append(d);
        }
        return result;
    },
```
```129:151:cpp/src/hub_python/python_interpreter.cpp
        auto sys       = py::module_::import("sys");
        auto old_out   = sys.attr("stdout");
        auto old_err   = sys.attr("stderr");
        auto buf       = io.attr("StringIO")();
        sys.attr("stdout") = buf;
        sys.attr("stderr") = buf;
...
        sys.attr("stdout") = old_out;
        sys.attr("stderr") = old_err;
        result.output = buf.attr("getvalue")().cast<std::string>();
```
```153:156:cpp/src/hub_python/python_interpreter.cpp
    catch (const std::exception& e)
    {
        result.success = false;
        result.error   = e.what();
    }
```

### `utils/data_block*`, `slot_*`, `shared_memory_*`
- **Design:** DataBlock and slot APIs are comprehensive and mostly aligned with HEP‑0002. However, the public remap API is a placeholder that throws at runtime and is still surfaced in headers.  
- **Quality:** The core validation path looks disciplined, but placeholder APIs should be marked `[[deprecated]]` or hidden to avoid production use.  
- **HEP tie‑in:** HEP explicitly notes incomplete broker/schema features; the remap APIs appear to anticipate those features but are not implemented.

### `utils/messenger*` + `utils/broker_service*`
- **Design:** Async command queue and broker client/server separation are good.  
- **Quality:** Duplication of `ChannelPattern` conversion appears in both Messenger and BrokerService (risk of drift).  
- **HEP tie‑in:** HEP‑0002 says `register_consumer` is a stub; code now has a full handshake. The doc needs update.

### `utils/logger*`, `utils/file_lock*`, `utils/lifecycle*`
- **Design:** Largely consistent with HEP‑0001/0003/0004. I didn’t find clear inconsistencies here in this pass.  
- **Quality:** No obvious regressions found, but I didn’t deep‑audit sink rotation and OS‑specific error paths.

### Other utility headers (`plh_*`, `schema_blds.hpp`, `format_tools.hpp`, etc.)
- **Design:** Umbrella headers and BLDS schema utilities are clear and appear aligned with their HEP references. No major issues found in quick scan.

## HEP consistency snapshot

- **HEP‑CORE‑0005 (Script Interface):** Not implemented in `hub_python` (no `IScriptEngine`/`IScriptContext`, no lifecycle methods).  
- **HEP‑CORE‑0002 (DataHub):** Document says `register_consumer` is stubbed, but Messenger implements it; doc needs sync.  
- **HEP‑CORE‑0006 (Slot‑Processor API):** The `hub_producer.hpp`/`hub_consumer.hpp` APIs look consistent with the HEP (push/pull, realtime handlers), no obvious mismatches.  
- **HEP‑CORE‑0001/0003/0004:** No clear divergences detected in this review.

## Suggested next steps (if you want fixes)
- Wire `RoleConfig::broker` into `ProducerOptions` / `ConsumerOptions` so multi‑broker configs work.  
- Make `--validate` actually validate and print layouts (or remove the flag).  
- Either implement `IScriptEngine` per HEP‑0005 or update the HEP to match the current design.  
- Deprecate or hide DataBlock remap placeholders until implemented.  
- De‑duplicate `ChannelPattern` string conversion to a shared utility.

If you want, I can go deeper into any specific module (e.g., slot recovery, integrity validator, JSON config atomic writes) and produce a narrower audit or test plan.