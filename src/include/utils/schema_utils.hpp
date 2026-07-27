#pragma once
/**
 * @file schema_utils.hpp
 * @brief Schema utilities — parse, resolve, convert, hash, compute size.
 *
 * Pure C++ utilities operating on schema types (FieldDef, SchemaSpec).
 * No pybind11 or language-specific dependencies.
 *
 * Public header — part of pylabhub-utils. Used by role hosts, engines,
 * role API, and tests.
 *
 * @see schema_types.hpp for type definitions (FieldDef, SchemaSpec, etc.)
 * @see schema_field_layout.hpp for layout computation (compute_field_layout)
 */

#include "utils/format_tools.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/schema_field_layout.hpp"
#include "utils/schema_loader.hpp"
#include "utils/schema_record.hpp"             // SchemaRecord (HEP-CORE-0034)
#include "utils/security/secure_subsystem.hpp" // secure().compute_blake2b_array

#include "utils/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::hub
{

// ── Schema JSON parsing ─────────────────────────────────────────────────────

inline SchemaSpec parse_schema_json(const nlohmann::json &schema_obj)
{
    SchemaSpec spec;
    spec.has_schema = true;

    if (schema_obj.contains("expose_as"))
        throw std::runtime_error("expose_as is no longer supported; use field-based schemas");

    // HEP-CORE-0034 §6.2 — `packing` MUST be declared explicitly.  It is
    // part of the schema fingerprint (§6.3); a missing packing field
    // would silently default to one mode and collide with the other.
    if (!schema_obj.contains("packing"))
        throw std::runtime_error("Schema: 'packing' field is required (HEP-CORE-0034 §6.2). "
                                 "Set \"packing\": \"aligned\" for natural C-struct layout or "
                                 "\"packing\": \"packed\" for no-padding layout.");
    spec.packing = schema_obj["packing"].get<std::string>();
    if (spec.packing != "aligned" && spec.packing != "packed")
        throw std::runtime_error("Schema: 'packing' must be 'aligned' or 'packed'");

    if (!schema_obj.contains("fields") || !schema_obj["fields"].is_array())
        throw std::runtime_error("Schema: ctypes mode requires a 'fields' array");

    for (const auto &f : schema_obj["fields"])
    {
        if (!f.contains("name") || !f["name"].is_string())
            throw std::runtime_error("Schema: each field must have a string 'name'");
        if (!f.contains("type") || !f["type"].is_string())
            throw std::runtime_error("Schema: field '" + f["name"].get<std::string>() +
                                     "' missing 'type'");

        FieldDef fd;
        fd.name = f["name"].get<std::string>();
        fd.type_str = f["type"].get<std::string>();
        fd.count = f.value("count", uint32_t{1});
        fd.length = f.value("length", uint32_t{0});

        static constexpr std::array<std::string_view, 13> kValidTypes = {
            "bool",   "int8",   "int16",   "int32",   "int64",  "uint8", "uint16",
            "uint32", "uint64", "float32", "float64", "string", "bytes"};
        if (!std::any_of(kValidTypes.begin(), kValidTypes.end(),
                         [&fd](std::string_view t) { return t == fd.type_str; }))
            throw std::runtime_error("Schema: field '" + fd.name + "' has unknown type '" +
                                     fd.type_str + "'");

        if (fd.count == 0)
            throw std::runtime_error("Schema: field '" + fd.name +
                                     "' has 'count' = 0 (must be >= 1)");

        if ((fd.type_str == "string" || fd.type_str == "bytes") && fd.length == 0)
            throw std::runtime_error("Schema: field '" + fd.name + "' of type '" + fd.type_str +
                                     "' requires 'length' > 0");

        spec.fields.push_back(std::move(fd));
    }

    if (spec.fields.empty())
        throw std::runtime_error("Schema: 'fields' array must not be empty");

    return spec;
}

// ── Named schema resolution (HEP-CORE-0016 Phase 5) ─────────────────────────

inline SchemaSpec schema_entry_to_spec(const schema::SchemaLayoutDef &layout)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.packing = layout.packing; // HEP-CORE-0034 — propagate per-section packing

    for (const auto &sf : layout.fields)
    {
        FieldDef fd;
        fd.name = sf.name;
        if (sf.type == "char")
        {
            fd.type_str = "string";
            fd.length = sf.count;
            fd.count = 1;
        }
        else
        {
            fd.type_str = sf.type;
            fd.count = sf.count;
            fd.length = 0;
        }
        spec.fields.push_back(std::move(fd));
    }
    return spec;
}

