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
#include <string_view>
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

// **ProducerRegReqBody** — REG_REQ from a producer or processor per
// HEP-CORE-0036 §5b.4 + HEP-CORE-0034 §10.1.  Producer DECLARES the
// schema (schema_hash + schema_blds + schema_packing); the id string
// carries the version (`$name.v<N>` per HEP-CORE-0033 §G2.2.0b) so
// no separate `schema_version` wire field exists.  Wire-version + ABI
// compatibility is carried by `abi_fingerprint` per HEP-CORE-0032 §8;
// no separate `broker_proto` scalar on this body.
PLH_WIRE_BODY_CLASS(ProducerRegReqBody)
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
    // HEP-CORE-0034 §10.1 producer schema-declaration fields.  All
    // OPTIONAL individually — when `schema_id` non-empty the others
    // become required per HEP-0034 §10.1 (checked by broker at
    // admission, not by this body class).  Version is embedded in
    // `schema_id` (`$name.v<N>`); no separate schema_version field
    // per C2 resolution.
    [[nodiscard]] std::string schema_id() const
    {
        return detail::read_string_or_empty(body_, "schema_id");
    }
    [[nodiscard]] std::string schema_hash() const
    {
        return detail::read_string_or_empty(body_, "schema_hash");
    }
    [[nodiscard]] std::string schema_blds() const
    {
        return detail::read_string_or_empty(body_, "schema_blds");
    }
    [[nodiscard]] std::string schema_packing() const
    {
        return detail::read_string_or_empty(body_, "schema_packing");
    }
    [[nodiscard]] std::string schema_owner() const
    {
        return detail::read_string_or_empty(body_, "schema_owner");
    }
    [[nodiscard]] std::string flexzone_blds() const
    {
        return detail::read_string_or_empty(body_, "flexzone_blds");
    }
    [[nodiscard]] std::string flexzone_packing() const
    {
        return detail::read_string_or_empty(body_, "flexzone_packing");
    }
    // Transport-specific endpoints.  Both OPTIONAL — one of the two
    // is populated depending on `data_transport`; broker validates
    // presence per HEP-CORE-0036 §5b.4.
    [[nodiscard]] std::string zmq_node_endpoint() const
    {
        return detail::read_string_or_empty(body_, "zmq_node_endpoint");
    }
    [[nodiscard]] std::string shm_capability_endpoint() const
    {
        return detail::read_string_or_empty(body_, "shm_capability_endpoint");
    }
    // Diagnostic / early-death-detection field per C10.  OPTIONAL;
    // not used for target resolution (role_uid is the authoritative
    // resolution key post broker_proto 2→3 per HEP-CORE-0023 §2.1.1).
    [[nodiscard]] std::uint64_t producer_pid() const
    {
        return detail::read_u64_or_zero(body_, "producer_pid");
    }
    // ABI carrier per HEP-CORE-0032 §8.  REQUIRED.
    [[nodiscard]] const nlohmann::json &abi_fingerprint() const
    {
        return detail::read_object(body_, "abi_fingerprint");
    }
    [[nodiscard]] std::string build_id() const
    {
        return detail::read_string_or_empty(body_, "build_id");
    }
    // Optional inbox companion fields per HEP-CORE-0027 §4.1.
    [[nodiscard]] std::string inbox_endpoint() const
    {
        return detail::read_string_or_empty(body_, "inbox_endpoint");
    }
    [[nodiscard]] std::string inbox_schema_json() const
    {
        return detail::read_string_or_empty(body_, "inbox_schema_json");
    }
    [[nodiscard]] std::string inbox_packing() const
    {
        return detail::read_string_or_empty(body_, "inbox_packing");
    }
    [[nodiscard]] std::string inbox_checksum() const
    {
        return detail::read_string_or_empty(body_, "inbox_checksum");
    }
    // Security triple per HEP-CORE-0046 §I-REPLAY-BOUND.
    [[nodiscard]] std::string client_nonce() const
    {
        return detail::read_string(body_, "client_nonce");
    }
    [[nodiscard]] std::uint64_t client_wall_ts() const
    {
        return detail::read_u64(body_, "client_wall_ts");
    }
};

