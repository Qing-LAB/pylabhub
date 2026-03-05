/**
 * @file consumer_script_host.cpp
 * @brief ConsumerScriptHost — role-specific implementation.
 *
 * The common do_python_work() skeleton lives in PythonRoleHostBase.
 * This file provides the consumer-specific virtual hook overrides:
 *  - Demand-driven consumption loop
 *  - Single input channel with hub::Consumer
 *  - on_consume callback dispatch
 *  - Read-only flexzone and message list without sender
 */
#include "consumer_script_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "zmq_poll_loop.hpp"

#include <cstring>
#include <iostream>
#include <thread>

namespace py = pybind11;

namespace pylabhub::consumer
{

using scripting::IncomingMessage;

// ============================================================================
// Destructor
// ============================================================================

ConsumerScriptHost::~ConsumerScriptHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void ConsumerScriptHost::set_config(ConsumerConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — identity and script loading
// ============================================================================

void ConsumerScriptHost::wire_api_identity()
{
    api_.set_uid(config_.consumer_uid);
    api_.set_name(config_.consumer_name);
    api_.set_channel(config_.channel);
    api_.set_log_level(config_.log_level);
    api_.set_script_dir(config_.script_path);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
}

void ConsumerScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_consume_ = py::getattr(mod, "on_consume", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
}

bool ConsumerScriptHost::has_required_callback() const
{
    return scripting::is_callable(py_on_consume_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool ConsumerScriptHost::build_role_types()
{
    using scripting::resolve_schema;

    try
    {
        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "cons");
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "cons");
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
        return false;
    }

    try
    {
        if (!build_schema_type_(slot_spec_, slot_type_, schema_slot_size_, "SlotFrame"))
            return false;
        if (!build_flexzone_type_())
            return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Failed to build Python schema types: {}", e.what());
        return false;
    }
    return true;
}

void ConsumerScriptHost::print_validate_layout()
{
    std::cout << "\nConsumer: " << config_.consumer_uid << "\n";
    print_slot_layout_(slot_type_, slot_spec_, schema_slot_size_,
                       "  Input slot: SlotFrame");
    print_slot_layout_(fz_type_, core_.fz_spec, core_.schema_fz_size,
                       "  FlexZone: FlexFrame");
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool ConsumerScriptHost::start_role()
{
    if (!build_role_types())
        return false;

    hub::ConsumerOptions opts;
    opts.channel_name         = config_.channel;
    opts.shm_shared_secret    = config_.shm_enabled ? config_.shm_secret : 0u;
    opts.expected_schema_hash = scripting::compute_schema_hash(slot_spec_, core_.fz_spec);
    opts.consumer_uid         = config_.consumer_uid;
    opts.consumer_name        = config_.consumer_name;

    if (!config_.broker.empty())
    {
        if (!in_messenger_.connect(config_.broker, config_.broker_pubkey,
                                   config_.auth.client_pubkey, config_.auth.client_seckey))
            LOGGER_WARN("[cons] broker connect failed ({}); degraded", config_.broker);
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", config_.channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Graceful shutdown: queue channel_closing as a regular event message.
    // The script sees it in FIFO order and is expected to call api.stop().
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[cons] CHANNEL_CLOSING_NOTIFY received, queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired — bypass queue, immediate stop.
    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[cons] FORCE_SHUTDOWN received, forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Route ZMQ data messages to incoming queue.
    in_consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            LOGGER_DEBUG("[cons] zmq_data: data_message size={}", data.size());
            IncomingMessage msg;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    // Wire producer control messages → IncomingMessage queue as event dicts.
    // NOTE: The data payload may contain arbitrary binary bytes. We hex-encode it
    // to avoid UnicodeDecodeError when json_to_py() converts to py::str.
    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
            LOGGER_INFO("[cons] ctrl_msg: producer_message type='{}' size={}",
                        type, data.size());
            static constexpr char kHex[] = "0123456789abcdef";
            IncomingMessage msg;
            msg.event = "producer_message";
            msg.details["type"] = std::string(type);
            std::string hex;
            hex.reserve(data.size() * 2);
            for (auto b : data)
            {
                auto c = static_cast<unsigned char>(b);
                hex.push_back(kHex[c >> 4]);
                hex.push_back(kHex[c & 0x0f]);
            }
            msg.details["data"] = std::move(hex);
            core_.enqueue_message(std::move(msg));
        });

    // Wire broker channel error/event notifications.
    in_consumer_->on_channel_error(
        [this](const std::string &event, const nlohmann::json &details)
        {
            LOGGER_INFO("[cons] broker_notify: channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = details;
            msg.details["detail"] = event;
            core_.enqueue_message(std::move(msg));
        });

    if (!in_consumer_->start_embedded())
    {
        LOGGER_ERROR("[cons] in_consumer->start_embedded() failed");
        return false;
    }

    // Wire API and input flexzone (read-only view).
    try
    {
        // Ensure the embedded pybind11 module is imported so the ConsumerAPI type
        // is registered before py::cast.
        py::module_::import("pylabhub_consumer");

        api_obj_ = py::cast(&api_, py::return_value_policy::reference);
        api_.set_consumer(&*in_consumer_);
        api_.set_messenger(&in_messenger_);

        if (core_.has_fz)
        {
            auto *in_shm = in_consumer_->shm();
            if (in_shm != nullptr)
            {
                auto fz_span = in_shm->flexible_zone_span();
                // Consumer: read-only memoryview into the input SHM flexzone.
                fz_mv_ = py::memoryview::from_memory(
                    const_cast<std::byte *>(fz_span.data()),
                    static_cast<ssize_t>(fz_span.size_bytes()),
                    /*readonly=*/true);

                if (core_.fz_spec.exposure == scripting::SlotExposure::Ctypes)
                {
                    fz_inst_ = fz_type_.attr("from_buffer_copy")(fz_mv_);
                }
                else
                {
                    py::module_ np = py::module_::import("numpy");
                    if (!core_.fz_spec.numpy_shape.empty())
                    {
                        py::list shape;
                        for (auto d : core_.fz_spec.numpy_shape) shape.append(d);
                        fz_inst_ = np.attr("ndarray")(shape, fz_type_, fz_mv_);
                    }
                    else
                    {
                        const size_t items =
                            fz_span.size_bytes() / fz_type_.attr("itemsize").cast<size_t>();
                        fz_inst_ = np.attr("ndarray")(
                            py::make_tuple(static_cast<ssize_t>(items)), fz_type_, fz_mv_);
                    }
                }
            }
        }
        if (fz_inst_.is_none())
            fz_inst_ = py::none();
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[cons] Failed to build flexzone view: {}", e.what());
        return false;
    }

    LOGGER_INFO("[cons] Consumer started on channel '{}'", config_.channel);

    core_.running_threads.store(true);

    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init_common_();

    if (!core_.running_threads.load())
        return true;

    loop_thread_ = std::thread([this] { run_loop_shm_(); });

    main_thread_release_.emplace();
    return true;
}

void ConsumerScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming();

    {
        py::gil_scoped_release release;
        if (loop_thread_.joinable()) loop_thread_.join();
        if (zmq_thread_.joinable())  zmq_thread_.join();
    }

    call_on_stop_common_();

    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }

    api_.set_consumer(nullptr);
    clear_role_pyobjects();
    clear_common_pyobjects_();

    LOGGER_INFO("[cons] Consumer stopped.");
}

void ConsumerScriptHost::cleanup_on_start_failure()
{
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

void ConsumerScriptHost::clear_role_pyobjects()
{
    py_on_consume_ = py::none();
    slot_type_     = py::none();
}

// ============================================================================
// build_messages_list_ — consumer: bare bytes (no sender) + event dicts
// ============================================================================

py::list ConsumerScriptHost::build_messages_list_(std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message → Python dict (same format as base class).
            py::dict d;
            d["event"] = m.event;
            for (auto &[key, val] : m.details.items())
                d[py::str(key)] = scripting::json_to_py(val);
            lst.append(std::move(d));
        }
        else
        {
            lst.append(
                py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size()));
        }
    }
    return lst;
}

