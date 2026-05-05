/**
 * @file python_engine.cpp
 * @brief PythonEngine — ScriptEngine implementation for CPython via pybind11.
 *
 * GIL is held for the engine's entire lifetime on the worker thread.
 * py::scoped_interpreter acquires it at creation; each invoke_*() uses
 * py::gil_scoped_acquire which is reentrant (no-op on the owning thread).
 * Cross-thread script execution is handled via the engine's request queue
 * (see docs/tech_draft/engine_thread_model.md §6).
 *
 * Ported from the legacy PythonRoleHostBase / ProducerScriptHost /
 * ConsumerScriptHost / ProcessorScriptHost monolithic implementations.
 */
#include "python_engine.hpp"

#include "utils/format_tools.hpp"

#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "utils/naming.hpp"
#include "json_py_helpers.hpp"   // detail::json_to_py / detail::py_to_json
#include "python_helpers.hpp"

#include "plh_platform.hpp"

// Role API headers are in sibling directories. When python_engine.cpp is compiled
// as part of a target that adds src/ or src/{producer,consumer,processor} to its
// include path, these resolve directly. When compiled from src/scripting/ with
// only ${CMAKE_CURRENT_SOURCE_DIR} as an include path, the relative paths work.
#include "../producer/producer_api.hpp"
#include "../consumer/consumer_api.hpp"
#include "../processor/processor_api.hpp"
#include "utils/hub_api.hpp"   // build_api_(HubAPI&) needs full type

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace py = pybind11;
namespace fs = std::filesystem;

