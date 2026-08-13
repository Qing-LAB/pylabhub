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
#include "utils/schema_types.hpp" // SchemaSpec — typed inbox_schema sub-structure (HEP-0046 B.2)

#include <cstdint>
#include <memory>
#include <optional>
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

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string read_string(const nlohmann::json &body,
                                                            const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string read_string_or_empty(const nlohmann::json &body,
                                                                     const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint32_t read_u32(const nlohmann::json &body,
                                                           const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint64_t read_u64(const nlohmann::json &body,
                                                           const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::uint64_t read_u64_or_zero(const nlohmann::json &body,
                                                                   const char *field);

[[nodiscard]] PYLABHUB_UTILS_EXPORT const nlohmann::json &read_object(const nlohmann::json &body,
                                                                      const char *field);

/// Validate that `body[field]` exists and has the requested JSON kind
/// (string/number/object/array).  Used at body-class construction to
/// fail-loud on shape drift.
enum class JsonKind
{
    String,
    U32,
    U64,
    Object,
    Array
};
PYLABHUB_UTILS_EXPORT void require(const nlohmann::json &body, const char *field, JsonKind kind);

/// Validate security-triple presence on REG-family bodies.  Throws
/// WireBodyError on any missing / mistyped field.  Called from every
/// REG-family body constructor.
PYLABHUB_UTILS_EXPORT void require_security_triple(const nlohmann::json &body);

/// Validate envelope_hash presence + string type on every body.
PYLABHUB_UTILS_EXPORT void require_envelope_hash(const nlohmann::json &body);

/// Refuse a field this msg_type must NOT carry, naming @p why in the error.
///
/// Ignoring an unwanted field is the softer option and the wrong one where
/// the field asserts something the broker decides for itself: the client
/// goes on believing it set that value, and its message travels under a
/// different one.  Refusing says so plainly, at the wire boundary.
PYLABHUB_UTILS_EXPORT void refuse_field(const nlohmann::json &body, const char *field,
                                        const char *why);

} // namespace detail

// ── Per-msg_type body classes ────────────────────────────────────────
//
// Each class wraps a nlohmann::json body_ that owns its fields.  Construct
// once (validates), read many via typed accessors.  Move-only to avoid
// accidental copies of large JSON payloads (e.g., initial_allowlist).

#define PLH_WIRE_BODY_CLASS(Name)                                                                  \
    class PYLABHUB_UTILS_EXPORT Name                                                               \
    {                                                                                              \
      public:                                                                                      \
        explicit Name(nlohmann::json body);                                                        \
        Name(Name &&) noexcept = default;                                                          \
        Name &operator=(Name &&) noexcept = default;                                               \
        Name(const Name &) = delete;                                                               \
        Name &operator=(const Name &) = delete;                                                    \
                                                                                                   \
        [[nodiscard]] std::string envelope_hash() const                                            \
        {                                                                                          \
            return detail::read_string(body_, "envelope_hash");                                    \
        }                                                                                          \
                                                                                                   \
        [[nodiscard]] const nlohmann::json &to_json() const noexcept { return body_; }             \
        [[nodiscard]] nlohmann::json release_json() noexcept { return std::move(body_); }          \
                                                                                                   \
      private:                                                                                     \
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
    return detail::read_string_or_empty(body_, "role_name");
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
// Optional producer hostname — diagnostic / record only.
[[nodiscard]] std::string producer_hostname() const
{
    return detail::read_string_or_empty(body_, "producer_hostname");
}
// Optional free-form producer metadata object, stored verbatim on the
// ProducerEntry.  Absent → `has_metadata()` is false; `metadata()` is only
// valid (does not throw) when `has_metadata()` is true.
[[nodiscard]] bool has_metadata() const
{
    auto it = body_.find("metadata");
    return it != body_.end() && it->is_object();
}
[[nodiscard]] const nlohmann::json &metadata() const
{
    return detail::read_object(body_, "metadata");
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
// Optional inbox companion fields per HEP-CORE-0027 §4.1.  The
// doubly-encoded `inbox_schema_json` (a string whose content is the
// HEP-0027 §6 canonical schema object) is parsed ONCE at construction
// via the canonical `hub::parse_schema_json`; `inbox_schema()` exposes
// the typed result and malformed content rejects as
// BODY_SCHEMA_VIOLATION at the wire boundary — no downstream reader
// ever re-parses the string.  Packing is carried once, INSIDE the
// schema object (HEP-CORE-0034 §6.2 requires it there); the separate
// `inbox_packing` wire field is retired.
[[nodiscard]] std::string inbox_endpoint() const
{
    return detail::read_string_or_empty(body_, "inbox_endpoint");
}
/// Raw advertisement string, stored verbatim on the ProducerEntry /
/// ConsumerEntry for ROLE_INFO re-emit.  Consumers of the CONTENT use
/// `inbox_schema()` — never re-parse this string.
[[nodiscard]] std::string inbox_schema_json() const
{
    return detail::read_string_or_empty(body_, "inbox_schema_json");
}
[[nodiscard]] bool has_inbox_schema() const noexcept
{
    return inbox_schema_.has_schema;
}
/// The once-parsed inbox schema (valid iff `has_inbox_schema()`).
[[nodiscard]] const ::pylabhub::hub::SchemaSpec &inbox_schema() const noexcept
{
    return inbox_schema_;
}
[[nodiscard]] std::string inbox_checksum() const
{
    return detail::read_string_or_empty(body_, "inbox_checksum");
}

private:
::pylabhub::hub::SchemaSpec inbox_schema_{};

public:
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
    return detail::read_string_or_empty(body_, "role_name");
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
    return detail::read_string_or_empty(body_, "expected_flexzone_packing");
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
// (`consumer_queue_type` is NOT exposed: "Forbidden / removed" per
// HEP-CORE-0036 §5b.6 — subsumed by the REQUIRED `data_transport` above,
// which the broker arbitrates against the channel's stored transport.)
//
// Expected schema owner for a named citation — OPTIONAL (consumer twin of the
// producer's `schema_owner`).  ⚠ Not yet in the HEP-0036 §5b.6 canonical
// catalog and named differently from HEP-0034's citation flow
// (`schema_owner`); production consumers do not send it.  Logged for the
// #72 HEP↔code reconciliation — do not extend its use until resolved.
[[nodiscard]] std::string expected_schema_owner() const
{
    return detail::read_string_or_empty(body_, "expected_schema_owner");
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
// Optional inbox companion fields per HEP-CORE-0027 §4.1.  The
// doubly-encoded `inbox_schema_json` (a string whose content is the
// HEP-0027 §6 canonical schema object) is parsed ONCE at construction
// via the canonical `hub::parse_schema_json`; `inbox_schema()` exposes
// the typed result and malformed content rejects as
// BODY_SCHEMA_VIOLATION at the wire boundary — no downstream reader
// ever re-parses the string.  Packing is carried once, INSIDE the
// schema object (HEP-CORE-0034 §6.2 requires it there); the separate
// `inbox_packing` wire field is retired.
[[nodiscard]] std::string inbox_endpoint() const
{
    return detail::read_string_or_empty(body_, "inbox_endpoint");
}
/// Raw advertisement string, stored verbatim on the ProducerEntry /
/// ConsumerEntry for ROLE_INFO re-emit.  Consumers of the CONTENT use
/// `inbox_schema()` — never re-parse this string.
[[nodiscard]] std::string inbox_schema_json() const
{
    return detail::read_string_or_empty(body_, "inbox_schema_json");
}
[[nodiscard]] bool has_inbox_schema() const noexcept
{
    return inbox_schema_.has_schema;
}
/// The once-parsed inbox schema (valid iff `has_inbox_schema()`).
[[nodiscard]] const ::pylabhub::hub::SchemaSpec &inbox_schema() const noexcept
{
    return inbox_schema_;
}
[[nodiscard]] std::string inbox_checksum() const
{
    return detail::read_string_or_empty(body_, "inbox_checksum");
}

private:
::pylabhub::hub::SchemaSpec inbox_schema_{};

public:
// Security triple per HEP-CORE-0046 §I-REPLAY-BOUND.
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

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
}
;

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
// Binding-side role of the applier (HEP-CORE-0042 §5.5.2): "producer" |
// "consumer".  REQUIRED — the broker discriminates its branch on this and
// checks registration by (channel, role_uid, role_type).
[[nodiscard]] std::string role_type() const
{
    return detail::read_string(body_, "role_type");
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
}
;

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
}
;

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
    return body_.at("initial_allowlist"); // array expected
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
    return detail::read_string_or_empty(body_, "broker_observer_pubkey_z85");
}
}
;

// **ConsumerRegAckBody** — CONSUMER_REG_ACK per HEP-CORE-0046 §14.3
// (erratum: split from RegAckBody 2026-07-15 task #45) + HEP-CORE-0036
// §5b/§6.4.  Consumer-side ACK does NOT carry `initial_allowlist`
// (that's producer-side per HEP-0046 §14.3); it carries `producers[]`
// and `data_transport` per §5b's unified transport-discriminator
// shape (B-4, #289, 2026-06-25).  Success ACK only — broker emits
// msg_type "ERROR" for the failure case, so ConsumerRegAckBody's
// required-field set covers the success path.
//
// Schema-at-establishment (HEP-CORE-0034 §10.3a): the channel's
// established schema rides the success ACK — the ACK is "your view of
// the book" (HEP-0046 C1), and the format is part of that view.  All
// five fields OPTIONAL: empty/absent means that axis is not
// established on the channel record (e.g. anonymous hash-only
// channels serve no structure by design — a runtime-resolved consumer
// aborts cleanly on the empty format).  NO packing fields: the
// fingerprint binds each zone's packing; the receiver recovers it by
// candidate recompute during pin verification (HEP-0034 §6.4).
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
    return body_.at("producers"); // array expected
}
[[nodiscard]] const nlohmann::json &broker_abi_fingerprint() const
{
    return detail::read_object(body_, "broker_abi_fingerprint");
}
[[nodiscard]] std::string broker_build_id() const
{
    return detail::read_string_or_empty(body_, "broker_build_id");
}
[[nodiscard]] std::string schema_id() const
{
    return detail::read_string_or_empty(body_, "schema_id");
}
[[nodiscard]] std::string schema_owner() const
{
    return detail::read_string_or_empty(body_, "schema_owner");
}
[[nodiscard]] std::string blds() const
{
    return detail::read_string_or_empty(body_, "blds");
}
[[nodiscard]] std::string flexzone_blds() const
{
    return detail::read_string_or_empty(body_, "flexzone_blds");
}
[[nodiscard]] std::string schema_hash() const
{
    return detail::read_string_or_empty(body_, "schema_hash");
}
}
;

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
}
;

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
}
;

