/**
 * @file consumer_api.cpp
 * @brief ConsumerAPI method implementations + pylabhub_consumer pybind11 module.
 */
#include "consumer_api.hpp"

#include "plh_version_registry.hpp"
#include "utils/logger.hpp"

#include <chrono>
#include "utils/json_fwd.hpp"
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace scripting = pylabhub::scripting;

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
    // LR-04: release ordering so any stores before stop() are visible to threads
    // reading the shutdown flag with acquire ordering.
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_release);
}

void ConsumerAPI::set_critical_error()
{
    if (critical_error_ptr_)
        critical_error_ptr_->store(true, std::memory_order_release);
    if (stop_reason_)
        stop_reason_->store(3, std::memory_order_relaxed); // RoleHostCore::StopReason::CriticalError
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

void ConsumerAPI::set_verify_checksum(bool enable)
{
    auto *q = reader_.load(std::memory_order_acquire);
    if (q)
        q->set_verify_checksum(enable, false);
}

std::string ConsumerAPI::stop_reason() const noexcept
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

uint64_t ConsumerAPI::ctrl_queue_dropped() const noexcept
{
    if (!consumer_) return 0u;
    return consumer_->ctrl_queue_dropped();
}

nlohmann::json ConsumerAPI::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_slots_received();
    base["script_errors"]      = script_error_count();
    base["last_cycle_work_us"] = last_cycle_work_us();
    base["loop_overrun_count"] = uint64_t{0}; // consumer is demand-driven, no deadline
    base["ctrl_queue_dropped"] = ctrl_queue_dropped();

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

py::dict ConsumerAPI::metrics() const
{
    py::dict d;
    d["last_cycle_work_us"] = py::int_(last_cycle_work_us());
    d["loop_overrun_count"] = py::int_(uint64_t{0}); // consumer is demand-driven, no deadline
    d["script_errors"]      = py::int_(script_error_count());
    d["in_received"]        = py::int_(in_slots_received());

    if (consumer_ != nullptr)
    {
        if (const auto *shm = consumer_->shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            d["context_elapsed_us"] = py::int_(m.context_elapsed_us);
            d["iteration_count"]    = py::int_(m.iteration_count);
            d["last_iteration_us"]  = py::int_(m.last_iteration_us);
            d["max_iteration_us"]   = py::int_(m.max_iteration_us);
            d["last_slot_wait_us"]  = py::int_(m.last_slot_wait_us);
            d["last_slot_work_us"]  = py::int_(m.last_slot_work_us);
        }
    }

    return d;
}

// ============================================================================
// ConsumerAPI — inbox
// ============================================================================

py::object ConsumerAPI::open_inbox(const std::string &target_uid)
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
        LOGGER_WARN("[cons] open_inbox('{}'): schema parse error: {}", target_uid, e.what());
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
        LOGGER_WARN("[cons] open_inbox('{}'): connect_to '{}' failed",
                    target_uid, info->inbox_endpoint);
        return py::none();
    }
    if (!client_ptr->start())
    {
        LOGGER_WARN("[cons] open_inbox('{}'): start() failed", target_uid);
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

bool ConsumerAPI::wait_for_role(const std::string &uid, int timeout_ms)
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

size_t ConsumerAPI::in_capacity() const noexcept
{
    const auto *r = reader_.load(std::memory_order_acquire);
    if (!r)
        return 0;
    try { return r->capacity(); }
    catch (...) { return 0; }
}

std::string ConsumerAPI::in_policy() const
{
    const auto *r = reader_.load(std::memory_order_acquire);
    if (!r)
        return {};
    try { return r->policy_info(); }
    catch (...) { return {}; }
}

py::object ConsumerAPI::spinlock(std::size_t index)
{
    hub::DataBlockConsumer *shm = nullptr;
    if (consumer_)
        shm = consumer_->shm();

    if (!shm)
        throw py::value_error("spinlock: SHM input channel not connected");

    return py::cast(ConsumerSpinLockPy{shm->get_spinlock(index)},
                    py::return_value_policy::move);
}

uint32_t ConsumerAPI::spinlock_count() const noexcept
{
    if (!consumer_)
        return 0u;
    const auto *shm = consumer_->shm();
    return shm ? shm->spinlock_count() : 0u;
}

void ConsumerAPI::clear_inbox_cache()
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

} // namespace pylabhub::consumer

