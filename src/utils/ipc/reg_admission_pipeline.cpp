#include "utils/reg_admission_pipeline.hpp"

#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"

namespace pylabhub::admission
{

namespace
{

// Extract the gate-facing view from a ProducerRegReqBody without allocating.
// The view holds string_views into the body's cached strings; both
// live as long as the body does.
struct ViewHolders
{
    std::string role_uid;
    std::string channel_name;
    std::string zmq_pubkey;
    std::string client_nonce;
};

RegFamilyBodyView make_view(const ::pylabhub::wire::ProducerRegReqBody &body, ViewHolders &h)
{
    // ProducerRegReqBody accessors return by value; hold them so the string_view
    // fields remain valid for the duration of gate execution.
    h.role_uid = body.role_uid();
    h.channel_name = body.channel_name();
    h.zmq_pubkey = body.zmq_pubkey();
    h.client_nonce = body.client_nonce();

    RegFamilyBodyView v;
    v.role_uid = h.role_uid;
    v.channel_name = h.channel_name;
    v.zmq_pubkey = h.zmq_pubkey;
    v.client_nonce = h.client_nonce;
    v.client_wall_ts = body.client_wall_ts();
    return v;
}

RegRequest make_request(const ::pylabhub::wire::ProducerRegReqBody &body)
{
    RegRequest r;
    r.channel_name = body.channel_name();
    r.role_uid = body.role_uid();
    r.role_name = body.role_name();
    r.role_type = body.role_type();
    r.zmq_pubkey = body.zmq_pubkey();
    r.channel_topology = body.channel_topology();
    r.data_transport = body.data_transport();
    r.schema_hash = body.schema_hash();
    // schema_version retired per C2 — version rides inside schema_id.
    r.schema_id = body.schema_id();
    r.schema_blds = body.schema_blds();
    r.schema_owner = body.schema_owner();
    return r;
}

RegRejected reject_from_gate(RejectDetail detail)
{
    RegRejected r;
    r.detail = std::move(detail);
    return r;
}

} // namespace

RegOutcome run_reg_admission(const ::pylabhub::wire::WireEnvelope &env,
                             const ::pylabhub::wire::ProducerRegReqBody &body,
                             const AdmissionContext &gate_ctx, const RegCommitFn &commit)
{
    // ── Step 1-2: pre-mutation gates ──────────────────────────────
    //
    // Body-view lifetime holders live in this stack frame; view's
    // string_views stay valid until the function returns.
    ViewHolders holders;
    auto view = make_view(body, holders);

    if (auto rejection = run_reg_family_gates(env, view, gate_ctx))
    {
        return reject_from_gate(std::move(*rejection));
    }

    // ── Step 3: typed protocol-level intermediate ──────────────────
    //
    // Body → RegRequest: no JSON field-picking; every value came from a
    // typed body-class accessor.  RegRequest is the interface the
    // state-mutation callback consumes.
    auto request = make_request(body);

    // ── Step 4: delegate to state-mutation primitive ──────────────
    //
    // Callback owns the topology / cardinality / schema / transport
    // gates and the writer-lock-scoped admission.  Pipeline forwards
    // the typed request and returns whatever variant it produces.
    if (!commit)
    {
        RejectDetail d;
        d.code = RejectCode::broker_internal_error;
        d.field = "";
        d.message = "internal: commit callback not bound";
        return reject_from_gate(std::move(d));
    }
    return commit(request);
}

} // namespace pylabhub::admission