namespace pylabhub::scripting
{

// json_to_py / py_to_json moved to src/scripting/json_py_helpers.hpp
// (S5 extraction) so role API metrics() bindings + hub_api_python.cpp
// can share the fast path instead of round-tripping through json.loads
// / json.dumps.  Local using-decls keep call sites unchanged.
using detail::json_to_py;
using detail::py_to_json;

// ============================================================================
// Destructor
// ============================================================================

PythonEngine::PythonEngine() = default;

PythonEngine::~PythonEngine()
{
    finalize();
}

// ============================================================================
// resolve_python_home — 3-tier Python home resolution (same as python_script_host.cpp)
// ============================================================================

static fs::path resolve_python_home_for_engine(const fs::path &exe_path)
{
    const fs::path prefix = fs::weakly_canonical(exe_path.parent_path() / "..");

    // --- Tier 1: Environment variable override ---
    const char *env_home = std::getenv("PYLABHUB_PYTHON_HOME");
    if (env_home && *env_home)
    {
        fs::path home(env_home);
        if (home.is_relative())
            home = fs::weakly_canonical(prefix / home);
        if (fs::is_directory(home))
        {
            LOGGER_INFO("PythonEngine: Python home from $PYLABHUB_PYTHON_HOME: '{}'",
                        home.string());
            return home;
        }
        LOGGER_WARN("PythonEngine: $PYLABHUB_PYTHON_HOME='{}' is not a directory — "
                    "falling through to config file",
                    home.string());
    }

    // --- Tier 2: System config file ---
    const fs::path config_file = prefix / "config" / "pylabhub.json";
    if (fs::is_regular_file(config_file))
    {
        try
        {
            std::ifstream ifs(config_file);
            const auto    j = nlohmann::json::parse(ifs);

            if (j.contains("python_home") && j["python_home"].is_string())
            {
                const auto val = j["python_home"].get<std::string>();
                if (!val.empty())
                {
                    fs::path home(val);
                    if (home.is_relative())
                        home = fs::weakly_canonical(prefix / home);
                    else
                        home = fs::weakly_canonical(home);
                    if (fs::is_directory(home))
                    {
                        LOGGER_INFO("PythonEngine: Python home from '{}': '{}'",
                                    config_file.string(), home.string());
                        return home;
                    }
                    LOGGER_WARN("PythonEngine: python_home='{}' from config is not "
                                "a directory — falling through to standalone",
                                home.string());
                }
            }
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGGER_WARN("PythonEngine: failed to parse '{}': {} — falling through",
                        config_file.string(), e.what());
        }
    }

    // --- Tier 3: Standalone default ---
    const fs::path standalone = prefix / "opt" / "python";
    if (fs::is_directory(standalone))
    {
        LOGGER_INFO("PythonEngine: Python home (standalone): '{}'",
                    standalone.string());
        return standalone;
    }

    // --- All tiers exhausted ---
    throw std::runtime_error(
        "PythonEngine: cannot locate a Python installation.\n"
        "  Checked:\n"
        "    1. $PYLABHUB_PYTHON_HOME (not set or invalid)\n"
        "    2. " + config_file.string() + " (missing or no python_home)\n"
        "    3. " + standalone.string() + " (not found)\n\n"
        "  For standalone builds:  cmake --build build --target stage_all\n"
        "  For system Python:      create " + config_file.string() + " with:\n"
        "    {\"python_home\": \"/usr/local\"}\n");
}

// ============================================================================
// resolve_venvs_dir — where virtual environments are stored
// ============================================================================

static fs::path resolve_venvs_dir_for_engine(const fs::path &exe_path)
{
    return fs::weakly_canonical(exe_path.parent_path() / ".." / "opt" / "python" / "venvs");
}

// ============================================================================
// initialize — create interpreter, apply config, optionally activate venv
// ============================================================================

bool PythonEngine::init_engine_(const std::string &log_tag, RoleHostCore *core)
{
    log_tag_ = log_tag.empty() ? "python" : log_tag;
    // owner_thread_id_, accepting_, api_->core() set by base class initialize().

    try
    {
        // Resolve Python home.
        const fs::path exe_path(platform::get_executable_name(/*include_path=*/true));
        const fs::path python_home = resolve_python_home_for_engine(exe_path);

        LOGGER_INFO("[{}] PythonEngine: Python home '{}'", log_tag_, python_home.string());

        // Force unbuffered Python I/O.
#if defined(_WIN32)
        _putenv_s("PYTHONUNBUFFERED", "1");
#else
        setenv("PYTHONUNBUFFERED", "1", 1);
#endif

        // Configure PyConfig.
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        config.parse_argv             = 0;
        config.install_signal_handlers = 0;

        const std::string home_str  = python_home.string();
        wchar_t          *home_wstr = Py_DecodeLocale(home_str.c_str(), nullptr);
        if (!home_wstr)
        {
            PyConfig_Clear(&config);
            LOGGER_ERROR("[{}] PythonEngine: Py_DecodeLocale failed for '{}'",
                         log_tag_, home_str);
            return false;
        }
        PyConfig_SetString(&config, &config.home, home_wstr);
        PyMem_RawFree(home_wstr);

        // Create the interpreter. GIL is held after this.
        interp_.emplace(&config);
        PyConfig_Clear(&config); // Free wchar strings allocated by PyConfig_Init.

        LOGGER_INFO("[{}] PythonEngine: Python {} initialized",
                    log_tag_, Py_GetVersion());

        // Activate venv if configured.
        if (!python_venv_.empty())
        {
            const fs::path venvs_dir = resolve_venvs_dir_for_engine(exe_path);
            const fs::path venv_dir  = venvs_dir / python_venv_;
            if (!fs::is_directory(venv_dir))
            {
                LOGGER_ERROR("[{}] PythonEngine: venv '{}' not found at '{}'",
                             log_tag_, python_venv_, venv_dir.string());
                interp_.reset(); // Destroy interpreter on this thread before returning.
                return false;
            }

            // Find site-packages.
            fs::path site_packages;
#if defined(_WIN32)
            site_packages = venv_dir / "Lib" / "site-packages";
#else
            const fs::path lib_dir = venv_dir / "lib";
            if (fs::is_directory(lib_dir))
            {
                for (const auto &entry : fs::directory_iterator(lib_dir))
                {
                    if (entry.is_directory()
                        && entry.path().filename().string().starts_with("python"))
                    {
                        site_packages = entry.path() / "site-packages";
                        break;
                    }
                }
            }
#endif
            if (site_packages.empty() || !fs::is_directory(site_packages))
            {
                LOGGER_ERROR("[{}] PythonEngine: venv '{}' site-packages not found",
                             log_tag_, python_venv_);
                interp_.reset(); // Destroy interpreter on this thread before returning.
                return false;
            }

            py::module_::import("site").attr("addsitedir")(site_packages.string());
            LOGGER_INFO("[{}] PythonEngine: activated venv '{}' (site-packages: '{}')",
                        log_tag_, python_venv_, site_packages.string());
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] PythonEngine: initialization failed: {}", log_tag_, e.what());
        interp_.reset(); // Destroy interpreter on this thread if it was created.
        return false;
    }

    // GIL is held at this point. It will be released after build_api() completes,
    // before the data loop starts. The role host is responsible for calling
    // the engine's methods in order: initialize → load_script → build_api →
    // (release GIL) → invoke loop → finalize.
    return true;
}

// ============================================================================
// load_script — import the role script module and extract callbacks
// ============================================================================

bool PythonEngine::load_script(const std::filesystem::path &script_dir,
                                const std::string &entry_point,
                                const std::string &required_callback)
{
    entry_point_        = entry_point.empty() ? "__init__.py" : entry_point;
    required_callback_  = required_callback;

    // PythonEngine uses package-based import (always __init__.py).
    // Warn if caller specified a different entry_point.
    if (entry_point_ != "__init__.py")
    {
        LOGGER_WARN("[{}] PythonEngine ignores entry_point '{}' — "
                    "Python always imports __init__.py via package convention",
                    log_tag_, entry_point_);
    }

    try
    {
        // Derive a stable module-name suffix from log_tag_ (which is
        // typically the role uid).  Route through the naming API
        // rather than hand-parsing: `parse_role_uid` returns the
        // dissected TaggedUidParts, and `unique` is the
        // uid_utils-generated `uid<8hex>` tail (HEP-0033 §G2.2.0b
        // "Numeric-token prefix convention").  If log_tag_ isn't a
        // full uid (e.g. pre-init log tag `"python"`), fall back.
        std::string uid_hex = "uid00000000";
        if (auto parts = pylabhub::hub::parse_role_uid(log_tag_);
            parts.has_value())
        {
            uid_hex.assign(parts->unique.data(), parts->unique.size());
        }

        // Determine script type from the entry_point path.
        // Convention: script_dir is already ".../script/python/" or ".../script/lua/"
        // The parent directory name is the script type.
        std::string script_type = "python"; // default for PythonEngine

        // Extract a reasonable module name from the script_dir.
        std::string module_name = "script";

        // The import function expects: base_dir / module_name / script_type / __init__.py
        // So if script_dir is already ".../script/python", base_dir should be script_dir's
        // grandparent and module_name = script_dir parent's name.
        const fs::path script_path = fs::weakly_canonical(script_dir);
        const fs::path base_dir    = script_path.parent_path().parent_path();
        module_name                = script_path.parent_path().filename().string();
        script_type                = script_path.filename().string();

        module_ = import_role_script_module(
            log_tag_, module_name, base_dir.string(), uid_hex, script_type);

        LOGGER_INFO("[{}] Loaded Python script from: {}", log_tag_, script_dir.string());

        // Extract callback references.
        py_on_init_    = py::getattr(module_, "on_init",    py::none());
        py_on_stop_    = py::getattr(module_, "on_stop",    py::none());
        py_on_produce_ = py::getattr(module_, "on_produce", py::none());
        py_on_consume_ = py::getattr(module_, "on_consume", py::none());
        py_on_process_ = py::getattr(module_, "on_process", py::none());
        py_on_inbox_   = py::getattr(module_, "on_inbox",   py::none());

        // Check required callback.
        if (!required_callback_.empty() && !has_callback(required_callback_))
        {
            LOGGER_ERROR("[{}] Script has no '{}' function", log_tag_, required_callback_);
            return false;
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] Failed to load script from '{}': {}",
                     log_tag_, script_dir.string(), e.what());
        return false;
    }

    return true;
}

// ============================================================================
// build_api — create role-specific Python API object
// ============================================================================

