/**
 * @file hub_script.cpp
 * @brief HubScript — hub-specific Python work: namespace, script load, tick loop.
 *
 * `PythonScriptHost::do_initialize()` (base class) owns the interpreter lifetime:
 *   - Derives Python home, configures PyConfig, creates py::scoped_interpreter.
 *   - Calls `HubScript::do_python_work()` with the GIL held.
 *   - After do_python_work() returns, Py_Finalize runs via scoped_interpreter.
 *
 * `HubScript::do_python_work()` owns all hub-specific Python work:
 *   - PythonInterpreter admin-shell namespace init/release.
 *   - Script package loading, callback lookup, on_start / on_tick / on_stop.
 *   - Tick loop driving broker queries and channel-close dispatch.
 */
#include "hub_script.hpp"
#include "hub_script_api.hpp"
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <filesystem>
#include <stdexcept>

namespace py = pybind11;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Script package loader (mirrors import_role_script_module in actor_worker_helpers)
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Load the hub script package from `<script_dir>/__init__.py`.
 *
 * The module is registered in sys.modules under `_plh_hub_<uid_hex>` to
 * prevent collisions when the same Python environment hosts multiple hubs.
 *
 * MUST be called with the GIL held.
 *
 * @throws std::runtime_error  if `__init__.py` is not found.
 * @throws py::error_already_set  on Python import error.
 */
py::module_ load_hub_script_package(const fs::path& script_dir,
                                     const std::string& hub_uid)
{
    const fs::path init_py = script_dir / "__init__.py";
    if (!fs::exists(init_py))
    {
        throw std::runtime_error(
            "HubScript: script package not found: '" + init_py.string() + "'");
    }

    // Derive a stable hex suffix from the hub UID (last 8 chars).
    std::string uid_hex = hub_uid;
    if (uid_hex.size() > 8)
        uid_hex = uid_hex.substr(uid_hex.size() - 8);
    const std::string alias = "_plh_hub_" + uid_hex;

    py::module_ importlib_util = py::module_::import("importlib.util");
    py::module_ sys_mod        = py::module_::import("sys");
    py::dict    sys_modules    = sys_mod.attr("modules").cast<py::dict>();

    // Enable relative imports: submodule_search_locations = [script_dir].
    py::list search_locs;
    search_locs.append(script_dir.string());

    py::object spec = importlib_util.attr("spec_from_file_location")(
        alias, init_py.string(),
        py::arg("submodule_search_locations") = search_locs);

    if (spec.is_none())
    {
        throw std::runtime_error(
            "HubScript: spec_from_file_location failed for '" + init_py.string() + "'");
    }

    py::object mod = importlib_util.attr("module_from_spec")(spec);
    sys_modules[alias.c_str()] = mod;
    spec.attr("loader").attr("exec_module")(mod);

    return mod.cast<py::module_>();
}

