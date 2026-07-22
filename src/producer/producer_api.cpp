/**
 * @file producer_api.cpp
 * @brief ProducerAPI Python-wrapping methods + pylabhub_producer pybind11 module.
 *
 * Phase 2: All C++ logic delegates to RoleAPIBase. This file only contains
 * Python-specific type conversions and the pybind11 module registration.
 */
#include "utils/script_engine.hpp"
#include "producer_api.hpp"

#include "plh_version_registry.hpp"
#include "python_helpers.hpp"
#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"
#include "utils/metrics_json.hpp"
#include "metrics_pydict.hpp"
#include "../scripting/json_py_helpers.hpp" // detail::json_to_py (S5)
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
namespace scripting = pylabhub::scripting;

namespace pylabhub::producer
{

// ============================================================================
// Python-wrapping methods (delegate to RoleAPIBase with type conversion)
// ============================================================================

py::object ProducerAPI::flexzone(std::optional<int> /*side*/) const
{
    // Producer has only Tx side; side arg is ignored (accepted for API consistency).
    if (!tx_flexzone_obj_.has_value())
        return py::none();
    return *tx_flexzone_obj_;
}

// ── Band pub/sub (HEP-CORE-0030) ─────────────────────────────────────────────

py::object ProducerAPI::band_join(const std::string &channel)
{
    // Use the shared fast-path walker — same converter the dispatch hot
    // path uses; replaces the prior `json.loads(dump())` round-trip.
    // Release GIL across the broker REQ (can block on slow round-trip).
    std::optional<nlohmann::json> result;
    {
        py::gil_scoped_release release;
        result = base_->band_join(channel);
    }
    if (!result.has_value())
        return py::none();
    return scripting::detail::json_to_py(*result);
}

void ProducerAPI::band_broadcast(const std::string &channel, py::dict body)
{
    // Convert body py::dict → nlohmann::json via shared walker (no
    // json.dumps round-trip).  Release GIL across the broker send.
    auto body_json = scripting::detail::py_to_json(body);
    py::gil_scoped_release release;
    base_->band_broadcast(channel, body_json);
}

py::object ProducerAPI::band_members(const std::string &channel)
{
    std::optional<nlohmann::json> result;
    {
        py::gil_scoped_release release;
        result = base_->band_members(channel);
    }
    if (!result.has_value())
        return py::none();
    return scripting::detail::json_to_py(*result);
}

bool ProducerAPI::is_in_band(const std::string &channel) const
{
    return base_->is_in_band(channel);
}

// Engine-parity inquiry helpers (Native + Lua already expose these via
// ctx->band_member_contains / ctx->band_member_count / ctx->allowed_*).
// Pythonic shape: return bool / int rather than int tristate.  Raise on
// transport failure rather than returning -1 sentinel.

bool ProducerAPI::band_member_contains(const std::string &channel, const std::string &role_uid)
{
    // Bug fix 2026-06-16 (#235): see ConsumerAPI counterpart.  Raw
    // broker reply is `{"members": [...]}`; previous `result->is_array()`
    // check was always false; function silently returned false.
    const auto members = scripting::detail::fetch_band_members_or_throw(base_, channel);
    for (const auto &m : members)
        if (m.value("role_uid", std::string{}) == role_uid)
            return true;
    return false;
}

int ProducerAPI::band_member_count(const std::string &channel)
{
    // Bug fix 2026-06-16 (#235): see ConsumerAPI counterpart.
    const auto members = scripting::detail::fetch_band_members_or_throw(base_, channel);
    int count = 0;
    for (const auto &m : members)
        if (!m.value("role_uid", std::string{}).empty())
            ++count;
    return count;
}

bool ProducerAPI::allowed_peer_contains(const std::string &channel,
                                        const std::string &role_uid) const
{
    for (const auto &p : base_->allowed_peers(channel))
        if (p.role_uid == role_uid)
            return true;
    return false;
}

int ProducerAPI::allowed_peer_count(const std::string &channel) const
{
    return static_cast<int>(base_->allowed_peers(channel).size());
}

py::dict ProducerAPI::metrics() const
{
    // S5: was `json.loads(j.dump())` round-trip — replaced with the
    // shared fast-path walker (src/scripting/json_py_helpers.hpp).
    // Same converter `python_engine.cpp::execute_direct_` already
    // uses on the dispatch hot path; semantics match for metrics
    // payloads (no NaN/Inf, no deep recursion — see header docstring
    // for the divergence rows).
    return scripting::detail::json_to_py(base_->snapshot_metrics_json()).cast<py::dict>();
}

py::list ProducerAPI::allowed_peers(const std::string &channel) const
{
    // HEP-CORE-0036 §I11 polling surface (engine-parity with Lua's
    // `api.allowed_peers`).  Returns a Python list of
    // `{"role_uid": str, "pubkey": str}` dicts — the same shape as the
    // `on_allowlist_changed` callback's allowlist argument.  Read-only
    // snapshot per §I11 audit S3 guardrail.
    return scripting::detail::peer_list_to_py(base_->allowed_peers(channel));
}

py::list ProducerAPI::producers(const std::string &channel) const
{
    // HEP-CORE-0028 §6a + HEP-CORE-0017 §3.3.2 LIVE-peer accessor.
    // Returns role_uid strings from live_peers[channel]["producer"],
    // populated by phase=live NOTIFY.  Empty on producer-side roles
    // (map only populates on binding side) — the documented sentinel
    // per HEP-CORE-0011 Cross-Engine Surface Parity.
    return scripting::detail::uid_list_to_py(base_->producers(channel));
}

py::list ProducerAPI::consumers(const std::string &channel) const
{
    // Symmetric with producers().  Fan-out / one-to-one producers
    // are the BINDING side under HEP-CORE-0017 §3.3.0; live consumer
    // set is what api.consumer_count()'s list companion returns.
    return scripting::detail::uid_list_to_py(base_->consumers(channel));
}

uint64_t ProducerAPI::slot_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->slot_logical_size(
        side.has_value()
            ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)}
            : std::nullopt));
}

