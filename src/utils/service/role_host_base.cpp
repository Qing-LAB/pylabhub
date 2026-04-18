/**
 * @file role_host_base.cpp
 * @brief RoleHostBase — shared lifecycle scaffolding for role hosts.
 *
 * The worker-thread body (`worker_main_`) is role-specific and lives in
 * each derived class; this file owns only the setup/teardown pattern
 * around it — lazy construction of RoleAPIBase, worker spawn via the
 * role's own ThreadManager, shutdown contract enforcement.
 */
#include "utils/role_host_base.hpp"
#include "utils/debug_info.hpp"   // PLH_PANIC
#include "utils/role_api_base.hpp"
#include "utils/thread_manager.hpp"  // api_->thread_manager() inside startup_

#include <utility>

namespace pylabhub::scripting
{

RoleHostBase::RoleHostBase(std::string_view role_tag,
                             config::RoleConfig config,
                             std::unique_ptr<ScriptEngine> engine,
                             std::atomic<bool> *shutdown_flag)
    : role_tag_(role_tag)
    , config_(std::move(config))
    , engine_(std::move(engine))
{
    core_.set_shutdown_flag(shutdown_flag);
}

// ─── Destructor — contract enforcement ───────────────────────────────────────
//
// C++ guarantees this destructor body always runs whenever a RoleHostBase-
// derived object is destroyed (provided RoleHostBase's own ctor completed).
// This is true for every lifetime path — normal scope exit, exception
// unwinding, `delete`, `unique_ptr::reset`, container teardown, static/
// thread-local destruction. The pure-virtual marker does NOT change that;
// the definition below is invoked by every derived destructor's implicit
// base-destructor call. Therefore the check below IS always performed.
//
// Contract: every path that reaches this destructor must have called
// @ref shutdown_ at least once. If not, the worker thread (owned by
// api_->thread_manager()) may still be alive and referencing derived-
// owned members that were just destroyed — a silent use-after-free.
// Rather than paper over that with a defensive cleanup, we surface the
// bug loudly via `std::abort()` with a diagnostic.
//
RoleHostBase::~RoleHostBase()
{
    if (!shutdown_called_.load(std::memory_order_acquire))
    {
        // The uid may be empty if config was never parsed; PLH_PANIC
        // formats unconditionally so the message layout stays stable.
        PLH_PANIC(
            "RoleHostBase destructor entered without shutdown_() having "
            "been called. role_tag='{}' uid='{}'. Either the derived "
            "destructor did not call shutdown_() as its first statement, "
            "or an override of shutdown_() failed to call "
            "RoleHostBase::shutdown_(). The worker thread may still "
            "reference now-destroyed derived members; aborting to avoid "
            "silent use-after-free.",
            role_tag_, config_.identity().uid);
    }
}

void RoleHostBase::startup_()
{
    ready_promise_ = std::promise<bool>{};
    auto ready_future = ready_promise_.get_future();

    // Construct api_ here (not in ctor) so role_tag + uid are available
    // and the worker thread can be spawned under this api's ThreadManager.
    // role_tag_ is the short form ("prod"/"cons"/"proc") used by the
    // RoleAPIBase-owned ThreadManager name + log prefixes.
    api_ = std::make_unique<RoleAPIBase>(
        core_, std::string(role_tag_), config_.identity().uid);

    api_->thread_manager().spawn("worker", [this] { worker_main_(); });

    const bool ok = ready_future.get();
    if (!ok)
    {
        // Worker signaled setup failure. Run shutdown_ to execute the
        // cleanup path AND set the contract flag — otherwise the base
        // destructor would abort even though we did clean up.
        shutdown_();
    }
}

void RoleHostBase::shutdown_() noexcept
{
    // First effective call wins the exchange; subsequent calls see
    // "already true" and early-return. This makes shutdown_ idempotent
    // AND guarantees the contract flag is set exactly at the point
    // cleanup work has been initiated.
    if (shutdown_called_.exchange(true, std::memory_order_acq_rel))
        return;

    // Underlying ops themselves tolerate repeated calls (atomic store,
    // condvar notify, reset-of-null-unique_ptr), so even if someone
    // tries to race, the flag is the sole correctness gate here.
    core_.request_stop();
    core_.notify_incoming();
    // api_.reset() → RoleAPIBase dtor → ThreadManager dtor →
    // bounded join of every role-scope thread (worker, ctrl, custom).
    api_.reset();
}

} // namespace pylabhub::scripting
