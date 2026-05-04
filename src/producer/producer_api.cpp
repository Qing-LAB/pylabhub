/**
 * @file producer_api.cpp
 * @brief ProducerAPI Python-wrapping methods + pylabhub_producer pybind11 module.
 *
 * Phase 2: All C++ logic delegates to RoleAPIBase. This file only contains
 * Python-specific type conversions and the pybind11 module registration.
 */
#include "utils/script_engine.hpp"
#include "producer_api.hpp"

#include "plh_version_registry.hpp"
#include "python_helpers.hpp"
#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"
#include "utils/metrics_json.hpp"
#include "metrics_pydict.hpp"
#include "../scripting/json_py_helpers.hpp"   // detail::json_to_py (S5)
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace scripting = pylabhub::scripting;

namespace pylabhub::producer
{

// ============================================================================
// Python-wrapping methods (delegate to RoleAPIBase with type conversion)
// ============================================================================

py::object ProducerAPI::flexzone(std::optional<int> /*side*/) const
{
    // Producer has only Tx side; side arg is ignored (accepted for API consistency).
    if (!tx_flexzone_obj_.has_value())
        return py::none();
    return *tx_flexzone_obj_;
}

// ── Band pub/sub (HEP-CORE-0030) ─────────────────────────────────────────────

py::object ProducerAPI::band_join(const std::string &channel)
{
    auto result = base_->band_join(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

void ProducerAPI::band_broadcast(const std::string &channel, py::dict body)
{
    auto json_mod = py::module_::import("json");
    std::string body_str = json_mod.attr("dumps")(body).cast<std::string>();
    base_->band_broadcast(channel, nlohmann::json::parse(body_str));
}

py::object ProducerAPI::band_members(const std::string &channel)
{
    auto result = base_->band_members(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

py::dict ProducerAPI::metrics() const
{
    // S5: was `json.loads(j.dump())` round-trip — replaced with the
    // shared fast-path walker (src/scripting/json_py_helpers.hpp).
    // Same converter `python_engine.cpp::execute_direct_` already
    // uses on the dispatch hot path; semantics match for metrics
    // payloads (no NaN/Inf, no deep recursion — see header docstring
    // for the divergence rows).
    return scripting::detail::json_to_py(base_->snapshot_metrics_json())
        .cast<py::dict>();
}

uint64_t ProducerAPI::slot_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->slot_logical_size(
        side.has_value() ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)} : std::nullopt));
}

uint64_t ProducerAPI::flexzone_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->flexzone_logical_size(
        side.has_value() ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)} : std::nullopt));
}

static std::optional<scripting::ChannelSide> to_channel_side(std::optional<int> side)
{
    if (!side.has_value())
        return std::nullopt;
    return static_cast<scripting::ChannelSide>(*side);
}

py::object ProducerAPI::spinlock(std::size_t index, std::optional<int> side)
{
    try
    {
        return py::cast(scripting::SpinLockPy{base_->get_spinlock(index, to_channel_side(side))},
                        py::return_value_policy::move);
    }
    catch (const std::exception &e)
    {
        throw py::value_error(e.what());
    }
}

uint32_t ProducerAPI::spinlock_count(std::optional<int> side) const
{
    try
    {
        return base_->spinlock_count(to_channel_side(side));
    }
    catch (const std::exception &e)
    {
        throw py::value_error(e.what());
    }
}

// ============================================================================
// Inbox (Python-specific wrapping)
// ============================================================================

py::object ProducerAPI::open_inbox(const std::string &target_uid)
{
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;

    std::optional<scripting::RoleAPIBase::InboxOpenResult> result;
    {
        py::gil_scoped_release release;
        result = base_->open_inbox_client(target_uid);
    }
    if (!result)
        return py::none();

    py::object slot_type = result->spec.fields.empty()
        ? py::none()
        : scripting::build_ctypes_struct(result->spec, "InboxSlot");

    py::object handle = py::cast(
        scripting::InboxHandle(std::move(result->client), std::move(result->spec),
                               std::move(slot_type), result->item_size),
        py::return_value_policy::move);
    inbox_cache_[target_uid] = handle;
    return handle;
}

bool ProducerAPI::wait_for_role(const std::string &uid, int timeout_ms)
{
    py::gil_scoped_release release;
    return base_->wait_for_role(uid, timeout_ms);
}

void ProducerAPI::clear_inbox_cache()
{
    for (auto &[uid, handle_obj] : inbox_cache_)
    {
        try
        {
            handle_obj.cast<scripting::InboxHandle &>().clear_pyobjects();
        }
        catch (const std::exception &e)
        {
            // Cleanup path — must not throw out (called during stop).
            // Log so broken handles do not silently leak Python refs;
            // continue draining the rest of the cache.
            LOGGER_WARN("ProducerAPI: clear_inbox_cache uid='{}' threw: {}",
                        uid, e.what());
        }
        catch (...)
        {
            LOGGER_WARN("ProducerAPI: clear_inbox_cache uid='{}' "
                        "threw (non-std exception)", uid);
        }
    }
    inbox_cache_.clear();
}

} // namespace pylabhub::producer

