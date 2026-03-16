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

#include <chrono>
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

/// Returns {commit, script_error}. commit=false, script_error=true on wrong type.
std::pair<bool, bool> parse_on_produce_return(const py::object &ret)
{
    if (ret.is_none())
        return {true, false};
    if (py::isinstance<py::bool_>(ret))
        return {ret.cast<bool>(), false};
    // LR-05: wrong return type is a script error — increment counter + log.
    LOGGER_ERROR("[prod] on_produce() must return bool or None — treating as skip");
    return {false, true};
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
    python_venv_ = config_.python_venv;
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
    api_.set_role_dir(config_.role_dir);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
    api_.set_stop_reason(&stop_reason_);
}

void ProducerScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_produce_ = py::getattr(mod, "on_produce", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
    // on_inbox is optional — only extract when inbox is configured
    if (config_.has_inbox() && py::hasattr(mod, "on_inbox"))
        py_on_inbox_ = mod.attr("on_inbox");
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

    // HEP-0011 §3.1: script.type should be explicit. Warn when defaulted.
    if (!config_.script_type_explicit)
        LOGGER_WARN("[prod] Config 'script.type' absent — defaulting to '{}'. "
                    "Set it explicitly to suppress this warning.", config_.script_type);

    try
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back((std::filesystem::path(config_.hub_dir) / "schemas").string());

        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "prod", schema_dirs);
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "prod", schema_dirs);

        // Inbox schema (optional) — resolve here while schema_dirs is in scope
        if (config_.has_inbox())
            inbox_spec_ = resolve_schema(config_.inbox_schema_json, false, "prod", schema_dirs);
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

        // Build inbox Python type
        if (config_.has_inbox())
        {
            if (!build_schema_type_(inbox_spec_, inbox_type_, inbox_schema_slot_size_, "InboxFrame"))
                return false;
        }
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
    // DataBlock-layer overrun detection: auto-activated when target_period_ms > 0.
    // This wires the same period into acquire_write_slot() so overrun_count tracks
    // start-to-start violations without a separate config field.
    if (config_.target_period_ms > 0)
    {
        opts.loop_policy = hub::LoopPolicy::FixedRate;
        opts.period_ms   = std::chrono::milliseconds{config_.target_period_ms};
    }
    opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    // Inbox setup (optional) — must start before Producer::create so actual_endpoint
    // can be included in opts.inbox_endpoint → REG_REQ.
    if (config_.has_inbox())
    {
        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"  // OS-assigned port
            : config_.inbox_endpoint;
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec_, inbox_schema_slot_size_);
        // Serialize full SchemaSpec JSON (with field names) for ROLE_INFO_REQ discovery.
        // parse_schema_json() on the receiver side needs {"fields":[{"name":...,"type":...}]}.
        nlohmann::json spec_json;
        spec_json["fields"] = nlohmann::json::array();
        for (const auto &f : inbox_spec_.fields)
        {
            nlohmann::json fj = {{"name", f.name}, {"type", f.type_str}};
            if (f.count > 1)  fj["count"]  = f.count;
            if (f.length > 0) fj["length"] = f.length;
            spec_json["fields"].push_back(fj);
        }
        if (inbox_spec_.packing != "aligned") spec_json["packing"] = inbox_spec_.packing;
        const std::string packing = config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;
        const int inbox_rcvhwm = (config_.inbox_overflow_policy == "block")
            ? 0
            : static_cast<int>(config_.inbox_buffer_depth);
        inbox_queue_ = hub::InboxQueue::bind_at(ep, std::move(zmq_fields),
                                                  packing,
                                                  inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[prod] Failed to start InboxQueue for channel '{}'", config_.channel);
            if (inbox_queue_) inbox_queue_.reset();
            return false;
        }
        opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        opts.inbox_schema_json = spec_json.dump();
        opts.inbox_packing     = packing;
    }

    if (config_.shm_enabled)
    {
        opts.shm_config.shared_secret        = config_.shm_secret;
        opts.shm_config.ring_buffer_capacity = config_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = config_.shm_consumer_sync_policy;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = core_.schema_fz_size;

        opts.shm_config.physical_page_size = hub::system_page_size();
        opts.shm_config.logical_unit_size  =
            (schema_slot_size_ == 0) ? 1 : schema_slot_size_;
    }

    if (!config_.broker.empty())
    {
        if (!out_messenger_.connect(config_.broker, config_.broker_pubkey,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[prod] broker connect failed ({}); aborting", config_.broker);
            return false;
        }
    }

    auto maybe_producer = hub::Producer::create(out_messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[prod] Failed to create producer for channel '{}'", config_.channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    // Initialize DataBlock metrics tracking for this run.
    if (auto *out_shm = out_producer_->shm(); out_shm != nullptr)
        out_shm->clear_metrics();

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
        return format_tools::bytes_to_hex(raw);
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

    // ── Wire peer-dead and hub-dead monitoring ─────────────────────────────────
    out_producer_->on_peer_dead([this]() {
        LOGGER_WARN("[prod] peer-dead: consumer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        stop_reason_.store(static_cast<int>(scripting::StopReason::PeerDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    out_messenger_.on_hub_dead([this]() {
        LOGGER_WARN("[prod] hub-dead: broker connection lost; triggering shutdown");
        stop_reason_.store(static_cast<int>(scripting::StopReason::HubDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // ── Create transport queue ─────────────────────────────────────────────────
    if (config_.transport == Transport::Shm)
    {
        auto *out_shm = out_producer_->shm();
        if (out_shm == nullptr)
        {
            LOGGER_ERROR("[prod] transport=shm but SHM unavailable for channel '{}'",
                         config_.channel);
            return false;
        }
        auto shm_q = hub::ShmQueue::from_producer_ref(
            *out_shm, schema_slot_size_, core_.schema_fz_size, config_.channel);
        queue_ = std::move(shm_q);
    }
    else // Transport::Zmq
    {
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        const auto zmq_policy = (config_.zmq_overflow_policy == "block")
                                     ? hub::OverflowPolicy::Block
                                     : hub::OverflowPolicy::Drop;
        queue_ = hub::ZmqQueue::push_to(
            config_.zmq_out_endpoint, std::move(zmq_fields), config_.zmq_packing,
            config_.zmq_out_bind, std::nullopt, 0, config_.zmq_buffer_depth, zmq_policy);
        if (!queue_)
        {
            LOGGER_ERROR("[prod] Failed to create ZmqQueue for endpoint '{}'",
                         config_.zmq_out_endpoint);
            return false;
        }
    }

    if (!queue_->start())
    {
        LOGGER_ERROR("[prod] Queue::start() failed for channel '{}'", config_.channel);
        queue_.reset();
        return false;
    }
    queue_->set_checksum_options(config_.update_checksum, core_.has_fz);

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
        api_.set_queue(queue_.get());

        if (void *fz = queue_->write_flexzone();
            fz != nullptr && queue_->flexzone_size() > 0)
        {
            const size_t fz_sz = queue_->flexzone_size();
            fz_mv_ = py::memoryview::from_memory(fz, static_cast<py::ssize_t>(fz_sz),
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
                        fz_sz / fz_type_.attr("itemsize").cast<size_t>();
                    fz_inst_ = np.attr("ndarray")(
                        py::make_tuple(static_cast<py::ssize_t>(items)), fz_type_, fz_mv_);
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

    // Startup coordination (HEP-0023): wait for required peer roles before on_init.
    for (const auto &wr : config_.wait_for_roles)
    {
        LOGGER_INFO("[prod] Startup: waiting for role '{}' (timeout {}ms)...",
                    wr.uid, wr.timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{wr.timeout_ms};
        static constexpr int kPollMs = 200;
        bool found = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                break;
            const int poll_ms = static_cast<int>(std::min<long long>(kPollMs, remaining));
            py::gil_scoped_release rel;
            if (out_messenger_.query_role_presence(wr.uid, poll_ms))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            LOGGER_ERROR("[prod] Startup wait failed: role '{}' not present after {}ms",
                         wr.uid, wr.timeout_ms);
            return false;
        }
        LOGGER_INFO("[prod] Startup: role '{}' found", wr.uid);
    }

    core_.running_threads.store(true);

    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    if (inbox_queue_)
        inbox_thread_ = std::thread([this] { run_inbox_thread_(); });

    call_on_init_common_();

    if (!core_.running_threads.load())
        return true;

    loop_thread_ = std::thread([this] { run_loop_(); });

    main_thread_release_.emplace();
    return true;
}

void ProducerScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming(); // unblock run_role_main_loop() immediately

    {
        py::gil_scoped_release release;
        // join() is unbounded: if a thread is stuck (e.g., SHM acquire hung or ZMQ
        // context not yet terminated), this call blocks indefinitely. In practice the
        // shutdown path sets shutdown_requested and closes the ZMQ context (ETERM),
        // which unblocks both threads within milliseconds. A timed join would require
        // a detach fallback, which risks use-after-free on the ProducerImpl data.
        // Revisit if watchdog-driven kill is ever needed.
        if (loop_thread_.joinable())  loop_thread_.join();
        if (ctrl_thread_.joinable())   ctrl_thread_.join();
        // Inbox thread: join BEFORE stopping inbox_queue_.
        // The inbox loop checks core_.running_threads (already false) at every
        // iteration and exits naturally after the next recv_one() timeout (~100ms).
        // Closing the ZMQ socket via inbox_queue_->stop() while inbox_thread_ is
        // inside zmq_recv() would violate ZMQ's single-thread-per-socket rule. CR-02.
        if (inbox_thread_.joinable()) inbox_thread_.join();
        if (inbox_queue_) inbox_queue_->stop();
        if (inbox_queue_) inbox_queue_.reset();
    }

    call_on_stop_common_();

    // Stop the transport queue before stopping the producer connection so any
    // in-flight send_thread_ work drains cleanly before the broker tears down.
    if (queue_)
    {
        queue_->stop();
        queue_.reset();
    }

    // Deregister hub-dead callback before closing the messenger so the worker
    // thread cannot invoke a callback pointing into this (now-stopping) object.
    out_messenger_.on_hub_dead(nullptr);

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }

    api_.set_flexzone_obj(nullptr);
    api_.set_producer(nullptr);
    api_.set_queue(nullptr);
    clear_role_pyobjects();
    clear_common_pyobjects_();

    LOGGER_INFO("[prod] Producer stopped.");
}

void ProducerScriptHost::cleanup_on_start_failure()
{
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }
    if (queue_)
    {
        queue_->stop();
        queue_.reset();
    }
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
}

void ProducerScriptHost::clear_role_pyobjects()
{
    api_.clear_inbox_cache();
    py_on_produce_ = py::none();
    slot_type_     = py::none();
    py_on_inbox_   = py::none();
    inbox_type_    = py::none();
}

void ProducerScriptHost::update_fz_checksum_after_init()
{
    if (queue_)
        queue_->sync_flexzone_checksum();
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
    auto [commit, is_err] = parse_on_produce_return(ret);
    if (is_err)
        api_.increment_script_errors();
    return commit;
}

// ============================================================================
// run_loop_ — transport-agnostic timer-driven production loop
//
// Uses the hub::QueueWriter interface for both SHM and ZMQ transports:
//   - ShmQueue (transport=shm): write_acquire blocks up to acquire_timeout;
//     write_commit() applies checksum if configured via set_checksum_options().
//   - ZmqQueue (transport=zmq): write_acquire honours zmq_overflow_policy (Drop or Block);
//     write_commit() enqueues to the internal send ring; send_thread_ delivers async.
// ============================================================================

void ProducerScriptHost::run_loop_()
{
    if (!queue_)
    {
        LOGGER_ERROR("[prod] run_loop_: transport queue not initialized — aborting");
        core_.running_threads.store(false);
        return;
    }

    const auto acquire_timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};

    // Deadline tracking: each iteration targets a start-to-start period of target_period_ms.
    // When target_period_ms == 0, free-run mode: no sleep, no deadline.
    // The first iteration fires immediately; subsequent iterations sleep to hit the deadline.
    auto next_deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds{config_.target_period_ms};

    const bool is_fixed_rate = (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto period        = std::chrono::milliseconds{config_.target_period_ms};

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        const auto iter_start = std::chrono::steady_clock::now();

        void *buf = queue_->write_acquire(acquire_timeout);

        // Drain control messages AFTER acquire so they are never lost on acquire
        // failure.  The script callback is always invoked — even without a write
        // slot — so that on_produce can process broadcast, channel_closing, and
        // other control events.
        auto msgs = core_.drain_messages();

        if (!buf)
        {
            // No write slot available.  Decide whether to call on_produce(None):
            //   - Messages pending → always call (control events are time-critical).
            //   - MaxRate          → always call (script expects every iteration).
            //   - FixedRate*       → call only when the deadline has been reached.
            const bool due = !is_fixed_rate || iter_start >= next_deadline;

            if (!msgs.empty() || due)
            {
                py::gil_scoped_acquire g;
                try
                {
                    py::object none_sv = py::none();
                    py::list   mlst    = build_messages_list_(msgs);
                    (void)call_on_produce_(none_sv, fz_inst_, mlst);
                }
                catch (py::error_already_set &e)
                {
                    api_.increment_script_errors();
                    LOGGER_ERROR("[prod] Python error in produce loop (no slot): {}",
                                 e.what());
                    if (config_.stop_on_script_error)
                        core_.running_threads.store(false);
                }

                // Advance deadline (same logic as normal path).
                if (is_fixed_rate)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_deadline)
                    {
                        if (config_.loop_timing ==
                            LoopTimingPolicy::FixedRateWithCompensation)
                            next_deadline += period;
                        else
                            next_deadline = now + period;
                    }
                }
            }
            api_.increment_drops();
            iteration_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const size_t buf_sz = queue_->item_size();
        std::memset(buf, 0, buf_sz);

        bool commit = false;
        {
            py::gil_scoped_acquire g;
            try
            {
                py::object out_sv = make_out_slot_view_(buf, buf_sz);
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
            queue_->write_commit();
            api_.increment_out_written();
        }
        else
        {
            queue_->write_discard();
            api_.increment_drops();
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        // Record work time (always), then sleep to hit target_period_ms (if FixedRate policy).
        {
            const auto now     = std::chrono::steady_clock::now();
            const auto work_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - iter_start).count());
            api_.set_last_cycle_work_us(work_us);

            if (is_fixed_rate)
            {
                if (now < next_deadline)
                {
                    std::this_thread::sleep_for(next_deadline - now);
                    next_deadline += period;
                }
                else
                {
                    // Overrun: start-to-start exceeded target_period_ms.
                    if (config_.loop_timing == LoopTimingPolicy::FixedRateWithCompensation) {
                        next_deadline += period;
                    } else { // FixedRate: reset from now — no catch-up
                        next_deadline = now + period;
                    }
                }
            }
        }
    }

    LOGGER_INFO("[prod] run_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={} g_shutdown={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                api_.critical_error(),
                core_.g_shutdown ? core_.g_shutdown->load() : false);
}

// ============================================================================
// run_ctrl_thread_ — polls producer peer socket, sends heartbeats
// ============================================================================

void ProducerScriptHost::run_ctrl_thread_()
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

// ============================================================================
// make_inbox_slot_view_ — read-only slot view for on_inbox callback
// ============================================================================

py::object ProducerScriptHost::make_inbox_slot_view_(const void* data, size_t size) const
{
    auto mv = py::memoryview::from_memory(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<void*>(data), static_cast<py::ssize_t>(size), /*readonly=*/true);
    if (inbox_spec_.exposure == scripting::SlotExposure::Ctypes)
        return inbox_type_.attr("from_buffer_copy")(mv);
    // NumpyArray or raw: return bytes view
    return mv;
}

// ============================================================================
// run_inbox_thread_ — receives inbox messages, dispatches on_inbox callback
// ============================================================================

void ProducerScriptHost::run_inbox_thread_()
{
    static constexpr auto kPollTimeout = std::chrono::milliseconds{100};

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        if (!inbox_queue_) break;

        const auto* item = inbox_queue_->recv_one(kPollTimeout);
        if (!item) continue;

        uint8_t ack_code = 0;
        {
            py::gil_scoped_acquire g;
            try
            {
                if (!py_on_inbox_.is_none())
                {
                    auto sv = make_inbox_slot_view_(item->data, inbox_schema_slot_size_);
                    py_on_inbox_(sv, py::str(item->sender_id), api_obj_);
                }
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("[prod] on_inbox raised: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3; // handler_error
                if (config_.stop_on_script_error)
                    core_.shutdown_requested.store(true, std::memory_order_release);
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[prod] on_inbox exception: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3;
            }
        }
        inbox_queue_->send_ack(ack_code);
    }

    LOGGER_INFO("[prod] run_inbox_thread_ exiting");
}

} // namespace pylabhub::producer
