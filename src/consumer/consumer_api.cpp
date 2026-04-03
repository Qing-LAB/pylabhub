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

#include <chrono>
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

py::list ConsumerAPI::list_channels()
{
    py::list result;
    for (auto &ch : base_->list_channels())
    {
        py::dict d;
        d["name"]           = ch.value("name", "");
        d["status"]         = ch.value("status", "");
        d["schema_id"]      = ch.value("schema_id", "");
        d["producer_uid"]   = ch.value("producer_uid", "");
        d["consumer_count"] = ch.value("consumer_count", 0);
        result.append(std::move(d));
    }
    return result;
}

py::object ConsumerAPI::shm_info(const std::string &channel)
{
    const std::string json_str = base_->request_shm_info(channel);
    if (json_str.empty())
        return py::none();
    return py::module_::import("json").attr("loads")(json_str);
}

py::dict ConsumerAPI::metrics() const
{
    auto j = base_->snapshot_metrics_json();
    return py::module_::import("json").attr("loads")(j.dump()).cast<py::dict>();
}

py::object ConsumerAPI::spinlock(std::size_t index)
{
    if (base_->spinlock_count() == 0)
        throw py::value_error("spinlock: SHM input channel not connected");
    return py::cast(ConsumerSpinLockPy{base_->get_spinlock(index)},
                    py::return_value_policy::move);
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
        .def("notify_channel",  &ConsumerAPI::notify_channel,
             py::arg("target_channel"), py::arg("event"), py::arg("data") = "")
        .def("broadcast_channel", &ConsumerAPI::broadcast_channel,
             py::arg("target_channel"), py::arg("message"), py::arg("data") = "")
        .def("list_channels",  &ConsumerAPI::list_channels)
        .def("shm_info",     &ConsumerAPI::shm_info, py::arg("channel") = "")
        .def("script_error_count", &ConsumerAPI::script_error_count)
        .def("in_slots_received",  &ConsumerAPI::in_slots_received)
        .def("loop_overrun_count", &ConsumerAPI::loop_overrun_count)
        .def("last_cycle_work_us", &ConsumerAPI::last_cycle_work_us)
        .def("last_seq",       &ConsumerAPI::last_seq)
        .def("in_capacity",    &ConsumerAPI::in_capacity)
        .def("in_policy",      &ConsumerAPI::in_policy)
        .def("set_verify_checksum", &ConsumerAPI::set_verify_checksum, py::arg("enable"))
        .def("spinlock",       &ConsumerAPI::spinlock, py::arg("index"))
        .def("spinlock_count", &ConsumerAPI::spinlock_count)
        .def("metrics",        &ConsumerAPI::metrics)
        .def("report_metric",  &ConsumerAPI::report_metric, py::arg("key"), py::arg("value"))
        .def("report_metrics", &ConsumerAPI::report_metrics, py::arg("kv"))
        .def("clear_custom_metrics", &ConsumerAPI::clear_custom_metrics)
        .def("open_inbox",    &ConsumerAPI::open_inbox, py::arg("target_uid"))
        .def("wait_for_role", &ConsumerAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("stop_reason",        &ConsumerAPI::stop_reason)
        .def("ctrl_queue_dropped", &ConsumerAPI::ctrl_queue_dropped)
        .def_readwrite("shared_data", &ConsumerAPI::shared_data_)
        .def_static("as_numpy", &scripting::as_numpy_view, py::arg("ctypes_array"));

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    });

    py::class_<ConsumerSpinLockPy>(m, "ConsumerSpinLock")
        .def("lock",   &ConsumerSpinLockPy::lock)
        .def("unlock", &ConsumerSpinLockPy::unlock)
        .def("try_lock_for", &ConsumerSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process", &ConsumerSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ConsumerSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ConsumerSpinLockPy::exit);
}
