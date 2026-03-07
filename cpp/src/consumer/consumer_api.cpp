/**
 * @file consumer_api.cpp
 * @brief ConsumerAPI method implementations + pylabhub_consumer pybind11 module.
 */
#include "consumer_api.hpp"

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace pylabhub::consumer
{

void ConsumerAPI::log(const std::string &level, const std::string &msg)
{
    if (level == "debug" || level == "Debug")
        LOGGER_DEBUG("[cons/{}] {}", uid_, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[cons/{}] {}", uid_, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[cons/{}] {}", uid_, msg);
    else
        LOGGER_INFO("[cons/{}] {}", uid_, msg);
}

void ConsumerAPI::stop()
{
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_relaxed);
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_relaxed);
}

void ConsumerAPI::set_critical_error()
{
    critical_error_.store(true, std::memory_order_release);
    stop();
}

void ConsumerAPI::notify_channel(const std::string &target_channel,
                                 const std::string &event,
                                 const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_notify(target_channel, uid_, event, data);
}

void ConsumerAPI::broadcast_channel(const std::string &target_channel,
                                    const std::string &message,
                                    const std::string &data)
{
    if (!messenger_)
        return;
    messenger_->enqueue_channel_broadcast(target_channel, uid_, message, data);
}

py::list ConsumerAPI::list_channels()
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

py::object ConsumerAPI::shm_blocks(const std::string& channel)
{
    if (!messenger_)
        return py::none();
    const std::string json_str = messenger_->query_shm_blocks(channel);
    if (json_str.empty())
        return py::none();
    return py::module_::import("json").attr("loads")(json_str);
}

// ============================================================================
// ConsumerAPI — custom metrics (HEP-CORE-0019)
// ============================================================================

void ConsumerAPI::report_metric(const std::string &key, double value)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_[key] = value;
}

void ConsumerAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    for (const auto &[k, v] : kv)
        custom_metrics_[k] = v;
}

void ConsumerAPI::clear_custom_metrics()
{
    hub::InProcessSpinStateGuard guard(metrics_spin_);
    custom_metrics_.clear();
}

nlohmann::json ConsumerAPI::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]    = in_slots_received_.load(std::memory_order_relaxed);
    base["script_errors"]  = script_errors_;

    // ContextMetrics from SHM handle (if available).
    if (consumer_ != nullptr)
    {
        if (const auto *shm = consumer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["iteration_count"]   = m.iteration_count;
            base["last_iteration_us"] = m.last_iteration_us;
            base["max_iteration_us"]  = m.max_iteration_us;
            base["last_slot_work_us"] = m.last_slot_work_us;
            base["last_slot_wait_us"] = m.last_slot_wait_us;
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

} // namespace pylabhub::consumer

// ============================================================================
// pybind11 embedded module: pylabhub_consumer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_consumer, m) // NOLINT
{
    using namespace pylabhub::consumer; // NOLINT

    py::class_<ConsumerAPI>(m, "ConsumerAPI")
        .def("log",          &ConsumerAPI::log,
             py::arg("level"), py::arg("msg"))
        .def("uid",          &ConsumerAPI::uid)
        .def("name",         &ConsumerAPI::name)
        .def("channel",      &ConsumerAPI::channel)
        .def("log_level",    &ConsumerAPI::log_level)
        .def("script_dir",   &ConsumerAPI::script_dir)
        .def("stop",         &ConsumerAPI::stop)
        .def("set_critical_error",    &ConsumerAPI::set_critical_error)
        .def("critical_error",        &ConsumerAPI::critical_error)
        .def("notify_channel",  &ConsumerAPI::notify_channel,
             py::arg("target_channel"), py::arg("event"), py::arg("data") = "")
        .def("broadcast_channel", &ConsumerAPI::broadcast_channel,
             py::arg("target_channel"), py::arg("message"), py::arg("data") = "")
        .def("list_channels",  &ConsumerAPI::list_channels)
        .def("shm_blocks",     &ConsumerAPI::shm_blocks, py::arg("channel") = "")
        .def("script_error_count", &ConsumerAPI::script_error_count)
        .def("in_slots_received",  &ConsumerAPI::in_slots_received)
        .def("report_metric", &ConsumerAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ConsumerAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ConsumerAPI::clear_custom_metrics,
             "Clear all custom metrics.");
}
