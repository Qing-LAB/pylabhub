// src/utils/hub_producer.cpp
#include "utils/hub_producer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "channel_handle_internals.hpp"
#include "utils/logger.hpp"
#include "hub_monitored_queue.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "utils/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <optional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include "portable_atomic_shared_ptr.hpp"

namespace pylabhub::hub
{

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{
constexpr int kPeerPollIntervalMs  = 50;  ///< ZMQ poll timeout in peer_thread loop
constexpr int kWriteSlotTimeoutMs  = 5000; ///< Timeout for acquiring a write slot
/// Managed producer lifecycle shutdown: waits for DEREG, in-flight ZMQ sends, and SHM
/// close.  Intentionally longer than kMidTimeoutMs — network operations may be in flight.
constexpr auto kManagedProducerShutdownMs = std::chrono::milliseconds(10000);
} // namespace

// ============================================================================
// PendingCtrlSend — queued outgoing message for peer_thread to send
// ============================================================================

struct PendingCtrlSend
{
    std::string            identity; ///< ZMQ identity of the target consumer
    std::string            type;     ///< Ctrl type string; empty (unused) when is_data=true.
    std::vector<std::byte> data;
    bool                   is_data{false}; ///< true = data frame (type ignored); false = ctrl frame
};

// ============================================================================
// ProducerImpl — internal state (defined in .cpp for pImpl idiom)
// ============================================================================
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) — field order kept for clarity; reorder in a dedicated layout pass if desired
struct ProducerImpl
{
    ChannelHandle                      handle;
    std::unique_ptr<DataBlockProducer> shm;
    Messenger                         *messenger{nullptr};
    std::atomic<bool>                  closed{false};

    // Consumer identity tracking (updated exclusively from peer_thread, read under lock)
    mutable std::mutex       consumer_list_mu;
    std::vector<std::string> consumer_identities;

    // Guards user-facing callbacks during concurrent close(). [PC1]
    mutable std::mutex callbacks_mu;
    /// Maps consumer_pid → ZMQ identity (populated from HELLO body consumer_pid field).
    std::unordered_map<uint64_t, std::string> pid_to_identity;

    // User callbacks (called from the Messenger worker thread or peer_thread)
    Producer::ConsumerCallback    on_consumer_joined_cb;
    Producer::ConsumerCallback    on_consumer_left_cb;
    Producer::MessageCallback     on_consumer_message_cb;
    std::function<void()>         on_channel_closing_cb;
    std::function<void()>         on_force_shutdown_cb;
    Producer::ConsumerDiedCallback on_consumer_died_cb;
    Producer::ChannelErrorCallback on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::thread       peer_thread_handle;
    std::thread       write_thread_handle;

    // Outgoing ctrl/data messages queued for peer_thread to send on the ROUTER socket.
    // MonitoredQueue provides drop-oldest capping (fire_and_forget=true: no backpressure
    // monitoring, since ZMQ always accepts sends regardless of peer liveness).
    MonitoredQueue<PendingCtrlSend> ctrl_queue_;

    // Peer-dead detection: track when we last received a message from any consumer.
    // Updated from recv_and_dispatch_ctrl_() on any HELLO/BYE/custom message.
    // Checked periodically in run_peer_thread() to detect silently-dead consumers.
    bool     peer_ever_seen_{false};
    std::chrono::steady_clock::time_point last_peer_recv_{};
    int      peer_dead_timeout_ms_{30000};

    std::function<void()> on_peer_dead_cb;

    // Mutex guarding sends on the data socket (peer_thread never touches data socket)
    std::mutex data_send_mu;

    // Write job queue (Queue mode): push() enqueues; write_thread dequeues and executes.
    // Jobs are fully-applied closures (type erasure happens at push<F,D> call site).
    std::mutex                        write_queue_mu;
    std::condition_variable           write_queue_cv;
    std::queue<std::function<void()>> write_queue;
    std::atomic<bool>                 write_stop{false};

    // Real-time handler (RealTime mode): write_thread calls this in a loop.
    // nullptr = Queue mode (default). Swapped atomically via _store_write_handler().
    pylabhub::utils::detail::PortableAtomicSharedPtr<InternalWriteHandlerFn> m_write_handler;

    // Messaging facade: filled by create_from_parts(); used by WriteProcessorContext<F,D>.
    ProducerMessagingFacade facade{};

    // HEP-CORE-0021: ZMQ PUSH socket (non-null only when data_transport=="zmq").
    std::unique_ptr<QueueWriter> zmq_queue_;

    // Queue abstraction Phase 2: unified QueueWriter.
    // SHM transport: ShmQueue wrapping `shm`. ZMQ transport: alias for zmq_queue_.
    // Non-null when item_size > 0 (SHM) or data_transport=="zmq".
    std::unique_ptr<QueueWriter> shm_queue_writer_; ///< Owned ShmQueue (SHM only)
    QueueWriter *queue_writer_{nullptr};

    void run_peer_thread();
    void run_write_thread();

