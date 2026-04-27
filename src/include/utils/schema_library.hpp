#pragma once
/**
 * @file schema_library.hpp
 * @brief Named Schema Library — plain utility class for schema lookup and registration.
 *
 * `SchemaLibrary` is a **plain utility** (no lifecycle dependency, no threads,
 * no singleton).  It manages a collection of named schemas in memory and
 * provides:
 *   - Forward lookup:  schema ID  → `SchemaEntry`  (`get()`)
 *   - Reverse lookup:  BLAKE2b-256 slot hash → schema ID  (`identify()`)
 *   - File-based loading: scan directories for `.json` schema files (`load_all()`)
 *   - In-memory registration: `register_schema()` for programmatic schemas
 *
 * ## Search path (priority: first wins)
 * 1. `PYLABHUB_SCHEMA_PATH` environment variable (colon-separated directories)
 * 2. `~/.pylabhub/schemas/`
 * 3. `/usr/share/pylabhub/schemas/`
 *
 * Pass an explicit `search_dirs` vector to the constructor to override the
 * default search path — useful in tests.
 *
 * ## Thread safety
 * `SchemaLibrary` is **not thread-safe**.  For multi-threaded use, acquire an
 * external mutex before calling any method.  `SchemaRegistry` (Phase 4,
 * HEP-CORE-0016) will wrap SchemaLibrary with thread-safe access.
 *
 * @see HEP-CORE-0016-Named-Schema-Registry.md §8 (C++ Module Design)
 * @see schema_def.hpp — data structures: SchemaFieldDef, SchemaLayoutDef, SchemaEntry
 */

