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

#include <algorithm>
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
// thread during drain() increments this counter. Production main() and
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
    std::thread thread;
    /// Set by the thread-body wrapper lambda when body() returns. The
    /// shared_ptr keeps the atomic alive even after ThreadManager::Impl is
    /// destroyed, so a detached thread can finish writing 'true' without UAF.
    std::shared_ptr<std::atomic<bool>> done;
    /// Monotonic-mark API (HEP-CORE-0031 §4.1; handy-case family).  Set
    /// by `SlotContext::mark_active_loop_exited()` or by the spawn
    /// wrapper as a safety net at body exit.  Don't mix with the
    /// transactional API on the same slot.  Same shared_ptr lifetime
    /// guarantee as `done`.
    std::shared_ptr<std::atomic<bool>> active_loop_exited;
    /// Transactional bracket API (HEP-CORE-0031 §4.1; production
    /// family).  Counter — incremented on `with_active_loop` entry,
    /// decremented on body return (RAII), so nested bracket calls
    /// on the same `SlotContext` are safe: the slot is "in active
    /// loop" iff this counter is > 0.  Default 0 — threads that
    /// never bracket stay safe (quiescent) forever.  Don't mix with
    /// the monotonic-mark API on the same slot.
    std::shared_ptr<std::atomic<int>> active_loop_depth;
    /// Per-slot shutdown flag.  Set by
    /// `ThreadManager::request_shutdown(name)` or
    /// `request_shutdown_all()`.  Observed by
    /// `SlotContext::with_active_loop` (skips body if true at entry)
    /// and `SlotContext::shutdown_requested()` (thread-side poll).
    std::shared_ptr<std::atomic<bool>> shutdown_requested;
    std::string name;
    /// Default matches pylabhub::kMidTimeoutMs; the SpawnOptions default
    /// on the caller side is what actually feeds this in normal flow.
    std::chrono::milliseconds join_timeout{pylabhub::kMidTimeoutMs};
    std::chrono::steady_clock::time_point spawn_time;
    /// Master/peer flag (HEP-CORE-0031 §4.2; post-MD1.5).  At most
    /// one master per ThreadManager — enforced at spawn time.  See
    /// `SpawnOptions::is_master` for the why.
    bool is_master{false};
};

