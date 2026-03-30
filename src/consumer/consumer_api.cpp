/**
 * @file consumer_api.cpp
 * @brief ConsumerAPI method implementations + pylabhub_consumer pybind11 module.
 */
#include "utils/script_engine.hpp"
#include "consumer_api.hpp"

#include "plh_version_registry.hpp"
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
    core_->request_stop();
}

void ConsumerAPI::set_critical_error()
{
    core_->set_critical_error();
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
    core_->report_metric(key, value);
}

void ConsumerAPI::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    core_->report_metrics(kv);
}

void ConsumerAPI::clear_custom_metrics()
{
    core_->clear_custom_metrics();
}

void ConsumerAPI::set_verify_checksum(bool enable)
{
    if (consumer_)
        consumer_->set_verify_checksum(enable, false);
}

std::string ConsumerAPI::stop_reason() const noexcept
{
    return core_->stop_reason_string();
}

uint64_t ConsumerAPI::ctrl_queue_dropped() const noexcept
{
    if (!consumer_) return 0u;
    return consumer_->ctrl_queue_dropped();
}

nlohmann::json ConsumerAPI::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (consumer_ != nullptr)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, consumer_->queue_metrics());
        result["queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_->loop_metrics());
        result["loop"] = std::move(lm);
    }

    result["role"] = {
        {"in_received",        in_slots_received()},
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

py::dict ConsumerAPI::metrics() const
{
    py::dict d;

    if (consumer_ != nullptr)
    {
        py::dict q;
        scripting::queue_metrics_to_pydict(q, consumer_->queue_metrics());
        d["queue"] = q;
    }

    {
        py::dict loop;
        scripting::loop_metrics_to_pydict(loop, core_->loop_metrics());
        d["loop"] = loop;
    }

    py::dict role;
    role["in_received"]        = py::int_(in_slots_received());
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
// ConsumerAPI — inbox
// ============================================================================

py::object ConsumerAPI::open_inbox(const std::string &target_uid)
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
    if (!consumer_)
        return 0;
    return consumer_->queue_capacity();
}

std::string ConsumerAPI::in_policy() const
{
    return consumer_ ? consumer_->queue_policy_info() : std::string{};
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
             "Cycles where the loop exceeded its configured deadline. 0 when no period configured.")
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
             "Number of ctrl-send messages dropped due to queue overflow.")
        .def_readwrite("shared_data",   &ConsumerAPI::shared_data_,
             "Shared script data dictionary. Persists across callbacks.");

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
