/**
 * @file processor_api.cpp
 * @brief ProcessorAPI method implementations + pylabhub_processor pybind11 module.
 */
#include "processor_api.hpp"

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace pylabhub::processor
{

// ============================================================================
// ProcessorAPI — logging
// ============================================================================

void ProcessorAPI::log(const std::string &level, const std::string &msg)
{
    if (level == "debug" || level == "Debug")
        LOGGER_DEBUG("[proc/{}] {}", uid_, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[proc/{}] {}", uid_, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[proc/{}] {}", uid_, msg);
    else
        LOGGER_INFO("[proc/{}] {}", uid_, msg);
}

// ============================================================================
// ProcessorAPI — shutdown
// ============================================================================

void ProcessorAPI::stop()
{
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_relaxed);
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_relaxed);
}

void ProcessorAPI::set_critical_error()
{
    critical_error_.store(true, std::memory_order_release);
    stop();
}

// ============================================================================
// ProcessorAPI — flexzone
// ============================================================================

py::object ProcessorAPI::flexzone() const
{
    if (flexzone_obj_ == nullptr)
        return py::none();
    return *flexzone_obj_;
}

// ============================================================================
// ProcessorAPI — producer (output side)
// ============================================================================

bool ProcessorAPI::broadcast(py::bytes data)
{
    if (!producer_)
        return false;
    const auto s = data.cast<std::string>();
    producer_->send(s.data(), s.size());
    return true;
}

bool ProcessorAPI::send(const std::string &identity, py::bytes data)
{
    if (!producer_)
        return false;
    const auto s = data.cast<std::string>();
    producer_->send_to(identity, s.data(), s.size());
    return true;
}

py::list ProcessorAPI::consumers()
{
    py::list lst;
    if (!producer_)
        return lst;
    for (const auto &id : producer_->connected_consumers())
        lst.append(id);
    return lst;
}

bool ProcessorAPI::update_flexzone_checksum()
{
    if (!producer_)
        return false;
    auto *shm = producer_->shm();
    if (!shm)
        return false;
    return shm->update_checksum_flexible_zone();
}

// ============================================================================
// ProcessorAPI — notify_channel
// ============================================================================

void ProcessorAPI::notify_channel(const std::string &target_channel,
                                  const std::string &event,
                                  const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_notify(target_channel, uid_, event, data);
}

// ============================================================================
// ProcessorAPI — broadcast_channel + list_channels
// ============================================================================

void ProcessorAPI::broadcast_channel(const std::string &target_channel,
                                     const std::string &message,
                                     const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_broadcast(target_channel, uid_, message, data);
}

py::list ProcessorAPI::list_channels()
{
    py::list result;
    if (!messenger_)
        return result;
    auto channels = messenger_->list_channels();
    for (auto &ch : channels)
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

// ============================================================================
// ProcessorAPI — custom metrics (HEP-CORE-0019)
// ============================================================================

void ProcessorAPI::report_metric(const std::string &key, double value)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_[key] = value;
}

void ProcessorAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    for (const auto &[k, v] : kv)
        custom_metrics_[k] = v;
}

void ProcessorAPI::clear_custom_metrics()
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_.clear();
}

nlohmann::json ProcessorAPI::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]    = in_slots_received_.load(std::memory_order_relaxed);
    base["out_written"]    = out_slots_written_.load(std::memory_order_relaxed);
    base["drops"]          = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]  = script_errors_;

    // Input side ContextMetrics (consumer SHM handle).
    if (consumer_ != nullptr)
    {
        if (const auto *shm = consumer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["in_iteration_count"]   = m.iteration_count;
            base["in_last_iteration_us"] = m.last_iteration_us;
            base["in_last_slot_work_us"] = m.last_slot_work_us;
            base["in_overrun_count"]     = m.overrun_count;
        }
    }

    // Output side ContextMetrics (producer SHM handle).
    if (producer_ != nullptr)
    {
        if (const auto *shm = producer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["out_iteration_count"]   = m.iteration_count;
            base["out_last_iteration_us"] = m.last_iteration_us;
            base["out_last_slot_work_us"] = m.last_slot_work_us;
            base["out_overrun_count"]     = m.overrun_count;
        }
    }

    nlohmann::json custom;
    {
        hub::InProcessSpinStateGuard guard(metrics_spin_);
        custom = nlohmann::json(custom_metrics_);
    }

    nlohmann::json result;
    result["base"]   = std::move(base);
    result["custom"] = std::move(custom);
    return result;
}

