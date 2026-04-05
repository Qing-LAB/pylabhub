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
#include "utils/hub_producer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "python_helpers.hpp"

#include "plh_platform.hpp"

// Role API headers are in sibling directories. When python_engine.cpp is compiled
// as part of a target that adds src/ or src/{producer,consumer,processor} to its
// include path, these resolve directly. When compiled from src/scripting/ with
// only ${CMAKE_CURRENT_SOURCE_DIR} as an include path, the relative paths work.
#include "../producer/producer_api.hpp"
#include "../consumer/consumer_api.hpp"
#include "../processor/processor_api.hpp"

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

// ============================================================================
// json_to_py — recursive nlohmann::json → py::object conversion
// ============================================================================

static py::object json_to_py(const nlohmann::json &val, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
        return py::str("<recursion limit>");
    if (val.is_string())
        return py::str(val.get<std::string>());
    if (val.is_boolean())
        return py::bool_(val.get<bool>());
    if (val.is_number_unsigned())
        return py::int_(val.get<uint64_t>());
    if (val.is_number_integer())
        return py::int_(val.get<int64_t>());
    if (val.is_number_float())
        return py::float_(val.get<double>());
    if (val.is_object())
    {
        py::dict d;
        for (auto &[k, v] : val.items())
            d[py::str(k)] = json_to_py(v, depth + 1);
        return std::move(d);
    }
    if (val.is_array())
    {
        py::list l;
        for (auto &elem : val)
            l.append(json_to_py(elem, depth + 1));
        return std::move(l);
    }
    if (val.is_null())
        return py::none();
    return py::str(val.dump());
}

// ============================================================================
// py_to_json — recursive py::object → nlohmann::json conversion
// ============================================================================

static nlohmann::json py_to_json(const py::object &obj, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
        return "<recursion limit>";
    if (obj.is_none())
        return nullptr;
    // Check bool before int — isinstance(True, int) is True in Python.
    if (py::isinstance<py::bool_>(obj))
        return obj.cast<bool>();
    if (py::isinstance<py::int_>(obj))
        return obj.cast<int64_t>();
    if (py::isinstance<py::float_>(obj))
        return obj.cast<double>();
    if (py::isinstance<py::str>(obj))
        return obj.cast<std::string>();
    if (py::isinstance<py::dict>(obj))
    {
        nlohmann::json j = nlohmann::json::object();
        for (auto &item : obj.cast<py::dict>())
            j[item.first.cast<std::string>()] =
                py_to_json(item.second.cast<py::object>(), depth + 1);
        return j;
    }
    if (py::isinstance<py::list>(obj))
    {
        nlohmann::json j = nlohmann::json::array();
        for (auto &elem : obj.cast<py::list>())
            j.push_back(py_to_json(elem.cast<py::object>(), depth + 1));
        return j;
    }
    if (py::isinstance<py::tuple>(obj))
    {
        nlohmann::json j = nlohmann::json::array();
        for (auto &elem : obj.cast<py::tuple>())
            j.push_back(py_to_json(elem.cast<py::object>(), depth + 1));
        return j;
    }
    // Fallback: repr as string.
    return py::str(obj).cast<std::string>();
}

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
    script_dir_str_     = script_dir.string();
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
        // Derive uid_hex from log_tag_ (e.g., "PROD-test-AABBCCDD" → "AABBCCDD").
        std::string uid_hex = "00000000";
        const auto  last_dash = log_tag_.rfind('-');
        if (last_dash != std::string::npos)
            uid_hex = log_tag_.substr(last_dash + 1);

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

    // Create role-specific Python wrapper around the RoleAPIBase.
    if (tag == "prod")
    {
        producer_api_ = std::make_unique<producer::ProducerAPI>(api);
        producer_api_->shared_data_ = py::dict();

        py::module_ mod = py::module_::import("pylabhub_producer");
        api_obj_ = py::cast(producer_api_.get(), py::return_value_policy::reference);
    }
    else if (tag == "cons")
    {
        consumer_api_ = std::make_unique<consumer::ConsumerAPI>(api);
        consumer_api_->shared_data_ = py::dict();

        py::module_ mod = py::module_::import("pylabhub_consumer");
        api_obj_ = py::cast(consumer_api_.get(), py::return_value_policy::reference);
    }
    else if (tag == "proc")
    {
        processor_api_ = std::make_unique<processor::ProcessorAPI>(api);
        processor_api_->shared_data_ = py::dict();

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
        request_queue_.push_back({name, {}, false, std::move(promise)});
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
        request_queue_.push_back({name, args, false, std::move(promise)});
    }
    return future.get().status == InvokeStatus::Ok;
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
        request_queue_.push_back({code, {}, true, std::move(promise)});
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
        return {InvokeStatus::NotFound, {}};

    executing_.store(true, std::memory_order_release);
    try
    {
        fn();
        executing_.store(false, std::memory_order_release);
        return {InvokeStatus::Ok, {}};
    }
    catch (const py::error_already_set &e)
    {
        executing_.store(false, std::memory_order_release);
        LOGGER_ERROR("[{}] invoke('{}'): {}", log_tag_, name, e.what());
        if (api_->core())
            api_->core()->inc_script_error_count();
        return {InvokeStatus::ScriptError, {}};
    }
}

