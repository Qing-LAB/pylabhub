// src/utils/hub_consumer.cpp
#include "utils/hub_consumer.hpp"
#include "channel_handle_internals.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{
constexpr int kCtrlPollIntervalMs = 50;  ///< ZMQ poll timeout in ctrl_thread loop
constexpr int kDataPollIntervalMs = 50;  ///< ZMQ poll timeout in data_thread loop

/// Outgoing ctrl message queued for ctrl_thread to send on the DEALER socket.
/// Consumer sends only one direction (no identity needed — DEALER routes automatically).
struct PendingConsumerCtrlSend
{
    std::string            type;
    std::vector<std::byte> data;
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
    Consumer::ChannelErrorCallback on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::thread       data_thread_handle;
    std::thread       ctrl_thread_handle;
    std::thread       shm_thread_handle;

    // Outgoing ctrl messages queued for ctrl_thread to send on the DEALER socket.
    // Prevents cross-thread ZMQ socket access when Consumer is started.
    mutable std::mutex                       ctrl_send_mu;
    std::queue<PendingConsumerCtrlSend>      ctrl_send_queue;

    // Real-time handler (RealTime mode): shm_thread calls this in a loop.
    // nullptr = Queue mode (default). Swapped atomically via _store_read_handler().
    std::atomic<std::shared_ptr<InternalReadHandlerFn>> m_read_handler{nullptr};

    // CV used to wake shm_thread from Queue-mode idle sleep.
    // Notified when: handler is installed, or stop() is called.
    std::mutex              m_handler_cv_mu;
    std::condition_variable m_handler_cv;

    // Messaging facade: filled by connect_from_parts(); used by ReadProcessorContext<F,D>.
    ConsumerMessagingFacade facade{};

    void run_data_thread();
    void run_ctrl_thread();
    void run_shm_thread();
};

// ============================================================================
// data_thread: polls SUB/PULL data socket for ZMQ data frames
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

        // Receive: [type_byte='A'][payload]
        std::vector<zmq::message_t> frames;
        try
        {
            auto res = zmq::recv_multipart(*data_sock, std::back_inserter(frames),
                                           zmq::recv_flags::dontwait);
            (void)res;
        }
        catch (const zmq::error_t &)
        {
            continue;
        }

        // Expect at least [type_byte, payload] = 2 frames
        if (frames.size() < 2)
        {
            continue;
        }

        if (frames[0].size() < 1)
        {
            continue;
        }
        const char type_byte = *static_cast<const char *>(frames[0].data());
        if (type_byte != 'A')
        {
            continue; // Not a data frame — ignore
        }

        if (on_zmq_data_cb)
        {
            std::span<const std::byte> payload(
                static_cast<const std::byte *>(frames[1].data()), frames[1].size());
            on_zmq_data_cb(payload);
        }
    }
}

