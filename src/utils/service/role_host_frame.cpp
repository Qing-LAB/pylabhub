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
#include "utils/data_block.hpp"                 // datablock_layout_total_size + DataBlockConfig
#include "utils/security/attach_protocol.hpp"   // AttachProtocolAcceptor + ObserverPubkeyAccessor
#include "utils/security/key_store.hpp"         // secure().keys() + kRoleIdentityName
#include "utils/security/shm_attach_orchestrator.hpp"
#include "utils/security/shm_capability_channel.hpp"
#include "utils/thread_manager.hpp"             // ThreadManager::SlotContext

#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>

#include <unistd.h>     // ::getuid

namespace pylabhub::scripting
{

RoleHostFrame::RoleHostFrame(config::RoleConfig    config,
                              std::atomic<bool>    *shutdown_flag,
                              RoleHostFrameConfig   frame_cfg)
    : RoleHostBase(frame_cfg.short_tag,
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
                             frame_cfg_.short_tag);
                return false;
            }
            rx_presence = &p;
        }
        else if (p.role_kind == RoleKind::Producer)
        {
            if (tx_presence)
            {
                LOGGER_ERROR("[{}] multiple tx presences not yet supported",
                             frame_cfg_.short_tag);
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
            frame_cfg_.short_tag.c_str(), api_ref.uid());
        if (!inbox_result)
        {
            LOGGER_ERROR("[{}] setup_inbox_facility failed",
                         frame_cfg_.short_tag);
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
                     frame_cfg_.short_tag, config_.in_channel());
        return false;
    }
    if (tx_opts)
    {
        // HEP-CORE-0041 1i-mig-2: derived role host post-processes
        // `tx_opts` to (a) create the per-channel L1
        // IShmCapabilityProducer for SHM TX channels and (b) populate
        // `opts.shm_capability_fd` from the transport's borrowed fd.
        // No-op for ZMQ TX channels (default impl returns true).
        if (!prepare_tx_capability_(*tx_opts, config_.out_channel()))
        {
            LOGGER_ERROR(
                "[{}] prepare_tx_capability_ failed for channel '{}' "
                "— SHM L1 transport setup refused (HEP-CORE-0041 §6.1 + "
                "1i-mig-2)",
                frame_cfg_.short_tag, config_.out_channel());
            return false;
        }
        if (!api_ref.build_tx_queue(*tx_opts))
        {
            LOGGER_ERROR("[{}] build_tx_queue failed for channel '{}'",
                         frame_cfg_.short_tag, config_.out_channel());
            return false;
        }
    }

    // ── 5. Reset per-queue metrics ──
    //
    // Per HEP-CORE-0036 §3.5.5 S1, queues are built in Standby here
    // and not started.  The canonical Standby → Active driver is
    // `apply_master_approval(ACK)` invoked at S3 by the role host's
    // worker_main_ after the broker accepts the registration —
    // `apply_consumer_reg_ack` / `apply_producer_reg_ack` route
    // through the polymorphic queue mutator per §6.7 Option B.
    //
    // Metric counters are reset here regardless of side, so the role
    // host's metric-snapshot calls (`snapshot_metrics_json()`) reflect
    // only data observed after build.
    if (rx_opts)
        api_ref.reset_rx_queue_metrics();
    if (tx_opts)
        api_ref.reset_tx_queue_metrics();

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
                    frame_cfg_.short_tag, config_.in_channel(),
                    api_ref.rx_has_shm());
    if (tx_presence)
        LOGGER_INFO("[{}] tx on channel '{}' (shm={})",
                    frame_cfg_.short_tag, config_.out_channel(),
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
    // Called in TWO contexts (both correct — this method is self-contained):
    //   1. Normal path — as sub-step 9.6 of `do_role_teardown`, AFTER the data
    //      loop, AFTER `set_running(false)`, AFTER the ThreadManager drain.
    //   2. FATAL path ("H3b") — inline on any post-setup early return in a
    //      role host's `worker_main_` (e.g. start_handler_threads /
    //      register / apply_reg_ack / wait_for_roles failure), BEFORE any
    //      drain has run.  All three role hosts do this uniformly.
    // It is safe in BOTH because `stop_handler_threads()` below is idempotent
    // and performs its OWN drain of the peer ctrl threads (HEP-CORE-0031 §4.1
    // bracket contract) — a prior `drain()` is NOT a precondition.  A defensive
    // `set_running(false)` is likewise unnecessary here.

    // Lifecycle observability: this marker is what L4 tests read to confirm the
    // infrastructure unwind actually ran on the worker thread — in particular
    // that a post-setup FATAL early-return (H3b) tore down rather than deferring
    // sockets to destructor cleanup on another thread.  Production log line, not
    // a test hook.
    LOGGER_INFO("[{}] event=TeardownInfrastructure", frame_cfg_.short_tag);

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
                     frame_cfg_.short_tag);
    }

    // HEP-CORE-0041 1i-mig-2: release the L1 transport + L2b acceptor +
    // L2c orchestrator AFTER `api().close_queues()` has reset
    // `tx_queue` — the ShmQueue held a borrowed fd from the L1
    // transport, so the transport MUST outlive the queue's
    // destruction.  Default impl (1i-mig-2c M3 extraction) handles
    // LIFO release; no-op when shm_transport_ is null (ZMQ TX or no
    // TX presence).
    cleanup_tx_capability_();
}