// ============================================================================
// ProcessorAPI — spinlocks
// ============================================================================

py::object ProcessorAPI::spinlock(std::size_t index)
{
    hub::DataBlockProducer *shm = nullptr;
    if (producer_)
        shm = producer_->shm();

    if (!shm)
        throw py::value_error("spinlock: SHM output channel not connected");

    return py::cast(ProcessorSpinLockPy{shm->get_spinlock(index)},
                    py::return_value_policy::move);
}

uint32_t ProcessorAPI::spinlock_count() const noexcept
{
    if (!producer_)
        return 0u;
    const auto *shm = producer_->shm();
    return shm ? shm->spinlock_count() : 0u;
}

} // namespace pylabhub::processor

// ============================================================================
// pybind11 embedded module: pylabhub_processor
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_processor, m) // NOLINT
{
    using namespace pylabhub::processor; // NOLINT

    // ProcessorAPI
    py::class_<ProcessorAPI>(m, "ProcessorAPI")
        .def("log",           &ProcessorAPI::log,
             py::arg("level"), py::arg("msg"))
        .def("uid",           &ProcessorAPI::uid)
        .def("name",          &ProcessorAPI::name)
        .def("in_channel",    &ProcessorAPI::in_channel)
        .def("out_channel",   &ProcessorAPI::out_channel)
        .def("log_level",     &ProcessorAPI::log_level)
        .def("script_dir",    &ProcessorAPI::script_dir)
        .def("stop",          &ProcessorAPI::stop)
        .def("set_critical_error",   &ProcessorAPI::set_critical_error)
        .def("critical_error",       &ProcessorAPI::critical_error)
        .def("flexzone",      &ProcessorAPI::flexzone)
        .def("broadcast",     &ProcessorAPI::broadcast)
        .def("send",          &ProcessorAPI::send,
             py::arg("identity"), py::arg("data"))
        .def("consumers",     &ProcessorAPI::consumers)
        .def("update_flexzone_checksum", &ProcessorAPI::update_flexzone_checksum)
        .def("notify_channel",  &ProcessorAPI::notify_channel,
             py::arg("target_channel"), py::arg("event"), py::arg("data") = "")
        .def("broadcast_channel", &ProcessorAPI::broadcast_channel,
             py::arg("target_channel"), py::arg("message"), py::arg("data") = "")
        .def("list_channels",  &ProcessorAPI::list_channels)
        .def("script_error_count", &ProcessorAPI::script_error_count)
        .def("in_slots_received",  &ProcessorAPI::in_slots_received)
        .def("out_slots_written",  &ProcessorAPI::out_slots_written)
        .def("out_drop_count",     &ProcessorAPI::out_drop_count)
        .def("spinlock",      &ProcessorAPI::spinlock, py::arg("index"))
        .def("spinlock_count",&ProcessorAPI::spinlock_count)
        .def("report_metric", &ProcessorAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ProcessorAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ProcessorAPI::clear_custom_metrics,
             "Clear all custom metrics.");

    // ProcessorSpinLock
    py::class_<ProcessorSpinLockPy>(m, "ProcessorSpinLock")
        .def("lock",   &ProcessorSpinLockPy::lock)
        .def("unlock", &ProcessorSpinLockPy::unlock)
        .def("try_lock_for", &ProcessorSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process",
             &ProcessorSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ProcessorSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ProcessorSpinLockPy::exit);
}
