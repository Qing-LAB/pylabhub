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
#include "utils/role_config_translation.hpp"   // make_tx_opts / make_rx_opts
#include "utils/role_host_core.hpp"
#include "utils/role_host_helpers.hpp"          // setup_inbox_facility
#include "utils/schema_utils.hpp"               // compute_schema_size

#include <optional>
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

// Note: `has_rx_fz()` / `has_tx_fz()` are intentionally NOT on the frame.
// They belong on `RoleAPIBase` next to the FlexzoneInfoCache
// the frame populates at setup time — script-side callers reach them
// through the API, not the frame.

// ============================================================================
// wire_api_for_presences_ — shared default impl, driven by presence role_kind
// ============================================================================
//
// Maps presence.channel onto the api's channel name accessors.  Handles
// the three current shapes (1-Producer, 1-Consumer, 1-Consumer+1-Producer);
// concrete role hosts override only when their presence shape doesn't fit
// the default mapping (future N-input router, etc.).

void RoleHostFrame::wire_api_for_presences_(
    const std::vector<scripting::Presence> &presences)
{
    auto &api_ref = api();

    const scripting::Presence *cons = nullptr;
    const scripting::Presence *prod = nullptr;
    for (const auto &p : presences)
    {
        if (p.role_kind == scripting::RoleKind::Consumer)      cons = &p;
        else if (p.role_kind == scripting::RoleKind::Producer) prod = &p;
    }

    if (cons && prod)
    {
        // Processor shape: consumer-side channel + producer-side out_channel.
        api_ref.set_channel(cons->channel);
        api_ref.set_out_channel(prod->channel);
    }
    else if (prod)
    {
        // Producer-only shape: single channel = the producer's channel.
        api_ref.set_channel(prod->channel);
    }
    else if (cons)
    {
        // Consumer-only shape: single channel = the consumer's channel.
        api_ref.set_channel(cons->channel);
    }
    // Else: presences empty — leave api channel state untouched.  Caller
    // will observe the missing wiring at setup_infrastructure_ (which
    // refuses to build queues for empty presence lists).
}

// ============================================================================
// setup_infrastructure_ — shared body, driven by presences_
// ============================================================================
//
// `presences_` MUST be populated by the caller (each role's
// `worker_main_` calls `build_presences_()` early).  Today asserts
// at most 1 rx and 1 tx presence — when API surface grows multi-rx/tx
// support, this assertion is removed and the queue-building loop
// iterates over all matching presences.

