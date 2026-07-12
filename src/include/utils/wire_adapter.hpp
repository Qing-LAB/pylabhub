#pragma once
/**
 * @file wire_adapter.hpp
 * @brief Adapter layer between the current-shape JSON dispatch (broker_service.cpp
 *        handle_XXX / broker_request_comm.cpp do_request) and the typed wire
 *        envelope + body classes from HEP-CORE-0046 §14.
 *
 * The adapter exists so Phase B can land in staged, testable pieces rather than
 * one 1500-line atomic bang:
 *
 *   Tier 1 (this file + wire_adapter.cpp): encoder + decoder helpers that ride
 *   alongside the current code without changing wire behavior.  Round-trip
 *   tested for semantic equivalence.
 *
 *   Tier 2 (later commits): swap BRC send / broker recv over to these helpers.
 *   Old handler bodies stay signed as `handle_XXX(nlohmann::json, ...)` — the
 *   decoder feeds them the legacy-shape JSON they already understand.  Wire
 *   flips atomically; handler signatures do NOT.
 *
 *   Tier 3 (later commits): migrate individual handler signatures to typed body
 *   classes.  Local refactors; no wire risk.
 *
 * **Semantic contract of the adapter** — this is what the round-trip tests
 * prove and what future code depends on:
 *
 *   Encoder augments the input payload with:
 *     - `envelope_hash` (stamped by `WireEnvelope::build_dealer_send`)
 *     - `client_nonce` + `client_wall_ts` for REG-family msg_types
 *   Encoder does NOT rewrite, rename, or drop any field present in the input.
 *
 *   Decoder produces `body_for_legacy_handler` such that:
 *     - Every field the encoder saw is present, byte-identical.
 *     - `envelope_hash` + security triple are present (as the encoder added them).
 *     - No other fields are added.
 *     - The receiver can pass this JSON to the current `handle_XXX(payload, ...)`
 *       and observe the same behavior as if the message had come in via the
 *       current 2-frame wire.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"
#include "utils/wire_envelope.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace zmq
{
class multipart_t;
}

namespace pylabhub::wire::adapter
{

/// REG-family msg_types (per HEP-CORE-0046 §14.3) that carry the security
/// triple `{client_nonce, client_wall_ts, envelope_hash}`.  The encoder
/// synthesizes `client_nonce` + `client_wall_ts` for these; other msg_types
/// get only `envelope_hash`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool
msg_type_carries_security_triple(std::string_view msg_type) noexcept;

/// Fixed inputs the encoder needs on top of the caller's JSON payload.  The
/// caller (BRC-side send) provides:
///   - `dealer_role_uid` — DEALER's `ZMQ_ROUTING_ID` = its owning role's
///     `role_uid` (I-DEALER-IDENTITY).  Mixed into `envelope_hash`; does NOT
///     go on the DEALER-side wire itself (libzmq attaches it to Frame 0
///     when routing to the ROUTER).
///   - `correlation_id` — unique per outbound REQ.  Empty allowed ONLY for
///     msg_types ending in `_NOTIFY` (I-CORRELATION-STABLE).
///   - `client_nonce` — required non-empty for REG-family msg_types.
///     Ignored (may be empty) for others.
///   - `client_wall_ts` — required non-zero for REG-family msg_types.
///     Ignored (may be 0) for others.
struct EncodeContext
{
    std::string_view dealer_role_uid;
    std::string_view correlation_id;
    std::string_view client_nonce{};
    std::uint64_t    client_wall_ts{0};
};

/// Encode the JSON payload BRC currently builds into a wire multipart ready
/// for `zmq::send_multipart` on the DEALER.  Augments `payload` with the
/// security triple (for REG-family) and stamps `envelope_hash`, then packages
/// into the 4-frame DEALER-side wire.
///
/// **Ownership**: `payload` is moved.  On return, the caller receives the
/// multipart_t ready to send; the payload's contents live inside it.
///
/// **Preconditions** (release-mode contract; caller responsibility):
///   - `msg_type` non-empty, <=64 bytes.
///   - `ctx.dealer_role_uid` non-empty (I-DEALER-IDENTITY).
///   - `ctx.correlation_id` non-empty unless msg_type ends with `_NOTIFY`.
///   - For REG-family msg_type: `ctx.client_nonce` non-empty AND
///     `ctx.client_wall_ts` != 0.
///
/// A precondition violation raises `pylabhub::wire::WireBodyError` from the
/// typed-body-class constructor — this catches encoder bugs at development
/// time, not at wire time.
[[nodiscard]] PYLABHUB_UTILS_EXPORT zmq::multipart_t
encode_dealer_send(std::string_view msg_type,
                   const EncodeContext &ctx,
                   nlohmann::json      payload);

/// Result of decoding an inbound ROUTER-side multipart.
struct DecodedRouterMsg
{
    /// The parsed envelope.  Owning; must outlive any string_view into it.
    WireEnvelope   env;

    /// Convenience mirror of `env.msg_type()` as an owning string, for
    /// callers that need to key a dispatcher.
    std::string    msg_type;

    /// The Frame 4 body content, shape-compatible with what the current
    /// `handle_XXX(nlohmann::json payload, ...)` handlers already accept.
    /// The adapter guarantees: every field the encoder was given is
    /// present here byte-identical, plus the security triple + envelope
    /// hash the encoder added.
    nlohmann::json body_for_legacy_handler;
};

/// Decode an inbound ROUTER-side multipart into an envelope + a JSON body
/// ready to hand to a legacy handler.  Returns `std::nullopt` on any
/// envelope-level validation failure (frame count, hash mismatch, empty
/// correlation_id on non-NOTIFY, etc.); on failure `*err_out` is populated
/// so the caller can WARN with a specific reason.  The message is DROPPED —
/// no reply is sent, because a sender that violated the envelope contract
/// cannot be reliably identified for a reply.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<DecodedRouterMsg>
decode_router_recv(zmq::multipart_t &&msg,
                   ParseError        *err_out = nullptr);

}  // namespace pylabhub::wire::adapter