bool PythonEngine::build_api_(RoleAPIBase &api)
{
    stop_on_script_error_ = api.stop_on_script_error();

    // Create role-specific aliases (SlotFrame/FlexFrame) for single-direction roles.
    const auto &tag = api.role_tag();
    if (tag == "prod")
    {
        if (!out_slot_type_.is_none())
        {
            slot_alias_      = build_ctypes_type_(out_slot_spec_, "SlotFrame", out_slot_spec_.packing);
            slot_alias_spec_ = out_slot_spec_;
        }
        if (!out_fz_type_.is_none())
        {
            fz_alias_      = build_ctypes_type_(out_fz_spec_, "FlexFrame", out_fz_spec_.packing);
            fz_alias_spec_ = out_fz_spec_;
        }
    }
    else if (tag == "cons")
    {
        if (!in_slot_type_ro_.is_none())
        {
            slot_alias_ro_   = build_ctypes_type_(in_slot_spec_, "SlotFrame", in_slot_spec_.packing);
            slot_alias_ro_   = wrap_readonly_(slot_alias_ro_);
            slot_alias_spec_ = in_slot_spec_;
        }
        if (!in_fz_type_.is_none())
        {
            fz_alias_      = build_ctypes_type_(in_fz_spec_, "FlexFrame", in_fz_spec_.packing);
            fz_alias_spec_ = in_fz_spec_;
        }
    }

    // Helper: build a cached flexzone typed view for a given side + type.
    // Returns nullopt if the side has no flexzone or no type registered.
    auto cache_fz = [&](scripting::ChannelSide side, const py::object &type,
                        const hub::SchemaSpec &spec) -> std::optional<py::object>
    {
        void *ptr = api.flexzone(side);
        size_t sz = api.flexzone_size(side);
        if (!ptr || sz == 0 || type.is_none()) return std::nullopt;
        return make_slot_view_(spec, type, ptr, sz, /*readonly=*/false);
    };

    // Create role-specific Python wrapper around the RoleAPIBase.
    if (tag == "prod")
    {
        producer_api_ = std::make_unique<producer::ProducerAPI>(api);
        producer_api_->shared_data_ = py::dict();
        producer_api_->set_tx_flexzone(cache_fz(
            scripting::ChannelSide::Tx, out_fz_type_, out_fz_spec_));

        py::module_ mod = py::module_::import("pylabhub_producer");
        api_obj_ = py::cast(producer_api_.get(), py::return_value_policy::reference);
    }
    else if (tag == "cons")
    {
        consumer_api_ = std::make_unique<consumer::ConsumerAPI>(api);
        consumer_api_->shared_data_ = py::dict();
        consumer_api_->set_rx_flexzone(cache_fz(
            scripting::ChannelSide::Rx, in_fz_type_, in_fz_spec_));

        py::module_ mod = py::module_::import("pylabhub_consumer");
        api_obj_ = py::cast(consumer_api_.get(), py::return_value_policy::reference);
    }
    else if (tag == "proc")
    {
        processor_api_ = std::make_unique<processor::ProcessorAPI>(api);
        processor_api_->shared_data_ = py::dict();
        processor_api_->set_tx_flexzone(cache_fz(
            scripting::ChannelSide::Tx, out_fz_type_, out_fz_spec_));
        processor_api_->set_rx_flexzone(cache_fz(
            scripting::ChannelSide::Rx, in_fz_type_, in_fz_spec_));

        py::module_ mod = py::module_::import("pylabhub_processor");
        api_obj_ = py::cast(processor_api_.get(), py::return_value_policy::reference);
    }
    else
    {
        LOGGER_ERROR("[{}] build_api: unknown role_tag '{}' — must be 'prod', 'cons', or 'proc'",
                     log_tag_, api_->role_tag());
        return false;
    }

    // Block non-active role modules: setting sys.modules[name] = None is the
    // standard Python convention for marking a module as unavailable. This
    // makes `import pylabhub_<wrong_role>` raise ImportError at runtime.
    {
        static const char *all_modules[] = {
            "pylabhub_producer", "pylabhub_consumer", "pylabhub_processor"};
        const char *active = (api_->role_tag() == "prod") ? "pylabhub_producer"
                           : (api_->role_tag() == "cons") ? "pylabhub_consumer"
                                                       : "pylabhub_processor";
        py::object sys_modules = py::module_::import("sys").attr("modules");
        for (const char *mod : all_modules)
        {
            if (std::strcmp(mod, active) != 0)
                sys_modules[py::str(mod)] = py::none();
        }
    }
    return true;
}

// ============================================================================
// build_api(HubAPI&) — hub-side surface (HEP-CORE-0033 Phase 7 D4.1)
// ============================================================================
//
// Mirrors the role-side build_api_(RoleAPIBase&) shape but with the
// Phase 7 minimum surface (log/uid/metrics).  No schema-driven ctypes
// types (hub has no slots/flexzones).
//
// Wires the api object into the script's namespace via TWO paths,
// matching the LuaEngine dual-exposure pattern (D3.2):
//
//   1. As `<script_module>.api` — set as a module attribute so
//      callbacks dispatched via generic `engine.invoke(name, args)`
//      can reach it via module-global lookup.  Generic invoke does
//      NOT pass api as an argument, so this is the only path for
//      event/tick callbacks (on_tick, on_channel_opened, ...).
//
//   2. As `api_obj_` — for `invoke_on_init` / `invoke_on_stop` which
//      pass api explicitly as the first positional arg (mirrors role-
//      side `fn(api_obj_)` shape).  Scripts that prefer the explicit-
//      arg style can write `def on_init(api): api.log(...)`.
//
// The HubAPI Python class itself is bound via PYBIND11_EMBEDDED_MODULE
// in src/scripting/hub_api_python.cpp — `pylabhub_hub.HubAPI`.

// Forward decl for force-link symbol — defined in hub_api_python.cpp.
// Calling it (even discarding the result of an empty function) forces
// the static linker to keep that .o file, so its PYBIND11_EMBEDDED_MODULE
// static initializer runs at program startup and registers `pylabhub_hub`
// in the inittab.  Without this, linking pylabhub-scripting as a static
// archive drops the .o (no other symbol referenced) and `import
// pylabhub_hub` raises ModuleNotFoundError.
extern "C" void plh_register_hub_api_python_module();