// ============================================================================
// prepare_tx_capability_ — HEP-CORE-0041 1i-mig-M3.5 default impl
// ============================================================================
//
// Pre-1i-mig-M3.5 this body was duplicated byte-for-byte across
// ProducerRoleHost + ProcessorRoleHost (~70 LOC each, identical apart
// from log-prefix tag).  Promoted to RoleHostFrame default impl so
// both subclasses inherit one canonical copy; log tag derives from
// frame_cfg_.short_tag.  Override only if a future role host needs
// different L1 setup (e.g. a non-memfd backend).
//
// Called from RoleHostFrame::setup_infrastructure_ between
// make_tx_opts and build_tx_queue.  For SHM TX channels, creates the
// per-channel IShmCapabilityProducer (substep 1b backend), binds the
// Unix-socket endpoint that the broker echoes to consumers via
// CONSUMER_REG_ACK.shm_capability_endpoint (HEP-0041 §5.3), and stuffs
// the borrowed fd into opts.shm_capability_fd so build_tx_queue (2a's
// dispatch) wraps the same memfd via the substep 1f fd-source factory.
//
// No-op for ZMQ TX (returns true with opts unchanged).

bool RoleHostFrame::prepare_tx_capability_(hub::TxQueueOptions &tx_opts,
                                            const std::string   &tx_channel)
{
    if (!tx_opts.has_shm || tx_opts.data_transport != "shm")
        return true;

    // Replicate the DataBlockConfig that ShmQueue::start() will build
    // internally from create_writer_standby + the post-1i-mig-1
    // capability path.  datablock_layout_total_size() must agree with
    // ShmQueue's view exactly — the fd-source factory validates
    // fstat(fd).st_size == this value (HEP-CORE-0041 §6.3 +
    // data_block.hpp:1308).
    const auto slot_fields = hub::schema_spec_to_zmq_fields(tx_opts.slot_spec);
    auto [slot_layout, item_size] =
        hub::compute_field_layout(slot_fields, tx_opts.slot_spec.packing);
    size_t fz_size = 0;
    if (tx_opts.fz_spec.has_schema && !tx_opts.fz_spec.fields.empty())
    {
        const auto fz_fields = hub::schema_spec_to_zmq_fields(tx_opts.fz_spec);
        auto [fz_layout, raw_fz_size] =
            hub::compute_field_layout(fz_fields, tx_opts.fz_spec.packing);
        fz_size = hub::align_to_physical_page(raw_fz_size);
    }
    hub::DataBlockConfig cfg;
    cfg.logical_unit_size    = item_size;
    cfg.flex_zone_size       = fz_size;
    cfg.ring_buffer_capacity = tx_opts.shm_config.ring_buffer_capacity;
    cfg.physical_page_size   = tx_opts.shm_config.physical_page_size;
    cfg.policy               = tx_opts.shm_config.policy;
    cfg.consumer_sync_policy = tx_opts.shm_config.consumer_sync_policy;
    cfg.checksum_policy      = tx_opts.shm_config.checksum_policy;
    const size_t total = hub::datablock_layout_total_size(cfg);
    if (total == 0)
    {
        LOGGER_ERROR(
            "[{}] prepare_tx_capability_: datablock_layout_total_size "
            "returned 0 for channel '{}' (item_size={}, fz_size={}) — "
            "schema/config invariants violated",
            frame_cfg_.short_tag, tx_channel, item_size, fz_size);
        return false;
    }

    namespace sec = pylabhub::utils::security;
    shm_transport_ = sec::create_shm_capability_producer(total);
    if (!shm_transport_)
    {
        LOGGER_ERROR(
            "[{}] prepare_tx_capability_: create_shm_capability_producer "
            "failed for channel '{}' (size={}, HEP-CORE-0041 §6.3 L1)",
            frame_cfg_.short_tag, tx_channel, total);
        return false;
    }

    const auto endpoint = sec::default_shm_capability_endpoint(tx_channel);
    if (!shm_transport_->bind_endpoint(endpoint))
    {
        LOGGER_ERROR(
            "[{}] prepare_tx_capability_: bind_endpoint('{}') failed "
            "for channel '{}' (HEP-CORE-0041 §5.1 L1)",
            frame_cfg_.short_tag, endpoint, tx_channel);
        shm_transport_.reset();
        return false;
    }

    tx_opts.shm_capability_fd = shm_transport_->borrow_fd();
    LOGGER_INFO(
        "[{}] event=ShmCapabilityTransportBound channel='{}' endpoint='{}' "
        "size={} fd={} (HEP-CORE-0041 1i-mig)",
        frame_cfg_.short_tag, tx_channel, endpoint, total,
        tx_opts.shm_capability_fd);
    return true;
}

