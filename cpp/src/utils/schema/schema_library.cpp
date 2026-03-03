// schema_library.cpp
//
// Implementation of SchemaLibrary: named schema loading, forward/reverse lookup.
//
// JSON schema format: HEP-CORE-0016 §6
// ID format:          "{namespace}.{name}@{version}"
// BLDS format:        HEP-CORE-0002 §11 / schema_blds.hpp
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

#include "utils/schema_library.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

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

SchemaLayoutDef parse_layout(const json &j)
{
    SchemaLayoutDef layout;
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

    // Resolve schema_id
    if (!id_override.empty())
    {
        e.schema_id = id_override;
        // Extract version from the override if it contains '@'
        const auto at = id_override.rfind('@');
        if (at != std::string::npos)
        {
            try
            {
                e.version = static_cast<uint32_t>(std::stoul(id_override.substr(at + 1)));
            }
            catch (...)
            {
                e.version = 1;
            }
        }
    }
    else
    {
        const std::string base_id = j.at("id").get<std::string>();
        e.version                 = j.value("version", 1u);
        e.schema_id               = base_id + "@" + std::to_string(e.version);
    }

    e.description = j.value("description", std::string{});

    // Parse slot (required)
    if (!j.contains("slot"))
        throw std::runtime_error("Schema '" + e.schema_id + "': missing required \"slot\" block");
    e.slot = parse_layout(j.at("slot"));

    // Parse flexzone (optional)
    if (j.contains("flexzone"))
        e.flexzone = parse_layout(j.at("flexzone"));

    // Compute slot SchemaInfo
    e.slot_info      = SchemaLibrary::compute_layout_info(e.slot.fields, e.schema_id + ".slot");

    // Compute flexzone SchemaInfo (even if empty — results in zero hash + zero size)
    e.flexzone_info  = SchemaLibrary::compute_layout_info(e.flexzone.fields,
                                                          e.schema_id + ".flexzone");

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
// SchemaLibrary — public implementation
// ============================================================================

SchemaLibrary::SchemaLibrary(std::vector<std::string> search_dirs)
    : search_dirs_(std::move(search_dirs))
{
    if (search_dirs_.empty())
        search_dirs_ = default_search_dirs();
}

// ── Static helpers ────────────────────────────────────────────────────────────

std::string SchemaLibrary::hash_to_hex(const std::array<uint8_t, 32> &h)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string            s;
    s.reserve(64);
    for (uint8_t b : h)
    {
        s += kHex[(b >> 4) & 0xF];
        s += kHex[b & 0xF];
    }
    return s;
}

SchemaInfo SchemaLibrary::compute_layout_info(const std::vector<SchemaFieldDef> &fields,
                                              const std::string                  &name)
{
    SchemaInfo info;
    info.name        = name;
    info.struct_size = fields_to_struct_size(fields);
    info.blds        = fields_to_blds(fields);

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

// ── Loading ───────────────────────────────────────────────────────────────────

size_t SchemaLibrary::load_all()
{
    size_t loaded = 0;
    for (const auto &dir : search_dirs_)
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

            auto opt = load_from_file(de.path().string());
            if (!opt)
                continue;

            // First-wins: skip if already registered under this ID
            if (by_id_.count(opt->schema_id) == 0)
            {
                register_schema(*opt);
                ++loaded;
            }
        }
    }
    return loaded;
}

std::optional<SchemaEntry> SchemaLibrary::load_from_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        LOGGER_WARN("[SchemaLibrary] Cannot open schema file: {}", path);
        return std::nullopt;
    }

    try
    {
        const json j = json::parse(file);
        return entry_from_json(j, {});
    }
    catch (const std::exception &ex)
    {
        LOGGER_WARN("[SchemaLibrary] Failed to parse schema file '{}': {}", path, ex.what());
        return std::nullopt;
    }
}

SchemaEntry SchemaLibrary::load_from_string(const std::string &json_text,
                                            const std::string &schema_id)
{
    const json j = json::parse(json_text); // throws on parse error
    return entry_from_json(j, schema_id);
}

// ── Lookup ────────────────────────────────────────────────────────────────────

std::optional<SchemaEntry> SchemaLibrary::get(const std::string &id) const
{
    const auto it = by_id_.find(id);
    if (it == by_id_.end())
        return std::nullopt;
    return it->second;
}

std::optional<std::string>
SchemaLibrary::identify(const std::array<uint8_t, 32> &slot_hash) const
{
    const std::string hex = hash_to_hex(slot_hash);
    const auto        it  = by_hash_.find(hex);
    if (it == by_hash_.end())
        return std::nullopt;
    return it->second;
}

// ── Registration ──────────────────────────────────────────────────────────────

void SchemaLibrary::register_schema(const SchemaEntry &entry)
{
    const std::string hex = hash_to_hex(entry.slot_info.hash);

    // Check for ID collision with different hash
    auto id_it = by_id_.find(entry.schema_id);
    if (id_it != by_id_.end())
    {
        if (id_it->second.slot_info.hash != entry.slot_info.hash)
        {
            LOGGER_WARN("[SchemaLibrary] ID '{}' already registered with different hash — "
                        "keeping first registration",
                        entry.schema_id);
        }
        return; // first-wins
    }

    by_id_.emplace(entry.schema_id, entry);

    // Register reverse map only if slot has fields (non-empty hash)
    const auto zero_hash = std::array<uint8_t, 32>{};
    if (entry.slot_info.hash != zero_hash)
    {
        // Only warn if a DIFFERENT schema ID already claims this hash
        auto hash_it = by_hash_.find(hex);
        if (hash_it == by_hash_.end())
            by_hash_.emplace(hex, entry.schema_id);
        else if (hash_it->second != entry.schema_id)
            LOGGER_WARN("[SchemaLibrary] Hash collision: schemas '{}' and '{}' have the same "
                        "slot hash — keeping first",
                        hash_it->second, entry.schema_id);
    }
}

// ── Enumeration ───────────────────────────────────────────────────────────────

std::vector<std::string> SchemaLibrary::list() const
{
    std::vector<std::string> ids;
    ids.reserve(by_id_.size());
    for (const auto &[id, _] : by_id_)
        ids.push_back(id);
    return ids;
}

} // namespace pylabhub::schema