/// Shared per-slot bounded-join state machine used by both `drain()` and
/// `join_named()`.  Implements the two-stage wait per HEP-CORE-0031 §4.1:
///   Stage 1 (first half of join_timeout): wait for `active_loop_depth == 0`
///                                          — slot has left every active
///                                          `with_active_loop` bracket
///                                          frame.
///   Stage 2 (second half):                 wait for `done == true` —
///                                          body has returned.
///
/// Threads that never call `with_active_loop` have `active_loop_depth = 0`
/// by default, so Stage 1 passes immediately for them.  Threads using the
/// monotonic-mark family (active_loop_exited) also pass Stage 1
/// immediately — their `active_loop_depth` stays at 0.
///
/// On success, joins the thread cleanly and returns true.  On timeout
/// (both stages elapsed without `done` becoming true), detaches the
/// thread, increments the process-wide unclean-shutdown counter, and
/// emits a stage-differentiated ERROR log so operators can distinguish
/// "stuck inside its active loop" (thread still in `with_active_loop`
/// bracket) from "stuck in post-loop cleanup" (bracket exited but body
/// hasn't returned).
///
/// Caller is responsible for slot extraction from `pImpl->slots` and any
/// surrounding orchestration (set `closing`, aggregate counts, etc.).
/// `caller_tag` is embedded in the diagnostic ERROR log ("drain" /
/// "join_named") so the operator sees which entry point produced the
/// detach.
///
/// @return true iff the thread body returned and `slot.thread.join()`
///   completed; false iff the thread was detached as last resort.
bool process_slot_bounded_join(ThreadSlot &slot, std::string_view owner_identity,
                               std::string_view caller_tag)
{
    if (!slot.thread.joinable())
        return true; // already joined/detached — idempotent no-op

    const auto half = slot.join_timeout / 2;

    // Stage 1 — wait for active_loop_depth = 0.  Fast path: if already
    // 0 (default-safe state, or thread already exited every bracket
    // frame), skip the poll loop entirely.
    auto in_loop_now = [&]() -> bool
    {
        return slot.active_loop_depth &&
               slot.active_loop_depth->load(std::memory_order_acquire) > 0;
    };
    bool was_in_loop = in_loop_now();
    if (was_in_loop)
    {
        const auto stage1_deadline = std::chrono::steady_clock::now() + half;
        while (std::chrono::steady_clock::now() < stage1_deadline)
        {
            if (!in_loop_now())
            {
                was_in_loop = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    // Stage 2 — wait for done.  If Stage 1 succeeded fast, done is
    // imminent (post-loop cleanup is the small residual gap); use the
    // full remaining timeout budget.
    const auto stage2_deadline = std::chrono::steady_clock::now() + half;
    while (std::chrono::steady_clock::now() < stage2_deadline)
    {
        if (slot.done->load(std::memory_order_acquire))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    if (slot.done->load(std::memory_order_acquire))
    {
        slot.thread.join(); // body returned; join completes fast
        LOGGER_DEBUG("[ThreadManager:{}] thread '{}' joined cleanly via {}", owner_identity,
                     slot.name, caller_tag);
        return true;
    }

    // Timeout — detach as last resort.  shared_ptr<atomic> captures in
    // the wrapper keep the atomics alive after slot destruction so the
    // runaway thread can still safely write its flags on its way out.
    if (was_in_loop)
    {
        LOGGER_ERROR("[ThreadManager:{}] {}: thread '{}' did NOT exit its active "
                     "loop within {}ms — detaching.  Thread is stuck inside its "
                     "active loop body (still in with_active_loop bracket).  "
                     "Detached thread may still hold resources (sockets, SHM, etc.).",
                     owner_identity, caller_tag, slot.name, half.count());
    }
    else
    {
        LOGGER_ERROR("[ThreadManager:{}] {}: thread '{}' exited its active loop "
                     "but did NOT return within {}ms post-loop — detaching.  "
                     "Thread is stuck in post-loop cleanup.  Detached thread may "
                     "still hold resources.",
                     owner_identity, caller_tag, slot.name, half.count());
    }
    slot.thread.detach();
    g_process_detached_count.fetch_add(1, std::memory_order_acq_rel);
    return false;
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct ThreadManager::Impl
{
    std::string owner_tag;         // class/category
    std::string owner_id;          // instance id
    std::string composed_identity; // "{owner_tag}:{owner_id}"
    std::string module_name;       // "ThreadManager:" + composed_identity
    std::chrono::milliseconds aggregate_shutdown_timeout;

    mutable std::mutex mu;
    std::vector<ThreadSlot> slots;
    /// Set at drain() entry. Once true, spawn() refuses new threads so
    /// the destructor's drain is the final one. Unlike the old join_all_done
    /// gate, it does NOT short-circuit the drain itself — it only guards
    /// against adding new slots after teardown has started.
    std::atomic<bool> closing{false};
    std::atomic<bool> lifecycle_registered{false};

    /// Generation key captured at registration. Used by the
    /// ModuleDef userdata-validate path to reject stale callbacks if
    /// the ThreadManager is destroyed before lifecycle dispatches.
    std::atomic<bool> impl_alive{true};

    /// Count of threads detached on the last drain() call. 0 means clean.
    /// Exposed via ThreadManager::detached_count_last_drain() so callers
    /// (tests, process-exit policy) cannot mistake a timeout-detach for
    /// normal teardown.
    std::atomic<std::size_t> detached_count_last_drain{0};

    /// Retained `done` flags of every slot that was detached during the
    /// last drain.  We hold a `shared_ptr` so the underlying
    /// `atomic<bool>` outlives the detached `std::thread` itself — the
    /// runaway thread's wrapper lambda also holds it, so the flag is
    /// safely writable from the thread until it returns and readable by
    /// `all_detached_done()` from the owner (e.g.
    /// `EngineHost::shutdown_()`'s best-effort grace-poll before
    /// `api_.reset()`).  Cleared at the start of every drain.
    std::vector<std::shared_ptr<std::atomic<bool>>> detached_done_flags;
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
/// which runs `drain()` directly on its own `pImpl`. The lifecycle-
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
                 "(destructor owns teardown)",
                 impl->composed_identity);
}

} // namespace

// ============================================================================
// ThreadManager — construction / destruction
// ============================================================================

ThreadManager::ThreadManager(std::string owner_tag, std::string owner_id,
                             std::chrono::milliseconds aggregate_shutdown_timeout)
    : pImpl(std::make_unique<Impl>())
{
    if (owner_tag.empty())
        throw std::invalid_argument("ThreadManager: owner_tag must be non-empty");
    if (owner_id.empty())
        throw std::invalid_argument("ThreadManager: owner_id must be non-empty");

    pImpl->owner_tag = std::move(owner_tag);
    pImpl->owner_id = std::move(owner_id);
    pImpl->composed_identity = pImpl->owner_tag + ":" + pImpl->owner_id;
    pImpl->module_name = "ThreadManager:" + pImpl->composed_identity;
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
        // Opt in to owner-managed teardown (HEP-CORE-0001 §"Owner-managed
        // teardown" exception / `ModuleDef::set_owner_managed_teardown`).
        // ~ThreadManager performs the real teardown synchronously (drain +
        // bounded join) BEFORE flipping `impl_alive=false`; the flip then
        // makes `tm_impl_validate` return false on any later lifecycle-
        // dispatched shutdown.  With the flag set, the lifecycle layer
        // treats this validator-fail as a success-without-callback (the
        // registered `tm_shutdown` is contractually a no-op anyway), runs
        // full graph cleanup so the module name is freed for re-
        // registration, and does NOT log a WARN or mark the module
        // contaminated.  Without the flag this fallback is anomalous per
        // HEP-0001 and surfaces as a real WARN — exactly what we want for
        // any module that does not deliberately opt in.
        mod.set_owner_managed_teardown(true);
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
        detached = drain();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[ThreadManager:{}] destructor drain() threw: {}",
                     pImpl ? pImpl->composed_identity : std::string{"<moved-from>"}, e.what());
    }

    if (detached > 0 && pImpl)
    {
        // Process-level unclean-shutdown flag. Any main() that wraps its
        // role/services in ThreadManager-owning objects should query this
        // at exit and propagate to a non-zero return code. A test harness
        // can similarly assert detached_count_last_drain() == 0 after the
        // fixture teardown to fail the test.
        //
        // We DO NOT throw from the dtor — that would std::terminate and
        // obscure the diagnostic ERROR logs already emitted. The
        // detached_count_last_drain() accessor is the machine-readable
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
// spawn / drain
// ============================================================================

// ── SlotContext — out-of-line members ───────────────────────────────────────
//
// (The transactional `with_active_loop` is an inline template in the
// header; it has no out-of-line definition.)

void ThreadManager::SlotContext::mark_active_loop_exited() noexcept
{
    if (active_loop_exited_)
    {
        active_loop_exited_->store(true, std::memory_order_release);
    }
}

bool ThreadManager::SlotContext::shutdown_requested() const noexcept
{
    return shutdown_requested_ && shutdown_requested_->load(std::memory_order_acquire);
}

// ── spawn — contract-aware overloads (with SlotContext) ─────────────────────

bool ThreadManager::spawn(const std::string &name, std::function<void(SlotContext &)> body)
{
    return spawn(name, std::move(body), SpawnOptions{});
}

bool ThreadManager::spawn(const std::string &name, std::function<void(SlotContext &)> body,
                          SpawnOptions opts)
{
    if (!pImpl)
        return false;

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto active_loop_exited = std::make_shared<std::atomic<bool>>(false);
    auto active_loop_depth = std::make_shared<std::atomic<int>>(0);
    auto shutdown_requested = std::make_shared<std::atomic<bool>>(false);

    // Wrap the body. The wrapper:
    //   - constructs a SlotContext bound to all per-slot flags and passes
    //     it into the user body;
    //   - on body return (normal or exception), defensively marks
    //     `active_loop_exited` (monotonic-mark family safety net) and
    //     forces `active_loop_depth` back to 0 (belt-and-suspenders —
    //     RAII guarantees this for normal exit, but resetting here
    //     catches any pathological path that left depth positive);
    //   - then sets `done` so the joiner can proceed.
    // shared_ptr captures keep all atomics alive for detached threads.
    auto wrapped = [body = std::move(body), done, active_loop_exited, active_loop_depth,
                    shutdown_requested, name, owner = pImpl->composed_identity]() mutable
    {
        SlotContext ctx{active_loop_exited, active_loop_depth, shutdown_requested};
        try
        {
            body(ctx);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[ThreadManager:{}] thread '{}' body threw: {}", owner, name, e.what());
        }
        catch (...)
        {
            LOGGER_ERROR("[ThreadManager:{}] thread '{}' body threw unknown exception", owner,
                         name);
        }
        // Defensive marks — guarantees both quiescence flags are in the
        // safe state before `done` is set, even on exception paths.
        active_loop_depth->store(0, std::memory_order_release);
        active_loop_exited->store(true, std::memory_order_release);
        done->store(true, std::memory_order_release);
    };

    // Re-check `closing` under the lock: combined with drain()'s
    // "set closing + move slots" done under the same lock, this guarantees
    // either the new slot lands in pImpl->slots *before* the drain grabbed
    // it (safe — we'll join it), or we observe closing=true here and reject.
    // No interleaving can leave a joinable std::thread orphaned in slots.
    std::lock_guard<std::mutex> lock(pImpl->mu);
    if (pImpl->closing.load(std::memory_order_acquire))
    {
        LOGGER_WARN("[ThreadManager:{}] spawn('{}') called after drain() — "
                    "thread will not be tracked; body NOT executed",
                    pImpl->composed_identity, name);
        return false;
    }
    // Enforce single-master invariant per HEP-CORE-0031 §4.2.  A
    // ThreadManager with two masters has no valid drain order — the
    // contract assumes exactly one orchestrator whose lifetime
    // envelopes the peers'.
    if (opts.is_master)
    {
        for (const auto &existing : pImpl->slots)
        {
            if (existing.is_master)
            {
                LOGGER_ERROR("[ThreadManager:{}] spawn('{}', is_master=true) "
                             "rejected — slot '{}' is already master; a "
                             "ThreadManager admits at most one master "
                             "(HEP-CORE-0031 §4.2).",
                             pImpl->composed_identity, name, existing.name);
                return false;
            }
        }
    }
    ThreadSlot slot;
    slot.name = name;
    slot.join_timeout = opts.join_timeout;
    slot.spawn_time = std::chrono::steady_clock::now();
    slot.done = done;
    slot.active_loop_exited = active_loop_exited;
    slot.active_loop_depth = active_loop_depth;
    slot.shutdown_requested = shutdown_requested;
    slot.is_master = opts.is_master;
    slot.thread = std::thread(std::move(wrapped));
    pImpl->slots.emplace_back(std::move(slot));

    LOGGER_INFO("[ThreadManager:{}] spawned thread '{}' (join_timeout={}ms)",
                pImpl->composed_identity, name, opts.join_timeout.count());
    return true;
}

// ── spawn — backwards-compatible overloads (body takes no arguments) ────────

bool ThreadManager::spawn(const std::string &name, std::function<void()> body)
{
    return spawn(name, std::move(body), SpawnOptions{});
}

bool ThreadManager::spawn(const std::string &name, std::function<void()> body, SpawnOptions opts)
{
    // Trampoline: ignore the SlotContext and call the no-arg body.  The
    // contract-aware spawn() above marks active_loop_exited automatically
    // right before `done`, so old-overload callers get a reasonable default
    // (their `active_loop_exited` becomes true at body-return).  Callers
    // that need to mark loop-exit BEFORE body-return must use the
    // SlotContext overload directly.
    return spawn(name, [body = std::move(body)](SlotContext &) mutable { body(); }, opts);
}

// ── is_active_loop_exited / wait_for_active_loop_exit ───────────────────────

bool ThreadManager::is_active_loop_exited(std::string_view name) const noexcept
{
    if (!pImpl)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->mu);
    for (const auto &slot : pImpl->slots)
    {
        if (slot.name == name)
        {
            return slot.active_loop_exited &&
                   slot.active_loop_exited->load(std::memory_order_acquire);
        }
    }
    return false; // no such slot
}

bool ThreadManager::request_shutdown(std::string_view name) noexcept
{
    if (!pImpl)
        return false;
    std::lock_guard<std::mutex> lock(pImpl->mu);
    for (auto &slot : pImpl->slots)
    {
        if (slot.name == name)
        {
            if (slot.shutdown_requested)
                slot.shutdown_requested->store(true, std::memory_order_release);
            return true;
        }
    }
    return false; // no such slot
}

std::size_t ThreadManager::request_shutdown_all() noexcept
{
    if (!pImpl)
        return 0;
    std::lock_guard<std::mutex> lock(pImpl->mu);
    pImpl->closing.store(true, std::memory_order_release);
    std::size_t count = 0;
    for (auto &slot : pImpl->slots)
    {
        // Per HEP-CORE-0031 §4.2 master/peer model: signal peers
        // only; the master's shutdown_requested is set later (inside
        // drain) only after every peer's `done` is true.  Signaling
        // the master here would defeat the purpose — the master is
        // exactly the thread whose lifetime must envelope the peers'.
        // See SpawnOptions::is_master in thread_manager.hpp.
        if (slot.is_master)
            continue;
        if (slot.shutdown_requested)
        {
            slot.shutdown_requested->store(true, std::memory_order_release);
            ++count;
        }
    }
    return count;
}

std::size_t ThreadManager::wait_for_quiescence(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl)
        return 0;

    // Snapshot the active_loop_depth counter of every spawned slot
    // under the lock, EXCLUDING the calling thread (waiting on
    // yourself deadlocks).  Threads whose depth is already 0 are
    // filtered out — quiescent by definition.
    struct Pending
    {
        std::string name;
        std::shared_ptr<std::atomic<int>> depth;
    };
    std::vector<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        const auto self_id = std::this_thread::get_id();
        for (const auto &slot : pImpl->slots)
        {
            if (slot.thread.get_id() == self_id)
                continue;
            if (!slot.active_loop_depth)
                continue;
            if (slot.active_loop_depth->load(std::memory_order_acquire) == 0)
                continue; // already quiescent (default-safe or already exited)
            pending.push_back({slot.name, slot.active_loop_depth});
        }
    }
    if (pending.empty())
        return 0;

    auto erase_done = [](std::vector<Pending> &v)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [](const Pending &p)
                               { return p.depth->load(std::memory_order_acquire) == 0; }),
                v.end());
    };

    // Shared wall-clock deadline (NOT per-slot — total wait bounded).
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline && !pending.empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        erase_done(pending);
    }
    erase_done(pending); // post-deadline final check

    for (const auto &p : pending)
    {
        LOGGER_WARN("[ThreadManager:{}] wait_for_quiescence: thread '{}' still in "
                    "active loop after {}ms — caller's destructive operations may "
                    "race with this thread's pImpl access",
                    pImpl->composed_identity, p.name, timeout.count());
    }
    return pending.size();
}

