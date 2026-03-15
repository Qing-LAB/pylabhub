#pragma once
/**
 * @file hub_script.hpp
 * @brief HubScript — hub-specific PythonScriptHost: loads and drives the hub Python script.
 *
 * `HubScript` inherits from `scripting::PythonScriptHost` and implements
 * `do_python_work()` with hub-specific logic:
 *
 *  - Initialise the `PythonInterpreter` exec-namespace (for AdminShell).
 *  - Load the hub script package from `<script_dir>/__init__.py`.
 *  - Call `on_start(api)` once.
 *  - Signal the main thread (via `signal_ready_()`) that Python is ready.
 *  - Run the tick loop: broker query → health log → `on_tick(api, tick)` → close dispatch.
 *  - Call `on_stop(api)` once.
 *  - Release all Python objects.
 *  - Release the `PythonInterpreter` exec-namespace.
 *
 * `PythonScriptHost::do_initialize()` owns `py::scoped_interpreter` (the
 * `Py_Initialize` / `Py_Finalize` RAII guard) and calls `do_python_work()` from
 * the dedicated interpreter thread.
 *
 * ## Thread model
 *
 * ```
 * main thread          → startup_() → base_startup_(script_dir)
 *                             ↘ spawns interpreter thread (in ScriptHost base)
 *
 * interpreter thread   → PythonScriptHost::do_initialize()
 *                           → Py_Initialize
 *                           → do_python_work()
 *                               → PythonInterpreter::init_namespace_()
 *                               → load script/__init__.py
 *                               → on_start(api)
 *                               → signal_ready_()   ← main thread unblocks
 *                               → tick loop (stop_ checked every 10 ms)
 *                               → on_stop(api)
 *                               → PythonInterpreter::release_namespace_()
 *                           → Py_Finalize
 * ```
 *
 * ## GIL ownership
 *
 * The interpreter thread holds the GIL during Python calls.  Between ticks
 * (and between on_start and the first tick) the GIL is released so that
 * `AdminShell::exec()` can run concurrently.  The main thread never holds the GIL.
 *
 * ## Lifecycle
 *
 * ```cpp
 * HubScript::get_instance().set_broker(&broker);
 * HubScript::get_instance().set_shutdown_flag(&g_shutdown_requested);
 * RegisterDynamicModule(HubScript::GetLifecycleModule());
 * LoadModule("pylabhub::HubScript");          // blocks until signal_ready_()
 * // ... main wait loop ...
 * UnloadModule("pylabhub::HubScript");        // sets stop_, joins interpreter thread
 * // LifecycleGuard tears down remaining modules
 * ```
 *
 * ## Script interface
 *
 * Script lives at `<hub_dir>/script/__init__.py`.  Callbacks:
 *  - `on_start(api)` — called once after hub lifecycle is fully started.
 *  - `on_tick(api, tick)` — called every `tick_interval_ms` (default 1000 ms).
 *  - `on_stop(api)` — called once before Py_Finalize.
 */

#include "hub_script_api.hpp"
#include "python_script_host.hpp"
#include "utils/broker_service.hpp"

#include <atomic>
#include <chrono>

namespace pylabhub
{

/**
 * @class HubScript
 * @brief Singleton hub Python script host.
 *
 * Inherits `PythonScriptHost` for interpreter lifecycle;
 * implements `do_python_work()` with all hub-specific behaviour.
 */
class HubScript : public scripting::PythonScriptHost
{
public:
    // Singleton — not copyable or movable.
    HubScript(const HubScript&)            = delete;
    HubScript& operator=(const HubScript&) = delete;

    /** Returns the global singleton. Safe from any thread. */
    static HubScript& get_instance();

    /** Returns the `ModuleDef` for dynamic registration with `LifecycleManager`. */
    static utils::ModuleDef GetLifecycleModule();

    /**
     * @brief Point the HubScript at the running `BrokerService`.
     *
     * Call before `LoadModule("pylabhub::HubScript")`.
     */
    void set_broker(broker::BrokerService* broker) noexcept;

    /**
     * @brief Set the process-wide shutdown flag.
     *
     * Call before `LoadModule("pylabhub::HubScript")`.
     */
    void set_shutdown_flag(std::atomic<bool>* flag) noexcept;

    // -----------------------------------------------------------------------
    // Hub federation event push (HEP-CORE-0022) — called from broker thread
    // -----------------------------------------------------------------------

    /**
     * @brief Notify HubScript that a federation peer connected.
     *
     * Thread-safe: queued and delivered to `on_hub_connected(hub_uid, api)` on
     * the next tick (if the callback is defined in the user script).
     */
    void on_hub_peer_connected(const std::string& hub_uid);

    /**
     * @brief Notify HubScript that a federation peer disconnected.
     *
     * Thread-safe: queued and delivered to `on_hub_disconnected(hub_uid, api)`.
     */
    void on_hub_peer_disconnected(const std::string& hub_uid);

    /**
     * @brief Deliver a HUB_TARGETED_MSG to the HubScript.
     *
     * Thread-safe: queued and delivered to `on_hub_message(channel, payload, source_uid, api)`.
     */
    void on_hub_peer_message(const std::string& channel,
                             const std::string& payload,
                             const std::string& source_hub_uid);

    // -----------------------------------------------------------------------
    // Internal lifecycle hooks — public so anonymous-namespace lifecycle
    // functions in the .cpp can call them.
    // -----------------------------------------------------------------------

    /**
     * @brief Start the interpreter thread and block until Python is ready.
     *
     * Reads `HubConfig` for the script directory and tick intervals, then
     * calls `base_startup_(script_dir)` which spawns the interpreter thread.
     *
     * @throws std::runtime_error on Python initialization failure.
     */
    void startup_();

    /**
     * @brief Stop the interpreter thread and wait for Python to finalize.
     *
     * Calls `base_shutdown_()`.  Safe to call even if `startup_()` was never
     * called.
     */
    void shutdown_();

protected:
    // ScriptHost / PythonScriptHost hook — implemented in hub_script.cpp.
    void do_python_work(const std::filesystem::path& script_path) override;

private:
    HubScript() = default;
    ~HubScript() = default;

    broker::BrokerService* broker_{nullptr};
    std::atomic<bool>*     shutdown_flag_{nullptr};

    HubScriptAPI           api_;

    uint64_t               tick_count_{0};
    std::chrono::steady_clock::time_point start_time_{};

    int tick_interval_ms_{1000};
    int health_log_interval_ms_{60000};
};

} // namespace pylabhub
