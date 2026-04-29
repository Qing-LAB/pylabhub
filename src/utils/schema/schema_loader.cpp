// schema_loader.cpp
//
// Stateless schema-file parsers (HEP-CORE-0034 §2.4 I5).
//
// This file holds:
//   - Static parser methods on `SchemaLibrary` (load_from_file,
//     load_from_string, compute_layout_info, default_search_dirs)
//   - Free function `load_all_from_dirs` (the §2.4 I2 entry-point)
//
// No state is held anywhere. The historical class state (`by_id_`,
// `by_hash_`) was removed by HEP-CORE-0034 Phase 4; runtime authority
// for schemas lives in `HubState.schemas` per §2.4 I1+I3.
//
// JSON schema format: HEP-CORE-0034 §6 / HEP-CORE-0016 §6
// ID format (HEP-0033 §G2.2.0b): "$<namespace>.<name>.v<version>"
//   Example: "$lab.sensors.temperature.raw.v1"
//
// JSON type → BLDS token map (HEP-CORE-0016 §6.1):
//   float32 → f32    float64 → f64
//   int8 → i8        int16 → i16    int32 → i32    int64 → i64
//   uint8 → u8       uint16 → u16   uint32 → u32   uint64 → u64
//   bool → b         char → c
//
// Natural alignment rules (§6.2):
//   - Offset aligned to sizeof(element_type)
//   - Total size rounded up to max(field alignments)
//   - Absent "count" or count==1 → scalar
//   - count > 1 → array of count scalars; alignment = scalar alignment

#include "utils/schema_loader.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/format_tools.hpp"
#include "utils/logger.hpp"
#include "utils/naming.hpp"

#include "utils/json_fwd.hpp"

