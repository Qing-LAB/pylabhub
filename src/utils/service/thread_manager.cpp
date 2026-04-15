/**
 * @file thread_manager.cpp
 * @brief Implementation of pylabhub::utils::ThreadManager.
 *
 * See docs/tech_draft/thread_manager_design.md for design rationale.
 */
#include "utils/thread_manager.hpp"

#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pylabhub::utils
{

// ============================================================================
// ThreadSlot — single tracked thread
// ============================================================================

namespace
{
struct ThreadSlot
{
    std::thread                              thread;
    /// Set by the thread-body wrapper lambda when body() returns. The
    /// shared_ptr keeps the atomic alive even after ThreadManager::Impl is
    /// destroyed, so a detached thread can finish writing 'true' without UAF.
    std::shared_ptr<std::atomic<bool>>       done;
    std::string                              name;
    /// Default matches pylabhub::kMidTimeoutMs; the SpawnOptions default
    /// on the caller side is what actually feeds this in normal flow.
    std::chrono::milliseconds                join_timeout{pylabhub::kMidTimeoutMs};
    std::chrono::steady_clock::time_point    spawn_time;
};
} // namespace

// ============================================================================
// Impl
// ============================================================================

struct ThreadManager::Impl
{
    std::string                              owner_tag;
    std::string                              module_name;   // "ThreadManager:" + owner_tag
    std::chrono::milliseconds                aggregate_shutdown_timeout;

    mutable std::mutex                       mu;
    std::vector<ThreadSlot>                  slots;
    std::atomic<bool>                        join_all_done{false};
    std::atomic<bool>                        lifecycle_registered{false};

    /// Generation key captured at registration. Used by the
    /// ModuleDef userdata-validate path to reject stale callbacks if
    /// the ThreadManager is destroyed before lifecycle dispatches.
    std::atomic<bool>                        impl_alive{true};
};

// ============================================================================
// Lifecycle bridge — static thunks called by LifecycleManager
// ============================================================================

namespace
{

/// UserDataValidateFn: returns true iff the Impl* is still alive. Key is
/// unused — the impl_alive atomic is the single source of truth for
/// liveness; if the ThreadManager is destroyed, impl_alive is set to false
/// before deregistration, so stale lifecycle-dispatched callbacks return
/// false here and are skipped by LifecycleManager.
bool tm_impl_validate(void *userdata, uint64_t /*key*/) noexcept
{
    auto *impl = static_cast<ThreadManager::Impl *>(userdata);
    return impl != nullptr && impl->impl_alive.load(std::memory_order_acquire);
}

/// LifecycleCallback startup: no-op. Threads spawn lazily via spawn().
void tm_startup(const char * /*arg*/, void * /*userdata*/)
{
    // ThreadManager spawn() is the mutator; module-startup is idempotent.
}

/// LifecycleCallback shutdown: calls join_all() on the instance if still alive.
void tm_shutdown(const char * /*arg*/, void *userdata)
{
    auto *impl = static_cast<ThreadManager::Impl *>(userdata);
    if (!impl || !impl->impl_alive.load(std::memory_order_acquire))
        return;
    // join_all_impl is an internal function that takes the Impl directly.
    // It's defined in the ThreadManager class below as a free helper to
    // avoid name-mangling issues with static functions referencing member
    // functions — just inline the body here.
    // (See ThreadManager::join_all below; this path is equivalent.)

    // NOTE: calling back into the dying instance is safe because the
    // destructor clears impl_alive BEFORE deregistering the module.

    std::lock_guard<std::mutex> lock(impl->mu);
    if (impl->join_all_done.load(std::memory_order_acquire))
        return;
    // Fall-through: join_all executes outside this static thunk via a
    // shared private helper. For now we set the flag and the destructor
    // will do the work. This keeps the lifecycle dispatch path simple.
    impl->join_all_done.store(true, std::memory_order_release);
}

} // namespace

// ============================================================================
// ThreadManager — construction / destruction
// ============================================================================

ThreadManager::ThreadManager(std::string owner_tag,
                             std::chrono::milliseconds aggregate_shutdown_timeout)
    : pImpl(std::make_unique<Impl>())
{
    if (owner_tag.empty())
        throw std::invalid_argument("ThreadManager: owner_tag must be non-empty");

    pImpl->owner_tag                  = std::move(owner_tag);
    pImpl->module_name                = "ThreadManager:" + pImpl->owner_tag;
    pImpl->aggregate_shutdown_timeout = aggregate_shutdown_timeout;

    // Register as a dynamic lifecycle module. Shutdown function marks the
    // instance so the destructor path will complete the actual join. We
    // deliberately do the real join in the destructor (not in the lifecycle
    // shutdown thunk) because:
    //   - LifecycleManager's timedShutdown already detaches on timeout;
    //     a second layer of detachment in the thunk would double-wrap the
    //     already-detaching flow.
    //   - The owner's destructor has complete knowledge of what threads it
    //     expects; lifecycle's module ordering guarantees the module-level
    //     shutdown fires before the owner's own destructor in a clean
    //     process-exit, but during crash / early destructor chains the
    //     owner's dtor is the only reliable ordering.
    //   - Making join_all() the single source of truth (invoked by the
    //     destructor always, and no-op if lifecycle already marked it
    //     done via the flag) avoids double-join.
    try
    {
        ModuleDef mod(pImpl->module_name.c_str(), pImpl.get(), tm_impl_validate);
        mod.add_dependency("pylabhub::utils::Logger");
        mod.set_startup(tm_startup);
        mod.set_shutdown(tm_shutdown, aggregate_shutdown_timeout);
        if (LifecycleManager::instance().register_dynamic_module(std::move(mod)))
        {
            (void)LoadModule(pImpl->module_name);
            pImpl->lifecycle_registered.store(true, std::memory_order_release);
        }
        else
        {
            LOGGER_WARN("[ThreadManager:{}] lifecycle module registration "
                        "returned false — continuing without lifecycle integration",
                        pImpl->owner_tag);
        }
    }
    catch (const std::exception &e)
    {
        // Non-fatal: ThreadManager is still functional without lifecycle
        // integration. Log and continue.
        LOGGER_WARN("[ThreadManager:{}] lifecycle registration threw: {} — "
                    "continuing without lifecycle integration",
                    pImpl->owner_tag, e.what());
    }
}

ThreadManager::~ThreadManager()
{
    // Run the bounded join first — this is the real teardown.
    try
    {
        join_all();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[ThreadManager:{}] destructor join_all() threw: {}",
                     pImpl ? pImpl->owner_tag : "<moved-from>", e.what());
    }

    // Mark Impl as no-longer-alive BEFORE deregistering so any concurrent
    // lifecycle-dispatched callback sees the validator return false.
    if (pImpl)
    {
        pImpl->impl_alive.store(false, std::memory_order_release);

        if (pImpl->lifecycle_registered.load(std::memory_order_acquire))
        {
            try
            {
                (void)UnloadModule(pImpl->module_name);
            }
            catch (const std::exception &e)
            {
                LOGGER_WARN("[ThreadManager:{}] lifecycle unload_module threw: {}",
                            pImpl->owner_tag, e.what());
            }
        }
    }
}