    // ── Embedded-mode helpers ────────────────────────────────────────────────

    /// Drain and send all outgoing ctrl/data frames queued via send_ctrl() / send_to().
    /// Safe to call from any thread that owns the ctrl socket.
    void drain_ctrl_send_queue_();

    /// @brief Non-blocking: receive and dispatch one ctrl message from the ROUTER ctrl socket.
    ///
    /// Returns true if a message was consumed (caller should loop to drain all pending);
    /// returns false when no message is available (EAGAIN) or on error.
    ///
    /// @note Callers MUST use a bounded loop (e.g., `while (recv_and_dispatch_ctrl_() &&
    ///       ++n < kMax) {}`) to prevent infinite spin if recv_multipart returns true for
    ///       malformed/empty frames. See §12.3 "Shutdown Pitfalls" in HEP-CORE-0007.
    bool recv_and_dispatch_ctrl_();
};

// ============================================================================
// peer_thread: polls ROUTER ctrl socket, drains outgoing queue
// ============================================================================

// ============================================================================
// ProducerImpl::drain_ctrl_send_queue_ — send all queued outbound ctrl/data frames
// ============================================================================

void ProducerImpl::drain_ctrl_send_queue_()
{
    ctrl_queue_.drain([this](PendingCtrlSend& msg) {
        if (msg.is_data)
            handle.send(msg.data.data(), msg.data.size(), msg.identity);
        else
            handle.send_typed_ctrl(msg.type, msg.data.data(), msg.data.size(), msg.identity);
    });
}

// ============================================================================
// ProducerImpl::recv_and_dispatch_ctrl_ — non-blocking receive + dispatch one message
// ============================================================================

bool ProducerImpl::recv_and_dispatch_ctrl_()
{
    zmq::socket_t *ctrl_sock = channel_handle_ctrl_socket(handle);
    if (!ctrl_sock)
    {
        return false;
    }

    // Non-blocking receive. Throws zmq::error_t(EAGAIN) if no message is waiting.
    // Expected layout: [identity][type_byte='C'][type_str][body]
    std::vector<zmq::message_t> frames;
    try
    {
        auto res = zmq::recv_multipart(*ctrl_sock, std::back_inserter(frames),
                                       zmq::recv_flags::dontwait);
        if (!res.has_value() || *res == 0)
            return false;
    }
    catch (const zmq::error_t &)
    {
        return false; // EAGAIN or other error — no message available
    }

    // Need at least: identity + type_byte + type_str + body = 4 frames
    if (frames.size() < 4 || frames[1].size() < 1)
    {
        return true; // Consumed a message (queue decremented) but malformed — continue
    }

    const char type_byte = *static_cast<const char *>(frames[1].data());
    if (type_byte != 'C')
    {
        return true; // Not a control frame — discard and continue
    }

    std::string identity(static_cast<const char *>(frames[0].data()), frames[0].size());
    std::string type_str(static_cast<const char *>(frames[2].data()), frames[2].size());
    std::span<const std::byte> body(static_cast<const std::byte *>(frames[3].data()),
                                     frames[3].size());

    if (type_str == "HELLO")
    {
        uint64_t cpid = 0;
        try
        {
            std::string body_str(static_cast<const char *>(frames[3].data()), frames[3].size());
            if (!body_str.empty())
            {
                auto body_json = nlohmann::json::parse(body_str);
                cpid = body_json.value("consumer_pid", uint64_t{0});
            }
        }
        catch (...) {}

        {
            std::lock_guard<std::mutex> lock(consumer_list_mu);
            auto it = std::find(consumer_identities.begin(), consumer_identities.end(), identity);
            if (it == consumer_identities.end())
            {
                consumer_identities.push_back(identity);
                if (cpid != 0)
                {
                    pid_to_identity[cpid] = identity;
                }
            }
        }
        // [PC1-ext] Copy under callbacks_mu before invoking — matches the
        // close() null-assignment pattern at callbacks_mu (lines 942-954).
        Producer::ConsumerCallback joined_cb;
        { std::lock_guard<std::mutex> lk(callbacks_mu); joined_cb = on_consumer_joined_cb; }
        if (joined_cb)
            joined_cb(identity);
    }
    else if (type_str == "BYE")
    {
        {
            std::lock_guard<std::mutex> lock(consumer_list_mu);
            auto it = std::find(consumer_identities.begin(), consumer_identities.end(), identity);
            if (it != consumer_identities.end())
            {
                consumer_identities.erase(it);
            }
            for (auto map_it = pid_to_identity.begin(); map_it != pid_to_identity.end();)
            {
                if (map_it->second == identity)
                    map_it = pid_to_identity.erase(map_it);
                else
                    ++map_it;
            }
        }
        Producer::ConsumerCallback left_cb;
        { std::lock_guard<std::mutex> lk(callbacks_mu); left_cb = on_consumer_left_cb; }
        if (left_cb)
            left_cb(identity);
    }
    else
    {
        Producer::MessageCallback msg_cb;
        { std::lock_guard<std::mutex> lk(callbacks_mu); msg_cb = on_consumer_message_cb; }
        if (msg_cb)
            msg_cb(identity, body);
    }
    // Track last peer contact for peer-dead detection.
    peer_ever_seen_ = true;
    last_peer_recv_ = std::chrono::steady_clock::now();
    return true;
}

