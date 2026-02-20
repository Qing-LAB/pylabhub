// src/utils/hub_producer.cpp
#include "utils/hub_producer.hpp"
#include "channel_handle_internals.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
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
constexpr int kPeerPollIntervalMs  = 50;  ///< ZMQ poll timeout in peer_thread loop
constexpr int kWriteSlotTimeoutMs  = 5000; ///< Timeout for acquiring a write slot
} // namespace

// ============================================================================
// PendingCtrlSend — queued outgoing message for peer_thread to send
// ============================================================================

struct PendingCtrlSend
{
    std::string            identity; ///< ZMQ identity of the target consumer
    std::string            type;     ///< Ctrl type string (ignored for data frames)
    std::vector<std::byte> data;
    bool                   is_data{false}; ///< true = data frame; false = ctrl frame
};

// ============================================================================
// ProducerImpl — internal state (defined in .cpp for pImpl idiom)
// ============================================================================

struct ProducerImpl
{
    ChannelHandle                      handle;
    std::unique_ptr<DataBlockProducer> shm;
    Messenger                         *messenger{nullptr};
    std::atomic<bool>                  closed{false};

    // Consumer identity tracking (updated exclusively from peer_thread, read under lock)
    mutable std::mutex       consumer_list_mu;
    std::vector<std::string> consumer_identities;
    /// Maps consumer_pid → ZMQ identity (populated from HELLO body consumer_pid field).
    std::unordered_map<uint64_t, std::string> pid_to_identity;

    // User callbacks (called from the Messenger worker thread or peer_thread)
    Producer::ConsumerCallback    on_consumer_joined_cb;
    Producer::ConsumerCallback    on_consumer_left_cb;
    Producer::MessageCallback     on_consumer_message_cb;
    std::function<void()>         on_channel_closing_cb;
    Producer::ConsumerDiedCallback on_consumer_died_cb;
    Producer::ChannelErrorCallback on_channel_error_cb;

    // Active mode
    std::atomic<bool> running{false};
    std::thread       peer_thread_handle;
    std::thread       write_thread_handle;

    // Outgoing ctrl/data messages queued for peer_thread to send on the ROUTER socket
    std::mutex                   ctrl_send_mu;
    std::queue<PendingCtrlSend>  ctrl_send_queue;

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
    std::atomic<std::shared_ptr<InternalWriteHandlerFn>> m_write_handler{nullptr};

    // Messaging facade: filled by create_from_parts(); used by WriteProcessorContext<F,D>.
    ProducerMessagingFacade facade{};

    void run_peer_thread();
    void run_write_thread();
};

