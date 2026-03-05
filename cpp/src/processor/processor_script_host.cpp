/**
 * @file processor_script_host.cpp
 * @brief ProcessorScriptHost — role-specific implementation.
 *
 * The common do_python_work() skeleton lives in PythonRoleHostBase.
 * This file provides the processor-specific virtual hook overrides:
 *  - Dual input/output channels with Consumer + Producer
 *  - Delegates data loop to hub::Processor
 *  - on_process callback dispatch
 */
#include "processor_script_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "zmq_poll_loop.hpp"

#include <cstring>
#include <iostream>
#include <thread>

namespace py = pybind11;

namespace pylabhub::processor
{

using scripting::IncomingMessage;

namespace
{

bool parse_on_process_return(const py::object &ret)
{
    if (ret.is_none())
        return true;
    if (py::isinstance<py::bool_>(ret))
        return ret.cast<bool>();
    LOGGER_ERROR("[proc] on_process() must return bool or None — treating as discard");
    return false;
}

} // anonymous namespace

// ============================================================================
// Destructor
// ============================================================================

ProcessorScriptHost::~ProcessorScriptHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void ProcessorScriptHost::set_config(ProcessorConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — identity and script loading
// ============================================================================

void ProcessorScriptHost::wire_api_identity()
{
    api_.set_uid(config_.processor_uid);
    api_.set_name(config_.processor_name);
    api_.set_in_channel(config_.in_channel);
    api_.set_out_channel(config_.out_channel);
    api_.set_log_level(config_.log_level);
    api_.set_script_dir(config_.script_path);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
}

void ProcessorScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_process_ = py::getattr(mod, "on_process", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
}

bool ProcessorScriptHost::has_required_callback() const
{
    return scripting::is_callable(py_on_process_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool ProcessorScriptHost::build_role_types()
{
    using scripting::resolve_schema;

    try
    {
        in_slot_spec_  = resolve_schema(config_.in_slot_schema_json,  false, "proc");
        out_slot_spec_ = resolve_schema(config_.out_slot_schema_json, false, "proc");
        core_.fz_spec  = resolve_schema(config_.flexzone_schema_json, true,  "proc");
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
        return false;
    }

    try
    {
        if (!build_schema_type_(in_slot_spec_, in_slot_type_, in_schema_slot_size_,
                                "InSlotFrame"))
            return false;
        if (!build_schema_type_(out_slot_spec_, out_slot_type_, out_schema_slot_size_,
                                "OutSlotFrame"))
            return false;
        if (!build_flexzone_type_())
            return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Failed to build Python schema types: {}", e.what());
        return false;
    }
    return true;
}

void ProcessorScriptHost::print_validate_layout()
{
    std::cout << "\nProcessor: " << config_.processor_uid << "\n";
    print_slot_layout_(in_slot_type_, in_slot_spec_, in_schema_slot_size_,
                       "  Input slot: InSlotFrame");
    print_slot_layout_(out_slot_type_, out_slot_spec_, out_schema_slot_size_,
                       "  Output slot: OutSlotFrame");
    print_slot_layout_(fz_type_, core_.fz_spec, core_.schema_fz_size,
                       "  FlexZone: FlexFrame");
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool ProcessorScriptHost::start_role()
{
    if (!build_role_types())
        return false;

    // ── Consumer side (in_channel) ──────────────────────────────────────────
    hub::ConsumerOptions in_opts;
    in_opts.channel_name         = config_.in_channel;
    in_opts.shm_shared_secret    = config_.in_shm_enabled ? config_.in_shm_secret : 0u;
    in_opts.expected_schema_hash = scripting::compute_schema_hash(
                                       in_slot_spec_, scripting::SchemaSpec{});
    in_opts.consumer_uid         = config_.processor_uid;
    in_opts.consumer_name        = config_.processor_name;

    const auto &in_ep  = config_.resolved_in_broker();
    const auto &in_pub = config_.resolved_in_broker_pubkey();
    if (!in_ep.empty())
    {
        if (!in_messenger_.connect(in_ep, in_pub,
                                   config_.auth.client_pubkey, config_.auth.client_seckey))
            LOGGER_WARN("[proc] in_messenger broker connect failed ({}); degraded", in_ep);
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, in_opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to connect consumer to in_channel '{}'",
                     config_.in_channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Graceful shutdown: queue channel_closing as a regular event message (input side).
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[proc] CHANNEL_CLOSING_NOTIFY received (in_channel), queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.in_channel;
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (input side).
    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[proc] FORCE_SHUTDOWN received (in_channel), forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Route ZMQ data messages to the incoming queue.
    in_consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            LOGGER_DEBUG("[proc] zmq_data: in_channel data_message size={}", data.size());
            IncomingMessage msg;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    // Wire consumer-side events → IncomingMessage queue.
    // NOTE: The data payload may contain arbitrary binary bytes. We hex-encode it
    // to avoid UnicodeDecodeError when json_to_py() converts to py::str.
    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
            LOGGER_INFO("[proc] ctrl_msg: in_channel producer_message type='{}' size={}",
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

    in_consumer_->on_channel_error(
        [this](const std::string &event, const nlohmann::json &details)
        {
            LOGGER_INFO("[proc] broker_notify: in_channel channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = details;
            msg.details["detail"] = event;
            msg.details["source"] = "in_channel";
            core_.enqueue_message(std::move(msg));
        });

    if (!in_consumer_->start_embedded())
    {
        LOGGER_ERROR("[proc] in_consumer->start_embedded() failed");
        return false;
    }

    // ── Producer side (out_channel) ─────────────────────────────────────────
    hub::ProducerOptions out_opts;
    out_opts.channel_name = config_.out_channel;
    out_opts.pattern      = hub::ChannelPattern::PubSub;
    out_opts.has_shm      = config_.out_shm_enabled;
    out_opts.schema_hash  = scripting::compute_schema_hash(out_slot_spec_, core_.fz_spec);
    out_opts.actor_name   = config_.processor_name;
    out_opts.actor_uid    = config_.processor_uid;

    if (config_.out_shm_enabled)
    {
        out_opts.shm_config.shared_secret        = config_.out_shm_secret;
        out_opts.shm_config.ring_buffer_capacity = config_.out_shm_slot_count;
        out_opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        out_opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Latest_only;
        out_opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        out_opts.shm_config.flex_zone_size       = core_.schema_fz_size;

        if (out_schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4K))
        {
            out_opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4K;
            out_opts.shm_config.logical_unit_size  =
                (out_schema_slot_size_ == 0) ? 1 : out_schema_slot_size_;
        }
        else if (out_schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4M))
        {
            out_opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4M;
            out_opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size4M);
        }
        else
        {
            out_opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size16M;
            out_opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size16M);
        }
    }

    const auto &out_ep  = config_.resolved_out_broker();
    const auto &out_pub = config_.resolved_out_broker_pubkey();
    if (!out_ep.empty())
    {
        if (!out_messenger_.connect(out_ep, out_pub,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
            LOGGER_WARN("[proc] out_messenger broker connect failed ({}); degraded", out_ep);
    }

    auto maybe_producer = hub::Producer::create(out_messenger_, out_opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to create producer for out_channel '{}'",
                     config_.out_channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    // Graceful shutdown: queue channel_closing event (output side).
    out_producer_->on_channel_closing([this]() {
        LOGGER_INFO("[proc] CHANNEL_CLOSING_NOTIFY received (out_channel), queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.out_channel;
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (output side).
    out_producer_->on_force_shutdown([this]() {
        LOGGER_WARN("[proc] FORCE_SHUTDOWN received (out_channel), forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Helper: hex-encode a binary ZMQ identity so it's safe for JSON→py::str conversion.
    auto hex_identity = [](const std::string &raw) -> std::string
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(raw.size() * 2);
        for (unsigned char c : raw)
        {
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
        return out;
    };

    // Wire producer-side peer events → IncomingMessage queue.
    out_producer_->on_consumer_joined(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[proc] peer_event: out_channel consumer_joined identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[proc] peer_event: out_channel consumer_left identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    // Wire broker notifications for the output channel.
    out_messenger_.on_consumer_died(config_.out_channel,
        [this](uint64_t pid, std::string reason)
        {
            LOGGER_INFO("[proc] broker_notify: out_channel consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.out_channel,
        [this](std::string event, nlohmann::json details)
        {
            LOGGER_INFO("[proc] broker_notify: out_channel channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = std::move(details);
            msg.details["detail"] = std::move(event);
            msg.details["source"] = "out_channel";
            core_.enqueue_message(std::move(msg));
        });

    if (!config_.out_channel.empty())
    {
        out_messenger_.suppress_periodic_heartbeat(config_.out_channel);
        out_messenger_.enqueue_heartbeat(config_.out_channel);
    }

    // Route consumer ZMQ messages (from producer's peer side) to incoming queue.
    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data)
        {
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    if (!out_producer_->start_embedded())
    {
        LOGGER_ERROR("[proc] out_producer->start_embedded() failed");
        return false;
    }

    // ── Wire API and output flexzone ────────────────────────────────────────
    try
    {
        // Ensure the embedded pybind11 module is imported so the ProcessorAPI type
        // is registered before py::cast.
        py::module_::import("pylabhub_processor");

        api_obj_ = py::cast(&api_, py::return_value_policy::reference);
        api_.set_producer(&*out_producer_);
        api_.set_consumer(&*in_consumer_);
        api_.set_messenger(&out_messenger_);

        if (core_.has_fz)
        {
            auto *out_shm = out_producer_->shm();
            if (out_shm != nullptr)
            {
                auto fz_span = out_shm->flexible_zone_span();
                fz_mv_ = py::memoryview::from_memory(
                    fz_span.data(),
                    static_cast<ssize_t>(fz_span.size_bytes()),
                    /*readonly=*/false);

                if (core_.fz_spec.exposure == scripting::SlotExposure::Ctypes)
                {
                    fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
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

        api_.set_flexzone_obj(&fz_inst_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[proc] Failed to build flexzone view: {}", e.what());
        return false;
    }

    LOGGER_INFO("[proc] Processor started: '{}' → '{}'",
                config_.in_channel, config_.out_channel);

    // ── Create ShmQueues wrapping the Consumer/Producer DataBlocks ──────────
    auto *in_shm  = in_consumer_->shm();
    auto *out_shm = out_producer_->shm();

    if (in_shm == nullptr)
    {
        LOGGER_ERROR("[proc] Input SHM unavailable (in_channel='{}')", config_.in_channel);
        return false;
    }
    if (out_shm == nullptr)
    {
        LOGGER_ERROR("[proc] Output SHM unavailable (out_channel='{}')", config_.out_channel);
        return false;
    }

    in_queue_ = hub::ShmQueue::from_consumer_ref(
        *in_shm, in_schema_slot_size_, core_.schema_fz_size, config_.in_channel);
    out_queue_ = hub::ShmQueue::from_producer_ref(
        *out_shm, out_schema_slot_size_, core_.schema_fz_size, config_.out_channel);

    if (config_.update_checksum)
        out_queue_->set_checksum_options(true, core_.has_fz);

    // ── Create hub::Processor ───────────────────────────────────────────────
    hub::ProcessorOptions proc_opts;
    proc_opts.overflow_policy = (config_.overflow_policy == OverflowPolicy::Drop)
                                    ? hub::OverflowPolicy::Drop
                                    : hub::OverflowPolicy::Block;
    proc_opts.input_timeout   = (config_.timeout_ms > 0)
                                    ? std::chrono::milliseconds{config_.timeout_ms}
                                    : std::chrono::milliseconds{5000};
    proc_opts.zero_fill_output = true;

    auto maybe_proc = hub::Processor::create(*in_queue_, *out_queue_, proc_opts);
    if (!maybe_proc.has_value())
    {
        LOGGER_ERROR("[proc] Failed to create hub::Processor");
        return false;
    }
    processor_ = std::move(maybe_proc);

    // ── Install type-erased handler ─────────────────────────────────────────
    processor_->set_raw_handler(
        [this](const void* in_data, const void* /*in_fz*/,
               void* out_data, void* /*out_fz*/) -> bool
        {
            auto msgs = core_.drain_messages();
            const size_t in_sz  = in_queue_->item_size();
            const size_t out_sz = out_queue_->item_size();

            py::gil_scoped_acquire g;
            try
            {
                py::object in_sv  = make_in_slot_view_(in_data, in_sz);
                py::object out_sv = make_out_slot_view_(out_data, out_sz);
                py::list   mlst   = build_messages_list_(msgs);

                bool commit = call_on_process_(in_sv, out_sv, fz_inst_, mlst);
                if (commit)
                    api_.increment_out_written();
                else
                    api_.increment_drops();

                api_.increment_in_received();
                return commit;
            }
            catch (py::error_already_set &e)
            {
                api_.increment_script_errors();
                LOGGER_ERROR("[proc] Python error in process handler: {}", e.what());
                if (config_.stop_on_script_error)
                    core_.running_threads.store(false);
                api_.increment_drops();
                api_.increment_in_received();
                return false;
            }
        });

    // ── Install timeout handler ─────────────────────────────────────────────
    if (config_.timeout_ms > 0)
    {
        processor_->set_timeout_handler(
            [this](void* out_data, void* /*out_fz*/) -> bool
            {
                auto msgs = core_.drain_messages();
                if (msgs.empty() && config_.timeout_ms <= 0)
                    return false;

                const size_t out_sz = out_data ? out_queue_->item_size() : 0;

                py::gil_scoped_acquire g;
                try
                {
                    py::object none_in = py::none();
                    py::object out_sv  = out_data
                        ? make_out_slot_view_(out_data, out_sz)
                        : py::none();
                    py::list mlst = build_messages_list_(msgs);

                    bool commit = call_on_process_(none_in, out_sv, fz_inst_, mlst);
                    if (commit)
                        api_.increment_out_written();
                    else if (out_data)
                        api_.increment_drops();
                    return commit;
                }
                catch (py::error_already_set &e)
                {
                    api_.increment_script_errors();
                    LOGGER_ERROR("[proc] Python error in timeout handler: {}", e.what());
                    if (config_.stop_on_script_error)
                        core_.running_threads.store(false);
                    if (out_data) api_.increment_drops();
                    return false;
                }
            });
    }

    core_.running_threads.store(true);

    // Start ZMQ thread.
    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    // Call on_init with GIL held.
    call_on_init_common_();

    if (!core_.running_threads.load())
        return true; // graceful early exit from on_init

    // Start the demand-driven processing loop via hub::Processor.
    processor_->start();

    // Release GIL so Processor's handler can acquire it for Python callbacks.
    main_thread_release_.emplace();

    return true;
}

void ProcessorScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming();

    // Stop Processor and join worker threads WITHOUT holding the GIL.
    {
        py::gil_scoped_release release;
        if (processor_.has_value())
            processor_->stop();
        if (zmq_thread_.joinable()) zmq_thread_.join();
    }
    // GIL re-held here.

    call_on_stop_common_();

    // Release Processor and Queues first (they reference Producer/Consumer SHM).
    processor_.reset();
    out_queue_.reset();
    in_queue_.reset();

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }

    // Release all Python references with GIL held.
    api_.set_flexzone_obj(nullptr);
    api_.set_producer(nullptr);
    api_.set_consumer(nullptr);
    clear_role_pyobjects();
    clear_common_pyobjects_();

    LOGGER_INFO("[proc] Processor stopped.");
}

void ProcessorScriptHost::cleanup_on_start_failure()
{
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

void ProcessorScriptHost::clear_role_pyobjects()
{
    py_on_process_ = py::none();
    in_slot_type_  = py::none();
    out_slot_type_ = py::none();
}

void ProcessorScriptHost::update_fz_checksum_after_init()
{
    if (core_.has_fz)
    {
        if (auto *shm = out_producer_->shm())
            (void)shm->update_checksum_flexible_zone();
    }
}

// ============================================================================
// Slot view builders
// ============================================================================

py::object ProcessorScriptHost::make_in_slot_view_(const void *data, size_t size) const
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto mv = py::memoryview::from_memory(const_cast<void *>(data),
                                           static_cast<ssize_t>(size),
                                           /*readonly=*/true);
    if (!in_slot_spec_.has_schema)
        return py::bytes(reinterpret_cast<const char *>(data), size);
    if (in_slot_spec_.exposure == scripting::SlotExposure::Ctypes)
        return in_slot_type_.attr("from_buffer_copy")(mv);
    py::module_ np  = py::module_::import("numpy");
    py::object  arr = np.attr("frombuffer")(mv, in_slot_type_);
    if (!in_slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : in_slot_spec_.numpy_shape) shape.append(d);
        arr = arr.attr("reshape")(shape);
    }
    return arr;
}

py::object ProcessorScriptHost::make_out_slot_view_(void *data, size_t size) const
{
    auto mv = py::memoryview::from_memory(data, static_cast<ssize_t>(size),
                                           /*readonly=*/false);
    if (!out_slot_spec_.has_schema)
        return py::bytearray(reinterpret_cast<const char *>(data), size);
    if (out_slot_spec_.exposure == scripting::SlotExposure::Ctypes)
        return out_slot_type_.attr("from_buffer")(mv);
    py::module_ np = py::module_::import("numpy");
    if (!out_slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : out_slot_spec_.numpy_shape) shape.append(d);
        return np.attr("ndarray")(shape, out_slot_type_, mv);
    }
    const size_t itemsize = out_slot_type_.attr("itemsize").cast<size_t>();
    const size_t count    = (itemsize > 0) ? (size / itemsize) : 0;
    return np.attr("ndarray")(py::make_tuple(static_cast<ssize_t>(count)), out_slot_type_, mv);
}

// ============================================================================
// Python callback wrapper
// ============================================================================

bool ProcessorScriptHost::call_on_process_(py::object &in_sv, py::object &out_sv,
                                            py::object &fz,    py::list   &msgs)
{
    py::object ret;
    try
    {
        ret = py_on_process_(in_sv, out_sv, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[proc] on_process error: {}", e.what());
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }
    return parse_on_process_return(ret);
}

// ============================================================================
// run_zmq_thread_ — polls ZMQ sockets and sends heartbeats
// ============================================================================

void ProcessorScriptHost::run_zmq_thread_()
{
    scripting::ZmqPollLoop loop{core_, "proc:" + config_.processor_uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }},
    };
    loop.get_iteration = [&] {
        return processor_.has_value() ? processor_->iteration_count() : 0;
    };
    loop.periodic_tasks.emplace_back(
        [&] { out_messenger_.enqueue_heartbeat(config_.out_channel); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::processor
