// src/utils/hub_consumer.cpp
#include "utils/hub_consumer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "channel_handle_internals.hpp"
#include "utils/logger.hpp"
#include "hub_monitored_queue.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include "utils/json_fwd.hpp"

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
constexpr int kCtrlPollIntervalMs = 50;  ///< ZMQ poll timeout in ctrl_thread loop
constexpr int kDataPollIntervalMs = 50;  ///< ZMQ poll timeout in data_thread loop
/// Managed consumer lifecycle shutdown: waits for BYE, SHM detach, and ZMQ teardown.
/// Intentionally longer than kMidTimeoutMs — network operations may be in flight.
constexpr auto kManagedConsumerShutdownMs = std::chrono::milliseconds(10000);

/// Outgoing ctrl message queued for ctrl_thread to send on the DEALER socket.
/// Consumer sends only one direction (no identity needed — DEALER routes automatically).
struct PendingCtrlSend
{
    std::string            type;    // empty when is_raw=true
    std::vector<std::byte> data;
    bool                   is_raw{false}; // true → handle.send(); false → handle.send_typed_ctrl()
};
} // namespace

// ============================================================================
// ConsumerImpl — internal state
// ============================================================================

struct ConsumerImpl
{
    ChannelHandle                       handle;
    std::unique_ptr<DataBlockConsumer>  shm;
    Messenger                          *messenger{nullptr};
    std::atomic<bool>                   closed{false};

    // User callbacks (called from data_thread / ctrl_thread / Messenger worker thread)
    Consumer::DataCallback         on_zmq_data_cb;
    Consumer::CtrlCallback         on_producer_message_cb;
    std::function<void()>          on_channel_closing_cb;
    std::function<void()>          on_force_shutdown_cb;
    Consumer::ChannelErrorCallback on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested_{false}; ///< Set by stop(); used by is_stopping(). [PC2]
    std::thread       data_thread_handle;
    std::thread       ctrl_thread_handle;
    std::thread       shm_thread_handle;

    // Guards user-facing callbacks during concurrent close(). [PC1]
    mutable std::mutex callbacks_mu;

    // Outgoing ctrl messages queued for ctrl_thread to send on the DEALER socket.
    // MonitoredQueue provides drop-oldest capping (fire_and_forget=true: no backpressure
    // monitoring, since ZMQ always accepts sends regardless of peer liveness).
    MonitoredQueue<PendingCtrlSend>  ctrl_queue_;

    // Peer-dead detection: track when we last received a message from the producer.
    // Updated from recv_and_dispatch_ctrl_() on any ctrl message.
    // Checked in run_ctrl_thread() (standalone mode) and handle_ctrl_events_nowait() (embedded).
    bool     peer_ever_seen_{false};
    std::chrono::steady_clock::time_point last_peer_recv_{};
    int      peer_dead_timeout_ms_{30000};

    std::function<void()> on_peer_dead_cb;

    // Real-time handler (RealTime mode): shm_thread calls this in a loop.
    // nullptr = Queue mode (default). Swapped atomically via _store_read_handler().
    pylabhub::utils::detail::PortableAtomicSharedPtr<InternalReadHandlerFn> m_read_handler;

    // CV used to wake shm_thread from Queue-mode idle sleep.
    // Notified when: handler is installed, or stop() is called.
    std::mutex              m_handler_cv_mu;
    std::condition_variable m_handler_cv;

    // Messaging facade: filled by establish_channel(); used by ReadProcessorContext<F,D>.
    ConsumerMessagingFacade facade{};

    // HEP-CORE-0021: ZMQ PULL socket (non-null only when data_transport=="zmq").
    std::unique_ptr<QueueReader> zmq_queue_;

    // Queue abstraction Phase 2: unified QueueReader.
    // SHM transport: ShmQueue wrapping `shm`. ZMQ transport: alias for zmq_queue_.
    // Non-null when item_size > 0 (SHM) or data_transport=="zmq".
    std::unique_ptr<QueueReader> shm_queue_reader_; ///< Owned ShmQueue (SHM only)
    QueueReader *queue_reader_{nullptr};

    void run_data_thread();
    void run_ctrl_thread();
    void run_shm_thread();

    // ── Embedded-mode helpers ────────────────────────────────────────────────

    /// Drain and send all queued outbound ctrl frames (from send_ctrl() calls).
    void drain_ctrl_send_queue_();

    /// @brief Non-blocking: receive and dispatch one data frame from data socket.
    /// Returns true if a message was consumed; false when no message available.
    /// @note Callers MUST use a bounded loop to prevent infinite spin.
    /// See HEP-CORE-0007 §12.3 "Shutdown Pitfalls".
    bool recv_and_dispatch_data_();

