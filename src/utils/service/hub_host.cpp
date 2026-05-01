/**
 * @file hub_host.cpp
 * @brief HubHost — lifecycle owner (HEP-CORE-0033 §4).
 */
#include "utils/hub_host.hpp"

#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/thread_manager.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>

namespace pylabhub::hub_host
{

// ============================================================================
// Impl
// ============================================================================

struct HubHost::Impl
{
    explicit Impl(config::HubConfig c)
        : cfg(std::move(c))
    {}

    config::HubConfig                          cfg;
    hub::HubState                              state;        // owned by HubHost
    std::unique_ptr<broker::BrokerService>     broker;       // built in startup()
    std::unique_ptr<utils::ThreadManager>      thread_mgr;   // built in startup()

    std::atomic<bool>                          shutdown_flag{false};
    std::atomic<bool>                          running{false};

    // Bound endpoint + pubkey, populated when the broker fires
    // `on_ready` (post-bind, pre-poll-loop).  startup() blocks on the
    // ready future so the accessors below are populated by the time
    // it returns.  Survive shutdown for diagnostics.
    std::string                                bound_endpoint;
    std::string                                bound_pubkey;

    // Wake-up coordination for run_main_loop().
    std::mutex                                 wake_mu;
    std::condition_variable                    wake_cv;
};

// ============================================================================
// Special members
// ============================================================================

HubHost::HubHost(config::HubConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg)))
{
}

HubHost::~HubHost()
{
    // Idempotent shutdown if the caller didn't run it explicitly.
    if (impl_ && impl_->running.load(std::memory_order_acquire))
    {
        try
        {
            shutdown();
        }
        catch (...)
        {
            // Destructor must not throw — swallow; ThreadManager dtor
            // logs detached threads if any.
        }
    }
    // impl_ destructor runs:
    //   1. thread_mgr_ dtor → drains tracked threads (bounded join)
    //   2. broker_ dtor (broker already stopped above)
    //   3. state dtor
    //   4. cfg dtor
}

// ============================================================================
// Lifecycle
// ============================================================================

void HubHost::startup()
{
    if (impl_->running.load(std::memory_order_acquire))
        return; // idempotent: already running

    // Build BrokerService::Config from HubConfig.
    broker::BrokerService::Config bcfg;
    bcfg.endpoint = impl_->cfg.network().broker_endpoint;
    // CURVE: enabled if a keypair was loaded into the auth config
    // (caller did `cfg.load_keypair(password)` before constructing HubHost).
    bcfg.use_curve         = !impl_->cfg.auth().client_pubkey.empty();
    bcfg.server_secret_key = impl_->cfg.auth().client_seckey;
    bcfg.server_public_key = impl_->cfg.auth().client_pubkey;

    // Heartbeat-multiplier timeouts (HEP-CORE-0023 §2.5; HEP-0033 §6.4).
    const auto &hb = impl_->cfg.broker();
    bcfg.heartbeat_interval =
        std::chrono::milliseconds(hb.heartbeat_interval_ms);
    bcfg.ready_miss_heartbeats   = hb.ready_miss_heartbeats;
    bcfg.pending_miss_heartbeats = hb.pending_miss_heartbeats;
    bcfg.grace_heartbeats        = hb.grace_heartbeats;
    if (hb.ready_timeout_ms.has_value())
        bcfg.ready_timeout_override =
            std::chrono::milliseconds(*hb.ready_timeout_ms);
    if (hb.pending_timeout_ms.has_value())
        bcfg.pending_timeout_override =
            std::chrono::milliseconds(*hb.pending_timeout_ms);
    if (hb.grace_ms.has_value())
        bcfg.grace_override =
            std::chrono::milliseconds(*hb.grace_ms);

    // Federation peers (HEP-CORE-0022; HEP-0033 §6.4).
    bcfg.self_hub_uid = impl_->cfg.identity().uid;
    for (const auto &p : impl_->cfg.federation().peers)
    {
        broker::FederationPeer fp;
        fp.hub_uid         = p.uid;
        fp.broker_endpoint = p.endpoint;
        fp.pubkey_z85      = p.pubkey;
        // p.channels is currently absent in HubFederationConfig — see
        // HEP-CORE-0035; falls back to an empty relay list (no relay).
        bcfg.peers.push_back(std::move(fp));
    }

    // Wire on_ready so startup() can block until the broker has bound
    // and capture the actual endpoint / pubkey (broker_endpoint may be
    // an ephemeral port like `tcp://127.0.0.1:0`).  on_ready is
    // contracted to fire exactly once per BrokerService instance; if
    // it fires twice, set_value will throw `future_error` which we
    // surface as a startup failure (catches a real broker bug).
    auto ready_promise = std::make_shared<
        std::promise<std::pair<std::string, std::string>>>();
    auto ready_future  = ready_promise->get_future();
    bcfg.on_ready = [ready_promise](const std::string &ep,
                                     const std::string &pk)
    {
        ready_promise->set_value({ep, pk});
    };

    // Cleanup helper used by every failure branch below.  Must be
    // safe regardless of how far startup() got.
    auto rollback_partial_startup = [this]() noexcept {
        try
        {
            if (impl_->broker) impl_->broker->stop();
            if (impl_->thread_mgr) impl_->thread_mgr->drain();
        }
        catch (...) { /* swallow — already in error path */ }
        impl_->broker.reset();
        impl_->thread_mgr.reset();
        impl_->bound_endpoint.clear();
        impl_->bound_pubkey.clear();
        impl_->shutdown_flag.store(false, std::memory_order_release);
        impl_->running.store(false, std::memory_order_release);
    };

    try
    {
        // Construct broker bound to our HubState (HEP-0033 §4 ownership).
        impl_->broker = std::make_unique<broker::BrokerService>(
            std::move(bcfg), impl_->state);

        // ThreadManager auto-registers as a dynamic lifecycle module
        // "ThreadManager:HubHost:<uid>".  Owns the broker thread (and,
        // in Phase 6.2, the admin thread) for bounded-join shutdown.
        impl_->thread_mgr = std::make_unique<utils::ThreadManager>(
            "HubHost", impl_->cfg.identity().uid);

        // Spawn broker thread.  ThreadManager only joins; the broker's
        // internal stop atomic + ZMQ wake-up break the poll loop on
        // shutdown.
        auto *broker_ptr = impl_->broker.get();
        if (!impl_->thread_mgr->spawn("broker",
                                       [broker_ptr] { broker_ptr->run(); }))
        {
            throw std::runtime_error(
                "HubHost::startup: ThreadManager refused to spawn broker thread");
        }

        // Block until the broker fires on_ready, with a bounded
        // deadline.  A healthy broker binds in <100 ms; 5 s is
        // generous.  If the broker thread silently exits before
        // firing on_ready (binding failure with no exception
        // surfaced through this future), wait_for() reports timeout
        // and we treat it as startup failure rather than hanging
        // forever.  If the broker thread did throw, the future also
        // delivers that exception via get().
        const auto status =
            ready_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::timeout)
        {
            throw std::runtime_error(
                "HubHost::startup: broker did not signal ready within 5s "
                "(bind likely failed silently — check logs)");
        }
        auto info = ready_future.get(); // may throw if broker thread did
        impl_->bound_endpoint = info.first;
        impl_->bound_pubkey   = info.second;
    }
    catch (...)
    {
        rollback_partial_startup();
        throw;
    }

    impl_->running.store(true, std::memory_order_release);
    LOGGER_INFO("[HubHost:{}] startup complete (broker on {})",
                impl_->cfg.identity().uid, impl_->bound_endpoint);
}

