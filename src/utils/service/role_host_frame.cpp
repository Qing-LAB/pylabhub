/**
 * @file role_host_frame.cpp
 * @brief RoleHostFrame definitions.
 *
 * See role_host_frame.hpp for full design context and the M9 staged-
 * migration plan.  This file currently provides only the constructor
 * and destructor for the skeleton; subsequent migration sub-steps
 * (2b/2c/2d) will add setup_infrastructure_, teardown_infrastructure_,
 * and worker_main_ bodies as they're absorbed from the three role
 * hosts.
 */

#include "utils/role_host_frame.hpp"

#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include <utility>

namespace pylabhub::scripting
{

RoleHostFrame::RoleHostFrame(config::RoleConfig    config,
                              std::atomic<bool>    *shutdown_flag,
                              RoleHostFrameConfig   frame_cfg)
    : RoleHostBase(frame_cfg.role_tag,
                   std::move(config),
                   shutdown_flag),
      frame_cfg_(std::move(frame_cfg))
{
}

RoleHostFrame::~RoleHostFrame() = default;

// ============================================================================
// teardown_infrastructure_ — shared body (M9 sub-step 2b, 2026-05-22)
// ============================================================================
//
// Absorbed from the three role hosts' byte-equivalent teardown bodies.
// Pre-M9 each role host had its own copy; the bodies differed only in
// comments.  Now lives here once.

void RoleHostFrame::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->thread_manager().drain().
    // set_running(false) also already called.  Defensive re-set is safe.

    // Clean up shared resources (engine already finalized — no scripts running).
    core().clear_inbox_cache();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Wave-B M5..M7: handler-mode teardown + data-plane queue close.
    // The role host no longer owns a `broker_comm_` unique_ptr — the
    // RoleHandler inside RoleAPIBase owns every BRC (1 for single-hub
    // roles, 2 for dual-hub processor).
    //
    //   `stop_handler_threads()` — full sequence: clear the legacy
    //     fallback view, signal each BRC, drain peer ctrl threads via
    //     the HEP-CORE-0031 §4.1 bracket contract, disconnect +
    //     release BRCs, reset the handler unique_ptr.  Idempotent.
    //   `close_queues()` — Tx / Rx data-plane teardown inside
    //     RoleAPIBase; safe for any role-side combination.
    //
    // Both safe after `do_role_teardown`'s Step 12.5 wait_for_quiescence
    // (HEP-CORE-0031 §4.1, MD1 fix).  The actual std::thread::join for
    // master ctrl threads happens later in EngineHost::shutdown_()
    // Phase 3.
    //
    // Lifecycle invariant: `api_` is constructed by `startup_()` BEFORE
    // the worker thread is spawned, and destroyed only AFTER the worker
    // is joined.  So `has_api()` should always be true while
    // `worker_main_` (and therefore this method) is running.  If it
    // ever isn't, that's a serious lifecycle bug upstream — record it
    // via LOGGER_ERROR (not panic, since shutdown is the wrong time
    // to abort) and skip the calls (calling them on a null api_ would
    // crash).  M9 step 2b (2026-05-22) consolidated the two historical
    // `if (has_api())` checks into one block + added the diagnostic
    // path.
    if (has_api())
    {
        api().stop_handler_threads();
        api().close_queues();
    }
    else
    {
        LOGGER_ERROR("[{}] teardown_infrastructure_ called with no api — "
                     "lifecycle invariant violated (api_ must be "
                     "constructed by startup_() before the worker spawns "
                     "and destroyed only after the worker is joined).  "
                     "Skipping handler-thread stop + queue close.",
                     frame_cfg_.role_tag);
    }
}

} // namespace pylabhub::scripting
