/**
 * @file wire_adapter.cpp
 * @brief Implementation of the wire adapter — see wire_adapter.hpp for the
 *        semantic contract.
 */

#include "utils/wire_adapter.hpp"

#include "utils/wire_bodies.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>
#include "cppzmq/zmq_addon.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace pylabhub::wire::adapter
{

namespace
{
// HEP-CORE-0046 §I-REPLAY-BOUND — the 6 msg_types that carry the
// security triple `(client_nonce, client_wall_ts, envelope_hash)`:
// REG_REQ, CONSUMER_REG_REQ, ENDPOINT_UPDATE_REQ,
// CHANNEL_AUTH_APPLIED_REQ, DEREG_REQ, CONSUMER_DEREG_REQ.
// The pre-Tier-2 list (2026-07-12) only listed 4 — missing
// CHANNEL_AUTH_APPLIED_REQ and CONSUMER_DEREG_REQ.  Sending either
// without the triple would silently violate I-REPLAY-BOUND: an
// attacker with captured messages could replay them beyond the
// nonce-dedup window since no nonce would be present to reject.
constexpr std::array<std::string_view, 6> kRegFamilyMsgTypes = {
    "REG_REQ",   "CONSUMER_REG_REQ",   "ENDPOINT_UPDATE_REQ", "CHANNEL_AUTH_APPLIED_REQ",
    "DEREG_REQ", "CONSUMER_DEREG_REQ",
};
} // namespace

bool msg_type_carries_security_triple(std::string_view msg_type) noexcept
{
    for (auto s : kRegFamilyMsgTypes)
    {
        if (s == msg_type)
            return true;
    }
    return false;
}

zmq::multipart_t encode_dealer_send(std::string_view msg_type, const EncodeContext &ctx,
                                    nlohmann::json payload)
{
    // Sanity checks that catch encoder-side misuse loudly, not at wire time.
    // These are contract violations, not runtime conditions — release-mode
    // callers are expected to satisfy them.
    if (msg_type.empty())
    {
        throw WireBodyError("wire_adapter: msg_type empty");
    }
    if (ctx.dealer_role_uid.empty())
    {
        throw WireBodyError("wire_adapter: dealer_role_uid empty "
                            "(I-DEALER-IDENTITY)");
    }
    if (ctx.correlation_id.empty() && !is_notify_msg_type(msg_type))
    {
        throw WireBodyError("wire_adapter: correlation_id empty on non-NOTIFY "
                            "msg_type (I-CORRELATION-STABLE)");
    }

    // Augment payload with the security triple for REG-family msg_types.
    // The envelope_hash gets stamped by WireEnvelope::build_dealer_send after
    // we hand off the body — we don't set it here.
    if (msg_type_carries_security_triple(msg_type))
    {
        if (ctx.client_nonce.empty())
        {
            throw WireBodyError("wire_adapter: client_nonce empty on "
                                "REG-family msg_type " +
                                std::string(msg_type));
        }
        if (ctx.client_wall_ts == 0)
        {
            throw WireBodyError("wire_adapter: client_wall_ts=0 on REG-family "
                                "msg_type " +
                                std::string(msg_type));
        }
        // Idempotent: if caller pre-populated (e.g., in a retry), preserve
        // their values.  Otherwise fill from the context.
        if (!payload.contains("client_nonce"))
        {
            payload["client_nonce"] = std::string(ctx.client_nonce);
        }
        if (!payload.contains("client_wall_ts"))
        {
            payload["client_wall_ts"] = ctx.client_wall_ts;
        }
    }

    return WireEnvelope::build_dealer_send(ctx.dealer_role_uid, msg_type, ctx.correlation_id,
                                           std::move(payload));
}

std::optional<DecodedRouterMsg> decode_router_recv(zmq::multipart_t &&msg, ParseError *err_out)
{
    // Parse the 5-frame envelope; validation lives in WireEnvelope::parse_router_recv.
    auto env_opt = WireEnvelope::parse_router_recv(std::move(msg), err_out);
    if (!env_opt.has_value())
    {
        return std::nullopt;
    }

    DecodedRouterMsg out{std::move(*env_opt), std::string(), nlohmann::json::object()};
    out.msg_type = std::string(out.env.msg_type());

    // Body — hand back the parsed JSON object.  The envelope already
    // validated envelope_hash + basic shape.  The security-triple fields
    // (client_nonce / client_wall_ts) are validated by the typed body
    // class at handler-side construction time (Tier 3); here in Tier 1
    // we just pass them through so legacy handlers see them alongside the
    // fields they already read.
    out.body_for_legacy_handler = out.env.body();

    return out;
}

} // namespace pylabhub::wire::adapter