/// Resolve a `schema_id` against the role-side local schema cache
/// (`<role_dir>/schemas/` + the platform default search dirs) by parsing
/// every file in the search path and returning the matching entry's
/// SchemaSpec.  Used by config-driven role startup to populate queue
/// options (slot / flexzone field lists) when the role's config refers
/// to a schema by name.
///
/// **Authority note (HEP-CORE-0034 §2.4 I8):** the role-side cache is
/// **not authoritative**.  This call resolves locally so the role can
/// open its queues; the broker independently validates the role's
/// schema fingerprint on REG_REQ (Stage-2).  If the local cache
/// disagrees with the hub, REG_REQ NACKs — there is no role-local
/// "truth" to defend.
///
/// **No state held:** this function performs a fresh parse on each
/// call (HEP-CORE-0034 §2.4 I5).  Callers that need the result more
/// than once should hold onto the returned SchemaSpec, not re-resolve.
inline SchemaSpec resolve_named_schema(const std::string &schema_id, bool use_flexzone,
                                       const char *label,
                                       const std::vector<std::string> &extra_search_dirs = {})
{
    auto search_dirs = extra_search_dirs;
    const auto defaults = schema::SchemaLibrary::default_search_dirs();
    search_dirs.insert(search_dirs.end(), defaults.begin(), defaults.end());

    // Walk the search path and look for the matching id.  First-match-wins
    // across directories is enforced inside `load_all_from_dirs`.
    const auto entries = schema::load_all_from_dirs(search_dirs);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const auto &p) { return p.second.schema_id == schema_id; });
    if (it == entries.end())
        throw std::runtime_error(std::string("[") + label + "] Named schema '" + schema_id +
                                 "' not found in role-side schema cache (search dirs: " +
                                 std::to_string(search_dirs.size()) + ")");

    const auto &entry = it->second;
    const auto &layout = use_flexzone ? entry.flexzone : entry.slot;
    if (layout.fields.empty())
        throw std::runtime_error(std::string("[") + label + "] Named schema '" + schema_id +
                                 "' has no " + (use_flexzone ? "flexzone" : "slot") + " fields");
    return schema_entry_to_spec(layout);
}

inline SchemaSpec resolve_schema(const nlohmann::json &schema_json, bool use_flexzone,
                                 const char *label,
                                 const std::vector<std::string> &extra_search_dirs = {})
{
    if (schema_json.is_null())
        return {};
    if (schema_json.is_string())
    {
        // HEP-CORE-0034 §10.3a — the explicit runtime-resolved
        // sentinel.  Only the DELIBERATE sentinel produces a
        // runtime_resolved spec; a null/absent axis stays plain
        // has_schema=false (queue builders refuse it — no silent
        // fallback into schema-pending).
        if (schema_json.get<std::string>() == kSchemaFromChannel)
        {
            SchemaSpec s;
            s.runtime_resolved = true;
            return s;
        }
        return resolve_named_schema(schema_json.get<std::string>(), use_flexzone, label,
                                    extra_search_dirs);
    }
    return parse_schema_json(schema_json);
}

// ── Schema hash — two jobs, do NOT conflate (HEP-CORE-0034 §6.3) ─────────────
//
// "Schema hash" covers two distinct mechanisms in different layers:
//
//   Job 1 — data-plane per-message `schema_tag`.  The folded whole-protocol
//     hash `compute_schema_hash(slot_spec, fz_spec)` below; its first 8 bytes
//     are stamped on every ZMQ data message as a drift tripwire.  Stays
//     FOLDED — a message is valid only if BOTH zones agree.
//
//   Job 2 — control-plane registry / citation fingerprint.  A 64-byte value
//     `datablock_half ‖ flexzone_half`, each half
//     `BLAKE2b-256(zone_blds || "|pack:" || zone_packing)` over ONLY that
//     zone's own fields.  An absent zone's half is all-zero; the full 64
//     bytes are never all-zero.  Stored in `SchemaRecord`, carried on
//     REG_REQ as `schema_hash` (128 hex), matched at registration/join,
//     returned by SCHEMA_ACK.  Built by `compute_fingerprint_from_wire()` /
//     `compute_zone_hash()` below.
//
// Wire-format invariant (HEP-0034 §10.1):
//
//   `schema_blds` is precisely the slot's `canonical_fields` string (no
//   "slot:" prefix, no "|pack:" suffix); `flexzone_blds` is the same for
//   the flexzone.  Packing rides in separate `schema_packing` /
//   `flexzone_packing` fields.  The broker recomputes each half from these
//   and compares to the producer's claimed 128-hex `schema_hash`.

