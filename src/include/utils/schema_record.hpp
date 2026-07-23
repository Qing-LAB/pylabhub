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
 * The fingerprint stored here is the 64-byte two-zone value
 * `datablock_half ‖ flexzone_half`, each half
 * `BLAKE2b-256(zone_blds || "|pack:" || zone_packing)` per HEP-CORE-0034
 * §6.3.  An absent zone's half is all-zero; the full 64 bytes are never
 * all-zero (at least one zone is always present).  This is the
 * control-plane registry/citation fingerprint (Job 2); it is distinct
 * from the data-plane per-message `schema_tag` (Job 1), which stays the
 * folded 8-byte whole-protocol tag computed by `compute_schema_hash`.
 *
 * @see HEP-CORE-0034-Schema-Registry.md §4.1 (record model), §6.3
 *      (two-zone fingerprint), §11 (HubState registry).
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
    std::string owner_uid;

    /// Schema identifier under the owner's namespace.  May be a namespaced
    /// id (`lab.sensors.temperature.raw@1`) for hub-globals, or a flat id
    /// (`frame`) for private records.  The registry holds channel schemas
    /// only — inbox message layouts are NOT registry records (HEP-CORE-0034
    /// §11.4 / HEP-CORE-0027).
    std::string schema_id;

    /// Two-zone fingerprint `datablock_half ‖ flexzone_half` (HEP-CORE-0034
    /// §6.3).  Each half is BLAKE2b-256 over that zone's `blds || "|pack:" ||
    /// packing`; an absent zone's half is all-zero.  Equality of the full 64
    /// bytes ⇔ both zones' layouts match (absent matches absent).  Never
    /// all-zero — at least one zone is present (enforced by `make_schema_record`).
    std::array<uint8_t, 64> hash;

    /// Datablock (slot) packing: "aligned" or "packed".  Empty iff the
    /// datablock zone is absent.  Part of the datablock half's fingerprint.
    std::string packing;

    /// Datablock (slot) canonical BLDS text (`name:type:count:length` joined
    /// with `|`).  Empty iff the datablock zone is absent.  Sufficient for
    /// ctypes reconstruction by remote citers (e.g. via SCHEMA_REQ).
    std::string blds;

    /// Flexzone packing: "aligned" or "packed".  Empty iff the flexzone zone
    /// is absent.  Part of the flexzone half's fingerprint.
    std::string flexzone_packing;

    /// Flexzone canonical BLDS text.  Empty iff the flexzone zone is absent.
    std::string flexzone_blds;

    /// When the record was inserted into `HubState.schemas` (set by the
    /// capability op, not by the caller).
    std::chrono::system_clock::time_point registered_at{std::chrono::system_clock::now()};

    /// A zone is present iff its BLDS text is non-empty.
    [[nodiscard]] bool has_datablock() const noexcept { return !blds.empty(); }
    [[nodiscard]] bool has_flexzone() const noexcept { return !flexzone_blds.empty(); }
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

/// Outcome of `HubState::_validate_schema_citation` — the single validator
/// for the HEP-CORE-0034 §9 matching contract.
///
/// The channel is the source of truth (channel-first).  Every joiner is
/// matched EXACTLY against the channel's stored `(schema_id, schema_owner,
/// fingerprint)`:
///   - **Fingerprint** must always match (necessary, not sufficient — §9.3).
///   - **schema_id** must be exactly equal (empty matches only empty —
///     anonymous↔anonymous; a name matches only that same name).  A named
///     citation against an anonymous channel, and vice versa, both reject.
///   - **owner** is asserted only by a producer named citation and must equal
///     the channel's owner (consumers cite by id only and never claim owner).
///   - For an explicit registry citation (producer adopting a hub-global, Path
///     C), §9.1 additionally requires the cited owner be `"hub"` or a channel
///     producer, the record to exist, and its fingerprint to match.
struct CitationOutcome
{
    enum class Reason : uint8_t
    {
        kOk = 0,

        /// Cited owner is neither "hub" nor any registered producer of
        /// the channel (explicit-citation / Path-C only; §9.1).
        kCrossCitation,

        /// `(cited_owner, cited_id)` record does not exist.
        kUnknownSchema,

        /// Record exists but hash or packing differs from the citer's
        /// expected fingerprint.
        kFingerprintMismatch,

        /// Joiner's `schema_id` differs from the channel's stored
        /// `schema_id` (named channel-match; HEP-CORE-0034 §9 step 2).
        /// Maps to the wire code `SCHEMA_ID_MISMATCH`.
        kSchemaIdMismatch,

        /// Joiner's claimed `schema_owner` differs from the channel's
        /// stored `schema_owner` (named channel-match; HEP-CORE-0034 §9
        /// step 2).  Producer-only — consumers name no owner, so this is
        /// never returned for a consumer.  Maps to `SCHEMA_MISMATCH`.
        kSchemaOwnerMismatch,
    };

    Reason reason{Reason::kOk};
    std::string detail; ///< Human-readable detail for logs / NACK reason.

    [[nodiscard]] bool ok() const noexcept { return reason == Reason::kOk; }
};

} // namespace pylabhub::schema