PLH_WIRE_BODY_CLASS(GetChannelAuthAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
[[nodiscard]] const nlohmann::json &allowlist() const
{
    // Array of `{role_uid, pubkey_z85}` rows — the same shape
    // `RegAckBody::initial_allowlist` carries (HEP-CORE-0036 §6.5).  Was
    // an array of bare Z85 strings until 2026-08-10; that form named
    // nobody, so a receiver could enforce on the key and still not tell
    // its script which peer it belonged to.
    //
    // Returned as raw JSON: the ROW is not typed yet, which is the gap
    // HEP-CORE-0046 § "Adding a field that is a LIST of things" names.
    return body_.at("allowlist");
}
[[nodiscard]] std::uint64_t channel_version() const
{
    return detail::read_u64_or_zero(body_, "channel_version");
}
}
;

// **ChannelAuthAppliedAckBody** — the broker's success reply per
// HEP-CORE-0042 §5.5.2: `{status:"ok", channel_name, applied_version}`.
// The echoed `applied_version` VALUE is the broker's resulting
// confirmed version (post-clamp `max(current, W)`) — equal to the
// request's W except when the ledger absorbed a duplicate/regressing
// confirm.  (An earlier draft shape named a `confirmed_version` field;
// the broker never emitted it — reconciled 2026-07-24.)
PLH_WIRE_BODY_CLASS(ChannelAuthAppliedAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
[[nodiscard]] std::string channel_name() const
{
    return detail::read_string(body_, "channel_name");
}
[[nodiscard]] std::uint64_t applied_version() const
{
    return detail::read_u64_or_zero(body_, "applied_version");
}
}
;

