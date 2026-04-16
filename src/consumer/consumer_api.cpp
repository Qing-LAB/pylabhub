/**
 * @file consumer_api.cpp
 * @brief ConsumerAPI Python-wrapping methods + pylabhub_consumer pybind11 module.
 *
 * Phase 2: All C++ logic delegates to RoleAPIBase.
 */
#include "utils/script_engine.hpp"
#include "consumer_api.hpp"

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

namespace pylabhub::consumer
{

// ============================================================================
// Python-wrapping methods
// ============================================================================

py::object ConsumerAPI::flexzone(std::optional<int> /*side*/) const
{
    // Consumer has only Rx side; side arg is ignored.
    if (!rx_flexzone_obj_.has_value())
        return py::none();
    return *rx_flexzone_obj_;
}

py::object ConsumerAPI::band_join(const std::string &channel)
{
    auto result = base_->band_join(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

void ConsumerAPI::band_broadcast(const std::string &channel, py::dict body)
{
    auto json_mod = py::module_::import("json");
    std::string body_str = json_mod.attr("dumps")(body).cast<std::string>();
    base_->band_broadcast(channel, nlohmann::json::parse(body_str));
}

py::object ConsumerAPI::band_members(const std::string &channel)
{
    auto result = base_->band_members(channel);
    if (!result.has_value())
        return py::none();
    return py::module_::import("json").attr("loads")(result->dump());
}

py::dict ConsumerAPI::metrics() const
{
    auto j = base_->snapshot_metrics_json();
    return py::module_::import("json").attr("loads")(j.dump()).cast<py::dict>();
}

uint64_t ConsumerAPI::slot_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->slot_logical_size(
        side.has_value() ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)} : std::nullopt));
}

uint64_t ConsumerAPI::flexzone_logical_size(std::optional<int> side) const
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

py::object ConsumerAPI::spinlock(std::size_t index, std::optional<int> side)
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

uint32_t ConsumerAPI::spinlock_count(std::optional<int> side) const
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

py::object ConsumerAPI::open_inbox(const std::string &target_uid)
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

bool ConsumerAPI::wait_for_role(const std::string &uid, int timeout_ms)
{
    py::gil_scoped_release release;
    return base_->wait_for_role(uid, timeout_ms);
}

void ConsumerAPI::clear_inbox_cache()
{
    for (auto &[uid, handle_obj] : inbox_cache_)
    {
        try { handle_obj.cast<scripting::InboxHandle &>().clear_pyobjects(); }
        catch (...) {}
    }
    inbox_cache_.clear();
}

} // namespace pylabhub::consumer

// ============================================================================
// pybind11 embedded module: pylabhub_consumer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_consumer, m) // NOLINT
{
    using namespace pylabhub::consumer; // NOLINT
    namespace scripting = pylabhub::scripting;

    py::class_<scripting::PyRxChannel>(m, "RxChannel")
        .def_readwrite("slot", &scripting::PyRxChannel::slot);

    py::class_<scripting::PyTxChannel>(m, "TxChannel")
        .def_readwrite("slot", &scripting::PyTxChannel::slot);

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

    py::class_<ConsumerAPI>(m, "ConsumerAPI")
        .def("log",          &ConsumerAPI::log, py::arg("level"), py::arg("msg"))
        .def("uid",          &ConsumerAPI::uid)
        .def("name",         &ConsumerAPI::name)
        .def("channel",      &ConsumerAPI::channel)
        .def("log_level",    &ConsumerAPI::log_level)
        .def("script_dir",   &ConsumerAPI::script_dir)
        .def("role_dir",     &ConsumerAPI::role_dir)
        .def("logs_dir",     &ConsumerAPI::logs_dir)
        .def("run_dir",      &ConsumerAPI::run_dir)
        .def("stop",         &ConsumerAPI::stop)
        .def("set_critical_error",    &ConsumerAPI::set_critical_error)
        .def("critical_error",        &ConsumerAPI::critical_error)
        .def("band_join",         &ConsumerAPI::band_join, py::arg("channel"))
        .def("band_leave",        &ConsumerAPI::band_leave, py::arg("channel"))
        .def("band_broadcast",    &ConsumerAPI::band_broadcast,
             py::arg("channel"), py::arg("body"))
        .def("band_members",      &ConsumerAPI::band_members, py::arg("channel"))
        .def("script_error_count", &ConsumerAPI::script_error_count)
        .def("in_slots_received",  &ConsumerAPI::in_slots_received)
        .def("loop_overrun_count", &ConsumerAPI::loop_overrun_count)
        .def("last_cycle_work_us", &ConsumerAPI::last_cycle_work_us)
        .def("last_seq",       &ConsumerAPI::last_seq)
        .def("in_capacity",    &ConsumerAPI::in_capacity)
        .def("in_policy",      &ConsumerAPI::in_policy)
        .def("flexzone",     &ConsumerAPI::flexzone,
             py::arg("side") = py::none(),
             "Flexzone typed view. Returns None if no flexzone configured.")
        .def("set_verify_checksum", &ConsumerAPI::set_verify_checksum, py::arg("enable"))
        .def("slot_logical_size", &ConsumerAPI::slot_logical_size,
             py::arg("side") = py::none())
        .def("flexzone_logical_size", &ConsumerAPI::flexzone_logical_size,
             py::arg("side") = py::none())
        .def("spinlock",       &ConsumerAPI::spinlock,
             py::arg("index"), py::arg("side") = py::none())
        .def("spinlock_count", &ConsumerAPI::spinlock_count,
             py::arg("side") = py::none())
        .def_property_readonly_static("Tx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Tx); })
        .def_property_readonly_static("Rx", [](py::object) { return static_cast<int>(scripting::ChannelSide::Rx); })
        .def("metrics",        &ConsumerAPI::metrics)
        .def("report_metric",  &ConsumerAPI::report_metric, py::arg("key"), py::arg("value"))
        .def("report_metrics", &ConsumerAPI::report_metrics, py::arg("kv"))
        .def("clear_custom_metrics", &ConsumerAPI::clear_custom_metrics)
        .def("open_inbox",         &ConsumerAPI::open_inbox, py::arg("target_uid"))
        .def("clear_inbox_cache",  &ConsumerAPI::clear_inbox_cache)
        .def("wait_for_role",      &ConsumerAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("stop_reason",        &ConsumerAPI::stop_reason)
        .def_readwrite("shared_data", &ConsumerAPI::shared_data_)
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