bool PythonEngine::build_api_(::pylabhub::hub_host::HubAPI &api)
{
    // Hub doesn't expose stop_on_script_error on HubAPI today (see
    // LuaEngine::build_api_(HubAPI&) — same default).  Default false.
    stop_on_script_error_ = false;

    // Force the static linker to keep hub_api_python.cpp's .o so its
    // PYBIND11_EMBEDDED_MODULE initializer fires at process start
    // (must happen BEFORE any `import pylabhub_hub`).
    plh_register_hub_api_python_module();

    py::gil_scoped_acquire gil;

    // Importing the embedded module triggers its
    // PYBIND11_EMBEDDED_MODULE static-init binding (registered at
    // process start).  This must succeed for `py::cast(&api)` below
    // to find the bound HubAPI class.
    try
    {
        py::module_::import("pylabhub_hub");
    }
    catch (const py::error_already_set &e)
    {
        LOGGER_ERROR("[{}] PythonEngine::build_api(HubAPI): "
                     "failed to import pylabhub_hub embedded module: {}",
                     log_tag_, e.what());
        return false;
    }

    // Wrap the C++ HubAPI as a Python object.  Reference policy —
    // pybind11 holds a non-owning pointer; HubAPI's lifetime is owned
    // by EngineHost<HubAPI>::api_, which outlives the engine's
    // finalize_engine_ (which clears api_obj_).
    api_obj_ = py::cast(&api, py::return_value_policy::reference);

    // Set as `<script_module>.api` so callbacks dispatched via
    // generic invoke (which doesn't pass api as an arg) can reach
    // it via module-global lookup.
    if (!module_.is_none())
        py::setattr(module_, "api", api_obj_);

    LOGGER_INFO("[{}] build_api(HubAPI) complete — api set on script "
                "module + stored for invoke_on_init; uid='{}'",
                log_tag_, api.uid());
    return true;
}

// ============================================================================
// finalize — clear all Python objects and destroy interpreter
// ============================================================================

void PythonEngine::finalize_engine_()
{
    if (!interp_.has_value())
        return;

    // GIL is held (py::scoped_interpreter holds it on the creating thread).
    // accepting_ already set false by base class finalize().
    // Cancel all pending generic invoke requests.
    {
        std::lock_guard lk(queue_mu_);
        for (auto &req : request_queue_)
            req.promise.set_value({InvokeStatus::EngineShutdown, {}});
        request_queue_.clear();
    }

    // Clear all Python objects before destroying the interpreter.
    clear_pyobjects_();

    // Clear inbox caches before role host tears down infrastructure.
    if (producer_api_)
        producer_api_->clear_inbox_cache();
    if (consumer_api_)
        consumer_api_->clear_inbox_cache();
    if (processor_api_)
        processor_api_->clear_inbox_cache();

    producer_api_.reset();
    consumer_api_.reset();
    processor_api_.reset();
    api_ = nullptr;  // non-owning — role host destroys the base

    // Destroy interpreter (calls Py_Finalize).
    interp_.reset();
}

// ============================================================================
// Generic invoke / eval
// ============================================================================

bool PythonEngine::invoke(const std::string &name)
{
    if (!is_accepting())
        return false;

    if (std::this_thread::get_id() == owner_thread_id_)
        return execute_direct_(name).status == InvokeStatus::Ok;

    std::promise<InvokeResponse> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lk(queue_mu_);
        if (!is_accepting())
            return false;
        request_queue_.push_back({name, {}, RequestKind::Invoke, std::move(promise)});
    }
    return future.get().status == InvokeStatus::Ok;
}

bool PythonEngine::invoke(const std::string &name, const nlohmann::json &args)
{
    if (!is_accepting())
        return false;

    if (std::this_thread::get_id() == owner_thread_id_)
        return execute_direct_(name, args).status == InvokeStatus::Ok;

    std::promise<InvokeResponse> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lk(queue_mu_);
        if (!is_accepting())
            return false;
        request_queue_.push_back({name, args, RequestKind::Invoke, std::move(promise)});
    }
    return future.get().status == InvokeStatus::Ok;
}

InvokeResponse PythonEngine::invoke_returning(const std::string &name,
                                               const nlohmann::json &args,
                                               int64_t timeout_ms)
{
    if (!is_accepting())
        return {InvokeStatus::EngineShutdown, {}};

    if (std::this_thread::get_id() == owner_thread_id_)
        return execute_direct_returning_(name, args);

    std::promise<InvokeResponse> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lk(queue_mu_);
        if (!is_accepting())
            return {InvokeStatus::EngineShutdown, {}};
        request_queue_.push_back({name, args, RequestKind::InvokeReturning,
                                  std::move(promise)});
    }
    // Wake the worker thread so it drains process_pending() promptly,
    // matching the §12.4 augmentation request transport contract.
    if (hub_api_ && hub_api_->core())
        hub_api_->core()->notify_incoming();
    else if (api_ && api_->core())
        api_->core()->notify_incoming();

    // Project timeout convention: -1=infinite, 0=non-blocking, >0=wait N ms.
    if (timeout_ms < 0)
        return future.get();

    if (future.wait_for(std::chrono::milliseconds(timeout_ms))
        == std::future_status::ready)
    {
        return future.get();
    }
    // Worker hasn't drained in time.  The future stays alive as a
    // detached `std::future` (the promise is owned by the queued
    // PendingRequest); when the worker eventually drains, it will set
    // the value on a future no one is reading — harmless.  Return
    // TimedOut so HubAPI::run_augment ships the default response.
    return {InvokeStatus::TimedOut, {}};
}

void PythonEngine::process_pending()
{
    if (std::this_thread::get_id() != owner_thread_id_)
        return;
    process_pending_();
}

size_t PythonEngine::pending_script_engine_request_count() const noexcept
{
    // Status probe — returns 0 when not accepting (queue is drained
    // + cancelled by finalize_engine_), which is the honest answer:
    // no future drain will happen.
    if (!is_accepting())
        return 0;
    std::lock_guard lk(queue_mu_);
    return request_queue_.size();
}

