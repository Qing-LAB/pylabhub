#pragma once
/**
 * @file wire_envelope.hpp
 * @brief Typed 5-frame wire envelope for pylabhub control-plane messages.
 *
 * Locks the wire skeleton contract per HEP-CORE-0046 §14:
 * every control-plane message is 5 ZMQ frames — 4 skeleton frames identical
 * across every REQ/ACK/NOTIFY (Frame 0 = identity, Frame 1 = 'C' marker,
 * Frame 2 = msg_type, Frame 3 = correlation_id) + 1 body frame (Frame 4,
 * parsed via a msg-type-specific typed body class from wire_bodies.hpp).
 *
 * Enforces at parse time:
 *   - I-CORRELATION-STABLE: empty correlation_id on a REQ is INVALID_REQUEST
 *     (NOTIFYs are exempt — msg_type suffix `_NOTIFY` per I-MSG-TYPE-TAXONOMY)
 *   - I-ENVELOPE-BODY-BINDING: envelope_hash on the body is
 *     BLAKE2b-256(identity || msg_type || correlation_id); mismatch =
 *     ENVELOPE_TAMPERED, message dropped
 *
 * Enforces at build time:
 *   - Stamps envelope_hash on the body before returning
 *   - Requires non-empty correlation_id for non-NOTIFY msg_types
 *
 * Handlers never touch individual frames; every wire field is accessed via
 * either a WireEnvelope accessor (skeleton fields) or a typed body class
 * accessor (Frame 4 fields).  No `body.value("field", "")` scatter anywhere
 * in the codebase.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zmq
{
class multipart_t;
}

namespace pylabhub::wire
{

/// Single byte marking the control frame per HEP-CORE-0033 §9.
inline constexpr std::uint8_t kFrameTypeControl = 'C';

/// Reasons parse() may return std::nullopt.  Logged at WARN by the caller;
/// no reply sent because the sender is not identified reliably.
enum class ParseError
{
    frame_count,            ///< != 5 frames
    frame_type_marker,      ///< Frame 1 != 'C'
    msg_type_empty,         ///< Frame 2 empty
    msg_type_too_long,      ///< Frame 2 > 64 bytes
    correlation_too_long,   ///< Frame 3 > 64 bytes
    correlation_missing,    ///< Frame 3 empty on a non-NOTIFY msg_type (I-CORRELATION-STABLE)
    body_not_json,          ///< Frame 4 parse failure
    body_not_object,        ///< Frame 4 parsed but not a JSON object
    envelope_hash_missing,  ///< Body lacks envelope_hash field (I-ENVELOPE-BODY-BINDING)
    envelope_hash_mismatch, ///< envelope_hash disagrees with recomputed value
};

/**
 * Immutable wire envelope.  Constructed by parse(); read by handlers.
 * Owns its frame bytes so accessor string_views stay valid for the
 * envelope's lifetime.
 */
class PYLABHUB_UTILS_EXPORT WireEnvelope
{
  public:
    ~WireEnvelope();
    WireEnvelope(WireEnvelope &&) noexcept;
    WireEnvelope &operator=(WireEnvelope &&) noexcept;
    WireEnvelope(const WireEnvelope &) = delete;
    WireEnvelope &operator=(const WireEnvelope &) = delete;

    // ── Build (outbound) ──────────────────────────────────────────────
    //
    // Identity contract (I-DEALER-IDENTITY).  Every ROUTER↔DEALER
    // connection has one stable value: the DEALER's `routing_id`, which
    // by invariant equals its owning role's `role_uid`.  That single
    // value is what the sender uses for `envelope_hash` and what the
    // receiver reconstructs against — regardless of send direction.
    //
    // build_dealer_send emits the DEALER-side wire: 4 frames
    // `[C, msg_type, correlation_id, body]`.  Frame 0 is not written
    // here — libzmq attaches the DEALER's own routing_id when the send
    // reaches the ROUTER.  The `identity` argument passed in is that
    // routing_id (the sender DEALER's own `role_uid`); it does not go
    // on the wire from this call, but IS mixed into `envelope_hash` so
    // the receiver can verify by recomputing over the same input.
    //
    // build_router_send emits the ROUTER-side wire: 5 frames
    // `[target_identity, C, msg_type, correlation_id, body]`.  Frame 0
    // = the target DEALER's routing_id (= target role's `role_uid`),
    // used by libzmq to route to the right DEALER connection and by
    // the receiver to reconstruct `envelope_hash`.
    //
    // Preconditions (release-mode contract):
    //   - `identity` / `target_identity` non-empty (I-DEALER-IDENTITY)
    //   - `msg_type` is non-empty, <=64 bytes ASCII
    //   - `correlation_id` may be empty ONLY if msg_type ends with
    //     "_NOTIFY" (I-CORRELATION-STABLE / I-MSG-TYPE-TAXONOMY)
    //   - `body` is a JSON object; the envelope stamps `envelope_hash`
    //     onto it before serializing
    //
    // Returns a `zmq::multipart_t` ready for `zmq::send_multipart`.
    [[nodiscard]] static zmq::multipart_t build_dealer_send(std::string_view identity,
                                                            std::string_view msg_type,
                                                            std::string_view correlation_id,
                                                            nlohmann::json body);