#include "utils/schema_def.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace pylabhub::schema
{

/**
 * @class SchemaLibrary
 * @brief Plain utility class: in-memory store of named schemas with forward
 *        and reverse (hash-based) lookup.
 *
 * ### Typical usage
 * @code{.cpp}
 * SchemaLibrary lib;           // uses default search path
 * lib.load_all();              // scan dirs, load all *.json schema files
 *
 * auto entry = lib.get("$lab.sensors.temperature.raw.v1");
 * if (entry) {
 *     auto fields = entry->slot_layout.fields;  // SchemaFieldDef list
 *     // Pass to ShmQueue::create_reader() which computes sizes internally
 * }
 *
 * // Reverse lookup: does this unnamed schema match a known named one?
 * auto id = lib.identify(producer_hash);
 * // id = "$lab.sensors.temperature.raw.v1" if a named schema matches
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT SchemaLibrary
{
public:
    // ── Construction ──────────────────────────────────────────────────────────

    /**
     * @brief Construct with explicit search directories.
     *
     * The directories are searched in order; the first match wins.
     * Pass an empty vector to use the default search path
     * (`PYLABHUB_SCHEMA_PATH` env, `~/.pylabhub/schemas`,
     * `/usr/share/pylabhub/schemas`).
     */
    explicit SchemaLibrary(std::vector<std::string> search_dirs = {});

    // ── Loading ───────────────────────────────────────────────────────────────

    /**
     * @brief Scan all search directories and load every `.json` schema file.
     *
     * Files that fail to parse are skipped (logged as warnings).  Files that
     * parse but have duplicate IDs do NOT replace an earlier registration
     * (first-wins semantics matching the search-path priority order).
     *
     * @return Number of schemas successfully loaded and registered.
     */
    size_t load_all();

    /**
     * @brief Load and parse a single JSON schema file by path.
     *
     * @param path Absolute or relative path to the `.json` file.
     * @return Parsed SchemaEntry on success; nullopt on soft error (logged).
     * @throws std::runtime_error on hard I/O error or corrupt JSON.
     */
    static std::optional<SchemaEntry> load_from_file(const std::string &path);

    /**
     * @brief Parse a SchemaEntry from a JSON string.
     *
     * Useful for tests and in-memory schema definitions without touching disk.
     * The JSON must follow the format specified in HEP-CORE-0016 §6.
     *
     * @param json_text   UTF-8 JSON string.
     * @param schema_id   Override the schema ID.  If empty, the ID is derived
     *                    from the JSON `"id"` and `"version"` fields:
     *                    `"{id}@{version}"`.
     * @return Parsed SchemaEntry.
     * @throws std::runtime_error on parse error or missing required fields.
     */
    static SchemaEntry load_from_string(const std::string &json_text,
                                        const std::string &schema_id = {});

    // ── Lookup ────────────────────────────────────────────────────────────────

    /**
     * @brief Forward lookup: schema ID → SchemaEntry.
     *
     * @param id Schema ID including version, e.g. `"$lab.sensors.temperature.raw.v1"`.
     * @return Matching entry, or nullopt if unknown.
     */
    [[nodiscard]] std::optional<SchemaEntry> get(const std::string &id) const;

    /**
     * @brief Reverse lookup: slot BLAKE2b-256 hash → schema ID.
     *
     * Enables broker-side annotation: given an unnamed schema's hash,
     * find whether any registered named schema has the same slot layout.
     *
     * @param slot_hash 32-byte BLAKE2b-256 hash of the slot BLDS string.
     * @return Matching schema ID, or nullopt if not registered.
     */
    [[nodiscard]] std::optional<std::string>
    identify(const std::array<uint8_t, 32> &slot_hash) const;

    // ── Registration ──────────────────────────────────────────────────────────

    /**
     * @brief Register a schema entry in memory.
     *
     * If a schema with the same ID is already registered, the call is a no-op
     * (first-wins semantics; log a warning if IDs match but hashes differ).
     */
    void register_schema(const SchemaEntry &entry);

    // ── Enumeration ───────────────────────────────────────────────────────────

    /**
     * @brief Return all registered schema IDs (unordered).
     */
    [[nodiscard]] std::vector<std::string> list() const;

    // ── Utilities ─────────────────────────────────────────────────────────────

    /**
     * @brief Compute the SchemaInfo (BLDS, hash, struct_size, packing) for a field list.
     *
     * Public for testing and for C++ producers that want to validate a
     * compile-time struct against a named schema without loading a file.
     *
     * @param fields    Parsed field list (from a SchemaLayoutDef).
     * @param packing   "aligned" or "packed" (HEP-CORE-0034 §6.2).  Folded into
     *                  the fingerprint canonical form (§6.3).
     * @param name      Logical name stored in SchemaInfo::name (informational only).
     * @return SchemaInfo with blds, hash, struct_size, and packing populated.
     * @throws std::invalid_argument if any field has an unknown type string.
     */
    static SchemaInfo compute_layout_info(const std::vector<SchemaFieldDef> &fields,
                                          const std::string                 &packing = "aligned",
                                          const std::string                 &name    = {});

    /**
     * @brief Return the default search directories for the current environment.
     *
     * Reads `PYLABHUB_SCHEMA_PATH` env var (colon-separated), then appends
     * `~/.pylabhub/schemas` and `/usr/share/pylabhub/schemas`.
     */
    static std::vector<std::string> default_search_dirs();

private:
    std::vector<std::string>                     search_dirs_;
    std::unordered_map<std::string, SchemaEntry> by_id_;    ///< schema_id → entry
    std::unordered_map<std::string, std::string> by_hash_;  ///< hex(slot_hash) → schema_id

    /// Encode a 32-byte hash as a 64-char lowercase hex string.
    static std::string hash_to_hex(const std::array<uint8_t, 32> &h);
};

} // namespace pylabhub::schema

// ============================================================================
// Named Schema Validation (HEP-CORE-0016 Phase 2)
// ============================================================================
//
// validate_named_schema<DataT, FlexT>(schema_id, lib)
//   Validates that DataT (and optionally FlexT) match the named schema in `lib`.
//   Performs size check unconditionally; adds BLDS hash check when DataT/FlexT
//   are registered with PYLABHUB_SCHEMA_BEGIN/MEMBER/END.
//
// validate_named_schema_from_env<DataT, FlexT>(schema_id)
//   Convenience wrapper: builds a SchemaLibrary from default_search_dirs(),
//   loads all schemas, then calls validate_named_schema.
//
// Both functions are no-ops when schema_id is empty.
// Both throw SchemaValidationException on any mismatch or unknown ID.
//
// Called from Producer::create<F,D>() and Consumer::connect<F,D>() when
// ProducerOptions::schema_id / ConsumerOptions::expected_schema_id is set.
// ============================================================================

