/**
 * @file processor_api.cpp
 * @brief ProcessorAPI method implementations + pylabhub_processor pybind11 module.
 */
#include "utils/script_engine.hpp"
#include "processor_api.hpp"

#include "plh_version_registry.hpp"
#include "utils/format_tools.hpp"
#include "utils/logger.hpp"

#include <chrono>
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
    core_->request_stop();
}

void ProcessorAPI::set_critical_error()
{
    core_->set_critical_error();
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
    if (!producer_)
        return false;
    producer_->sync_flexzone_checksum();
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
    core_->report_metric(key, value);
}

void ProcessorAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    core_->report_metrics(kv);
}

void ProcessorAPI::clear_custom_metrics()
{
    core_->clear_custom_metrics();
}

std::string ProcessorAPI::stop_reason() const noexcept
{
    return core_->stop_reason_string();
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
    nlohmann::json result;

    if (consumer_ != nullptr)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, consumer_->queue_metrics());
        result["in_queue"] = std::move(q);
    }
    if (producer_ != nullptr)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, producer_->queue_metrics());
        result["out_queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_->loop_metrics());
        result["loop"] = std::move(lm);
    }

    {
        uint64_t in_dropped  = consumer_ ? consumer_->ctrl_queue_dropped() : 0;
        uint64_t out_dropped = producer_ ? producer_->ctrl_queue_dropped() : 0;
        result["role"] = {
            {"in_received",        in_slots_received()},
            {"out_written",        out_slots_written()},
            {"drops",              out_drop_count()},
            {"script_errors",      script_error_count()},
            {"ctrl_queue_dropped", {{"input", in_dropped}, {"output", out_dropped}}}
        };
    }

    if (inbox_queue_ != nullptr)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, inbox_queue_->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    {
        auto cm = core_->custom_metrics_snapshot();
        if (!cm.empty())
            result["custom"] = nlohmann::json(cm);
    }

    return result;
}

py::dict ProcessorAPI::metrics() const
{
    py::dict d;

    if (consumer_ != nullptr)
    {
        py::dict q;
        scripting::queue_metrics_to_pydict(q, consumer_->queue_metrics());
        d["in_queue"] = q;
    }
    if (producer_ != nullptr)
    {
        py::dict q;
        scripting::queue_metrics_to_pydict(q, producer_->queue_metrics());
        d["out_queue"] = q;
    }

    {
        py::dict loop;
        scripting::loop_metrics_to_pydict(loop, core_->loop_metrics());
        d["loop"] = loop;
    }

    {
        uint64_t in_dropped  = consumer_ ? consumer_->ctrl_queue_dropped() : 0;
        uint64_t out_dropped = producer_ ? producer_->ctrl_queue_dropped() : 0;
        py::dict role;
        role["in_received"]        = py::int_(in_slots_received());
        role["out_written"]        = py::int_(out_slots_written());
        role["drops"]              = py::int_(out_drop_count());
        role["script_errors"]      = py::int_(script_error_count());
        py::dict cqd;
        cqd["input"]  = py::int_(in_dropped);
        cqd["output"] = py::int_(out_dropped);
        role["ctrl_queue_dropped"] = cqd;
        d["role"] = role;
    }

    if (inbox_queue_ != nullptr)
    {
        py::dict ib;
        scripting::inbox_metrics_to_pydict(ib, inbox_queue_->inbox_metrics());
        d["inbox"] = ib;
    }

    {
        auto cm = core_->custom_metrics_snapshot();
        if (!cm.empty())
        {
            py::dict custom;
            for (auto &[k, v] : cm)
                custom[py::str(k)] = py::float_(v);
            d["custom"] = custom;
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
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;

    if (!engine_)
        return py::none();

    std::optional<scripting::ScriptEngine::InboxOpenResult> result;
    {
        py::gil_scoped_release release;
        result = engine_->open_inbox_client(target_uid);
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
    return consumer_ ? consumer_->last_seq() : 0u;
}

uint64_t ProcessorAPI::in_capacity() const noexcept
{
    return consumer_ ? consumer_->queue_capacity() : 0u;
}

std::string ProcessorAPI::in_policy() const
{
    return consumer_ ? consumer_->queue_policy_info() : std::string{};
}

uint64_t ProcessorAPI::out_capacity() const noexcept
{
    return producer_ ? producer_->queue_capacity() : 0u;
}

std::string ProcessorAPI::out_policy() const
{
    return producer_ ? producer_->queue_policy_info() : std::string{};
}

void ProcessorAPI::set_verify_checksum(bool enable)
{
    if (consumer_)
        consumer_->set_verify_checksum(enable, false);
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
             "Cycles where the loop exceeded its configured deadline. 0 when no period configured.")
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
             "Total ctrl-send messages dropped by both in and out queues due to overflow.")
        .def_readwrite("shared_data",   &ProcessorAPI::shared_data_,
             "Shared script data dictionary. Persists across callbacks.");

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    }, "Return JSON string with all component version information.");

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
