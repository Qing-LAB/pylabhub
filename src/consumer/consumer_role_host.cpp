/**
 * @file consumer_role_host.cpp
 * @brief ConsumerRoleHost — unified engine-agnostic consumer implementation.
 *
 * This is the canonical data loop for the consumer role.  It follows
 * docs/tech_draft/loop_design_unified.md §4 exactly.
 *
 * Layer 3 (infrastructure): BrokerRequestComm, Consumer, queue, ctrl_thread_, events.
 * Layer 2 (data loop): inner retry acquire, read, invoke, release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_consume / invoke_on_inbox.
 *
 * Shared lifecycle scaffolding (ctor, startup_(), shutdown_(), ready-promise,
 * RoleAPIBase ownership, ThreadManager-owned worker) lives in RoleHostBase.
 * This file implements only the consumer-specific worker_main_() hook and
 * its setup_infrastructure_() / teardown_infrastructure_() helpers.
 */
#include "consumer_role_host.hpp"
#include "utils/script_engine_factory.hpp"  // scripting::create_engine — worker_main_ Step 0
#include "utils/thread_manager.hpp"
#include "service/cycle_ops.hpp"
#include "service/data_loop.hpp"
#include "consumer_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/role_config_translation.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/role_handler.hpp"     // Wave-B M6: handler-mode startup
#include "utils/role_presence.hpp"    // Wave-B M6: Presence + RoleKind
#include "utils/schema_utils.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::consumer
{

using scripting::ConsumerCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ConsumerRoleHost::ConsumerRoleHost(config::RoleConfig config,
                                     std::atomic<bool> *shutdown_flag)
    : scripting::RoleHostFrame(std::move(config),
                                shutdown_flag,
                                { /*role_tag=*/         "cons",
                                  /*role_label=*/       "consumer",
                                  /*required_callback=*/"on_consume" })
{
    // Engine constructed in worker_main_ Step 0 — see HEP-CORE-0011
    // §"Engine Construction Lifecycle".
}

ConsumerRoleHost::~ConsumerRoleHost()
{
    // Join worker BEFORE consumer-specific members (broker_comm_,
    // inbox_queue_) begin destruction — the worker's teardown path
    // reads them. shutdown_() is idempotent; base dtor's flag check
    // therefore passes when it runs later.
    shutdown_();
}

// ============================================================================
// worker_main_ — the worker thread entry point (RoleHostBase hook)
// ============================================================================
//
// Sequence identical to the pre-migration standalone implementation; only
// the state it reads (config_, core_, engine_, api_, ready_promise_) is
// now accessed via protected RoleHostBase accessors instead of direct
// member references.

void ConsumerRoleHost::worker_main_()
{
    // GIL pickup — see HEP-CORE-0011 §"Engine Construction Lifecycle"
    // and the same comment block in producer_role_host.cpp.  Holds GIL
    // on this thread for worker_main_'s lifetime iff Python is in play.
    pylabhub::scripting::PythonGilLease gil_lease;

    // Step 0: construct engine on this worker thread (HEP-CORE-0011
    // §"Engine Construction Lifecycle").  GIL is held via the lease
    // above iff Python is configured.
    auto       &core_        = core();
    const auto &config_      = config();
    auto       &promise_ref0 = ready_promise();
    set_engine_(scripting::create_engine(config_.script()));
    if (!has_engine())
    {
        LOGGER_ERROR("[cons] scripting::create_engine returned null for "
                     "script.type='{}'", config_.script().type);
        promise_ref0.set_value(false);
        return;
    }
    auto       &engine_ref   = engine();
    auto       &api_ref      = api();
    auto       &promise_ref  = ready_promise();

    const auto &id   = config_.identity();
    const auto &sc   = config_.script();
    const auto &hub  = config_.in_hub();
    const auto &tr   = config_.in_transport();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[cons] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // ── Step 1a: Build presences (single resolve of channel schemas) ─────────
    // `build_presences_()` is the single resolver of channel schemas
    // (slot + fz).  presences_[i] is the canonical per-channel home;
    // every downstream consumer (wire-emission readers, params.*,
    // FlexzoneInfoCache populate) reads from it.
    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;

    try
    {
        presences_ = build_presences_(config_);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
        promise_ref.set_value(false);
        return;
    }
    // Consumer's build_presences_ returns exactly one Consumer-kind
    // presence on in_hub/in_channel.
    const auto &rx_presence = presences_[0];

    // Local refs into the canonical presence — used by wire-emission
    // readers + params + slot-size storage on core below.
    in_slot_spec_ = rx_presence.slot_spec;
    const hub::SchemaSpec &in_fz_local = rx_presence.fz_spec;

    // ── Step 1b: Inbox schema (role-level, not per-channel) ──────────────────
    // Inbox is not on a Presence (presences_ carries per-channel state).
    // Resolved separately if configured.
    hub::SchemaSpec inbox_spec_local;
    if (config_.inbox().has_inbox())
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());
        try
        {
            inbox_spec_local = hub::resolve_schema(
                config_.inbox().schema_json, false, "cons", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[cons] Inbox schema parse error: {}", e.what());
            promise_ref.set_value(false);
            return;
        }
    }

    // Packing is schema-driven (was transport-level `zmq_packing` pre-2026-04-20).
    const std::string packing =
        in_slot_spec_.has_schema ? in_slot_spec_.packing : "aligned";

    // Compute and store slot logical size on core (infrastructure-authoritative).
    // Flexzone sizes live on RoleAPIBase::FlexzoneInfoCache, populated by
    // RoleHostFrame::setup_infrastructure_ step 6.5 from the presence's fz_spec.
    if (in_slot_spec_.has_schema)
        core_.set_in_slot_spec(hub::SchemaSpec{in_slot_spec_},
                               hub::compute_schema_size(in_slot_spec_, packing));

    // ── Step 2a: Wire api_ identity + config (no infrastructure deps) ────────
    //
    // api_ was constructed in RoleHostBase::startup_() before the worker
    // was spawned so its ThreadManager could spawn this thread under
    // bounded join.  Here we populate the mutable post-ctor wiring:
    // name, channels, log level, script/role dirs, engine, checksum
    // policy.
    //
    // ORDERING (audit 2026-05-20, demo-harness discovery): the api_ state
    // wiring MUST happen BEFORE `setup_infrastructure_` because the SHM
    // reader / writer setup reads `pImpl->channel` to derive the SHM
    // block name.  Pre-fix this ran the other way around — empty channel
    // string → shm_create("", ...) → EINVAL → worker thread throws.  See
    // HEP-CORE-0011 §"worker_main_ phase ordering" for the canonical
    // sequence.  `set_inbox_queue` stays AFTER setup_infrastructure_
    // because the inbox queue object is created there.
    api_ref.set_name(id.name);
    api_ref.set_channel(config_.in_channel());
    api_ref.set_log_level(id.log_level);
    api_ref.set_script_dir(script_dir.string());
    api_ref.set_role_dir(config_.base_dir().string());
    api_ref.set_checksum_policy(config_.checksum().policy);
    api_ref.set_stop_on_script_error(sc.stop_on_script_error);
    api_ref.set_engine(&engine_ref);

    // ── Step 2b: Setup infrastructure (inherited from RoleHostFrame) ─────────
    // Skipped in validate-only mode (no broker/queue needed).

    if (!core_.is_validate_only())
    {
        if (!setup_infrastructure_(inbox_spec_local))
        {
            teardown_infrastructure_();
            promise_ref.set_value(false);
            return;
        }
        // Wave-B M6: BrokerRequestComm allocation moved into RoleHandler,
        // built lazily in Step 6 below.  See M5 (producer) for the same
        // pattern; consumer is the second role host to migrate.
        api_ref.set_inbox_queue(inbox_queue_.get());
    }

    // ── Step 4: Load engine via lifecycle startup ────────────────────────────

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = &engine_ref;
    params.api                = &api_ref;
    params.tag                = "cons";
    params.script_dir         = script_dir;
    // Audit B12 (2026-05-21): see producer_role_host.cpp comment.
    params.entry_point        = (sc.type == "lua")    ? "init.lua"
                              : (sc.type == "native") ? "plugin.so"
                                                      : "__init__.py";
    params.required_callback  = "on_consume";
    params.in_slot_spec       = in_slot_spec_;
    params.in_fz_spec         = in_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.in_packing         = packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Engine startup failed: {}", e.what());
        core_.set_script_load_ok(false);
        engine_ref.finalize();
        if (!core_.is_validate_only())
            teardown_infrastructure_();
        promise_ref.set_value(false);
        return;
    }

    // Validate-only mode: engine loaded successfully, exit.
    if (core_.is_validate_only())
    {
        engine_ref.finalize();
        promise_ref.set_value(true);
        return;
    }

    // ── Step 5: invoke on_init ───────────────────────────────────────────────
    engine_ref.invoke_on_init();

    // ── Step 6: Connect to broker, start handler ctrl thread(s), register
    // (Wave-B M6 — handler-mode replaces legacy start_ctrl_thread; mirrors
    // M5 producer migration with Consumer presence + register_consumer).
    core_.set_running(true);
    {
        // 6a — Build presence list: 1 Consumer presence on in_hub.
        std::vector<scripting::Presence> presences;
        {
            scripting::Presence p;
            p.hub       = config_.in_hub();
            p.channel   = config_.in_channel();
            p.role_kind = scripting::RoleKind::Consumer;
            presences.push_back(std::move(p));
        }

        // 6b — Auth + start handler threads.
        api_ref.set_auth(config_.auth().client_pubkey,
                         config_.auth().client_seckey);

        auto handler = std::make_unique<scripting::RoleHandler>(
            std::move(presences));

        if (!api_ref.start_handler_threads(std::move(handler)))
        {
            LOGGER_ERROR("[cons] start_handler_threads failed");
            promise_ref.set_value(false);
            return;
        }

        // 6c — Build CONSUMER_REG_REQ payload (HEP-CORE-0034 Phase 5b).
        const auto &ch = config_.in_channel();
        auto reg_opts = hub::build_consumer_reg_payload(
            hub::ConsumerRegInputs{ch, id.uid, id.name});

        // Citation fields (HEP-CORE-0034 §10.3) — named-mode vs anonymous
        // vs absent decided by the schema JSON shape; broker enforces the
        // mode rules.
        const auto &cf_for_wire = config_.role_data<consumer::ConsumerFields>();
        const auto wire_schema = hub::make_wire_schema_fields(
            cf_for_wire.in_slot_schema_json, in_slot_spec_, in_fz_local);
        hub::apply_consumer_schema_fields(reg_opts, wire_schema);

        // Inbox metadata (HEP-CORE-0034 §10.2; no-op if no inbox).
        api_ref.append_inbox_to_reg(reg_opts, inbox_cfg_);

        // 6d — CONSUMER_REG_REQ + heartbeat install.  register_consumer
        // transitions the matching Presence's `registration_state`
        // through RegRequestPending → Registered on success (audit
        // S1+O4, 2026-05-17 — replaces the pre-S1
        // `shared.consumer_channel` string).
        auto reg_result = api_ref.register_consumer(reg_opts);
        if (!reg_result.has_value() ||
            reg_result->value("status", std::string{}) != "success")
        {
            LOGGER_ERROR("[cons] Broker consumer registration failed — "
                         "broker won't track this consumer for liveness");
            // Non-fatal: consumer can still receive data via the producer's
            // direct ZMQ connection if discovery already happened.
        }
        else
        {
            auto hub_max = scripting::RoleAPIBase::extract_hub_heartbeat_max(*reg_result);
            api_ref.install_heartbeat(config_.timing().heartbeat_interval_ms,
                                       hub_max);
        }
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(api_ref, config_.startup().wait_for_roles, "[cons]"))
        {
            LOGGER_ERROR("[cons] Startup coordination failed — required roles not available");
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    promise_ref.set_value(true);

    // Step 8: Run the data loop via shared frame + ConsumerCycleOps.
    if (!api_ref.has_rx_side())
    {
        LOGGER_ERROR("[cons] run_data_loop: consumer not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        ConsumerCycleOps ops(api_ref, engine_ref, core_,
                             sc.stop_on_script_error);
        scripting::LoopConfig lcfg;
        lcfg.period_us                   = tc_loop.period_us;
        lcfg.loop_timing                 = tc_loop.loop_timing;
        lcfg.queue_io_wait_timeout_ratio = tc_loop.queue_io_wait_timeout_ratio;
        lcfg.release_global_lock_during_wait =
            engine_ref.release_global_lock_during_wait();
        scripting::run_data_loop(api_ref, core_, lcfg, ops);
    }

    // Steps 9-14: shared epilogue (HEP-CORE-0034 Phase 5c).
    scripting::do_role_teardown(
        engine_ref, api_ref, core_, has_api(),
        [this] { teardown_infrastructure_(); });
}

