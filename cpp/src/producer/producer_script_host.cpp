/**
 * @file producer_script_host.cpp
 * @brief ProducerScriptHost — role-specific implementation.
 *
 * The common do_python_work() skeleton lives in PythonRoleHostBase.
 * This file provides the producer-specific virtual hook overrides:
 *  - Timer-driven production loop
 *  - Single output channel with hub::Producer
 *  - on_produce callback dispatch
 */
#include "producer_script_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "zmq_poll_loop.hpp"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace py = pybind11;

namespace pylabhub::producer
{

using scripting::IncomingMessage;

namespace
{

bool parse_on_produce_return(const py::object &ret)
{
    if (ret.is_none())
        return true;
    if (py::isinstance<py::bool_>(ret))
        return ret.cast<bool>();
    LOGGER_ERROR("[prod] on_produce() must return bool or None — treating as skip");
    return false;
}

} // anonymous namespace

// ============================================================================
// Destructor — must call shutdown_() before subclass members are destroyed
// ============================================================================

ProducerScriptHost::~ProducerScriptHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void ProducerScriptHost::set_config(ProducerConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — identity and script loading
// ============================================================================

void ProducerScriptHost::wire_api_identity()
{
    api_.set_uid(config_.producer_uid);
    api_.set_name(config_.producer_name);
    api_.set_channel(config_.channel);
    api_.set_log_level(config_.log_level);
    api_.set_script_dir(config_.script_path);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
}

void ProducerScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_produce_ = py::getattr(mod, "on_produce", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
}

bool ProducerScriptHost::has_required_callback() const
{
    return scripting::is_callable(py_on_produce_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool ProducerScriptHost::build_role_types()
{
    using scripting::resolve_schema;

    try
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back((std::filesystem::path(config_.hub_dir) / "schemas").string());

        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "prod", schema_dirs);
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "prod", schema_dirs);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[prod] Schema parse error: {}", e.what());
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
        LOGGER_ERROR("[prod] Failed to build Python schema types: {}", e.what());
        return false;
    }
    return true;
}

void ProducerScriptHost::print_validate_layout()
{
    std::cout << "\nProducer: " << config_.producer_uid << "\n";
    print_slot_layout_(slot_type_, slot_spec_, schema_slot_size_,
                       "  Output slot: SlotFrame");
    print_slot_layout_(fz_type_, core_.fz_spec, core_.schema_fz_size,
                       "  FlexZone: FlexFrame");
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool ProducerScriptHost::start_role()
{
    if (!build_role_types())
        return false;

    hub::ProducerOptions opts;
    opts.channel_name = config_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = config_.shm_enabled;
    opts.schema_hash  = scripting::compute_schema_hash(slot_spec_, core_.fz_spec);
    opts.actor_name   = config_.producer_name;
    opts.actor_uid    = config_.producer_uid;

    if (config_.shm_enabled)
    {
        opts.shm_config.shared_secret        = config_.shm_secret;
        opts.shm_config.ring_buffer_capacity = config_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Latest_only;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = core_.schema_fz_size;

        if (schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4K))
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4K;
            opts.shm_config.logical_unit_size  =
                (schema_slot_size_ == 0) ? 1 : schema_slot_size_;
        }
        else if (schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4M))
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4M;
            opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size4M);
        }
        else
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size16M;
            opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size16M);
        }
    }

    if (!config_.broker.empty())
    {
        if (!out_messenger_.connect(config_.broker, config_.broker_pubkey,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
            LOGGER_WARN("[prod] broker connect failed ({}); degraded", config_.broker);
    }

    auto maybe_producer = hub::Producer::create(out_messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[prod] Failed to create producer for channel '{}'", config_.channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    if (!config_.channel.empty())
    {
        out_messenger_.suppress_periodic_heartbeat(config_.channel);
        out_messenger_.enqueue_heartbeat(config_.channel);
    }

    // Graceful shutdown: queue channel_closing as a regular event message.
    // The script sees it in FIFO order and is expected to call api.stop().
    out_producer_->on_channel_closing([this]() {
        LOGGER_INFO("[prod] CHANNEL_CLOSING_NOTIFY received, queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired — bypass queue, immediate stop.
    out_producer_->on_force_shutdown([this]() {
        LOGGER_WARN("[prod] FORCE_SHUTDOWN received, forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Helper: hex-encode a binary ZMQ identity so it's safe for JSON→py::str conversion.
    // ZMQ identities can contain arbitrary bytes (non-UTF-8), which would cause
    // UnicodeDecodeError when json_to_py() converts to py::str.
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

    // Wire peer events → IncomingMessage queue as event dicts.
    out_producer_->on_consumer_joined(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[prod] peer_event: consumer_joined identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[prod] peer_event: consumer_left identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    // Wire broker notifications → IncomingMessage queue.
    out_messenger_.on_consumer_died(config_.channel,
        [this](uint64_t pid, std::string reason)
        {
            LOGGER_INFO("[prod] broker_notify: consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.channel,
        [this](std::string event, nlohmann::json details)
        {
            LOGGER_INFO("[prod] broker_notify: channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = std::move(details);
            msg.details["detail"] = std::move(event);
            core_.enqueue_message(std::move(msg));
        });

    // Route consumer ZMQ messages to incoming queue.
    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data)
        {
            LOGGER_INFO("[prod] zmq_data: consumer_message size={}", data.size());
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    if (!out_producer_->start_embedded())
    {
        LOGGER_ERROR("[prod] out_producer->start_embedded() failed");
        return false;
    }

    // Wire API + flexzone.
    try
    {
        // Ensure the embedded pybind11 module is imported so the ProducerAPI type
        // is registered. The user script may or may not import it; we need the type
        // to be known to pybind11 before py::cast.
        py::module_::import("pylabhub_producer");

        // LIFETIME: api_obj_ borrows a raw pointer to api_ (reference policy).
        // api_ must outlive any Python code that holds a reference to api_obj_.
        // This is guaranteed because api_ is a member of ProducerScriptHost and
        // Python callbacks only run while the script host is alive (between start_role
        // and stop_role). set_producer/set_messenger are cleared in stop_role before
        // out_producer_ is destroyed.
        api_obj_ = py::cast(&api_, py::return_value_policy::reference);
        api_.set_producer(&*out_producer_);
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
        LOGGER_ERROR("[prod] Failed to build flexzone view: {}", e.what());
        return false;
    }

    LOGGER_INFO("[prod] Producer started on channel '{}' (shm={})", config_.channel,
                out_producer_->has_shm());

    core_.running_threads.store(true);

    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init_common_();

    if (!core_.running_threads.load())
        return true;

    loop_thread_ = std::thread([this] { run_loop_shm_(); });

    main_thread_release_.emplace();
    return true;
}

void ProducerScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming();

    {
        py::gil_scoped_release release;
        // join() is unbounded: if a thread is stuck (e.g., SHM acquire hung or ZMQ
        // context not yet terminated), this call blocks indefinitely. In practice the
        // shutdown path sets shutdown_requested and closes the ZMQ context (ETERM),
        // which unblocks both threads within milliseconds. A timed join would require
        // a detach fallback, which risks use-after-free on the ProducerImpl data.
        // Revisit if watchdog-driven kill is ever needed.
        if (loop_thread_.joinable()) loop_thread_.join();
        if (zmq_thread_.joinable())  zmq_thread_.join();
    }

    call_on_stop_common_();

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }

    api_.set_flexzone_obj(nullptr);
    api_.set_producer(nullptr);
    clear_role_pyobjects();
    clear_common_pyobjects_();

    LOGGER_INFO("[prod] Producer stopped.");
}

void ProducerScriptHost::cleanup_on_start_failure()
{
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
}

void ProducerScriptHost::clear_role_pyobjects()
{
    py_on_produce_ = py::none();
    slot_type_     = py::none();
}

void ProducerScriptHost::update_fz_checksum_after_init()
{
    if (core_.has_fz)
    {
        if (auto *shm = out_producer_->shm())
            (void)shm->update_checksum_flexible_zone();
    }
}

// ============================================================================
// Slot view builder (write mode)
// ============================================================================

py::object ProducerScriptHost::make_out_slot_view_(void *data, size_t size) const
{
    return scripting::make_slot_view(
        slot_spec_, slot_type_, data, size, /*is_read_side=*/false);
}

// ============================================================================
// Python callback wrapper
// ============================================================================

bool ProducerScriptHost::call_on_produce_(py::object &out_sv, py::object &fz,
                                           py::list &msgs)
{
    py::object ret;
    try
    {
        ret = py_on_produce_(out_sv, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[prod] on_produce error: {}", e.what());
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }
    return parse_on_produce_return(ret);
}

// ============================================================================
// run_loop_shm_ — timer-driven production loop
// ============================================================================

void ProducerScriptHost::run_loop_shm_()
{
    auto *out_shm = out_producer_->shm();

    if (out_shm == nullptr)
    {
        LOGGER_ERROR("[prod] Output SHM unavailable (channel='{}')", config_.channel);
        core_.running_threads.store(false);
        return;
    }

    static constexpr int kShmBlockMs = 5000;
    const int acquire_ms = config_.timeout_ms > 0 ? config_.timeout_ms : kShmBlockMs;

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        // Timer-driven: sleep interval_ms between iterations.
        if (config_.interval_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds{config_.interval_ms});

        if (!core_.running_threads.load() || core_.shutdown_requested.load() ||
            api_.critical_error())
            break;

        auto msgs = core_.drain_messages();

        auto out_handle = out_shm->acquire_write_slot(acquire_ms);
        if (!out_handle)
        {
            api_.increment_drops();
            LOGGER_WARN("[prod] Output SHM full — skipped iteration");
            continue;
        }

        auto         out_span = out_handle->buffer_span();
        const size_t out_sz   = std::min(out_span.size_bytes(), schema_slot_size_);
        std::memset(out_span.data(), 0, out_sz);

        bool commit = false;
        {
            py::gil_scoped_acquire g;
            try
            {
                py::object out_sv = make_out_slot_view_(out_span.data(), out_sz);
                py::list   mlst   = build_messages_list_(msgs);
                commit = call_on_produce_(out_sv, fz_inst_, mlst);
            }
            catch (py::error_already_set &e)
            {
                api_.increment_script_errors();
                LOGGER_ERROR("[prod] Python error in produce loop: {}", e.what());
                if (config_.stop_on_script_error)
                    core_.running_threads.store(false);
            }
        }

        if (commit)
        {
            (void)out_handle->commit(out_sz);
            if (config_.update_checksum)
            {
                (void)out_handle->update_checksum_slot();
                if (core_.has_fz)
                    (void)out_handle->update_checksum_flexible_zone();
            }
            api_.increment_out_written();
        }
        else
        {
            api_.increment_drops();
        }
        (void)out_shm->release_write_slot(*out_handle);

        iteration_count_.fetch_add(1, std::memory_order_relaxed);
    }

    LOGGER_INFO("[prod] run_loop_shm_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={} g_shutdown={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                api_.critical_error(),
                core_.g_shutdown ? core_.g_shutdown->load() : false);
}

// ============================================================================
// run_zmq_thread_ — polls producer peer socket, sends heartbeats
// ============================================================================

void ProducerScriptHost::run_zmq_thread_()
{
    scripting::ZmqPollLoop loop{core_, "prod:" + config_.producer_uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
    };
    loop.get_iteration = [&] { return iteration_count_.load(std::memory_order_relaxed); };
    loop.periodic_tasks.emplace_back(
        [&] { out_messenger_.enqueue_heartbeat(config_.channel,
                                                api_.snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::producer
