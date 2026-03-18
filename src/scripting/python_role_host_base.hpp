#pragma once
/**
 * @file python_role_host_base.hpp
 * @brief PythonRoleHostBase — Python-specific common layer for all role script hosts.
 *
 * Inherits PythonScriptHost (interpreter management), composes RoleHostCore
 * (engine-agnostic infrastructure).  Provides the common do_python_work()
 * skeleton via virtual hooks for role-specific dispatch.
 *
 * ## What PythonRoleHostBase provides
 *
 * **From PythonScriptHost:**
 *  - Interpreter thread, Py_Initialize / Py_Finalize
 *  - base_startup_() / base_shutdown_() lifecycle
 *
 * **Own common code:**
 *  - do_python_work() skeleton (~120 lines shared across all roles)
 *  - FlexZone Python type building (build_flexzone_type_)
 *  - call_on_init / call_on_stop common wrappers
 *  - Message list builder (virtual; Consumer overrides to omit sender)
 *  - Common Python object management (fz_type_, fz_inst_, fz_mv_, api_obj_)
 *  - GIL release management (main_thread_release_)
 *  - startup_() / shutdown_() / signal_shutdown() identical for all roles
 *
 * **From RoleHostCore (composed):**
 *  - Thread-safe incoming message queue
 *  - Shutdown coordination flags
 *  - State flags (validate_only, script_load_ok, running)
 *  - FlexZone schema storage
 *
 * ## Subclass contract (virtual hooks)
 *
 * See protected section for the full list.  Each role subclass overrides these
 * to provide its specific behavior while reusing the common skeleton.
 *
 * See HEP-CORE-0011 for the ScriptHost abstraction framework.
 */

#include "python_script_host.hpp"
#include "role_host_core.hpp"

#include "utils/script_host_helpers.hpp"

#include <pybind11/pybind11.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace py = pybind11;

namespace pylabhub::scripting
{

/// Recursively convert nlohmann::json to py::object (dict, list, str, int, float, bool, None).
py::object json_to_py(const nlohmann::json &val);

/// StopReason is now in RoleHostCore (engine-agnostic).
using StopReason = RoleHostCore::StopReason;

class PythonRoleHostBase : public PythonScriptHost
{
  public:
    ~PythonRoleHostBase() override;

    PythonRoleHostBase(const PythonRoleHostBase &)            = delete;
    PythonRoleHostBase &operator=(const PythonRoleHostBase &) = delete;

    // ── Common lifecycle (identical for all roles) ───────────────────────────

    void startup_();
    void shutdown_() noexcept;
    void signal_shutdown() noexcept;

    // ── Common configuration ─────────────────────────────────────────────────

    void set_validate_only(bool v) noexcept { core_.validate_only = v; }
    void set_shutdown_flag(std::atomic<bool> *flag) noexcept { core_.g_shutdown = flag; }

    /**
     * @brief Block until notified or timeout_ms elapses.
     *
     * Used by run_role_main_loop() instead of sleep_for() so that stop_role()
     * can wake the monitoring thread immediately via core_.notify_incoming().
     */
    void wait_for_wakeup(int timeout_ms) noexcept { core_.wait_for_incoming(timeout_ms); }

    // ── Common post-startup results ──────────────────────────────────────────

    [[nodiscard]] bool script_load_ok() const noexcept { return core_.script_load_ok; }
    [[nodiscard]] bool is_running()     const noexcept { return core_.running; }

  protected:
    PythonRoleHostBase() = default;

    // ── Composed engine-agnostic infrastructure ──────────────────────────────

    RoleHostCore core_;

    // ── Python objects common to all roles ────────────────────────────────────

    py::object fz_type_{py::none()};   ///< FlexZone ctypes/numpy type
    py::object fz_inst_{py::none()};   ///< FlexZone instance (view into SHM)
    py::object fz_mv_{py::none()};     ///< FlexZone memoryview
    py::object api_obj_{py::none()};   ///< Role API as py::object
    py::object py_on_init_{py::none()}; ///< on_init callback
    py::object py_on_stop_{py::none()}; ///< on_stop callback