    /// 5-frame variant for ROUTER-side sends targeting a specific DEALER.
    /// Frame 0 = the target DEALER's routing_id (target's `role_uid`)
    /// per I-DEALER-IDENTITY; libzmq routes on it, receiver recomputes
    /// `envelope_hash` over it.
    [[nodiscard]] static zmq::multipart_t build_router_send(std::string_view target_identity,
                                                            std::string_view msg_type,
                                                            std::string_view correlation_id,
                                                            nlohmann::json body);

    // ── Parse (inbound) ───────────────────────────────────────────────
    //
    // Parses a 5-frame envelope on the ROUTER side (which sees Frame 0
    // = sender's routing_id as libzmq attached it on the DEALER's send),
    // or a 4-frame envelope on the DEALER side (libzmq strips Frame 0
    // during routing; the DEALER's user code never sees it).
    //
    // Validates skeleton shape, correlation_id policy, and envelope_hash.
    // Returns std::nullopt on any violation with `err_out` populated so
    // the caller can WARN with a specific reason.

    [[nodiscard]] static std::optional<WireEnvelope>
    parse_router_recv(zmq::multipart_t &&msg, ParseError *err_out = nullptr);

    /// DEALER receive path: Frame 0 is not on the wire (libzmq stripped
    /// it during routing).  The DEALER caller supplies its OWN
    /// routing_id (== its `role_uid` per I-DEALER-IDENTITY) so
    /// `envelope_hash` can be reconstructed over the same input the
    /// sender used.  This is the SAME value the DEALER passes as
    /// `identity` when it uses `build_dealer_send` on outbound — one
    /// stable per-connection value in both directions.
    [[nodiscard]] static std::optional<WireEnvelope>
    parse_dealer_recv(zmq::multipart_t &&msg, std::string_view dealer_own_routing_id,
                      ParseError *err_out = nullptr);

    // ── Accessors ─────────────────────────────────────────────────────

    /// Frame 0 — ROUTER-captured identity (equals sender's role_uid per
    /// I-DEALER-IDENTITY).  On DEALER receive this is the broker's ROUTER
    /// identity as passed to parse_dealer_recv.
    [[nodiscard]] std::string_view identity() const noexcept;

    /// Frame 2 — msg_type ASCII string.
    [[nodiscard]] std::string_view msg_type() const noexcept;

    /// Frame 3 — correlation_id; empty for NOTIFY msg_types.
    [[nodiscard]] std::string_view correlation_id() const noexcept;

    /// True if msg_type suffix is `_NOTIFY` (I-MSG-TYPE-TAXONOMY).
    [[nodiscard]] bool is_notify() const noexcept;

    /// Frame 4 body as JSON.  Handlers should NOT read fields directly;
    /// they should construct a typed body class from wire_bodies.hpp
    /// which validates and exposes named accessors.
    [[nodiscard]] const nlohmann::json &body() const noexcept;

    // ── Envelope hash helper ──────────────────────────────────────────
    //
    // Public for L1 test verification + reuse by body-class self-tests.
    // Returns lowercase hex (64 chars) of BLAKE2b-256 over
    // (identity || msg_type || correlation_id).

    [[nodiscard]] static std::string compute_envelope_hash(std::string_view identity,
                                                           std::string_view msg_type,
                                                           std::string_view correlation_id);

    /// Impl is public for internal parse-helper access.  Not part of the
    /// stable API — treat as opaque outside wire_envelope.cpp.
    struct Impl;

  private:
    std::unique_ptr<Impl> pImpl;

    WireEnvelope();
};

/// True if `msg_type` ends with the "_NOTIFY" suffix (fire-and-forget
/// per I-MSG-TYPE-TAXONOMY).  Standalone helper for callers that need
/// the check without instantiating a WireEnvelope.
[[nodiscard]] inline bool is_notify_msg_type(std::string_view msg_type) noexcept
{
    constexpr std::string_view kSuffix{"_NOTIFY"};
    return msg_type.size() > kSuffix.size() &&
           msg_type.substr(msg_type.size() - kSuffix.size()) == kSuffix;
}

} // namespace pylabhub::wire