InvokeResponse PythonEngine::eval(const std::string &code)
{
    if (!is_accepting())
        return {InvokeStatus::EngineShutdown, {}};

    if (std::this_thread::get_id() == owner_thread_id_)
        return eval_direct_(code);

    std::promise<InvokeResponse> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lk(queue_mu_);
        if (!is_accepting())
            return {InvokeStatus::EngineShutdown, {}};
        request_queue_.push_back({code, {}, RequestKind::Eval, std::move(promise)});
    }
    return future.get();
}

InvokeResponse PythonEngine::execute_direct_(const std::string &name)
{
    if (name.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    py::object fn = py::getattr(module_, name.c_str(), py::none());
    if (fn.is_none())
    {
        // NotFound still drains pending — a misnamed callback shouldn't
        // stall cross-thread invoke/eval requests on the queue.
        process_pending_();
        return {InvokeStatus::NotFound, {}};
    }

    InvokeResponse resp;
    try
    {
        fn();
        resp = {InvokeStatus::Ok, {}};
    }
    catch (const py::error_already_set &e)
    {
        // Route through on_python_error_ so admin-facing invoke() honors
        // stop_on_script_error_ consistently with the hot-path callbacks.
        const std::string tag = "invoke('" + name + "')";
        on_python_error_(tag.c_str(), e);
        resp = {InvokeStatus::ScriptError, {}};
    }
    // Drain pending cross-thread requests AFTER the owner-thread invoke
    // finishes — same point on the timeline `invoke_produce` / `_consume`
    // / `_process` / `_on_inbox` already drain (see line ~1024 below).
    // Hub-side has no external eval surface today (HEP-CORE-0033 §17
    // "No remote code injection"), but the drain stays as an internal
    // invariant: any future C++ caller that holds a non-owner-thread
    // reference to `engine.invoke/eval` queues here, and the next
    // owner-thread invoke discharges the queue.  Role-side already
    // drains via the role-specific invoke_* methods.
    process_pending_();
    return resp;
}

InvokeResponse PythonEngine::execute_direct_(const std::string &name,
                                              const nlohmann::json &args)
{
    if (name.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    py::object fn = py::getattr(module_, name.c_str(), py::none());
    if (fn.is_none())
    {
        process_pending_();
        return {InvokeStatus::NotFound, {}};
    }

    InvokeResponse resp;
    try
    {
        // Unpack JSON args as keyword arguments (recursive for nested types).
        py::dict kwargs;
        for (auto it = args.begin(); it != args.end(); ++it)
            kwargs[py::str(it.key())] = json_to_py(it.value());
        fn(**kwargs);
        resp = {InvokeStatus::Ok, {}};
    }
    catch (const py::error_already_set &e)
    {
        const std::string tag = "invoke('" + name + "', args)";
        on_python_error_(tag.c_str(), e);
        resp = {InvokeStatus::ScriptError, {}};
    }
    // See process_pending_() comment in the no-args overload above —
    // mirrors the role-side `invoke_produce` drain pattern.
    process_pending_();
    return resp;
}

InvokeResponse PythonEngine::execute_direct_returning_(const std::string &name,
                                                       const nlohmann::json &args)
{
    // HEP-CORE-0033 §12.2.2 augmentation hook entry — same call shape as
    // execute_direct_(name, args) but the script's return value is
    // converted back to JSON and returned in resp.value (instead of
    // being discarded).  Used by HubAPI::augment_* paths after the
    // hub has built the default response on the receiving thread.
    if (name.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    py::object fn = py::getattr(module_, name.c_str(), py::none());
    if (fn.is_none())
    {
        process_pending_();
        return {InvokeStatus::NotFound, {}};
    }

    InvokeResponse resp;
    try
    {
        py::dict kwargs;
        for (auto it = args.begin(); it != args.end(); ++it)
            kwargs[py::str(it.key())] = json_to_py(it.value());
        py::object ret = fn(**kwargs);
        // py_to_json handles py::none() → nlohmann::json(nullptr) so a
        // missing-return script callback yields {Ok, null} rather than
        // an error — the augment caller then keeps the default response.
        resp = {InvokeStatus::Ok, py_to_json(ret)};
    }
    catch (const py::error_already_set &e)
    {
        const std::string tag = "invoke_returning('" + name + "', args)";
        on_python_error_(tag.c_str(), e);
        resp = {InvokeStatus::ScriptError, {}};
    }
    process_pending_();
    return resp;
}

InvokeResponse PythonEngine::eval_direct_(const std::string &code)
{
    if (code.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    try
    {
        // Evaluate in the script module's namespace so module-level
        // functions and variables are accessible.
        py::object result = py::eval(code, module_.attr("__dict__"));

        return {InvokeStatus::Ok, py_to_json(result)};
    }
    catch (const py::error_already_set &e)
    {
        on_python_error_("eval()", e);
        return {InvokeStatus::ScriptError, {}};
    }
}

void PythonEngine::process_pending_()
{
    if (!is_accepting())
        return;

    std::deque<PendingRequest> local;
    {
        std::lock_guard lk(queue_mu_);
        if (request_queue_.empty())
            return;
        local.swap(request_queue_);
    }

    for (auto &req : local)
    {
        if (!is_accepting())
        {
            req.promise.set_value({InvokeStatus::EngineShutdown, {}});
            continue;
        }
        switch (req.kind)
        {
        case RequestKind::Eval:
            req.promise.set_value(eval_direct_(req.name));
            break;
        case RequestKind::InvokeReturning:
            req.promise.set_value(
                execute_direct_returning_(req.name.c_str(), req.args));
            break;
        case RequestKind::Invoke:
        default:
            InvokeResponse resp;
            if (req.args.is_null() || req.args.empty())
                resp = execute_direct_(req.name.c_str());
            else
                resp = execute_direct_(req.name.c_str(), req.args);
            req.promise.set_value(std::move(resp));
            break;
        }
    }
}

// ============================================================================
// has_callback
// ============================================================================

bool PythonEngine::has_callback(const std::string &name) const
{
    if (name.empty())
        return false;

    if (name == "on_init")
        return is_callable(py_on_init_);
    if (name == "on_stop")
        return is_callable(py_on_stop_);
    if (name == "on_produce")
        return is_callable(py_on_produce_);
    if (name == "on_consume")
        return is_callable(py_on_consume_);
    if (name == "on_process")
        return is_callable(py_on_process_);
    if (name == "on_inbox")
        return is_callable(py_on_inbox_);

    // Unknown callback — try getattr on the module.
    if (!module_.is_none())
    {
        py::object fn = py::getattr(module_, name.c_str(), py::none());
        return is_callable(fn);
    }
    return false;
}

// ============================================================================
// register_slot_type — build ctypes/numpy type and cache it
// ============================================================================

bool PythonEngine::register_slot_type(const hub::SchemaSpec &spec,
                                       const std::string &type_name,
                                       const std::string &packing)
{
    // Validate canonical name UP FRONT — before any side effects.
    // Rejecting unknown names here (rather than silently falling
    // through to LOGGER_WARN + garbage-collected py::object) pins
    // the design invariant documented in
    // script_engine.hpp::register_slot_type: only the five canonical
    // frame names are valid.  A typo in a role host / schema config
    // must fail loudly at this point.
    if (type_name != "InSlotFrame"
        && type_name != "OutSlotFrame"
        && type_name != "InFlexFrame"
        && type_name != "OutFlexFrame"
        && type_name != "InboxFrame")
    {
        LOGGER_ERROR("[{}] register_slot_type: unknown canonical type_name "
                     "'{}' — must be one of InSlotFrame, OutSlotFrame, "
                     "InFlexFrame, OutFlexFrame, InboxFrame",
                     log_tag_, type_name);
        return false;
    }

    if (!spec.has_schema)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') called with has_schema=false",
                     log_tag_, type_name);
        return false;
    }

    // Normalize: the explicit `packing` arg is authoritative for this
    // registration.  We store a copy with spec.packing overwritten so
    // that later consumers of the cached spec (notably build_api_'s
    // SlotFrame/FlexFrame alias creation, which forwards
    // <stored_spec>.packing) always see the packing that was actually
    // used, regardless of what the caller set on spec.packing.
    // Eliminates a footgun where the two can disagree.
    hub::SchemaSpec stored_spec = spec;
    stored_spec.packing = packing;

    // Compute expected size from schema (infrastructure-authoritative).
    auto [layout, expected_size] = hub::compute_field_layout(to_field_descs(spec.fields), packing);

    try
    {
        py::object type = build_ctypes_type_(spec, type_name, packing);

        // Validate: engine-built ctypes struct size must match schema-computed size.
        size_t actual_size = ctypes_sizeof(type);
        if (actual_size != expected_size)
        {
            LOGGER_ERROR("[{}] register_slot_type('{}') size mismatch: "
                         "ctypes={}, schema={}",
                         log_tag_, type_name, actual_size, expected_size);
            return false;
        }

        if (type_name == "InSlotFrame")
        {
            in_slot_type_ro_ = wrap_readonly_(type);
            in_slot_spec_    = stored_spec;
        }
        else if (type_name == "OutSlotFrame")
        {
            out_slot_type_ = type;
            out_slot_spec_ = stored_spec;
        }
        else if (type_name == "InFlexFrame")
        {
            in_fz_type_ = type; // mutable — flexzone is bidirectional per HEP-0002
            in_fz_spec_ = stored_spec;
        }
        else if (type_name == "OutFlexFrame")
        {
            out_fz_type_ = type;
            out_fz_spec_ = stored_spec;
        }
        else  // type_name == "InboxFrame" — guaranteed by upfront name check
        {
            inbox_type_ro_ = wrap_readonly_(type);
            inbox_spec_    = stored_spec;
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] PythonEngine: register_slot_type('{}') failed: {}",
                     log_tag_, type_name, e.what());
        return false;
    }

    return true;
}

// ============================================================================
// type_sizeof
// ============================================================================

size_t PythonEngine::type_sizeof(const std::string &type_name) const
{
    

    // Return size for the cached type (represents the actual struct size).
    py::object type = py::none();
    if (type_name == "InSlotFrame")
        type = in_slot_type_ro_;
    else if (type_name == "OutSlotFrame")
        type = out_slot_type_;
    else if (type_name == "InFlexFrame")
        type = in_fz_type_;
    else if (type_name == "OutFlexFrame")
        type = out_fz_type_;
    else if (type_name == "InboxFrame")
        type = inbox_type_ro_;
    // Alias lookup: Python stores aliases as explicit py::object members
    // because ctypes classes are not name-addressable at runtime. Each alias
    // is a separate ctypes class created in build_api_().
    // In contrast, Lua aliases are FFI typedefs (e.g., "typedef OutSlotFrame SlotFrame;")
    // which are resolved automatically by ffi.sizeof() — no explicit dispatch needed.
    else if (type_name == "SlotFrame")
        type = slot_alias_.is_none() ? slot_alias_ro_ : slot_alias_;
    else if (type_name == "FlexFrame")
        type = fz_alias_;

    if (type.is_none())
        return 0;

    try
    {
        return ctypes_sizeof(type);
    }
    catch (const std::exception &e)
    {
        // ctypes_sizeof failed (rare — usually means a registered type
        // alias is broken).  Return 0 like the type.is_none() path, but
        // log at DEBUG so the cause shows up in diagnostic traces; without
        // this the caller would see an unexplained "size = 0" and have
        // no signal that ctypes itself raised.
        LOGGER_DEBUG("[{}] PythonEngine: ctypes_sizeof('{}') threw: {}",
                     log_tag_, type_name, e.what());
        return 0;
    }
}

// ============================================================================
// invoke_on_init — on_init(api)
// ============================================================================

void PythonEngine::invoke_on_init()
{
    // No process_pending_() needed: any request enqueued before on_init
    // returns is drained at the next owner-thread invoke (either
    // role-side `invoke_produce` etc. inside the data loop, or
    // hub-side `execute_direct_` inside the event/tick loop).  For the
    // role-side fast path the original argument also holds — `ctrl_thread_`
    // is not yet spawned, so no non-owner thread exists to queue
    // requests.  For hub-side, AdminService is already running by
    // ctor time, so a pre-on_init eval is theoretically possible —
    // but it lands on the queue and waits at most one event/tick
    // cycle (typically ms-to-1s) before draining via execute_direct_.
    if (!is_callable(py_on_init_))
        return;

    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        on_python_error_("on_init", e);
    }
}