// **ConsumerRegReqBody** — CONSUMER_REG_REQ from a consumer or
// processor per HEP-CORE-0036 §5b.6 + HEP-CORE-0034 §10.2.  Consumer
// CITES the schema (`expected_schema_*` prefix); the `expected_`
// prefix is normative per HEP-0034 §10.2 last paragraph.  Same
// version-in-id form as producer (`$name.v<N>`); no separate
// `expected_schema_version` field.
PLH_WIRE_BODY_CLASS(ConsumerRegReqBody)
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
    // REQUIRED per HEP-CORE-0036 §5b.6 (C9 resolution).  Broker
    // rejects TRANSPORT_MISMATCH if this disagrees with channel's
    // stored `data_transport`.
    [[nodiscard]] std::string data_transport() const
    {
        return detail::read_string(body_, "data_transport");
    }
    [[nodiscard]] std::string zmq_pubkey() const
    {
        return detail::read_string(body_, "zmq_pubkey");
    }
    // HEP-CORE-0034 §10.2 consumer schema-citation fields; the
    // `expected_` prefix is normative per §10.2 last paragraph.
    // Named-citation mode: expected_schema_id + expected_schema_hash
    // required; blds + packing optional (defense-in-depth).
    // Anonymous-citation mode: expected_schema_blds +
    // expected_schema_packing required; expected_schema_hash
    // optional (self-consistency).  Empty mode: none required
    // (legacy backward-compat).  Broker checks mode + fields per
    // §10.2; this body class exposes accessors only.
    [[nodiscard]] std::string expected_schema_id() const
    {
        return detail::read_string_or_empty(body_, "expected_schema_id");
    }
    [[nodiscard]] std::string expected_schema_hash() const
    {
        return detail::read_string_or_empty(body_, "expected_schema_hash");
    }
    [[nodiscard]] std::string expected_schema_blds() const
    {
        return detail::read_string_or_empty(body_, "expected_schema_blds");
    }
    [[nodiscard]] std::string expected_schema_packing() const
    {
        return detail::read_string_or_empty(body_, "expected_schema_packing");
    }
    [[nodiscard]] std::string expected_flexzone_blds() const
    {
        return detail::read_string_or_empty(body_, "expected_flexzone_blds");
    }
    [[nodiscard]] std::string expected_flexzone_packing() const
    {
        return detail::read_string_or_empty(body_,
                                              "expected_flexzone_packing");
    }
    // Diagnostic / early-death-detection fields per C10.  Both
    // OPTIONAL; role_uid is the authoritative resolution key.
    [[nodiscard]] std::uint64_t consumer_pid() const
    {
        return detail::read_u64_or_zero(body_, "consumer_pid");
    }
    [[nodiscard]] std::string consumer_hostname() const
    {
        return detail::read_string_or_empty(body_, "consumer_hostname");
    }
    // ABI carrier per HEP-CORE-0032 §8.  REQUIRED.
    [[nodiscard]] const nlohmann::json &abi_fingerprint() const
    {
        return detail::read_object(body_, "abi_fingerprint");
    }
    [[nodiscard]] std::string build_id() const
    {
        return detail::read_string_or_empty(body_, "build_id");
    }
    // Optional inbox companion fields per HEP-CORE-0027 §4.1.
    [[nodiscard]] std::string inbox_endpoint() const
    {
        return detail::read_string_or_empty(body_, "inbox_endpoint");
    }
    [[nodiscard]] std::string inbox_schema_json() const
    {
        return detail::read_string_or_empty(body_, "inbox_schema_json");
    }
    [[nodiscard]] std::string inbox_packing() const
    {
        return detail::read_string_or_empty(body_, "inbox_packing");
    }
    [[nodiscard]] std::string inbox_checksum() const
    {
        return detail::read_string_or_empty(body_, "inbox_checksum");
    }
    // Security triple per HEP-CORE-0046 §I-REPLAY-BOUND.
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