// ============================================================================
// peer_thread: polls ROUTER ctrl socket, drains outgoing queue
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
        {
            std::lock_guard<std::mutex> lock(ctrl_send_mu);
            while (!ctrl_send_queue.empty())
            {
                PendingCtrlSend &msg = ctrl_send_queue.front();
                if (msg.is_data)
                {
                    handle.send(msg.data.data(), msg.data.size(), msg.identity);
                }
                else
                {
                    handle.send_typed_ctrl(msg.type, msg.data.data(), msg.data.size(),
                                           msg.identity);
                }
                ctrl_send_queue.pop();
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

        // ── 3. Receive multipart message from ROUTER ────────────────────────
        // Expected layout: [identity][type_byte='C'][type_str][body]
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

        // Need at least: identity + type_byte + type_str + body = 4 frames
        if (frames.size() < 4)
        {
            continue;
        }

        if (frames[1].size() < 1)
        {
            continue;
        }
        const char type_byte = *static_cast<const char *>(frames[1].data());
        if (type_byte != 'C')
        {
            continue; // Not a control frame — ignore data frames on ctrl socket
        }

        std::string identity(static_cast<const char *>(frames[0].data()), frames[0].size());
        std::string type_str(static_cast<const char *>(frames[2].data()), frames[2].size());
        std::span<const std::byte> body(static_cast<const std::byte *>(frames[3].data()),
                                         frames[3].size());

        // ── 4. Dispatch HELLO / BYE / other ────────────────────────────────
        if (type_str == "HELLO")
        {
            // Parse consumer_pid from HELLO body (consumer includes it in JSON).
            uint64_t cpid = 0;
            try
            {
                std::string body_str(static_cast<const char *>(frames[3].data()),
                                     frames[3].size());
                if (!body_str.empty())
                {
                    auto body_json = nlohmann::json::parse(body_str);
                    cpid = body_json.value("consumer_pid", uint64_t{0});
                }
            }
            catch (...) {}

            {
                std::lock_guard<std::mutex> lock(consumer_list_mu);
                auto it = std::find(consumer_identities.begin(), consumer_identities.end(),
                                    identity);
                if (it == consumer_identities.end())
                {
                    consumer_identities.push_back(identity);
                    if (cpid != 0)
                    {
                        pid_to_identity[cpid] = identity;
                    }
                }
            }
            if (on_consumer_joined_cb)
            {
                on_consumer_joined_cb(identity);
            }
        }
        else if (type_str == "BYE")
        {
            {
                std::lock_guard<std::mutex> lock(consumer_list_mu);
                auto it = std::find(consumer_identities.begin(), consumer_identities.end(),
                                    identity);
                if (it != consumer_identities.end())
                {
                    consumer_identities.erase(it);
                }
                // Remove the corresponding pid_to_identity entry (find by identity value).
                for (auto map_it = pid_to_identity.begin(); map_it != pid_to_identity.end();)
                {
                    if (map_it->second == identity)
                        map_it = pid_to_identity.erase(map_it);
                    else
                        ++map_it;
                }
            }
            if (on_consumer_left_cb)
            {
                on_consumer_left_cb(identity);
            }
        }
        else
        {
            if (on_consumer_message_cb)
            {
                on_consumer_message_cb(identity, body);
            }
        }
    }

    // Drain remaining outgoing messages before exit
    {
        std::lock_guard<std::mutex> lock(ctrl_send_mu);
        while (!ctrl_send_queue.empty())
        {
            ctrl_send_queue.pop();
        }
    }
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
    auto ch = messenger.create_channel(opts.channel_name, opts.pattern, opts.has_shm,
                                        opts.schema_hash, opts.schema_version, opts.timeout_ms);
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
                              const ProducerOptions & /*opts*/)
{
    auto impl       = std::make_unique<ProducerImpl>();
    impl->handle    = std::move(channel);
    impl->shm       = std::move(shm_producer);
    impl->messenger = &messenger;
    impl->closed    = false;

    // Fill the messaging facade. Function pointers capture nothing except `ctx` (the
    // heap-stable ProducerImpl*). The facade itself lives inside ProducerImpl, so
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
            std::lock_guard<std::mutex> lock(p->ctrl_send_mu);
            p->ctrl_send_queue.push(
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

    messenger.on_channel_closing(ch, [raw]() {
        if (!raw->closed && raw->on_channel_closing_cb)
            raw->on_channel_closing_cb();
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
        if (raw->on_consumer_died_cb)
            raw->on_consumer_died_cb(pid, reason);
    });

    messenger.on_channel_error(ch, [raw](std::string event, nlohmann::json details) {
        if (!raw->closed && raw->on_channel_error_cb)
            raw->on_channel_error_cb(event, details);
    });

    return Producer(std::move(impl));
}

// ============================================================================
// Producer — callback registration
// ============================================================================

void Producer::on_consumer_joined(ConsumerCallback cb)
{
    if (pImpl)
    {
        pImpl->on_consumer_joined_cb = std::move(cb);
    }
}

void Producer::on_consumer_left(ConsumerCallback cb)
{
    if (pImpl)
    {
        pImpl->on_consumer_left_cb = std::move(cb);
    }
}

void Producer::on_consumer_message(MessageCallback cb)
{
    if (pImpl)
    {
        pImpl->on_consumer_message_cb = std::move(cb);
    }
}

void Producer::on_channel_closing(std::function<void()> cb)
{
    if (pImpl)
    {
        pImpl->on_channel_closing_cb = std::move(cb);
    }
}

void Producer::on_consumer_died(ConsumerDiedCallback cb)
{
    if (pImpl)
    {
        pImpl->on_consumer_died_cb = std::move(cb);
    }
}

void Producer::on_channel_error(ChannelErrorCallback cb)
{
    if (pImpl)
    {
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
    if (pImpl->running.exchange(true, std::memory_order_acquire))
    {
        return false; // Already running
    }

    pImpl->write_stop.store(false, std::memory_order_relaxed);

    pImpl->peer_thread_handle = std::thread([this] { pImpl->run_peer_thread(); });

    if (pImpl->shm)
    {
        pImpl->write_thread_handle = std::thread([this] { pImpl->run_write_thread(); });
    }

    return true;
}

void Producer::stop()
{
    if (!pImpl)
    {
        return;
    }

    if (!pImpl->running.exchange(false, std::memory_order_acquire))
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
}

bool Producer::is_running() const noexcept
{
    return pImpl && pImpl->running.load(std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(pImpl->ctrl_send_mu);
        pImpl->ctrl_send_queue.push(
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
        std::lock_guard<std::mutex> lock(pImpl->ctrl_send_mu);
        pImpl->ctrl_send_queue.push(
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

ShmProcessingMode Producer::shm_processing_mode() const noexcept
{
    if (pImpl && pImpl->m_write_handler.load(std::memory_order_relaxed) != nullptr)
        return ShmProcessingMode::RealTime;
    return ShmProcessingMode::Queue;
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

ChannelHandle &Producer::channel_handle()
{
    assert(pImpl);
    return pImpl->handle;
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
        pImpl->messenger->on_consumer_died(ch, nullptr);
        pImpl->messenger->on_channel_error(ch, nullptr);
        pImpl->messenger->unregister_channel(ch);
    }

    pImpl->handle.invalidate();
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
    def.set_shutdown(&ManagedProducer::s_shutdown, std::chrono::milliseconds(10000),
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