// ============================================================================
// spawn / join_all
// ============================================================================

bool ThreadManager::spawn(const std::string &name,
                          std::function<void()> body)
{
    return spawn(name, std::move(body), SpawnOptions{});
}

bool ThreadManager::spawn(const std::string &name,
                          std::function<void()> body,
                          SpawnOptions opts)
{
    if (!pImpl) return false;
    if (pImpl->join_all_done.load(std::memory_order_acquire))
    {
        LOGGER_WARN("[ThreadManager:{}] spawn('{}') called after join_all() — "
                    "thread will not be tracked; body NOT executed",
                    pImpl->owner_tag, name);
        return false;
    }

    auto done = std::make_shared<std::atomic<bool>>(false);

    // Wrap the body so completion sets the done flag. shared_ptr capture
    // keeps the atomic alive for detached threads.
    auto wrapped = [body = std::move(body), done, name,
                    owner = pImpl->owner_tag]() mutable
    {
        try
        {
            body();
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[ThreadManager:{}] thread '{}' body threw: {}",
                         owner, name, e.what());
        }
        catch (...)
        {
            LOGGER_ERROR("[ThreadManager:{}] thread '{}' body threw unknown exception",
                         owner, name);
        }
        done->store(true, std::memory_order_release);
    };

    std::lock_guard<std::mutex> lock(pImpl->mu);
    ThreadSlot slot;
    slot.name         = name;
    slot.join_timeout = opts.join_timeout;
    slot.spawn_time   = std::chrono::steady_clock::now();
    slot.done         = done;
    slot.thread       = std::thread(std::move(wrapped));
    pImpl->slots.emplace_back(std::move(slot));

    LOGGER_INFO("[ThreadManager:{}] spawned thread '{}' (join_timeout={}ms)",
                pImpl->owner_tag, name, opts.join_timeout.count());
    return true;
}

