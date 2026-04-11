/**
 * @file processor_api.cpp
 * @brief ProcessorAPI Python-wrapping methods + pylabhub_processor pybind11 module.
 *
 * Phase 2: All C++ logic delegates to RoleAPIBase.
 */
#include "utils/script_engine.hpp"
#include "processor_api.hpp"

#include "plh_version_registry.hpp"
#include "python_helpers.hpp"
#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"
#include "utils/metrics_json.hpp"
#include "metrics_pydict.hpp"
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace scripting = pylabhub::scripting;

namespace pylabhub::processor
{

// ============================================================================
// Python-wrapping methods
// ============================================================================

py::object ProcessorAPI::flexzone() const
{
    if (flexzone_obj_ == nullptr)
        return py::none();
    return *flexzone_obj_;
}

bool ProcessorAPI::broadcast(py::bytes data)
{
    const auto s = data.cast<std::string>();
    return base_->broadcast(s.data(), s.size());
}

bool ProcessorAPI::send(const std::string &identity, py::bytes data)
{
    const auto s = data.cast<std::string>();
    return base_->send(identity, s.data(), s.size());
}

py::list ProcessorAPI::consumers()
{
    py::list lst;
    for (const auto &hex : base_->connected_consumers())
        lst.append(py::str(hex));
    return lst;
}

py::object ProcessorAPI::join_channel(const std::string &channel)
{
    auto result = base_->join_channel(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

void ProcessorAPI::send_channel_msg(const std::string &channel, py::dict body)
{
    auto json_mod = py::module_::import("json");
    std::string body_str = json_mod.attr("dumps")(body).cast<std::string>();
    base_->send_channel_msg(channel, nlohmann::json::parse(body_str));
}

py::object ProcessorAPI::channel_members(const std::string &channel)
{
    auto result = base_->channel_members(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

py::dict ProcessorAPI::metrics() const
{
    auto j = base_->snapshot_metrics_json();
    return py::module_::import("json").attr("loads")(j.dump()).cast<py::dict>();
}

uint64_t ProcessorAPI::slot_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->slot_logical_size(
        side.has_value() ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)} : std::nullopt));
}

uint64_t ProcessorAPI::flexzone_logical_size(std::optional<int> side) const
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

py::object ProcessorAPI::spinlock(std::size_t index, std::optional<int> side)
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

uint32_t ProcessorAPI::spinlock_count(std::optional<int> side) const
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
// Inbox (Python-specific)
// ============================================================================

py::object ProcessorAPI::open_inbox(const std::string &target_uid)
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

bool ProcessorAPI::wait_for_role(const std::string &uid, int timeout_ms)
{
    py::gil_scoped_release release;
    return base_->wait_for_role(uid, timeout_ms);
}

void ProcessorAPI::clear_inbox_cache()
{
    for (auto &[uid, handle_obj] : inbox_cache_)
    {
        try { handle_obj.cast<scripting::InboxHandle &>().clear_pyobjects(); }
        catch (...) {}
    }
    inbox_cache_.clear();
}

} // namespace pylabhub::processor

