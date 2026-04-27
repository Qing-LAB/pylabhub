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
#include "utils/hub_zmq_queue.hpp"
#include "utils/schema_field_layout.hpp"
#include "utils/schema_library.hpp"

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

inline SchemaSpec resolve_named_schema(const std::string &schema_id, bool use_flexzone,
                                       const char *label,
                                       const std::vector<std::string> &extra_search_dirs = {})
{
    auto search_dirs = extra_search_dirs;
    const auto defaults = schema::SchemaLibrary::default_search_dirs();
    search_dirs.insert(search_dirs.end(), defaults.begin(), defaults.end());
    schema::SchemaLibrary lib(std::move(search_dirs));
    lib.load_all();
    auto entry = lib.get(schema_id);
    if (!entry)
        throw std::runtime_error(
            std::string("[") + label + "] Named schema '" + schema_id +
            "' not found in schema library");
    const auto &layout = use_flexzone ? entry->flexzone : entry->slot;
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
//   slot:<fields>|pack:<packing>[|fz:<fields>|fzpack:<packing>]
//
// `packing` is appended for each present section so two layouts with
// identical fields and different packing produce different hashes.

inline void append_schema_canonical(std::string &out, const std::string &prefix,
                                    const SchemaSpec &spec)
{
    out += prefix;
    bool first = true;
    for (const auto &f : spec.fields)
    {
        if (!first) out += '|';
        first = false;
        out += f.name;    out += ':';
        out += f.type_str; out += ':';
        out += std::to_string(f.count);  out += ':';
        out += std::to_string(f.length);
    }
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