    /// @brief Non-blocking: receive and dispatch one ctrl frame from ctrl socket.
    /// Returns true if a message was consumed; false when no message available.
    /// @note Callers MUST use a bounded loop to prevent infinite spin.
    /// See HEP-CORE-0007 §12.3 "Shutdown Pitfalls".
    bool recv_and_dispatch_ctrl_();
};

// ============================================================================
// ConsumerImpl::drain_ctrl_send_queue_ — send all queued outbound ctrl frames
// ============================================================================

void ConsumerImpl::drain_ctrl_send_queue_()
{
    ctrl_queue_.drain([this](PendingCtrlSend& msg) {
        if (msg.is_raw)
            handle.send(msg.data.data(), msg.data.size());
        else
            handle.send_typed_ctrl(msg.type, msg.data.data(), msg.data.size());
    });
}

// ============================================================================
// ConsumerImpl::recv_and_dispatch_data_ — non-blocking receive + dispatch one data frame
// ============================================================================

bool ConsumerImpl::recv_and_dispatch_data_()
{
    zmq::socket_t *data_sock = channel_handle_data_socket(handle);
    if (!data_sock)
    {
        return false;
    }

    // Receive: [type_byte='A'][payload] (non-blocking)
    std::vector<zmq::message_t> frames;
    try
    {
        auto res = zmq::recv_multipart(*data_sock, std::back_inserter(frames),
                                       zmq::recv_flags::dontwait);
        if (!res.has_value() || *res == 0)
            return false;
    }
    catch (const zmq::error_t &)
    {
        return false; // EAGAIN or other error
    }

    if (frames.size() < 2 || frames[0].size() < 1)
    {
        return true; // Consumed but malformed
    }

    const char type_byte = *static_cast<const char *>(frames[0].data());
    if (type_byte != 'A')
    {
        return true; // Not a data frame — discard
    }

    {
        // [PC1-ext] Copy under callbacks_mu — on_zmq_data_cb setter and
        // close() both write under callbacks_mu; invocation must match.
        Consumer::DataCallback data_cb;
        { std::lock_guard<std::mutex> lk(callbacks_mu); data_cb = on_zmq_data_cb; }
        if (data_cb)
        {
            std::span<const std::byte> payload(
                static_cast<const std::byte *>(frames[1].data()), frames[1].size());
            data_cb(payload);
        }
    }
    return true;
}

// ============================================================================
// ConsumerImpl::recv_and_dispatch_ctrl_ — non-blocking receive + dispatch one ctrl frame
// ============================================================================

bool ConsumerImpl::recv_and_dispatch_ctrl_()
{
    zmq::socket_t *ctrl_sock = channel_handle_ctrl_socket(handle);
    if (!ctrl_sock)
    {
        return false;
    }

    // Receive from DEALER: [type_byte='C'][type_str][body]
    // (For Bidir, data frames ['A'][payload] may also arrive here)
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
        return false; // EAGAIN or other error
    }

    if (frames.size() < 2 || frames[0].size() < 1)
    {
        return true; // Consumed but malformed
    }

    const char type_byte = *static_cast<const char *>(frames[0].data());

    if (type_byte == 'A')
    {
        // Data frame on ctrl socket (Bidir pattern)
        Consumer::DataCallback data_cb;
        { std::lock_guard<std::mutex> lk(callbacks_mu); data_cb = on_zmq_data_cb; }
        if (data_cb && frames.size() >= 2)
        {
            std::span<const std::byte> payload(
                static_cast<const std::byte *>(frames[1].data()), frames[1].size());
            data_cb(payload);
        }
        return true;
    }

    if (type_byte != 'C' || frames.size() < 3)
    {
        return true; // Unknown frame type or too short
    }

    std::string_view type_str(static_cast<const char *>(frames[1].data()), frames[1].size());
    std::span<const std::byte> body(static_cast<const std::byte *>(frames[2].data()),
                                     frames[2].size());

    Consumer::CtrlCallback ctrl_cb;
    { std::lock_guard<std::mutex> lk(callbacks_mu); ctrl_cb = on_producer_message_cb; }
    if (ctrl_cb)
        ctrl_cb(type_str, body);
    // Track last peer contact for peer-dead detection.
    peer_ever_seen_ = true;
    last_peer_recv_ = std::chrono::steady_clock::now();
    return true;
}

// ============================================================================
// data_thread: polls SUB/PULL data socket for ZMQ data frames
// Refactored to use recv_and_dispatch_data_() helper.
// ============================================================================

void ConsumerImpl::run_data_thread()
{
    zmq::socket_t *data_sock = channel_handle_data_socket(handle);
    if (!data_sock)
    {
        return; // No data socket (Bidir pattern uses ctrl socket only)
    }

    while (running.load(std::memory_order_relaxed))
    {
        std::vector<zmq::pollitem_t> items = {{data_sock->handle(), 0, ZMQ_POLLIN, 0}};
        try
        {
            zmq::poll(items, std::chrono::milliseconds(kDataPollIntervalMs));
        }
        catch (const zmq::error_t &)
        {
            break;
        }

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        recv_and_dispatch_data_();
    }
}

