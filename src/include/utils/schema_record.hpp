#pragma once
/**
 * @file schema_record.hpp
 * @brief Owner-authoritative schema record type (HEP-CORE-0034).
 *
 * `SchemaRecord` is the runtime representation of a single schema in the
 * hub's authoritative registry (`HubState.schemas`).  Records are keyed
 * `(owner_uid, schema_id)`:
 *   - `owner_uid == "hub"` for hub-globals loaded from
 *     `<hub_dir>/schemas/`.  Lifetime = hub process lifetime.
 *   - `owner_uid == <role uid>` for private records registered by a role's
 *     REG_REQ.  Lifetime = role process lifetime; cascade-evicted from
 *     `_set_role_disconnected` (HEP-CORE-0034 §7.2).
 *
 * The fingerprint stored here is `BLAKE2b-256(canonical_fields || packing)`
 * per HEP-CORE-0034 §6.3.  The same fingerprint is computed on the
 * SchemaSpec / SchemaInfo paths and on the inbox-tag path (Phase 1,
 * 2026-04-27).
 *
 * @see HEP-CORE-0034-Schema-Registry.md §4 (record model), §11 (HubState),
 *      §15 Phase 2 (this file lands as part of Phase 2 work).
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace pylabhub::schema
{

/**
 * @brief One schema record in the hub's registry.
 *
 * Keyed by `(owner_uid, schema_id)` in `HubState.schemas`.  Conflict
 * policy is namespace-by-owner (HEP-CORE-0034 §8): two roles may both
 * register `frame` and they live under separate keys.
 */
struct SchemaRecord
{
    /// Owner: literal "hub" for globals, or a role uid for private records.
    std::string             owner_uid;

    /// Schema identifier under the owner's namespace.  May be a namespaced
    /// id (`lab.sensors.temperature.raw@1`) for hub-globals, or a flat id
    /// (`frame`, `inbox`) for private records.
    std::string             schema_id;

    /// BLAKE2b-256 over canonical(BLDS, packing) — the wire fingerprint
    /// (HEP-CORE-0034 §6.3).  Equality of fingerprint ⇔ bytewise-equal layout.
    std::array<uint8_t, 32> hash;

    /// "aligned" or "packed".  Part of the fingerprint; not redundant with
    /// the BLDS string, which encodes only the field list.
    std::string             packing;

    /// Canonical BLDS text (`name:type[N];...`) — sufficient for ctypes
    /// reconstruction by remote citers (e.g. via SCHEMA_REQ in Phase 3).
    std::string             blds;

    /// When the record was inserted into `HubState.schemas` (set by the
    /// capability op, not by the caller).
    std::chrono::system_clock::time_point registered_at{
        std::chrono::system_clock::now()};
};

/// Outcome of `HubState::_on_schema_registered`.
///
/// All non-`kCreated` outcomes leave the existing record (if any) unchanged.
enum class SchemaRegOutcome : uint8_t
{
    /// New `(owner_uid, schema_id)` record inserted.  Caller may proceed.
    kCreated = 0,

    /// `(owner_uid, schema_id)` already existed with the same hash and
    /// packing; treated as a no-op success.  Counter NOT bumped.
    kIdempotent,

    /// `(owner_uid, schema_id)` exists with a different hash or packing.
    /// Caller's REG_REQ should NACK with `reason=hash_mismatch_self`
    /// (HEP-CORE-0034 §10.4).
    kHashMismatchSelf,

    /// Caller attempted to register under another owner's uid (e.g. a
    /// producer claiming `(hub, ...)` or another producer's namespace).
    /// Phase 2 enforces only the owner-uid-vs-self check; cross-namespace
    /// reservations into `hub` are validated by the broker dispatcher in
    /// Phase 3 with full role context.
    kForbiddenOwner,
};

/// Outcome of `HubState::_validate_schema_citation`.
///
/// Citation rules per HEP-CORE-0034 §9.1:
///   - Cited owner must be either `"hub"` or the channel's producer uid.
///   - Cited record must exist and its hash + packing must match the
///     citer's expected fingerprint.
///   - Cross-citation (cited owner is a third role) is rejected even when
///     the fingerprint matches — see §9.3 for rationale.
struct CitationOutcome
{
    enum class Reason : uint8_t
    {
        kOk = 0,

        /// Cited owner is neither "hub" nor the channel's producer uid.
        kCrossCitation,

        /// Cited owner uid is not registered as a producer in HubState
        /// (and is not the literal "hub").  Distinct from
        /// `kCrossCitation`: here the owner doesn't exist at all.
        kUnknownOwner,

        /// `(cited_owner, cited_id)` record does not exist.
        kUnknownSchema,

        /// Record exists but hash or packing differs from the citer's
        /// expected fingerprint.
        kFingerprintMismatch,
    };

    Reason      reason{Reason::kOk};
    std::string detail;  ///< Human-readable detail for logs / NACK reason.

    [[nodiscard]] bool ok() const noexcept { return reason == Reason::kOk; }
};

} // namespace pylabhub::schema