// ============================================================================
// peer_thread: polls ROUTER ctrl socket, drains outgoing queue
// Refactored to use drain_ctrl_send_queue_() + recv_and_dispatch_ctrl_() helpers.
// ============================================================================

void ProducerImpl::run_peer_thread()
{
    zmq::socket_t *ctrl_sock = channel_handle_ctrl_socket(handle);
    if (!ctrl_sock)
    {
        return; // No ctrl socket — nothing to monitor
    }

    while (running.load(std::memory_order_relaxed))
    {
        // ── 1. Drain outgoing ctrl/data send queue ──────────────────────────
        drain_ctrl_send_queue_();

        // ── Periodic peer-dead check ─────────────────────────────────────
        if (peer_ever_seen_ && peer_dead_timeout_ms_ > 0)
        {
            std::function<void()> dead_cb;
            { std::lock_guard<std::mutex> lk(callbacks_mu); dead_cb = on_peer_dead_cb; }
            if (dead_cb)
            {
                auto gap_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_peer_recv_).count());
                if (gap_ms > peer_dead_timeout_ms_)
                {
                    LOGGER_WARN("[hub::Producer] No peer message for {}ms (timeout={}ms); "
                                "declaring peer dead.", gap_ms, peer_dead_timeout_ms_);
                    dead_cb();
                    peer_ever_seen_ = false;  // Don't fire repeatedly
                }
            }
        }

        // ── 2. Poll ctrl socket for incoming consumer messages ──────────────
        std::vector<zmq::pollitem_t> items = {{ctrl_sock->handle(), 0, ZMQ_POLLIN, 0}};
        try
        {
            zmq::poll(items, std::chrono::milliseconds(kPeerPollIntervalMs));
        }
        catch (const zmq::error_t &)
        {
            break;
        }

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue; // Timeout — loop back and drain send queue again
        }

        // ── 3. Receive and dispatch one message ─────────────────────────────
        recv_and_dispatch_ctrl_();
    }

    drain_ctrl_send_queue_();
}

// ============================================================================
// write_thread: dequeues WriteJobs, acquires slots, executes, commits
// ============================================================================

void ProducerImpl::run_write_thread()
{
    while (true)
    {
        // ── Real-time mode: handler installed ──────────────────────────────
        auto handler = m_write_handler.load(std::memory_order_acquire);
        if (handler)
        {
            if (write_stop.load(std::memory_order_relaxed))
                break; // Exit real-time loop on stop signal
            try
            {
                (*handler)(facade);
            }
            catch (...)
            {
                // Handler threw — continue loop; handler should check is_stopping()
            }
            continue; // Re-check write_stop and handler on every iteration
        }

        // ── Queue mode: wait for a job, a handler install, or stop ─────────
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(write_queue_mu);
            write_queue_cv.wait(lock, [this] {
                return write_stop.load(std::memory_order_relaxed) ||
                       !write_queue.empty() ||
                       m_write_handler.load(std::memory_order_relaxed) != nullptr;
            });

            if (write_stop.load(std::memory_order_relaxed) && write_queue.empty())
            {
                break; // Shutdown — no remaining jobs, exit cleanly
            }
            if (write_queue.empty())
            {
                continue; // Woken by handler install or spurious wake — re-check
            }
            job = std::move(write_queue.front());
            write_queue.pop();
        }

        try
        {
            job(); // Fully-applied closure; manages its own slot acquire/release
        }
        catch (...)
        {
            // Job threw — exception path releases slot inside the closure
        }
    }
}

// ============================================================================
// Producer — construction / destruction
// ============================================================================

Producer::Producer(std::unique_ptr<ProducerImpl> impl) : pImpl(std::move(impl)) {}

Producer::~Producer()
{
    close();
}

Producer::Producer(Producer &&) noexcept            = default;
Producer &Producer::operator=(Producer &&) noexcept = default;

// ============================================================================
// Producer::create — non-template factory
// ============================================================================