// ============================================================================
// ctrl_thread: polls DEALER ctrl socket for control frames from producer
// Refactored to use drain_ctrl_send_queue_() + recv_and_dispatch_ctrl_() helpers.
// ============================================================================

void ConsumerImpl::run_ctrl_thread()
{
    zmq::socket_t *ctrl_sock = channel_handle_ctrl_socket(handle);
    if (!ctrl_sock)
    {
        return;
    }

    while (running.load(std::memory_order_relaxed))
    {
        // ── 1. Drain outgoing ctrl send queue ───────────────────────────────
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
                    LOGGER_WARN("[hub::Consumer] No peer message for {}ms (timeout={}ms); "
                                "declaring peer dead.", gap_ms, peer_dead_timeout_ms_);
                    dead_cb();
                    peer_ever_seen_ = false;  // Don't fire repeatedly
                }
            }
        }

        // ── 2. Poll ctrl socket for incoming producer messages ───────────────
        std::vector<zmq::pollitem_t> items = {{ctrl_sock->handle(), 0, ZMQ_POLLIN, 0}};
        try
        {
            zmq::poll(items, std::chrono::milliseconds(kCtrlPollIntervalMs));
        }
        catch (const zmq::error_t &)
        {
            break;
        }

        if ((items[0].revents & ZMQ_POLLIN) == 0)
        {
            continue;
        }

        recv_and_dispatch_ctrl_();
    }
}

// ============================================================================
// shm_thread: drives DataBlock SHM reading in Queue or Real-time mode
// ============================================================================

void ConsumerImpl::run_shm_thread()
{
    if (!shm)
    {
        return;
    }

    while (running.load(std::memory_order_relaxed))
    {
        // ── Real-time mode: handler installed ──────────────────────────────
        auto handler = m_read_handler.load(std::memory_order_acquire);
        if (handler)
        {
            try
            {
                (*handler)(facade);
            }
            catch (...)
            {
                // Handler threw — continue loop; handler should check is_stopping()
            }
            continue; // Re-check running and handler on every iteration
        }

        // ── Queue mode: sleep until handler installed or stop ───────────────
        // pull<F,D>() runs directly in the caller's thread; shm_thread is idle here.
        std::unique_lock<std::mutex> lock(m_handler_cv_mu);
        m_handler_cv.wait(lock, [this] {
            return !running.load(std::memory_order_relaxed) ||
                   m_read_handler.load(std::memory_order_relaxed) != nullptr;
        });
        // Loop again: running flag checked by while() condition above.
    }
}

// ============================================================================
// Consumer — construction / destruction
// ============================================================================

Consumer::Consumer(std::unique_ptr<ConsumerImpl> impl) : pImpl(std::move(impl)) {}

Consumer::~Consumer()
{
    close();
}

Consumer::Consumer(Consumer &&) noexcept            = default;
Consumer &Consumer::operator=(Consumer &&) noexcept = default;

// ============================================================================
// Consumer::connect — non-template factory
// ============================================================================

std::optional<Consumer>
Consumer::connect(Messenger &messenger, const ConsumerOptions &opts)
{
    // loop_driver: explicit declaration wins; empty = no declaration (processor transport-agnostic use-case).
    auto ch = messenger.connect_channel(opts.channel_name, opts.timeout_ms,
                                         opts.expected_schema_hash,
                                         opts.consumer_uid, opts.consumer_name,
                                         {}, opts.queue_type,
                                         opts.inbox_endpoint, opts.inbox_schema_json,
                                         opts.inbox_packing, opts.inbox_checksum);
    if (!ch.has_value())
    {
        return std::nullopt;
    }

    std::unique_ptr<DataBlockConsumer> shm_consumer;
    if (ch->has_shm() && opts.shm_shared_secret != 0)
    {
        // Attach to SHM without schema type validation (non-template path)
        const DataBlockConfig *cfg_ptr =
            opts.expected_shm_config.has_value() ? &(*opts.expected_shm_config) : nullptr;

        const char *uid  = opts.consumer_uid.empty()  ? nullptr : opts.consumer_uid.c_str();
        const char *cnam = opts.consumer_name.empty() ? nullptr : opts.consumer_name.c_str();
        shm_consumer = find_datablock_consumer_impl(ch->shm_name(), opts.shm_shared_secret,
                                                     cfg_ptr, nullptr, nullptr, uid, cnam);
        // nullptr is acceptable — secret mismatch or SHM unavailable; ZMQ still works
    }

    return Consumer::establish_channel(messenger, std::move(*ch), std::move(shm_consumer),
                                         opts);
}

// ============================================================================
// Consumer::establish_channel — wire callbacks, create queues, configure channel
// ============================================================================