// ============================================================================
// pybind11 embedded module: pylabhub_processor
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_processor, m) // NOLINT
{
    using namespace pylabhub::processor; // NOLINT
    namespace scripting = pylabhub::scripting;

    py::class_<scripting::PyRxChannel>(m, "RxChannel")
        .def_readwrite("slot", &scripting::PyRxChannel::slot)
        .def_readwrite("fz",   &scripting::PyRxChannel::fz);

    py::class_<scripting::PyTxChannel>(m, "TxChannel")
        .def_readwrite("slot", &scripting::PyTxChannel::slot)
        .def_readwrite("fz",   &scripting::PyTxChannel::fz);

    py::class_<scripting::PyInboxMsg>(m, "InboxMsg")
        .def_readonly("data",       &scripting::PyInboxMsg::data)
        .def_readonly("sender_uid", &scripting::PyInboxMsg::sender_uid)
        .def_readonly("seq",        &scripting::PyInboxMsg::seq);

    py::class_<scripting::InboxHandle>(m, "InboxHandle")
        .def("acquire",  &scripting::InboxHandle::acquire)
        .def("send",     &scripting::InboxHandle::send,    py::arg("timeout_ms") = 5000)
        .def("discard",  &scripting::InboxHandle::discard)
        .def("is_ready", &scripting::InboxHandle::is_ready)
        .def("close",    &scripting::InboxHandle::close);

    py::class_<ProcessorAPI>(m, "ProcessorAPI")
        .def("log",           &ProcessorAPI::log, py::arg("level"), py::arg("msg"))
        .def("uid",           &ProcessorAPI::uid)
        .def("name",          &ProcessorAPI::name)
        .def("in_channel",    &ProcessorAPI::in_channel)
        .def("out_channel",   &ProcessorAPI::out_channel)
        .def("log_level",     &ProcessorAPI::log_level)
        .def("script_dir",    &ProcessorAPI::script_dir)
        .def("role_dir",      &ProcessorAPI::role_dir)
        .def("logs_dir",      &ProcessorAPI::logs_dir)
        .def("run_dir",       &ProcessorAPI::run_dir)
        .def("stop",          &ProcessorAPI::stop)
        .def("set_critical_error",   &ProcessorAPI::set_critical_error)
        .def("critical_error",       &ProcessorAPI::critical_error)
        .def("flexzone",      &ProcessorAPI::flexzone)
        .def("broadcast",     &ProcessorAPI::broadcast)
        .def("send",          &ProcessorAPI::send, py::arg("identity"), py::arg("data"))
        .def("consumers",     &ProcessorAPI::consumers)
        .def("update_flexzone_checksum", &ProcessorAPI::update_flexzone_checksum)
        .def("join_channel",      &ProcessorAPI::join_channel, py::arg("channel"))
        .def("leave_channel",     &ProcessorAPI::leave_channel, py::arg("channel"))
        .def("send_channel_msg",  &ProcessorAPI::send_channel_msg,
             py::arg("channel"), py::arg("body"))
        .def("channel_members",   &ProcessorAPI::channel_members, py::arg("channel"))
        .def("script_error_count", &ProcessorAPI::script_error_count)
        .def("in_slots_received",  &ProcessorAPI::in_slots_received)
        .def("out_slots_written",  &ProcessorAPI::out_slots_written)
        .def("out_drop_count",     &ProcessorAPI::out_drop_count)
        .def("loop_overrun_count", &ProcessorAPI::loop_overrun_count)
        .def("last_cycle_work_us", &ProcessorAPI::last_cycle_work_us)
        .def("metrics",            &ProcessorAPI::metrics)
        .def("slot_logical_size", &ProcessorAPI::slot_logical_size,
             py::arg("side") = py::none())
        .def("flexzone_logical_size", &ProcessorAPI::flexzone_logical_size,
             py::arg("side") = py::none())
        .def("spinlock",       &ProcessorAPI::spinlock,
             py::arg("index"), py::arg("side") = py::none())
        .def("spinlock_count", &ProcessorAPI::spinlock_count,
             py::arg("side") = py::none())
        .def_property_readonly_static("Tx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Tx); })
        .def_property_readonly_static("Rx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Rx); })
        .def("report_metric", &ProcessorAPI::report_metric, py::arg("key"), py::arg("value"))
        .def("report_metrics", &ProcessorAPI::report_metrics, py::arg("kv"))
        .def("clear_custom_metrics", &ProcessorAPI::clear_custom_metrics)
        .def("open_inbox",         &ProcessorAPI::open_inbox, py::arg("target_uid"))
        .def("clear_inbox_cache",  &ProcessorAPI::clear_inbox_cache)
        .def("wait_for_role",      &ProcessorAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("last_seq",      &ProcessorAPI::last_seq)
        .def("in_capacity",   &ProcessorAPI::in_capacity)
        .def("in_policy",     &ProcessorAPI::in_policy)
        .def("out_capacity",  &ProcessorAPI::out_capacity)
        .def("out_policy",    &ProcessorAPI::out_policy)
        .def("set_verify_checksum", &ProcessorAPI::set_verify_checksum, py::arg("enable"))
        .def("stop_reason",        &ProcessorAPI::stop_reason)
        .def("ctrl_queue_dropped", &ProcessorAPI::ctrl_queue_dropped)
        .def_readwrite("shared_data", &ProcessorAPI::shared_data_)
        .def_static("as_numpy", &scripting::as_numpy_view, py::arg("ctypes_array"));

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    });

    py::class_<scripting::SpinLockPy>(m, "SpinLock")
        .def("lock",   &scripting::SpinLockPy::lock)
        .def("unlock", &scripting::SpinLockPy::unlock)
        .def("try_lock_for", &scripting::SpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process", &scripting::SpinLockPy::is_locked_by_current_process)
        .def("__enter__", &scripting::SpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &scripting::SpinLockPy::exit);
}