// ============================================================================
// cleanup_tx_capability_ — HEP-CORE-0041 1i-mig-2c M3 default impl
// ============================================================================
//
// Single owner of the SHM auth stack's teardown ordering, shared across
// every role host with a SHM TX presence.  Pre-1i-mig-2c this lived as
// a duplicated override on ProducerRoleHost; 1i-mig-3 (processor) would
// have needed a byte-equivalent copy.  Now both inherit the default.

void RoleHostFrame::cleanup_tx_capability_()
{
    // Release order: orchestrator → acceptor → transport.  Each holds
    // references (non-owning) to the one(s) below it.  By the time
    // this fires, teardown_infrastructure_ has called
    // `api().stop_handler_threads()` which drains the role host's
    // ThreadManager (HEP-CORE-0031 §4.1 Shutdown Contract) — including
    // the `shm_accept_loop` slot.  So no thread is still executing
    // inside the orchestrator when we reset it here.
    if (shm_orchestrator_)
    {
        LOGGER_INFO("[{}] event=ShmAttachOrchestratorReleased "
                    "(HEP-CORE-0041 1i-mig-2b-2)",
                    frame_cfg_.short_tag);
        shm_orchestrator_.reset();
    }
    if (shm_acceptor_)
    {
        shm_acceptor_.reset();
    }
    if (shm_transport_)
    {
        LOGGER_INFO("[{}] event=ShmCapabilityTransportReleased "
                    "(HEP-CORE-0041 1i-mig-2)",
                    frame_cfg_.short_tag);
        shm_transport_.reset();
    }
}