uint64_t ProducerAPI::flexzone_logical_size(std::optional<int> side) const
{
    return static_cast<uint64_t>(base_->flexzone_logical_size(
        side.has_value()
            ? std::optional<scripting::ChannelSide>{static_cast<scripting::ChannelSide>(*side)}
            : std::nullopt));
}

static std::optional<scripting::ChannelSide> to_channel_side(std::optional<int> side)
{
    if (!side.has_value())
        return std::nullopt;
    return static_cast<scripting::ChannelSide>(*side);
}

py::object ProducerAPI::spinlock(std::size_t index, std::optional<int> side)
{
    try
    {
        return py::cast(scripting::SpinLockPy{base_->get_spinlock(index, to_channel_side(side))},
                        py::return_value_policy::move);
    }
    catch (const std::exception &e)
    {
        throw py::value_error(e.what());
    }
}

uint32_t ProducerAPI::spinlock_count(std::optional<int> side) const
{
    try
    {
        return base_->spinlock_count(to_channel_side(side));
    }
    catch (const std::exception &e)
    {
        throw py::value_error(e.what());
    }
}

// ============================================================================
// Inbox (Python-specific wrapping)
// ============================================================================

py::object ProducerAPI::open_inbox(const std::string &target_uid)
{
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;

    std::optional<scripting::RoleAPIBase::InboxOpenResult> result;
    {
        py::gil_scoped_release release;
        result = base_->open_inbox_client(target_uid);
    }
    if (!result)
        return py::none();

    py::object slot_type = result->spec.fields.empty()
                               ? py::none()
                               : scripting::build_ctypes_struct(result->spec, "InboxSlot");

    py::object handle =
        py::cast(scripting::InboxHandle(std::move(result->client), std::move(result->spec),
                                        std::move(slot_type), result->item_size),
                 py::return_value_policy::move);
    inbox_cache_[target_uid] = handle;
    return handle;
}

bool ProducerAPI::wait_for_role(const std::string &uid, int timeout_ms)
{
    py::gil_scoped_release release;
    return base_->wait_for_role(uid, timeout_ms);
}