std::optional<Producer>
Producer::create(Messenger &messenger, const ProducerOptions &opts)
{
    ChannelRegistrationOptions ch_opts;
    ch_opts.pattern           = opts.pattern;
    ch_opts.has_shared_memory = opts.has_shm;
    ch_opts.schema_hash       = opts.schema_hash;
    ch_opts.schema_version    = opts.schema_version;
    ch_opts.timeout_ms        = opts.timeout_ms;
    ch_opts.role_name        = opts.role_name;
    ch_opts.role_uid         = opts.role_uid;
    ch_opts.data_transport    = opts.data_transport;
    ch_opts.zmq_node_endpoint = opts.zmq_node_endpoint;
    ch_opts.inbox_endpoint    = opts.inbox_endpoint;
    ch_opts.inbox_schema_json = opts.inbox_schema_json;
    ch_opts.inbox_packing     = opts.inbox_packing;
    auto ch = messenger.create_channel(opts.channel_name, ch_opts);
    if (!ch.has_value())
    {
        return std::nullopt;
    }

    std::unique_ptr<DataBlockProducer> shm_producer;
    if (opts.has_shm)
    {
        // Use impl without schema (no-template path has no compile-time type info).
        shm_producer = create_datablock_producer_impl(opts.channel_name,
                                                       DataBlockPolicy::RingBuffer,
                                                       opts.shm_config, nullptr, nullptr);
        if (!shm_producer)
        {
            return std::nullopt;
        }
    }

    return Producer::create_from_parts(messenger, std::move(*ch), std::move(shm_producer), opts);
}

// ============================================================================
// Producer::create_from_parts — assembles a Producer from pre-built parts
// ============================================================================

