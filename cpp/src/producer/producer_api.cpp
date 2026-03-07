/**
 * @file producer_api.cpp
 * @brief ProducerAPI method implementations + pylabhub_producer pybind11 module.
 */
#include "producer_api.hpp"

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace pylabhub::producer
{

// ============================================================================
// ProducerAPI — logging
// ============================================================================

void ProducerAPI::log(const std::string &level, const std::string &msg)
{
    if (level == "debug" || level == "Debug")
        LOGGER_DEBUG("[prod/{}] {}", uid_, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[prod/{}] {}", uid_, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[prod/{}] {}", uid_, msg);
    else
        LOGGER_INFO("[prod/{}] {}", uid_, msg);
}

// ============================================================================
// ProducerAPI — shutdown
// ============================================================================

void ProducerAPI::stop()
{
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_relaxed);
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_relaxed);
}

void ProducerAPI::set_critical_error()
{
    critical_error_.store(true, std::memory_order_release);
    stop();
}

// ============================================================================
// ProducerAPI — flexzone
// ============================================================================

py::object ProducerAPI::flexzone() const
{
    if (flexzone_obj_ == nullptr)
        return py::none();
    return *flexzone_obj_;
}

// ============================================================================
// ProducerAPI — producer-side
// ============================================================================

bool ProducerAPI::broadcast(py::bytes data)
{
    if (!producer_)
        return false;
    const auto s = data.cast<std::string>();
    producer_->send(s.data(), s.size());
    return true;
}

bool ProducerAPI::send(const std::string &identity, py::bytes data)
{
    if (!producer_)
        return false;
    const auto s = data.cast<std::string>();
    producer_->send_to(identity, s.data(), s.size());
    return true;
}

py::list ProducerAPI::consumers()
{
    py::list lst;
    if (!producer_)
        return lst;
    for (const auto &id : producer_->connected_consumers())
        lst.append(id);
    return lst;
}

bool ProducerAPI::update_flexzone_checksum()
{
    if (!producer_)
        return false;
    auto *shm = producer_->shm();
    if (!shm)
        return false;
    return shm->update_checksum_flexible_zone();
}

// ============================================================================
// ProducerAPI — notify_channel
// ============================================================================

void ProducerAPI::notify_channel(const std::string &target_channel,
                                 const std::string &event,
                                 const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_notify(target_channel, uid_, event, data);
}

// ============================================================================
// ProducerAPI — broadcast_channel + list_channels
// ============================================================================

void ProducerAPI::broadcast_channel(const std::string &target_channel,
                                    const std::string &message,
                                    const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_broadcast(target_channel, uid_, message, data);
}

py::list ProducerAPI::list_channels()
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

py::object ProducerAPI::shm_blocks(const std::string& channel)
{
    if (!messenger_)
        return py::none();
    const std::string json_str = messenger_->query_shm_blocks(channel);
    if (json_str.empty())
        return py::none();
    return py::module_::import("json").attr("loads")(json_str);
}

// ============================================================================
// ProducerAPI — custom metrics (HEP-CORE-0019)
// ============================================================================

void ProducerAPI::report_metric(const std::string &key, double value)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_[key] = value;
}

void ProducerAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    for (const auto &[k, v] : kv)
        custom_metrics_[k] = v;
}

void ProducerAPI::clear_custom_metrics()
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_.clear();
}

nlohmann::json ProducerAPI::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["out_written"]    = out_slots_written_.load(std::memory_order_relaxed);
    base["drops"]          = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]  = script_errors_;

    // ContextMetrics from SHM handle (if available).
    if (producer_ != nullptr)
    {
        if (const auto *shm = producer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["iteration_count"]   = m.iteration_count;
            base["overrun_count"]     = m.overrun_count;
            base["last_iteration_us"] = m.last_iteration_us;
            base["max_iteration_us"]  = m.max_iteration_us;
            base["last_slot_work_us"] = m.last_slot_work_us;
            base["last_slot_wait_us"] = m.last_slot_wait_us;
            base["period_ms"]         = m.period_ms;
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
// ProducerAPI — spinlocks
// ============================================================================

py::object ProducerAPI::spinlock(std::size_t index)
{
    hub::DataBlockProducer *shm = nullptr;
    if (producer_)
        shm = producer_->shm();

    if (!shm)
        throw py::value_error("spinlock: SHM output channel not connected");

    return py::cast(ProducerSpinLockPy{shm->get_spinlock(index)},
                    py::return_value_policy::move);
}

uint32_t ProducerAPI::spinlock_count() const noexcept
{
    if (!producer_)
        return 0u;
    const auto *shm = producer_->shm();
    return shm ? shm->spinlock_count() : 0u;
}

} // namespace pylabhub::producer

// ============================================================================
// pybind11 embedded module: pylabhub_producer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_producer, m) // NOLINT
{
    using namespace pylabhub::producer; // NOLINT

    py::class_<ProducerAPI>(m, "ProducerAPI")
        .def("log",          &ProducerAPI::log,
             py::arg("level"), py::arg("msg"))
        .def("uid",          &ProducerAPI::uid)
        .def("name",         &ProducerAPI::name)
        .def("channel",      &ProducerAPI::channel)
        .def("log_level",    &ProducerAPI::log_level)
        .def("script_dir",   &ProducerAPI::script_dir)
        .def("stop",         &ProducerAPI::stop)
        .def("set_critical_error",    &ProducerAPI::set_critical_error)
        .def("critical_error",        &ProducerAPI::critical_error)
        .def("flexzone",     &ProducerAPI::flexzone)
        .def("broadcast",    &ProducerAPI::broadcast)
        .def("send",         &ProducerAPI::send,
             py::arg("identity"), py::arg("data"))
        .def("consumers",    &ProducerAPI::consumers)
        .def("update_flexzone_checksum", &ProducerAPI::update_flexzone_checksum)
        .def("notify_channel",  &ProducerAPI::notify_channel,
             py::arg("target_channel"), py::arg("event"), py::arg("data") = "")
        .def("broadcast_channel", &ProducerAPI::broadcast_channel,
             py::arg("target_channel"), py::arg("message"), py::arg("data") = "")
        .def("list_channels",  &ProducerAPI::list_channels)
        .def("shm_blocks",     &ProducerAPI::shm_blocks, py::arg("channel") = "")
        .def("script_error_count", &ProducerAPI::script_error_count)
        .def("out_slots_written",  &ProducerAPI::out_slots_written)
        .def("out_drop_count",     &ProducerAPI::out_drop_count)
        .def("spinlock",     &ProducerAPI::spinlock, py::arg("index"))
        .def("spinlock_count",&ProducerAPI::spinlock_count)
        .def("report_metric", &ProducerAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ProducerAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ProducerAPI::clear_custom_metrics,
             "Clear all custom metrics.");

    py::class_<ProducerSpinLockPy>(m, "ProducerSpinLock")
        .def("lock",   &ProducerSpinLockPy::lock)
        .def("unlock", &ProducerSpinLockPy::unlock)
        .def("try_lock_for", &ProducerSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process",
             &ProducerSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ProducerSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ProducerSpinLockPy::exit);
}
