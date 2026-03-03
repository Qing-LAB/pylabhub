// src/scripting/python_role_host_base.cpp
/**
 * @file python_role_host_base.cpp
 * @brief PythonRoleHostBase — common do_python_work() skeleton and helpers.
 *
 * This file contains the ~120-line do_python_work() skeleton that was previously
 * duplicated across all three role script hosts (producer, consumer, processor).
 * Role-specific behavior is dispatched via virtual hooks.
 */
#include "python_role_host_base.hpp"

#include "utils/logger.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <chrono>
#include <thread>

namespace py = pybind11;

namespace pylabhub::scripting
{

// ============================================================================
// Destructor — safety net (thread should already be joined by subclass dtor)
// ============================================================================

PythonRoleHostBase::~PythonRoleHostBase()
{
    shutdown_();
}

// ============================================================================
// Common lifecycle
// ============================================================================

void PythonRoleHostBase::startup_()
{
    base_startup_({});
}

void PythonRoleHostBase::shutdown_() noexcept
{
    base_shutdown_();
}

void PythonRoleHostBase::signal_shutdown() noexcept
{
    core_.shutdown_requested.store(true, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
}

// ============================================================================
// do_python_work — the common skeleton
// ============================================================================

void PythonRoleHostBase::do_python_work(const std::filesystem::path & /*script_path*/)
{
    // GIL held on entry.

    // ── Early exit: signal fired before interpreter thread ran ────────────────
    if (stop_.load(std::memory_order_acquire))
    {
        core_.script_load_ok = false;
        signal_ready_();
        return; // GIL held ✓
    }

    // ── Wire API identity (role-specific) ────────────────────────────────────
    wire_api_identity();

    // ── Load script module ───────────────────────────────────────────────────
    try
    {
        std::string uid_hex   = "00000000";
        const auto  last_dash = role_uid().rfind('-');
        if (last_dash != std::string::npos)
            uid_hex = role_uid().substr(last_dash + 1);

        py::module_ mod = import_role_script_module(
            role_name(), "script", script_base_dir(), uid_hex, script_type_str());

        extract_callbacks(mod);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] Failed to load script from '{}': {}",
                     role_tag(), script_base_dir(), e.what());
        core_.script_load_ok = false;
        signal_ready_();
        return; // GIL held ✓
    }

    if (!has_required_callback())
    {
        LOGGER_ERROR("[{}] Script has no '{}' function — {} not started",
                     role_tag(), required_callback_name(), role_name());
        core_.script_load_ok = false;
        signal_ready_();
        return; // GIL held ✓
    }

    core_.script_load_ok = true;

    // ── Validate mode: print layout and exit ─────────────────────────────────
    if (core_.validate_only)
    {
        if (!build_role_types())
        {
            signal_ready_();
            return; // GIL held ✓
        }
        print_validate_layout();
        signal_ready_();
        return; // GIL held ✓
    }

    // ── Start role ───────────────────────────────────────────────────────────
    if (!start_role())
    {
        LOGGER_ERROR("[{}] Failed to start {}", role_tag(), role_name());
        cleanup_on_start_failure();
        signal_ready_();
        return; // GIL held ✓
    }
    // GIL released inside start_role() via main_thread_release_.emplace().

    core_.running = true;
    signal_ready_(); // Unblocks startup_() on main thread.