/// Build the slot's `canonical_fields` portion of the canonical bytes
/// (HEP-CORE-0034 §6.3 / §10.1).  Public so producer-side code can
/// populate the wire `schema_blds` field with the exact bytes the
/// broker will hash.
inline std::string canonical_fields_str(const SchemaSpec &spec)
{
    std::string out;
    bool first = true;
    for (const auto &f : spec.fields)
    {
        if (!first)
            out += '|';
        first = false;
        out += f.name;
        out += ':';
        out += f.type_str;
        out += ':';
        out += std::to_string(f.count);
        out += ':';
        out += std::to_string(f.length);
    }
    return out;
}

inline void append_schema_canonical(std::string &out, const std::string &prefix,
                                    const SchemaSpec &spec)
{
    out += prefix;
    out += canonical_fields_str(spec);
}

inline std::string compute_schema_hash(const SchemaSpec &slot_spec, const SchemaSpec &fz_spec)
{
    if (!slot_spec.has_schema && !fz_spec.has_schema)
        return {};

    std::string canonical;
    canonical.reserve(256);
    if (slot_spec.has_schema)
    {
        append_schema_canonical(canonical, "slot:", slot_spec);
        canonical += "|pack:";
        canonical += slot_spec.packing;
    }
    if (fz_spec.has_schema)
    {
        append_schema_canonical(canonical, slot_spec.has_schema ? "|fz:" : "fz:", fz_spec);
        canonical += slot_spec.has_schema ? "|fzpack:" : "|pack:";
        canonical += fz_spec.packing;
    }

    const auto hash = pylabhub::utils::security::secure().compute_blake2b_array(canonical.data(),
                                                                                canonical.size());
    return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

/// Per-zone hash (Job 2): `BLAKE2b-256(zone_blds || "|pack:" || zone_packing)`
/// over ONLY that zone's own fields, with NO `slot:`/`fz:` prefix — so the
/// same field layout hashes identically whether it lives in the datablock or
/// the flexzone (zone-agnostic per half).  An absent zone (empty `blds`)
/// hashes to all-zero, which is the "absent" sentinel used in the 64-byte
/// fingerprint.  A real zone hashing to all-zero is a 1-in-2^256
/// impossibility, so zero is unambiguous.
inline std::array<uint8_t, 32> compute_zone_hash(const std::string &blds,
                                                 const std::string &packing)
{
    if (blds.empty())
        return {}; // absent zone → all-zero half
    std::string canonical;
    canonical.reserve(blds.size() + packing.size() + 8);
    canonical += blds;
    canonical += "|pack:";
    canonical += packing;
    return pylabhub::utils::security::secure().compute_blake2b_array(canonical.data(),
                                                                     canonical.size());
}

/// Recompute the 64-byte Job-2 registry/citation fingerprint
/// `datablock_half ‖ flexzone_half` from the already-canonical wire fields
/// (`schema_blds`/`schema_packing` + `flexzone_blds`/`flexzone_packing`).
/// Each half is `compute_zone_hash`; an absent zone contributes 32 zero
/// bytes.  This is the verification path the broker uses to check the
/// producer's claimed 128-hex `schema_hash`, and the source of the value
/// stored in `SchemaRecord`.
inline std::array<uint8_t, 64> compute_fingerprint_from_wire(const std::string &slot_blds,
                                                             const std::string &slot_packing,
                                                             const std::string &fz_blds = {},
                                                             const std::string &fz_packing = {})
{
    std::array<uint8_t, 64> fp{};
    const auto db_half = compute_zone_hash(slot_blds, slot_packing);
    const auto fz_half = compute_zone_hash(fz_blds, fz_packing);
    std::copy(db_half.begin(), db_half.end(), fp.begin());
    std::copy(fz_half.begin(), fz_half.end(), fp.begin() + 32);
    return fp;
}

/// True iff every byte of a fingerprint is zero — i.e. neither zone present.
/// `make_schema_record` rejects this; a wire claim of all-zero is invalid.
inline bool fingerprint_is_all_zero(const std::array<uint8_t, 64> &fp) noexcept
{
    return std::all_of(fp.begin(), fp.end(), [](uint8_t b) { return b == 0; });
}

// ── Fingerprint ↔ wire hex — THE shared conversion pair ─────────────────────
//
// A fingerprint travels the wire and lives in config as 128 lowercase hex
// characters; it is computed and compared as 64 raw bytes.  These
// functions are the ONLY sanctioned conversion between those two forms
// (HEP-CORE-0034 §2.4 I10).  Hand-rolling either direction at a call
// site is a review-flaggable violation: the decode direction needs a
// length + character guard that is easy to get subtly wrong, and two
// independent guards drift apart.
//
// Both sides of the wire share this pair — the broker when it reads a
// claimed hash or answers a query, the role when it verifies a format
// the hub delivered.  One implementation means a malformed value is
// rejected identically everywhere.

/// Decode a 128-hex fingerprint into its 64 bytes, or refuse.
/// `std::nullopt` means "not a valid fingerprint" — wrong length or a
/// non-hex character.  Callers MUST handle the empty case; there is no
/// silent zero-filled fallback, because an all-zero fingerprint has its
/// own meaning ("no zone present" — see `fingerprint_is_all_zero`).
[[nodiscard]] inline std::optional<std::array<uint8_t, 64>>
fingerprint_from_hex(std::string_view hex) noexcept
{
    return ::pylabhub::format_tools::bytes_from_hex_array<64>(hex);
}

/// Encode a 64-byte fingerprint as its 128-hex wire form.
[[nodiscard]] inline std::string fingerprint_hex(const std::array<uint8_t, 64> &fp)
{
    return ::pylabhub::format_tools::bytes_to_hex(fp);
}

/// Compute a fingerprint straight to its wire hex form — the
/// composition every wire-emitting site needs (compute, then encode).
[[nodiscard]] inline std::string fingerprint_hex_from_wire(const std::string &slot_blds,
                                                           const std::string &slot_packing,
                                                           const std::string &fz_blds = {},
                                                           const std::string &fz_packing = {})
{
    return fingerprint_hex(
        compute_fingerprint_from_wire(slot_blds, slot_packing, fz_blds, fz_packing));
}

/// Inverse of `canonical_fields_str` (HEP-CORE-0034 §6.3): parse a wire
/// canonical BLDS string (`name:type:count:length|…`) back into a
/// `SchemaSpec`.  Used by receivers of a runtime-resolved format
/// (HEP-0034 §10.3a — the CONSUMER_REG_ACK delivery) to materialize the
/// structure the queue and script tiers consume.
///
/// Strict grammar: each `|`-separated entry must be exactly four
/// `:`-separated tokens with non-empty name/type and numeric
/// count/length — anything else returns nullopt (no partial results).
/// Lossless round-trip: `canonical_fields_str(*parse) == input` for
/// every accepted input, so a fingerprint recomputed over the parsed
/// spec equals one computed over the original string.
///
/// `packing` is NOT part of the BLDS (it rides separate wire fields at
/// registration and is otherwise recovered from the fingerprint — §6.4);
/// the returned spec's `packing` is empty and the caller sets it after
/// recovery.
inline std::optional<SchemaSpec> parse_canonical_fields_str(const std::string &blds)
{
    if (blds.empty())
        return std::nullopt;
    SchemaSpec spec;
    spec.has_schema = true;
    spec.packing.clear(); // NOT in the BLDS — caller sets after §6.4 recovery
    size_t pos = 0;
    while (pos <= blds.size())
    {
        const size_t bar = blds.find('|', pos);
        const std::string entry =
            blds.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        // Exactly four tokens: name:type:count:length.
        std::array<std::string, 4> tok;
        size_t tpos = 0;
        for (int i = 0; i < 4; ++i)
        {
            const size_t colon = entry.find(':', tpos);
            if (i < 3)
            {
                if (colon == std::string::npos)
                    return std::nullopt; // too few tokens
                tok[static_cast<size_t>(i)] = entry.substr(tpos, colon - tpos);
                tpos = colon + 1;
            }
            else
            {
                if (colon != std::string::npos)
                    return std::nullopt; // too many tokens
                tok[3] = entry.substr(tpos);
            }
        }
        if (tok[0].empty() || tok[1].empty() || tok[2].empty() || tok[3].empty())
            return std::nullopt;
        FieldDef f;
        f.name = tok[0];
        f.type_str = tok[1];
        try
        {
            size_t consumed = 0;
            const unsigned long c = std::stoul(tok[2], &consumed);
            if (consumed != tok[2].size())
                return std::nullopt;
            const unsigned long l = std::stoul(tok[3], &consumed);
            if (consumed != tok[3].size())
                return std::nullopt;
            f.count = static_cast<uint32_t>(c);
            f.length = static_cast<uint32_t>(l);
        }
        catch (const std::exception &)
        {
            return std::nullopt; // non-numeric count/length
        }
        spec.fields.push_back(std::move(f));
        if (bar == std::string::npos)
            break;
        pos = bar + 1;
        if (pos == blds.size())
            return std::nullopt; // trailing '|'
    }
    return spec;
}

/// HEP-CORE-0034 §6.4 — two-candidate packing recovery.  Packing is
/// never stored on the channel record and never delivered (design
/// ruling 2026-07-26): a receiver holding a zone's BLDS and that zone's
/// 32-byte fingerprint half recovers the packing by recomputing over
/// the closed candidate domain {"aligned","packed"} and matching.
///
/// This doubles as the verification step: a successful recovery
/// PROVES the delivered BLDS hashes to the delivered/pinned half in the
/// same act.  nullopt means NO candidate matches — the (blds, half)
/// pair is inconsistent and the caller must abort naming the pair.
/// Absent zones are the caller's business (an empty `blds` returns
/// nullopt; verify absent zones against the all-zero half directly).
inline std::optional<std::string> recover_zone_packing(const std::string &blds,
                                                       const std::array<uint8_t, 32> &zone_half)
{
    if (blds.empty())
        return std::nullopt;
    for (const char *cand : {"aligned", "packed"})
    {
        if (compute_zone_hash(blds, cand) == zone_half)
            return std::string{cand};
    }
    return std::nullopt;
}

/// Result of `verify_request_fingerprint` (HEP-CORE-0034 §9, Job A).
struct RequestFingerprint
{
    std::array<uint8_t, 64> hash{}; ///< recomputed 64-byte `db‖fz` fingerprint
    bool consistent{true};          ///< false iff a non-empty claimed
                                    ///< hash disagrees with `hash`
};

/// Request self-consistency pre-check (HEP-CORE-0034 §9 step 1 / §2.4 I4).
///
/// Recomputes the 64-byte `db‖fz` fingerprint from the caller's supplied
/// wire structure and, if the caller ALSO gave a claimed 128-hex hash,
/// verifies the two agree.  This is the ONE place a registration handler
/// recomputes the fingerprint — the channel/registry validator
/// (`_validate_schema_citation`) only ever compares already-computed
/// fingerprints, never recomputes.
///
/// Required-field presence (which fields a given wire mode demands) is
/// mode-specific and stays at the call site.  `claimed_hash_hex` empty means
/// "no claim — just compute".  A `consistent == false` result maps to the
/// wire code `FINGERPRINT_INCONSISTENT` at every caller.
inline RequestFingerprint verify_request_fingerprint(const std::string &slot_blds,
                                                     const std::string &slot_packing,
                                                     const std::string &fz_blds,
                                                     const std::string &fz_packing,
                                                     const std::string &claimed_hash_hex)
{
    RequestFingerprint out;
    out.hash = compute_fingerprint_from_wire(slot_blds, slot_packing, fz_blds, fz_packing);
    if (!claimed_hash_hex.empty())
    {
        const std::string claimed = ::pylabhub::format_tools::bytes_from_hex(claimed_hash_hex);
        out.consistent = claimed.size() == out.hash.size() &&
                         std::equal(claimed.begin(), claimed.end(),
                                    reinterpret_cast<const char *>(out.hash.data()));
    }
    return out;
}

/// THE single builder for a two-zone `SchemaRecord` (HEP-CORE-0034 §4.1).
/// Fills both zones' content un-merged and the 64-byte `db‖fz` fingerprint.
/// An empty `db_blds` (resp. `fz_blds`) means that zone is absent (its half
/// is all-zero).  Asserts the record carries at least one zone — a record
/// with neither is meaningless.  Both `to_hub_schema_record` and the broker
/// path-B builder route through this.
inline ::pylabhub::schema::SchemaRecord
make_schema_record(const std::string &owner, const std::string &id, const std::string &db_blds,
                   const std::string &db_packing, const std::string &fz_blds = {},
                   const std::string &fz_packing = {})
{
    ::pylabhub::schema::SchemaRecord rec;
    rec.owner_uid = owner;
    rec.schema_id = id;
    rec.blds = db_blds;
    rec.packing = db_packing;
    rec.flexzone_blds = fz_blds;
    rec.flexzone_packing = fz_packing;
    rec.hash = compute_fingerprint_from_wire(db_blds, db_packing, fz_blds, fz_packing);
    assert(!fingerprint_is_all_zero(rec.hash) &&
           "make_schema_record: a SchemaRecord must carry at least one zone");
    return rec;
}

/// THE single per-record comparison (HEP-CORE-0034 §9): two records describe
/// the same protocol iff their 64-byte fingerprints are equal (packing folds
/// into each half, so a packing difference changes the fingerprint).  Used by
/// registration idempotency and citation fingerprint-match.
inline bool schema_records_equivalent(const ::pylabhub::schema::SchemaRecord &a,
                                      const ::pylabhub::schema::SchemaRecord &b) noexcept
{
    return a.hash == b.hash;
}

// ── HEP-CORE-0034 §12 hub-global record helpers ─────────────────────────────
//
// `to_hub_schema_record(SchemaEntry)` is the bridge between the file-loader
// (`SchemaLibrary`, which produces `SchemaEntry` from a JSON file) and the
// owner-keyed registry (`HubState.schemas`, keyed by `(owner_uid, schema_id)`).
//
// Used by hub startup (Phase 4b — deferred until plh_hub binary lands) to
// register globals via `_on_schema_registered({owner: "hub", id, hash, ...})`.
// The hash is the WIRE-FORM two-zone fingerprint
// (`make_schema_record` → `compute_fingerprint_from_wire`), NOT
// `SchemaEntry::slot_info.hash` — those use different canonical forms (the
// SHM-header hash includes only the slot's BLDS, while wire/registry hashes
// use canonical_fields_str).  Consumer citation against `(hub, id)` recomputes
// the wire fingerprint, so the stored record must use the same form.

inline ::pylabhub::schema::SchemaRecord
to_hub_schema_record(const ::pylabhub::schema::SchemaEntry &entry)
{
    // SchemaEntry::slot is `SchemaLayoutDef` (uses "char" for char arrays);
    // schema_entry_to_spec maps "char[N]" → string{length=N}.  This gives
    // us a SchemaSpec whose canonical_fields_str() exactly matches what
    // a wire client would build via the same conversion.  Both zones are
    // carried un-merged; the flexzone (if declared) is its own half of the
    // 64-byte fingerprint.
    auto slot_spec = schema_entry_to_spec(entry.slot);
    const bool has_fz = entry.has_flexzone();
    auto fz_spec = has_fz ? schema_entry_to_spec(entry.flexzone) : SchemaSpec{};
    return make_schema_record("hub", entry.schema_id, canonical_fields_str(slot_spec),
                              entry.slot.packing,
                              has_fz ? canonical_fields_str(fz_spec) : std::string{},
                              has_fz ? entry.flexzone.packing : std::string{});
}

// ── HEP-CORE-0034 §10 wire fields — producer + consumer payload helpers ─────
//
// `WireSchemaFields` packages every schema field that REG_REQ /
// CONSUMER_REG_REQ / PROC_REG_REQ may carry on the wire, computed once
// from the role's resolved SchemaSpecs.  Two `apply_*` helpers paste
// the fields into a `nlohmann::json` payload using the producer-side
// (`schema_*`) or consumer-side (`expected_*`) key names.
//
// These exist because role hosts (producer, consumer, processor) all
// need to build the same payload from the same inputs; without the
// helpers, the four call sites (producer-out, processor-out,
// consumer-in, processor-in) would diverge under maintenance.

struct WireSchemaFields
{
    /// Empty for anonymous channels; otherwise the named schema id
    /// extracted from the JSON config (when the config used the
    /// `"<schema>": "$lab.x.v1"` string form).
    std::string schema_id;

    /// Selects between path B (self-register, default empty) and
    /// path C (adopt hub-global, set to the literal string `"hub"`).
    /// HEP-CORE-0034 §10.1.  Today only Phase 5d's typed C++ API
    /// populates this for path C; `make_wire_schema_fields` always
    /// leaves it empty (path B), so role hosts default to
    /// self-registration.  Tests may set it manually.
    std::string schema_owner;

    /// Hex-encoded 64-byte two-zone fingerprint `datablock_half ‖
    /// flexzone_half` per HEP-CORE-0034 §6.3 (128 hex chars).  An absent
    /// zone contributes 32 zero bytes.  Empty when the role has no schema.
    std::string schema_hash;

    /// `canonical_fields_str(slot_spec)`.  Empty when no slot schema.
    std::string schema_blds;

    /// Slot's packing string ("aligned" | "packed").  Empty when
    /// no slot schema.
    std::string schema_packing;

    /// `canonical_fields_str(fz_spec)`.  Empty when no flexzone.
    std::string flexzone_blds;

    /// Flexzone's packing.  Empty when no flexzone.
    std::string flexzone_packing;
};

/// Build the wire fields from the role's resolved schema state.
///
/// `slot_schema_json` is the original config JSON value (a string for
/// named schemas, an object for inline schemas, null for none).  We
/// pull the schema_id out of the string form; if the JSON is an
/// object, schema_id stays empty (anonymous channel).
///
/// `slot_spec` and `fz_spec` are the resolved `SchemaSpec`s the role
/// host has already built via `resolve_schema()`.  `has_schema=false`
/// on either is treated as "not present" (the corresponding wire
/// fields stay empty, which is the producer's signal that the role
/// has no slot/flexzone for this side).
inline WireSchemaFields make_wire_schema_fields(const nlohmann::json &slot_schema_json,
                                                const SchemaSpec &slot_spec,
                                                const SchemaSpec &fz_spec)
{
    WireSchemaFields w;
    // The "from-channel" sentinel (HEP-CORE-0034 §10.3a) is NOT a
    // citation: a runtime-resolved consumer joins citation-free and
    // receives the channel's format on the success ACK.  Every other
    // string form is a named schema id.
    if (slot_schema_json.is_string() && slot_schema_json.get<std::string>() != kSchemaFromChannel)
        w.schema_id = slot_schema_json.get<std::string>();
    if (slot_spec.has_schema)
    {
        w.schema_blds = canonical_fields_str(slot_spec);
        w.schema_packing = slot_spec.packing;
    }
    if (fz_spec.has_schema)
    {
        w.flexzone_blds = canonical_fields_str(fz_spec);
        w.flexzone_packing = fz_spec.packing;
    }
    if (slot_spec.has_schema || fz_spec.has_schema)
    {
        const auto h = compute_fingerprint_from_wire(w.schema_blds, w.schema_packing,
                                                     w.flexzone_blds, w.flexzone_packing);
        w.schema_hash = pylabhub::format_tools::bytes_to_hex(
            {reinterpret_cast<const char *>(h.data()), h.size()});
    }
    return w;
}

/// Paste the producer-side wire fields into a REG_REQ payload.
/// Empty fields in `w` are skipped — this preserves the
/// "no schema fields → no Stage-2 verification" backward-compat path
/// for roles that haven't declared a schema.
inline void apply_producer_schema_fields(nlohmann::json &reg_opts, const WireSchemaFields &w)
{
    if (!w.schema_id.empty())
        reg_opts["schema_id"] = w.schema_id;
    if (!w.schema_owner.empty())
        reg_opts["schema_owner"] = w.schema_owner;
    if (!w.schema_hash.empty())
        reg_opts["schema_hash"] = w.schema_hash;
    if (!w.schema_blds.empty())
        reg_opts["schema_blds"] = w.schema_blds;
    if (!w.schema_packing.empty())
        reg_opts["schema_packing"] = w.schema_packing;
    if (!w.flexzone_blds.empty())
        reg_opts["flexzone_blds"] = w.flexzone_blds;
    if (!w.flexzone_packing.empty())
        reg_opts["flexzone_packing"] = w.flexzone_packing;
}

/// Paste the consumer-side wire fields into a CONSUMER_REG_REQ payload.
/// Field name mapping (HEP-CORE-0034 §10.2 / Phase 4d alignment):
///   schema_id        → expected_schema_id
///   schema_hash      → expected_schema_hash
///   schema_blds      → expected_schema_blds
///   schema_packing   → expected_schema_packing
///   flexzone_blds    → expected_flexzone_blds
///   flexzone_packing → expected_flexzone_packing
///
/// Mode is determined by which fields the consumer's config produced:
///   - id present → broker treats as named-citation; structure
///     fields (when present) drive defense-in-depth verification.
///   - id absent + structure present → anonymous citation.
///   - all empty → no validation (legacy backward-compat).
inline void apply_consumer_schema_fields(nlohmann::json &reg_opts, const WireSchemaFields &w)
{
    // HEP-CORE-0034 §10.2 — every consumer-side wire field carries the
    // `expected_schema_*` / `expected_flexzone_*` prefix to mirror the
    // producer's `schema_*` / `flexzone_*` fields.
    if (!w.schema_id.empty())
        reg_opts["expected_schema_id"] = w.schema_id;
    if (!w.schema_hash.empty())
        reg_opts["expected_schema_hash"] = w.schema_hash;
    if (!w.schema_blds.empty())
        reg_opts["expected_schema_blds"] = w.schema_blds;
    if (!w.schema_packing.empty())
        reg_opts["expected_schema_packing"] = w.schema_packing;
    if (!w.flexzone_blds.empty())
        reg_opts["expected_flexzone_blds"] = w.flexzone_blds;
    if (!w.flexzone_packing.empty())
        reg_opts["expected_flexzone_packing"] = w.flexzone_packing;
}

// ── Schema size computation ──────────────────────────────────────────────────

/// Compute the C struct size for a schema spec + packing, without the engine.
/// Uses the shared field layout algorithm (schema_field_layout.hpp).
inline size_t compute_schema_size(const SchemaSpec &spec, const std::string &packing)
{
    if (!spec.has_schema || spec.fields.empty())
        return 0;
    auto [layout, size] = compute_field_layout(to_field_descs(spec.fields), packing);
    return size;
}

/// Physical page size for SHM flexzone alignment.
/// Configurable via cmake PYLABHUB_PHYSICAL_PAGE_SIZE (default 4096).
#ifndef PYLABHUB_PHYSICAL_PAGE_SIZE
#define PYLABHUB_PHYSICAL_PAGE_SIZE 4096
#endif

/// Round a byte size up to the physical page boundary.
/// Used for SHM flexzone allocation where the OS allocates in page-sized chunks.
inline size_t align_to_physical_page(size_t logical_size)
{
    constexpr size_t kPageSize = PYLABHUB_PHYSICAL_PAGE_SIZE;
    static_assert(kPageSize > 0 && (kPageSize & (kPageSize - 1)) == 0,
                  "PYLABHUB_PHYSICAL_PAGE_SIZE must be a power of two");
    if (logical_size == 0)
        return 0;
    return (logical_size + kPageSize - 1) & ~(kPageSize - 1);
}

// ── ZMQ/SHM schema field conversion ─────────────────────────────────────────

/// Convert a SchemaSpec to a SchemaFieldDesc list for queue creation.
inline std::vector<SchemaFieldDesc> schema_spec_to_zmq_fields(const SchemaSpec &spec)
{
    if (spec.fields.empty())
        throw std::runtime_error("fields must not be empty");
    return to_field_descs(spec.fields);
}

} // namespace pylabhub::hub