// ============================================================================
// invoke_on_stop — on_stop(api)
// ============================================================================

void PythonEngine::invoke_on_stop()
{
    // No process_pending_() needed: called after stop_accepting() + ctrl_thread_.join(),
    // so no non-owner threads are running and no new requests can be queued.
    // Any remaining pending requests are cancelled in finalize_engine_().
    if (!is_callable(py_on_stop_))
        return;

    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        on_python_error_("on_stop", e);
    }
}

// ============================================================================
// invoke_produce — on_produce(tx, msgs, api) -> bool
// ============================================================================

InvokeResult PythonEngine::invoke_produce(
    InvokeTx tx,
    std::vector<IncomingMessage> &msgs)
{
    if (!is_callable(py_on_produce_))
    {
        InvokeResult r = InvokeResult::Error;
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_produce called but on_produce is not "
                         "registered — the role requires this callback",
                         log_tag_);
            r = handle_script_error_("on_produce [missing callback]");
        }
        process_pending_();
        return r;
    }
    InvokeResult result = InvokeResult::Error;
    {
        py::gil_scoped_acquire g;
        try
        {
            PyTxChannel tx_ch;
            if (tx.slot != nullptr && !out_slot_type_.is_none())
                tx_ch.slot = make_slot_view_(out_slot_spec_, out_slot_type_, tx.slot, tx.slot_size, false);
            py::list msgs_list = build_messages_list_(msgs);
            py::object ret = py_on_produce_(py::cast(tx_ch), msgs_list, api_obj_);
            result = parse_return_value_(ret, "on_produce");
        }
        catch (py::error_already_set &e)
        {
            result = on_python_error_("on_produce", e);
        }
    }
    process_pending_();
    return result;
}