PLH_WIRE_BODY_CLASS(DeregAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
}
;

PLH_WIRE_BODY_CLASS(DiscReqBody)
public:
[[nodiscard]] std::string channel_name() const
{
    return detail::read_string(body_, "channel_name");
}
}
;

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
}
;

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
}
;

// **RosterCheckNotifyBody** — "here is the roster version I hold"
// (HEP-CORE-0035 §4.9.7).  A role sends one per hub on the periodic tick
// it already runs; the hub answers with `ROSTER_UPDATE_NOTIFY` only when
// the version differs, so staying current costs nothing on the way back.
//
// `known_roles_version` is REQUIRED, not optional-defaulting-to-zero.  A
// role that holds no roster yet has a version — it is 0 — so a body
// without the field is a sender that did not know what it was reporting,
// and reading it as 0 would make the hub resend a roster to a role that
// may already be current.
PLH_WIRE_BODY_CLASS(RosterCheckNotifyBody)
public:
[[nodiscard]] std::string role_uid() const
{
    return detail::read_string(body_, "role_uid");
}
[[nodiscard]] std::uint64_t known_roles_version() const
{
    return detail::read_u64(body_, "known_roles_version");
}
}
;

// **ChannelBroadcastSendBody** — a member's broadcast to everyone on a
// data channel (HEP-CORE-0007 §"Broker Notifications", HEP-CORE-0030 §9.1).
//
// It carries no sender.  Recipients read the sender of the fan-out as fact,
// so it is decided from the key the connection proved at handshake and
// stamped by the broker — never announced by the sender.  A body still
// carrying the retired `sender_uid` is refused rather than re-attributed,
// so an old client learns its label was not honoured instead of watching
// its message go out under a name it did not choose.
PLH_WIRE_BODY_CLASS(ChannelBroadcastSendBody)
public:
[[nodiscard]] std::string target_channel() const
{
    return detail::read_string(body_, "target_channel");
}
[[nodiscard]] std::string message() const
{
    return detail::read_string_or_empty(body_, "message");
}
// Optional opaque application payload, forwarded byte-for-byte.
[[nodiscard]] std::string data() const
{
    return detail::read_string_or_empty(body_, "data");
}
}
;

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
}
;

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
}
;

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
}
;

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
    return detail::read_string_or_empty(body_, "role_name");
}
}
;

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
    return detail::read_string_or_empty(body_, "role_name");
}
}
;

