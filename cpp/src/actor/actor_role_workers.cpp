/**
 * @file actor_role_workers.cpp
 * @brief ProducerRoleWorker and ConsumerRoleWorker implementations.
 *
 * Each role worker manages the two-thread model (loop_thread_ + zmq_thread_)
 * for a single actor role. See actor_host.hpp for the class declarations and
 * actor_worker_helpers.hpp for the shared internal helper functions.
 */
#include "actor_worker_helpers.hpp"

namespace pylabhub::actor
{

// ============================================================================
// ProducerRoleWorker
// ============================================================================

ProducerRoleWorker::ProducerRoleWorker(const std::string     &role_name,
                                        const RoleConfig      &role_cfg,
                                        const std::string     &actor_uid,
                                        const ActorAuthConfig &auth,
                                        std::atomic<bool>     &shutdown,
                                        const py::module_     &script_module)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , auth_(auth)
    , shutdown_(shutdown)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
    // ── PylabhubEnv: fields available at construction time ────────────────────
    api_.set_channel(role_cfg_.channel);
    api_.set_broker(role_cfg_.broker);
    api_.set_kind_str("producer");
    // actor_name, log_level, script_dir wired by ActorHost::start() before start().

    // ── Look up callbacks from module (under GIL) ─────────────────────────────
    {
        py::gil_scoped_acquire g;
        py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
        py_on_init_      = py::getattr(script_module, "on_init",      py::none());
        py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
    }
}

ProducerRoleWorker::~ProducerRoleWorker()
{
    stop();
}

bool ProducerRoleWorker::build_slot_types_()
{
    return build_schema_types(role_cfg_,
                               slot_spec_, fz_spec_,
                               slot_type_, fz_type_,
                               schema_slot_size_, schema_fz_size_,
                               has_fz_);
}

void ProducerRoleWorker::print_layout_() const
{
    py::gil_scoped_acquire g;
    print_layout(slot_spec_, slot_type_, schema_slot_size_,
                  fz_spec_,  fz_type_,  schema_fz_size_,
                  role_name_ + " [producer]");
}

py::object ProducerRoleWorker::make_slot_view_(void *data, size_t size) const
{
    auto mv = py::memoryview::from_memory(data, static_cast<ssize_t>(size),
                                           /*readonly=*/false);
    if (!slot_spec_.has_schema)
        return py::bytearray(reinterpret_cast<const char *>(data), size);
    if (slot_spec_.exposure == SlotExposure::Ctypes)
        return slot_type_.attr("from_buffer")(mv);
    // numpy_array mode
    py::module_ np = py::module_::import("numpy");
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape) shape.append(d);
        return np.attr("ndarray")(shape, slot_type_, mv);
    }
    size_t itemsize = slot_type_.attr("itemsize").cast<size_t>();
    size_t count    = (itemsize > 0) ? (size / itemsize) : 0;
    return np.attr("ndarray")(py::make_tuple(static_cast<ssize_t>(count)),
                               slot_type_, mv);
}

std::vector<ProducerRoleWorker::IncomingMessage>
ProducerRoleWorker::drain_incoming_queue_()
{
    std::vector<IncomingMessage> msgs;
    std::unique_lock<std::mutex> lock(incoming_mu_);
    if (!incoming_queue_.empty())
    {
        msgs.reserve(incoming_queue_.size());
        for (auto &m : incoming_queue_)
            msgs.push_back(std::move(m));
        incoming_queue_.clear();
    }
    return msgs;
}