// ============================================================================
// ctrl_thread: polls DEALER ctrl socket for control frames from producer
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
        // send_ctrl() from other threads queues here; ctrl_thread is the only
        // sender on this DEALER socket, keeping ZMQ thread ownership correct.
        {
            std::lock_guard<std::mutex> lock(ctrl_send_mu);
            while (!ctrl_send_queue.empty())
            {
                PendingConsumerCtrlSend &msg = ctrl_send_queue.front();
                handle.send_typed_ctrl(msg.type, msg.data.data(), msg.data.size());
                ctrl_send_queue.pop();
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

        // Receive from DEALER: [type_byte='C'][type_str][body]
        // (For Bidir, data frames ['A'][payload] may also arrive here)
        std::vector<zmq::message_t> frames;
        try
        {
            auto res = zmq::recv_multipart(*ctrl_sock, std::back_inserter(frames),
                                           zmq::recv_flags::dontwait);
            (void)res;
        }
        catch (const zmq::error_t &)
        {
            continue;
        }

        if (frames.size() < 2)
        {
            continue;
        }

        if (frames[0].size() < 1)
        {
            continue;
        }
        const char type_byte = *static_cast<const char *>(frames[0].data());

        if (type_byte == 'A')
        {
            // Data frame received on ctrl socket (Bidir pattern)
            if (on_zmq_data_cb && frames.size() >= 2)
            {
                std::span<const std::byte> payload(
                    static_cast<const std::byte *>(frames[1].data()), frames[1].size());
                on_zmq_data_cb(payload);
            }
            continue;
        }

        if (type_byte != 'C')
        {
            continue; // Unknown frame type
        }

        // Control frame: [type_byte='C'][type_str][body]
        if (frames.size() < 3)
        {
            continue;
        }

        std::string_view type_str(static_cast<const char *>(frames[1].data()),
                                   frames[1].size());
        std::span<const std::byte> body(static_cast<const std::byte *>(frames[2].data()),
                                         frames[2].size());

        if (on_producer_message_cb)
        {
            on_producer_message_cb(type_str, body);
        }
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
    auto ch = messenger.connect_channel(opts.channel_name, opts.timeout_ms,
                                         opts.expected_schema_hash);
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

    return Consumer::connect_from_parts(messenger, std::move(*ch), std::move(shm_consumer),
                                         opts);
}

// ============================================================================
// Consumer::connect_from_parts — assembles a Consumer from pre-built parts
// ============================================================================

std::optional<Consumer>
Consumer::connect_from_parts(Messenger &messenger, ChannelHandle channel,
                               std::unique_ptr<DataBlockConsumer> shm_consumer,
                               const ConsumerOptions & /*opts*/)
{
    auto impl       = std::make_unique<ConsumerImpl>();
    impl->handle    = std::move(channel);
    impl->shm       = std::move(shm_consumer);
    impl->messenger = &messenger;
    impl->closed    = false;

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
            std::lock_guard<std::mutex> lock(c->ctrl_send_mu);
            c->ctrl_send_queue.push({std::string(type), std::move(buf)});
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

    messenger.on_channel_closing(ch, [raw]() {
        if (!raw->closed && raw->on_channel_closing_cb)
            raw->on_channel_closing_cb();
    });

    messenger.on_channel_error(ch, [raw](std::string event, nlohmann::json details) {
        if (!raw->closed && raw->on_channel_error_cb)
            raw->on_channel_error_cb(event, details);
    });

    return Consumer(std::move(impl));
}

// ============================================================================
// Consumer — callback registration
// ============================================================================

void Consumer::on_zmq_data(DataCallback cb)
{
    if (pImpl)
    {
        pImpl->on_zmq_data_cb = std::move(cb);
    }
}

void Consumer::on_producer_message(CtrlCallback cb)
{
    if (pImpl)
    {
        pImpl->on_producer_message_cb = std::move(cb);
    }
}

void Consumer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Consumer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
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
    if (pImpl->running.exchange(true, std::memory_order_acquire))
    {
        return false; // Already running
    }

    // data_thread handles the data socket (SUB/PULL for PubSub/Pipeline; not for Bidir)
    if (channel_handle_data_socket(pImpl->handle))
    {
        pImpl->data_thread_handle = std::thread([this] { pImpl->run_data_thread(); });
    }

    // ctrl_thread handles the ctrl socket (DEALER for all patterns)
    pImpl->ctrl_thread_handle = std::thread([this] { pImpl->run_ctrl_thread(); });

    // shm_thread only when SHM is attached
    if (pImpl->shm)
    {
        pImpl->shm_thread_handle = std::thread([this] { pImpl->run_shm_thread(); });
    }

    return true;
}

void Consumer::stop()
{
    if (!pImpl)
    {
        return;
    }
    if (!pImpl->running.exchange(false, std::memory_order_acquire))
    {
        return; // Was not running
    }

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
}

bool Consumer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
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
    // Consumer sends data via the ctrl (DEALER) socket
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
        std::lock_guard<std::mutex> lock(pImpl->ctrl_send_mu);
        pImpl->ctrl_send_queue.push({std::string(type), std::move(buf)});
        return true;
    }
    // Not started — caller owns the socket; send directly.
    return pImpl->handle.send_typed_ctrl(type, data, size);
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
    return pImpl && !pImpl->running.load(std::memory_order_relaxed);
}

ShmProcessingMode Consumer::shm_processing_mode() const noexcept
{
    if (pImpl && pImpl->m_read_handler.load(std::memory_order_relaxed) != nullptr)
        return ShmProcessingMode::RealTime;
    return ShmProcessingMode::Queue;
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
        pImpl->messenger->on_channel_closing(ch, nullptr);
        pImpl->messenger->on_channel_error(ch, nullptr);
        // Deregister from broker (fire-and-forget via Messenger worker thread).
        pImpl->messenger->deregister_consumer(ch);
    }

    // Now the ctrl socket is owned by this thread (ctrl_thread has exited).
    // Send BYE so the producer's peer_thread can update its consumer list.
    if (pImpl->handle.is_valid())
    {
        const char kEmpty = '\0';
        (void)pImpl->handle.send_typed_ctrl("BYE", &kEmpty, 0);
    }

    pImpl->handle.invalidate();
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
    def.set_shutdown(&ManagedConsumer::s_shutdown, std::chrono::milliseconds(10000),
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