// === Admin console family (HEP-CORE-0033 §11) ========================
//
// The typed operator-console protocol.  Admin messages are NOT part of
// the REG admission taxonomy (wire_dispatch); they are gated by the
// sealed session id (§11.0.5), not by the broker's admission pipeline.
// These are the first fully-typed pathway and the reference for the
// JSON→typed migration (task #57).  Security fields:
//   - COMMAND requests (Ping / CloseChannel / Session / Named /
//     BroadcastChannel / QueryMetrics) carry the full security TRIPLE —
//     `client_nonce` + `client_wall_ts` + `envelope_hash` — for
//     in-session replay defense (§11.0.5; deduped by the shared
//     `ReplayGuard` keyed on the session's origin_uid, HEP-CORE-0027
//     §3.6).
//   - HELLO and every ACK / ERROR body carry `envelope_hash` only
//     (stamped/validated by WireEnvelope), like the rest of the control
//     plane.

/// msg_type constants for the typed admin console (no central msg_type
/// registry exists; these named constants keep admin off the string-literal
/// scatter the broker plane still uses).
inline constexpr std::string_view kAdminHelloReq = "ADMIN_HELLO_REQ";
inline constexpr std::string_view kAdminHelloAck = "ADMIN_HELLO_ACK";
inline constexpr std::string_view kAdminPingReq = "ADMIN_PING_REQ";
inline constexpr std::string_view kAdminPingAck = "ADMIN_PING_ACK";
inline constexpr std::string_view kAdminCloseChannelReq = "ADMIN_CLOSE_CHANNEL_REQ";
inline constexpr std::string_view kAdminCloseChannelAck = "ADMIN_CLOSE_CHANNEL_ACK";
inline constexpr std::string_view kAdminBroadcastChannelReq = "ADMIN_BROADCAST_CHANNEL_REQ";
inline constexpr std::string_view kAdminBroadcastChannelAck = "ADMIN_BROADCAST_CHANNEL_ACK";
inline constexpr std::string_view kAdminRequestShutdownReq = "ADMIN_REQUEST_SHUTDOWN_REQ";
inline constexpr std::string_view kAdminRequestShutdownAck = "ADMIN_REQUEST_SHUTDOWN_ACK";
inline constexpr std::string_view kAdminListChannelsReq = "ADMIN_LIST_CHANNELS_REQ";
inline constexpr std::string_view kAdminListChannelsAck = "ADMIN_LIST_CHANNELS_ACK";
inline constexpr std::string_view kAdminListRolesReq = "ADMIN_LIST_ROLES_REQ";
inline constexpr std::string_view kAdminListRolesAck = "ADMIN_LIST_ROLES_ACK";
inline constexpr std::string_view kAdminListBandsReq = "ADMIN_LIST_BANDS_REQ";
inline constexpr std::string_view kAdminListBandsAck = "ADMIN_LIST_BANDS_ACK";
inline constexpr std::string_view kAdminListPeersReq = "ADMIN_LIST_PEERS_REQ";
inline constexpr std::string_view kAdminListPeersAck = "ADMIN_LIST_PEERS_ACK";
inline constexpr std::string_view kAdminGetChannelReq = "ADMIN_GET_CHANNEL_REQ";
inline constexpr std::string_view kAdminGetChannelAck = "ADMIN_GET_CHANNEL_ACK";
inline constexpr std::string_view kAdminGetRoleReq = "ADMIN_GET_ROLE_REQ";
inline constexpr std::string_view kAdminGetRoleAck = "ADMIN_GET_ROLE_ACK";
inline constexpr std::string_view kAdminQueryMetricsReq = "ADMIN_QUERY_METRICS_REQ";
inline constexpr std::string_view kAdminQueryMetricsAck = "ADMIN_QUERY_METRICS_ACK";
/// Console output poll (HEP-CORE-0033 §11.0.4).  Request reuses
/// `AdminSessionReqBody` (session id only); the ack reuses `AdminResultAckBody`
/// with `result = { status, lines[], dropped_count }`.  A read: session-id +
/// skew checked, NO replay nonce (§11.1).
inline constexpr std::string_view kAdminResponseQueryReq = "ADMIN_RESPONSE_QUERY_REQ";
inline constexpr std::string_view kAdminResponseQueryAck = "ADMIN_RESPONSE_QUERY_ACK";
/// Typed error reply for any admin failure (unauthorized, invalid session,
/// bad params, not-found).  Carries the §11.5 error code + message.
inline constexpr std::string_view kAdminError = "ADMIN_ERROR";

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
}
;