bool RoleHostFrame::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    auto       &core_   = this->core();
    const auto &config_ = this->config();
    auto       &api_ref = this->api();

    inbox_cfg_ = config_.inbox();

    // ── 1. Find rx and tx presences (today: ≤ 1 of each) ──
    const Presence *rx_presence = nullptr;
    const Presence *tx_presence = nullptr;
    for (const auto &p : presences_)
    {
        if (p.role_kind == RoleKind::Consumer)
        {
            if (rx_presence)
            {
                LOGGER_ERROR("[{}] multiple rx presences not yet supported "
                             "(today's API surface limits to 1 rx queue)",
                             frame_cfg_.role_tag);
                return false;
            }
            rx_presence = &p;
        }
        else if (p.role_kind == RoleKind::Producer)
        {
            if (tx_presence)
            {
                LOGGER_ERROR("[{}] multiple tx presences not yet supported",
                             frame_cfg_.role_tag);
                return false;
            }
            tx_presence = &p;
        }
    }

    // ── 2. Inbox setup (optional, independent of queue building) ──
    if (inbox_cfg_.has_inbox())
    {
        auto inbox_result = setup_inbox_facility(
            inbox_spec, inbox_cfg_, config_.checksum().policy,
            frame_cfg_.role_tag.c_str());
        if (!inbox_result)
        {
            LOGGER_ERROR("[{}] setup_inbox_facility failed",
                         frame_cfg_.role_tag);
            return false;
        }
        inbox_queue_ = std::move(inbox_result->queue);
    }

    // ── 3. Translate opts (per-presence schemas from presences_) ──
    std::optional<hub::TxQueueOptions> tx_opts;
    std::optional<hub::RxQueueOptions> rx_opts;
    if (tx_presence)
        tx_opts.emplace(make_tx_opts(
            config_, tx_presence->slot_spec, tx_presence->fz_spec,
            tx_presence->fz_spec.has_schema));
    if (rx_presence)
        rx_opts.emplace(make_rx_opts(
            config_, rx_presence->slot_spec, rx_presence->fz_spec,
            rx_presence->fz_spec.has_schema));

    // ── 4. Build queues (Rx first, then Tx — normalized order) ──
    if (rx_opts && !api_ref.build_rx_queue(*rx_opts))
    {
        LOGGER_ERROR("[{}] build_rx_queue failed for channel '{}'",
                     frame_cfg_.role_tag, config_.in_channel());
        return false;
    }
    if (tx_opts && !api_ref.build_tx_queue(*tx_opts))
    {
        LOGGER_ERROR("[{}] build_tx_queue failed for channel '{}'",
                     frame_cfg_.role_tag, config_.out_channel());
        return false;
    }

    // ── 5. Start queues + reset metrics ──
    if (rx_opts)
    {
        if (!api_ref.start_rx_queue())
        {
            LOGGER_ERROR("[{}] start_rx_queue failed for channel '{}'",
                         frame_cfg_.role_tag, config_.in_channel());
            return false;
        }
        api_ref.reset_rx_queue_metrics();
    }
    if (tx_opts)
    {
        if (!api_ref.start_tx_queue())
        {
            LOGGER_ERROR("[{}] start_tx_queue failed for channel '{}'",
                         frame_cfg_.role_tag, config_.out_channel());
            return false;
        }
        api_ref.reset_tx_queue_metrics();
    }

    // ── 6. Configured period (role-level) ──
    core_.set_configured_period(
        static_cast<uint64_t>(config_.timing().period_us));

    // ── 6.5 Flexzone introspection cache on RoleAPIBase ──
    // Populate exactly once, after build_*_queue succeeds.  Per side:
    //   logical  = compute_schema_size(spec, packing)        (struct size)
    //   physical = align_to_physical_page(logical)            (SHM region)
    // Both derived from the same Presence::fz_spec.  Script-API readers
    // (flexzone_{logical,physical}_size, has_*_fz, fz_info_cache)
    // see them at step 5+.  Invariant checked at engine startup entry.
    {
        RoleAPIBase::FlexzoneInfoCache fz_info;
        if (tx_presence)
        {
            fz_info.has_tx_fz        = tx_presence->fz_spec.has_schema;
            fz_info.tx_logical_size  =
                hub::compute_schema_size(tx_presence->fz_spec,
                                         tx_presence->fz_spec.packing);
            fz_info.tx_physical_size =
                hub::align_to_physical_page(fz_info.tx_logical_size);
        }
        if (rx_presence)
        {
            fz_info.has_rx_fz        = rx_presence->fz_spec.has_schema;
            fz_info.rx_logical_size  =
                hub::compute_schema_size(rx_presence->fz_spec,
                                         rx_presence->fz_spec.packing);
            fz_info.rx_physical_size =
                hub::align_to_physical_page(fz_info.rx_logical_size);
        }
        api_ref.set_flexzone_info_cache_(fz_info);
    }

    // ── 7. Startup log lines (per direction) ──
    if (rx_presence)
        LOGGER_INFO("[{}] rx on channel '{}' (shm={})",
                    frame_cfg_.role_tag, config_.in_channel(),
                    api_ref.rx_has_shm());
    if (tx_presence)
        LOGGER_INFO("[{}] tx on channel '{}' (shm={})",
                    frame_cfg_.role_tag, config_.out_channel(),
                    api_ref.tx_has_shm());

    return true;
}

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
    // crash).
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