// **ConsumerRegAckBody** — CONSUMER_REG_ACK per HEP-CORE-0046 §14.3
// (erratum: split from RegAckBody 2026-07-15 task #45) + HEP-CORE-0036
// §5b/§6.4.  Consumer-side ACK does NOT carry `initial_allowlist`
// (that's producer-side per HEP-0046 §14.3); it carries `producers[]`
// and `data_transport` per §5b's unified transport-discriminator
// shape (B-4, #289, 2026-06-25).  Success ACK only — broker emits
// msg_type "ERROR" for the failure case, so ConsumerRegAckBody's
// required-field set covers the success path.
PLH_WIRE_BODY_CLASS(ConsumerRegAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
    [[nodiscard]] std::string channel_name() const
    {
        return detail::read_string(body_, "channel_name");
    }
    [[nodiscard]] std::string data_transport() const
    {
        return detail::read_string(body_, "data_transport");
    }
    [[nodiscard]] const nlohmann::json &heartbeat() const
    {
        return detail::read_object(body_, "heartbeat");
    }
    [[nodiscard]] const nlohmann::json &producers() const
    {
        return body_.at("producers");  // array expected
    }
    [[nodiscard]] const nlohmann::json &broker_abi_fingerprint() const
    {
        return detail::read_object(body_, "broker_abi_fingerprint");
    }
    [[nodiscard]] std::string broker_build_id() const
    {
        return detail::read_string_or_empty(body_, "broker_build_id");
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

// **HeartbeatNotifyBody** — presence maintenance (HEP-CORE-0023 §2.5).
// Renamed from `HeartbeatReqBody` per C13 for taxonomy consistency
// with HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY (`_NOTIFY` suffix =
// fire-and-forget; heartbeat has no ACK path in the broker).
PLH_WIRE_BODY_CLASS(HeartbeatNotifyBody)
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
    // Diagnostic / early-death-detection field per C10.  OPTIONAL.
    [[nodiscard]] std::uint64_t producer_pid() const
    {
        return detail::read_u64_or_zero(body_, "producer_pid");
    }
    // Optional metrics piggyback per HEP-CORE-0019 §2.3 Phase 6.
    [[nodiscard]] const nlohmann::json &metrics() const
    {
        return detail::read_object(body_, "metrics");
    }
    [[nodiscard]] bool has_metrics() const
    {
        return body_.contains("metrics") && body_.at("metrics").is_object();
    }
};

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

// === Admin console family (HEP-CORE-0033 §11) ========================
//
// The typed operator-console protocol.  Admin messages are NOT part of
// the REG admission taxonomy (wire_dispatch): they carry no security
// triple and are gated by the sealed session id (§11.0.5), not by the
// broker's admission pipeline.  These are the first fully-typed pathway
// and the reference for the JSON→typed migration (task #57).  Every body
// carries `envelope_hash` (stamped/validated by WireEnvelope) like the
// rest of the control plane.

/// msg_type constants for the typed admin console (no central msg_type
/// registry exists; these named constants keep admin off the string-literal
/// scatter the broker plane still uses).
inline constexpr std::string_view kAdminHelloReq        = "ADMIN_HELLO_REQ";
inline constexpr std::string_view kAdminHelloAck        = "ADMIN_HELLO_ACK";
inline constexpr std::string_view kAdminPingReq         = "ADMIN_PING_REQ";
inline constexpr std::string_view kAdminPingAck         = "ADMIN_PING_ACK";
inline constexpr std::string_view kAdminCloseChannelReq = "ADMIN_CLOSE_CHANNEL_REQ";
inline constexpr std::string_view kAdminCloseChannelAck = "ADMIN_CLOSE_CHANNEL_ACK";
inline constexpr std::string_view kAdminBroadcastChannelReq = "ADMIN_BROADCAST_CHANNEL_REQ";
inline constexpr std::string_view kAdminBroadcastChannelAck = "ADMIN_BROADCAST_CHANNEL_ACK";
inline constexpr std::string_view kAdminRequestShutdownReq  = "ADMIN_REQUEST_SHUTDOWN_REQ";
inline constexpr std::string_view kAdminRequestShutdownAck  = "ADMIN_REQUEST_SHUTDOWN_ACK";
inline constexpr std::string_view kAdminListChannelsReq = "ADMIN_LIST_CHANNELS_REQ";
inline constexpr std::string_view kAdminListChannelsAck = "ADMIN_LIST_CHANNELS_ACK";
inline constexpr std::string_view kAdminListRolesReq    = "ADMIN_LIST_ROLES_REQ";
inline constexpr std::string_view kAdminListRolesAck    = "ADMIN_LIST_ROLES_ACK";
inline constexpr std::string_view kAdminListBandsReq    = "ADMIN_LIST_BANDS_REQ";
inline constexpr std::string_view kAdminListBandsAck    = "ADMIN_LIST_BANDS_ACK";
inline constexpr std::string_view kAdminListPeersReq    = "ADMIN_LIST_PEERS_REQ";
inline constexpr std::string_view kAdminListPeersAck    = "ADMIN_LIST_PEERS_ACK";
inline constexpr std::string_view kAdminGetChannelReq   = "ADMIN_GET_CHANNEL_REQ";
inline constexpr std::string_view kAdminGetChannelAck   = "ADMIN_GET_CHANNEL_ACK";
inline constexpr std::string_view kAdminGetRoleReq      = "ADMIN_GET_ROLE_REQ";
inline constexpr std::string_view kAdminGetRoleAck      = "ADMIN_GET_ROLE_ACK";
inline constexpr std::string_view kAdminQueryMetricsReq = "ADMIN_QUERY_METRICS_REQ";
inline constexpr std::string_view kAdminQueryMetricsAck = "ADMIN_QUERY_METRICS_ACK";
/// Typed error reply for any admin failure (unauthorized, invalid session,
/// bad params, not-found).  Carries the §11.5 error code + message.
inline constexpr std::string_view kAdminError           = "ADMIN_ERROR";

/// ADMIN_HELLO_REQ — console establishment (§11.0.5 / §11.3).  The admin
/// token authorizes the session; `label` is the operator-supplied display
/// name folded into the session id.
PLH_WIRE_BODY_CLASS(AdminHelloReqBody)
  public:
    [[nodiscard]] std::string token() const
    {
        return detail::read_string(body_, "token");
    }
    [[nodiscard]] std::string label() const
    {
        return detail::read_string(body_, "label");
    }
};