    // ── Wait loop ────────────────────────────────────────────────────────────
    // GIL not held during the wait loop.
    while (!stop_.load(std::memory_order_acquire) &&
           !core_.shutdown_requested.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    // If internal shutdown (api.stop()), propagate to the main thread.
    if (core_.shutdown_requested.load() && core_.g_shutdown)
        core_.g_shutdown->store(true, std::memory_order_release);

    // ── Stop role ────────────────────────────────────────────────────────────
    // Re-acquire GIL before calling stop_role().
    main_thread_release_.reset();
    // GIL held from here.

    stop_role();
    core_.running = false;

    LOGGER_INFO("[{}] ScriptHost: all done; returning to PythonScriptHost", role_tag());
    // Return with GIL held ✓
    // py::scoped_interpreter destructor: Py_Finalize
}

// ============================================================================
// build_flexzone_type_ — build fz_type_ from core_.fz_spec
// ============================================================================

bool PythonRoleHostBase::build_flexzone_type_()
{
    if (!core_.fz_spec.has_schema)
        return true;

    core_.has_fz = true;

    if (core_.fz_spec.exposure == SlotExposure::Ctypes)
    {
        fz_type_            = build_ctypes_struct(core_.fz_spec, "FlexFrame");
        core_.schema_fz_size = ctypes_sizeof(fz_type_);
    }
    else
    {
        fz_type_            = build_numpy_dtype(core_.fz_spec);
        core_.schema_fz_size = fz_type_.attr("itemsize").cast<size_t>();
        if (!core_.fz_spec.numpy_shape.empty())
        {
            size_t total = 1;
            for (auto d : core_.fz_spec.numpy_shape)
                total *= static_cast<size_t>(d);
            core_.schema_fz_size = total * core_.schema_fz_size;
        }
    }

    // Round up to 4 KiB page boundary.
    core_.schema_fz_size = (core_.schema_fz_size + 4095U) & ~size_t{4095U};
    return true;
}

// ============================================================================
// build_schema_type_ — static helper for building a single slot/fz type
// ============================================================================

bool PythonRoleHostBase::build_schema_type_(const SchemaSpec &spec, py::object &type_out,
                                            size_t &size_out, const char *struct_name)
{
    if (!spec.has_schema)
        return true;

    if (spec.exposure == SlotExposure::Ctypes)
    {
        type_out = build_ctypes_struct(spec, struct_name);
        size_out = ctypes_sizeof(type_out);
    }
    else
    {
        type_out = build_numpy_dtype(spec);
        size_out = type_out.attr("itemsize").cast<size_t>();
        if (!spec.numpy_shape.empty())
        {
            size_t total = 1;
            for (auto d : spec.numpy_shape)
                total *= static_cast<size_t>(d);
            size_out = total * size_out;
        }
    }
    return true;
}

// ============================================================================
// print_slot_layout_ — static helper for --validate mode
// ============================================================================

void PythonRoleHostBase::print_slot_layout_(const py::object &type, const SchemaSpec &spec,
                                            size_t size, const char *label)
{
    if (!spec.has_schema)
        return;
    if (spec.exposure == SlotExposure::Ctypes)
        print_ctypes_layout(type, label, size);
    else
        print_numpy_layout(type, spec, label);
}

// ============================================================================
// call_on_init_common_ / call_on_stop_common_
// ============================================================================

void PythonRoleHostBase::call_on_init_common_()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        on_script_error();
        LOGGER_ERROR("[{}] on_init error: {}", role_tag(), e.what());
    }
    update_fz_checksum_after_init();
}

void PythonRoleHostBase::call_on_stop_common_()
{
    if (!is_callable(py_on_stop_) || !has_connection_for_stop())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        on_script_error();
        LOGGER_ERROR("[{}] on_stop error: {}", role_tag(), e.what());
    }
}

// ============================================================================
// clear_common_pyobjects_
// ============================================================================

void PythonRoleHostBase::clear_common_pyobjects_()
{
    fz_inst_    = py::none();
    fz_mv_      = py::none();
    api_obj_    = py::none();
    py_on_init_ = py::none();
    py_on_stop_ = py::none();
    fz_type_    = py::none();
}

// ============================================================================
// build_messages_list_ — default: (sender, bytes) tuples
// ============================================================================

py::list PythonRoleHostBase::build_messages_list_(std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
        lst.append(py::make_tuple(
            m.sender,
            py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
    return lst;
}

} // namespace pylabhub::scripting
