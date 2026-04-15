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
    /// Set at join_all() entry. Once true, spawn() refuses new threads so
    /// the destructor's drain is the final one. Unlike the old join_all_done
    /// gate, it does NOT short-circuit the drain itself — it only guards
    /// against adding new slots after teardown has started.
    std::atomic<bool>                        closing{false};
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

/// LifecycleCallback shutdown: intentional no-op.
///
/// Teardown ownership is concentrated in the destructor (`~ThreadManager`),
/// which runs `join_all()` directly on its own `pImpl`. The lifecycle-
/// dispatched callback here has no access to the owner's intent and must NOT
/// touch Impl state — any mutation (e.g. setting a "done" flag) risks racing
/// the destructor path and abandoning joinable `std::thread` objects in the
/// slots vector, which would trigger `std::terminate` at Impl destruction.
///
/// Two dispatch modes end up here:
///   1. Normal teardown: `~ThreadManager` sets `impl_alive = false` BEFORE
///      `UnloadModule`, so by the time the async lifecycle shutdown thread
///      runs the validator, it returns false and this thunk is skipped.
///   2. Late/stale dispatch (process finalization, out-of-order shutdown):
///      the validator is the only guard. If it passes and we reach here,
///      logging the call is useful but state mutation would be unsafe.
void tm_shutdown(const char * /*arg*/, void *userdata)
{
    auto *impl = static_cast<ThreadManager::Impl *>(userdata);
    if (!impl || !impl->impl_alive.load(std::memory_order_acquire))
        return;
    LOGGER_DEBUG("[ThreadManager:{}] lifecycle shutdown thunk — no-op "
                 "(destructor owns teardown)", impl->composed_identity);
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

    // Register as a dynamic lifecycle module. The module exists to participate
    // in the lifecycle-graph teardown order (so owners that depend on this
    // ThreadManager shut down before it); its `tm_shutdown` thunk is a safe
    // no-op — `~ThreadManager` is the sole owner of the actual drain/join
    // path. See tm_shutdown for why lifecycle-dispatched shutdown must not
    // mutate state.
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

    // Re-check `closing` under the lock: combined with join_all()'s
    // "set closing + move slots" done under the same lock, this guarantees
    // either the new slot lands in pImpl->slots *before* the drain grabbed
    // it (safe — we'll join it), or we observe closing=true here and reject.
    // No interleaving can leave a joinable std::thread orphaned in slots.
    std::lock_guard<std::mutex> lock(pImpl->mu);
    if (pImpl->closing.load(std::memory_order_acquire))
    {
        LOGGER_WARN("[ThreadManager:{}] spawn('{}') called after join_all() — "
                    "thread will not be tracked; body NOT executed",
                    pImpl->composed_identity, name);
        return false;
    }
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

    // Set `closing` under the lock so a concurrent spawn() either:
    //   (a) grabbed the lock first and its slot is already in pImpl->slots
    //       — we'll pick it up in this drain, OR
    //   (b) observes closing=true after we release the lock and rejects.
    // Move slots out; process each outside the lock. Per-slot idempotency is
    // natural: std::thread::joinable() returns false after join/detach.
    std::vector<ThreadSlot> to_join;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        pImpl->closing.store(true, std::memory_order_release);
        to_join = std::move(pImpl->slots);
        pImpl->slots.clear();
    }

    std::size_t detached = 0;

    // Reverse spawn order: last spawned = first joined. This matches the
    // LIFO teardown convention used by LifecycleGuard for static modules
    // and gives deterministic join ordering when threads have ordering
    // dependencies (e.g. a worker depends on a ctrl thread to be alive).
    for (auto it = to_join.rbegin(); it != to_join.rend(); ++it)
    {
        auto &slot = *it;
        if (!slot.thread.joinable())
            continue;  // already joined/detached — idempotent no-op

        // Poll the done flag with a per-slot deadline. sleep_for(10ms) gives
        // a bounded reaction time vs. tight spinning.
        const auto deadline = std::chrono::steady_clock::now() + slot.join_timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (slot.done->load(std::memory_order_acquire))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        if (slot.done->load(std::memory_order_acquire))
        {
            slot.thread.join();  // thread body returned; join completes fast
            LOGGER_DEBUG("[ThreadManager:{}] thread '{}' joined cleanly",
                         pImpl->composed_identity, slot.name);
        }
        else
        {
            // Thread still running past deadline. Detach + ERROR log. The
            // shared_ptr<done> keeps the atomic alive so the runaway thread
            // can still safely write to it when it eventually finishes.
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