py::list ProducerRoleWorker::build_messages_list_(
    std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        lst.append(py::make_tuple(
            m.sender,
            py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
    }
    return lst;
}

bool ProducerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!is_callable(py_on_iteration_))
    {
        LOGGER_WARN("[actor/{}] module has no 'on_iteration' — role not started",
                    role_name_);
        return false;
    }

    if (!build_slot_types_())
        return false;

    // ── Create Producer ───────────────────────────────────────────────────────
    hub::ProducerOptions opts;
    opts.channel_name = role_cfg_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = role_cfg_.has_shm;
    opts.schema_hash  = compute_schema_hash(slot_spec_, fz_spec_);
    opts.actor_name   = api_.actor_name();
    opts.actor_uid    = api_.uid();

    if (role_cfg_.has_shm)
    {
        opts.shm_config.shared_secret        = role_cfg_.shm_secret;
        opts.shm_config.ring_buffer_capacity = role_cfg_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Latest_only;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = schema_fz_size_;

        if (schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4K))
        {
            // Pass the raw schema size; effective_logical_unit_size() rounds it up to
            // the nearest 64-byte cache-line boundary automatically.
            // physical_page_size stays Size4K — it controls OS allocation granularity.
            // Use 1 (not 0) when schema_slot_size_ is zero so that effective_logical_unit_size()
            // does not fall back to the "use physical_page_size" default (0 is the sentinel).
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4K;
            opts.shm_config.logical_unit_size  = (schema_slot_size_ == 0) ? 1 : schema_slot_size_;
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

    // Connect to the role's configured broker endpoint.
    if (!role_cfg_.broker.empty())
    {
        if (!messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey,
                                auth_.client_pubkey, auth_.client_seckey))
            LOGGER_WARN("[actor] Role '{}': broker connect failed ({}); running degraded",
                        role_name_, role_cfg_.broker);
    }

    auto maybe_producer = hub::Producer::create(messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to create producer for channel '{}'",
                     role_name_, role_cfg_.channel);
        return false;
    }
    producer_ = std::move(maybe_producer);

    // ── Wire LoopPolicy for acquire-side overrun detection (HEP-CORE-0008) ────
    if (auto *shm = producer_->shm(); shm != nullptr)
    {
        shm->set_loop_policy(role_cfg_.loop_policy, role_cfg_.period_ms);
        shm->clear_metrics();
    }

    // ── Wire on_consumer_message → incoming_queue_ ───────────────────────────
    // GIL-safe: peer_thread pushes to queue; loop_thread drains.
    producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data)
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            if (incoming_queue_.size() >= kMaxIncomingQueue)
            {
                LOGGER_WARN("[actor/{}] Incoming message queue full ({}) — dropping",
                            role_name_, kMaxIncomingQueue);
                return;
            }
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            incoming_queue_.push_back(std::move(msg));
            incoming_cv_.notify_one();
        });

    if (!producer_->start_embedded())
    {
        LOGGER_ERROR("[actor/{}] producer->start_embedded() failed", role_name_);
        // Cleanup handled by RAII: producer_ optional<Producer> is destroyed on scope
        // exit, which calls Producer's destructor and releases all ZMQ resources.
        return false;
    }

    // ── Phase 3: hand heartbeat responsibility to zmq_thread_ ────────────────
    // Suppress the Messenger periodic heartbeat; zmq_thread_ will send
    // application-level HEARTBEAT_REQ only when iteration_count_ advances.
    // Enqueue one immediate heartbeat to keep the channel alive during on_init
    // (in case on_init takes longer than the broker timeout).
    if (!role_cfg_.channel.empty())
    {
        messenger_.suppress_periodic_heartbeat(role_cfg_.channel);
        messenger_.enqueue_heartbeat(role_cfg_.channel);
    }

    // ── Build persistent flexzone view (writable, producer owns it) ───────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_producer(&*producer_);

            if (has_fz_)
            {
                auto *shm = producer_->shm();
                if (shm != nullptr)
                {
                    auto fz_span = shm->flexible_zone_span();
                    fz_mv_  = py::memoryview::from_memory(
                        fz_span.data(),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/false);  // Producer owns flexzone — writable

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape) shape.append(d);
                            fz_inst_ = np.attr("ndarray")(shape, fz_type_, fz_mv_);
                        }
                        else
                        {
                            size_t items = fz_span.size_bytes() /
                                           fz_type_.attr("itemsize").cast<size_t>();
                            fz_inst_ = np.attr("ndarray")(
                                py::make_tuple(static_cast<ssize_t>(items)),
                                fz_type_, fz_mv_);
                        }
                    }
                }
            }
            if (fz_inst_.is_none())
                fz_inst_ = py::none();

            // Wire flexzone pointer into api so api.flexzone() works.
            api_.set_flexzone_obj(&fz_inst_);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor/{}] Failed to build flexzone view: {}", role_name_, e.what());
            return false;
        }
    }

    LOGGER_INFO("[actor/{}] producer started on channel '{}'",
                role_name_, role_cfg_.channel);

    api_.reset_all_role_run_metrics();
    running_.store(true);

    // Launch zmq_thread_ BEFORE on_init so that any ZMQ sends (e.g. api.broadcast)
    // inside on_init are dispatched immediately. Mirrors old behaviour where peer_thread
    // was already running before on_init was called.
    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init();

    // Check if on_init cleared running_ (e.g. via api.stop() in on_init).
    if (!running_.load())
        return true;  // graceful early exit — not an error

    if (role_cfg_.loop_trigger == RoleConfig::LoopTrigger::Shm)
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_messenger(); });

    return true;
}