std::optional<Consumer>
Consumer::establish_channel(Messenger &messenger, ChannelHandle channel,
                               std::unique_ptr<DataBlockConsumer> shm_consumer,
                               const ConsumerOptions &opts)
{
    auto impl       = std::make_unique<ConsumerImpl>();
    impl->handle    = std::move(channel);
    impl->shm       = std::move(shm_consumer);
    impl->messenger = &messenger;
    impl->closed    = false;

    // Timing is set at the queue level after ShmQueue/ZmqQueue creation (see below).

    // ABI guard: ConsumerMessagingFacade is exported across the shared library boundary.
    // 6 pointers × 8 bytes = 48 bytes on LP64/LLP64.
    static_assert(sizeof(ConsumerMessagingFacade) == 48,
                  "ConsumerMessagingFacade size changed — ABI break! "
                  "Append new fields at the end and bump SOVERSION.");

    // Fill the messaging facade. Function pointers capture nothing except `ctx` (the
    // heap-stable ConsumerImpl*). The facade itself lives inside ConsumerImpl.
    ConsumerImpl *raw    = impl.get();
    impl->facade.context = raw;

    impl->facade.fn_get_shm = [](void *ctx) -> DataBlockConsumer * {
        return static_cast<ConsumerImpl *>(ctx)->shm.get();
    };
    impl->facade.fn_send_ctrl = [](void *ctx, const char *type, const void *data,
                                    size_t size) -> bool {
        auto *c = static_cast<ConsumerImpl *>(ctx);
        if (!c->handle.is_valid() || c->closed)
            return false;
        if (c->running.load(std::memory_order_relaxed))
        {
            std::vector<std::byte> buf(size);
            if (data && size > 0)
                std::memcpy(buf.data(), data, size);
            c->ctrl_queue_.push({std::string(type), std::move(buf)});
            return true;
        }
        return c->handle.send_typed_ctrl(type, data, size);
    };
    impl->facade.fn_is_stopping = [](void *ctx) -> bool {
        return !static_cast<ConsumerImpl *>(ctx)->running.load(std::memory_order_relaxed);
    };
    impl->facade.fn_messenger = [](void *ctx) -> Messenger * {
        return static_cast<ConsumerImpl *>(ctx)->messenger;
    };
    impl->facade.fn_channel_name = [](void *ctx) -> const std::string & {
        return static_cast<ConsumerImpl *>(ctx)->handle.channel_name();
    };

    // Announce this consumer to the producer via the ctrl (DEALER) socket.
    // HELLO body includes consumer_pid so producer can populate its pid_to_identity map.
    if (impl->handle.is_valid())
    {
        try
        {
            nlohmann::json hello_body;
            hello_body["consumer_pid"] = pylabhub::platform::get_pid();
            const std::string hello_str = hello_body.dump();
            (void)impl->handle.send_typed_ctrl("HELLO", hello_str.data(), hello_str.size());
        }
        catch (...)
        {
            // Send failure is non-fatal — producer will handle missing HELLO gracefully.
        }
    }

    // Auto-wire per-channel Messenger callbacks.
    // Callbacks capture `raw` (heap-stable ConsumerImpl*) — Consumer::close() clears
    // them before destroying pImpl, preventing use-after-free.
    const std::string ch = raw->handle.channel_name();

    // [PC1] Messenger callbacks run on the Messenger worker thread and may race with
    // close(). Copy the user callback under callbacks_mu before invoking it.
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

    messenger.on_channel_error(ch, [raw](std::string event, nlohmann::json details) {
        Consumer::ChannelErrorCallback cb;
        { std::lock_guard<std::mutex> lk(raw->callbacks_mu); cb = raw->on_channel_error_cb; }
        if (!raw->closed && cb) cb(event, details);
    });

    // HEP-CORE-0021: create ZMQ PULL socket when broker reported data_transport=="zmq".
    const std::string &dt = raw->handle.data_transport();
    if (dt == "zmq")
    {
        const std::string &ep = raw->handle.zmq_node_endpoint();
        if (ep.empty())
        {
            LOGGER_ERROR("[consumer] data_transport='zmq' but zmq_node_endpoint from broker is empty");
            return std::nullopt;
        }
        // PUSH binds → PULL connects (bind=false).
        // Derive 8-byte schema tag from the first 8 bytes of expected_schema_hash (binary).
        std::optional<std::array<uint8_t, 8>> schema_tag;
        if (opts.expected_schema_hash.size() >= 8)
        {
            std::array<uint8_t, 8> tag{};
            std::memcpy(tag.data(), opts.expected_schema_hash.data(), 8);
            schema_tag = tag;
        }
        impl->zmq_queue_ = ZmqQueue::pull_from(ep, opts.zmq_schema, opts.zmq_packing,
                                                /*bind=*/false, opts.zmq_buffer_depth, schema_tag);
        if (!impl->zmq_queue_)
        {
            return std::nullopt; // Error already logged by factory (empty/invalid schema, etc.)
        }
        if (!impl->zmq_queue_->start()) // [ZQ2] Check return value — connect may fail.
        {
            LOGGER_ERROR("[consumer] ZMQ PULL socket start() failed for '{}'", ep);
            return std::nullopt;
        }
        LOGGER_INFO("[consumer] ZMQ PULL socket connected to '{}'", ep);
    }

    // ctrl_queue_ is fire_and_forget (ZMQ always accepts sends); no monitoring callbacks needed.
    // Always apply opts.ctrl_queue_max_depth (0 = unbounded, >0 = drop-oldest cap).
    {
        MonitoredQueue<PendingCtrlSend>::Config qcfg;
        qcfg.max_depth    = opts.ctrl_queue_max_depth;
        impl->ctrl_queue_ = MonitoredQueue<PendingCtrlSend>(qcfg);
    }
    impl->peer_dead_timeout_ms_ = opts.peer_dead_timeout_ms;

    // Queue abstraction Phase 2: create unified QueueReader.
    // SHM: wrap DataBlockConsumer in ShmQueue. ZMQ: use existing zmq_queue_.
    if (impl->shm && opts.item_size > 0)
    {
        impl->shm_queue_reader_ = ShmQueue::from_consumer_ref(
            *impl->shm, opts.item_size, opts.flexzone_size, opts.channel_name,
            false, false);  // checksum flags: set via set_checksum_policy() below
        impl->queue_reader_ = impl->shm_queue_reader_.get();
    }
    else if (impl->zmq_queue_)
    {
        impl->queue_reader_ = impl->zmq_queue_.get();
    }

    // Set timing on the queue (single path for both SHM and ZMQ).
    if (impl->queue_reader_ && opts.timing.period_us > 0)
    {
        impl->queue_reader_->set_configured_period(opts.timing.period_us);
    }

    // Set checksum policy on the queue (single path for both SHM and ZMQ).
    if (impl->queue_reader_)
    {
        impl->queue_reader_->set_checksum_policy(opts.checksum_policy);
        impl->queue_reader_->set_flexzone_checksum(opts.flexzone_checksum);
    }

    return Consumer(std::move(impl));
}