void ProducerAPI::clear_inbox_cache()
{
    for (auto &[uid, handle_obj] : inbox_cache_)
    {
        try
        {
            handle_obj.cast<scripting::InboxHandle &>().clear_pyobjects();
        }
        catch (const std::exception &e)
        {
            // Cleanup path — must not throw out (called during stop).
            // Log so broken handles do not silently leak Python refs;
            // continue draining the rest of the cache.
            LOGGER_WARN("ProducerAPI: clear_inbox_cache uid='{}' threw: {}", uid, e.what());
        }
        catch (...)
        {
            LOGGER_WARN("ProducerAPI: clear_inbox_cache uid='{}' "
                        "threw (non-std exception)",
                        uid);
        }
    }
    inbox_cache_.clear();
}

} // namespace pylabhub::producer

// ============================================================================
// pybind11 embedded module: pylabhub_producer
// ============================================================================

PYBIND11_EMBEDDED_MODULE(pylabhub_producer, m) // NOLINT
{
    namespace producer = pylabhub::producer;
    namespace scripting = pylabhub::scripting;

    // Named string constants for stop_reason() comparisons.
    scripting::register_stop_reason_constants(m);

    // Direction objects — required for invoke_produce(tx, msgs, api).
    py::class_<scripting::PyTxChannel>(m, "TxChannel")
        .def_readwrite("slot", &scripting::PyTxChannel::slot);

    py::class_<scripting::PyRxChannel>(m, "RxChannel")
        .def_readwrite("slot", &scripting::PyRxChannel::slot);

    py::class_<scripting::PyInboxMsg>(m, "InboxMsg")
        .def_readonly("data", &scripting::PyInboxMsg::data)
        .def_readonly("sender_uid", &scripting::PyInboxMsg::sender_uid)
        .def_readonly("seq", &scripting::PyInboxMsg::seq);

    m.def("version_info", []() -> py::str { return pylabhub::version::version_info_json(); });

    py::class_<scripting::SpinLockPy>(m, "SpinLock")
        .def("lock", &scripting::SpinLockPy::lock)
        .def("unlock", &scripting::SpinLockPy::unlock)
        .def("try_lock_for", &scripting::SpinLockPy::try_lock_for, py::arg("timeout_ms"))
        .def("is_locked_by_current_process", &scripting::SpinLockPy::is_locked_by_current_process)
        .def("__enter__", &scripting::SpinLockPy::enter, py::return_value_policy::reference)
        .def("__exit__", &scripting::SpinLockPy::exit);

    py::class_<scripting::InboxHandle>(m, "InboxHandle")
        .def("acquire", &scripting::InboxHandle::acquire)
        .def("send", &scripting::InboxHandle::send, py::arg("timeout_ms") = 5000)
        .def("discard", &scripting::InboxHandle::discard)
        .def("is_ready", &scripting::InboxHandle::is_ready)
        .def("close", &scripting::InboxHandle::close);

    py::class_<producer::ProducerAPI>(m, "ProducerAPI")
        .def("uid", &producer::ProducerAPI::uid)
        .def("name", &producer::ProducerAPI::name)
        .def("channel", &producer::ProducerAPI::channel)
        .def("log_level", &producer::ProducerAPI::log_level)
        .def("script_dir", &producer::ProducerAPI::script_dir)
        .def("role_dir", &producer::ProducerAPI::role_dir)
        .def("logs_dir", &producer::ProducerAPI::logs_dir)
        .def("run_dir", &producer::ProducerAPI::run_dir)
        .def("stop", &producer::ProducerAPI::stop)
        .def("set_critical_error", &producer::ProducerAPI::set_critical_error, py::arg("msg"),
             "Flag a critical (unrecoverable) error and request shutdown. "
             "msg is REQUIRED — logged at ERROR level by the framework "
             "before flipping state (uniform across Python/Lua/Native). "
             "stop_reason becomes 'critical_error'. For ordinary stop "
             "use api.stop() (reason='normal').")
        .def("critical_error", &producer::ProducerAPI::critical_error)
        .def("flexzone", &producer::ProducerAPI::flexzone, py::arg("side") = py::none(),
             "Flexzone typed view. Returns None if no flexzone configured.")
        .def("update_flexzone_checksum", &producer::ProducerAPI::update_flexzone_checksum)
        .def("band_join", &producer::ProducerAPI::band_join, py::arg("channel"))
        .def("band_leave", &producer::ProducerAPI::band_leave, py::arg("channel"))
        .def("band_broadcast", &producer::ProducerAPI::band_broadcast, py::arg("channel"),
             py::arg("body"))
        .def("band_members", &producer::ProducerAPI::band_members, py::arg("channel"))
        .def("band_member_contains", &producer::ProducerAPI::band_member_contains,
             py::arg("channel"), py::arg("role_uid"),
             "Engine-parity inquiry — true iff role_uid is in the band's "
             "member list.  Equivalent to `role_uid in api.band_members(ch)` "
             "but avoids materialising the full list in Python.")
        .def("band_member_count", &producer::ProducerAPI::band_member_count, py::arg("channel"),
             "Engine-parity inquiry — band member count.  Raises "
             "ValueError on broker transport failure.")
        .def("is_in_band", &producer::ProducerAPI::is_in_band, py::arg("channel"))
        .def("script_error_count", &producer::ProducerAPI::script_error_count)
        .def("out_slots_written", &producer::ProducerAPI::out_slots_written)
        .def("out_drop_count", &producer::ProducerAPI::out_drop_count)
        .def("loop_overrun_count", &producer::ProducerAPI::loop_overrun_count,
             "Cycles where start-to-start time exceeded configured period. "
             "0 when period==0 (free-run) or not connected.")
        .def("out_capacity", &producer::ProducerAPI::out_capacity,
             "Ring/send buffer slot count for the output transport queue. 0 if not connected.")
        .def("out_policy", &producer::ProducerAPI::out_policy,
             "Overflow policy description (e.g. 'shm_write', 'zmq_push_drop').")
        .def("last_cycle_work_us", &producer::ProducerAPI::last_cycle_work_us,
             "Microseconds of active work (acquire+script+commit) in the last iteration.")
        .def("metrics", &producer::ProducerAPI::metrics,
             "Combined metrics dict: DataBlock ContextMetrics + loop_overruns + script_errors.")
        .def("allowed_peers", &producer::ProducerAPI::allowed_peers, py::arg("channel"),
             "HEP-CORE-0036 §I11 polling surface — snapshot of "
             "authorized peers for the named channel.  Returns a list of "
             "{'role_uid': str, 'pubkey': str} dicts.  Empty when no "
             "GET_CHANNEL_AUTH_REQ has completed.  Engine-parity with "
             "Lua's api.allowed_peers; read-only.")
        .def("producers", &producer::ProducerAPI::producers, py::arg("channel"),
             "HEP-CORE-0028 §6a + HEP-CORE-0017 §3.3.2 — live producer "
             "role_uid list, backed by live_peers[channel] populated by "
             "phase=live NOTIFY.  Returns list[str].  Empty on non-"
             "binding side (documented sentinel per HEP-CORE-0011 "
             "Cross-Engine Surface Parity).")
        .def("consumers", &producer::ProducerAPI::consumers, py::arg("channel"),
             "HEP-CORE-0028 §6a — live consumer role_uid list, symmetric "
             "with producers().  Fan-out / one-to-one producer role's "
             "binding-side observation of live subscribers; use with "
             "consumer_count() for the size + list pair.")
        .def("allowed_peer_contains", &producer::ProducerAPI::allowed_peer_contains,
             py::arg("channel"), py::arg("role_uid"),
             "Engine-parity inquiry — true iff role_uid is in the "
             "channel's authorized-peer list.  O(N) in the local cache; "
             "no broker round-trip.")
        .def("allowed_peer_count", &producer::ProducerAPI::allowed_peer_count, py::arg("channel"),
             "Engine-parity inquiry — authorized-peer count for the "
             "channel.  Served from local cache; no broker round-trip.")
        .def("consumer_count", &producer::ProducerAPI::consumer_count, py::arg("channel"),
             "HEP-CORE-0028 §6a — binding-side live consumer count "
             "backed by the broker's phase=live NOTIFY stream "
             "(HEP-CORE-0007 lines 1834-1838).  Fan-out producers gate "
             "the first publish on this count>0 to close the libzmq "
             "PUB/SUB slow-joiner window.  0 when the map is not "
             "populated for this role (documented sentinel per "
             "HEP-CORE-0011 Cross-Engine Surface Parity).")
        .def("producer_count", &producer::ProducerAPI::producer_count, py::arg("channel"),
             "HEP-CORE-0028 §6a — binding-side live producer count "
             "(symmetric with consumer_count on the consumer side of "
             "the channel).")
        .def("is_channel_ready", &producer::ProducerAPI::is_channel_ready, py::arg("channel"),
             "HEP-CORE-0036 §6.7 (#190) — true iff the queue serving the "
             "named channel is in the Active state.  Use as a script-side "
             "gate from non-data-loop callbacks (on_init, on_band_message, "
             "etc.) — cycle ops already short-circuits the data-loop "
             "callback on Standby.  Read-only.  Engine-parity with "
             "Lua's api.is_channel_ready.")
        .def("queue_mechanism", &producer::ProducerAPI::queue_mechanism, py::arg("side"),
             "HEP-CORE-0035 §2 (#194) — direct mechanism accessor: returns "
             "the libzmq-negotiated mechanism name "
             "('Curve'/'Plaintext'/'Uninitialized') for the named side "
             "(0=Tx, 1=Rx).  Engine-parity with Lua's api.queue_mechanism; "
             "closes the parity gap left by #186 which only wired Lua.")
        .def("slot_logical_size", &producer::ProducerAPI::slot_logical_size,
             py::arg("side") = py::none(), "Logical C struct size for the slot schema (bytes).")
        .def("flexzone_logical_size", &producer::ProducerAPI::flexzone_logical_size,
             py::arg("side") = py::none(), "Logical C struct size for the flexzone schema (bytes).")
        .def("spinlock", &producer::ProducerAPI::spinlock, py::arg("index"),
             py::arg("side") = py::none())
        .def("spinlock_count", &producer::ProducerAPI::spinlock_count, py::arg("side") = py::none())
        .def_property_readonly_static("Tx", [](py::object)
                                      { return static_cast<int>(scripting::ChannelSide::Tx); })
        .def_property_readonly_static("Rx", [](py::object)
                                      { return static_cast<int>(scripting::ChannelSide::Rx); })
        .def("report_metric", &producer::ProducerAPI::report_metric, py::arg("key"),
             py::arg("value"), "Report a custom metric (key-value pair) for broker aggregation.")
        .def("report_metrics", &producer::ProducerAPI::report_metrics, py::arg("kv"),
             "Report multiple custom metrics at once.")
        .def("clear_custom_metrics", &producer::ProducerAPI::clear_custom_metrics,
             "Clear all custom metrics.")
        .def("open_inbox", &producer::ProducerAPI::open_inbox, py::arg("target_uid"))
        .def("clear_inbox_cache", &producer::ProducerAPI::clear_inbox_cache)
        .def("wait_for_role", &producer::ProducerAPI::wait_for_role, py::arg("uid"),
             py::arg("timeout_ms") = 5000)
        .def("stop_reason", &producer::ProducerAPI::stop_reason,
             "Why the role stopped: 'normal', 'peer_dead', 'hub_dead', or 'critical_error'.")
        .def_readwrite("shared_data", &producer::ProducerAPI::shared_data_,
                       "Shared script data dictionary. Persists across callbacks.")
        .def_static("as_numpy", &scripting::as_numpy_view, py::arg("ctypes_array"),
                    "Convert a ctypes array field to a numpy ndarray view (zero-copy).")
        .def("log", &producer::ProducerAPI::log, py::arg("level"), py::arg("msg"));
}