void ProducerRoleWorker::stop()
{
    // Guard: return only if fully idle (running=false AND no threads to join).
    // If running_=false but a thread is still joinable (e.g. api.stop() was called from
    // on_init, which exits before loop_thread_/zmq_thread_ are launched), we must still
    // join to avoid std::terminate in std::thread's destructor.
    if (!running_.load() && !loop_thread_.joinable() && !zmq_thread_.joinable())
        return;
    running_.store(false);

    // Wake the incoming_cv_ so the loop thread exits wait_for.
    incoming_cv_.notify_all();

    if (loop_thread_.joinable())
        loop_thread_.join();

    if (zmq_thread_.joinable())
        zmq_thread_.join();

    call_on_stop();

    if (producer_.has_value())
    {
        producer_->stop();
        producer_->close();
        producer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_flexzone_obj(nullptr);
        api_.set_producer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("[actor/{}] producer stopped", role_name_);
}

void ProducerRoleWorker::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_init error: {}", role_name_, e.what());
    }
    // Update flexzone checksum after on_init writes.
    if (has_fz_)
    {
        if (auto *shm = producer_->shm())
            (void)shm->update_checksum_flexible_zone();
    }
}

void ProducerRoleWorker::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !producer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

bool ProducerRoleWorker::call_on_iteration_(py::object &slot,
                                              py::object &fz,
                                              py::list   &msgs)
{
    py::object ret;
    try
    {
        ret = py_on_iteration_(slot, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_iteration error: {}", role_name_, e.what());
        if (role_cfg_.validation.stop_on_python_error)
            running_.store(false);
        return false;
    }
    return parse_on_iteration_return(ret);
}

bool ProducerRoleWorker::step_write_deadline_(
    std::chrono::steady_clock::time_point &next_deadline)
{
    if (role_cfg_.interval_ms > 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_deadline)
        {
            std::this_thread::sleep_for(next_deadline - now);
            next_deadline += std::chrono::milliseconds(role_cfg_.interval_ms);
        }
        else
        {
            // Overrun: deadline already past — no sleep needed.
            api_.increment_loop_overruns();
            if (role_cfg_.loop_timing == RoleConfig::LoopTimingPolicy::Compensating)
                next_deadline += std::chrono::milliseconds(role_cfg_.interval_ms);
            else // FixedPace
                next_deadline = now + std::chrono::milliseconds(role_cfg_.interval_ms);
        }
        if (!running_.load() || shutdown_.load())
            return false;
    }
    // interval_ms == 0: run at full throughput — no sleep, no overrun tracking.
    return true;
}