void HubHost::run_main_loop()
{
    std::unique_lock<std::mutex> lk(impl_->wake_mu);
    impl_->wake_cv.wait(lk, [this] {
        return impl_->shutdown_flag.load(std::memory_order_acquire);
    });
    LOGGER_INFO("[HubHost:{}] main loop woke for shutdown",
                impl_->cfg.identity().uid);
}

void HubHost::shutdown()
{
    if (!impl_->running.load(std::memory_order_acquire))
        return; // idempotent: not running, nothing to stop

    LOGGER_INFO("[HubHost:{}] shutdown initiated",
                impl_->cfg.identity().uid);

    // Wake any thread blocked in run_main_loop().
    impl_->shutdown_flag.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(impl_->wake_mu);
    }
    impl_->wake_cv.notify_all();

    // Break the broker's poll loop.  Internal atomic flip + ZMQ
    // wake-up; broker thread exits run() shortly after.
    if (impl_->broker)
        impl_->broker->stop();

    // Synchronously drain the ThreadManager so the broker thread has
    // actually exited by the time shutdown() returns.  This is the
    // strong contract: after shutdown(), no protocol traffic is being
    // processed and the broker will reject new connections.  If we
    // only flipped the flags (delegating join to ~HubHost), in-flight
    // requests could still be serviced between shutdown() and dtor.
    if (impl_->thread_mgr)
    {
        const auto detached = impl_->thread_mgr->drain();
        if (detached > 0)
        {
            LOGGER_WARN("[HubHost:{}] shutdown: {} thread(s) exceeded "
                        "bounded-join timeout and were detached",
                        impl_->cfg.identity().uid, detached);
        }
    }

    impl_->running.store(false, std::memory_order_release);
    LOGGER_INFO("[HubHost:{}] shutdown complete",
                impl_->cfg.identity().uid);
}

void HubHost::request_shutdown() noexcept
{
    // Async equivalent of `shutdown()` minus the synchronous drain:
    //   1. flip the shutdown flag (run_main_loop's wait predicate)
    //   2. break the broker's poll loop
    //   3. wake run_main_loop
    // After this returns, the broker has begun stopping; the broker
    // thread will exit shortly.  ThreadManager join happens later
    // (in `shutdown()` if the caller drives it; otherwise in
    // `~HubHost`).  Safe from any thread (signal handler, admin RPC
    // dispatcher, broker error path).
    impl_->shutdown_flag.store(true, std::memory_order_release);
    if (impl_->broker)
    {
        try { impl_->broker->stop(); }
        catch (...) { /* noexcept contract — swallow */ }
    }
    {
        std::lock_guard<std::mutex> lk(impl_->wake_mu);
    }
    impl_->wake_cv.notify_all();
}

bool HubHost::is_running() const noexcept
{
    return impl_->running.load(std::memory_order_acquire);
}

// ============================================================================
// Accessors
// ============================================================================

const config::HubConfig &HubHost::config() const noexcept
{
    return impl_->cfg;
}

broker::BrokerService &HubHost::broker() noexcept
{
    return *impl_->broker; // UB if called outside startup..shutdown window
}

const hub::HubState &HubHost::state() const noexcept
{
    return impl_->state;
}

const std::string &HubHost::broker_endpoint() const noexcept
{
    return impl_->bound_endpoint;
}

const std::string &HubHost::broker_pubkey() const noexcept
{
    return impl_->bound_pubkey;
}

} // namespace pylabhub::hub_host
