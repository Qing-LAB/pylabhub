#pragma once
/**
 * @file wire_bodies.hpp
 * @brief Typed body classes for pylabhub control-plane wire messages.
 *
 * One class per msg_type per HEP-CORE-0046 §14.3.
 * Each class:
 *   - Constructor takes `const nlohmann::json&` and validates required fields
 *     (throws WireBodyError on shape mismatch or missing required field)
 *   - Exposes named accessors ONLY for the fields ITS msg_type carries
 *   - `to_json()` returns a JSON object suitable for
 *     `WireEnvelope::build_dealer_send(..., std::move(body.to_json()))`
 *
 * NO handler ever calls `body.value("field", ...)` — the accessor IS the
 * schema.  Adding a wire field means adding one accessor here; no scatter.
 *
 * Security triple `{client_nonce, client_wall_ts, envelope_hash}` per
 * I-REPLAY-BOUND + I-ENVELOPE-BODY-BINDING lives on REG-family bodies
 * (msg_types that mutate admission state).  Every body carries
 * `envelope_hash` (stamped/verified by WireEnvelope; body classes expose
 * it as an accessor for callers that need the value).
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Full JSON header needed for typed accessors that return json subobjects.
#include <nlohmann/json.hpp>

namespace pylabhub::wire
{

/// Thrown by body-class constructors when a required field is missing or
/// has the wrong JSON type.  Broker's ROUTER poll-loop catches at the
/// dispatch site and replies with `INVALID_REQUEST error_code=
/// BODY_SCHEMA_VIOLATION`.
class PYLABHUB_UTILS_EXPORT WireBodyError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// ── Common accessors mix-in ───────────────────────────────────────────
//
// All body classes carry `envelope_hash` (I-ENVELOPE-BODY-BINDING) —
// stamped by WireEnvelope on build and validated by WireEnvelope on
// parse.  Body classes expose it as a passthrough accessor.
//
// REG-family bodies (msg_types that mutate admission state) additionally
// carry `client_nonce` and `client_wall_ts` (I-REPLAY-BOUND) — checked
// by the broker's admission gate BEFORE any state mutation.
//
// The mix-in is a plain aggregate: derived classes hold a `nlohmann::json`
// body member and delegate to these free functions.  No virtual dispatch;
// no per-class boilerplate.

namespace detail
{

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string read_string(
    const nlohmann::json &body, const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string read_string_or_empty(
    const nlohmann::json &body, const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint32_t read_u32(
    const nlohmann::json &body, const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint64_t read_u64(
    const nlohmann::json &body, const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint64_t read_u64_or_zero(
    const nlohmann::json &body, const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT const nlohmann::json &read_object(
    const nlohmann::json &body, const char *field);

/// Validate that `body[field]` exists and has the requested JSON kind
/// (string/number/object/array).  Used at body-class construction to
/// fail-loud on shape drift.
enum class JsonKind { String, U32, U64, Object, Array };
PYLABHUB_UTILS_EXPORT void require(const nlohmann::json &body,
                                    const char           *field,
                                    JsonKind              kind);

/// Validate security-triple presence on REG-family bodies.  Throws
/// WireBodyError on any missing / mistyped field.  Called from every
/// REG-family body constructor.
PYLABHUB_UTILS_EXPORT void require_security_triple(
    const nlohmann::json &body);

/// Validate envelope_hash presence + string type on every body.
PYLABHUB_UTILS_EXPORT void require_envelope_hash(const nlohmann::json &body);

}  // namespace detail

// ── Per-msg_type body classes ────────────────────────────────────────
//
// Each class wraps a nlohmann::json body_ that owns its fields.  Construct
// once (validates), read many via typed accessors.  Move-only to avoid
// accidental copies of large JSON payloads (e.g., initial_allowlist).

#define PLH_WIRE_BODY_CLASS(Name)                                              \
    class PYLABHUB_UTILS_EXPORT Name                                           \
    {                                                                          \
      public:                                                                  \
        explicit Name(nlohmann::json body);                                    \
        Name(Name &&) noexcept            = default;                           \
        Name &operator=(Name &&) noexcept = default;                           \
        Name(const Name &)                = delete;                            \
        Name &operator=(const Name &)     = delete;                            \
                                                                               \
        [[nodiscard]] std::string envelope_hash() const                        \
        {                                                                      \
            return detail::read_string(body_, "envelope_hash");                \
        }                                                                      \
                                                                               \
        [[nodiscard]] const nlohmann::json &to_json() const noexcept           \
        {                                                                      \
            return body_;                                                      \
        }                                                                      \
        [[nodiscard]] nlohmann::json release_json() noexcept                   \
        {                                                                      \
            return std::move(body_);                                           \
        }                                                                      \
                                                                               \
      private:                                                                 \
        nlohmann::json body_;

// === REG-family (carries security triple) ============================

PLH_WIRE_BODY_CLASS(RegReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string role_type() const
    {
        return detail::read_string(body_, "role_type");
    }
    [[nodiscard]] std::string role_name() const
    {
        return detail::read_string(body_, "role_name");
    }
    [[nodiscard]] std::string channel_topology() const
    {
        return detail::read_string_or_empty(body_, "channel_topology");
    }
    [[nodiscard]] std::string data_transport() const
    {
        return detail::read_string(body_, "data_transport");
    }
    [[nodiscard]] std::string zmq_pubkey() const
    {
        return detail::read_string(body_, "zmq_pubkey");
    }
    [[nodiscard]] std::uint32_t broker_proto() const
    {
        return detail::read_u32(body_, "broker_proto");
    }
    [[nodiscard]] std::string schema_hash() const
    {
        return detail::read_string(body_, "schema_hash");
    }
    [[nodiscard]] std::uint32_t schema_version() const
    {
        return detail::read_u32(body_, "schema_version");
    }
    [[nodiscard]] std::string schema_id() const
    {
        return detail::read_string_or_empty(body_, "schema_id");
    }
    [[nodiscard]] std::string schema_blds() const
    {
        return detail::read_string_or_empty(body_, "schema_blds");
    }
    [[nodiscard]] std::string schema_owner() const
    {
        return detail::read_string_or_empty(body_, "schema_owner");
    }
    [[nodiscard]] const nlohmann::json &abi_fingerprint() const
    {
        return detail::read_object(body_, "abi_fingerprint");
    }
    [[nodiscard]] std::string client_nonce() const
    {
        return detail::read_string(body_, "client_nonce");
    }
    [[nodiscard]] std::uint64_t client_wall_ts() const
    {
        return detail::read_u64(body_, "client_wall_ts");
    }
};

PLH_WIRE_BODY_CLASS(EndpointUpdateReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string endpoint_type() const
    {
        return detail::read_string(body_, "endpoint_type");
    }
    [[nodiscard]] std::string endpoint() const
    {
        return detail::read_string(body_, "endpoint");
    }
    [[nodiscard]] std::string client_nonce() const
    {
        return detail::read_string(body_, "client_nonce");
    }
    [[nodiscard]] std::uint64_t client_wall_ts() const
    {
        return detail::read_u64(body_, "client_wall_ts");
    }
};

PLH_WIRE_BODY_CLASS(ChannelAuthAppliedReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::uint64_t applied_version() const
    {
        return detail::read_u64(body_, "applied_version");
    }
    [[nodiscard]] std::uint64_t instance_id() const
    {
        return detail::read_u64(body_, "instance_id");
    }
    [[nodiscard]] std::string client_nonce() const
    {
        return detail::read_string(body_, "client_nonce");
    }
    [[nodiscard]] std::uint64_t client_wall_ts() const
    {
        return detail::read_u64(body_, "client_wall_ts");
    }
};

PLH_WIRE_BODY_CLASS(DeregReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string client_nonce() const
    {
        return detail::read_string(body_, "client_nonce");
    }
    [[nodiscard]] std::uint64_t client_wall_ts() const
    {
        return detail::read_u64(body_, "client_wall_ts");
    }
};

// === ACK / query / NOTIFY bodies (envelope_hash only) ================

PLH_WIRE_BODY_CLASS(RegAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    [[nodiscard]] std::string error_code() const
    {
        return detail::read_string_or_empty(body_, "error_code");
    }
    [[nodiscard]] std::string message() const
    {
        return detail::read_string_or_empty(body_, "message");
    }
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::uint64_t instance_id() const
    {
        return detail::read_u64_or_zero(body_, "instance_id");
    }
    [[nodiscard]] std::uint64_t snapshot_version() const
    {
        return detail::read_u64_or_zero(body_, "snapshot_version");
    }
    [[nodiscard]] const nlohmann::json &heartbeat() const
    {
        return detail::read_object(body_, "heartbeat");
    }
    [[nodiscard]] const nlohmann::json &initial_allowlist() const
    {
        return body_.at("initial_allowlist");  // array expected
    }
    [[nodiscard]] const nlohmann::json &broker_abi_fingerprint() const
    {
        return detail::read_object(body_, "broker_abi_fingerprint");
    }
    [[nodiscard]] std::string broker_build_id() const
    {
        return detail::read_string_or_empty(body_, "broker_build_id");
    }
    [[nodiscard]] std::string broker_observer_pubkey_z85() const
    {
        return detail::read_string_or_empty(body_,
                                              "broker_observer_pubkey_z85");
    }
};

PLH_WIRE_BODY_CLASS(EndpointUpdateAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    [[nodiscard]] std::string message() const
    {
        return detail::read_string_or_empty(body_, "message");
    }
};

PLH_WIRE_BODY_CLASS(GetChannelAuthReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
};

PLH_WIRE_BODY_CLASS(GetChannelAuthAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    [[nodiscard]] const nlohmann::json &allowlist() const
    {
        return body_.at("allowlist");  // array of Z85 strings
    }
    [[nodiscard]] std::uint64_t channel_version() const
    {
        return detail::read_u64_or_zero(body_, "channel_version");
    }
};

PLH_WIRE_BODY_CLASS(ChannelAuthAppliedAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    [[nodiscard]] std::uint64_t confirmed_version() const
    {
        return detail::read_u64_or_zero(body_, "confirmed_version");
    }
};

PLH_WIRE_BODY_CLASS(DeregAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
};

PLH_WIRE_BODY_CLASS(HeartbeatReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
};

PLH_WIRE_BODY_CLASS(HeartbeatAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
};

PLH_WIRE_BODY_CLASS(DiscReqBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
};

PLH_WIRE_BODY_CLASS(DiscAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    // Discovery payload is protocol-extensible; expose the raw body for
    // fields not part of the fixed schema.
    [[nodiscard]] const nlohmann::json &raw_body() const noexcept
    {
        return body_;
    }
};

// === NOTIFY bodies (fire-and-forget; envelope_hash only) =============

PLH_WIRE_BODY_CLASS(ChannelAuthChangedNotifyBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string role_type() const
    {
        return detail::read_string(body_, "role_type");
    }
    [[nodiscard]] std::string phase() const
    {
        return detail::read_string(body_, "phase");
    }
    [[nodiscard]] std::uint64_t channel_version() const
    {
        return detail::read_u64_or_zero(body_, "channel_version");
    }
};

PLH_WIRE_BODY_CLASS(ChannelClosingNotifyBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string reason() const
    {
        return detail::read_string_or_empty(body_, "reason");
    }
};

PLH_WIRE_BODY_CLASS(ConsumerDiedNotifyBody)
  public:
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string reason() const
    {
        return detail::read_string_or_empty(body_, "reason");
    }
    [[nodiscard]] std::string target_role() const
    {
        return detail::read_string_or_empty(body_, "target_role");
    }
};

PLH_WIRE_BODY_CLASS(BandJoinNotifyBody)
  public:
    [[nodiscard]] std::string band() const
    {
        return detail::read_string(body_, "band");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string role_name() const
    {
        return detail::read_string(body_, "role_name");
    }
};

PLH_WIRE_BODY_CLASS(BandLeaveNotifyBody)
  public:
    [[nodiscard]] std::string band() const
    {
        return detail::read_string(body_, "band");
    }
    [[nodiscard]] std::string role_uid() const
    {
        return detail::read_string(body_, "role_uid");
    }
    [[nodiscard]] std::string role_name() const
    {
        return detail::read_string(body_, "role_name");
    }
};

#undef PLH_WIRE_BODY_CLASS

}  // namespace pylabhub::wire