// ============================================================================
// Consumer — callback registration
// ============================================================================

void Consumer::on_zmq_data(DataCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_zmq_data_cb = std::move(cb);
    }
}

void Consumer::on_producer_message(CtrlCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_producer_message_cb = std::move(cb);
    }
}

void Consumer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Consumer::on_force_shutdown(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_force_shutdown_cb = std::move(cb);
    }
}

void Consumer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_channel_error_cb = std::move(cb);
    }
}

// ============================================================================
// Consumer — active mode
// ============================================================================

bool Consumer::start()
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.exchange(true, std::memory_order_acq_rel))
    {
        return false; // Already running
    }
    pImpl->stop_requested_.store(false, std::memory_order_relaxed); // [PC2]

    // data_thread handles the data socket (SUB/PULL for PubSub/Pipeline; not for Bidir)
    auto *impl_ptr = pImpl.get();
    if (channel_handle_data_socket(pImpl->handle))
    {
        pImpl->data_thread_handle = std::thread([impl_ptr] { impl_ptr->run_data_thread(); });
    }

    // ctrl_thread handles the ctrl socket (DEALER for all patterns)
    pImpl->ctrl_thread_handle = std::thread([impl_ptr] { impl_ptr->run_ctrl_thread(); });

    // shm_thread only when SHM is attached
    if (pImpl->shm)
    {
        pImpl->shm_thread_handle = std::thread([impl_ptr] { impl_ptr->run_shm_thread(); });
    }

    return true;
}

void Consumer::stop()
{
    if (!pImpl)
    {
        return;
    }
    if (!pImpl->running.exchange(false, std::memory_order_acq_rel))
    {
        return; // Was not running
    }
    pImpl->stop_requested_.store(true, std::memory_order_relaxed); // [PC2]

    // Wake shm_thread if it is sleeping in Queue-mode idle wait.
    // Without this notify, shm_thread would sleep indefinitely until the CV timeout.
    pImpl->m_handler_cv.notify_all();

    if (pImpl->data_thread_handle.joinable())
    {
        pImpl->data_thread_handle.join();
    }
    if (pImpl->ctrl_thread_handle.joinable())
    {
        pImpl->ctrl_thread_handle.join();
    }
    if (pImpl->shm_thread_handle.joinable())
    {
        pImpl->shm_thread_handle.join();
    }

    // Stop ZMQ PULL socket after threads join.
    if (pImpl->zmq_queue_)
    {
        pImpl->zmq_queue_->stop();
        pImpl->zmq_queue_.reset(); // [PC3 analog] Null after stop.
    }
}

