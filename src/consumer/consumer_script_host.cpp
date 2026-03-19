/**
 * @file consumer_script_host.cpp
 * @brief ConsumerScriptHost — role-specific implementation.
 *
 * The common do_python_work() skeleton lives in PythonRoleHostBase.
 * This file provides the consumer-specific virtual hook overrides:
 *  - Demand-driven consumption loop
 *  - Single input channel with hub::Consumer
 *  - on_consume callback dispatch
 *  - Writable flexzone (zero-copy from_buffer — user-coordinated R/W) and message list without sender
 */
#include "consumer_script_host.hpp"

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
    python_venv_ = config_.python_venv;
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
    api_.set_role_dir(config_.role_dir);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
    api_.set_stop_reason(&core_.stop_reason_);
    api_.set_critical_error_ptr(&core_.critical_error_);
}

void ConsumerScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_consume_ = py::getattr(mod, "on_consume", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
    // on_inbox is optional — only extract when inbox is configured
    if (config_.has_inbox() && py::hasattr(mod, "on_inbox"))
        py_on_inbox_ = mod.attr("on_inbox");
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

    // HEP-0011 §3.1: script.type should be explicit. Warn when defaulted.
    if (!config_.script_type_explicit)
        LOGGER_WARN("[cons] Config 'script.type' absent — defaulting to '{}'. "
                    "Set it explicitly to suppress this warning.", config_.script_type);

    try
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back((std::filesystem::path(config_.hub_dir) / "schemas").string());

        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "cons", schema_dirs);
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "cons", schema_dirs);

        // Inbox schema (optional) — resolve here while schema_dirs is in scope
        if (config_.has_inbox())
            inbox_spec_ = resolve_schema(config_.inbox_schema_json, false, "cons", schema_dirs);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
        return false;
    }

    try
    {
        if (!build_schema_type_(slot_spec_, slot_type_, schema_slot_size_, "SlotFrame",
                                /*readonly=*/true))
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
    if (config_.has_inbox())
        print_slot_layout_(inbox_type_, inbox_spec_, inbox_schema_slot_size_,
                           "  Inbox: InboxFrame");
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
    // DataBlock-layer acquire pacing: active when loop_timing != MaxRate (period > 0).
    if (config_.target_period_ms > 0)
    {
        opts.loop_policy = hub::LoopPolicy::FixedRate;
        opts.period_ms   = std::chrono::milliseconds{static_cast<int>(config_.target_period_ms)};
    }
    // Transport declaration (Phase 7): explicit queue_type avoids TRANSPORT_MISMATCH false-positive.
    opts.queue_type = (config_.queue_type == QueueType::Zmq) ? "zmq" : "shm";
    // ZMQ data loop (HEP-CORE-0021): set zmq_schema so Consumer creates ZmqQueue PULL.
    if (config_.queue_type == QueueType::Zmq)
    {
        opts.zmq_schema       = scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        opts.zmq_buffer_depth = config_.zmq_buffer_depth;
    }
    opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;
    if (!config_.broker.empty())
    {
        if (!in_messenger_.connect(config_.broker, config_.broker_pubkey,
                                   config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[cons] broker connect failed ({}); aborting", config_.broker);
            return false;
        }
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", config_.channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Initialize DataBlock metrics tracking for this run.
    if (auto *in_shm = in_consumer_->shm(); in_shm != nullptr)
        in_shm->clear_metrics();

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

    // ZMQ data routing:
    // - Shm queue_type: ZMQ data frames arrive as side-channel messages (via on_zmq_data).
    // - Zmq queue_type: ZMQ data drives the main loop via queue()->read_acquire(); no callback.
    if (config_.queue_type == QueueType::Shm)
    {
        in_consumer_->on_zmq_data(
            [this](std::span<const std::byte> data)
            {
                LOGGER_DEBUG("[cons] zmq_data: data_message size={}", data.size());
                IncomingMessage msg;
                msg.data.assign(data.begin(), data.end());
                core_.enqueue_message(std::move(msg));
            });
    }

    // Wire producer control messages → IncomingMessage queue as event dicts.
    // NOTE: The data payload may contain arbitrary binary bytes. We hex-encode it
    // to avoid UnicodeDecodeError when json_to_py() converts to py::str.
    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
            LOGGER_INFO("[cons] ctrl_msg: producer_message type='{}' size={}",
                        type, data.size());
            IncomingMessage msg;
            msg.event = "producer_message";
            msg.details["type"] = std::string(type);
            msg.details["data"] = format_tools::bytes_to_hex(
                {reinterpret_cast<const char *>(data.data()), data.size()});
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

    // ── Wire peer-dead and hub-dead monitoring ─────────────────────────────────
    in_consumer_->on_peer_dead([this]() {
        LOGGER_WARN("[cons] peer-dead: producer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        core_.stop_reason_.store(static_cast<int>(scripting::StopReason::PeerDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    in_messenger_.on_hub_dead([this]() {
        LOGGER_WARN("[cons] hub-dead: broker connection lost; triggering shutdown");
        core_.stop_reason_.store(static_cast<int>(scripting::StopReason::HubDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Wire API and input flexzone (read-only view).
    try
    {
        // Ensure the embedded pybind11 module is imported so the ConsumerAPI type
        // is registered before py::cast.
        py::module_::import("pylabhub_consumer");

        api_obj_ = py::cast(&api_, py::return_value_policy::reference);
        api_.set_consumer(&*in_consumer_);
        api_.set_messenger(&in_messenger_);

        // Create the transport QueueReader used by the unified run_loop_().
        // SHM: create an owned ShmQueue wrapping the DataBlockConsumer.
        // ZMQ: borrow the ZmqQueue owned by Consumer (non-owning pointer).
        if (config_.queue_type == QueueType::Shm)
        {
            auto *in_shm = in_consumer_->shm();
            if (in_shm == nullptr)
            {
                LOGGER_ERROR("[cons] queue_type='shm' but SHM unavailable for channel '{}'",
                             config_.channel);
                return false;
            }
            shm_queue_    = hub::ShmQueue::from_consumer_ref(
                *in_shm, schema_slot_size_, core_.schema_fz_size, config_.channel);
            queue_reader_ = shm_queue_.get();
        }
        else // QueueType::Zmq
        {
            queue_reader_ = in_consumer_->queue_reader();
            if (queue_reader_ == nullptr)
            {
                LOGGER_ERROR("[cons] queue_type='zmq' but broker reported SHM transport for "
                             "channel '{}'; check that the producer uses ZMQ transport",
                             config_.channel);
                return false;
            }
        }

        if (queue_reader_)
            queue_reader_->set_verify_checksum(config_.verify_checksum, core_.has_fz);
        api_.set_reader(queue_reader_);

        // Inbox facility (optional).
        if (config_.has_inbox())
        {
            const std::string ep = config_.inbox_endpoint.empty()
                ? "tcp://127.0.0.1:0"
                : config_.inbox_endpoint;
            const std::string packing = inbox_spec_.packing.empty()
                ? config_.zmq_packing
                : inbox_spec_.packing;

            auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec_,
                                                                    inbox_schema_slot_size_);
            // Serialize inbox schema + packing for future advertisement (CONSUMER_REG_REQ).
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

            const int inbox_rcvhwm = (config_.inbox_overflow_policy == "block")
                ? 0
                : static_cast<int>(config_.inbox_buffer_depth);
            inbox_queue_ = hub::InboxQueue::bind_at(ep, std::move(zmq_fields),
                                                    packing,
                                                    inbox_rcvhwm);
            if (!inbox_queue_ || !inbox_queue_->start())
            {
                LOGGER_ERROR("[cons] Failed to start InboxQueue at '{}'", ep);
                if (inbox_queue_) inbox_queue_.reset();
            }
            else
            {
                LOGGER_INFO("[cons] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());
            }
        }

        // Flexzone is user-coordinated shared memory: expose writable zero-copy
        // view so consumer scripts can both read and write it freely.
        // read_flexzone() returns nullptr for transports that have no flexzone (e.g. ZMQ).
        if (const void *fz_ro = queue_reader_->read_flexzone();
            fz_ro != nullptr && queue_reader_->flexzone_size() > 0)
        {
            const size_t fz_sz = queue_reader_->flexzone_size();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            fz_mv_ = py::memoryview::from_memory(
                const_cast<void *>(fz_ro), static_cast<py::ssize_t>(fz_sz),
                /*readonly=*/false);

            if (core_.fz_spec.exposure == scripting::SlotExposure::Ctypes)
            {
                // from_buffer() creates a zero-copy live view — no refresh needed.
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
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[cons] Failed to build flexzone view: {}", e.what());
        return false;
    }

    LOGGER_INFO("[cons] Consumer started on channel '{}'", config_.channel);

    // Startup coordination (HEP-0023): wait for required peer roles before on_init.
    for (const auto &wr : config_.wait_for_roles)
    {
        LOGGER_INFO("[cons] Startup: waiting for role '{}' (timeout {}ms)...",
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
            if (in_messenger_.query_role_presence(wr.uid, poll_ms))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            LOGGER_ERROR("[cons] Startup wait failed: role '{}' not present after {}ms",
                         wr.uid, wr.timeout_ms);
            return false;
        }
        LOGGER_INFO("[cons] Startup: role '{}' found", wr.uid);
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

void ConsumerScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming(); // unblock run_role_main_loop() immediately

    {
        py::gil_scoped_release release;
        // No timed join: shutdown_requested + ETERM unblocks threads within ms.
        // A timed join would require a detach fallback, which risks use-after-free.
        if (loop_thread_.joinable())  loop_thread_.join();
        if (ctrl_thread_.joinable())  ctrl_thread_.join();
        // Inbox thread: join BEFORE stopping inbox_queue_.
        // The inbox loop checks core_.running_threads (already false) at every
        // iteration and exits naturally after the next recv_one() timeout (~100ms).
        // Closing the ZMQ socket via inbox_queue_->stop() while inbox_thread_ is
        // inside zmq_recv() would violate ZMQ's single-thread-per-socket rule.
        if (inbox_thread_.joinable()) inbox_thread_.join();
        if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }
    }

    call_on_stop_common_();

    // Null out the QueueReader pointer before destroying the SHM queue or consumer.
    api_.set_reader(nullptr);
    queue_reader_ = nullptr;
    shm_queue_.reset();

    // Deregister hub-dead callback before closing the messenger.
    in_messenger_.on_hub_dead(nullptr);

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
    if (inbox_thread_.joinable()) inbox_thread_.join();
    if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

void ConsumerScriptHost::clear_role_pyobjects()
{
    // LR-07: clear_inbox_cache() must be called before clearing py objects — it
    // iterates inbox_cache_ (holding py::object InboxHandle values) and calls
    // InboxHandle::close() on each, which requires the GIL (held here).
    api_.clear_inbox_cache();
    py_on_consume_ = py::none();
    slot_type_     = py::none();
    inbox_type_    = py::none();
    py_on_inbox_   = py::none();
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
// Slot view builder
// ============================================================================

py::object ConsumerScriptHost::make_in_slot_view_(const void *data, size_t size) const
{
    return scripting::make_slot_view(slot_spec_, slot_type_, data, size, /*is_read_side=*/true);
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
// run_loop_ — unified transport-agnostic consumption loop
//
// Uses hub::QueueReader (set as queue_reader_ in start_role) for both transports:
//   SHM: ShmQueue::from_consumer_ref — blocks on DataBlock acquire; checksum optional.
//   ZMQ: ZmqQueue (Consumer-owned) — blocks on recv ring; no flexzone.
//
// The flexzone view (fz_inst_) is pre-built in start_role() and remains valid
// for the loop lifetime.  ZMQ transport leaves fz_inst_ as py::none().
// Deadline tracking applies when loop_timing != MaxRate (FixedRate or FixedRateWithCompensation).
// ============================================================================

void ConsumerScriptHost::run_loop_()
{
    if (!queue_reader_)
    {
        LOGGER_ERROR("[cons] run_loop_: QueueReader not initialized — aborting");
        core_.running_threads.store(false);
        return;
    }

    const auto timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};
    const size_t item_sz = queue_reader_->item_size();

    const bool is_fixed_rate = (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto period        = std::chrono::milliseconds{static_cast<int>(config_.target_period_ms)};

    // Deadline tracking (applies when loop_timing != MaxRate).
    auto next_deadline = std::chrono::steady_clock::now() + period;

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        const auto iter_start = std::chrono::steady_clock::now();

        // 1. Block until a slot arrives (or timeout).
        const void *data = queue_reader_->read_acquire(timeout);
        api_.update_last_seq(queue_reader_->last_seq());

        // 2. Drain any queued ZMQ ctrl messages (no GIL needed).
        auto msgs = core_.drain_messages();

        if (!data)
        {
            // No data.  Decide whether to call on_consume(None):
            //   - Messages pending → always call (control events are time-critical).
            //   - MaxRate          → always call (script expects every iteration).
            //   - FixedRate*       → call only when the deadline has been reached.
            const bool due = !is_fixed_rate || iter_start >= next_deadline;

            if (!msgs.empty() || due)
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
            iteration_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        api_.increment_in_received();

        if (!core_.running_threads.load() || api_.critical_error())
        {
            queue_reader_->read_release();
            break;
        }

        {
            py::gil_scoped_acquire g;
            try
            {
                py::object in_sv = make_in_slot_view_(data, item_sz);
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

        queue_reader_->read_release();

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        // Deadline tracking: sleep to hit target_period_ms (when FixedRate).
        {
            const auto now     = std::chrono::steady_clock::now();
            const auto work_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - iter_start).count());
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
                    if (config_.loop_timing == LoopTimingPolicy::FixedRateWithCompensation) {
                        next_deadline += period;
                    } else { // FixedRate: reset from now — no catch-up
                        next_deadline = now + period;
                    }
                }
            }
        }
    }

    LOGGER_INFO("[cons] run_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={} g_shutdown={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                api_.critical_error(),
                core_.g_shutdown ? core_.g_shutdown->load() : false);
}

// ============================================================================
// make_inbox_slot_view_ — read-only slot view for on_inbox callback
// ============================================================================

py::object ConsumerScriptHost::make_inbox_slot_view_(const void *data, size_t size) const
{
    auto mv = py::memoryview::from_memory(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<void *>(data), static_cast<py::ssize_t>(size), /*readonly=*/true);
    if (inbox_spec_.exposure == scripting::SlotExposure::Ctypes)
        return inbox_type_.attr("from_buffer_copy")(mv);
    // NumpyArray or raw: return bytes view
    return mv;
}

// ============================================================================
// run_inbox_thread_ — receives inbox messages, dispatches on_inbox callback
// ============================================================================

void ConsumerScriptHost::run_inbox_thread_()
{
    static constexpr auto kPollTimeout = std::chrono::milliseconds{100};
    LOGGER_INFO("[cons] run_inbox_thread_ started");

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        if (!inbox_queue_) break;

        const auto *item = inbox_queue_->recv_one(kPollTimeout);
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
                LOGGER_ERROR("[cons] on_inbox raised: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3; // handler_error
                if (config_.stop_on_script_error)
                    core_.running_threads.store(false);
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[cons] on_inbox exception: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3;
            }
        }
        inbox_queue_->send_ack(ack_code);
    }

    LOGGER_INFO("[cons] run_inbox_thread_ exiting");
}

// ============================================================================
// run_ctrl_thread_ — polls consumer ZMQ sockets, sends heartbeats
// ============================================================================

void ConsumerScriptHost::run_ctrl_thread_()
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