// ============================================================================
// invoke_consume — on_consume(rx, msgs, api) -> bool
// ============================================================================

InvokeResult PythonEngine::invoke_consume(
    InvokeRx rx,
    std::vector<IncomingMessage> &msgs)
{
    if (!is_callable(py_on_consume_))
    {
        InvokeResult r = InvokeResult::Error;
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_consume called but on_consume is not "
                         "registered — the role requires this callback",
                         log_tag_);
            r = handle_script_error_("on_consume [missing callback]");
        }
        process_pending_();
        return r;
    }
    InvokeResult result = InvokeResult::Error;
    {
        py::gil_scoped_acquire g;
        try
        {
            PyRxChannel rx_ch;
            if (rx.slot != nullptr && !in_slot_type_ro_.is_none())
            {
                rx_ch.slot = make_slot_view_(
                    in_slot_spec_, in_slot_type_ro_,
                    const_cast<void *>(rx.slot), rx.slot_size, true);
            }
            py::list msgs_list = build_messages_list_bare_(msgs);
            py::object ret = py_on_consume_(py::cast(rx_ch), msgs_list, api_obj_);
            result = parse_return_value_(ret, "on_consume");
        }
        catch (py::error_already_set &e)
        {
            result = on_python_error_("on_consume", e);
        }
    }
    process_pending_();
    return result;
}

// ============================================================================
// invoke_process — on_process(rx, tx, msgs, api) -> bool
// ============================================================================

InvokeResult PythonEngine::invoke_process(
    InvokeRx rx, InvokeTx tx,
    std::vector<IncomingMessage> &msgs)
{
    if (!is_callable(py_on_process_))
    {
        InvokeResult r = InvokeResult::Error;
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_process called but on_process is not "
                         "registered — the role requires this callback",
                         log_tag_);
            r = handle_script_error_("on_process [missing callback]");
        }
        process_pending_();
        return r;
    }
    InvokeResult result = InvokeResult::Error;
    {
        py::gil_scoped_acquire g;
        try
        {
            PyRxChannel rx_ch;
            if (rx.slot != nullptr && !in_slot_type_ro_.is_none())
            {
                rx_ch.slot = make_slot_view_(
                    in_slot_spec_, in_slot_type_ro_,
                    const_cast<void *>(rx.slot), rx.slot_size, true);
            }
            PyTxChannel tx_ch;
            if (tx.slot != nullptr && !out_slot_type_.is_none())
                tx_ch.slot = make_slot_view_(out_slot_spec_, out_slot_type_, tx.slot, tx.slot_size, false);
            py::list msgs_list = build_messages_list_(msgs);
            py::object ret = py_on_process_(py::cast(rx_ch), py::cast(tx_ch), msgs_list, api_obj_);
            result = parse_return_value_(ret, "on_process");
        }
        catch (py::error_already_set &e)
        {
            result = on_python_error_("on_process", e);
        }
    }
    process_pending_();
    return result;
}

// ============================================================================
// invoke_on_inbox — on_inbox(msg, api) -> bool
// msg.data = typed payload, msg.sender_uid = sender UID, msg.seq = sequence
// ============================================================================

InvokeResult PythonEngine::invoke_on_inbox(InvokeInbox msg)
{
    InvokeResult result = InvokeResult::Error;
    if (!is_callable(py_on_inbox_))
    {
        if (is_accepting())
        {
            LOGGER_ERROR("[{}] invoke_on_inbox called but on_inbox is not "
                         "registered — caller should gate on has_callback",
                         log_tag_);
            result = handle_script_error_("on_inbox [missing callback]");
        }
        process_pending_();
        return result;
    }

    // Inbox type must be cached at startup via register_slot_type("InboxFrame").
    if (inbox_type_ro_.is_none())
    {
        LOGGER_ERROR("[{}] invoke_on_inbox: InboxFrame type not registered — "
                     "inbox_schema must be configured and registered before use",
                     log_tag_);
        auto r = handle_script_error_("on_inbox [InboxFrame not registered]");
        process_pending_();
        return r;
    }

    py::gil_scoped_acquire g;
    try
    {
        PyInboxMsg msg_obj;
        if (msg.data != nullptr && msg.data_size > 0)
        {
            // Use from_buffer_copy: inbox data is only valid until the next
            // recv_one() call, so we must copy into the ctypes struct.
            auto buf = py::bytes(reinterpret_cast<const char *>(msg.data), msg.data_size);
            msg_obj.data = inbox_type_ro_.attr("from_buffer_copy")(buf);
        }
        msg_obj.sender_uid = msg.sender_uid;
        msg_obj.seq        = msg.seq;

        py::object ret = py_on_inbox_(py::cast(msg_obj), api_obj_);
        result = parse_return_value_(ret, "on_inbox");
    }
    catch (py::error_already_set &e)
    {
        result = on_python_error_("on_inbox", e);
    }
    process_pending_();
    return result;
}