std::optional<Producer>
Producer::create_from_parts(Messenger &messenger, ChannelHandle channel,
                              std::unique_ptr<DataBlockProducer> shm_producer,
                              const ProducerOptions &opts)
{
    auto impl       = std::make_unique<ProducerImpl>();
    impl->handle    = std::move(channel);
    impl->shm       = std::move(shm_producer);
    impl->messenger = &messenger;
    impl->closed    = false;

    // Wire LoopPolicy (HEP-CORE-0008 Pass 3): role host overrides this after start_embedded().
    if (impl->shm != nullptr &&
        (opts.loop_policy != LoopPolicy::MaxRate || opts.configured_period_us.count() > 0))
    {
        impl->shm->set_loop_policy(opts.loop_policy, opts.configured_period_us);
    }

    // Fill the messaging facade. Function pointers capture nothing except `ctx` (the
    // heap-stable ProducerImpl*). The facade itself lives inside ProducerImpl, so
    // ABI guard: ProducerMessagingFacade is exported across the shared library boundary.
    // Field insertion would silently corrupt function pointer offsets in pre-compiled templates.
    // 8 pointers × 8 bytes = 64 bytes on LP64/LLP64.
    static_assert(sizeof(ProducerMessagingFacade) == 64,
                  "ProducerMessagingFacade size changed — ABI break! "
                  "Append new fields at the end and bump SOVERSION.");

    // `&impl->facade` is stable for the lifetime of ProducerImpl.
    ProducerImpl *raw = impl.get();
    impl->facade.context = raw;

    impl->facade.fn_get_shm = [](void *ctx) -> DataBlockProducer * {
        return static_cast<ProducerImpl *>(ctx)->shm.get();
    };
    impl->facade.fn_consumers = [](void *ctx) -> std::vector<std::string> {
        auto *p = static_cast<ProducerImpl *>(ctx);
        std::lock_guard<std::mutex> lock(p->consumer_list_mu);
        return p->consumer_identities;
    };
    impl->facade.fn_broadcast = [](void *ctx, const void *data, size_t size) -> bool {
        auto *p = static_cast<ProducerImpl *>(ctx);
        if (!p->handle.is_valid() || p->closed)
            return false;
        std::lock_guard<std::mutex> lock(p->data_send_mu);
        return p->handle.send(data, size);
    };
    impl->facade.fn_send_to = [](void *ctx, const std::string &identity, const void *data,
                                  size_t size) -> bool {
        auto *p = static_cast<ProducerImpl *>(ctx);
        if (!p->handle.is_valid() || p->closed)
            return false;
        if (p->running.load(std::memory_order_relaxed))
        {
            std::vector<std::byte> buf(size);
            if (data && size > 0)
                std::memcpy(buf.data(), data, size);
            p->ctrl_queue_.push(
                PendingCtrlSend{identity, {}, std::move(buf), /*is_data=*/true});
            return true;
        }
        return p->handle.send(data, size, identity);
    };
    impl->facade.fn_is_stopping = [](void *ctx) -> bool {
        return static_cast<ProducerImpl *>(ctx)->write_stop.load(std::memory_order_relaxed);
    };
    impl->facade.fn_messenger = [](void *ctx) -> Messenger * {
        return static_cast<ProducerImpl *>(ctx)->messenger;
    };
    impl->facade.fn_channel_name = [](void *ctx) -> const std::string & {
        return static_cast<ProducerImpl *>(ctx)->handle.channel_name();
    };

    // Auto-wire per-channel Messenger callbacks.
    // Callbacks capture a raw ProducerImpl* — Producer::close() clears them before
    // destroying pImpl, preventing use-after-free.
    const std::string ch = raw->handle.channel_name();

    // [PC1] Messenger callbacks run on the Messenger worker thread and may race with
    // close(). Copy the user callback under callbacks_mu before invoking it so that
    // close()'s null-assignment cannot produce a null std::function call.
    messenger.on_channel_closing(ch, [raw]() {
        std::function<void()> cb;
        { std::lock_guard<std::mutex> lk(raw->callbacks_mu); cb = raw->on_channel_closing_cb; }
        if (!raw->closed && cb) cb();
    });

    messenger.on_force_shutdown(ch, [raw]() {
        std::function<void()> cb;
        { std::lock_guard<std::mutex> lk(raw->callbacks_mu); cb = raw->on_force_shutdown_cb; }
        if (!raw->closed && cb) cb();
    });

    messenger.on_consumer_died(ch, [raw](uint64_t pid, std::string reason) {
        if (raw->closed) return;
        {
            std::lock_guard<std::mutex> lock(raw->consumer_list_mu);
            auto it = raw->pid_to_identity.find(pid);
            if (it != raw->pid_to_identity.end())
            {
                const std::string &dead_id = it->second;
                raw->consumer_identities.erase(
                    std::remove(raw->consumer_identities.begin(),
                                raw->consumer_identities.end(), dead_id),
                    raw->consumer_identities.end());
                raw->pid_to_identity.erase(it);
            }
        }
        Producer::ConsumerDiedCallback cb;
        { std::lock_guard<std::mutex> lk(raw->callbacks_mu); cb = raw->on_consumer_died_cb; }
        if (cb) cb(pid, reason);
    });

    messenger.on_channel_error(ch, [raw](std::string event, nlohmann::json details) {
        Producer::ChannelErrorCallback cb;
        { std::lock_guard<std::mutex> lk(raw->callbacks_mu); cb = raw->on_channel_error_cb; }
        if (!raw->closed && cb) cb(event, details);
    });

    // HEP-CORE-0021: create ZMQ PUSH socket when data_transport == "zmq".
    if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[producer] data_transport='zmq' but zmq_node_endpoint is empty");
            return std::nullopt;
        }
        // Derive 8-byte schema tag from the first 8 bytes of schema_hash (binary).
        std::optional<std::array<uint8_t, 8>> schema_tag;
        if (opts.schema_hash.size() >= 8)
        {
            std::array<uint8_t, 8> tag{};
            std::memcpy(tag.data(), opts.schema_hash.data(), 8);
            schema_tag = tag;
        }
        impl->zmq_queue_ = ZmqQueue::push_to(
            opts.zmq_node_endpoint, opts.zmq_schema, opts.zmq_packing, opts.zmq_bind, schema_tag,
            /*sndhwm=*/0, opts.zmq_buffer_depth, opts.zmq_overflow_policy);
        if (!impl->zmq_queue_)
        {
            return std::nullopt; // Error already logged by factory (empty/invalid schema, etc.)
        }
        if (!impl->zmq_queue_->start()) // [ZQ2] Check return value — bind/connect may fail.
        {
            LOGGER_ERROR("[producer] ZMQ PUSH socket start() failed for '{}'",
                         opts.zmq_node_endpoint);
            return std::nullopt;
        }
        LOGGER_INFO("[producer] ZMQ PUSH socket created at '{}'", opts.zmq_node_endpoint);
    }

    // ctrl_queue_ is fire_and_forget (ZMQ always accepts sends); no monitoring callbacks needed.
    // Always apply opts.ctrl_queue_max_depth (0 = unbounded, >0 = drop-oldest cap).
    {
        MonitoredQueue<PendingCtrlSend>::Config qcfg;
        qcfg.max_depth    = opts.ctrl_queue_max_depth;
        impl->ctrl_queue_ = MonitoredQueue<PendingCtrlSend>(qcfg);
    }
    impl->peer_dead_timeout_ms_ = opts.peer_dead_timeout_ms;

    // Queue abstraction Phase 2: create unified QueueWriter.
    // SHM: wrap DataBlockProducer in ShmQueue. ZMQ: use existing zmq_queue_.
    if (impl->shm && opts.item_size > 0)
    {
        impl->shm_queue_writer_ = ShmQueue::from_producer_ref(
            *impl->shm, opts.item_size, opts.flexzone_size, opts.channel_name);
        impl->queue_writer_ = impl->shm_queue_writer_.get();
    }
    else if (impl->zmq_queue_)
    {
        impl->queue_writer_ = impl->zmq_queue_.get();
    }

    return Producer(std::move(impl));
}

// ============================================================================
// Producer — callback registration
// ============================================================================

void Producer::on_consumer_joined(ConsumerCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_consumer_joined_cb = std::move(cb);
    }
}

void Producer::on_consumer_left(ConsumerCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_consumer_left_cb = std::move(cb);
    }
}

void Producer::on_consumer_message(MessageCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_consumer_message_cb = std::move(cb);
    }
}

void Producer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Producer::on_force_shutdown(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_force_shutdown_cb = std::move(cb);
    }
}

void Producer::on_consumer_died(ConsumerDiedCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_consumer_died_cb = std::move(cb);
    }
}

void Producer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_error_cb = std::move(cb);
    }
}

// ============================================================================
// Producer — active mode
// ============================================================================