void ProducerRoleWorker::run_loop_shm()
{
    auto *shm = producer_->shm();
    if (shm == nullptr)
    {
        LOGGER_ERROR("[actor/{}] SHM unavailable despite has_shm=true", role_name_);
        running_.store(false);
        return;
    }

    const auto &val = role_cfg_.validation;

    // Deadline initialised to now so the first write fires immediately.
    auto next_deadline = std::chrono::steady_clock::now();

    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        if (!step_write_deadline_(next_deadline))
            break;

        // ── Acquire SHM write slot ────────────────────────────────────────────
        // Timeout budget = interval_ms (scheduled mode) or kShmMaxRateMs (max-rate).
        // Rationale: after step_write_deadline_ returns, next_deadline is interval_ms
        // away. Using interval_ms as acquire budget means a slot miss is treated as an
        // overrun on the next step_write_deadline_ call — no false overruns.
        static constexpr int kShmMaxRateMs = 5;
        const int acquire_ms = role_cfg_.interval_ms > 0 ? role_cfg_.interval_ms
                                                          : kShmMaxRateMs;
        const auto work_start = std::chrono::steady_clock::now();
        auto slot_handle = shm->acquire_write_slot(acquire_ms);
        if (!slot_handle)
            continue;
        if (!running_.load() || shutdown_.load())
            break;

        // ── Drain incoming queue (no GIL) ─────────────────────────────────────
        auto msgs = drain_incoming_queue_();

        auto   span        = slot_handle->buffer_span();
        size_t write_bytes = std::min(span.size_bytes(), schema_slot_size_);
        std::memset(span.data(), 0, write_bytes);

        bool commit = false;
        {
            py::gil_scoped_acquire g;
            py::object slot = make_slot_view_(span.data(), write_bytes);
            py::list   mlst = build_messages_list_(msgs);
            commit = call_on_iteration_(slot, fz_inst_, mlst);
        }

        if (commit)
        {
            (void)slot_handle->commit(write_bytes);
            if (val.slot_checksum != ValidationPolicy::Checksum::None)
                (void)slot_handle->update_checksum_slot();
            if (has_fz_ && val.flexzone_checksum != ValidationPolicy::Checksum::None)
                (void)slot_handle->update_checksum_flexible_zone();
        }
        (void)shm->release_write_slot(*slot_handle);
        if (commit)
        {
            api_.set_last_cycle_work_us(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - work_start).count()));
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ProducerRoleWorker::run_loop_messenger()
{
    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        // Wait for incoming messages or timeout.
        std::vector<IncomingMessage> msgs;
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            incoming_cv_.wait_for(
                lock,
                std::chrono::milliseconds(role_cfg_.messenger_poll_ms),
                [this] {
                    return !incoming_queue_.empty() || !running_.load() || shutdown_.load();
                });
            if (!incoming_queue_.empty())
            {
                msgs.reserve(incoming_queue_.size());
                for (auto &m : incoming_queue_)
                    msgs.push_back(std::move(m));
                incoming_queue_.clear();
            }
        }

        if (!running_.load() || shutdown_.load())
            break;

        {
            py::gil_scoped_acquire g;
            py::object slot = py::none();
            py::list   mlst = build_messages_list_(msgs);
            call_on_iteration_(slot, fz_inst_, mlst);
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ProducerRoleWorker::run_zmq_thread_()
{
    void *peer_sock = producer_->peer_ctrl_socket_handle();
    if (peer_sock == nullptr)
        return;  // no broker/peer connection — nothing to poll

    zmq_pollitem_t items[1] = {{peer_sock, 0, ZMQ_POLLIN, 0}};
    uint64_t last_iter{0};

    // Phase 3: application-level heartbeat throttle.
    // Compute heartbeat interval from config:
    //   heartbeat_interval_ms > 0 → use it directly
    //   heartbeat_interval_ms = 0, interval_ms > 0 → 10 × interval_ms
    //   heartbeat_interval_ms = 0, interval_ms = 0 (max-rate) → 2000 ms
    const auto hb_interval = [&]() -> std::chrono::milliseconds
    {
        if (role_cfg_.heartbeat_interval_ms > 0)
            return std::chrono::milliseconds{role_cfg_.heartbeat_interval_ms};
        if (role_cfg_.interval_ms > 0)
            return std::chrono::milliseconds{role_cfg_.interval_ms * 10};
        return std::chrono::milliseconds{2000};
    }();
    // Initialise to (now - hb_interval) so the first iteration advance fires immediately.
    auto last_heartbeat = std::chrono::steady_clock::now() - hb_interval;

    while (running_.load(std::memory_order_relaxed) && !shutdown_.load(std::memory_order_relaxed))
    {
        const int rc = zmq_poll(items, 1, role_cfg_.messenger_poll_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            LOGGER_WARN("[actor/{}/zmq_thread] zmq_poll error: {}", role_name_, zmq_strerror(errno));
            break;
        }
        if (rc > 0 && (items[0].revents & ZMQ_POLLIN))
            producer_->handle_peer_events_nowait();

        // Send HEARTBEAT_REQ when iteration_count_ has advanced AND the throttle
        // window has elapsed.  Application-level liveness: a stalled loop (GIL
        // deadlock, SHM full) stops heartbeats even if TCP is alive.
        const uint64_t cur = iteration_count_.load(std::memory_order_relaxed);
        if (cur != last_iter)
        {
            last_iter = cur;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat >= hb_interval)
            {
                messenger_.enqueue_heartbeat(role_cfg_.channel);
                last_heartbeat = now;
            }
        }
    }
}