// ============================================================================
// Internal helpers
// ============================================================================

py::object PythonEngine::build_ctypes_type_(const hub::SchemaSpec &spec, const std::string &name,
                                             const std::string &packing)
{
    // Override spec packing with the explicitly requested one if provided.
    hub::SchemaSpec effective_spec = spec;
    if (!packing.empty())
        effective_spec.packing = packing;

    return build_ctypes_struct(effective_spec, name);
}

py::object PythonEngine::wrap_readonly_(const py::object &type)
{
    return wrap_as_readonly_ctypes(type);
}

py::object PythonEngine::make_slot_view_(const hub::SchemaSpec &spec, const py::object &type,
                                          void *data, size_t size, bool readonly)
{
    return make_slot_view(spec, type, data, size, readonly);
}

py::list PythonEngine::build_messages_list_(std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message -> Python dict with "event" key + detail fields.
            py::dict d;
            d["event"] = m.event;
            for (auto &[key, val] : m.details.items())
                d[py::str(key)] = json_to_py(val);
            lst.append(std::move(d));
        }
        else
        {
            // Data message -> (sender_hex, bytes) tuple.
            lst.append(py::make_tuple(
                py::str(format_tools::bytes_to_hex(m.sender)),
                py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
        }
    }
    return lst;
}

py::list PythonEngine::build_messages_list_bare_(std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message -> Python dict (same format as above).
            py::dict d;
            d["event"] = m.event;
            for (auto &[key, val] : m.details.items())
                d[py::str(key)] = json_to_py(val);
            lst.append(std::move(d));
        }
        else
        {
            // Consumer format: bare bytes, no sender identity.
            lst.append(
                py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size()));
        }
    }
    return lst;
}

InvokeResult PythonEngine::parse_return_value_(const py::object &ret, const char *callback_name)
{
    // IMPORTANT: isinstance(True, int) is True in Python.
    // Must check bool BEFORE int.
    if (py::isinstance<py::bool_>(ret))
    {
        return ret.cast<bool>() ? InvokeResult::Commit : InvokeResult::Discard;
    }

    if (ret.is_none())
    {
        LOGGER_ERROR("[{}] {} returned None — explicit 'return True' or "
                     "'return False' is required. Treating as error.",
                     log_tag_, callback_name);
        const std::string tag = std::string(callback_name) + " [missing return]";
        return handle_script_error_(tag.c_str());
    }

    // Wrong type.
    LOGGER_ERROR("[{}] {} returned non-boolean type '{}' — "
                 "expected 'return True' or 'return False'. Treating as error.",
                 log_tag_, callback_name,
                 py::str(py::type::of(ret).attr("__name__")).cast<std::string>());
    const std::string tag = std::string(callback_name) + " [wrong return type]";
    return handle_script_error_(tag.c_str());
}

InvokeResult PythonEngine::on_python_error_(const char *callback_name,
                                             const py::error_already_set &e)
{
    LOGGER_ERROR("[{}] {} error: {}", log_tag_, callback_name, e.what());
    return handle_script_error_(callback_name);
}

InvokeResult PythonEngine::handle_script_error_(const char *callback_tag)
{
    // Resolve core via whichever ApiT was bound — base class stores
    // ONE of `api_` (RoleAPIBase*, role-side) or `hub_api_` (HubAPI*,
    // hub-side, HEP-CORE-0033 Phase 7 D4); the other is null per
    // build_api's contract.  Mirrors LuaEngine::on_pcall_error_'s
    // resolution shape.  Pre-D4 the direct `api_->core()` access
    // would null-deref on the hub path.
    RoleHostCore *core = api_     ? api_->core()
                       : hub_api_ ? hub_api_->core()
                                  : nullptr;
    if (core)
        core->inc_script_error_count();

    if (stop_on_script_error_)
    {
        LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} error",
                     log_tag_, callback_tag);
        if (core)
            core->request_stop();
    }
    return InvokeResult::Error;
}

void PythonEngine::clear_pyobjects_()
{
    module_        = py::none();
    py_on_init_    = py::none();
    py_on_stop_    = py::none();
    py_on_produce_ = py::none();
    py_on_consume_ = py::none();
    py_on_process_ = py::none();
    py_on_inbox_   = py::none();
    api_obj_       = py::none();

    in_slot_type_ro_ = py::none();
    out_slot_type_   = py::none();
    in_fz_type_      = py::none();
    out_fz_type_     = py::none();
    inbox_type_ro_   = py::none();
    slot_alias_      = py::none();
    slot_alias_ro_   = py::none();
    fz_alias_        = py::none();
}

// ============================================================================
// script_error_count — null-safe across role/hub paths (S1 fix)
// ============================================================================
//
// Mirrors the resolution shape of handle_script_error_ (the writer):
// counter is bumped through whichever ApiT was bound; reading it must
// resolve through the same path or the metric silently lies on hub
// side (api_ is null when hub_api_ is set, post-D4).  Out-of-line
// because hub_api_->core() needs the full HubAPI type, which the
// header only forward-declares.

uint64_t PythonEngine::script_error_count() const noexcept
{
    return api_     ? api_->core()->script_error_count()
         : hub_api_ ? hub_api_->core()->script_error_count()
                    : 0;
}

} // namespace pylabhub::scripting
