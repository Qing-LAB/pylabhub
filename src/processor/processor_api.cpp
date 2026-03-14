/**
 * @file processor_api.cpp
 * @brief ProcessorAPI method implementations + pylabhub_processor pybind11 module.
 */
#include "processor_api.hpp"

#include "utils/format_tools.hpp"
#include "utils/logger.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace scripting = pylabhub::scripting;

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
    // LR-04: release ordering so any stores before stop() are visible to threads
    // reading the shutdown flag with acquire ordering.
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_release);
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_release);
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
    const auto s   = data.cast<std::string>();
    const auto raw = format_tools::bytes_from_hex(identity); // identity is hex string from script
    producer_->send_to(raw, s.data(), s.size());
    return true;
}

py::list ProcessorAPI::consumers()
{
    py::list lst;
    if (!producer_)
        return lst;
    for (const auto &id : producer_->connected_consumers())
        lst.append(py::str(format_tools::bytes_to_hex(id)));
    return lst;
}

bool ProcessorAPI::update_flexzone_checksum()
{
    auto *q = out_queue_.load(std::memory_order_acquire);
    if (!q)
        return false;
    q->sync_flexzone_checksum(); // no-op for ZmqQueue; ShmQueue updates segment checksum
    return true;
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

py::object ProcessorAPI::shm_blocks(const std::string& channel)
{
    if (!messenger_)
        return py::none();
    const std::string json_str = messenger_->query_shm_blocks(channel);
    if (json_str.empty())
        return py::none();
    return py::module_::import("json").attr("loads")(json_str);
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

std::string ProcessorAPI::stop_reason() const noexcept
{
    if (!stop_reason_) return "normal";
    switch (stop_reason_->load(std::memory_order_relaxed))
    {
    case 1:  return "peer_dead";
    case 2:  return "hub_dead";
    case 3:  return "critical_error";
    default: return "normal";
    }
}

uint64_t ProcessorAPI::ctrl_queue_dropped() const noexcept
{
    uint64_t total = 0;
    if (producer_) total += producer_->ctrl_queue_dropped();
    if (consumer_) total += consumer_->ctrl_queue_dropped();
    return total;
}

nlohmann::json ProcessorAPI::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_slots_received_.load(std::memory_order_relaxed);
    base["out_written"]        = out_slots_written_.load(std::memory_order_relaxed);
    base["drops"]              = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]      = script_errors_.load(std::memory_order_relaxed);
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["ctrl_queue_dropped"] = ctrl_queue_dropped();

    // Input side ContextMetrics (consumer SHM handle, D2+D3).
    if (consumer_ != nullptr)
    {
        if (const auto *shm = consumer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["in_iteration_count"]    = m.iteration_count;
            base["in_context_elapsed_us"] = m.context_elapsed_us;
            base["in_last_iteration_us"]  = m.last_iteration_us;
            base["in_max_iteration_us"]   = m.max_iteration_us;
            base["in_last_slot_wait_us"]  = m.last_slot_wait_us;
            base["in_last_slot_work_us"]  = m.last_slot_work_us;
            base["in_overrun_count"]      = m.overrun_count;
            base["in_period_ms"]          = m.period_ms;
        }
    }

    // Output side ContextMetrics (producer SHM handle, D2+D3).
    if (producer_ != nullptr)
    {
        if (const auto *shm = producer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["out_iteration_count"]    = m.iteration_count;
            base["out_context_elapsed_us"] = m.context_elapsed_us;
            base["out_last_iteration_us"]  = m.last_iteration_us;
            base["out_max_iteration_us"]   = m.max_iteration_us;
            base["out_last_slot_wait_us"]  = m.last_slot_wait_us;
            base["out_last_slot_work_us"]  = m.last_slot_work_us;
            base["out_overrun_count"]      = m.overrun_count;
            base["out_period_ms"]          = m.period_ms;
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

py::dict ProcessorAPI::metrics() const
{
    py::dict d;
    // D4 — script supervision
    d["script_error_count"] = py::int_(script_errors_.load(std::memory_order_relaxed));
    d["loop_overrun_count"] = py::int_(uint64_t{0}); // processor is queue-driven, no deadline
    d["last_cycle_work_us"] = py::int_(last_cycle_work_us_.load(std::memory_order_relaxed));
    d["in_received"]        = py::int_(in_slots_received_.load(std::memory_order_relaxed));
    d["out_written"]        = py::int_(out_slots_written_.load(std::memory_order_relaxed));
    d["drops"]              = py::int_(out_drops_.load(std::memory_order_relaxed));

    // D2+D3 — input side (consumer SHM)
    if (consumer_ != nullptr)
    {
        if (const auto *shm = consumer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            d["in_context_elapsed_us"] = py::int_(m.context_elapsed_us);
            d["in_iteration_count"]    = py::int_(m.iteration_count);
            d["in_last_iteration_us"]  = py::int_(m.last_iteration_us);
            d["in_max_iteration_us"]   = py::int_(m.max_iteration_us);
            d["in_last_slot_wait_us"]  = py::int_(m.last_slot_wait_us);
            d["in_overrun_count"]      = py::int_(m.overrun_count);
            d["in_last_slot_work_us"]  = py::int_(m.last_slot_work_us);
            d["in_period_ms"]          = py::int_(m.period_ms);
        }
    }

    // D2+D3 — output side (producer SHM)
    if (producer_ != nullptr)
    {
        if (const auto *shm = producer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            d["out_context_elapsed_us"] = py::int_(m.context_elapsed_us);
            d["out_iteration_count"]    = py::int_(m.iteration_count);
            d["out_last_iteration_us"]  = py::int_(m.last_iteration_us);
            d["out_max_iteration_us"]   = py::int_(m.max_iteration_us);
            d["out_last_slot_wait_us"]  = py::int_(m.last_slot_wait_us);
            d["out_overrun_count"]      = py::int_(m.overrun_count);
            d["out_last_slot_work_us"]  = py::int_(m.last_slot_work_us);
            d["out_period_ms"]          = py::int_(m.period_ms);
        }
    }

    return d;
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

// ============================================================================
// ProcessorAPI — inbox
// ============================================================================

py::object ProcessorAPI::open_inbox(const std::string &target_uid)
{
    // Cache hit — return existing handle without broker round-trip.
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;

    if (!messenger_)
        return py::none();

    // HR-05: release GIL during broker round-trip so interpreter stays responsive.
    std::optional<hub::RoleInfoResult> info;
    {
        py::gil_scoped_release release;
        info = messenger_->query_role_info(target_uid, /*timeout_ms=*/1000);
    }
    if (!info.has_value())
        return py::none();

    // inbox_schema is the full SchemaSpec JSON {"fields":[{"name":...,"type":...}]}.
    if (!info->inbox_schema.is_object() || !info->inbox_schema.contains("fields"))
        return py::none();

    scripting::SchemaSpec spec;
    try
    {
        spec = scripting::parse_schema_json(info->inbox_schema);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("[proc] open_inbox('{}'): schema parse error: {}", target_uid, e.what());
        return py::none();
    }

    // Build ctypes slot type (GIL already held — called from Python).
    py::object slot_type = scripting::build_ctypes_struct(spec, "InboxSlot");
    const size_t item_size = scripting::ctypes_sizeof(slot_type);

    // Build ZmqSchemaField list from SchemaSpec.
    auto zmq_fields = scripting::schema_spec_to_zmq_fields(spec, item_size);

    // Connect InboxClient (DEALER connecting to target's ROUTER).
    auto client_ptr = hub::InboxClient::connect_to(
        info->inbox_endpoint, uid_, std::move(zmq_fields), info->inbox_packing);
    if (!client_ptr)
    {
        LOGGER_WARN("[proc] open_inbox('{}'): connect_to '{}' failed",
                    target_uid, info->inbox_endpoint);
        return py::none();
    }
    if (!client_ptr->start())
    {
        LOGGER_WARN("[proc] open_inbox('{}'): start() failed", target_uid);
        return py::none();
    }

    auto shared_client = std::shared_ptr<hub::InboxClient>(std::move(client_ptr));
    py::object handle  = py::cast(
        scripting::InboxHandle(std::move(shared_client), std::move(spec),
                               std::move(slot_type), item_size),
        py::return_value_policy::move);
    inbox_cache_[target_uid] = handle;
    return handle;
}

bool ProcessorAPI::wait_for_role(const std::string &uid, int timeout_ms)
{
    if (!messenger_)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            py::gil_scoped_release rel;
            if (messenger_->query_role_presence(uid, kPollMs))
                return true;
        }
    }
    return false;
}

void ProcessorAPI::clear_inbox_cache()
{
    for (auto &[uid, handle_obj] : inbox_cache_)
    {
        try
        {
            handle_obj.cast<scripting::InboxHandle &>().clear_pyobjects();
        }
        catch (...) {}
    }
    inbox_cache_.clear();
}

// ============================================================================
// ProcessorAPI — queue-state accessors (PR-07/08/09)
// ============================================================================

uint64_t ProcessorAPI::last_seq() const noexcept
{
    const auto *q = in_queue_.load(std::memory_order_acquire);
    return q ? q->last_seq() : 0u;
}

uint64_t ProcessorAPI::in_capacity() const noexcept
{
    const auto *q = in_queue_.load(std::memory_order_acquire);
    return q ? q->capacity() : 0u;
}

std::string ProcessorAPI::in_policy() const
{
    const auto *q = in_queue_.load(std::memory_order_acquire);
    return q ? q->policy_info() : std::string{};
}

uint64_t ProcessorAPI::out_capacity() const noexcept
{
    const auto *q = out_queue_.load(std::memory_order_acquire);
    return q ? q->capacity() : 0u;
}

std::string ProcessorAPI::out_policy() const
{
    const auto *q = out_queue_.load(std::memory_order_acquire);
    return q ? q->policy_info() : std::string{};
}

void ProcessorAPI::set_verify_checksum(bool enable)
{
    const auto *q = in_queue_.load(std::memory_order_acquire);
    if (q)
        q->set_verify_checksum(enable, false);
}

} // namespace pylabhub::processor

// ============================================================================
// pybind11 embedded module: pylabhub_processor
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_processor, m) // NOLINT
{
    using namespace pylabhub::processor; // NOLINT

    py::class_<scripting::InboxHandle>(m, "InboxHandle")
        .def("acquire",  &scripting::InboxHandle::acquire)
        .def("send",     &scripting::InboxHandle::send,    py::arg("timeout_ms") = 5000)
        .def("discard",  &scripting::InboxHandle::discard)
        .def("is_ready", &scripting::InboxHandle::is_ready)
        .def("close",    &scripting::InboxHandle::close);

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
        .def("role_dir",      &ProcessorAPI::role_dir)
        .def("logs_dir",      &ProcessorAPI::logs_dir)
        .def("run_dir",       &ProcessorAPI::run_dir)
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
        .def("shm_blocks",     &ProcessorAPI::shm_blocks, py::arg("channel") = "")
        .def("script_error_count", &ProcessorAPI::script_error_count)
        .def("in_slots_received",  &ProcessorAPI::in_slots_received)
        .def("out_slots_written",  &ProcessorAPI::out_slots_written)
        .def("out_drop_count",     &ProcessorAPI::out_drop_count)
        .def("loop_overrun_count", &ProcessorAPI::loop_overrun_count,
             "Always 0 — processor is queue-driven (no deadline to overrun).")
        .def("last_cycle_work_us", &ProcessorAPI::last_cycle_work_us,
             "Microseconds of active work (GIL acquire + on_process callback) in the last iteration.")
        .def("metrics",            &ProcessorAPI::metrics,
             "Combined metrics dict: in/out DataBlock ContextMetrics + D4 counters.")
        .def("spinlock",      &ProcessorAPI::spinlock, py::arg("index"))
        .def("spinlock_count",&ProcessorAPI::spinlock_count)
        .def("report_metric", &ProcessorAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ProcessorAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ProcessorAPI::clear_custom_metrics,
             "Clear all custom metrics.")
        .def("open_inbox",    &ProcessorAPI::open_inbox,    py::arg("target_uid"))
        .def("wait_for_role", &ProcessorAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("last_seq",      &ProcessorAPI::last_seq,
             "Sequence number of the last consumed input slot (0 until first slot). "
             "IC-04: SHM=ring-buffer slot index (wraps at capacity); ZMQ=monotone wire seq.")
        .def("in_capacity",   &ProcessorAPI::in_capacity,
             "Input ring-buffer capacity (slot count), or 0 if not connected.")
        .def("in_policy",     &ProcessorAPI::in_policy,
             "Input queue policy string, or empty string if not connected.")
        .def("out_capacity",  &ProcessorAPI::out_capacity,
             "Output ring-buffer capacity (slot count), or 0 if not connected.")
        .def("out_policy",    &ProcessorAPI::out_policy,
             "Output queue policy string, or empty string if not connected.")
        .def("set_verify_checksum", &ProcessorAPI::set_verify_checksum, py::arg("enable"),
             "Enable/disable BLAKE2b checksum verification on input slots (SHM only; no-op for ZMQ).")
        .def("stop_reason",        &ProcessorAPI::stop_reason,
             "Why the role stopped: 'normal', 'peer_dead', 'hub_dead', or 'critical_error'.")
        .def("ctrl_queue_dropped", &ProcessorAPI::ctrl_queue_dropped,
             "Total ctrl-send messages dropped by both in and out queues due to overflow.");

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