// ============================================================================
// ConsumerRoleWorker
// ============================================================================

ConsumerRoleWorker::ConsumerRoleWorker(const std::string     &role_name,
                                        const RoleConfig      &role_cfg,
                                        const std::string     &actor_uid,
                                        const ActorAuthConfig &auth,
                                        std::atomic<bool>     &shutdown,
                                        const py::module_     &script_module)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , auth_(auth)
    , shutdown_(shutdown)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
    // ── PylabhubEnv: fields available at construction time ────────────────────
    api_.set_channel(role_cfg_.channel);
    api_.set_broker(role_cfg_.broker);
    api_.set_kind_str("consumer");
    // actor_name, log_level, script_dir wired by ActorHost::start() before start().

    // ── Look up callbacks from module (under GIL) ─────────────────────────────
    {
        py::gil_scoped_acquire g;
        py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
        py_on_init_      = py::getattr(script_module, "on_init",      py::none());
        py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
    }
}

ConsumerRoleWorker::~ConsumerRoleWorker()
{
    stop();
}

bool ConsumerRoleWorker::build_slot_types_()
{
    return build_schema_types(role_cfg_,
                               slot_spec_, fz_spec_,
                               slot_type_, fz_type_,
                               schema_slot_size_, schema_fz_size_,
                               has_fz_);
}

void ConsumerRoleWorker::print_layout_() const
{
    py::gil_scoped_acquire g;
    print_layout(slot_spec_, slot_type_, schema_slot_size_,
                  fz_spec_,  fz_type_,  schema_fz_size_,
                  role_name_ + " [consumer]");
}

py::object ConsumerRoleWorker::make_slot_view_readonly_(const void *data,
                                                          size_t      size) const
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto mv = py::memoryview::from_memory(const_cast<void *>(data),
                                           static_cast<ssize_t>(size),
                                           /*readonly=*/true);
    if (!slot_spec_.has_schema)
        return py::bytes(reinterpret_cast<const char *>(data), size);

    if (slot_spec_.exposure == SlotExposure::Ctypes)
        // from_buffer_copy() accepts read-only memoryviews; from_buffer() requires writable
        return slot_type_.attr("from_buffer_copy")(mv);

    py::module_ np  = py::module_::import("numpy");
    py::object  arr = np.attr("frombuffer")(mv, slot_type_);
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape) shape.append(d);
        arr = arr.attr("reshape")(shape);
    }
    return arr;
}

std::vector<ConsumerRoleWorker::IncomingMessage>
ConsumerRoleWorker::drain_incoming_queue_()
{
    std::vector<IncomingMessage> msgs;
    std::unique_lock<std::mutex> lock(incoming_mu_);
    if (!incoming_queue_.empty())
    {
        msgs.reserve(incoming_queue_.size());
        for (auto &m : incoming_queue_)
            msgs.push_back(std::move(m));
        incoming_queue_.clear();
    }
    return msgs;
}

