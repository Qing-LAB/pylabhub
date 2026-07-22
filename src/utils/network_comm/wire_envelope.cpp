#include "utils/wire_envelope.hpp"

#include "utils/format_tools.hpp" // bytes_to_hex (canonical hex codec)
#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"

#include <nlohmann/json.hpp>
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include <array>
#include <cstring>
#include <string>

namespace pylabhub::wire
{

namespace
{

constexpr std::string_view kEnvelopeHashKey{"envelope_hash"};

// Concatenate identity + msg_type + correlation_id into a scratch buffer
// and BLAKE2b-256 hash it.  The concatenation is unambiguous because ALL
// three inputs have known lengths at the moment we compute the hash (we
// know exactly what bytes were / will be in Frames 0, 2, 3).  Length
// prefixing is not required for pylabhub's threat model — the hash binds
// this specific triple; splicing frames from different messages produces
// a different concatenation and therefore a different hash.
std::string compute_envelope_hash_impl(std::string_view identity, std::string_view msg_type,
                                       std::string_view correlation_id)
{
    std::string scratch;
    scratch.reserve(identity.size() + msg_type.size() + correlation_id.size());
    scratch.append(identity);
    scratch.append(msg_type);
    scratch.append(correlation_id);
    const auto hash =
        pylabhub::utils::security::secure().compute_blake2b_array(scratch.data(), scratch.size());
    return format_tools::bytes_to_hex(
        std::string_view(reinterpret_cast<const char *>(hash.data()), hash.size()));
}

constexpr std::size_t kMaxMsgTypeLen = 64;
constexpr std::size_t kMaxCorrelationIdLen = 64;

} // namespace

struct WireEnvelope::Impl
{
    std::string identity_bytes;
    std::string msg_type_bytes;
    std::string correlation_id_bytes;
    nlohmann::json body_json;
};

WireEnvelope::WireEnvelope() : pImpl(std::make_unique<Impl>()) {}
WireEnvelope::~WireEnvelope() = default;
WireEnvelope::WireEnvelope(WireEnvelope &&) noexcept = default;
WireEnvelope &WireEnvelope::operator=(WireEnvelope &&) noexcept = default;

std::string_view WireEnvelope::identity() const noexcept
{
    return pImpl->identity_bytes;
}
std::string_view WireEnvelope::msg_type() const noexcept
{
    return pImpl->msg_type_bytes;
}
std::string_view WireEnvelope::correlation_id() const noexcept
{
    return pImpl->correlation_id_bytes;
}
bool WireEnvelope::is_notify() const noexcept
{
    return is_notify_msg_type(pImpl->msg_type_bytes);
}
const nlohmann::json &WireEnvelope::body() const noexcept
{
    return pImpl->body_json;
}

std::string WireEnvelope::compute_envelope_hash(std::string_view identity,
                                                std::string_view msg_type,
                                                std::string_view correlation_id)
{
    return compute_envelope_hash_impl(identity, msg_type, correlation_id);
}

// ── Build ────────────────────────────────────────────────────────────

namespace
{

// Shared build core.  Stamps envelope_hash onto body then serializes the
// 4 skeleton-plus-body frames (Frames 1..4).  Caller prepends Frame 0
// only for ROUTER-side sends.
void stamp_and_append_body_frames(std::string_view identity, std::string_view msg_type,
                                  std::string_view correlation_id, nlohmann::json &body,
                                  zmq::multipart_t &out)
{
    body[std::string{kEnvelopeHashKey}] =
        compute_envelope_hash_impl(identity, msg_type, correlation_id);

    out.addmem(&kFrameTypeControl, 1);
    out.addstr(std::string{msg_type});
    out.addstr(std::string{correlation_id});
    out.addstr(body.dump());
}

} // namespace

zmq::multipart_t WireEnvelope::build_dealer_send(std::string_view identity,
                                                 std::string_view msg_type,
                                                 std::string_view correlation_id,
                                                 nlohmann::json body)
{
    // I-DEALER-IDENTITY: identity is required so envelope_hash covers
    // the same bytes the receiver will see in Frame 0.
    if (identity.empty())
    {
        throw std::invalid_argument("WireEnvelope::build_dealer_send: identity empty "
                                    "(I-DEALER-IDENTITY: routing_id ≡ role_uid must be set)");
    }
    if (msg_type.empty() || msg_type.size() > kMaxMsgTypeLen)
    {
        throw std::invalid_argument(
            "WireEnvelope::build_dealer_send: msg_type empty or > 64 bytes");
    }
    if (correlation_id.size() > kMaxCorrelationIdLen)
    {
        throw std::invalid_argument("WireEnvelope::build_dealer_send: correlation_id > 64 bytes");
    }
    if (correlation_id.empty() && !is_notify_msg_type(msg_type))
    {
        // I-CORRELATION-STABLE: empty allowed only on NOTIFY.
        throw std::invalid_argument("WireEnvelope::build_dealer_send: empty correlation_id on "
                                    "non-NOTIFY msg_type (I-CORRELATION-STABLE requires non-empty "
                                    "correlation_id on every REQ/ACK)");
    }
    if (!body.is_object())
    {
        throw std::invalid_argument("WireEnvelope::build_dealer_send: body must be a JSON object");
    }

    zmq::multipart_t frames;
    stamp_and_append_body_frames(identity, msg_type, correlation_id, body, frames);
    return frames;
}

zmq::multipart_t WireEnvelope::build_router_send(std::string_view target_identity,
                                                 std::string_view msg_type,
                                                 std::string_view correlation_id,
                                                 nlohmann::json body)
{
    // ROUTER-side build reuses the DEALER-side validation + hash stamp,
    // then prepends Frame 0 = target identity.
    if (target_identity.empty())
    {
        throw std::invalid_argument("WireEnvelope::build_router_send: target_identity empty");
    }
    auto frames = build_dealer_send(target_identity, msg_type, correlation_id, std::move(body));
    frames.pushmem(target_identity.data(), target_identity.size());
    return frames;
}

// ── Parse ────────────────────────────────────────────────────────────

namespace
{

// Parse-time validation of skeleton + body-object shape + envelope hash.
// On success, populates `out` (moved into caller's std::optional).
// On failure, writes reason to `*err_out` (if non-null) and returns false.
bool parse_common(std::string_view identity, std::string_view msg_type,
                  std::string_view correlation_id, std::string_view body_str,
                  WireEnvelope::Impl &out, ParseError *err_out)
{
    if (msg_type.empty())
    {
        if (err_out)
            *err_out = ParseError::msg_type_empty;
        return false;
    }
    if (msg_type.size() > kMaxMsgTypeLen)
    {
        if (err_out)
            *err_out = ParseError::msg_type_too_long;
        return false;
    }
    if (correlation_id.size() > kMaxCorrelationIdLen)
    {
        if (err_out)
            *err_out = ParseError::correlation_too_long;
        return false;
    }
    if (correlation_id.empty() && !is_notify_msg_type(msg_type))
    {
        // I-CORRELATION-STABLE.
        if (err_out)
            *err_out = ParseError::correlation_missing;
        return false;
    }

    nlohmann::json body;
    try
    {
        body = nlohmann::json::parse(body_str);
    }
    catch (const nlohmann::json::exception &)
    {
        if (err_out)
            *err_out = ParseError::body_not_json;
        return false;
    }
    if (!body.is_object())
    {
        if (err_out)
            *err_out = ParseError::body_not_object;
        return false;
    }

    // I-ENVELOPE-BODY-BINDING.
    if (!body.contains(std::string{kEnvelopeHashKey}) ||
        !body.at(std::string{kEnvelopeHashKey}).is_string())
    {
        if (err_out)
            *err_out = ParseError::envelope_hash_missing;
        return false;
    }
    const auto received_hash = body.at(std::string{kEnvelopeHashKey}).get<std::string>();
    const auto expected_hash = compute_envelope_hash_impl(identity, msg_type, correlation_id);
    if (received_hash != expected_hash)
    {
        if (err_out)
            *err_out = ParseError::envelope_hash_mismatch;
        return false;
    }

    out.identity_bytes.assign(identity);
    out.msg_type_bytes.assign(msg_type);
    out.correlation_id_bytes.assign(correlation_id);
    out.body_json = std::move(body);
    return true;
}

// Extract a std::string_view from a zmq::message_t.
inline std::string_view msg_as_view(const zmq::message_t &m)
{
    return std::string_view{static_cast<const char *>(m.data()), m.size()};
}

} // namespace

std::optional<WireEnvelope> WireEnvelope::parse_router_recv(zmq::multipart_t &&msg,
                                                            ParseError *err_out)
{
    // ROUTER receive: [identity, 'C', msg_type, correlation_id, body]
    if (msg.size() != 5)
    {
        if (err_out)
            *err_out = ParseError::frame_count;
        return std::nullopt;
    }
    const auto &identity_frame = msg[0];
    const auto &marker_frame = msg[1];
    const auto &msg_type_frame = msg[2];
    const auto &correlation_frame = msg[3];
    const auto &body_frame = msg[4];

    if (marker_frame.size() != 1 ||
        *static_cast<const std::uint8_t *>(marker_frame.data()) != kFrameTypeControl)
    {
        if (err_out)
            *err_out = ParseError::frame_type_marker;
        return std::nullopt;
    }

    WireEnvelope env;
    if (!parse_common(msg_as_view(identity_frame), msg_as_view(msg_type_frame),
                      msg_as_view(correlation_frame), msg_as_view(body_frame), *env.pImpl, err_out))
    {
        return std::nullopt;
    }
    return env;
}

std::optional<WireEnvelope> WireEnvelope::parse_dealer_recv(zmq::multipart_t &&msg,
                                                            std::string_view dealer_own_routing_id,
                                                            ParseError *err_out)
{
    // DEALER receive: ['C', msg_type, correlation_id, body] — 4 frames.
    // Frame 0 was stripped by libzmq during routing.  The caller passes
    // the DEALER's own routing_id (== its role_uid per I-DEALER-IDENTITY),
    // the same value the sender used when building envelope_hash.
    if (msg.size() != 4)
    {
        if (err_out)
            *err_out = ParseError::frame_count;
        return std::nullopt;
    }
    const auto &marker_frame = msg[0];
    const auto &msg_type_frame = msg[1];
    const auto &correlation_frame = msg[2];
    const auto &body_frame = msg[3];

    if (marker_frame.size() != 1 ||
        *static_cast<const std::uint8_t *>(marker_frame.data()) != kFrameTypeControl)
    {
        if (err_out)
            *err_out = ParseError::frame_type_marker;
        return std::nullopt;
    }

    WireEnvelope env;
    if (!parse_common(dealer_own_routing_id, msg_as_view(msg_type_frame),
                      msg_as_view(correlation_frame), msg_as_view(body_frame), *env.pImpl, err_out))
    {
        return std::nullopt;
    }
    return env;
}

} // namespace pylabhub::wire