bool Producer::start()
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.exchange(true, std::memory_order_acq_rel))
    {
        return false; // Already running
    }

    pImpl->write_stop.store(false, std::memory_order_relaxed);

    auto *impl_ptr = pImpl.get();
    pImpl->peer_thread_handle = std::thread([impl_ptr] { impl_ptr->run_peer_thread(); });

    if (pImpl->shm)
    {
        pImpl->write_thread_handle = std::thread([impl_ptr] { impl_ptr->run_write_thread(); });
    }

    return true;
}

void Producer::stop()
{
    if (!pImpl)
    {
        return;
    }

    if (!pImpl->running.exchange(false, std::memory_order_acq_rel))
    {
        return; // Was not running
    }

    // Signal write_thread to stop after draining the queue
    {
        std::lock_guard<std::mutex> lock(pImpl->write_queue_mu);
        pImpl->write_stop.store(true, std::memory_order_relaxed);
    }
    pImpl->write_queue_cv.notify_all();

    if (pImpl->peer_thread_handle.joinable())
    {
        pImpl->peer_thread_handle.join();
    }
    if (pImpl->write_thread_handle.joinable())
    {
        pImpl->write_thread_handle.join();
    }

    // Stop ZMQ PUSH socket after threads join (they may still be sending). [PC3]
    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset(); // Null after stop so a hypothetical re-start cannot use a stale queue.
    }
}

bool Producer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
}

// ============================================================================
// Producer — embedded mode
// ============================================================================

bool Producer::start_embedded() noexcept
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    bool expected = false;
    // CAS: only transitions running false→true; returns false if already running.
    // Does NOT launch peer_thread or write_thread — caller drives ZMQ polling.
    return pImpl->running.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void *Producer::peer_ctrl_socket_handle() const noexcept
{
    if (!pImpl || pImpl->closed || !pImpl->handle.is_valid())
    {
        return nullptr;
    }
    zmq::socket_t *sock = channel_handle_ctrl_socket(pImpl->handle);
    return sock ? sock->handle() : nullptr;
}

void Producer::handle_peer_events_nowait() noexcept
{
    if (!pImpl || pImpl->closed || !pImpl->running.load(std::memory_order_relaxed))
    {
        return;
    }
    pImpl->drain_ctrl_send_queue_();
    // Drain all pending ctrl messages, with safety cap to prevent spin loops.
    // See HEP-CORE-0007 §12.3 "Shutdown Pitfalls".
    static constexpr int kMaxRecvBatch = 100;
    int n = 0;
    while (pImpl->recv_and_dispatch_ctrl_() && ++n < kMaxRecvBatch) {}
    if (n >= kMaxRecvBatch)
        LOGGER_WARN("Producer: handle_peer_events_nowait hit recv batch cap ({})", n);

    // Peer-dead check (embedded mode): mirrors the check in run_peer_thread().
    // Called from the script host's ctrl_thread on each loop iteration.
    if (pImpl->peer_ever_seen_ && pImpl->peer_dead_timeout_ms_ > 0)
    {
        std::function<void()> dead_cb;
        { std::lock_guard<std::mutex> lk(pImpl->callbacks_mu); dead_cb = pImpl->on_peer_dead_cb; }
        if (dead_cb)
        {
            auto gap_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - pImpl->last_peer_recv_).count());
            if (gap_ms > pImpl->peer_dead_timeout_ms_)
            {
                LOGGER_WARN("[hub::Producer] No peer message for {}ms (timeout={}ms); "
                            "declaring peer dead.", gap_ms, pImpl->peer_dead_timeout_ms_);
                dead_cb();
                pImpl->peer_ever_seen_ = false; // Prevent repeated firing
            }
        }
    }
}

// ============================================================================
// Producer — ZMQ messaging
// ============================================================================

bool Producer::send(const void *data, size_t size)
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    // data_socket is NOT owned by peer_thread, so a mutex suffices.
    std::lock_guard<std::mutex> lock(pImpl->data_send_mu);
    return pImpl->handle.send(data, size);
}

bool Producer::send_to(const std::string &identity, const void *data, size_t size)
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.load(std::memory_order_relaxed))
    {
        // ctrl_socket is owned by peer_thread — must queue
        std::vector<std::byte> buf(size);
        if (data && size > 0)
        {
            std::memcpy(buf.data(), data, size);
        }
        pImpl->ctrl_queue_.push(
            PendingCtrlSend{identity, {}, std::move(buf), /*is_data=*/true});
        return true;
    }
    // Not started — send directly (caller's thread owns the sockets)
    return pImpl->handle.send(data, size, identity);
}

bool Producer::send_ctrl(const std::string &identity, std::string_view type,
                          const void *data, size_t size)
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.load(std::memory_order_relaxed))
    {
        std::vector<std::byte> buf(size);
        if (data && size > 0)
        {
            std::memcpy(buf.data(), data, size);
        }
        pImpl->ctrl_queue_.push(
            PendingCtrlSend{identity, std::string(type), std::move(buf), /*is_data=*/false});
        return true;
    }
    // Not started — send directly
    return pImpl->handle.send_typed_ctrl(type, data, size, identity);
}