// ============================================================================
// spawn_shm_auth_listener_ — HEP-CORE-0041 1i-mig-2c M3 extraction
// ============================================================================
//
// Builds the L2b AttachProtocolAcceptor + L2c ShmAttachOrchestrator on
// top of shm_transport_, then spawns the accept thread on
// `api().thread_manager()`.  Shared across ProducerRoleHost + the
// future ProcessorRoleHost — both call this exactly once, after
// `apply_producer_reg_ack` succeeds (the orchestrator's CacheLookup
// reads the allowlist cache seeded by REG_ACK).

bool RoleHostFrame::spawn_shm_auth_listener_()
{
    namespace sec = pylabhub::utils::security;

    if (!shm_transport_)
    {
        LOGGER_ERROR("[{}] spawn_shm_auth_listener_: shm_transport_ is "
                     "null — prepare_tx_capability_ must run first "
                     "(HEP-CORE-0041 1i-mig-2c)",
                     frame_cfg_.short_tag);
        return false;
    }

    auto &api_ref = this->api();
    const std::string tx_ch        = this->config().out_channel();
    const std::string producer_uid = api_ref.uid();

    // HEP-CORE-0041 §D1(d) observer pubkey accessor (task #317 C.2.b).
    // The acceptor calls this ONCE per observer handshake to fetch the
    // broker observer pubkey the producer trusts (learned via REG_ACK
    // per task #317 D2 / f7d3a51e).  Snapshotted under RoleAPIBase's
    // shared_mutex — thread-safe.  Empty return → observer handshakes
    // rejected with a clear diagnostic; broker's SHM metrics path
    // falls back to the heartbeat source.
    sec::ObserverPubkeyAccessor observer_pubkey_accessor =
        [&api_ref]() -> std::string {
            return api_ref.broker_observer_pubkey_z85();
        };

    try
    {
        shm_acceptor_ = std::make_unique<sec::AttachProtocolAcceptor>(
            *shm_transport_,
            ::getuid(),  // SO_PEERCRED uid sanity (HEP-0036 §I8)
            std::string(sec::kRoleIdentityName),  // KeyStore name — SMS reads it
            std::move(observer_pubkey_accessor));
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] AttachProtocolAcceptor ctor threw for "
                     "channel '{}': {}",
                     frame_cfg_.short_tag, tx_ch, e.what());
        return false;
    }

    // CacheLookup: the role's local script-observable cache
    // (HEP-CORE-0036 §I11.1 — one cache scripts read).
    sec::ShmAttachOrchestrator::CacheLookup cache_lookup =
        [&api_ref, tx_ch](const std::string &pk) {
            const auto peers = api_ref.allowed_peers(tx_ch);
            return std::any_of(peers.begin(), peers.end(),
                [&](const auto &p) { return p.pubkey == pk; });
        };

    // BrokerQuery: route through RoleAPIBase::consumer_attach, which
    // finds the right BRC for this channel and runs the sync
    // CONSUMER_ATTACH_REQ_SHM.  Producer role_uid identifies the role to
    // the broker so it can correlate the channel.
    // EDGE-2 (REVIEW-A close-out #280) — consumer_attach timeout MUST
    // be < the 5s `wait_for_quiescence` budget in
    // `RoleAPIBase::stop_handler_threads` Phase 3
    // (`role_api_base.cpp:2226`).  If consumer_attach blocks longer
    // than 5s, the quiescence wait expires, Phase 4 destroys BRCs,
    // and the still-blocked accept thread reads freed BRC pImpl.
    // The `ctx.with_active_loop(...)` bracket below (also EDGE-2)
    // primarily prevents this — quiescence waits properly — but the
    // shorter timeout is belt-and-suspenders: if the bracket were
    // somehow missed (future regression), the request would still
    // time out before quiescence expires.
    sec::ShmAttachOrchestrator::BrokerQuery broker_query =
        [&api_ref, tx_ch, producer_uid](
            const std::string &consumer_pk,
            const std::string &consumer_role_uid) {
            return api_ref.consumer_attach(
                tx_ch, consumer_pk, consumer_role_uid,
                producer_uid, /*timeout_ms=*/2000);
        };

    sec::ShmAttachOrchestrator::Config cfg{
        tx_ch, producer_uid,
        std::move(cache_lookup), std::move(broker_query)};

    try
    {
        shm_orchestrator_ = std::make_unique<sec::ShmAttachOrchestrator>(
            *shm_acceptor_, *shm_transport_, std::move(cfg));
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] ShmAttachOrchestrator ctor threw for "
                     "channel '{}': {}",
                     frame_cfg_.short_tag, tx_ch, e.what());
        shm_acceptor_.reset();
        return false;
    }

    // HEP-CORE-0031 §2 (role-scope thread) + §4.1 Shutdown Contract:
    // spawn on api.thread_manager() with the SlotContext-aware
    // overload.  Poll ctx.shutdown_requested() with a 100 ms accept
    // timeout so graceful teardown is bounded.
    //
    // EDGE-2 (REVIEW-A close-out #280) — the whole loop body lives
    // inside `ctx.with_active_loop(...)` so `wait_for_quiescence`
    // in `stop_handler_threads` Phase 3 properly waits for the
    // in-flight accept to complete BEFORE Phase 4 destroys BRCs.
    // Pre-#280: the bare `while (!ctx.shutdown_requested())` body
    // never engaged the bracket → wait_for_quiescence skipped the
    // slot (depth=0 forever) → Phase 4 destroyed BRCs while the
    // accept thread could be mid-`broker_query` (consumer_attach
    // CV wait on BRC's cmd_queue) → read-after-free.  The bracket
    // makes `active_loop_depth > 0` while we're inside the body,
    // so quiescence wait blocks until the body returns (which it
    // does promptly after shutdown_requested is set + the current
    // 100 ms accept_and_serve_one round completes).  Per HEP-0031
    // §4.1 rule 3, a thread picks the bracket form OR
    // `mark_active_loop_exited()` — we use the bracket because
    // the entire while-loop is the critical region.
    const bool spawned = api_ref.thread_manager().spawn(
        "shm_accept_loop",
        [this](pylabhub::utils::ThreadManager::SlotContext &ctx) {
            ctx.with_active_loop([&] {
                while (!ctx.shutdown_requested())
                {
                    if (!shm_orchestrator_) break;
                    // Per-iteration isolation (HEP-CORE-0041
                    // 1i-mig-2c H2): the orchestrator catches
                    // handshake-level errors internally, but
                    // post-broker-query paths (LOGGER allocation,
                    // send_capability edge cases) can still
                    // escape.  Catch + continue keeps the loop
                    // alive across one bad attach.
                    try
                    {
                        (void)shm_orchestrator_->accept_and_serve_one(
                            std::chrono::milliseconds(100));
                    }
                    catch (const std::exception &e)
                    {
                        LOGGER_WARN(
                            "[{}] shm_accept_loop iteration threw "
                            "'{}' — continuing (HEP-CORE-0041 §9 "
                            "D4 per-attach isolation)",
                            frame_cfg_.short_tag, e.what());
                    }
                }
            });
        });
    if (!spawned)
    {
        LOGGER_ERROR("[{}] failed to spawn shm_accept_loop on "
                     "ThreadManager for channel '{}' "
                     "(HEP-CORE-0041 1i-mig-2b-2)",
                     frame_cfg_.short_tag, tx_ch);
        shm_orchestrator_.reset();
        shm_acceptor_.reset();
        return false;
    }

    LOGGER_INFO("[{}] event=ShmAcceptLoopSpawned channel='{}' "
                "(HEP-CORE-0041 §9 D4 + 1i-mig-2b-2)",
                frame_cfg_.short_tag, tx_ch);
    return true;
}

} // namespace pylabhub::scripting