bool ThreadManager::wait_for_active_loop_exit(std::string_view name,
                                              std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl)
        return false;

    // Capture the shared_ptr to the flag under the lock, then poll outside
    // the lock so concurrent spawns / drains are not blocked by the poll.
    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        for (const auto &slot : pImpl->slots)
        {
            if (slot.name == name)
            {
                flag = slot.active_loop_exited;
                break;
            }
        }
    }
    if (!flag)
        return false; // no such slot

    if (flag->load(std::memory_order_acquire))
        return true; // fast path

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (flag->load(std::memory_order_acquire))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    // Final check in case the flag was set right at the deadline boundary.
    return flag->load(std::memory_order_acquire);
}

std::size_t ThreadManager::drain()
{
    if (!pImpl)
        return 0;

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

    // ── Master/peer orchestration (HEP-CORE-0031 §4.2; post-MD1.5) ──
    //
    // Locate the master slot (at most one — enforced at spawn time).
    // When present, drain runs the four-phase orchestrated path that
    // guarantees the master outlives every peer: peers signaled +
    // exit first, only then the master is signaled, only then is
    // anyone joined.
    //
    // Without this, the role-host shutdown sequence deterministically
    // crashes:
    //   * `EngineHost::shutdown_()` flips `core.is_shutdown_requested`
    //     (global flag — signals worker AND ctrl simultaneously);
    //   * LIFO drain joins ctrl first → ctrl exits its with_active_loop
    //     bracket immediately on the global flag;
    //   * worker (still alive) enters `do_role_teardown` Step 9a
    //     (`deregister_from_broker`), which sends DEREG_REQ via
    //     `BrokerRequestComm::do_request`;
    //   * `do_request` blocks on a CV waiting for DEREG_ACK from the
    //     ctrl thread — but ctrl is gone, nobody will reply;
    //   * worker's bounded-join timeout fires, drain detaches worker
    //     and returns; `api_.reset()` destroys `RoleAPIBase::Impl`;
    //   * the detached worker thread eventually returns from the
    //     timed-out `do_request` and touches the freed pImpl → SEGV.
    // Confirmed via gdb + libc backtrace + in-source breadcrumbs
    // 2026-05-13; the SEGV reproduces 5/5 — not a race, a sequence
    // violation.
    //
    // The master/peer orchestration encodes the dependency: "ctrl
    // (master) must outlive every other slot's runtime needs."
    int master_idx = -1;
    for (std::size_t i = 0; i < to_join.size(); ++i)
    {
        if (to_join[i].is_master)
        {
            master_idx = static_cast<int>(i);
            break;
        }
    }

    if (master_idx >= 0)
    {
        // Phase 1 — signal every peer.  If the caller already invoked
        // `request_shutdown_all()` upstream this is a re-signal (cheap
        // idempotent atomic store).  Class-level wake-ups (sockets,
        // CVs) that unblock a peer from a blocking operation are
        // still the caller's responsibility — ThreadManager only
        // flips per-slot flags.
        for (std::size_t i = 0; i < to_join.size(); ++i)
        {
            if (static_cast<int>(i) == master_idx)
                continue;
            if (to_join[i].shutdown_requested)
                to_join[i].shutdown_requested->store(true, std::memory_order_release);
        }

        // Phase 2 — wait for every peer's `done` flag.  Bounded by
        // each peer's per-slot `join_timeout`.  Polls in 10 ms
        // granularity; per-slot wall-clock cap so a single
        // unresponsive peer can't extend the overall drain
        // arbitrarily.
        for (std::size_t i = 0; i < to_join.size(); ++i)
        {
            if (static_cast<int>(i) == master_idx)
                continue;
            auto &slot = to_join[i];
            if (!slot.done)
                continue;
            const auto deadline = std::chrono::steady_clock::now() + slot.join_timeout;
            while (!slot.done->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            // Don't act on timeout here — Phase 4's bounded-join is
            // the single place that performs detach + ERROR log +
            // process-wide counter increment, so any timing
            // accounting and operator-facing diagnostic stay
            // canonical.
        }

        // Phase 3 — signal the master.  All peers are now done (or
        // we waited up to their timeouts).  Setting the master's
        // shutdown_requested is what releases its `with_active_loop`
        // bracket via the `ctx.shutdown_requested()` predicate the
        // master is required to consult; see SpawnOptions::is_master.
        if (to_join[master_idx].shutdown_requested)
            to_join[master_idx].shutdown_requested->store(true, std::memory_order_release);
    }

    std::size_t detached = 0;

    // Reset the retained-`done`-flags list at the start of the join
    // phase.  Each detach in the loop below appends one entry so the
    // owner can later poll `all_detached_done()` for a grace period
    // before destroying state that the detached thread still touches
    // (see `EngineHost::shutdown_()`'s safety gate).
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        pImpl->detached_done_flags.clear();
    }

    // Phase 4 (or sole path when no master): per-slot bounded join in
    // reverse spawn order (LIFO).  This matches the LIFO convention
    // used by LifecycleGuard for static modules and gives
    // deterministic join ordering across the manager's tracked
    // threads.  When a master was orchestrated above, every slot is
    // already on its way to `done == true`; LIFO is just the final
    // join order.
    //
    // Per-slot work delegates to the shared `process_slot_bounded_join`
    // helper (two-stage wait + join-or-detach + stage-differentiated
    // ERROR log + process-wide detach counter).  See
    // HEP-CORE-0031 §4.1.3.
    for (auto it = to_join.rbegin(); it != to_join.rend(); ++it)
    {
        if (!process_slot_bounded_join(*it, pImpl->composed_identity, "drain"))
        {
            ++detached;
            // Retain the slot's `done` shared_ptr — the detached
            // thread's wrapper lambda also holds it and will flip it
            // true when the body finally returns; the owner can poll
            // via `all_detached_done()` before tearing down state.
            if (it->done)
            {
                std::lock_guard<std::mutex> lock(pImpl->mu);
                pImpl->detached_done_flags.push_back(it->done);
            }
        }
    }

    pImpl->detached_count_last_drain.store(detached, std::memory_order_release);

    if (detached > 0)
    {
        LOGGER_ERROR("[ThreadManager:{}] UNCLEAN SHUTDOWN — {} thread(s) detached "
                     "on timeout. Process exit should NOT report success.",
                     pImpl->composed_identity, detached);
    }

    return detached;
}