// ============================================================================
// Producer — non-template helpers for template method implementations
// ============================================================================

bool Producer::_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm != nullptr;
}

bool Producer::_is_started_and_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm != nullptr &&
           pImpl->running.load(std::memory_order_relaxed);
}

ProducerMessagingFacade &Producer::_messaging_facade() const
{
    assert(pImpl);
    return pImpl->facade;
}

void Producer::_enqueue_write_job(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(pImpl->write_queue_mu);
        pImpl->write_queue.push(std::move(job));
    }
    pImpl->write_queue_cv.notify_one();
}

void Producer::_store_write_handler(std::shared_ptr<InternalWriteHandlerFn> h) noexcept
{
    if (pImpl)
    {
        pImpl->m_write_handler.store(std::move(h), std::memory_order_release);
        // Wake the write_thread so it transitions between Queue and RealTime modes.
        pImpl->write_queue_cv.notify_all();
    }
}

bool Producer::is_stopping() const noexcept
{
    return pImpl && pImpl->write_stop.load(std::memory_order_relaxed);
}

bool Producer::has_realtime_handler() const noexcept
{
    return pImpl && pImpl->m_write_handler.load(std::memory_order_relaxed) != nullptr;
}

Messenger &Producer::messenger() const
{
    assert(pImpl && pImpl->messenger);
    return *pImpl->messenger;
}

// ============================================================================
// Producer — consumer list
// ============================================================================

std::vector<std::string> Producer::connected_consumers() const
{
    if (!pImpl)
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(pImpl->consumer_list_mu);
    return pImpl->consumer_identities;
}

// ============================================================================
// Producer — introspection
// ============================================================================

bool Producer::is_valid() const
{
    return pImpl && !pImpl->closed && pImpl->handle.is_valid();
}

const std::string &Producer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->handle.channel_name() : kEmpty;
}

ChannelPattern Producer::pattern() const
{
    return pImpl ? pImpl->handle.pattern() : ChannelPattern::PubSub;
}

bool Producer::has_shm() const
{
    return pImpl && pImpl->shm != nullptr;
}

DataBlockProducer *Producer::shm() noexcept
{
    return pImpl ? pImpl->shm.get() : nullptr;
}

ZmqQueue *Producer::queue() noexcept
{
    // zmq_queue_ is stored as QueueWriter; runtime type is always ZmqQueue (from push_to).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Producer — Queue data operations (forwarded to internal QueueWriter)
// ============================================================================

void *Producer::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    auto *q = pImpl ? pImpl->queue_writer_ : nullptr;
    return q ? q->write_acquire(timeout) : nullptr;
}

void Producer::write_commit() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->write_commit();
}

void Producer::write_discard() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->write_discard();
}

size_t Producer::queue_item_size() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->item_size() : 0;
}

size_t Producer::queue_capacity() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->capacity() : 0;
}

QueueMetrics Producer::queue_metrics() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->metrics() : QueueMetrics{};
}

void Producer::reset_queue_metrics() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->reset_metrics();
}

bool Producer::start_queue()
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->start() : false;
}

void Producer::stop_queue()
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->stop();
}

// ============================================================================
// Producer — Channel data operations (flexzone, checksum)
// ============================================================================

void *Producer::write_flexzone() noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->write_flexzone() : nullptr;
}

const void *Producer::read_flexzone() const noexcept
{
    // Writer side can read its own flexzone for verification / on_init populate.
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->write_flexzone() : nullptr;
}

size_t Producer::flexzone_size() const noexcept
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->flexzone_size() : 0;
}

void Producer::set_checksum_options(bool slot, bool fz) noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->set_checksum_options(slot, fz);
}

void Producer::sync_flexzone_checksum() noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->sync_flexzone_checksum();
}

void Producer::set_queue_period(uint64_t period_us) noexcept
{
    if (pImpl && pImpl->queue_writer_)
        pImpl->queue_writer_->set_configured_period(period_us);
}

std::string Producer::queue_policy_info() const
{
    return (pImpl && pImpl->queue_writer_) ? pImpl->queue_writer_->policy_info() : std::string{};
}

ChannelHandle &Producer::channel_handle()
{
    assert(pImpl);
    return pImpl->handle;
}

// ============================================================================
// Producer — peer-dead callback + metrics
// ============================================================================

void Producer::on_peer_dead(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_peer_dead_cb = std::move(cb);
    }
}

uint64_t Producer::ctrl_queue_dropped() const
{
    return pImpl ? pImpl->ctrl_queue_.total_dropped() : 0;
}

// ============================================================================
// Producer::close — idempotent teardown
// ============================================================================