// ============================================================================
// pybind11 embedded module: pylabhub_consumer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_consumer, m) // NOLINT
{
    using namespace pylabhub::consumer; // NOLINT

    py::class_<scripting::InboxHandle>(m, "InboxHandle")
        .def("acquire",  &scripting::InboxHandle::acquire)
        .def("send",     &scripting::InboxHandle::send,    py::arg("timeout_ms") = 5000)
        .def("discard",  &scripting::InboxHandle::discard)
        .def("is_ready", &scripting::InboxHandle::is_ready)
        .def("close",    &scripting::InboxHandle::close);

    py::class_<ConsumerAPI>(m, "ConsumerAPI")
        .def("log",          &ConsumerAPI::log,
             py::arg("level"), py::arg("msg"))
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
        .def("shm_blocks",     &ConsumerAPI::shm_blocks, py::arg("channel") = "")
        .def("script_error_count", &ConsumerAPI::script_error_count)
        .def("in_slots_received",  &ConsumerAPI::in_slots_received)
        .def("loop_overrun_count", &ConsumerAPI::loop_overrun_count,
             "Always 0 — consumer is demand-driven (no deadline to overrun).")
        .def("last_cycle_work_us", &ConsumerAPI::last_cycle_work_us,
             "Microseconds of active work (callback + release) in the last consume iteration.")
        .def("last_seq",       &ConsumerAPI::last_seq,
             "Sequence number of the last slot read (0 until first slot). "
             "IC-04: SHM=ring-buffer slot index (wraps at capacity); ZMQ=monotone wire seq.")
        .def("in_capacity",    &ConsumerAPI::in_capacity,
             "Ring/recv buffer slot count for the input transport queue. 0 if not connected.")
        .def("in_policy",      &ConsumerAPI::in_policy,
             "Input queue overflow policy description (e.g. 'shm_read', 'zmq_pull_ring_64').")
        .def("set_verify_checksum", &ConsumerAPI::set_verify_checksum, py::arg("enable"),
             "Enable/disable BLAKE2b checksum verification on input slots (SHM only; no-op for ZMQ).")
        .def("spinlock",       &ConsumerAPI::spinlock, py::arg("index"))
        .def("spinlock_count", &ConsumerAPI::spinlock_count)
        .def("metrics",            &ConsumerAPI::metrics,
             "Combined metrics dict: DataBlock ContextMetrics + last_cycle_work_us + script_errors.")
        .def("report_metric", &ConsumerAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ConsumerAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ConsumerAPI::clear_custom_metrics,
             "Clear all custom metrics.")
        .def("open_inbox",    &ConsumerAPI::open_inbox,    py::arg("target_uid"))
        .def("wait_for_role", &ConsumerAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("stop_reason",        &ConsumerAPI::stop_reason,
             "Why the role stopped: 'normal', 'peer_dead', 'hub_dead', or 'critical_error'.")
        .def("ctrl_queue_dropped", &ConsumerAPI::ctrl_queue_dropped,
             "Number of ctrl-send messages dropped due to queue overflow.");

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    }, "Return JSON string with all component version information.");

    py::class_<ConsumerSpinLockPy>(m, "ConsumerSpinLock")
        .def("lock",   &ConsumerSpinLockPy::lock)
        .def("unlock", &ConsumerSpinLockPy::unlock)
        .def("try_lock_for", &ConsumerSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process",
             &ConsumerSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ConsumerSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ConsumerSpinLockPy::exit);
}