// ============================================================================
// pybind11 embedded module: pylabhub_producer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_producer, m) // NOLINT
{
    namespace producer = pylabhub::producer;
    namespace scripting = pylabhub::scripting;

    // Direction objects — required for invoke_produce(tx, msgs, api).
    py::class_<scripting::PyTxChannel>(m, "TxChannel")
        .def_readwrite("slot", &scripting::PyTxChannel::slot);

    py::class_<scripting::PyRxChannel>(m, "RxChannel")
        .def_readwrite("slot", &scripting::PyRxChannel::slot);

    py::class_<scripting::PyInboxMsg>(m, "InboxMsg")
        .def_readonly("data",       &scripting::PyInboxMsg::data)
        .def_readonly("sender_uid", &scripting::PyInboxMsg::sender_uid)
        .def_readonly("seq",        &scripting::PyInboxMsg::seq);

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    });

    py::class_<scripting::SpinLockPy>(m, "SpinLock")
        .def("lock",    &scripting::SpinLockPy::lock)
        .def("unlock",  &scripting::SpinLockPy::unlock)
        .def("try_lock_for", &scripting::SpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process", &scripting::SpinLockPy::is_locked_by_current_process)
        .def("__enter__", &scripting::SpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &scripting::SpinLockPy::exit);

    py::class_<scripting::InboxHandle>(m, "InboxHandle")
        .def("acquire",  &scripting::InboxHandle::acquire)
        .def("send",     &scripting::InboxHandle::send, py::arg("timeout_ms") = 5000)
        .def("discard",  &scripting::InboxHandle::discard)
        .def("is_ready", &scripting::InboxHandle::is_ready)
        .def("close",    &scripting::InboxHandle::close);

    py::class_<producer::ProducerAPI>(m, "ProducerAPI")
        .def("uid",          &producer::ProducerAPI::uid)
        .def("name",         &producer::ProducerAPI::name)
        .def("channel",      &producer::ProducerAPI::channel)
        .def("log_level",    &producer::ProducerAPI::log_level)
        .def("script_dir",   &producer::ProducerAPI::script_dir)
        .def("role_dir",     &producer::ProducerAPI::role_dir)
        .def("logs_dir",     &producer::ProducerAPI::logs_dir)
        .def("run_dir",      &producer::ProducerAPI::run_dir)
        .def("stop",         &producer::ProducerAPI::stop)
        .def("set_critical_error",    &producer::ProducerAPI::set_critical_error)
        .def("critical_error",        &producer::ProducerAPI::critical_error)
        .def("flexzone",     &producer::ProducerAPI::flexzone,
             py::arg("side") = py::none(),
             "Flexzone typed view. Returns None if no flexzone configured.")
        .def("update_flexzone_checksum", &producer::ProducerAPI::update_flexzone_checksum)
        .def("band_join",         &producer::ProducerAPI::band_join, py::arg("channel"))
        .def("band_leave",        &producer::ProducerAPI::band_leave, py::arg("channel"))
        .def("band_broadcast",    &producer::ProducerAPI::band_broadcast,
             py::arg("channel"), py::arg("body"))
        .def("band_members",      &producer::ProducerAPI::band_members, py::arg("channel"))
        .def("script_error_count", &producer::ProducerAPI::script_error_count)
        .def("out_slots_written",  &producer::ProducerAPI::out_slots_written)
        .def("out_drop_count",     &producer::ProducerAPI::out_drop_count)
        .def("loop_overrun_count", &producer::ProducerAPI::loop_overrun_count,
             "Cycles where start-to-start time exceeded configured period. "
             "0 when period==0 (free-run) or not connected.")
        .def("out_capacity",       &producer::ProducerAPI::out_capacity,
             "Ring/send buffer slot count for the output transport queue. 0 if not connected.")
        .def("out_policy",         &producer::ProducerAPI::out_policy,
             "Overflow policy description (e.g. 'shm_write', 'zmq_push_drop').")
        .def("last_cycle_work_us", &producer::ProducerAPI::last_cycle_work_us,
             "Microseconds of active work (acquire+script+commit) in the last iteration.")
        .def("metrics",            &producer::ProducerAPI::metrics,
             "Combined metrics dict: DataBlock ContextMetrics + loop_overruns + script_errors.")
        .def("slot_logical_size", &producer::ProducerAPI::slot_logical_size,
             py::arg("side") = py::none(),
             "Logical C struct size for the slot schema (bytes).")
        .def("flexzone_logical_size", &producer::ProducerAPI::flexzone_logical_size,
             py::arg("side") = py::none(),
             "Logical C struct size for the flexzone schema (bytes).")
        .def("spinlock",       &producer::ProducerAPI::spinlock,
             py::arg("index"), py::arg("side") = py::none())
        .def("spinlock_count", &producer::ProducerAPI::spinlock_count,
             py::arg("side") = py::none())
        .def_property_readonly_static("Tx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Tx); })
        .def_property_readonly_static("Rx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Rx); })
        .def("report_metric", &producer::ProducerAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &producer::ProducerAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &producer::ProducerAPI::clear_custom_metrics,
             "Clear all custom metrics.")
        .def("open_inbox",         &producer::ProducerAPI::open_inbox, py::arg("target_uid"))
        .def("clear_inbox_cache",  &producer::ProducerAPI::clear_inbox_cache)
        .def("wait_for_role",      &producer::ProducerAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("stop_reason",        &producer::ProducerAPI::stop_reason,
             "Why the role stopped: 'normal', 'peer_dead', 'hub_dead', or 'critical_error'.")
        .def_readwrite("shared_data",   &producer::ProducerAPI::shared_data_,
             "Shared script data dictionary. Persists across callbacks.")
        .def_static("as_numpy", &scripting::as_numpy_view, py::arg("ctypes_array"),
             "Convert a ctypes array field to a numpy ndarray view (zero-copy).")
        .def("log",          &producer::ProducerAPI::log, py::arg("level"), py::arg("msg"));
}