namespace pylabhub::schema
{

/**
 * @brief Validate that DataT (and optionally FlexT) match a named schema in `lib`.
 *
 * Checks performed when schema_id is non-empty:
 *   1. `lib.get(schema_id)` must return an entry (not nullopt).
 *   2. `sizeof(DataT)` == `entry.slot_info.struct_size` (always).
 *   3. If DataT has PYLABHUB_SCHEMA macros: BLDS hash must match.
 *   4. When FlexT != void and schema declares a flexzone:
 *      - `sizeof(FlexT)` == `entry.flexzone_info.struct_size`.
 *      - If FlexT has PYLABHUB_SCHEMA macros: BLDS hash must match.
 *
 * @throws SchemaValidationException on any mismatch or unknown ID.
 */
template <typename DataT, typename FlexT = void>
void validate_named_schema(const std::string &schema_id, const SchemaLibrary &lib)
{
    if (schema_id.empty())
        return;

    // ── Forward lookup ────────────────────────────────────────────────────────
    const auto entry = lib.get(schema_id);
    if (!entry)
    {
        throw SchemaValidationException(
            "Named schema '" + schema_id + "' not found in schema library. "
            "Ensure schema JSON files are on PYLABHUB_SCHEMA_PATH or call lib.load_all().",
            {}, {});
    }

    // ── Slot size check ───────────────────────────────────────────────────────
    if (entry->slot_info.struct_size != sizeof(DataT))
    {
        throw SchemaValidationException(
            "Named schema '" + schema_id + "' slot size mismatch: "
            "schema=" + std::to_string(entry->slot_info.struct_size) +
            " bytes, C++ sizeof=" + std::to_string(sizeof(DataT)),
            {}, {});
    }

    // ── Slot hash check (only when PYLABHUB_SCHEMA macros are used) ───────────
    if constexpr (has_schema_registry_v<DataT>)
    {
        const SchemaInfo cpp_info = generate_schema_info<DataT>("", SchemaVersion{});
        if (cpp_info.hash != entry->slot_info.hash)
        {
            throw SchemaValidationException(
                "Named schema '" + schema_id + "' slot BLDS mismatch: "
                "C++ struct layout differs from the named schema definition. "
                "Schema BLDS: " + entry->slot_info.blds + "  "
                "C++ BLDS: " + cpp_info.blds,
                entry->slot_info.hash, cpp_info.hash);
        }
    }

    // ── FlexZone checks (when FlexT != void) ──────────────────────────────────
    if constexpr (!std::is_void_v<FlexT>)
    {
        if (entry->has_flexzone())
        {
            if (entry->flexzone_info.struct_size != sizeof(FlexT))
            {
                throw SchemaValidationException(
                    "Named schema '" + schema_id + "' flexzone size mismatch: "
                    "schema=" + std::to_string(entry->flexzone_info.struct_size) +
                    " bytes, C++ sizeof=" + std::to_string(sizeof(FlexT)),
                    {}, {});
            }

            if constexpr (has_schema_registry_v<FlexT>)
            {
                const SchemaInfo fz_info = generate_schema_info<FlexT>("", SchemaVersion{});
                if (fz_info.hash != entry->flexzone_info.hash)
                {
                    throw SchemaValidationException(
                        "Named schema '" + schema_id + "' flexzone BLDS mismatch: "
                        "C++ flexzone layout differs from the named schema definition.",
                        entry->flexzone_info.hash, fz_info.hash);
                }
            }
        }
    }
}

/**
 * @brief Convenience: build a SchemaLibrary from default_search_dirs(), load all
 *        schemas, then call validate_named_schema<DataT, FlexT>(schema_id, lib).
 *
 * Called by Producer::create<F,D>() and Consumer::connect<F,D>() when
 * ProducerOptions::schema_id / ConsumerOptions::expected_schema_id is set.
 *
 * @throws SchemaValidationException on mismatch or unknown ID.
 * @note No-op when schema_id is empty.
 */
template <typename DataT, typename FlexT = void>
void validate_named_schema_from_env(const std::string &schema_id)
{
    if (schema_id.empty())
        return;

    SchemaLibrary lib(SchemaLibrary::default_search_dirs());
    lib.load_all();
    validate_named_schema<DataT, FlexT>(schema_id, lib);
}

} // namespace pylabhub::schema
