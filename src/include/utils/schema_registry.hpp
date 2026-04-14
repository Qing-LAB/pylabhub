#pragma once
/**
 * @file schema_registry.hpp
 * @brief Lifecycle-managed singleton wrapping SchemaLibrary with thread-safe access.
 *
 * `SchemaStore` is a lifecycle-managed singleton (Phase 4, HEP-CORE-0016)
 * that provides thread-safe access to a `SchemaLibrary` instance.  It loads
 * all schemas from the search path at startup and offers:
 *
 *   - Thread-safe forward lookup:  schema ID → SchemaEntry
 *   - Thread-safe reverse lookup:  BLAKE2b-256 hash → schema ID
 *   - Manual `reload()` to re-scan directories
 *
 * Named `SchemaStore` (not `SchemaRegistry`) to avoid collision with the
 * compile-time `template <typename T> struct SchemaRegistry` in schema_blds.hpp.
 *
 * ## Lifecycle
 * Depends on `pylabhub::utils::Logger` (for logging during init/shutdown).
 * Does NOT depend on ZMQ.
 *
 * ## Thread safety
 * All public methods acquire an internal mutex.  Safe to call from any thread
 * after lifecycle initialization.
 *
 * @see HEP-CORE-0016-Named-Schema-Registry.md §8
 * @see schema_library.hpp — underlying non-thread-safe storage
 * @see schema_def.hpp — SchemaEntry, SchemaFieldDef, SchemaLayoutDef
 */

#include "utils/schema_def.hpp"
#include "utils/schema_library.hpp"
#include "utils/module_def.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::schema
{

class PYLABHUB_UTILS_EXPORT SchemaStore
{
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Returns the ModuleDef for LifecycleManager registration.
    static pylabhub::utils::ModuleDef GetLifecycleModule();

    /// Returns the Meyer's singleton instance.
    static SchemaStore &instance();

    /// True after lifecycle startup completes, false after shutdown.
    static bool lifecycle_initialized() noexcept;

    // ── Lookup (thread-safe) ──────────────────────────────────────────────────

    /// Forward lookup: schema_id → SchemaEntry.
    [[nodiscard]] std::optional<SchemaEntry>
    get(const std::string &schema_id) const;

    /// Reverse lookup: BLAKE2b-256 hash → schema_id.
    [[nodiscard]] std::optional<std::string>
    identify(const std::array<uint8_t, 32> &slot_hash) const;

    /// List all registered schema IDs.
    [[nodiscard]] std::vector<std::string> list() const;

    // ── Reload ────────────────────────────────────────────────────────────────

    /// Re-scan search dirs and reload all schemas. Returns count loaded.
    /// In-memory registrations (from register_schema/query) are lost.
    size_t reload();

    /// Register a schema in-memory (e.g. from broker query result).
    void register_schema(const SchemaEntry &entry);

    // ── Configuration ─────────────────────────────────────────────────────────

    /// Set search dirs before lifecycle init (or before reload).
    /// If not called, SchemaLibrary::default_search_dirs() is used.
    void set_search_dirs(std::vector<std::string> dirs);

private:
    SchemaStore();
    ~SchemaStore();
    SchemaStore(const SchemaStore &) = delete;
    SchemaStore &operator=(const SchemaStore &) = delete;

    mutable std::mutex mu_;
    std::unique_ptr<SchemaLibrary> lib_;
    std::vector<std::string> search_dirs_;
};

} // namespace pylabhub::schema