    std::optional<py::gil_scoped_release> main_thread_release_;
    std::thread                           ctrl_thread_;

    // ── Common implementations ───────────────────────────────────────────────

    /** do_python_work() skeleton — the main shared code block. */
    void do_python_work(const std::filesystem::path &script_path) override;

    /** Build fz_type_ from core_.fz_spec.  GIL must be held.  Returns false on error. */
    bool build_flexzone_type_();

    /** Common on_init wrapper: calls py_on_init_, handles errors, updates fz checksum. */
    void call_on_init_common_();

    /** Common on_stop wrapper: calls py_on_stop_, handles errors. */
    void call_on_stop_common_();

    /** Release all common Python references.  GIL must be held. */
    void clear_common_pyobjects_();

    /**
     * @brief Build Python message list from incoming messages.
     *
     * Default: emits (sender, bytes) tuples.
     * Consumer overrides to emit bare bytes (no sender).
     */
    virtual py::list build_messages_list_(std::vector<IncomingMessage> &msgs);

    // ── Static helpers ───────────────────────────────────────────────────────

    /**
     * @brief Build a Python type (ctypes struct or numpy dtype) from a SchemaSpec.
     *
     * @param spec         The schema spec to build from.
     * @param type_out     [out] The built Python type.
     * @param size_out     [out] The computed size in bytes.
     * @param struct_name  Name for the ctypes struct (e.g., "SlotFrame").
     * @param readonly     If true and exposure==Ctypes, wraps the generated struct
     *                     with wrap_as_readonly_ctypes() to block field writes at
     *                     the Python level. Use for read-side types only (consumer
     *                     in_slot, processor in_slot).
     * @return true on success; false should not happen (throws on error).
     */
    static bool build_schema_type_(const SchemaSpec &spec, py::object &type_out,
                                   size_t &size_out, const char *struct_name,
                                   bool readonly = false);

    /** Print a single slot/flexzone layout for --validate mode. */
    static void print_slot_layout_(const py::object &type, const SchemaSpec &spec,
                                   size_t size, const char *label);

    // ── Virtual hooks for role dispatch ──────────────────────────────────────

    // Identity
    virtual const char *role_tag()  const = 0; ///< Short log prefix: "prod", "cons", "proc"
    virtual const char *role_name() const = 0; ///< Full role: "producer", "consumer", "processor"
    virtual std::string role_uid()  const = 0; ///< From config (e.g., config_.producer_uid)

    // Script loading
    virtual std::string script_base_dir() const = 0; ///< Config script path
    virtual std::string script_type_str() const = 0; ///< "python" (from config)
    virtual std::string required_callback_name() const = 0; ///< "on_produce" etc.

    // API wiring (called with GIL held, before script load)
    virtual void wire_api_identity() = 0;

    // Callback extraction (called with GIL held, after script module loaded)
    virtual void extract_callbacks(py::module_ &mod) = 0;
    virtual bool has_required_callback() const = 0;

    // Schema and validation
    virtual bool build_role_types() = 0;
    virtual void print_validate_layout() = 0;

    // Lifecycle (called with GIL held)
    virtual bool start_role() = 0;
    virtual void stop_role() = 0;
    virtual void cleanup_on_start_failure() = 0;
    virtual void clear_role_pyobjects() = 0;

    // Error handling
    virtual void on_script_error() = 0; ///< Typically: api_.increment_script_errors()

    // call_on_stop guard: returns true if the connection needed for on_stop exists
    virtual bool has_connection_for_stop() const = 0;

    // FlexZone checksum update after on_init (default: no-op)
    virtual void update_fz_checksum_after_init() {}
};

} // namespace pylabhub::scripting