/// ADMIN_HELLO_ACK — carries the sealed, connection-bound session id the
/// operator presents on every later command (§11.0.5).
PLH_WIRE_BODY_CLASS(AdminHelloAckBody)
public:
[[nodiscard]] std::string session_id() const
{
    return detail::read_string(body_, "session_id");
}
}
;

/// ADMIN_PING_REQ — liveness / round-trip proof.  Carries only the session
/// id (verified against the connection facts before dispatch).
PLH_WIRE_BODY_CLASS(AdminPingReqBody)
public:
[[nodiscard]] std::string session_id() const
{
    return detail::read_string(body_, "session_id");
}
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

/// ADMIN_PING_ACK — `status` = "ok" (JsonKind has no bool; every ACK uses a
/// status string, matching the broker ACK bodies).
PLH_WIRE_BODY_CLASS(AdminPingAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
}
;

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
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

/// ADMIN_CLOSE_CHANNEL_ACK — `status` = "ok" (accepted / enqueued).
PLH_WIRE_BODY_CLASS(AdminCloseChannelAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
}
;

/// Session-only request — carries just the session id.  Shared by the
/// list queries (list_channels/roles/bands/peers) and request_shutdown.
PLH_WIRE_BODY_CLASS(AdminSessionReqBody)
public:
[[nodiscard]] std::string session_id() const
{
    return detail::read_string(body_, "session_id");
}
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

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
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

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
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

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
/// In-session replay pair (§11.0.5) — required by the ctor's
/// security triple; exposed so the admin gate reads via typed
/// accessors, never `body.value(...)` (HEP-0046 §14.3).
[[nodiscard]] std::string client_nonce() const
{
    return detail::read_string(body_, "client_nonce");
}
[[nodiscard]] std::uint64_t client_wall_ts() const
{
    return detail::read_u64(body_, "client_wall_ts");
}
}
;

/// Query result ACK — carries the dynamic snapshot as an object (the
/// query result has no fixed schema; the object IS the typed field, like
/// `RegAck.heartbeat`).  Shared by every query ACK.
PLH_WIRE_BODY_CLASS(AdminResultAckBody)
public:
[[nodiscard]] const nlohmann::json &result() const
{
    return detail::read_object(body_, "result");
}
}
;

/// Control ACK — `status` = "ok" (accepted).  Shared by broadcast /
/// request_shutdown (close has its own for symmetry with the REQ).
PLH_WIRE_BODY_CLASS(AdminStatusAckBody)
public:
[[nodiscard]] std::string status() const
{
    return detail::read_string(body_, "status");
}
}
;

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
}
;

#undef PLH_WIRE_BODY_CLASS

