/**
 * @file consumer_api.cpp
 * @brief ConsumerAPI method implementations + pylabhub_consumer pybind11 module.
 */
#include "consumer_api.hpp"

#include "utils/logger.hpp"

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
        .def("script_error_count", &ConsumerAPI::script_error_count)
        .def("in_slots_received",  &ConsumerAPI::in_slots_received);
}