InvokeResponse PythonEngine::execute_direct_(const std::string &name,
                                              const nlohmann::json &args)
{
    if (name.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    py::object fn = py::getattr(module_, name.c_str(), py::none());
    if (fn.is_none())
        return {InvokeStatus::NotFound, {}};

    executing_.store(true, std::memory_order_release);
    try
    {
        // Unpack JSON args as keyword arguments (recursive for nested types).
        py::dict kwargs;
        for (auto it = args.begin(); it != args.end(); ++it)
            kwargs[py::str(it.key())] = json_to_py(it.value());
        fn(**kwargs);
        executing_.store(false, std::memory_order_release);
        return {InvokeStatus::Ok, {}};
    }
    catch (const py::error_already_set &e)
    {
        executing_.store(false, std::memory_order_release);
        LOGGER_ERROR("[{}] invoke('{}', args): {}", log_tag_, name, e.what());
        if (api_->core())
            api_->core()->inc_script_error_count();
        return {InvokeStatus::ScriptError, {}};
    }
}

InvokeResponse PythonEngine::eval_direct_(const std::string &code)
{
    if (code.empty())
        return {InvokeStatus::NotFound, {}};

    py::gil_scoped_acquire gil;
    executing_.store(true, std::memory_order_release);
    try
    {
        // Evaluate in the script module's namespace so module-level
        // functions and variables are accessible.
        py::object result = py::eval(code, module_.attr("__dict__"));
        executing_.store(false, std::memory_order_release);

        return {InvokeStatus::Ok, py_to_json(result)};
    }
    catch (const py::error_already_set &e)
    {
        executing_.store(false, std::memory_order_release);
        LOGGER_ERROR("[{}] eval(): {}", log_tag_, e.what());
        if (api_->core())
            api_->core()->inc_script_error_count();
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
        if (req.is_eval)
        {
            req.promise.set_value(eval_direct_(req.name));
        }
        else
        {
            InvokeResponse resp;
            if (req.args.is_null() || req.args.empty())
                resp = execute_direct_(req.name.c_str());
            else
                resp = execute_direct_(req.name.c_str(), req.args);
            req.promise.set_value(std::move(resp));
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
    if (!spec.has_schema)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') called with has_schema=false",
                     log_tag_, type_name);
        return false;
    }

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
            in_slot_spec_    = spec;
        }
        else if (type_name == "OutSlotFrame")
        {
            out_slot_type_ = type;
            out_slot_spec_ = spec;
        }
        else if (type_name == "InFlexFrame")
        {
            in_fz_type_ = type; // mutable — flexzone is bidirectional per HEP-0002
            in_fz_spec_ = spec;
        }
        else if (type_name == "OutFlexFrame")
        {
            out_fz_type_ = type;
            out_fz_spec_ = spec;
        }
        else if (type_name == "InboxFrame")
        {
            inbox_type_ro_ = wrap_readonly_(type);
            inbox_spec_    = spec;
        }
        else
        {
            LOGGER_WARN("[{}] PythonEngine: unknown type_name '{}' — type built but not cached",
                        log_tag_, type_name);
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
    catch (const std::exception &)
    {
        return 0;
    }
}

// ============================================================================
// invoke_on_init — on_init(api)
// ============================================================================

void PythonEngine::invoke_on_init()
{
    // No process_pending_() needed: called before ctrl_thread_ is spawned,
    // so no non-owner threads exist yet to queue requests.
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
    InvokeResult result = InvokeResult::Error;
    if (is_callable(py_on_produce_))
    {
        py::gil_scoped_acquire g;
        try
        {
            PyTxChannel tx_ch;
            if (tx.slot != nullptr && !out_slot_type_.is_none())
                tx_ch.slot = make_slot_view_(out_slot_spec_, out_slot_type_, tx.slot, tx.slot_size, false);
            if (tx.fz != nullptr && tx.fz_size > 0 && !out_fz_type_.is_none())
                tx_ch.fz = make_slot_view_(out_fz_spec_, out_fz_type_, tx.fz, tx.fz_size, false);

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
    InvokeResult result = InvokeResult::Error;
    if (is_callable(py_on_consume_))
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
            if (rx.fz != nullptr && rx.fz_size > 0 && !in_fz_type_.is_none())
            {
                // Flexzone is mutable on both sides (HEP-0002).
                rx_ch.fz = make_slot_view_(
                    in_fz_spec_, in_fz_type_,
                    rx.fz, rx.fz_size, false);
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
    InvokeResult result = InvokeResult::Error;
    if (is_callable(py_on_process_))
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
            if (rx.fz != nullptr && rx.fz_size > 0 && !in_fz_type_.is_none())
                rx_ch.fz = make_slot_view_(in_fz_spec_, in_fz_type_, rx.fz, rx.fz_size, false);

            PyTxChannel tx_ch;
            if (tx.slot != nullptr && !out_slot_type_.is_none())
                tx_ch.slot = make_slot_view_(out_slot_spec_, out_slot_type_, tx.slot, tx.slot_size, false);
            if (tx.fz != nullptr && tx.fz_size > 0 && !out_fz_type_.is_none())
                tx_ch.fz = make_slot_view_(out_fz_spec_, out_fz_type_, tx.fz, tx.fz_size, false);

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
        process_pending_();
        return result;
    }

    // Inbox type must be cached at startup via register_slot_type("InboxFrame").
    if (inbox_type_ro_.is_none())
    {
        LOGGER_ERROR("[{}] invoke_on_inbox: InboxFrame type not registered — "
                     "inbox_schema must be configured and registered before use",
                     log_tag_);
        api_->core()->inc_script_error_count();
        process_pending_();
        return InvokeResult::Error;
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
        LOGGER_WARN("[{}] {} returned None — explicit 'return True' or "
                    "'return False' is required. Treating as error.",
                    log_tag_, callback_name);
        api_->core()->inc_script_error_count();
        if (stop_on_script_error_)
        {
            LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} [missing return]",
                         log_tag_, callback_name);
            api_->core()->request_stop();
        }
        return InvokeResult::Error;
    }

    // Wrong type.
    LOGGER_ERROR("[{}] {} returned non-boolean type '{}' — "
                 "expected 'return True' or 'return False'. Treating as error.",
                 log_tag_, callback_name,
                 py::str(py::type::of(ret).attr("__name__")).cast<std::string>());
    api_->core()->inc_script_error_count();
    if (stop_on_script_error_)
    {
        LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} [wrong return type]",
                     log_tag_, callback_name);
        api_->core()->request_stop();
    }
    return InvokeResult::Error;
}

InvokeResult PythonEngine::on_python_error_(const char *callback_name,
                                             const py::error_already_set &e)
{
    api_->core()->inc_script_error_count();
    LOGGER_ERROR("[{}] {} error: {}", log_tag_, callback_name, e.what());

    if (stop_on_script_error_)
    {
        LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} error",
                     log_tag_, callback_name);
        api_->core()->request_stop();
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

} // namespace pylabhub::scripting