#include <algorithm>
#include <cstdlib>  // std::getenv
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace pylabhub::schema
{

// ============================================================================
// Internal helpers (anonymous namespace)
// ============================================================================

namespace
{

// ── JSON type → element size / alignment ─────────────────────────────────────

struct TypeInfo
{
    size_t      size;       ///< sizeof(element_type)
    size_t      align;      ///< alignof(element_type) == size for BLDS types
    const char *blds_token; ///< BLDS type identifier string
};

const TypeInfo *type_info_for(const std::string &json_type)
{
    static constexpr TypeInfo kTypes[] = {
        {4, 4, "f32"},  // float32
        {8, 8, "f64"},  // float64
        {1, 1, "i8"},   // int8
        {2, 2, "i16"},  // int16
        {4, 4, "i32"},  // int32
        {8, 8, "i64"},  // int64
        {1, 1, "u8"},   // uint8
        {2, 2, "u16"},  // uint16
        {4, 4, "u32"},  // uint32
        {8, 8, "u64"},  // uint64
        {1, 1, "b"},    // bool
        {1, 1, "c"},    // char
    };
    static constexpr const char *kNames[] = {
        "float32", "float64",
        "int8", "int16", "int32", "int64",
        "uint8", "uint16", "uint32", "uint64",
        "bool", "char",
    };
    static constexpr size_t kCount = sizeof(kNames) / sizeof(kNames[0]);

    for (size_t i = 0; i < kCount; ++i)
    {
        if (json_type == kNames[i])
            return &kTypes[i];
    }
    return nullptr;
}

// ── BLDS string from field list ───────────────────────────────────────────────

std::string fields_to_blds(const std::vector<SchemaFieldDef> &fields)
{
    std::string blds;
    for (const auto &f : fields)
    {
        const TypeInfo *ti = type_info_for(f.type);
        if (!ti)
            throw std::invalid_argument("Unknown schema field type: '" + f.type + "'");

        if (!blds.empty())
            blds += ';';

        blds += f.name;
        blds += ':';
        blds += ti->blds_token;
        if (f.count > 1)
        {
            blds += '[';
            blds += std::to_string(f.count);
            blds += ']';
        }
    }
    return blds;
}

// ── Natural-alignment struct size from field list ─────────────────────────────

size_t fields_to_struct_size(const std::vector<SchemaFieldDef> &fields)
{
    size_t offset    = 0;
    size_t max_align = 1;

    for (const auto &f : fields)
    {
        const TypeInfo *ti = type_info_for(f.type);
        if (!ti)
            throw std::invalid_argument("Unknown schema field type: '" + f.type + "'");

        const size_t ealign = ti->align;
        const size_t esize  = ti->size * static_cast<size_t>(f.count);

        if (ealign > max_align)
            max_align = ealign;

        // Align current offset
        if (ealign > 0 && offset % ealign != 0)
            offset += ealign - (offset % ealign);

        offset += esize;
    }

    // Round total size up to max alignment (struct tail padding)
    if (max_align > 1 && offset % max_align != 0)
        offset += max_align - (offset % max_align);

    return offset;
}

// ── SchemaLayoutDef from JSON object ─────────────────────────────────────────

SchemaFieldDef parse_field(const json &j)
{
    SchemaFieldDef f;
    f.name        = j.at("name").get<std::string>();
    f.type        = j.at("type").get<std::string>();
    f.count       = j.value("count", 1u);
    f.unit        = j.value("unit", std::string{});
    f.description = j.value("description", std::string{});
    if (f.count < 1)
        f.count = 1;
    return f;
}

SchemaLayoutDef parse_layout(const json &j, const std::string &section_label)
{
    SchemaLayoutDef layout;

    // HEP-CORE-0034 §6.2 — `packing` MUST be declared explicitly per section.
    // It is part of the schema fingerprint (§6.3); a missing field would
    // silently default to one mode and collide with the other.
    if (!j.contains("packing"))
        throw std::runtime_error(
            "Schema: '" + section_label +
            ".packing' is required (HEP-CORE-0034 §6.2). Set "
            "\"packing\": \"aligned\" or \"packing\": \"packed\".");
    layout.packing = j.at("packing").get<std::string>();
    if (layout.packing != "aligned" && layout.packing != "packed")
        throw std::runtime_error(
            "Schema: '" + section_label +
            ".packing' must be \"aligned\" or \"packed\" (got \"" +
            layout.packing + "\").");

    if (j.contains("fields"))
    {
        for (const auto &fj : j.at("fields"))
            layout.fields.push_back(parse_field(fj));
    }
    return layout;
}

// ── SchemaEntry from a parsed JSON object ─────────────────────────────────────

SchemaEntry entry_from_json(const json &j, const std::string &id_override)
{
    SchemaEntry e;

    // Resolve schema_id.  One canonical form per HEP-0033 §G2.2.0b:
    //   "$<base>.v<version>"
    // Any old `<base>@<version>` input is rejected with a clear
    // diagnostic — no fallback parser (configs migrate to the new
    // form explicitly).
    if (!id_override.empty())
    {
        // Reject old-format overrides with a migration hint.
        if (id_override.find('@') != std::string::npos)
            throw std::runtime_error(
                "Schema id override '" + id_override +
                "' uses the retired '@<version>' form. Use "
                "'$<base>.v<version>' (HEP-0033 §G2.2.0b).");

        const auto parts = pylabhub::hub::parse_schema_id(id_override);
        if (!parts)
            throw std::runtime_error(
                "Schema id override '" + id_override +
                "' is not a valid schema id. Required form: "
                "'$<base>.v<version>' (HEP-0033 §G2.2.0b).");
        e.schema_id = id_override;
        e.version   = parts->version;
    }
    else
    {
        const std::string base_id  = j.at("id").get<std::string>();
        if (base_id.empty())
            throw std::runtime_error("Schema: 'id' field is empty");
        if (base_id.find('@') != std::string::npos)
            throw std::runtime_error(
                "Schema 'id' = '" + base_id +
                "' must not contain '@'; version is encoded as '.v<N>' "
                "(HEP-0033 §G2.2.0b).");
        if (base_id.front() == '$')
            throw std::runtime_error(
                "Schema 'id' = '" + base_id +
                "' must NOT include the '$' sigil — that is added "
                "automatically along with the '.v<version>' suffix.");

        e.version   = j.value("version", 1u);
        e.schema_id = "$" + base_id + ".v" + std::to_string(e.version);

        if (!pylabhub::hub::is_valid_identifier(
                e.schema_id, pylabhub::hub::IdentifierKind::Schema))
            throw std::runtime_error(
                "Schema auto-generated id '" + e.schema_id +
                "' is not a valid HEP-0033 schema id. Check that the "
                "'id' field uses only [A-Za-z0-9_-] in name components "
                "and '.' as separator.");
    }

    e.description = j.value("description", std::string{});

    // Parse slot (required)
    if (!j.contains("slot"))
        throw std::runtime_error("Schema '" + e.schema_id + "': missing required \"slot\" block");
    e.slot = parse_layout(j.at("slot"), e.schema_id + ".slot");

    // Parse flexzone (optional)
    if (j.contains("flexzone"))
        e.flexzone = parse_layout(j.at("flexzone"), e.schema_id + ".flexzone");

    // Compute slot SchemaInfo (packing folded into fingerprint per HEP-CORE-0034 §6.3)
    e.slot_info     = SchemaLibrary::compute_layout_info(
        e.slot.fields, e.slot.packing, e.schema_id + ".slot");

    // Compute flexzone SchemaInfo (even if empty — results in zero hash + zero size).
    // When flexzone is absent, the SchemaLayoutDef default packing="aligned" is fine
    // because compute_layout_info early-returns on empty fields.
    e.flexzone_info = SchemaLibrary::compute_layout_info(
        e.flexzone.fields, e.flexzone.packing, e.schema_id + ".flexzone");

    return e;
}

// ── Home directory lookup (portable) ─────────────────────────────────────────

std::string home_dir()
{
    const char *home = std::getenv("HOME");
    if (home && home[0] != '\0')
        return home;
#if defined(_WIN32)
    const char *userprofile = std::getenv("USERPROFILE");
    if (userprofile && userprofile[0] != '\0')
        return userprofile;
#endif
    return {};
}

} // anonymous namespace

// ============================================================================
// SchemaLibrary — static parser methods (no state)
// ============================================================================

std::string SchemaLibrary::hash_to_hex(const std::array<uint8_t, 32> &h)
{
    return format_tools::bytes_to_hex({reinterpret_cast<const char *>(h.data()), h.size()});
}

SchemaInfo SchemaLibrary::compute_layout_info(const std::vector<SchemaFieldDef> &fields,
                                              const std::string                 &packing,
                                              const std::string                 &name)
{
    SchemaInfo info;
    info.name        = name;
    info.struct_size = fields_to_struct_size(fields);
    info.blds        = fields_to_blds(fields);
    info.packing     = packing;  // HEP-CORE-0034 §6.3 — packing in fingerprint

    if (!info.blds.empty())
        info.compute_hash();
    // If blds is empty (no fields), hash stays zero-filled and struct_size == 0.

    return info;
}

std::vector<std::string> SchemaLibrary::default_search_dirs()
{
    std::vector<std::string> dirs;

    // 1. PYLABHUB_SCHEMA_PATH env (colon-separated on POSIX, semicolon on Windows)
    const char *env = std::getenv("PYLABHUB_SCHEMA_PATH");
    if (env && env[0] != '\0')
    {
#if defined(_WIN32)
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif
        std::string     sv(env);
        std::string::size_type pos = 0;
        while (pos < sv.size())
        {
            const auto end = sv.find(sep, pos);
            const auto segment = (end == std::string::npos)
                                     ? sv.substr(pos)
                                     : sv.substr(pos, end - pos);
            if (!segment.empty())
                dirs.push_back(segment);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }

    // 2. ~/.pylabhub/schemas
    const std::string home = home_dir();
    if (!home.empty())
        dirs.push_back(home + "/.pylabhub/schemas");

    // 3. /usr/share/pylabhub/schemas (POSIX)
#if !defined(_WIN32)
    dirs.emplace_back("/usr/share/pylabhub/schemas");
#endif

    return dirs;
}

std::optional<SchemaEntry> SchemaLibrary::load_from_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        LOGGER_WARN("[schema_loader] Cannot open schema file: {}", path);
        return std::nullopt;
    }

    try
    {
        const json j = json::parse(file);
        return entry_from_json(j, {});
    }
    catch (const std::exception &ex)
    {
        LOGGER_WARN("[schema_loader] Failed to parse schema file '{}': {}", path, ex.what());
        return std::nullopt;
    }
}

