/**
 * @file producer_api.cpp
 * @brief ProducerAPI method implementations + pylabhub_producer pybind11 module.
 */
#include "producer_api.hpp"

#include "utils/logger.hpp"

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
        .def("script_error_count", &ProducerAPI::script_error_count)
        .def("out_slots_written",  &ProducerAPI::out_slots_written)
        .def("out_drop_count",     &ProducerAPI::out_drop_count)
        .def("spinlock",     &ProducerAPI::spinlock, py::arg("index"))
        .def("spinlock_count",&ProducerAPI::spinlock_count);

    py::class_<ProducerSpinLockPy>(m, "ProducerSpinLock")
        .def("lock",   &ProducerSpinLockPy::lock)
        .def("unlock", &ProducerSpinLockPy::unlock)
        .def("try_lock_for", &ProducerSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process",
             &ProducerSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ProducerSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ProducerSpinLockPy::exit);
}
