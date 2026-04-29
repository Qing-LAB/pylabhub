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

#include "utils/crypto_utils.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/schema_field_layout.hpp"
#include "utils/schema_loader.hpp"
#include "utils/schema_record.hpp"  // SchemaRecord (HEP-CORE-0034)

#include "utils/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
        throw std::runtime_error(
            "Schema: 'packing' field is required (HEP-CORE-0034 §6.2). "
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
            throw std::runtime_error(
                "Schema: field '" + f["name"].get<std::string>() + "' missing 'type'");

        FieldDef fd;
        fd.name     = f["name"].get<std::string>();
        fd.type_str = f["type"].get<std::string>();
        fd.count    = f.value("count",  uint32_t{1});
        fd.length   = f.value("length", uint32_t{0});

        static constexpr std::array<std::string_view, 13> kValidTypes = {
            "bool", "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float32", "float64", "string", "bytes"};
        if (!std::any_of(kValidTypes.begin(), kValidTypes.end(),
                [&fd](std::string_view t) { return t == fd.type_str; }))
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' has unknown type '" + fd.type_str + "'");

        if (fd.count == 0)
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' has 'count' = 0 (must be >= 1)");

        if ((fd.type_str == "string" || fd.type_str == "bytes") && fd.length == 0)
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' of type '" + fd.type_str +
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
    spec.packing    = layout.packing;  // HEP-CORE-0034 — propagate per-section packing

    for (const auto &sf : layout.fields)
    {
        FieldDef fd;
        fd.name = sf.name;
        if (sf.type == "char")
        {
            fd.type_str = "string";
            fd.length   = sf.count;
            fd.count    = 1;
        }
        else
        {
            fd.type_str = sf.type;
            fd.count    = sf.count;
            fd.length   = 0;
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
        throw std::runtime_error(
            std::string("[") + label + "] Named schema '" + schema_id +
            "' not found in role-side schema cache (search dirs: " +
            std::to_string(search_dirs.size()) + ")");

    const auto &entry  = it->second;
    const auto &layout = use_flexzone ? entry.flexzone : entry.slot;
    if (layout.fields.empty())
        throw std::runtime_error(
            std::string("[") + label + "] Named schema '" + schema_id +
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
        return resolve_named_schema(schema_json.get<std::string>(), use_flexzone, label,
                                    extra_search_dirs);
    return parse_schema_json(schema_json);
}

// ── Schema hash ──────────────────────────────────────────────────────────────
//
// Canonical form (HEP-CORE-0034 §6.3):
//
//   slot:<canonical_fields>|pack:<packing>
//   [|fz:<canonical_fields>|fzpack:<packing>]
//
// where canonical_fields = "name:type:count:length" joined with "|"
//
// The "|pack:..." suffix is appended for each present section so two
// layouts with identical fields and different packing produce different
// fingerprints (Phase 1 correction).
//
// Wire-format invariant (HEP-0034 §10.1):
//
//   The wire field `schema_blds` is precisely the `canonical_fields`
//   string for the slot (no "slot:" prefix, no "|pack:..." suffix).  The
//   wire field `flexzone_blds` is the same form for the flexzone.
//   Packing is sent as a separate `schema_packing` / `flexzone_packing`
//   field.  This split lets the broker recompute the full canonical
//   bytes deterministically and verify the producer's claimed hash —
//   `compute_canonical_hash_from_wire()` below is the verification path.

/// Build the slot's `canonical_fields` portion of the canonical bytes
/// (HEP-CORE-0034 §6.3 / §10.1).  Public so producer-side code can
/// populate the wire `schema_blds` field with the exact bytes the
/// broker will hash.
inline std::string canonical_fields_str(const SchemaSpec &spec)
{
    std::string out;
    bool        first = true;
    for (const auto &f : spec.fields)
    {
        if (!first) out += '|';
        first = false;
        out += f.name;    out += ':';
        out += f.type_str; out += ':';
        out += std::to_string(f.count);  out += ':';
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

    const auto hash = pylabhub::crypto::compute_blake2b_array(
        canonical.data(), canonical.size());
    return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

/// Reconstruct the canonical bytes from the wire fields and hash.
///
/// The producer sends `schema_blds` = `canonical_fields_str(slot_spec)`
/// and `schema_packing` (and optionally `flexzone_blds` /
/// `flexzone_packing`).  The broker calls this helper to recompute the
/// 32-byte fingerprint and compare against the producer's claimed
/// `schema_hash`.  Identical algorithm to `compute_schema_hash` above
/// — just operating on already-canonical wire strings instead of
/// SchemaSpec inputs.
inline std::array<uint8_t, 32>
compute_canonical_hash_from_wire(const std::string &slot_blds,
                                 const std::string &slot_packing,
                                 const std::string &fz_blds   = {},
                                 const std::string &fz_packing = {})
{
    std::string canonical;
    canonical.reserve(slot_blds.size() + fz_blds.size() + 64);
    if (!slot_blds.empty())
    {
        canonical += "slot:";
        canonical += slot_blds;
        canonical += "|pack:";
        canonical += slot_packing;
    }
    if (!fz_blds.empty())
    {
        canonical += slot_blds.empty() ? "fz:" : "|fz:";
        canonical += fz_blds;
        canonical += slot_blds.empty() ? "|pack:" : "|fzpack:";
        canonical += fz_packing;
    }
    return pylabhub::crypto::compute_blake2b_array(canonical.data(), canonical.size());
}

// ── HEP-CORE-0034 §12 hub-global record helpers ─────────────────────────────
//
// `to_hub_schema_record(SchemaEntry)` is the bridge between the file-loader
// (`SchemaLibrary`, which produces `SchemaEntry` from a JSON file) and the
// owner-keyed registry (`HubState.schemas`, keyed by `(owner_uid, schema_id)`).
//
// Used by hub startup (Phase 4b — deferred until plh_hub binary lands) to
// register globals via `_on_schema_registered({owner: "hub", id, hash, ...})`.
// The hash is the WIRE-FORM fingerprint
// (`compute_canonical_hash_from_wire`), NOT `SchemaEntry::slot_info.hash`
// — those use different canonical forms (the SHM-header hash includes only
// the slot's BLDS, while wire/registry hashes use canonical_fields_str).
// Consumer citation against `(hub, id)` recomputes the wire hash, so the
// stored record must use the same form.

inline ::pylabhub::schema::SchemaRecord
to_hub_schema_record(const ::pylabhub::schema::SchemaEntry &entry)
{
    ::pylabhub::schema::SchemaRecord rec;
    rec.owner_uid = "hub";
    rec.schema_id = entry.schema_id;

    // SchemaEntry::slot is `SchemaLayoutDef` (uses "char" for char arrays);
    // schema_entry_to_spec maps "char[N]" → string{length=N}.  This gives
    // us a SchemaSpec whose canonical_fields_str() exactly matches what
    // a wire client would build via the same conversion.
    auto slot_spec = schema_entry_to_spec(entry.slot);
    auto fz_spec   = entry.has_flexzone() ? schema_entry_to_spec(entry.flexzone)
                                           : SchemaSpec{};

    // Hub-globals are slot-canonical for citation purposes.  If a global
    // schema declares a flexzone, the canonical form folds it in too —
    // identical to the producer's path.
    rec.blds    = canonical_fields_str(slot_spec);
    rec.packing = entry.slot.packing;
    rec.hash    = compute_canonical_hash_from_wire(rec.blds,
                                                    rec.packing,
                                                    fz_spec.has_schema
                                                        ? canonical_fields_str(fz_spec)
                                                        : std::string{},
                                                    fz_spec.has_schema
                                                        ? entry.flexzone.packing
                                                        : std::string{});
    return rec;
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

    /// Hex-encoded BLAKE2b-256 fingerprint over canonical(slot, fz)
    /// per HEP-CORE-0034 §6.3.  Empty when the role has no schema.
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
inline WireSchemaFields
make_wire_schema_fields(const nlohmann::json &slot_schema_json,
                        const SchemaSpec     &slot_spec,
                        const SchemaSpec     &fz_spec)
{
    WireSchemaFields w;
    if (slot_schema_json.is_string())
        w.schema_id = slot_schema_json.get<std::string>();
    if (slot_spec.has_schema)
    {
        w.schema_blds    = canonical_fields_str(slot_spec);
        w.schema_packing = slot_spec.packing;
    }
    if (fz_spec.has_schema)
    {
        w.flexzone_blds    = canonical_fields_str(fz_spec);
        w.flexzone_packing = fz_spec.packing;
    }
    if (slot_spec.has_schema || fz_spec.has_schema)
    {
        const auto h = compute_canonical_hash_from_wire(
            w.schema_blds, w.schema_packing,
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
inline void apply_producer_schema_fields(nlohmann::json         &reg_opts,
                                         const WireSchemaFields &w)
{
    if (!w.schema_id.empty())        reg_opts["schema_id"]        = w.schema_id;
    if (!w.schema_owner.empty())     reg_opts["schema_owner"]     = w.schema_owner;
    if (!w.schema_hash.empty())      reg_opts["schema_hash"]      = w.schema_hash;
    if (!w.schema_blds.empty())      reg_opts["schema_blds"]      = w.schema_blds;
    if (!w.schema_packing.empty())   reg_opts["schema_packing"]   = w.schema_packing;
    if (!w.flexzone_blds.empty())    reg_opts["flexzone_blds"]    = w.flexzone_blds;
    if (!w.flexzone_packing.empty()) reg_opts["flexzone_packing"] = w.flexzone_packing;
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
inline void apply_consumer_schema_fields(nlohmann::json         &reg_opts,
                                         const WireSchemaFields &w)
{
    // HEP-CORE-0034 §10.2 — every consumer-side wire field carries the
    // `expected_schema_*` / `expected_flexzone_*` prefix to mirror the
    // producer's `schema_*` / `flexzone_*` fields.
    if (!w.schema_id.empty())        reg_opts["expected_schema_id"]        = w.schema_id;
    if (!w.schema_hash.empty())      reg_opts["expected_schema_hash"]      = w.schema_hash;
    if (!w.schema_blds.empty())      reg_opts["expected_schema_blds"]      = w.schema_blds;
    if (!w.schema_packing.empty())   reg_opts["expected_schema_packing"]   = w.schema_packing;
    if (!w.flexzone_blds.empty())    reg_opts["expected_flexzone_blds"]    = w.flexzone_blds;
    if (!w.flexzone_packing.empty()) reg_opts["expected_flexzone_packing"] = w.flexzone_packing;
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
inline std::vector<SchemaFieldDesc>
schema_spec_to_zmq_fields(const SchemaSpec &spec)
{
    if (spec.fields.empty())
        throw std::runtime_error("fields must not be empty");
    return to_field_descs(spec.fields);
}

} // namespace pylabhub::hub