bool ThreadManager::join_named(std::string_view name)
{
    if (!pImpl)
        return false;

    // Find + signal + extract the named slot, all under one lock
    // acquisition.  Signaling means flipping the per-slot
    // shutdown_requested flag — `with_active_loop` in this thread will
    // skip a subsequent entry, and the body can poll
    // `SlotContext::shutdown_requested()` if it wants to bail out
    // promptly.  We do NOT flip the manager-wide `closing` flag (other
    // threads can still spawn).  Class-level wake-up (sockets, CVs)
    // must be done by the caller; ThreadManager has no visibility
    // into class-specific blocking mechanisms.
    ThreadSlot slot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(pImpl->mu);
        if (pImpl->closing.load(std::memory_order_acquire))
            return false; // drain() already ran — no slots to find
        for (auto it = pImpl->slots.begin(); it != pImpl->slots.end(); ++it)
        {
            if (it->name == name)
            {
                // Signal first — this is the join_named internal
                // shutdown_requested set per HEP-CORE-0031 §4.1.
                if (it->shutdown_requested)
                    it->shutdown_requested->store(true, std::memory_order_release);
                slot = std::move(*it);
                pImpl->slots.erase(it);
                found = true;
                break;
            }
        }
    }
    if (!found)
        return false;

    // Per-slot work delegates to the shared helper (two-stage wait +
    // join-or-detach + stage-differentiated ERROR log + process-wide
    // detach counter).  See HEP-CORE-0031 §4.1.
    return process_slot_bounded_join(slot, pImpl->composed_identity, "join_named");
}

// ============================================================================
// Accessors
// ============================================================================

std::size_t ThreadManager::active_count() const
{
    if (!pImpl)
        return 0;
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
    if (!pImpl)
        return out;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(pImpl->mu);
    out.reserve(pImpl->slots.size());
    for (const auto &slot : pImpl->slots)
    {
        ThreadInfo info;
        info.name = slot.name;
        info.alive = slot.thread.joinable() && !slot.done->load(std::memory_order_acquire);
        info.elapsed = now - slot.spawn_time;
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

std::size_t ThreadManager::detached_count_last_drain() const
{
    return pImpl ? pImpl->detached_count_last_drain.load(std::memory_order_acquire) : 0;
}

bool ThreadManager::all_detached_done() const noexcept
{
    if (!pImpl)
        return true;
    std::lock_guard<std::mutex> lock(pImpl->mu);
    for (const auto &flag : pImpl->detached_done_flags)
    {
        if (!flag)
            continue;
        if (!flag->load(std::memory_order_acquire))
            return false;
    }
    return true;
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
