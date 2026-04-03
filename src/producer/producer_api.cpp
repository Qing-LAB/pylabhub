/**
 * @file producer_api.cpp
 * @brief ProducerAPI method implementations + pylabhub_producer pybind11 module.
 */
#include "utils/script_engine.hpp"
#include "producer_api.hpp"

#include "plh_version_registry.hpp"
#include "python_helpers.hpp"
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
    core_->request_stop();
}

void ProducerAPI::set_critical_error()
{
    core_->set_critical_error();
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
    const auto s   = data.cast<std::string>();
    const auto raw = format_tools::bytes_from_hex(identity); // identity is hex string from script
    producer_->send_to(raw, s.data(), s.size());
    return true;
}

py::list ProducerAPI::consumers()
{
    py::list lst;
    if (!producer_)
        return lst;
    // Return hex strings — ZMQ identities are opaque binary; hex makes them
    // printable, loggable, and passable back to api.send() without encoding issues.
    for (const auto &id : producer_->connected_consumers())
        lst.append(py::str(format_tools::bytes_to_hex(id)));
    return lst;
}

bool ProducerAPI::update_flexzone_checksum()
{
    if (!producer_)
        return false;
    producer_->sync_flexzone_checksum();
    return true;
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
    core_->report_metric(key, value);
}

void ProducerAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    core_->report_metrics(kv);
}

void ProducerAPI::clear_custom_metrics()
{
    core_->clear_custom_metrics();
}

uint64_t ProducerAPI::loop_overrun_count() const noexcept
{
    return core_->loop_overrun_count();
}

size_t ProducerAPI::out_capacity() const noexcept
{
    if (!producer_)
        return 0;
    return producer_->queue_capacity();
}

std::string ProducerAPI::out_policy() const
{
    return producer_ ? producer_->queue_policy_info() : std::string{};
}

std::string ProducerAPI::stop_reason() const noexcept
{
    return core_->stop_reason_string();
}

uint64_t ProducerAPI::ctrl_queue_dropped() const noexcept
{
    if (!producer_) return 0u;
    return producer_->ctrl_queue_dropped();
}

nlohmann::json ProducerAPI::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (producer_ != nullptr)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, producer_->queue_metrics());
        result["queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_->loop_metrics());
        result["loop"] = std::move(lm);
    }

    result["role"] = {
        {"out_written",        out_slots_written()},
        {"drops",              out_drop_count()},
        {"script_errors",      script_error_count()},
        {"ctrl_queue_dropped", ctrl_queue_dropped()}
    };

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