py::list ConsumerRoleWorker::build_messages_list_(
    std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        lst.append(py::make_tuple(
            m.sender,
            py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
    }
    return lst;
}

bool ConsumerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!is_callable(py_on_iteration_))
    {
        LOGGER_WARN("[actor/{}] module has no 'on_iteration' — role not started",
                    role_name_);
        return false;
    }

    if (!build_slot_types_())
        return false;

    hub::ConsumerOptions opts;
    opts.channel_name         = role_cfg_.channel;
    opts.shm_shared_secret    = role_cfg_.has_shm ? role_cfg_.shm_secret : 0U;
    opts.expected_schema_hash = compute_schema_hash(slot_spec_, fz_spec_);
    opts.consumer_uid         = api_.uid();
    opts.consumer_name        = api_.actor_name();

    if (!role_cfg_.broker.empty())
    {
        if (!messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey,
                                auth_.client_pubkey, auth_.client_seckey))
            LOGGER_WARN("[actor] Role '{}': broker connect failed ({}); running degraded",
                        role_name_, role_cfg_.broker);
    }

    auto maybe_consumer = hub::Consumer::connect(messenger_, opts);
    // connect() returns empty optional on failure (broker reject / timeout).
    // Move into consumer_ only after confirming has_value() to avoid UB.
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to connect consumer to channel '{}'",
                     role_name_, role_cfg_.channel);
        return false;
    }
    consumer_ = std::move(maybe_consumer);

    // ── Wire LoopPolicy for acquire-side overrun detection (HEP-CORE-0008) ────
    if (auto *shm = consumer_->shm(); shm != nullptr)
    {
        shm->set_loop_policy(role_cfg_.loop_policy, role_cfg_.period_ms);
        shm->clear_metrics();
    }

    // ── Wire on_zmq_data → incoming_queue_ ───────────────────────────────────
    // GIL-safe: data_thread pushes to queue; loop_thread drains.
    consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            if (incoming_queue_.size() >= kMaxIncomingQueue)
            {
                LOGGER_WARN("[actor/{}] Incoming message queue full ({}) — dropping",
                            role_name_, kMaxIncomingQueue);
                return;
            }
            IncomingMessage msg;
            msg.sender = {};  // ZMQ broadcast has no sender identity
            msg.data.assign(data.begin(), data.end());
            incoming_queue_.push_back(std::move(msg));
            incoming_cv_.notify_one();
        });

    if (!consumer_->start_embedded())
    {
        LOGGER_ERROR("[actor/{}] consumer->start_embedded() failed", role_name_);
        return false;
    }

    // ── Build API and persistent flexzone view ────────────────────────────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_consumer(&*consumer_);

            if (has_fz_)
            {
                auto *shm = consumer_->shm();
                if (shm != nullptr)
                {
                    const auto fz_span = shm->flexible_zone_span();

                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    fz_mv_ = py::memoryview::from_memory(
                        const_cast<std::byte *>(fz_span.data()),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/true);

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        // from_buffer_copy() accepts read-only memoryviews; from_buffer() requires writable
                        fz_inst_ = fz_type_.attr("from_buffer_copy")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        py::object  arr = np.attr("frombuffer")(fz_mv_, fz_type_);
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape) shape.append(d);
                            arr = arr.attr("reshape")(shape);
                        }
                        fz_inst_ = arr;
                    }

                    if (role_cfg_.validation.flexzone_checksum ==
                        ValidationPolicy::Checksum::Enforce)
                    {
                        const bool fz_ok = shm->verify_checksum_flexible_zone();
                        if (!fz_ok)
                            LOGGER_WARN("[actor/{}] Initial flexzone checksum failed",
                                        role_name_);
                    }
                }
            }
            if (fz_inst_.is_none())
                fz_inst_ = py::none();

            // Wire flexzone pointer into api so api.flexzone() works.
            api_.set_flexzone_obj(&fz_inst_);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor/{}] Failed to build consumer flexzone view: {}",
                         role_name_, e.what());
            return false;
        }
    }

    LOGGER_INFO("[actor/{}] consumer connected to channel '{}'",
                role_name_, role_cfg_.channel);

    api_.reset_all_role_run_metrics();
    running_.store(true);

    // Launch zmq_thread_ BEFORE on_init so that ctrl events (e.g. channel-closing
    // notifications) and queued ctrl sends are processed immediately. Mirrors old
    // behaviour where ctrl_thread was already running before on_init was called.
    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init();

    // Check if on_init cleared running_ via api.stop().
    if (!running_.load())
        return true;

    if (role_cfg_.loop_trigger == RoleConfig::LoopTrigger::Shm &&
        consumer_->has_shm())
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_messenger(); });

    return true;
}