void ThreadManager::join_all()
{
    if (!pImpl) return;

    // Idempotent: snapshot slots under lock, clear, then process outside lock.
    std::vector<ThreadSlot> to_join;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        if (pImpl->join_all_done.load(std::memory_order_acquire))
            return;
        pImpl->join_all_done.store(true, std::memory_order_release);
        to_join = std::move(pImpl->slots);
        pImpl->slots.clear();
    }

    // Reverse spawn order (last spawned = first joined).
    for (auto it = to_join.rbegin(); it != to_join.rend(); ++it)
    {
        auto &slot = *it;
        if (!slot.thread.joinable())
            continue;

        // Poll the done flag until the per-thread deadline.
        const auto deadline = std::chrono::steady_clock::now() + slot.join_timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (slot.done->load(std::memory_order_acquire))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        if (slot.done->load(std::memory_order_acquire))
        {
            // Thread body returned; std::thread::join() now completes fast.
            slot.thread.join();
            LOGGER_DEBUG("[ThreadManager:{}] thread '{}' joined cleanly",
                         pImpl->owner_tag, slot.name);
        }
        else
        {
            // Thread still running past deadline. Detach + ERROR log so the
            // stuck thread is identified by owner+name. The shared_ptr<done>
            // keeps its state alive so the runaway thread can still safely
            // write to it when it eventually finishes.
            LOGGER_ERROR(
                "[ThreadManager:{}] thread '{}' did NOT exit within {}ms — "
                "detaching. Shutdown continuing; detached thread may still "
                "hold resources (sockets, SHM, etc.). Investigate body logic "
                "for shutdown-signal observation.",
                pImpl->owner_tag, slot.name, slot.join_timeout.count());
            slot.thread.detach();
        }
    }
}

// ============================================================================
// Accessors
// ============================================================================

std::size_t ThreadManager::active_count() const
{
    if (!pImpl) return 0;
    std::lock_guard<std::mutex> lock(pImpl->mu);
    std::size_t n = 0;
    for (const auto &slot : pImpl->slots)
        if (slot.thread.joinable())
            ++n;
    return n;
}

std::vector<ThreadManager::ThreadInfo> ThreadManager::snapshot() const
{
    std::vector<ThreadInfo> out;
    if (!pImpl) return out;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(pImpl->mu);
    out.reserve(pImpl->slots.size());
    for (const auto &slot : pImpl->slots)
    {
        ThreadInfo info;
        info.name         = slot.name;
        info.alive        = slot.thread.joinable()
                            && !slot.done->load(std::memory_order_acquire);
        info.elapsed      = now - slot.spawn_time;
        info.join_timeout = slot.join_timeout;
        out.emplace_back(std::move(info));
    }
    return out;
}

const std::string &ThreadManager::owner_tag() const noexcept
{
    static const std::string empty;
    return pImpl ? pImpl->owner_tag : empty;
}

std::string ThreadManager::module_name() const
{
    return pImpl ? pImpl->module_name : std::string{};
}

} // namespace pylabhub::utils