// ============================================================================
// Peer lists — one channel from a source of truth to the wire
// ============================================================================
//
// Several REG-family messages carry "the peers on this channel":
// `REG_ACK.initial_allowlist`, `CONSUMER_REG_ACK.producers[]`,
// `GET_CHANNEL_AUTH_ACK.allowlist`.  They differ only in WHICH peers and
// whether the reader will dial them — never in what a peer looks like.
//
// They used to be built by a hand-written loop each, and the loops
// disagreed: some named the peer, some emitted a bare key, one emitted a
// bare string.  A role could then see a peer it could not name, and
// `allowed_peer_contains(channel, uid)` answered false for an admitted
// peer (HEP-CORE-0036 §6.2, "Why a peer entry always names its peer").
//
// So the list is not assembled at the call sites any more.  A caller
// declares the source and the detail; this channel owns everything else —
// resolving names, the row shape, ordering, and the diagnostic when a key
// cannot be named.  There is no way to reach the wire around it.

/// How much of a peer a message needs.
///
/// A dial target needs somewhere to dial.  An allow-entry does not, and
/// putting one there would place a transport detail on a list that is
/// purely about identity.  This is the ONLY axis on which these lists
/// legitimately differ, so it is an argument rather than a second loop.
enum class PeerDetail
{
    IdentityOnly, ///< `{role_uid, pubkey_z85}` — binding-side allow entries.
    WithEndpoint, ///< adds `endpoint` — the reader is going to dial this peer.
};

/// One peer as every REG-family message carries it: the name AND the key.
///
/// A receiver that gets only keys cannot name a sender to its script,
/// cannot keep per-peer state, and cannot log who anything came from —
/// which is why both the replicated roster and the wire carry pairs
/// (HEP-CORE-0035 §4.9).
///
/// **Where that is enforced, precisely.**  The name is a required
/// positional parameter of `from_pair`, so no caller can silently omit
/// it — but an EMPTY name is still representable, and that is deliberate.
/// `PeerListBuilder::add_key` emits exactly such a row when a key in a
/// channel's admission ledger cannot be named, together with an
/// `AdmittedKeyHasNoName` ERROR: dropping the row would silently
/// un-admit a peer the ledger holds, so the row goes out and `parse`
/// below refuses the whole message, leaving the receiver on its previous
/// set.  Fail loud, not fail quiet.
///
/// So the guarantee is on the READ side: `parse` is where a nameless row
/// is rejected.  `from_pair` deliberately does not validate — the one
/// caller that produces an invalid row is doing so on purpose.
/// (An earlier version of this comment claimed a nameless row was
/// "unconstructible".  It is not, and the one site that constructs one
/// is the reason the claim could not have been true.)
class PYLABHUB_UTILS_EXPORT PeerRow
{
  public:
    [[nodiscard]] static PeerRow from_pair(std::string role_uid, std::string pubkey_z85,
                                           std::string endpoint = {});

    /// Read one row.  `detail` is what the READER needs — the same axis
    /// `to_json` uses on the writing side, so the two ends are held to
    /// one description of the shape instead of two.
    ///
    /// `nullopt` when the row is not an object, does not name its peer,
    /// does not carry a well-formed key, or omits the endpoint a reader
    /// that is going to dial has no way to proceed without.  Callers
    /// reject the whole message on that rather than skipping the row:
    /// these lists REPLACE the reader's previous set, so a dropped row
    /// is a peer silently denied.
    [[nodiscard]] static std::optional<PeerRow> parse(const nlohmann::json &entry,
                                                      PeerDetail detail);

    [[nodiscard]] nlohmann::json to_json(PeerDetail detail) const;

    [[nodiscard]] const std::string &role_uid() const noexcept { return role_uid_; }
    [[nodiscard]] const std::string &pubkey_z85() const noexcept { return pubkey_z85_; }
    [[nodiscard]] const std::string &endpoint() const noexcept { return endpoint_; }

  private:
    PeerRow() = default;
    std::string role_uid_;
    std::string pubkey_z85_;
    std::string endpoint_;
};

/// Read a whole peer list, all-or-nothing.
///
/// `nullopt` means the list is unusable and the caller must keep whatever
/// it already had.  Partial acceptance is not offered on purpose: every
/// one of these lists replaces the reader's set, so "most of the peers"
/// is a quietly reduced allowlist, which reads as a working system that
/// denies someone.
///
/// `detail` is required rather than defaulted so that every reader says
/// out loud whether it is about to dial these peers.  A reader that will
/// dial and forgot to say so would accept a row it cannot use, and would
/// discover that only when the connect had nowhere to go.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<std::vector<PeerRow>>
parse_peer_list(const nlohmann::json &arr, PeerDetail detail);

} // namespace pylabhub::wire