void ConsumerRoleWorker::stop()
{
    // Guard: return only if fully idle (running=false AND no threads to join).
    // Covers the on_init api.stop() early-exit case — zmq_thread_ may be joinable
    // while running_ is already false.
    if (!running_.load() && !loop_thread_.joinable() && !zmq_thread_.joinable())
        return;
    running_.store(false);

    // Wake the incoming_cv_ so the loop thread exits wait_for.
    incoming_cv_.notify_all();

    if (loop_thread_.joinable())
        loop_thread_.join();

    if (zmq_thread_.joinable())
        zmq_thread_.join();

    call_on_stop();

    if (consumer_.has_value())
    {
        consumer_->stop();
        consumer_->close();
        consumer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_flexzone_obj(nullptr);
        api_.set_consumer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("[actor/{}] consumer stopped", role_name_);
}

void ConsumerRoleWorker::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_init error: {}", role_name_, e.what());
    }
}

void ConsumerRoleWorker::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !consumer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

void ConsumerRoleWorker::call_on_iteration_(py::object &slot,
                                              py::object &fz,
                                              py::list   &msgs)
{
    try
    {
        py_on_iteration_(slot, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_iteration error: {}", role_name_, e.what());
        if (role_cfg_.validation.stop_on_python_error)
            running_.store(false);
    }
}

void ConsumerRoleWorker::run_loop_shm()
{
    auto *shm = consumer_->shm();
    if (shm == nullptr)
    {
        LOGGER_WARN("[actor/{}] SHM unavailable; falling back to messenger loop",
                    role_name_);
        run_loop_messenger();
        return;
    }

    const auto &val = role_cfg_.validation;

    // Timeout budget for acquire_consume_slot:
    //   timeout_ms > 0  → use timeout_ms as the wait budget; fire on_iteration(None)
    //                      on timeout (silence notification for watchdog / heartbeat use).
    //   timeout_ms == 0 → kShmMaxRateMs: max-rate spin; no timeout callback on miss.
    //   timeout_ms == -1→ kShmBlockMs: near-indefinite wait; bounded for shutdown
    //                      responsiveness; no timeout callback on miss.
    static constexpr int kShmMaxRateMs  = 5;
    static constexpr int kShmBlockMs    = 5000;
    const int acquire_ms = role_cfg_.timeout_ms > 0  ? role_cfg_.timeout_ms
                         : role_cfg_.timeout_ms == 0 ? kShmMaxRateMs
                         : kShmBlockMs;

    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        auto slot_handle = shm->acquire_consume_slot(acquire_ms);
        if (!slot_handle)
        {
            // Slot not available within the timeout window.
            auto msgs = drain_incoming_queue_();
            if (role_cfg_.timeout_ms > 0 || !msgs.empty())
            {
                // Notify script: silence interval OR queued ZMQ messages to deliver.
                py::gil_scoped_acquire g;
                py::object none_slot = py::none();
                py::list   mlst      = build_messages_list_(msgs);
                call_on_iteration_(none_slot, fz_inst_, mlst);
                iteration_count_.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        if (!running_.load() || shutdown_.load())
            break;

        const auto   span     = slot_handle->buffer_span();
        const size_t read_sz  = std::min(span.size_bytes(), schema_slot_size_);

        // ── Slot checksum enforcement ─────────────────────────────────────────
        bool slot_ok = true;
        if (val.slot_checksum == ValidationPolicy::Checksum::Enforce)
        {
            slot_ok = slot_handle->verify_checksum_slot();
            if (!slot_ok)
                LOGGER_WARN("[actor/{}] Slot checksum failed (slot={})",
                            role_name_, slot_handle->slot_id());
        }

        // ── FlexZone checksum enforcement ─────────────────────────────────────
        bool fz_ok = true;
        if (has_fz_ && val.flexzone_checksum == ValidationPolicy::Checksum::Enforce)
        {
            const auto fz_span = slot_handle->flexible_zone_span();
            if (api_.is_fz_accepted(fz_span))
                fz_ok = true;
            else
            {
                fz_ok = slot_handle->verify_checksum_flexible_zone();
                if (!fz_ok)
                    LOGGER_WARN("[actor/{}] FlexZone checksum failed", role_name_);
            }
        }

        const bool overall_valid = slot_ok && fz_ok;
        bool       call_iter     = overall_valid;
        if (!overall_valid &&
            !val.skip_on_validation_error)
            call_iter = true;

        api_.set_slot_valid(overall_valid);

        // ── Drain incoming queue (no GIL) ─────────────────────────────────────
        auto msgs = drain_incoming_queue_();

        if (call_iter)
        {
            py::gil_scoped_acquire g;
            py::object slot_obj = make_slot_view_readonly_(span.data(), read_sz);
            py::list   mlst     = build_messages_list_(msgs);
            call_on_iteration_(slot_obj, fz_inst_, mlst);
        }
        else if (!msgs.empty())
        {
            // Slot skipped due to checksum failure but we still deliver messages.
            py::gil_scoped_acquire g;
            py::object none_slot = py::none();
            py::list   mlst      = build_messages_list_(msgs);
            call_on_iteration_(none_slot, fz_inst_, mlst);
        }

        (void)shm->release_consume_slot(*slot_handle);

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ConsumerRoleWorker::run_loop_messenger()
{
    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        std::vector<IncomingMessage> msgs;
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            incoming_cv_.wait_for(
                lock,
                std::chrono::milliseconds(role_cfg_.messenger_poll_ms),
                [this] {
                    return !incoming_queue_.empty() || !running_.load() || shutdown_.load();
                });
            if (!incoming_queue_.empty())
            {
                msgs.reserve(incoming_queue_.size());
                for (auto &m : incoming_queue_)
                    msgs.push_back(std::move(m));
                incoming_queue_.clear();
            }
        }

        if (!running_.load() || shutdown_.load())
            break;

        {
            py::gil_scoped_acquire g;
            py::object slot = py::none();
            py::list   mlst = build_messages_list_(msgs);
            call_on_iteration_(slot, fz_inst_, mlst);
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ConsumerRoleWorker::run_zmq_thread_()
{
    void *data_sock = consumer_->data_zmq_socket_handle();  // nullptr for Bidir pattern
    void *ctrl_sock = consumer_->ctrl_zmq_socket_handle();
    if (ctrl_sock == nullptr)
        return;  // no ZMQ connection — nothing to poll

    zmq_pollitem_t items[2];
    int nfds = 0;
    if (data_sock != nullptr)
        items[nfds++] = {data_sock, 0, ZMQ_POLLIN, 0};
    const int ctrl_idx = nfds;
    items[nfds++] = {ctrl_sock, 0, ZMQ_POLLIN, 0};

    uint64_t last_iter{0};

    while (running_.load(std::memory_order_relaxed) && !shutdown_.load(std::memory_order_relaxed))
    {
        const int rc = zmq_poll(items, nfds, role_cfg_.messenger_poll_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            LOGGER_WARN("[actor/{}/zmq_thread] zmq_poll error: {}", role_name_, zmq_strerror(errno));
            break;
        }
        if (rc > 0)
        {
            if (data_sock != nullptr && (items[0].revents & ZMQ_POLLIN))
                consumer_->handle_data_events_nowait();
            if (items[ctrl_idx].revents & ZMQ_POLLIN)
                consumer_->handle_ctrl_events_nowait();
        }

        // Consumers do not own the channel; heartbeat responsibility stays with
        // the producer's zmq_thread_.  Track iteration_count_ for future use
        // (e.g. consumer-side application metrics).
        const uint64_t cur = iteration_count_.load(std::memory_order_relaxed);
        if (cur != last_iter)
            last_iter = cur;
    }
}


} // namespace pylabhub::actor
