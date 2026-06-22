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
#include "processor_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/role_config_translation.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/role_handler.hpp"     // Wave-B M7: handler-mode startup
#include "utils/security/key_store.hpp"  // HEP-CORE-0040 §173: identity pubkey
#include "utils/security/shm_capability_channel.hpp"  // HEP-CORE-0041 §5.1: default endpoint helper
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
    : scripting::RoleHostFrame(std::move(config),
                                shutdown_flag,
                                { /*role_tag=*/         "proc",
                                  /*role_label=*/       "processor",
                                  /*required_callback=*/"on_process" })
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

    // ── Step 1a: Build presences (single resolve of channel schemas) ─────────
    // `build_presences_()` is the single resolver of channel schemas
    // (slot + fz, both directions).  presences_[i] is the canonical
    // per-channel home; every downstream consumer (wire-emission
    // readers, params.*, FlexzoneInfoCache populate) reads from it.
    //
    // SCHEMA-DIR SCOPING: build_presences_ scopes the search path
    // per-presence (each presence resolves only against its own
    // hub's schemas dir).  See the override comment on
    // `build_presences_` for details.
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
        LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
        promise_ref.set_value(false);
        return;
    }
    // Processor's build_presences_ returns two presences: one
    // Consumer-kind (in side) + one Producer-kind (out side).  Look
    // them up by role_kind so this code is order-independent.
    const scripting::Presence *in_presence  = nullptr;
    const scripting::Presence *out_presence = nullptr;
    for (const auto &p : presences_)
    {
        if (p.role_kind == scripting::RoleKind::Consumer) in_presence  = &p;
        else if (p.role_kind == scripting::RoleKind::Producer) out_presence = &p;
    }
    if (!in_presence || !out_presence)
    {
        LOGGER_ERROR("[proc] build_presences_ must return one Consumer + one Producer presence");
        promise_ref.set_value(false);
        return;
    }
    // Local refs into the canonical presences — used by wire-emission
    // readers + params + slot-size storage on core below.
    in_slot_spec_  = in_presence->slot_spec;
    out_slot_spec_ = out_presence->slot_spec;
    const hub::SchemaSpec &in_fz_local  = in_presence->fz_spec;
    const hub::SchemaSpec &out_fz_local = out_presence->fz_spec;

    // ── Step 1b: Inbox schema (role-level, not per-channel) ──────────────────
    // Inbox is not on a Presence (presences_ carries per-channel state).
    // Resolved separately if configured.  Uses the COMBINED schema-dir
    // search path (both hubs' schemas/ dirs) because the inbox is a
    // role-level concern not tied to either side.
    hub::SchemaSpec inbox_spec_local;
    if (config_.inbox().has_inbox())
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
        try
        {
            inbox_spec_local = hub::resolve_schema(
                config_.inbox().schema_json, false, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Inbox schema parse error: {}", e.what());
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

    // Compute and store slot logical sizes on core (infrastructure-authoritative).
    // Flexzone sizes live on RoleAPIBase::FlexzoneInfoCache, populated by
    // RoleHostFrame::setup_infrastructure_ step 6.5 from the presence's fz_spec.
    if (in_slot_spec_.has_schema)
        core_.set_in_slot_spec(hub::SchemaSpec{in_slot_spec_},
                               hub::compute_schema_size(in_slot_spec_, in_packing));
    if (out_slot_spec_.has_schema)
        core_.set_out_slot_spec(hub::SchemaSpec{out_slot_spec_},
                                hub::compute_schema_size(out_slot_spec_, out_packing));

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
    wire_api_for_presences_(presences_);  // sets channel + out_channel from presences_
    api_ref.set_log_level(config_.identity().log_level);
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
    // Audit B12 (2026-05-21): see producer_role_host.cpp comment.
    params.entry_point        = (sc.type == "lua")    ? "init.lua"
                              : (sc.type == "native") ? "plugin.so"
                                                      : "__init__.py";
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
    if (api_ref.has_tx_side() && api_ref.has_tx_fz())
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

        // HEP-CORE-0040 §173: CURVE keypair lives in `key_store()`.
        // RoleHandler dedups connections; single-hub processor → 1 BRC,
        // dual-hub → 2.
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
            // HEP-CORE-0036 §4.1 — producer-side identity pubkey is
            // REQUIRED on REG_REQ; processor uses its own role's
            // CURVE client pubkey (same vault-loaded value as the BRC).
            reg_in.zmq_pubkey        = std::string(
                pylabhub::utils::security::key_store().pubkey(pylabhub::utils::security::kRoleIdentityName));
            // HEP-CORE-0041 §5.1 (substep 1g #254) — same as producer
            // role host: SHM out-channels publish a capability-transport
            // endpoint so the broker can echo it to authorized consumers.
            if (reg_in.has_shm && !reg_in.is_zmq_transport)
            {
                reg_in.shm_capability_endpoint =
                    pylabhub::utils::security::default_shm_capability_endpoint(
                        reg_in.channel);
            }
            prod_reg = hub::build_producer_reg_payload(reg_in);
        }
        const auto &pf_for_wire = config_.role_data<ProcessorFields>();
        const auto out_wire = hub::make_wire_schema_fields(
            pf_for_wire.out_slot_schema_json, out_slot_spec_, out_fz_local);
        hub::apply_producer_schema_fields(prod_reg, out_wire);
        api_ref.append_inbox_to_reg(prod_reg, inbox_cfg_);

        // Input consumer CONSUMER_REG_REQ — routes via brc_for_channel(in_channel)
        // which maps to the in_hub's BRC (or out_hub's BRC if dedup'd).
        // `zmq_pubkey` carries the role's own CURVE pubkey
        // (HEP-CORE-0036 §6.5) so the broker can populate the
        // channel-scope auth allowlist.
        auto cons_reg = hub::build_consumer_reg_payload(
            hub::ConsumerRegInputs{config_.in_channel(), id.uid, id.name,
                                    std::string(pylabhub::utils::security::
                                                key_store().pubkey(pylabhub::utils::security::kRoleIdentityName))});
        const auto in_wire = hub::make_wire_schema_fields(
            pf_for_wire.in_slot_schema_json, in_slot_spec_, in_fz_local);
        hub::apply_consumer_schema_fields(cons_reg, in_wire);
        api_ref.append_inbox_to_reg(cons_reg, inbox_cfg_);

        // 6d — Send both REGs (auto-record via M5a) + install heartbeat.
        // We capture the hub-max from whichever REG returns it last,
        // mirroring legacy behaviour (the legacy code path captured
        // both REGs' heartbeat blocks and let the second overwrite the
        // first; same precedence here).
        std::optional<int> hub_max;

        // Per HEP-CORE-0036 §3.5.1 registration failure is FATAL on BOTH
        // the in-side (consumer registration) and out-side (producer
        // registration).  Either DEREG side cleans up via
        // do_role_teardown presence-walk.
        auto prod_result = api_ref.register_producer_channel(prod_reg);
        if (!prod_result.has_value() ||
            prod_result->value("status", std::string{}) != "success")
        {
            LOGGER_ERROR("[proc] Output producer registration failed — "
                         "aborting role startup");
            promise_ref.set_value(false);
            return;
        }

        // HEP-CORE-0036 §6.7 — drive Tx queue Standby → Active from
        // REG_ACK (carries initial_allowlist).  Symmetric with the
        // in-side consumer activation below.
        if (!api_ref.apply_producer_reg_ack(*prod_result))
        {
            LOGGER_ERROR("[proc] apply_producer_reg_ack failed — "
                         "Tx queue did not reach Active state");
            promise_ref.set_value(false);
            return;
        }
        {
            auto m = scripting::RoleAPIBase::extract_hub_heartbeat_max(*prod_result);
            if (m.has_value()) hub_max = m;
        }

        auto cons_result = api_ref.register_consumer(cons_reg);
        if (!cons_result.has_value() ||
            cons_result->value("status", std::string{}) != "success")
        {
            LOGGER_ERROR("[proc] Input consumer registration failed — "
                         "aborting role startup");
            promise_ref.set_value(false);
            return;
        }

        // HEP-CORE-0036 §6.7 — drive Rx queue Standby → Active from
        // CONSUMER_REG_ACK (carries producers[]).  Same uniform pattern
        // as consumer_role_host.
        if (!api_ref.apply_consumer_reg_ack(*cons_result))
        {
            LOGGER_ERROR("[proc] apply_consumer_reg_ack failed — "
                         "Rx queue did not reach Active state");
            promise_ref.set_value(false);
            return;
        }
        {
            auto m = scripting::RoleAPIBase::extract_hub_heartbeat_max(*cons_result);
            if (m.has_value()) hub_max = m;  // consumer's wins (legacy parity)
        }

        api_ref.install_heartbeat(config_.timing().heartbeat_interval_ms,
                                   hub_max);

        // HEP-CORE-0041 1i-mig-3 — for SHM OUT channels, wire the L2b
        // acceptor + L2c orchestrator + accept thread on top of the L1
        // transport that prepare_tx_capability_ created.  Symmetric
        // with ProducerRoleHost (1i-mig-2b-2); helper lives on
        // RoleHostFrame (1i-mig-2c M3).  No-op when shm_transport_ is
        // null (ZMQ OUT channels).
        if (shm_transport_ && !spawn_shm_auth_listener_())
        {
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 6e: Startup coordination — wait for prerequisite roles (HEP-0023).
    // Sub-step of Step 6 per HEP-CORE-0011 § "Role Host worker_main_() Steps"
    // (renumbered 2026-06-13: was 6b in the pre-AUTH-1 step list).
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
// make_rx_opts / make_tx_opts — delegate to shared free functions (M9 §11.6)
// ============================================================================
//
// Q1 RESOLUTION (2026-05-22): the previous per-role static
// `ProcessorRoleHost::make_rx_opts` set `opts.zmq_buffer_depth =
// tr.zmq_buffer_depth` unconditionally (before the `if (zmq)` block);
// Consumer's per-role version set it only inside the `if (zmq)`
// block.  The shared `scripting::make_rx_opts` adopts Consumer's
// convention (set only inside `if (zmq)`).  Functionally inert today
// because `zmq_buffer_depth` is ignored on the SHM path; the change
// eliminates a pre-existing inconsistency.

hub::RxQueueOptions
ProcessorRoleHost::make_rx_opts(const config::RoleConfig &config,
                                 const hub::SchemaSpec    &in_slot_spec,
                                 const hub::SchemaSpec    &in_fz_spec,
                                 bool                      has_rx_fz)
{
    return scripting::make_rx_opts(config, in_slot_spec, in_fz_spec,
                                    has_rx_fz);
}

hub::TxQueueOptions
ProcessorRoleHost::make_tx_opts(const config::RoleConfig &config,
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
// build_presences_ — Processor's per-role override
// ============================================================================
//
// Returns TWO presences (in dedup-friendly order: Consumer first, then
// Producer).  Both presences resolve their schemas inline using their
// own hub's schema directory.
//
// SCHEMA-DIR SCOPING NOTE (review finding 2026-05-23): the legacy
// worker_main_ step 1 (still active during Phase 1 shadow) computes a
// COMBINED schema_dirs list — both `in_hub/schemas` AND `out_hub/schemas`
// — and uses it to resolve ALL four schemas (in_slot, out_slot, in_fz,
// out_fz).  This implicitly allows cross-hub schema references (e.g. an
// in_slot schema referencing a type defined only in out_hub's schemas).
//
// This `build_presences_` deliberately scopes each presence to its OWN
// hub's schema directory only.  Rationale:
//   - Each channel's schema authoritatively lives on its own hub.
//   - Cross-hub schema sharing is a quirky implicit feature, not a
//     documented design.
//   - Today's dual-hub demos pass — no cross-hub sharing exists in
//     practice.
//
// If a future dual-hub config DOES need cross-hub schema reference, the
// fix is per-Presence: extend the search path explicitly, not by going
// back to a global merged list.  See HEP-CORE-0034 §6 for schema-search
// scoping rules.

std::vector<scripting::Presence>
ProcessorRoleHost::build_presences_(const config::RoleConfig &c) const
{
    const auto &fields = c.role_data<ProcessorFields>();

    auto build_one = [](const config::HubRefConfig &hub_cfg,
                        const std::string &channel,
                        scripting::RoleKind kind,
                        const nlohmann::json &slot_json,
                        const nlohmann::json &fz_json) {
        scripting::Presence p;
        p.hub       = hub_cfg;
        p.channel   = channel;
        p.role_kind = kind;
        std::vector<std::string> schema_dirs;
        if (!p.hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(p.hub.hub_dir) / "schemas").string());
        p.slot_spec = hub::resolve_schema(slot_json, false, "proc", schema_dirs);
        p.fz_spec   = hub::resolve_schema(fz_json,   true,  "proc", schema_dirs);
        return p;
    };

    std::vector<scripting::Presence> v;
    v.push_back(build_one(c.in_hub(), c.in_channel(),
                          scripting::RoleKind::Consumer,
                          fields.in_slot_schema_json,
                          fields.in_flexzone_schema_json));
    v.push_back(build_one(c.out_hub(), c.out_channel(),
                          scripting::RoleKind::Producer,
                          fields.out_slot_schema_json,
                          fields.out_flexzone_schema_json));
    return v;
}

// prepare_tx_capability_ + cleanup_tx_capability_ — both inherited from
// RoleHostFrame default impls.  prepare_ was identical to the producer's;
// promoted to the frame in 1i-mig-M3.5 (#266) so both subclasses share
// the canonical body.  Log prefix derives from frame_cfg_.role_tag.


} // namespace pylabhub::processor