py::dict ProducerAPI::metrics() const
{
    py::dict d;

    if (producer_ != nullptr)
    {
        py::dict q;
        scripting::queue_metrics_to_pydict(q, producer_->queue_metrics());
        d["queue"] = q;
    }

    {
        py::dict loop;
        scripting::loop_metrics_to_pydict(loop, core_->loop_metrics());
        d["loop"] = loop;
    }

    py::dict role;
    role["out_written"]        = py::int_(out_slots_written());
    role["drops"]              = py::int_(out_drop_count());
    role["script_errors"]      = py::int_(script_error_count());
    role["ctrl_queue_dropped"] = py::int_(ctrl_queue_dropped());
    d["role"] = role;

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
// ProducerAPI — spinlocks
// ============================================================================

py::object ProducerAPI::spinlock(std::size_t index)
{
    if (!producer_ || !producer_->has_shm())
        throw py::value_error("spinlock: SHM output channel not connected");

    return py::cast(ProducerSpinLockPy{producer_->get_spinlock(index)},
                    py::return_value_policy::move);
}

uint32_t ProducerAPI::spinlock_count() const noexcept
{
    return producer_ ? producer_->spinlock_count() : 0u;
}

// ============================================================================
// ProducerAPI — inbox
// ============================================================================

py::object ProducerAPI::open_inbox(const std::string &target_uid)
{
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;

    if (!engine_)
        return py::none();

    // Delegate to ScriptEngine base — broker query + InboxClient creation + core_ cache.
    std::optional<scripting::ScriptEngine::InboxOpenResult> result;
    {
        py::gil_scoped_release release;  // release GIL during broker round-trip
        result = engine_->open_inbox_client(target_uid);
    }
    if (!result)
        return py::none();

    // Build ctypes slot type from schema (Python-specific wrapping).
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

bool ProducerAPI::wait_for_role(const std::string &uid, int timeout_ms)
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

void ProducerAPI::clear_inbox_cache()
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

} // namespace pylabhub::producer

// ============================================================================
// pybind11 embedded module: pylabhub_producer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_producer, m) // NOLINT
{
    using namespace pylabhub::producer; // NOLINT

    py::class_<scripting::PyTxChannel>(m, "TxChannel")
        .def_readwrite("slot", &scripting::PyTxChannel::slot)
        .def_readwrite("fz",   &scripting::PyTxChannel::fz);

    py::class_<scripting::PyRxChannel>(m, "RxChannel")
        .def_readwrite("slot", &scripting::PyRxChannel::slot)
        .def_readwrite("fz",   &scripting::PyRxChannel::fz);

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

    py::class_<ProducerAPI>(m, "ProducerAPI")
        .def("log",          &ProducerAPI::log,
             py::arg("level"), py::arg("msg"))
        .def("uid",          &ProducerAPI::uid)
        .def("name",         &ProducerAPI::name)
        .def("channel",      &ProducerAPI::channel)
        .def("log_level",    &ProducerAPI::log_level)
        .def("script_dir",   &ProducerAPI::script_dir)
        .def("role_dir",     &ProducerAPI::role_dir)
        .def("logs_dir",     &ProducerAPI::logs_dir)
        .def("run_dir",      &ProducerAPI::run_dir)
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
        .def("loop_overrun_count", &ProducerAPI::loop_overrun_count,
             "Cycles where start-to-start time exceeded configured period. "
             "0 when period==0 (free-run) or not connected.")
        .def("out_capacity",       &ProducerAPI::out_capacity,
             "Ring/send buffer slot count for the output transport queue. 0 if not connected.")
        .def("out_policy",         &ProducerAPI::out_policy,
             "Overflow policy description (e.g. 'shm_write', 'zmq_push_drop').")
        .def("last_cycle_work_us", &ProducerAPI::last_cycle_work_us,
             "Microseconds of active work (acquire+script+commit) in the last iteration.")
        .def("metrics",            &ProducerAPI::metrics,
             "Combined metrics dict: DataBlock ContextMetrics + loop_overruns + script_errors.")
        .def("spinlock",     &ProducerAPI::spinlock, py::arg("index"))
        .def("spinlock_count",&ProducerAPI::spinlock_count)
        .def("report_metric", &ProducerAPI::report_metric,
             py::arg("key"), py::arg("value"),
             "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &ProducerAPI::report_metrics,
             py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &ProducerAPI::clear_custom_metrics,
             "Clear all custom metrics.")
        .def("open_inbox",    &ProducerAPI::open_inbox,    py::arg("target_uid"))
        .def("wait_for_role", &ProducerAPI::wait_for_role,
             py::arg("uid"), py::arg("timeout_ms") = 5000)
        .def("stop_reason",        &ProducerAPI::stop_reason,
             "Why the role stopped: 'normal', 'peer_dead', 'hub_dead', or 'critical_error'.")
        .def("ctrl_queue_dropped", &ProducerAPI::ctrl_queue_dropped,
             "Number of ctrl-send messages dropped due to queue overflow.")
        .def_readwrite("shared_data",   &ProducerAPI::shared_data_,
             "Shared script state dictionary. Persists across callbacks.")
        .def_static("as_numpy", &scripting::as_numpy_view, py::arg("ctypes_array"),
             "Convert a ctypes array field to a numpy ndarray view (zero-copy). "
             "Dtype inferred automatically. Usage: pixels = api.as_numpy(slot.pixels)");

    m.def("version_info", []() -> py::str
    {
        return pylabhub::version::version_info_json();
    }, "Return JSON string with all component version information.");

    py::class_<ProducerSpinLockPy>(m, "ProducerSpinLock")
        .def("lock",   &ProducerSpinLockPy::lock)
        .def("unlock", &ProducerSpinLockPy::unlock)
        .def("try_lock_for", &ProducerSpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process",
             &ProducerSpinLockPy::is_locked_by_current_process)
        .def("__enter__", &ProducerSpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__",  &ProducerSpinLockPy::exit);
}