SchemaEntry SchemaLibrary::load_from_string(const std::string &json_text,
                                            const std::string &schema_id)
{
    const json j = json::parse(json_text); // throws on parse error
    return entry_from_json(j, schema_id);
}

// ============================================================================
// Free function — pure walker (HEP-CORE-0034 §2.4 I2 entry-point)
// ============================================================================

std::vector<std::pair<std::string, SchemaEntry>>
load_all_from_dirs(const std::vector<std::string> &dirs)
{
    std::vector<std::pair<std::string, SchemaEntry>> out;
    // Track schema_ids seen so we can apply first-match-wins across dirs
    // without holding any persistent state ourselves.
    std::vector<std::string> seen_ids;

    for (const auto &dir : dirs)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
            continue;

        for (const auto &de : fs::recursive_directory_iterator(dir, ec))
        {
            if (ec)
                break; // directory unreadable
            if (!de.is_regular_file())
                continue;
            if (de.path().extension() != ".json")
                continue;

            auto opt = SchemaLibrary::load_from_file(de.path().string());
            if (!opt)
                continue;

            const auto id = opt->schema_id;
            if (std::find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end())
            {
                LOGGER_WARN("[schema_loader] Duplicate schema_id '{}' in '{}' — "
                            "earlier directory wins (HEP-CORE-0034 §2.4 I2)",
                            id, de.path().string());
                continue;
            }
            seen_ids.push_back(id);
            out.emplace_back(de.path().string(), std::move(*opt));
        }
    }
    return out;
}

} // namespace pylabhub::schema
