/**
 * @file processor_role_host.cpp
 * @brief ProcessorRoleHost — unified engine-agnostic processor implementation.
 *
 * This is the canonical data loop for the processor role.  It follows
 * docs/tech_draft/loop_design_unified.md §5 exactly.
 *
 * Layer 3 (infrastructure): BrokerRequestComm, Consumer + Producer, dual queues,
 *   ctrl_thread_, events.
 * Layer 2 (data loop): dual-queue inner retry acquire, deadline wait, drain,
 *   invoke, commit/release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_process / invoke_on_inbox.
 *
 * Shared lifecycle scaffolding (ctor, startup_(), shutdown_(), ready-promise,
 * RoleAPIBase ownership, ThreadManager-owned worker) lives in RoleHostBase.
 * This file implements only the processor-specific worker_main_() hook and
 * its setup_infrastructure_() / teardown_infrastructure_() helpers.
 */
#include "processor_role_host.hpp"
#include "utils/script_engine_factory.hpp"  // scripting::create_engine — worker_main_ Step 0
#include "utils/thread_manager.hpp"
#include "service/cycle_ops.hpp"
#include "service/data_loop.hpp"
#include "utils/broker_request_comm.hpp"
#include "processor_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/role_handler.hpp"     // Wave-B M7: handler-mode startup
#include "utils/role_presence.hpp"    // Wave-B M7: Presence + RoleKind
#include "utils/schema_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::processor
{

using scripting::ProcessorCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ProcessorRoleHost::ProcessorRoleHost(config::RoleConfig config,
                                       std::atomic<bool> *shutdown_flag)
    : scripting::RoleHostBase("proc",
                              std::move(config),
                              shutdown_flag)
{
    // Engine constructed in worker_main_ Step 0 — see HEP-CORE-0011
    // §"Engine Construction Lifecycle".
}

ProcessorRoleHost::~ProcessorRoleHost()
{
    // Join worker BEFORE processor-specific members (broker_comm_,
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

void ProcessorRoleHost::worker_main_()
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
    auto       &promise_ref  = ready_promise();
    set_engine_(scripting::create_engine(config_.script()));
    if (!has_engine())
    {
        LOGGER_ERROR("[proc] scripting::create_engine returned null for "
                     "script.type='{}'", config_.script().type);
        promise_ref.set_value(false);
        return;
    }
    auto       &engine_ref   = engine();
    auto       &api_ref      = api();

    const auto &sc = config_.script();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[proc] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // ── Step 1: Resolve schemas from config ──────────────────────────────────

    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;

    hub::SchemaSpec out_fz_local;
    hub::SchemaSpec in_fz_local;
    hub::SchemaSpec inbox_spec_local;
    {
        std::vector<std::string> schema_dirs;
        auto add_schema_dir = [&schema_dirs](const std::string &hub_dir) {
            if (hub_dir.empty())
                return;
            const std::string d = (std::filesystem::path(hub_dir) / "schemas").string();
            if (std::find(schema_dirs.begin(), schema_dirs.end(), d) == schema_dirs.end())
                schema_dirs.push_back(d);
        };
        add_schema_dir(config_.in_hub().hub_dir);
        add_schema_dir(config_.out_hub().hub_dir);

        const auto &pf = config_.role_data<ProcessorFields>();
        try
        {
            in_slot_spec_  = hub::resolve_schema(
                pf.in_slot_schema_json, false, "proc", schema_dirs);
            out_slot_spec_ = hub::resolve_schema(
                pf.out_slot_schema_json, false, "proc", schema_dirs);
            in_fz_local    = hub::resolve_schema(
                pf.in_flexzone_schema_json, true, "proc", schema_dirs);
            out_fz_local   = hub::resolve_schema(
                pf.out_flexzone_schema_json, true, "proc", schema_dirs);
            if (config_.inbox().has_inbox())
                inbox_spec_local = hub::resolve_schema(
                    config_.inbox().schema_json, false, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
            promise_ref.set_value(false);
            return;
        }
    }

    // Packing is schema-driven (was transport-level `zmq_packing` pre-2026-04-20).
    // Input and output sides can have different packings; each is sourced from
    // its own schema spec.
    const std::string in_packing =
        in_slot_spec_.has_schema ? in_slot_spec_.packing : "aligned";
    const std::string out_packing =
        out_slot_spec_.has_schema ? out_slot_spec_.packing : "aligned";

    // Compute and store sizes (infrastructure-authoritative).
    if (in_slot_spec_.has_schema)
        core_.set_in_slot_spec(hub::SchemaSpec{in_slot_spec_},
                               hub::compute_schema_size(in_slot_spec_, in_packing));
    if (out_slot_spec_.has_schema)
        core_.set_out_slot_spec(hub::SchemaSpec{out_slot_spec_},
                                hub::compute_schema_size(out_slot_spec_, out_packing));
    {
        size_t out_fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(out_fz_local, out_packing));
        core_.set_out_fz_spec(hub::SchemaSpec{out_fz_local}, out_fz_size);
    }
    {
        size_t in_fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(in_fz_local, in_packing));
        core_.set_in_fz_spec(hub::SchemaSpec{in_fz_local}, in_fz_size);
    }

    // ── Step 2a: Wire api_ identity + config (no infrastructure deps) ────────
    //
    // api_ was constructed in RoleHostBase::startup_() before the worker
    // was spawned so its ThreadManager could own this worker thread
    // (bounded join on teardown). Populate post-ctor state here.
    //
    // ORDERING (audit 2026-05-20, demo-harness discovery): the api_ state
    // wiring MUST happen BEFORE `setup_infrastructure_` because both the
    // input SHM reader and output SHM writer setup read `pImpl->channel`
    // / `pImpl->out_channel` to derive their SHM block names.  Pre-fix
    // this ran the other way around — empty channel strings →
    // shm_create("", ...) → EINVAL → worker thread throws.  See
    // HEP-CORE-0011 §"worker_main_ phase ordering" for the canonical
    // sequence.  `set_inbox_queue` stays AFTER setup_infrastructure_
    // because the inbox queue object is created there.
    api_ref.set_name(config_.identity().name);
    api_ref.set_channel(config_.in_channel());
    api_ref.set_out_channel(config_.out_channel());
    api_ref.set_log_level(config_.identity().log_level);
    api_ref.set_script_dir(script_dir.string());
    api_ref.set_role_dir(config_.base_dir().string());
    api_ref.set_checksum_policy(config_.checksum().policy);
    api_ref.set_stop_on_script_error(sc.stop_on_script_error);
    api_ref.set_engine(&engine_ref);

    // ── Step 2b: Setup infrastructure (depends on api state set above) ───────
    // Skipped in validate-only mode (no broker/queue needed).
    if (!core_.is_validate_only())
    {
        if (!setup_infrastructure_(inbox_spec_local))
        {
            teardown_infrastructure_();
            promise_ref.set_value(false);
            return;
        }
        // Wave-B M7: BrokerRequestComm allocation moved into RoleHandler,
        // built lazily in Step 6 below.  See M5 (producer) for the same
        // pattern.  Processor declares a 2-presence list — RoleHandler
        // dedups to 1 connection for single-hub processor (the common
        // case), 2 connections for dual-hub (M8).
        api_ref.set_inbox_queue(inbox_queue_.get());
    }

    // ── Step 4: Load engine via lifecycle startup ────────────────────────────

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, config_.identity().uid);

    scripting::EngineModuleParams params;
    params.engine             = &engine_ref;
    params.api                = &api_ref;
    params.tag                = "proc";
    params.script_dir         = script_dir;
    params.entry_point        = (sc.type == "lua") ? "init.lua" : "__init__.py";
    params.required_callback  = "on_process";
    params.in_slot_spec       = in_slot_spec_;
    params.out_slot_spec      = out_slot_spec_;
    params.in_fz_spec         = in_fz_local;
    params.out_fz_spec        = out_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.in_packing         = in_packing;
    params.out_packing        = out_packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Engine startup failed: {}", e.what());
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

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (api_ref.has_tx_side() && core_.has_tx_fz())
        api_ref.sync_tx_flexzone_checksum();

    // ── Step 6: Connect to broker(s), start handler ctrl thread(s), register
    // (Wave-B M7 — handler-mode replaces legacy start_ctrl_thread; first
    // 2-presence migration.  Per HEP-CORE-0033 §19, RoleHandler dedups
    // presences by (broker_endpoint, broker_pubkey) — single-hub
    // processor where in_hub == out_hub gets 1 connection, dual-hub
    // gets 2.  Wire trace for the single-hub case is byte-identical to
    // legacy because both REGs go through the same BRC; dual-hub
    // routing is the M8 payoff that this commit unlocks.)
    core_.set_running(true);
    {
        const auto &id  = config_.identity();
        const auto &shm = config_.out_shm();
        const auto &tr  = config_.out_transport();

        // 6a — Build 2-presence list: consumer (in_channel on in_hub)
        // + producer (out_channel on out_hub).
        std::vector<scripting::Presence> presences;
        {
            scripting::Presence cons;
            cons.hub       = config_.in_hub();
            cons.channel   = config_.in_channel();
            cons.role_kind = scripting::RoleKind::Consumer;
            presences.push_back(std::move(cons));

            scripting::Presence prod;
            prod.hub       = config_.out_hub();
            prod.channel   = config_.out_channel();
            prod.role_kind = scripting::RoleKind::Producer;
            presences.push_back(std::move(prod));
        }

        // 6b — Auth + start handler threads.  RoleHandler dedups
        // connections; single-hub processor → 1 BRC, dual-hub → 2.
        api_ref.set_auth(config_.auth().client_pubkey,
                         config_.auth().client_seckey);

        auto handler = std::make_unique<scripting::RoleHandler>(
            std::move(presences));

        if (!api_ref.start_handler_threads(std::move(handler)))
        {
            LOGGER_ERROR("[proc] start_handler_threads failed");
            promise_ref.set_value(false);
            return;
        }

        // 6c — Build BOTH registration payloads (HEP-CORE-0034 Phase 5b).
        // Output producer REG_REQ — routes via brc_for_channel(out_channel)
        // which the M3 dedup maps to the out_hub's BRC.
        nlohmann::json prod_reg;
        {
            hub::ProducerRegInputs reg_in;
            reg_in.channel           = config_.out_channel();
            reg_in.role_uid          = id.uid;
            reg_in.role_name         = id.name;
            reg_in.role_tag          = "processor";
            reg_in.has_shm           = shm.enabled;
            reg_in.is_zmq_transport  = (tr.transport == config::Transport::Zmq);
            reg_in.zmq_node_endpoint = tr.zmq_endpoint;
            prod_reg = hub::build_producer_reg_payload(reg_in);
        }
        const auto &pf_for_wire = config_.role_data<ProcessorFields>();
        const auto out_wire = hub::make_wire_schema_fields(
            pf_for_wire.out_slot_schema_json, out_slot_spec_, core_.out_fz_spec());
        hub::apply_producer_schema_fields(prod_reg, out_wire);
        api_ref.append_inbox_to_reg(prod_reg, inbox_cfg_);

        // Input consumer CONSUMER_REG_REQ — routes via brc_for_channel(in_channel)
        // which maps to the in_hub's BRC (or out_hub's BRC if dedup'd).
        auto cons_reg = hub::build_consumer_reg_payload(
            hub::ConsumerRegInputs{config_.in_channel(), id.uid, id.name});
        const auto in_wire = hub::make_wire_schema_fields(
            pf_for_wire.in_slot_schema_json, in_slot_spec_, core_.in_fz_spec());
        hub::apply_consumer_schema_fields(cons_reg, in_wire);
        api_ref.append_inbox_to_reg(cons_reg, inbox_cfg_);

        // 6d — Send both REGs (auto-record via M5a) + install heartbeat.
        // We capture the hub-max from whichever REG returns it last,
        // mirroring legacy behaviour (the legacy code path captured
        // both REGs' heartbeat blocks and let the second overwrite the
        // first; same precedence here).
        std::optional<int> hub_max;

        auto prod_result = api_ref.register_producer_channel(prod_reg);
        if (!prod_result.has_value() ||
            prod_result->value("status", std::string{}) != "success")
        {
            LOGGER_ERROR("[proc] Output producer registration failed");
            // Non-fatal: data loop may still run if discovery already happened.
        }
        else
        {
            auto m = scripting::RoleAPIBase::extract_hub_heartbeat_max(*prod_result);
            if (m.has_value()) hub_max = m;
        }

        auto cons_result = api_ref.register_consumer(cons_reg);
        if (!cons_result.has_value() ||
            cons_result->value("status", std::string{}) != "success")
        {
            LOGGER_ERROR("[proc] Input consumer registration failed");
        }
        else
        {
            auto m = scripting::RoleAPIBase::extract_hub_heartbeat_max(*cons_result);
            if (m.has_value()) hub_max = m;  // consumer's wins (legacy parity)
        }

        api_ref.install_heartbeat(config_.timing().heartbeat_interval_ms,
                                   hub_max);
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(api_ref, config_.startup().wait_for_roles, "[proc]"))
        {
            LOGGER_ERROR("[proc] Startup coordination failed — required roles not available");
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    promise_ref.set_value(true);

    // Step 8: Run the data loop via shared frame + ProcessorCycleOps.
    if (!api_ref.has_rx_side() || !api_ref.has_tx_side())
    {
        LOGGER_ERROR("[proc] run_data_loop: transport queues not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        const bool drop_mode =
            (config_.out_transport().zmq_overflow_policy == "drop");
        ProcessorCycleOps ops(api_ref, engine_ref, core_,
                              sc.stop_on_script_error,
                              drop_mode);
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
// setup_infrastructure_ — dual broker connect, consumer + producer, wire events
// ============================================================================

bool ProcessorRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    auto       &core_   = core();
    const auto &config_ = config();
    auto       &api_ref = api();

    // ── Consumer side (in_channel) ──────────────────────────────────────────
    // channel_name / consumer_uid / consumer_name removed from opts —
    // build_rx_queue reads those from RoleAPIBase state.
    hub::RxQueueOptions in_opts;
    // Audit B5/G21 (2026-05-20, demo-harness discovery): see corresponding
    // comment in consumer_role_host.cpp.  shm_name = in_channel; the
    // producer's `ShmQueue::create_writer` takes the channel name as its
    // first arg, so the SHM block we attach to here lives at that name.
    in_opts.shm_name             = config_.in_channel();
    in_opts.shm_shared_secret    = config_.in_shm().enabled ? config_.in_shm().secret : 0u;
    in_opts.slot_spec            = in_slot_spec_;       // fields + packing
    in_opts.fz_spec              = core_.in_fz_spec();  // schema-hash match
    in_opts.zmq_buffer_depth     = config_.in_transport().zmq_buffer_depth;
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    in_opts.checksum_policy      = config_.checksum().policy;
    in_opts.flexzone_checksum    = config_.checksum().flexzone && core_.has_rx_fz();
    if (!api_ref.build_rx_queue(in_opts))
    {
        LOGGER_ERROR("[proc] Failed to connect consumer to in_channel '{}'",
                     config_.in_channel());
        return false;
    }

    // ── Producer side (out_channel) ─────────────────────────────────────────
    // channel_name / role_name / role_uid removed from opts — build_tx_queue
    // reads those from RoleAPIBase state.
    hub::TxQueueOptions out_opts;
    out_opts.has_shm       = config_.out_shm().enabled;
    out_opts.slot_spec     = out_slot_spec_;
    out_opts.fz_spec       = core_.out_fz_spec();
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    out_opts.checksum_policy    = config_.checksum().policy;
    out_opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_tx_fz();
    // SHM config (output side).
    if (config_.out_shm().enabled)
    {
        out_opts.shm_config.shared_secret        = config_.out_shm().secret;
        out_opts.shm_config.ring_buffer_capacity = config_.out_shm().slot_count;
        out_opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        out_opts.shm_config.consumer_sync_policy = config_.out_shm().sync_policy;
        out_opts.shm_config.checksum_policy      = config_.checksum().policy;
        out_opts.shm_config.physical_page_size = hub::system_page_size();
    }

    // HEP-CORE-0021: for ZMQ output, register the peer endpoint with the broker.
    if (config_.out_transport().transport == config::Transport::Zmq)
    {
        out_opts.has_shm           = false;
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.out_transport().zmq_endpoint;
        out_opts.zmq_bind          = config_.out_transport().zmq_bind;
        out_opts.zmq_buffer_depth  = config_.out_transport().zmq_buffer_depth;
        out_opts.zmq_overflow_policy =
            (config_.out_transport().zmq_overflow_policy == "block")
                ? hub::OverflowPolicy::Block
                : hub::OverflowPolicy::Drop;
    }

    // ── Inbox facility (optional) ───────────────────────────────────────────
    inbox_cfg_ = config_.inbox();
    if (inbox_cfg_.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox_cfg_, config_.checksum().policy, "proc");
        if (!inbox_result)
            return false;
        inbox_queue_ = std::move(inbox_result->queue);
    }

    // --- Create producer (RoleAPIBase owns the Tx queue) ---
    if (!api_ref.build_tx_queue(out_opts))
    {
        LOGGER_ERROR("[proc] Failed to create producer for out_channel '{}'",
                     config_.out_channel());
        return false;
    }

    // Broker notifications (band, hub-dead) are handled by BrokerRequestComm.

    // --- Start and configure data queues ---
    if (!api_ref.start_rx_queue())
    {
        LOGGER_ERROR("[proc] Input start_queue() failed (in_channel='{}')",
                     config_.in_channel());
        return false;
    }
    if (!api_ref.start_tx_queue())
    {
        LOGGER_ERROR("[proc] Output start_queue() failed (out_channel='{}')",
                     config_.out_channel());
        return false;
    }
    // Reset metrics (checksum and period already configured via Options).
    api_ref.reset_rx_queue_metrics();
    api_ref.reset_tx_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(config_.timing().period_us));

    LOGGER_INFO("[proc] Processor started: '{}' -> '{}'",
                config_.in_channel(), config_.out_channel());

    // Startup coordination (HEP-0023) moved to after start_ctrl_thread().
    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProcessorRoleHost::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->thread_manager().drain().

    core().clear_inbox_cache();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Wave-B M7: handler-mode teardown.  RoleHandler inside RoleAPIBase
    // owns every BRC (1 for single-hub processor, 2 for dual-hub);
    // api.stop_handler_threads() does the full signal/drain/disconnect/
    // release sequence for all of them.  Safe AFTER do_role_teardown's
    // Step 12.5 wait_for_quiescence (HEP-CORE-0031 §4.1, MD1 fix); the
    // actual std::thread::join for master ctrl threads happens later in
    // EngineHost::shutdown_() Phase 3.
    if (has_api()) api().stop_handler_threads();

    // Close Tx/Rx queues (data-plane teardown happens inside RoleAPIBase).
    if (has_api())
        api().close_queues();
}


} // namespace pylabhub::processor
