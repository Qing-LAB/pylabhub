#include "utils/broker_reg_handler.hpp"

#include "utils/hub_state.hpp"
#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace pylabhub::admission
{

namespace
{

std::uint64_t system_now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// Wire-string → typed topology, distinguishing three cases:
//   omitted  = empty string → HubState default ("inherit stored, or
//              OneToOne on fresh channel" per _on_producer_added contract)
//   valid    = recognized enum value → typed ChannelTopology
//   unknown  = non-empty, unrecognized string → wire violation
// Silent fallback of unknown-to-empty is a memory-flagged anti-pattern
// under the CURVE-integrity frame — reject at the wire instead.
enum class TopologyParse
{
    omitted,
    valid,
    unknown,
};
struct TopologyResult
{
    TopologyParse kind;
    std::optional<::pylabhub::hub::ChannelTopology> topology; // set when valid
};
TopologyResult parse_channel_topology(std::string_view s)
{
    using T = ::pylabhub::hub::ChannelTopology;
    if (s.empty())
        return {TopologyParse::omitted, std::nullopt};
    if (s == "fan-in")
        return {TopologyParse::valid, T::FanIn};
    if (s == "fan-out")
        return {TopologyParse::valid, T::FanOut};
    if (s == "one-to-one")
        return {TopologyParse::valid, T::OneToOne};
    return {TopologyParse::unknown, std::nullopt};
}

// Derive AdmissionSide from (role_type, topology) per HEP-CORE-0017 §3.3.0.
// The topology matrix:
//   fan-in     → producer=dialing, consumer=binding
//   fan-out    → producer=binding, consumer=dialing
//   one-to-one → producer=binding, consumer=dialing
// Nullopt topology means "HubState will infer/default" — call site knows
// whether the effective topology is fan-in and passes accordingly.
AdmissionSide side_for(std::string_view role_type,
                       std::optional<::pylabhub::hub::ChannelTopology> topology)
{
    using T = ::pylabhub::hub::ChannelTopology;
    const bool is_producer = (role_type == "producer" || role_type == "processor");
    const T effective = topology.value_or(T::OneToOne);
    switch (effective)
    {
    case T::FanIn:
        return is_producer ? AdmissionSide::dialing : AdmissionSide::binding;
    case T::FanOut:
    case T::OneToOne:
        return is_producer ? AdmissionSide::binding : AdmissionSide::dialing;
    }
    return AdmissionSide::binding; // unreachable under complete switch
}

// Build ProducerEntry from the typed RegRequest.  Populates the fields
// present in the §14.3 body catalog + role_uid identity captured from
// the envelope.  Fields marked for retirement (inbox_*, zmq_node_endpoint,
// zmq_identity per addendum D1/G2) stay defaulted; when broker_service.cpp
// migrates, per-msg_type body extensions add producer_pid / hostname if
// still needed.
::pylabhub::hub::ProducerEntry make_producer_entry_from(const RegRequest &r)
{
    ::pylabhub::hub::ProducerEntry entry;
    entry.role_name = r.role_name;
    entry.role_uid = r.role_uid;
    entry.zmq_pubkey = r.zmq_pubkey;
    // Legacy fields remain empty; addendum §11 lists them for retirement
    // once the atomic wire cut lands (broker_proto ≥ N).  Retention here
    // matches the existing HubState surface without introducing wire
    // fields that §14.3 has not declared.
    return entry;
}

::pylabhub::hub::ChannelSchemaInvariants make_schema_from(const RegRequest &r)
{
    ::pylabhub::hub::ChannelSchemaInvariants s;
    s.schema_hash = r.schema_hash;
    s.schema_id = r.schema_id;
    s.schema_blds = r.schema_blds;
    s.schema_owner = r.schema_owner;
    // schema_version retired per C2 — version rides inside schema_id.
    return s;
}

::pylabhub::hub::ChannelTransportInvariants make_transport_from(const RegRequest &r)
{
    ::pylabhub::hub::ChannelTransportInvariants t;
    t.data_transport = r.data_transport;
    return t;
}

// Translate the typed ProducerAdmissionResult into the pipeline's typed
// RegOutcome variant.  Pure function of its inputs — takes the already-
// resolved side + instance_id + channel_version from the commit closure
// so the translator has no HubState dependency and no hidden reads.
// Precedence per HubState doc: check invalid_identifier →
// topology_error_code → invariant_result → producer_result.  Any
// rejection produces RegRejected; Created / IdempotentEqual maps to
// RegAccepted.
RegOutcome translate_producer_admission(const RegRequest &r,
                                        const ::pylabhub::hub::ProducerAdmissionResult &result,
                                        AdmissionSide side, std::uint64_t assigned_instance_id,
                                        std::uint64_t channel_version)
{
    using namespace ::pylabhub::hub;

    auto reject = [](RejectCode code, std::string field, std::string message) -> RegOutcome
    {
        RegRejected r_out;
        r_out.detail.code = code;
        r_out.detail.field = std::move(field);
        r_out.detail.message = std::move(message);
        return r_out;
    };

    if (result.invalid_identifier)
    {
        return reject(RejectCode::invalid_request, "role_uid",
                      "HubState grammar rejection at admission");
    }
    if (result.topology_error_code != nullptr)
    {
        // topology_error_code is a stable HubState-owned string
        // ("TOPOLOGY_MISMATCH", "FAN_IN_IS_SINGLE_CONSUMER", etc.).
        // Passed through verbatim as the reject.message; the pipeline's
        // typed RejectCode collapses these into invalid_request until
        // §14.5 defines dedicated codes.
        return reject(RejectCode::invalid_request, "channel_topology",
                      std::string{result.topology_error_code});
    }
    if (result.invariant_result == InvariantSetResult::RejectedMismatch)
    {
        return reject(RejectCode::invalid_request, result.mismatched_invariant,
                      "schema/transport invariant mismatch on existing channel");
    }
    if (result.producer_result == AddProducerResult::RejectedUidConflict)
    {
        return reject(RejectCode::uid_conflict, "role_uid",
                      "role_uid already registered on this channel");
    }

    RegAccepted accepted;
    accepted.request = r;
    accepted.side = side;
    accepted.channel_opened = result.channel_opened;
    accepted.assigned_instance_id = assigned_instance_id;
    accepted.channel_version = channel_version;
    accepted.pending_notifies = {};
    return accepted;
}

} // namespace

BrokerRegHandler::BrokerRegHandler(::pylabhub::hub::HubState &hub_state,
                                   KnownRolesConfig known_roles, BrokerAdmissionConfig config)
    : hub_state_(hub_state), known_roles_(std::move(known_roles)), config_(config)
{
    // Bind the gate callbacks against HubState + known_roles.  Bound
    // once here so per-request handling does not re-construct
    // std::function objects.  Every callback captures `this` by
    // reference; BrokerRegHandler is non-movable / non-copyable so the
    // captures remain valid for the handler's lifetime.

    gate_callbacks_.lookup_known_role = [this](std::string_view uid,
                                               std::string_view pubkey) -> KnownRoleLookup
    {
        if (!known_roles_.lookup_pubkey_for_uid)
            return KnownRoleLookup::uid_unknown;
        const std::string registered = known_roles_.lookup_pubkey_for_uid(uid);
        if (registered.empty())
            return KnownRoleLookup::uid_unknown;
        if (registered != pubkey)
            return KnownRoleLookup::pubkey_mismatch;
        return KnownRoleLookup::binding_matches;
    };

    // I-KEY-ROTATION-VIA-DEREG (HEP-0046): a role's CURVE pubkey is
    // immutable for the broker's lifetime — rotation is edit-config +
    // hard-reload + re-REG.  gate_known_role_binding already rejects any
    // on-the-fly re-REG with a mismatched pubkey (PUBKEY_MISMATCH), so
    // there is no separate key-rotation gate.

    gate_callbacks_.record_and_check_nonce = [this](std::string_view uid, std::string_view nonce)
    {
        // No timestamp crosses here — the ReplayGuard owns its trusted
        // monotonic clock (see ReplayGuard header).
        return hub_state_.nonce_seen(uid, nonce, config_.nonce_window_ms);
    };

    gate_callbacks_.wall_now_ms = &system_now_ms;
}

RegOutcome BrokerRegHandler::handle(const ::pylabhub::wire::WireEnvelope &env,
                                    const ::pylabhub::wire::ProducerRegReqBody &body)
{
    // ── Ambient context ──────────────────────────────────────────────
    AdmissionContext ctx;
    ctx.cb = &gate_callbacks_;
    ctx.skew_tolerance_ms = config_.skew_tolerance_ms;
    ctx.nonce_window_ms = config_.nonce_window_ms;
    // No `broker_proto` field on AdmissionContext (retired per C3);
    // ABI verification uses `abi_fingerprint` at the broker's REG
    // handler, not this pipeline.

    // ── Commit callback (state mutation + protocol gates) ────────────
    //
    // Dispatches on request.role_type:
    //   - producer / processor → binding-side (fan-out / 1-to-1) or
    //     fan-in dialing-side admission via HubState::_on_producer_added
    //   - consumer             → binding-side (fan-in opens channel)
    //     or dialing-side pending via HubState::_on_consumer_joined
    //
    // Producer path wired below.  Consumer path (role_type=="consumer")
    // follows the same shape via _on_consumer_joined — added
    // incrementally as CONSUMER_REG_REQ migrates from broker_service.cpp.
    RegCommitFn commit = [this](const RegRequest &r) -> RegOutcome
    {
        // Parse channel_topology at the boundary.  Unknown values are
        // wire violations and rejected here — silent-fallback pattern
        // (empty-treatment for unknown) prohibited under the CURVE
        // integrity frame.
        const auto tp = parse_channel_topology(r.channel_topology);
        if (tp.kind == TopologyParse::unknown)
        {
            RegRejected rej;
            rej.detail.code = RejectCode::invalid_request;
            rej.detail.field = "channel_topology";
            rej.detail.message = "channel_topology='" + r.channel_topology +
                                 "' is not a recognized value "
                                 "(expected 'fan-in' | 'fan-out' | "
                                 "'one-to-one' or omitted)";
            return rej;
        }

        // BrokerRegHandler currently owns the producer / processor
        // path.  Consumer REG (CONSUMER_REG_REQ) has its own handler
        // path per §14.3; if the caller routed a consumer body here,
        // that is a broker-side dispatch bug — surface it distinctly
        // rather than dressing it as a client wire violation.
        if (r.role_type != "producer" && r.role_type != "processor")
        {
            RegRejected rej;
            rej.detail.code = RejectCode::broker_internal_error;
            rej.detail.field = "role_type";
            rej.detail.message = "role_type='" + r.role_type +
                                 "' not accepted by BrokerRegHandler; "
                                 "dispatch bug — CONSUMER_REG_REQ has "
                                 "its own handler path per §14.3";
            return rej;
        }

        // ── Effective-topology pre-check (§3.3.0 binding-side rule) ──
        //
        // Under fan-in, the producer is DIALING side and cannot open
        // the channel — the consumer is the topology-legal opener.
        // Fan-in producer admission requires R6 pending (§7.2), which
        // is not yet wired in this iteration.  Rather than silently
        // let HubState open a channel with a fan-in producer as first
        // admitted (which mislabels state and violates §3.3.0), surface
        // this as broker_internal_error until R6 machinery lands.
        //
        // Determination:
        //   - If wire topology present, use it directly.
        //   - Else look up stored topology from an existing ChannelEntry.
        //     Missing channel + omitted topology → HubState defaults to
        //     OneToOne, so producer is binding-side → proceed.
        using ChTopo = ::pylabhub::hub::ChannelTopology;
        std::optional<ChTopo> effective_topology = tp.topology;
        if (!effective_topology.has_value())
        {
            if (auto stored = hub_state_.channel(r.channel_name))
            {
                effective_topology = stored->topology;
            }
        }
        if (effective_topology.has_value() && *effective_topology == ChTopo::FanIn)
        {
            RegRejected rej;
            rej.detail.code = RejectCode::broker_internal_error;
            rej.detail.field = "channel_topology";
            rej.detail.message = "fan-in producer admission requires R6 pending queue "
                                 "(§7.2); not yet wired in BrokerRegHandler.  Consumer "
                                 "must open the channel per §3.3.0 binding-side rule.";
            return rej;
        }

        // §14.5 gate 4 (REG_REQ-specific extension): role_name grammar.
        // Design mandates grammar on role_uid / role_name / channel_name
        // at gate 4; the shared `gate_grammar` covers universal fields
        // (role_uid + channel_name) since role_name only exists on
        // ProducerRegReqBody per §14.3.  The role_name check lives here where
        // the body class is known.  Uses the same is_id_char grammar
        // as the shared gate — [A-Za-z0-9._-], 1..128 chars.
        {
            const auto &n = r.role_name;
            bool ok = !n.empty() && n.size() <= 128;
            if (ok)
            {
                for (char c : n)
                {
                    const bool cls = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                    if (!cls)
                    {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok)
            {
                RegRejected rej;
                rej.detail.code = RejectCode::invalid_request;
                rej.detail.field = "role_name";
                rej.detail.message = "role_name fails identifier grammar "
                                     "(HEP-CORE-0033 §G2.2.0b / §14.5 gate 4)";
                return rej;
            }
        }

        auto schema = make_schema_from(r);
        auto transport = make_transport_from(r);
        auto producer = make_producer_entry_from(r);

        auto result =
            hub_state_._on_producer_added(r.channel_name, std::move(schema), std::move(transport),
                                          tp.topology, std::move(producer));

        // ── Post-admission effective topology + side derivation ──────
        //
        // Read the effective topology from the newly-admitted channel
        // so the side label reflects reality (not the wire-only view
        // that would default to OneToOne when the wire omitted topology
        // and the stored topology differs).
        //
        // `channel_version` is sourced from the ledger (HEP-CORE-0042
        // §5.5.2 unified 2026-07-13) via `channel_access`, not the
        // dead `ChannelEntry.channel_version` field that this call
        // used to read.  That field was retired 2026-07-13; the ledger
        // on `ChannelAccessEntry` is the single source of truth for
        // per-channel admission version.
        std::uint64_t channel_version = 0;
        std::optional<ChTopo> post_topology;
        if (auto ch = hub_state_.channel(r.channel_name))
        {
            post_topology = ch->topology;
        }
        if (auto acc = hub_state_.channel_access(r.channel_name))
        {
            channel_version = acc->ledger.current_version();
        }
        const AdmissionSide side = side_for(r.role_type, post_topology);
        const std::uint64_t instance_id = hub_state_.producer_instance(r.role_uid);
        return translate_producer_admission(r, result, side, instance_id, channel_version);
    };

    return run_reg_admission(env, body, ctx, commit);
}

} // namespace pylabhub::admission