/// ADMIN_HELLO_ACK — carries the sealed, connection-bound session id the
/// operator presents on every later command (§11.0.5).
PLH_WIRE_BODY_CLASS(AdminHelloAckBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
};

/// ADMIN_PING_REQ — liveness / round-trip proof.  Carries only the session
/// id (verified against the connection facts before dispatch).
PLH_WIRE_BODY_CLASS(AdminPingReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
};

/// ADMIN_PING_ACK — `status` = "ok" (JsonKind has no bool; every ACK uses a
/// status string, matching the broker ACK bodies).
PLH_WIRE_BODY_CLASS(AdminPingAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
};

/// ADMIN_CLOSE_CHANNEL_REQ — control command (§11.2).  Fire-and-forget:
/// the ACK means accepted, not completed (§11.0.4).
PLH_WIRE_BODY_CLASS(AdminCloseChannelReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
    [[nodiscard]] std::string channel() const
    {
        return detail::read_string(body_, "channel");
    }
};

/// ADMIN_CLOSE_CHANNEL_ACK — `status` = "ok" (accepted / enqueued).
PLH_WIRE_BODY_CLASS(AdminCloseChannelAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
};

/// Session-only request — carries just the session id.  Shared by the
/// list queries (list_channels/roles/bands/peers) and request_shutdown.
PLH_WIRE_BODY_CLASS(AdminSessionReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
};

/// Named request — session id + a target name.  Shared by get_channel /
/// get_role.
PLH_WIRE_BODY_CLASS(AdminNamedReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
    [[nodiscard]] std::string name() const
    {
        return detail::read_string(body_, "name");
    }
};

/// ADMIN_BROADCAST_CHANNEL_REQ — hub-originated broadcast (§11.2).
PLH_WIRE_BODY_CLASS(AdminBroadcastChannelReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
    [[nodiscard]] std::string channel() const
    {
        return detail::read_string(body_, "channel");
    }
    [[nodiscard]] std::string message() const
    {
        return detail::read_string(body_, "message");
    }
    /// Optional opaque data payload; empty when absent.
    [[nodiscard]] std::string data() const
    {
        return detail::read_string_or_empty(body_, "data");
    }
};

/// ADMIN_QUERY_METRICS_REQ — session id + an optional metrics filter object
/// (empty object = all categories).
PLH_WIRE_BODY_CLASS(AdminQueryMetricsReqBody)
  public:
    [[nodiscard]] std::string session_id() const
    {
        return detail::read_string(body_, "session_id");
    }
    /// Filter object; empty object when absent.
    [[nodiscard]] const nlohmann::json &filter() const
    {
        return detail::read_object(body_, "filter");
    }
};

/// Query result ACK — carries the dynamic snapshot as an object (the
/// query result has no fixed schema; the object IS the typed field, like
/// `RegAck.heartbeat`).  Shared by every query ACK.
PLH_WIRE_BODY_CLASS(AdminResultAckBody)
  public:
    [[nodiscard]] const nlohmann::json &result() const
    {
        return detail::read_object(body_, "result");
    }
};

/// Control ACK — `status` = "ok" (accepted).  Shared by broadcast /
/// request_shutdown (close has its own for symmetry with the REQ).
PLH_WIRE_BODY_CLASS(AdminStatusAckBody)
  public:
    [[nodiscard]] std::string status() const
    {
        return detail::read_string(body_, "status");
    }
};

/// ADMIN_ERROR — typed failure reply (§11.5 code + message).
PLH_WIRE_BODY_CLASS(AdminErrorBody)
  public:
    [[nodiscard]] std::string code() const
    {
        return detail::read_string(body_, "code");
    }
    [[nodiscard]] std::string message() const
    {
        return detail::read_string(body_, "message");
    }
};

#undef PLH_WIRE_BODY_CLASS

}  // namespace pylabhub::wire