bool is_callable(const py::object& obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

// ---------------------------------------------------------------------------
// Static lifecycle callback functions — bridge to the singleton.
// ---------------------------------------------------------------------------

void do_hub_script_startup(const char* /*arg*/)
{
    pylabhub::HubScript::get_instance().startup_();
}

void do_hub_script_shutdown(const char* /*arg*/)
{
    pylabhub::HubScript::get_instance().shutdown_();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HubScript
// ---------------------------------------------------------------------------

namespace pylabhub
{

// static
HubScript& HubScript::get_instance()
{
    static HubScript instance;
    return instance;
}

// static
utils::ModuleDef HubScript::GetLifecycleModule()
{
    utils::ModuleDef module("pylabhub::HubScript");
    module.add_dependency("pylabhub::utils::Logger");
    module.add_dependency("pylabhub::HubConfig");
    // Note: PythonInterpreter is NOT listed as a dependency. HubScript owns
    // the interpreter — PythonInterpreter's startup/shutdown are no-ops.
    // HubConfig is needed so startup_() can read hub_script_dir() etc.
    module.set_startup(&do_hub_script_startup);
    module.set_shutdown(&do_hub_script_shutdown,
                        std::chrono::milliseconds(pylabhub::kLongTimeoutMs));
    return module;
}

void HubScript::set_broker(broker::BrokerService* broker) noexcept
{
    broker_ = broker;
    api_.set_broker(broker);
}

void HubScript::set_shutdown_flag(std::atomic<bool>* flag) noexcept
{
    shutdown_flag_ = flag;
    api_.set_shutdown_flag(flag);
}

void HubScript::startup_()
{
    const auto& cfg = HubConfig::get_instance();
    tick_interval_ms_       = cfg.tick_interval_ms();
    health_log_interval_ms_ = cfg.health_log_interval_ms();

    const fs::path script_dir = cfg.hub_script_dir();

    // base_startup_ spawns the interpreter thread and blocks until
    // signal_ready_() is called from do_python_work() (or throws on error).
    base_startup_(script_dir);
}

void HubScript::on_hub_peer_connected(const std::string& hub_uid)
{
    api_.push_hub_connected(hub_uid);
}

void HubScript::on_hub_peer_disconnected(const std::string& hub_uid)
{
    api_.push_hub_disconnected(hub_uid);
}

void HubScript::on_hub_peer_message(const std::string& channel,
                                     const std::string& payload,
                                     const std::string& source_hub_uid)
{
    api_.push_hub_message(channel, payload, source_hub_uid);
}

void HubScript::shutdown_()
{
    // base_shutdown_ sets stop_ and joins the interpreter thread.
    base_shutdown_();

    LOGGER_INFO("HubScript: interpreter thread joined; Python finalized");

    // Null out dangling pointers so they are not accidentally reused.
    broker_        = nullptr;
    shutdown_flag_ = nullptr;
    api_.set_broker(nullptr);
    api_.set_shutdown_flag(nullptr);
}

// ---------------------------------------------------------------------------
// do_python_work — called by PythonScriptHost::do_initialize() on interpreter thread.
//
// Preconditions:
//   - py::scoped_interpreter is live (Py_Initialize done).
//   - GIL is held on entry.
//   - PythonScriptHost has derived Python home and set up PyConfig.
//
// Postconditions:
//   - All py::object locals have been released (Py_Finalize follows).
//   - PythonInterpreter namespace is released.
//   - Returns normally regardless of script errors (all exceptions caught here).
// ---------------------------------------------------------------------------

void HubScript::do_python_work(const fs::path& script_path)
{
    using Clock = std::chrono::steady_clock;

    LOGGER_INFO("HubScript: Python {} on interpreter thread", Py_GetVersion());

    // ------------------------------------------------------------------
    // Phase 1: Set up PythonInterpreter namespace.
    //   - Creates __main__.__dict__ as the persistent exec() namespace.
    //   - Imports pylabhub.
    //   - Sets interpreter ready_ = true (AdminShell exec() becomes available).
    // GIL held throughout.
    // ------------------------------------------------------------------
    PythonInterpreter::get_instance().init_namespace_();

    // ------------------------------------------------------------------
    // Phase 2: Load hub script package and look up callbacks.
    // Errors here are non-fatal: the hub runs without a user script.
    // GIL held throughout.
    // ------------------------------------------------------------------
    py::object script_module{py::none()};
    py::object on_start_fn{py::none()};
    py::object on_tick_fn{py::none()};
    py::object on_stop_fn{py::none()};
    // HEP-CORE-0022 federation callbacks (optional)
    py::object on_hub_connected_fn{py::none()};
    py::object on_hub_disconnected_fn{py::none()};
    py::object on_hub_message_fn{py::none()};
    bool script_loaded = false;

    [&]() -> void
    {
        // "script_path" is empty when hub.json has no "script" block, or when
        // "script.type" was absent (both are normal: hub runs without a user script).
        if (script_path.empty())
        {
            LOGGER_INFO("HubScript: no hub script configured — running without user script");
            return;
        }

        // Script type-specific subdir and __init__.py are REQUIRED.
        // Missing either is an error — caught here and logged; hub continues.
        try
        {
            if (!fs::exists(script_path))
            {
                throw std::runtime_error(
                    "HubScript: script directory '" + script_path.string() +
                    "' not found. Expected: <hub_dir>/script/<type>/ "
                    "(e.g. script/python/ or script/lua/).");
            }
            if (!fs::exists(script_path / "__init__.py"))
            {
                throw std::runtime_error(
                    "HubScript: '__init__.py' not found in '" + script_path.string() +
                    "'. The Python script directory must be a package "
                    "(contain __init__.py with on_start/on_tick/on_stop callbacks).");
            }

            // Import the C++ API module so type bindings are wired.
            py::module_::import("hub_script_api");

            LOGGER_INFO("HubScript: loading script package from '{}'",
                        script_path.string());
            script_module = load_hub_script_package(script_path,
                                                    HubConfig::get_instance().hub_uid());

            on_start_fn            = py::getattr(script_module, "on_start",             py::none());
            on_tick_fn             = py::getattr(script_module, "on_tick",              py::none());
            on_stop_fn             = py::getattr(script_module, "on_stop",              py::none());
            on_hub_connected_fn    = py::getattr(script_module, "on_hub_connected",     py::none());
            on_hub_disconnected_fn = py::getattr(script_module, "on_hub_disconnected",  py::none());
            on_hub_message_fn      = py::getattr(script_module, "on_hub_message",       py::none());

            LOGGER_INFO("HubScript: callbacks — on_start={} on_tick={} on_stop={} "
                        "on_hub_connected={} on_hub_disconnected={} on_hub_message={}",
                        is_callable(on_start_fn)            ? "yes" : "no",
                        is_callable(on_tick_fn)             ? "yes" : "no",
                        is_callable(on_stop_fn)             ? "yes" : "no",
                        is_callable(on_hub_connected_fn)    ? "yes" : "no",
                        is_callable(on_hub_disconnected_fn) ? "yes" : "no",
                        is_callable(on_hub_message_fn)      ? "yes" : "no");
            script_loaded = true;
        }
        catch (const py::error_already_set& e)
        {
            LOGGER_ERROR("HubScript: failed to load script package: {}", e.what());
        }
        catch (const std::exception& e)
        {
            LOGGER_ERROR("HubScript: failed to load script package: {}", e.what());
        }
    }();

    // ------------------------------------------------------------------
    // Phase 3: Call on_start (GIL held).
    // ------------------------------------------------------------------
    if (script_loaded && is_callable(on_start_fn))
    {
        try
        {
            on_start_fn(py::cast(&api_));
        }
        catch (const py::error_already_set& e)
        {
            LOGGER_ERROR("HubScript: on_start() raised exception: {}", e.what());
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: Signal "Python ready" to base_startup_() on the main thread.
    //
    // signal_ready_() sets ready_=true and unblocks init_future_.get() in
    // ScriptHost::base_startup_().  After this, startup_() returns and the
    // main loop starts.  AdminShell can call exec() from now on.
    // ------------------------------------------------------------------
    start_time_ = Clock::now();
    signal_ready_();
    LOGGER_INFO("HubScript: initialized; tick interval={}ms", tick_interval_ms_);

    // ------------------------------------------------------------------
    // Phase 5: Tick loop.
    //
    // py::gil_scoped_release releases the GIL so that AdminShell::exec()
    // calls can run between ticks.  Inside each tick, py::gil_scoped_acquire
    // briefly re-acquires the GIL for Python calls only.
    // ------------------------------------------------------------------
    {
        py::gil_scoped_release outer_release; // GIL released; other threads can exec()

        auto next_tick      = Clock::now();
        auto last_tick_time = Clock::now();

        const int health_log_ticks =
            (tick_interval_ms_ > 0)
            ? std::max(1, health_log_interval_ms_ / tick_interval_ms_)
            : 60;

        while (!stop_.load(std::memory_order_acquire))
        {
            // Sleep in short slices so we can react to the stop flag promptly.
            static constexpr int kSliceMs = 10;
            while (!stop_.load(std::memory_order_acquire))
            {
                if (Clock::now() >= next_tick)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
            }
            if (stop_.load(std::memory_order_acquire))
                break;

            const auto tick_start = Clock::now();
            const uint64_t elapsed_ms =
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        tick_start - last_tick_time).count());
            const uint64_t uptime_ms =
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        tick_start - start_time_).count());

            ++tick_count_;
            last_tick_time = tick_start;
            next_tick      = tick_start + std::chrono::milliseconds(tick_interval_ms_);

            // 1. Query broker (no GIL needed; broker is C++ only).
            const broker::ChannelSnapshot snap =
                broker_ ? broker_->query_channel_snapshot()
                        : broker::ChannelSnapshot{};

            const int n_ready   = snap.count_by_status("Ready");
            const int n_pending = snap.count_by_status("PendingReady");
            const int n_closing = snap.count_by_status("Closing");
            const int n_total   = static_cast<int>(snap.channels.size());

            // 2. Periodic health log (no GIL needed).
            if (tick_count_ % static_cast<uint64_t>(health_log_ticks) == 0)
            {
                LOGGER_INFO("HubScript: health [tick={}] {} channels:"
                            " {} ready, {} pending, {} closing",
                            tick_count_, n_total, n_ready, n_pending, n_closing);
            }

            // 3. If no script loaded, skip GIL acquisition.
            if (script_module.is_none()) // pointer check; safe without GIL
                continue;

            // 4. Acquire GIL for Python calls.
            std::vector<std::string> closes;
            {
                py::gil_scoped_acquire gil;

                // 4a. Dispatch hub federation events queued from the broker thread.
                {
                    const auto hub_events = api_.take_hub_events();
                    for (const auto& ev : hub_events)
                    {
                        try
                        {
                            if (ev.type == HubScriptAPI::HubEvent::Type::Connected &&
                                is_callable(on_hub_connected_fn))
                            {
                                on_hub_connected_fn(ev.hub_uid, py::cast(&api_));
                            }
                            else if (ev.type == HubScriptAPI::HubEvent::Type::Disconnected &&
                                     is_callable(on_hub_disconnected_fn))
                            {
                                on_hub_disconnected_fn(ev.hub_uid, py::cast(&api_));
                            }
                            else if (ev.type == HubScriptAPI::HubEvent::Type::Message &&
                                     is_callable(on_hub_message_fn))
                            {
                                on_hub_message_fn(ev.channel, ev.payload,
                                                  ev.hub_uid, py::cast(&api_));
                            }
                        }
                        catch (const py::error_already_set& e)
                        {
                            LOGGER_ERROR("HubScript: hub event callback raised: {}", e.what());
                        }
                    }
                }

                api_.set_snapshot(snap);

                if (is_callable(on_tick_fn))
                {
                    HubTickInfo tick_info;
                    tick_info.set_tick_count(tick_count_);
                    tick_info.set_elapsed_ms(elapsed_ms);
                    tick_info.set_uptime_ms(uptime_ms);
                    tick_info.set_channels_ready(n_ready);
                    tick_info.set_channels_pending(n_pending);
                    tick_info.set_channels_closing(n_closing);

                    try
                    {
                        on_tick_fn(py::cast(&api_), py::cast(&tick_info));
                    }
                    catch (const py::error_already_set& e)
                    {
                        LOGGER_ERROR("HubScript: on_tick() raised exception: {}",
                                     e.what());
                    }
                }

                closes = api_.take_pending_closes();
                for (const auto& ch : closes)
                    LOGGER_INFO("HubScript: script requested close of channel '{}'", ch);
            } // GIL released here

            // 5. Dispatch closes without GIL (broker is C++, thread-safe).
            if (broker_)
                for (const auto& ch : closes)
                    broker_->request_close_channel(ch);
        }
    } // outer_release destructor: GIL re-acquired

    // ------------------------------------------------------------------
    // Phase 6: Call on_stop (GIL held after outer_release scope exits).
    // ------------------------------------------------------------------
    if (script_loaded && is_callable(on_stop_fn))
    {
        try
        {
            on_stop_fn(py::cast(&api_));
        }
        catch (const py::error_already_set& e)
        {
            LOGGER_ERROR("HubScript: on_stop() raised exception: {}", e.what());
        }
    }

    // ------------------------------------------------------------------
    // Phase 7: Release Python objects and PythonInterpreter namespace.
    //
    // All py::object locals must be released BEFORE returning from this
    // function, because py::scoped_interpreter's destructor (Py_Finalize)
    // runs in PythonScriptHost::do_initialize() immediately after we return.
    // ------------------------------------------------------------------
    on_stop_fn             = py::none();
    on_tick_fn             = py::none();
    on_start_fn            = py::none();
    on_hub_message_fn      = py::none();
    on_hub_disconnected_fn = py::none();
    on_hub_connected_fn    = py::none();
    script_module          = py::none();

    PythonInterpreter::get_instance().release_namespace_();

    LOGGER_INFO("HubScript: Python objects released; returning to PythonScriptHost");
}

} // namespace pylabhub