bool Consumer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
}

// ============================================================================
// Consumer — embedded mode
// ============================================================================

bool Consumer::start_embedded() noexcept
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    bool expected = false;
    // CAS: only transitions running false→true; returns false if already running.
    // Does NOT launch data_thread, ctrl_thread, or shm_thread.
    return pImpl->running.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void *Consumer::data_zmq_socket_handle() const noexcept
{
    if (!pImpl || pImpl->closed)
    {
        return nullptr;
    }
    zmq::socket_t *sock = channel_handle_data_socket(pImpl->handle);
    return sock ? sock->handle() : nullptr;
}

void *Consumer::ctrl_zmq_socket_handle() const noexcept
{
    if (!pImpl || pImpl->closed)
    {
        return nullptr;
    }
    zmq::socket_t *sock = channel_handle_ctrl_socket(pImpl->handle);
    return sock ? sock->handle() : nullptr;
}

void Consumer::handle_data_events_nowait() noexcept
{
    if (!pImpl || pImpl->closed || !pImpl->running.load(std::memory_order_relaxed))
    {
        return;
    }
    static constexpr int kMaxRecvBatch = 100;
    int n = 0;
    while (pImpl->recv_and_dispatch_data_() && ++n < kMaxRecvBatch) {}
    if (n >= kMaxRecvBatch)
        LOGGER_WARN("Consumer: handle_data_events_nowait hit recv batch cap ({})", n);
}

void Consumer::handle_ctrl_events_nowait() noexcept
{
    if (!pImpl || pImpl->closed || !pImpl->running.load(std::memory_order_relaxed))
    {
        return;
    }
    pImpl->drain_ctrl_send_queue_();
    static constexpr int kMaxRecvBatch = 100;
    int n = 0;
    while (pImpl->recv_and_dispatch_ctrl_() && ++n < kMaxRecvBatch) {}
    if (n >= kMaxRecvBatch)
        LOGGER_WARN("Consumer: handle_ctrl_events_nowait hit recv batch cap ({})", n);

    // Peer-dead check (embedded mode): mirrors the check in run_ctrl_thread().
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
                LOGGER_WARN("[hub::Consumer] No peer message for {}ms (timeout={}ms); "
                            "declaring peer dead.", gap_ms, pImpl->peer_dead_timeout_ms_);
                dead_cb();
                pImpl->peer_ever_seen_ = false; // Prevent repeated firing
            }
        }
    }
}

// ============================================================================
// Consumer — ZMQ messaging (to producer)
// ============================================================================

bool Consumer::send(const void *data, size_t size)
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.load(std::memory_order_relaxed))
    {
        // ctrl_thread owns the DEALER socket; queue with is_raw=true to preserve ZMQ thread safety.
        std::vector<std::byte> buf(size);
        if (data != nullptr && size > 0)
        {
            std::memcpy(buf.data(), data, size);
        }
        pImpl->ctrl_queue_.push({"", std::move(buf), true});
        return true;
    }
    // Not started — caller owns the socket; send directly.
    return pImpl->handle.send(data, size);
}

bool Consumer::send_ctrl(std::string_view type, const void *data, size_t size)
{
    if (!pImpl || !pImpl->handle.is_valid() || pImpl->closed)
    {
        return false;
    }
    if (pImpl->running.load(std::memory_order_relaxed))
    {
        // ctrl_thread owns the DEALER socket; queue the send to keep ZMQ thread safety.
        std::vector<std::byte> buf(size);
        if (data != nullptr && size > 0)
        {
            std::memcpy(buf.data(), data, size);
        }
        pImpl->ctrl_queue_.push({std::string(type), std::move(buf)});
        return true;
    }
    // Not started — caller owns the socket; send directly.
    return pImpl->handle.send_typed_ctrl(type, data, size);
}

// ============================================================================
// Consumer — peer-dead callback + metrics
// ============================================================================

void Consumer::on_peer_dead(std::function<void()> cb)
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
        pImpl->on_peer_dead_cb = std::move(cb);
    }
}

uint64_t Consumer::ctrl_queue_dropped() const
{
    return pImpl ? pImpl->ctrl_queue_.total_dropped() : 0;
}

// ============================================================================
// Consumer — non-template helpers for template method implementations
// ============================================================================

bool Consumer::_has_shm() const noexcept
{
    return pImpl && !pImpl->closed && pImpl->shm != nullptr;
}

ConsumerMessagingFacade &Consumer::_messaging_facade() const
{
    assert(pImpl);
    return pImpl->facade;
}

void Consumer::_store_read_handler(std::shared_ptr<InternalReadHandlerFn> h) noexcept
{
    if (pImpl)
    {
        pImpl->m_read_handler.store(std::move(h), std::memory_order_release);
        // Wake shm_thread so it transitions from Queue-mode idle to RealTime (or vice versa).
        pImpl->m_handler_cv.notify_all();
    }
}