void Producer::close()
{
    if (!pImpl || pImpl->closed)
    {
        return;
    }
    stop();

    // Clear per-channel Messenger callbacks BEFORE deregistering so no callbacks
    // fire during the deregistration process.
    if (pImpl->messenger && pImpl->handle.is_valid())
    {
        const std::string &ch = pImpl->handle.channel_name();
        pImpl->messenger->on_channel_closing(ch, nullptr);
        pImpl->messenger->on_force_shutdown(ch, nullptr);
        pImpl->messenger->on_consumer_died(ch, nullptr);
        pImpl->messenger->on_channel_error(ch, nullptr);
        pImpl->messenger->unregister_channel(ch);
    }

    // Clear all user-facing callbacks. Messenger-registered lambdas (which run on the
    // Messenger worker thread) copy callbacks under callbacks_mu before invoking, so
    // nulling here is race-free with respect to those lambdas. Peer-thread callbacks
    // (joined_cb, left_cb, message_cb, on_peer_dead_cb) are safe to null without lock
    // because peer_thread has already been joined by stop() above.
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb  = nullptr;
        pImpl->on_force_shutdown_cb   = nullptr;
        pImpl->on_consumer_died_cb    = nullptr;
        pImpl->on_channel_error_cb    = nullptr;
    }
    pImpl->on_consumer_joined_cb  = nullptr;
    pImpl->on_consumer_left_cb    = nullptr;
    pImpl->on_consumer_message_cb = nullptr;
    pImpl->on_peer_dead_cb        = nullptr;

    pImpl->handle.invalidate();
    // Reset queue abstraction before shm — ShmQueue holds non-owning ref to DataBlock.
    pImpl->queue_writer_ = nullptr;
    pImpl->shm_queue_writer_.reset();
    pImpl->zmq_queue_.reset();
    pImpl->shm.reset();
    pImpl->closed = true;
}

// ============================================================================
// ManagedProducer — static registry
// ============================================================================

namespace
{
std::mutex                                          g_producer_registry_mu;
std::unordered_map<std::string, ManagedProducer *>  g_producer_registry;
} // namespace

ManagedProducer::ManagedProducer(Messenger &messenger, ProducerOptions opts)
    : messenger_(&messenger)
    , opts_(std::move(opts))
    , module_key_("pylabhub::hub::Producer::" + opts_.channel_name)
{
}

ManagedProducer::~ManagedProducer()
{
    // Best-effort cleanup if lifecycle shutdown didn't run
    if (producer_.has_value())
    {
        producer_->close();
    }
    std::lock_guard<std::mutex> lock(g_producer_registry_mu);
    g_producer_registry.erase(module_key_);
}

// NOTE: move is safe only BEFORE get_module_def() registers `this` in g_producer_registry.
// After get_module_def(), the registry holds the old `this` pointer. Moving the object
// without updating the registry entry will leave a dangling pointer (moved-from `this`)
// in the registry. The intended usage pattern is:
//   ManagedProducer mp(messenger, opts);         // move safe here
//   lifecycle.add_module(mp.get_module_def());   // registers `this` — no move after this point
//   lifecycle.run();                             // lifecycle owns mp in-place
ManagedProducer::ManagedProducer(ManagedProducer &&) noexcept            = default;
ManagedProducer &ManagedProducer::operator=(ManagedProducer &&) noexcept = default;

pylabhub::utils::ModuleDef ManagedProducer::get_module_def()
{
    {
        std::lock_guard<std::mutex> lock(g_producer_registry_mu);
        g_producer_registry[module_key_] = this;
    }

    pylabhub::utils::ModuleDef def(module_key_);
    def.add_dependency("pylabhub::hub::DataExchangeHub");
    def.set_startup(&ManagedProducer::s_startup, module_key_);
    def.set_shutdown(&ManagedProducer::s_shutdown, kManagedProducerShutdownMs,
                     module_key_);
    return def;
}

Producer &ManagedProducer::get()
{
    if (!producer_.has_value())
    {
        throw std::runtime_error(
            "ManagedProducer::get(): not initialized (lifecycle not started?)");
    }
    return *producer_;
}

bool ManagedProducer::is_initialized() const noexcept
{
    return producer_.has_value();
}

void ManagedProducer::s_startup(const char *key)
{
    ManagedProducer *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_producer_registry_mu);
        auto it = g_producer_registry.find(key);
        if (it == g_producer_registry.end())
        {
            return;
        }
        self = it->second;
    }

    auto p = Producer::create(*self->messenger_, self->opts_);
    if (!p.has_value())
    {
        throw std::runtime_error(
            std::string("ManagedProducer: failed to create producer for channel: ") +
            self->opts_.channel_name);
    }
    p->start();
    self->producer_ = std::move(p);
}

void ManagedProducer::s_shutdown(const char *key)
{
    ManagedProducer *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_producer_registry_mu);
        auto it = g_producer_registry.find(key);
        if (it == g_producer_registry.end())
        {
            return;
        }
        self = it->second;
    }

    if (self->producer_.has_value())
    {
        self->producer_->stop();
        self->producer_->close();
    }
}

} // namespace pylabhub::hub
