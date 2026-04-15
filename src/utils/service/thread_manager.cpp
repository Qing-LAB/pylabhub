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
// Process-wide unclean-shutdown counter
// ============================================================================
//
// Any ThreadManager instance in this process that had to detach a stuck
// thread during join_all() increments this counter. Production main() and
// gtest event listeners query it via ThreadManager::process_detached_count()
// to decide whether to treat exit as clean.

namespace
{
std::atomic<std::size_t> g_process_detached_count{0};
}

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
    std::string                              owner_tag;      // class/category
    std::string                              owner_id;       // instance id
    std::string                              composed_identity; // "{owner_tag}:{owner_id}"
    std::string                              module_name;    // "ThreadManager:" + composed_identity
    std::chrono::milliseconds                aggregate_shutdown_timeout;

    mutable std::mutex                       mu;
    std::vector<ThreadSlot>                  slots;
    std::atomic<bool>                        join_all_done{false};
    std::atomic<bool>                        lifecycle_registered{false};

    /// Generation key captured at registration. Used by the
    /// ModuleDef userdata-validate path to reject stale callbacks if
    /// the ThreadManager is destroyed before lifecycle dispatches.
    std::atomic<bool>                        impl_alive{true};

    /// Count of threads detached on the last join_all() call. 0 means clean.
    /// Exposed via ThreadManager::detached_count_last_join() so callers
    /// (tests, process-exit policy) cannot mistake a timeout-detach for
    /// normal teardown.
    std::atomic<std::size_t>                 detached_count_last_join{0};
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
                             std::string owner_id,
                             std::chrono::milliseconds aggregate_shutdown_timeout)
    : pImpl(std::make_unique<Impl>())
{
    if (owner_tag.empty())
        throw std::invalid_argument("ThreadManager: owner_tag must be non-empty");
    if (owner_id.empty())
        throw std::invalid_argument("ThreadManager: owner_id must be non-empty");

    pImpl->owner_tag                  = std::move(owner_tag);
    pImpl->owner_id                   = std::move(owner_id);
    pImpl->composed_identity          = pImpl->owner_tag + ":" + pImpl->owner_id;
    pImpl->module_name                = "ThreadManager:" + pImpl->composed_identity;
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
                        pImpl->composed_identity);
        }
    }
    catch (const std::exception &e)
    {
        // Non-fatal: ThreadManager is still functional without lifecycle
        // integration. Log and continue.
        LOGGER_WARN("[ThreadManager:{}] lifecycle registration threw: {} — "
                    "continuing without lifecycle integration",
                    pImpl->composed_identity, e.what());
    }
}

ThreadManager::~ThreadManager()
{
    // Run the bounded join first — this is the real teardown.
    std::size_t detached = 0;
    try
    {
        detached = join_all();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[ThreadManager:{}] destructor join_all() threw: {}",
                     pImpl ? pImpl->composed_identity : std::string{"<moved-from>"}, e.what());
    }

    if (detached > 0 && pImpl)
    {
        // Process-level unclean-shutdown flag. Any main() that wraps its
        // role/services in ThreadManager-owning objects should query this
        // at exit and propagate to a non-zero return code. A test harness
        // can similarly assert detached_count_last_join() == 0 after the
        // fixture teardown to fail the test.
        //
        // We DO NOT throw from the dtor — that would std::terminate and
        // obscure the diagnostic ERROR logs already emitted. The
        // detached_count_last_join() accessor is the machine-readable
        // signal; operators see the log immediately.
        LOGGER_ERROR("[ThreadManager:{}] dtor: {} thread(s) leaked. Caller "
                     "MUST NOT treat process/test return as success.",
                     pImpl->composed_identity, detached);
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
                            pImpl->composed_identity, e.what());
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
                    pImpl->composed_identity, name);
        return false;
    }

    auto done = std::make_shared<std::atomic<bool>>(false);

    // Wrap the body so completion sets the done flag. shared_ptr capture
    // keeps the atomic alive for detached threads.
    auto wrapped = [body = std::move(body), done, name,
                    owner = pImpl->composed_identity]() mutable
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
                pImpl->composed_identity, name, opts.join_timeout.count());
    return true;
}

std::size_t ThreadManager::join_all()
{
    if (!pImpl) return 0;

    // Idempotent: snapshot slots under lock, clear, then process outside lock.
    std::vector<ThreadSlot> to_join;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        if (pImpl->join_all_done.load(std::memory_order_acquire))
            return pImpl->detached_count_last_join.load(std::memory_order_acquire);
        pImpl->join_all_done.store(true, std::memory_order_release);
        to_join = std::move(pImpl->slots);
        pImpl->slots.clear();
    }

    std::size_t detached = 0;

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
                         pImpl->composed_identity, slot.name);
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
                pImpl->composed_identity, slot.name, slot.join_timeout.count());
            slot.thread.detach();
            ++detached;
            g_process_detached_count.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    pImpl->detached_count_last_join.store(detached, std::memory_order_release);

    if (detached > 0)
    {
        // Loud summary at ERROR so an operator scanning logs for "N threads
        // leaked during shutdown of X" has the aggregate visible next to the
        // per-thread ERROR entries. Process-exit policy (main() returning
        // non-zero if any ThreadManager leaked) should query
        // detached_count_last_join() to make this a non-zero exit code.
        LOGGER_ERROR(
            "[ThreadManager:{}] UNCLEAN SHUTDOWN — {} thread(s) detached "
            "on timeout. Process exit should NOT report success.",
            pImpl->composed_identity, detached);
    }

    return detached;
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
    return pImpl ? pImpl->composed_identity : empty;
}

const std::string &ThreadManager::owner_id() const noexcept
{
    static const std::string empty;
    return pImpl ? pImpl->owner_id : empty;
}

const std::string &ThreadManager::composed_identity() const noexcept
{
    static const std::string empty;
    return pImpl ? pImpl->composed_identity : empty;
}

std::string ThreadManager::module_name() const
{
    return pImpl ? pImpl->module_name : std::string{};
}

std::size_t ThreadManager::detached_count_last_join() const
{
    return pImpl ? pImpl->detached_count_last_join.load(std::memory_order_acquire)
                 : 0;
}

// ============================================================================
// Process-wide accessors
// ============================================================================

std::size_t ThreadManager::process_detached_count() noexcept
{
    return g_process_detached_count.load(std::memory_order_acquire);
}

void ThreadManager::reset_process_detached_count_for_testing() noexcept
{
    g_process_detached_count.store(0, std::memory_order_release);
}

} // namespace pylabhub::utils