bool Consumer::is_stopping() const noexcept
{
    // [PC2] Use stop_requested_ (defaults false before start()) instead of !running,
    // which was true before start() causing is_stopping() to incorrectly return true.
    return pImpl && pImpl->stop_requested_.load(std::memory_order_relaxed);
}

bool Consumer::has_realtime_handler() const noexcept
{
    return pImpl && pImpl->m_read_handler.load(std::memory_order_relaxed) != nullptr;
}

Messenger &Consumer::messenger() const
{
    assert(pImpl && pImpl->messenger);
    return *pImpl->messenger;
}

// ============================================================================
// Consumer — introspection
// ============================================================================

bool Consumer::is_valid() const
{
    return pImpl && !pImpl->closed && pImpl->handle.is_valid();
}

const std::string &Consumer::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->handle.channel_name() : kEmpty;
}

ChannelPattern Consumer::pattern() const
{
    return pImpl ? pImpl->handle.pattern() : ChannelPattern::PubSub;
}

bool Consumer::has_shm() const
{
    return pImpl && pImpl->shm != nullptr;
}

DataBlockConsumer *Consumer::shm() noexcept
{
    return pImpl ? pImpl->shm.get() : nullptr;
}

ChannelHandle &Consumer::channel_handle()
{
    assert(pImpl);
    return pImpl->handle;
}

const std::string &Consumer::data_transport() const noexcept
{
    static const std::string kShm{"shm"};
    return pImpl ? pImpl->handle.data_transport() : kShm;
}

const std::string &Consumer::zmq_node_endpoint() const noexcept
{
    static const std::string kEmpty;
    return pImpl ? pImpl->handle.zmq_node_endpoint() : kEmpty;
}

ZmqQueue *Consumer::queue() noexcept
{
    // zmq_queue_ is stored as QueueReader; runtime type is always ZmqQueue (from pull_from).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return pImpl ? static_cast<ZmqQueue *>(pImpl->zmq_queue_.get()) : nullptr;
}

// ============================================================================
// Consumer — Queue data operations (forwarded to internal QueueReader)
// ============================================================================

const void *Consumer::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    auto *q = pImpl ? pImpl->queue_reader_ : nullptr;
    return q ? q->read_acquire(timeout) : nullptr;
}

void Consumer::read_release() noexcept
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->read_release();
}

uint64_t Consumer::last_seq() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->last_seq() : 0;
}

size_t Consumer::queue_item_size() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->item_size() : 0;
}

size_t Consumer::queue_capacity() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->capacity() : 0;
}

QueueMetrics Consumer::queue_metrics() const noexcept
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->metrics() : QueueMetrics{};
}

void Consumer::reset_queue_metrics() noexcept
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->init_metrics();
}

bool Consumer::start_queue()
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->start() : false;
}

void Consumer::stop_queue()
{
    if (pImpl && pImpl->queue_reader_)
        pImpl->queue_reader_->stop();
}

// ============================================================================
// Consumer — Channel data operations (flexzone, checksum)
// ============================================================================

const void *Consumer::read_flexzone() const noexcept
{
    if (!pImpl || !pImpl->shm) return nullptr;
    auto fz = pImpl->shm->flexible_zone_span();
    return fz.empty() ? nullptr : fz.data();
}

size_t Consumer::flexzone_size() const noexcept
{
    if (!pImpl || !pImpl->shm) return 0;
    return pImpl->shm->flexible_zone_span().size();
}

void Consumer::set_verify_checksum(bool slot, bool fz) noexcept
{
    auto *sq = pImpl ? static_cast<ShmQueue *>(pImpl->shm_queue_reader_.get()) : nullptr;
    if (sq) sq->set_verify_checksum(slot, fz);
}

std::string Consumer::queue_policy_info() const
{
    return (pImpl && pImpl->queue_reader_) ? pImpl->queue_reader_->policy_info() : std::string{};
}

// ============================================================================
// Consumer::close — idempotent teardown
// ============================================================================

