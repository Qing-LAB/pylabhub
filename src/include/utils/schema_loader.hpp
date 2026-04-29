#pragma once
/**
 * @file schema_loader.hpp
 * @brief Stateless schema parsers (HEP-CORE-0034 §2.4 I5).
 *
 * Pure JSON file/string parsers + the file-walker `load_all_from_dirs`
 * that produces `SchemaEntry` values. After parsing, callers MUST
 * translate via `to_hub_schema_record` (`schema_utils.hpp`) and route
 * into `HubState::_on_schema_registered` (HEP-CORE-0034 §2.4 I1+I2).
 * This module never holds, indexes, or returns schemas by id at runtime.
 *
 * **No state.** No maps, no caches, no in-memory registry. The historical
 * stateful `SchemaLibrary` (HEP-CORE-0016) carried `by_id_` / `by_hash_`
 * maps; those were removed in HEP-CORE-0034 Phase 4 — see §2.4 I5 for
 * the binding rule.
 *
 * The class name `SchemaLibrary` is retained as a namespace-shell
 * carrying the static parser methods.  All instance state, lookup
 * methods, and reverse-by-hash methods were deleted in Phase 4c; the
 * class can no longer be instantiated (copy/move ctors deleted).
 *
 * @see HEP-CORE-0034 §2.4 — module ownership and runtime invariants
 * @see schema_utils.hpp — `to_hub_schema_record` (sole bridge into the registry)
 * @see hub_state.hpp — `HubState::_on_schema_registered` (sole mutator)
 */

#include "pylabhub_utils_export.h"
#include "utils/schema_def.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pylabhub::schema
{

/**
 * @class SchemaLibrary
 * @brief Stateless namespace-shell for schema-file parsers.
 *
 * Despite the historical name, this class holds **no state** and is
 * **not** a registry. It carries only static parser methods. The class
 * shell exists for caller-source compatibility during HEP-CORE-0034
 * Phase 4 — the file rename to `schema_loader.hpp` lands in Phase 4β.
 *
 * **Forbidden** (would re-introduce HEP-CORE-0034 §2.4 I5 violation):
 *   - Adding non-static members.
 *   - Adding lookup-by-id or lookup-by-hash methods.
 *   - Adding any in-memory map keyed on schema_id, hash, or owner.
 *
 * **Permitted:** additional static parser helpers (e.g. parse-from-stream,
 * parse-from-bytes) that produce `SchemaEntry` and forget.
 *
 * @see HEP-CORE-0034 §2.4 I5 (stateless parser invariant)
 */
class PYLABHUB_UTILS_EXPORT SchemaLibrary
{
public:
    // ── Loading ───────────────────────────────────────────────────────────────

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
     * The JSON must follow the format specified in HEP-CORE-0034 §6 / HEP-CORE-0016 §6.
     *
     * @param json_text   UTF-8 JSON string.
     * @param schema_id   Override the schema ID. If empty, the ID is derived
     *                    from the JSON `"id"` and `"version"` fields:
     *                    `"${id}.v{version}"` (HEP-CORE-0033 §G2.2.0b).
     * @return Parsed SchemaEntry.
     * @throws std::runtime_error on parse error or missing required fields.
     */
    static SchemaEntry load_from_string(const std::string &json_text,
                                        const std::string &schema_id = {});

    // ── Utilities ─────────────────────────────────────────────────────────────

    /**
     * @brief Compute the SchemaInfo (BLDS, hash, struct_size, packing) for a field list.
     *
     * Public for testing and for C++ producers that want to compute a
     * compile-time struct's HEP-0002 BLDS fingerprint without loading a file.
     *
     * @note The returned `SchemaInfo::hash` is the **HEP-0002 BLDS form**
     *       (used by the SHM-header self-description), NOT the HEP-0034
     *       wire/registry form. The two are different by design — see
     *       HEP-CORE-0034 §2.4 I6. For wire/registry use, call
     *       `compute_canonical_hash_from_wire` in `schema_utils.hpp`.
     *
     * @param fields    Parsed field list (from a SchemaLayoutDef).
     * @param packing   "aligned" or "packed" (HEP-CORE-0034 §6.2). Folded into
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
     * Reads `PYLABHUB_SCHEMA_PATH` env var (colon-separated on POSIX,
     * semicolon on Windows), then appends `~/.pylabhub/schemas` and
     * `/usr/share/pylabhub/schemas` (POSIX only).
     */
    static std::vector<std::string> default_search_dirs();

    // SchemaLibrary is a stateless namespace-shell; no instances.
    SchemaLibrary()                                = delete;
    SchemaLibrary(const SchemaLibrary &)           = delete;
    SchemaLibrary(SchemaLibrary &&)                = delete;
    SchemaLibrary &operator=(const SchemaLibrary &) = delete;
    SchemaLibrary &operator=(SchemaLibrary &&)      = delete;

private:
    /// Encode a 32-byte hash as a 64-char lowercase hex string. Internal helper.
    static std::string hash_to_hex(const std::array<uint8_t, 32> &h);
    friend std::vector<std::pair<std::string, SchemaEntry>>
    load_all_from_dirs(const std::vector<std::string> &dirs);
};

// ============================================================================
// Free functions (stateless)
// ============================================================================

/**
 * @brief Walk @p dirs and parse every `.json` file, returning (path, entry) pairs.
 *
 * Directories are searched in order; first-match-wins by `schema_id`
 * (a file in an earlier-listed directory wins over a duplicate id in a
 * later-listed directory). Files that fail to parse are logged as
 * warnings and skipped. Files that parse successfully but whose
 * `schema_id` was already seen in an earlier directory are also skipped.
 *
 * The path is returned alongside the entry so diagnostics can name the
 * offending file when `_on_schema_registered` rejects with
 * `kHashMismatchSelf` (two separate files declaring the same id with
 * different fingerprints).
 *
 * **Caller responsibility (HEP-CORE-0034 §2.4 I2):** translate each
 * `SchemaEntry` via `to_hub_schema_record` (schema_utils.hpp) and
 * route into `HubState::_on_schema_registered`. This function does
 * NOT register anything — it is a pure walker.
 *
 * @param dirs Search path. Pass `SchemaLibrary::default_search_dirs()` for
 *             the platform default.
 * @return Vector of (file_path, parsed_entry) pairs; empty if no schemas
 *         were found.
 */
PYLABHUB_UTILS_EXPORT
std::vector<std::pair<std::string, SchemaEntry>>
load_all_from_dirs(const std::vector<std::string> &dirs);

} // namespace pylabhub::schema