// ============================================================================
// Slot view builder (read-only view)
// ============================================================================

py::object ConsumerScriptHost::make_in_slot_view_(const void *data, size_t size) const
{
    if (!slot_spec_.has_schema)
        return py::bytes(reinterpret_cast<const char *>(data), size);

    auto mv = py::memoryview::from_memory(
        const_cast<void *>(data), static_cast<ssize_t>(size), /*readonly=*/true);

    if (slot_spec_.exposure == scripting::SlotExposure::Ctypes)
        return slot_type_.attr("from_buffer_copy")(mv);

    py::module_ np = py::module_::import("numpy");
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape) shape.append(d);
        return np.attr("ndarray")(shape, slot_type_, mv);
    }
    const size_t itemsize = slot_type_.attr("itemsize").cast<size_t>();
    const size_t count    = (itemsize > 0) ? (size / itemsize) : 0;
    return np.attr("ndarray")(py::make_tuple(static_cast<ssize_t>(count)), slot_type_, mv);
}

// ============================================================================
// Python callback wrapper
// ============================================================================

void ConsumerScriptHost::call_on_consume_(py::object &in_sv, py::object &fz,
                                           py::list &msgs)
{
    try
    {
        py_on_consume_(in_sv, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[cons] on_consume error: {}", e.what());
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
    }
}

// ============================================================================
// run_loop_shm_ — demand-driven consumption loop
// ============================================================================

void ConsumerScriptHost::run_loop_shm_()
{
    auto *in_shm = in_consumer_->shm();

    if (in_shm == nullptr)
    {
        LOGGER_ERROR("[cons] Input SHM unavailable (channel='{}')", config_.channel);
        core_.running_threads.store(false);
        return;
    }

    static constexpr int kShmBlockMs = 5000;
    const int acquire_in_ms = config_.timeout_ms > 0 ? config_.timeout_ms : kShmBlockMs;

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        // 1. Block until a slot is available (or timeout).
        auto in_handle = in_shm->acquire_consume_slot(acquire_in_ms);

        // 2. Drain any queued ZMQ messages (no GIL needed).
        auto msgs = core_.drain_messages();

        if (!in_handle)
        {
            // Timeout — notify script if timeout_ms > 0 or there are queued messages.
            if (config_.timeout_ms > 0 || !msgs.empty())
            {
                py::gil_scoped_acquire g;
                try
                {
                    py::object none_in = py::none();
                    py::list   mlst    = build_messages_list_(msgs);
                    call_on_consume_(none_in, fz_inst_, mlst);
                }
                catch (py::error_already_set &e)
                {
                    api_.increment_script_errors();
                    LOGGER_ERROR("[cons] Python error in consume loop (timeout): {}", e.what());
                    if (config_.stop_on_script_error)
                        core_.running_threads.store(false);
                }
            }
            // Always advance — proves the loop is cycling, not stuck.
            iteration_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        api_.increment_in_received();

        if (!core_.running_threads.load() || api_.critical_error())
        {
            (void)in_shm->release_consume_slot(*in_handle);
            break;
        }

        const auto   in_span = in_handle->buffer_span();
        const size_t in_sz   = std::min(in_span.size_bytes(), schema_slot_size_);

        {
            py::gil_scoped_acquire g;
            try
            {
                py::object in_sv = make_in_slot_view_(in_span.data(), in_sz);
                py::list   mlst  = build_messages_list_(msgs);
                call_on_consume_(in_sv, fz_inst_, mlst);
            }
            catch (py::error_already_set &e)
            {
                api_.increment_script_errors();
                LOGGER_ERROR("[cons] Python error in consume loop: {}", e.what());
                if (config_.stop_on_script_error)
                    core_.running_threads.store(false);
            }
        }

        (void)in_shm->release_consume_slot(*in_handle);

        iteration_count_.fetch_add(1, std::memory_order_relaxed);
    }

    LOGGER_INFO("[cons] run_loop_shm_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={} g_shutdown={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                api_.critical_error(),
                core_.g_shutdown ? core_.g_shutdown->load() : false);
}

// ============================================================================
// run_zmq_thread_ — polls consumer ZMQ sockets, sends heartbeats
// ============================================================================

void ConsumerScriptHost::run_zmq_thread_()
{
    scripting::ZmqPollLoop loop{core_, "cons:" + config_.consumer_uid};
    loop.sockets = {
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }},
    };
    loop.get_iteration = [&] { return iteration_count_.load(std::memory_order_relaxed); };
    loop.periodic_tasks.emplace_back(
        [&] { in_messenger_.enqueue_heartbeat(config_.channel); },
        config_.heartbeat_interval_ms);
    // HEP-CORE-0019: periodic metrics report (consumers don't piggyback on heartbeat).
    loop.periodic_tasks.emplace_back(
        [&] { in_messenger_.enqueue_metrics_report(
                  config_.channel, config_.consumer_uid,
                  api_.snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::consumer