void Consumer::close()
{
    if (!pImpl || pImpl->closed)
    {
        return;
    }

    // Stop all threads FIRST — ctrl_thread must exit before we send on the ctrl socket.
    // Sending BYE before stop() would race with ctrl_thread's ZMQ polling (UB).
    stop();

    // Clear per-channel Messenger callbacks BEFORE deregistering to prevent re-entrant
    // invocation during the CONSUMER_DEREG_REQ/ACK exchange.
    if (pImpl->messenger && pImpl->handle.is_valid())
    {
        const std::string &ch = pImpl->handle.channel_name();
        // Clear Messenger-registered lambdas first so no new invocations can start.
        pImpl->messenger->on_channel_closing(ch, nullptr);
        pImpl->messenger->on_force_shutdown(ch, nullptr);
        pImpl->messenger->on_channel_error(ch, nullptr);
        // Null user callbacks under the guard used by Messenger lambdas. [PC1]
        {
            std::lock_guard<std::mutex> lk(pImpl->callbacks_mu);
            pImpl->on_channel_closing_cb = nullptr;
            pImpl->on_force_shutdown_cb  = nullptr;
            pImpl->on_channel_error_cb   = nullptr;
        }
        // Deregister from broker (fire-and-forget via Messenger worker thread).
        pImpl->messenger->deregister_consumer(ch);
    }

    // Clear ctrl/data-thread and peer-dead callbacks. ctrl_thread, data_thread, and
    // shm_thread have all been joined by stop() above, so no concurrent reads exist.
    // Releasing captures here prevents unnecessary lifetime extension of any objects
    // captured by these callbacks (e.g. script host references).
    pImpl->on_zmq_data_cb         = nullptr;
    pImpl->on_producer_message_cb = nullptr;
    pImpl->on_peer_dead_cb        = nullptr;

    // Now the ctrl socket is owned by this thread (ctrl_thread has exited).
    // Send BYE so the producer's peer_thread can update its consumer list.
    if (pImpl->handle.is_valid())
    {
        const char kEmpty = '\0';
        (void)pImpl->handle.send_typed_ctrl("BYE", &kEmpty, 0);
    }

    pImpl->handle.invalidate();
    // Reset queue abstraction before shm — ShmQueue holds non-owning ref to DataBlock.
    pImpl->queue_reader_ = nullptr;
    pImpl->shm_queue_reader_.reset();
    pImpl->zmq_queue_.reset();
    pImpl->shm.reset();
    pImpl->closed = true;
}

// ============================================================================
// ManagedConsumer — static registry
// ============================================================================

namespace
{
std::mutex                                           g_consumer_registry_mu;
std::unordered_map<std::string, ManagedConsumer *>   g_consumer_registry;
} // namespace

ManagedConsumer::ManagedConsumer(Messenger &messenger, ConsumerOptions opts)
    : messenger_(&messenger)
    , opts_(std::move(opts))
    , module_key_("pylabhub::hub::Consumer::" + opts_.channel_name)
{
}

ManagedConsumer::~ManagedConsumer()
{
    if (consumer_.has_value())
    {
        consumer_->close();
    }
    std::lock_guard<std::mutex> lock(g_consumer_registry_mu);
    g_consumer_registry.erase(module_key_);
}

// NOTE: move is safe only BEFORE get_module_def() registers `this` in g_consumer_registry.
// After get_module_def(), the registry holds the old `this` pointer. See ManagedProducer
// for the full explanation and intended usage pattern.
ManagedConsumer::ManagedConsumer(ManagedConsumer &&) noexcept            = default;
ManagedConsumer &ManagedConsumer::operator=(ManagedConsumer &&) noexcept = default;

pylabhub::utils::ModuleDef ManagedConsumer::get_module_def()
{
    {
        std::lock_guard<std::mutex> lock(g_consumer_registry_mu);
        g_consumer_registry[module_key_] = this;
    }

    pylabhub::utils::ModuleDef def(module_key_);
    def.add_dependency("pylabhub::hub::DataExchangeHub");
    def.set_startup(&ManagedConsumer::s_startup, module_key_);
    def.set_shutdown(&ManagedConsumer::s_shutdown, kManagedConsumerShutdownMs,
                     module_key_);
    return def;
}

Consumer &ManagedConsumer::get()
{
    if (!consumer_.has_value())
    {
        throw std::runtime_error(
            "ManagedConsumer::get(): not initialized (lifecycle not started?)");
    }
    return *consumer_;
}

bool ManagedConsumer::is_initialized() const noexcept
{
    return consumer_.has_value();
}

void ManagedConsumer::s_startup(const char *key)
{
    ManagedConsumer *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_consumer_registry_mu);
        auto it = g_consumer_registry.find(key);
        if (it == g_consumer_registry.end())
        {
            return;
        }
        self = it->second;
    }

    auto c = Consumer::connect(*self->messenger_, self->opts_);
    if (!c.has_value())
    {
        throw std::runtime_error(
            std::string("ManagedConsumer: failed to connect to channel: ") +
            self->opts_.channel_name);
    }
    c->start();
    self->consumer_ = std::move(c);
}

void ManagedConsumer::s_shutdown(const char *key)
{
    ManagedConsumer *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_consumer_registry_mu);
        auto it = g_consumer_registry.find(key);
        if (it == g_consumer_registry.end())
        {
            return;
        }
        self = it->second;
    }

    if (self->consumer_.has_value())
    {
        self->consumer_->stop();
        self->consumer_->close();
    }
}

} // namespace pylabhub::hub
