/**
 * @file producer_role_host.cpp
 * @brief ProducerRoleHost — unified engine-agnostic producer implementation.
 *
 * This is the canonical data loop for the producer role.  It follows
 * docs/tech_draft/loop_design_unified.md §3 exactly.
 *
 * Layer 3 (infrastructure): Producer, queue, BrokerRequestComm, events.
 * Layer 2 (data loop): inner retry acquire, deadline wait, drain, invoke, commit.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_produce / invoke_on_inbox.
 *
 * Shared lifecycle scaffolding (ctor, startup_(), shutdown_(), ready-promise,
 * RoleAPIBase ownership, ThreadManager-owned worker) lives in RoleHostBase.
 * This file implements only the producer-specific worker_main_() hook and
 * its setup_infrastructure_() / teardown_infrastructure_() helpers.
 */
#include "producer_role_host.hpp"
#include "utils/script_engine_factory.hpp"  // scripting::create_engine — worker_main_ Step 0
#include "utils/thread_manager.hpp"
#include "producer_fields.hpp"
#include "service/cycle_ops.hpp"
#include "service/data_loop.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/role_config_translation.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/role_handler.hpp"     // Wave-B M5: handler-mode startup
#include "utils/security/key_store.hpp"  // HEP-CORE-0040 §173: identity pubkey
#include "utils/role_presence.hpp"    // Wave-B M5: Presence + RoleKind
#include "utils/schema_utils.hpp"
#include "utils/lifecycle.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::producer
{

using scripting::ProducerCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ProducerRoleHost::ProducerRoleHost(config::RoleConfig config,
                                     std::atomic<bool> *shutdown_flag)
    : scripting::RoleHostFrame(std::move(config),
                                shutdown_flag,
                                { /*role_tag=*/         "prod",
                                  /*role_label=*/       "producer",
                                  /*required_callback=*/"on_produce" })
{
    // Engine is constructed in worker_main_ Step 0 (HEP-CORE-0011
    // §"Engine Construction Lifecycle").  Not constructed here.
}

ProducerRoleHost::~ProducerRoleHost()
{
    // Join worker BEFORE producer-specific members (broker_comm_, inbox_queue_)
    // begin destruction — the worker's teardown path reads them. shutdown_()
    // is idempotent, so the base destructor calling it again is a no-op.
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

void ProducerRoleHost::worker_main_()
{
    // GIL pickup (HEP-CORE-0011 §"Engine Construction Lifecycle",
    // Option E final design).  PythonInterpreter is owned by main();
    // pi_startup released the GIL via a stored py::gil_scoped_release
    // so workers can acquire it.  The lease holds the GIL on THIS
    // thread for the entire worker_main_ lifetime; it's a no-op for
    // Lua/Native deployments where the interpreter is not loaded.
    pylabhub::scripting::PythonGilLease gil_lease;

    // Step 0: Construct the script engine ON THIS WORKER THREAD.  The
    // worker holds the GIL via the lease above iff Python is in play,
    // so PythonEngine's `py::object{py::none()}` member-default-
    // initializers run under GIL safely.  Lua / Native engines have
    // no module dependency; they construct self-contained.
    auto       &core_       = core();
    const auto &config_     = config();
    auto       &promise_ref = ready_promise();

    set_engine_(scripting::create_engine(config_.script()));
    if (!has_engine())
    {
        LOGGER_ERROR("[prod] scripting::create_engine returned null for "
                     "script.type='{}'", config_.script().type);
        promise_ref.set_value(false);
        return;
    }
    auto &engine_ref = engine();
    auto &api_ref    = api();

    const auto &id   = config_.identity();
    const auto &sc   = config_.script();
    const auto &hub  = config_.out_hub();
    const auto &tr   = config_.out_transport();
    const auto &pf   = config_.role_data<ProducerFields>();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[prod] 'script.type' not set in config — defaulting to '{}'. "
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
        LOGGER_ERROR("[prod] Schema parse error: {}", e.what());
        promise_ref.set_value(false);
        return;
    }
    // Producer's build_presences_ returns exactly one Producer-kind
    // presence on out_hub/out_channel.
    const auto &tx_presence = presences_[0];

    // Local refs into the canonical presence — used by wire-emission
    // readers + params + slot-size storage on core below.
    out_slot_spec_ = tx_presence.slot_spec;
    const hub::SchemaSpec &out_fz_local = tx_presence.fz_spec;

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
                config_.inbox().schema_json, false, "prod", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[prod] Inbox schema parse error: {}", e.what());
            promise_ref.set_value(false);
            return;
        }
    }

    // Packing is schema-driven (was transport-level `zmq_packing` pre-2026-04-20).
    // Slot and flexzone each carry their own packing; script/wire/SHM all honor
    // the schema's packing for that side.  "aligned" fallback for flexzone-only
    // roles where out_slot_spec_ is unset.
    const std::string packing =
        out_slot_spec_.has_schema ? out_slot_spec_.packing : "aligned";

    // Compute and store slot logical size on core (infrastructure-authoritative).
    // Flexzone sizes live on RoleAPIBase::FlexzoneInfoCache, populated by
    // RoleHostFrame::setup_infrastructure_ step 6.5 from the presence's fz_spec.
    if (out_slot_spec_.has_schema)
        core_.set_out_slot_spec(hub::SchemaSpec{out_slot_spec_},
                                hub::compute_schema_size(out_slot_spec_, packing));

    // ── Step 2a: Wire api_ identity + config (no infrastructure deps) ────────
    //
    // api_ was constructed in RoleHostBase::startup_() before the worker
    // was spawned so its ThreadManager could spawn this thread under
    // bounded join.  Here we populate mutable post-ctor wiring state.
    //
    // ORDERING (audit 2026-05-20, demo-harness discovery): the api_ state
    // wiring MUST happen BEFORE `setup_infrastructure_` because
    // `setup_infrastructure_` calls `api_ref.build_tx_queue(opts)`, which
    // reads `pImpl->channel` to derive the SHM block name via
    // `tx_channel = pImpl->out_channel.empty() ? pImpl->channel : pImpl->out_channel`
    // (role_api_base.cpp).  Pre-fix this ran the other way around — empty
    // tx_channel → `shm_create("", ...)` → EINVAL → worker thread throws.
    // See HEP-CORE-0011 §"worker_main_ phase ordering" for the canonical
    // sequence.  `set_inbox_queue` stays AFTER setup_infrastructure_
    // because the inbox queue object is created there.
    api_ref.set_name(id.name);
    wire_api_for_presences_(presences_);  // sets channel from presences_
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
        // Wave-B M5: BrokerRequestComm allocation moved into RoleHandler,
        // built lazily in Step 6 below.  The role host no longer owns a
        // BRC unique_ptr; api.start_handler_threads takes ownership of
        // the RoleHandler which owns one BRC per HubConnection.
        api_ref.set_inbox_queue(inbox_queue_.get());
    }

    // ── Step 4: Load engine via lifecycle module ─────────────────────────────
    // engine_lifecycle_startup does: initialize → load_script → register_slot_type
    // (all directions + inbox) → build_api. Single call replaces manual steps.

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = &engine_ref;
    params.api                = &api_ref;
    params.tag                = "prod";
    params.script_dir         = script_dir;
    // Audit B12 (2026-05-21, demo-harness discovery): native engines
    // need a .so filename, not "__init__.py".  Pre-fix the ternary
    // covered only lua/python; type=="native" fell through to
    // "__init__.py" and the native engine's load_script tried
    // <dir>/__init__.py + <dir> (as a .so), both failed.
    params.entry_point        = (sc.type == "lua")    ? "init.lua"
                              : (sc.type == "native") ? "plugin.so"
                                                      : "__init__.py";
    params.required_callback  = "on_produce";
    params.out_slot_spec      = out_slot_spec_;
    params.out_fz_spec        = out_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.out_packing        = packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[prod] Engine startup failed: {}", e.what());
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
    if (api_ref.has_tx_side() && api_ref.has_tx_fz())
        api_ref.sync_tx_flexzone_checksum();

    // ── Step 6: Connect to broker, start handler ctrl thread(s), register
    // (Wave-B M5 — handler-mode replaces legacy start_ctrl_thread).
    core_.set_running(true);
    {
        // 6a — Build the role's presence list: 1 producer presence on
        // the configured out_hub.  Per HEP-CORE-0033 §19, even single-
        // presence roles go through RoleHandler so the routing helpers
        // (resolve_bc_for_channel/role/band) work uniformly with the
        // dual-hub processor shape that lands in M8.
        std::vector<scripting::Presence> presences;
        {
            scripting::Presence p;
            p.hub       = config_.out_hub();
            p.channel   = config_.out_channel();
            p.role_kind = scripting::RoleKind::Producer;
            presences.push_back(std::move(p));
        }

        // HEP-CORE-0040 §173: CURVE keypair lives in `key_store()`
        // (LockedKey-backed).  Handler / build_tx_queue / BRC connect
        // path read it on-site via `key_store().with_seckey` /
        // `pubkey` — no plumbing through RoleAPIBase.

        auto handler = std::make_unique<scripting::RoleHandler>(
            std::move(presences));

        if (!api_ref.start_handler_threads(std::move(handler)))
        {
            LOGGER_ERROR("[prod] start_handler_threads failed");
            promise_ref.set_value(false);
            return;
        }

        // 6c — Build REG_REQ payload (HEP-CORE-0034 Phase 5b).  Channel /
        // identity / transport fields, then schema (§10.1), then inbox
        // (§10.2).  Layered builder pattern identical to the pre-M5 code
        // — only the dispatch target (api.register_producer_channel
        // instead of bundling inside ctrl_cfg) changed.
        const auto &ch  = config_.out_channel();
        const auto &shm = config_.out_shm();
        hub::ProducerRegInputs reg_in;
        reg_in.channel           = ch;
        reg_in.role_uid          = id.uid;
        reg_in.role_name         = id.name;
        reg_in.role_tag          = "producer";
        reg_in.has_shm           = shm.enabled;
        reg_in.is_zmq_transport  = (tr.transport == config::Transport::Zmq);
        reg_in.zmq_node_endpoint = tr.zmq_endpoint;
        // HEP-CORE-0036 §4.1 + §5.1 + §6.4 — producer's CURVE identity
        // pubkey is REQUIRED on REG_REQ.  Broker stores it on
        // ChannelEntry::producers[i].zmq_pubkey and emits it back to
        // consumers via CONSUMER_REG_ACK.producers[] so the consumer can
        // use it as the data-plane curve_serverkey.
        reg_in.zmq_pubkey        = std::string(
            pylabhub::utils::security::key_store().pubkey(pylabhub::utils::security::kRoleIdentityName));
        auto reg_opts = hub::build_producer_reg_payload(reg_in);

        // Schema fields (HEP-CORE-0034 §10.1).  Empty fields → broker
        // takes the legacy/anonymous path.
        const auto wire_schema = hub::make_wire_schema_fields(
            pf.out_slot_schema_json, out_slot_spec_, out_fz_local);
        hub::apply_producer_schema_fields(reg_opts, wire_schema);

        // Inbox metadata (HEP-CORE-0034 §10.2; no-op if no inbox).
        api_ref.append_inbox_to_reg(reg_opts, inbox_cfg_);

        // 6d — REG_REQ + heartbeat install (the post-spawn block legacy
        // start_ctrl_thread ran internally; explicit at this layer in
        // handler-mode).  register_producer_channel transitions the
        // matching Presence's `registration_state` through
        // RegRequestPending → Registered on success (audit S1+O4,
        // 2026-05-17 — replaces the pre-S1 `shared.producer_channel`
        // string).
        auto reg_result = api_ref.register_producer_channel(reg_opts);
        if (!reg_result.has_value() ||
            reg_result->value("status", std::string{}) != "success")
        {
            // Per HEP-CORE-0036 §3.5.1 registration failure is FATAL — there
            // is no "operate locally" mode under the AUTH-gate principle (an
            // unregistered producer has no authorized consumers and would be
            // a port-holder with no peers).
            LOGGER_ERROR("[prod] Broker registration failed — "
                         "aborting role startup");
            promise_ref.set_value(false);
            return;
        }

        // HEP-CORE-0036 §3.5.5 S3 — drive the tx queue from Standby
        // to Active using the REG_ACK payload.  apply_producer_reg_ack
        // dispatches through QueueWriter::apply_master_approval, which
        // extracts ACK.initial_allowlist (per §6.2), seeds the ZAP cache,
        // binds the PUSH socket, and spawns the worker.  Must run before
        // install_heartbeat so the first heartbeat fires after the data
        // plane is up (§3.5.4 INV1).
        if (!api_ref.apply_producer_reg_ack(*reg_result))
        {
            LOGGER_ERROR("[prod] apply_producer_reg_ack failed — "
                         "tx queue did not activate; aborting startup");
            promise_ref.set_value(false);
            return;
        }
        auto hub_max = scripting::RoleAPIBase::extract_hub_heartbeat_max(*reg_result);
        api_ref.install_heartbeat(config_.timing().heartbeat_interval_ms,
                                   hub_max);
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(api_ref, config_.startup().wait_for_roles, "[prod]"))
        {
            LOGGER_ERROR("[prod] Startup coordination failed — required roles not available");
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    promise_ref.set_value(true);

    // Step 8: Run the data loop via shared frame + ProducerCycleOps.
    if (!api_ref.has_tx_side())
    {
        LOGGER_ERROR("[prod] run_data_loop: producer not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        ProducerCycleOps ops(api_ref, engine_ref, core_,
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
// make_tx_opts — delegates to the shared free function (M9 §11.6)
// ============================================================================
//
// Originally extracted from setup_infrastructure_ in commit eb3eed36
// as a per-role static method.  Now a thin shim around the shared
// `pylabhub::scripting::make_tx_opts` free function in
// src/utils/service/role_config_translation.cpp.  Kept as a class
// member so existing L2 test callers continue to work unchanged; the
// implementation lives in exactly one place.  Producer's and
// Processor's pre-consolidation per-role static methods were
// byte-identical — collapsing them into the shared free function is
// pure de-duplication.

hub::TxQueueOptions
ProducerRoleHost::make_tx_opts(const config::RoleConfig &config,
                                const hub::SchemaSpec    &out_slot_spec,
                                const hub::SchemaSpec    &out_fz_spec,
                                bool                      has_tx_fz)
{
    return scripting::make_tx_opts(config, out_slot_spec, out_fz_spec,
                                    has_tx_fz);
}

// ============================================================================
// setup_infrastructure_ + teardown_infrastructure_ — inherited from
// RoleHostFrame.  See src/utils/service/role_host_frame.cpp.
// ============================================================================

// ============================================================================
// build_presences_ — Producer's per-role override
// ============================================================================
//
// Returns a single Producer-kind presence on out_hub/out_channel.
// Schemas (slot + fz) are resolved inline via hub::resolve_schema().
// The presence's `hub.hub_dir / "schemas"` is the schema search path.

std::vector<scripting::Presence>
ProducerRoleHost::build_presences_(const config::RoleConfig &c) const
{
    scripting::Presence p;
    p.hub       = c.out_hub();
    p.channel   = c.out_channel();
    p.role_kind = scripting::RoleKind::Producer;

    std::vector<std::string> schema_dirs;
    if (!p.hub.hub_dir.empty())
        schema_dirs.push_back(
            (std::filesystem::path(p.hub.hub_dir) / "schemas").string());

    const auto &fields = c.role_data<ProducerFields>();
    p.slot_spec = hub::resolve_schema(
        fields.out_slot_schema_json, false, "prod", schema_dirs);
    p.fz_spec = hub::resolve_schema(
        fields.out_flexzone_schema_json, true, "prod", schema_dirs);

    std::vector<scripting::Presence> v;
    v.push_back(std::move(p));
    return v;
}

} // namespace pylabhub::producer