// ============================================================================
// make_rx_opts — delegates to the shared free function (M9 §11.6)
// ============================================================================

hub::RxQueueOptions
ConsumerRoleHost::make_rx_opts(const config::RoleConfig &config,
                                const hub::SchemaSpec    &in_slot_spec,
                                const hub::SchemaSpec    &in_fz_spec,
                                bool                      has_rx_fz)
{
    return scripting::make_rx_opts(config, in_slot_spec, in_fz_spec,
                                    has_rx_fz);
}

// ============================================================================
// setup_infrastructure_ + teardown_infrastructure_ — inherited from
// RoleHostFrame.  See src/utils/service/role_host_frame.cpp.
// ============================================================================

// ============================================================================
// build_presences_ — Consumer's per-role override
// ============================================================================

std::vector<scripting::Presence>
ConsumerRoleHost::build_presences_(const config::RoleConfig &c) const
{
    scripting::Presence p;
    p.hub       = c.in_hub();
    p.channel   = c.in_channel();
    p.role_kind = scripting::RoleKind::Consumer;

    std::vector<std::string> schema_dirs;
    if (!p.hub.hub_dir.empty())
        schema_dirs.push_back(
            (std::filesystem::path(p.hub.hub_dir) / "schemas").string());

    const auto &fields = c.role_data<ConsumerFields>();
    p.slot_spec = hub::resolve_schema(
        fields.in_slot_schema_json, false, "cons", schema_dirs);
    p.fz_spec = hub::resolve_schema(
        fields.in_flexzone_schema_json, true, "cons", schema_dirs);

    std::vector<scripting::Presence> v;
    v.push_back(std::move(p));
    return v;
}


} // namespace pylabhub::consumer
