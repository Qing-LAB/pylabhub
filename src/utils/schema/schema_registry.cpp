// schema_registry.cpp
//
// Lifecycle-managed singleton wrapping SchemaLibrary with thread-safe access.
//
// Phase 4 of HEP-CORE-0016 (Named Schema Registry).
//
// Lifecycle: depends on Logger only. Loads all schemas from search dirs at
// startup. Provides thread-safe get/identify/list/reload and an explicit
// broker query fallback (caller supplies Messenger&).

#include "utils/schema_registry.hpp"
#include "utils/logger.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <mutex>

namespace pylabhub::schema
{

// ============================================================================
// Lifecycle state
// ============================================================================

static std::atomic<bool> g_registry_initialized{false};

// ── C-style lifecycle callbacks (ABI-safe) ──────────────────────────────────

static void do_registry_startup(const char * /*arg*/, void * /*userdata*/)
{
    auto &reg         = SchemaStore::instance();
    const size_t count = reg.reload();
    g_registry_initialized.store(true, std::memory_order_release);
    LOGGER_INFO("[SchemaStore] Initialized — {} schema(s) loaded", count);
}

static void do_registry_shutdown(const char * /*arg*/, void * /*userdata*/)
{
    g_registry_initialized.store(false, std::memory_order_release);
    LOGGER_INFO("[SchemaStore] Shutdown");
}

// ============================================================================
// Lifecycle module definition
// ============================================================================

pylabhub::utils::ModuleDef SchemaStore::GetLifecycleModule()
{
    pylabhub::utils::ModuleDef m("pylabhub::schema::SchemaStore");
    m.add_dependency("pylabhub::utils::Logger");
    m.set_startup(&do_registry_startup);
    m.set_shutdown(&do_registry_shutdown, std::chrono::milliseconds{1000});
    return m;
}

// ============================================================================
// Singleton
// ============================================================================

SchemaStore &SchemaStore::instance()
{
    static SchemaStore inst;
    return inst;
}

bool SchemaStore::lifecycle_initialized() noexcept
{
    return g_registry_initialized.load(std::memory_order_acquire);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

SchemaStore::SchemaStore()
    : lib_(std::make_unique<SchemaLibrary>(std::vector<std::string>{}))
{
    // Constructed with empty search dirs — actual loading happens in reload()
    // which is called by do_registry_startup().
}

SchemaStore::~SchemaStore() = default;

// ============================================================================
// Lookup — thread-safe delegates to SchemaLibrary
// ============================================================================

std::optional<SchemaEntry> SchemaStore::get(const std::string &schema_id) const
{
    std::lock_guard lock(mu_);
    return lib_->get(schema_id);
}

std::optional<std::string>
SchemaStore::identify(const std::array<uint8_t, 32> &slot_hash) const
{
    std::lock_guard lock(mu_);
    return lib_->identify(slot_hash);
}

std::vector<std::string> SchemaStore::list() const
{
    std::lock_guard lock(mu_);
    return lib_->list();
}

// ============================================================================
// Registration
// ============================================================================

void SchemaStore::register_schema(const SchemaEntry &entry)
{
    std::lock_guard lock(mu_);
    lib_->register_schema(entry);
}

// ============================================================================
// Reload
// ============================================================================

size_t SchemaStore::reload()
{
    std::lock_guard lock(mu_);
    const auto dirs = search_dirs_.empty()
                          ? SchemaLibrary::default_search_dirs()
                          : search_dirs_;
    lib_ = std::make_unique<SchemaLibrary>(dirs);
    return lib_->load_all();
}

// ============================================================================
// Configuration
// ============================================================================

void SchemaStore::set_search_dirs(std::vector<std::string> dirs)
{
    std::lock_guard lock(mu_);
    search_dirs_ = std::move(dirs);
}

// ============================================================================
// Broker query fallback
// ============================================================================

std::optional<SchemaEntry>
SchemaStore::query_from_broker(const std::string &channel_name,
                                  pylabhub::hub::Messenger &messenger,
                                  int timeout_ms)
{
    // query_channel_schema is thread-safe on Messenger side — no lock needed for the call.
    auto info = messenger.query_channel_schema(channel_name, timeout_ms);
    if (!info)
        return std::nullopt;
    if (info->blds.empty())
        return std::nullopt; // anonymous channel with no BLDS

    // Build a SchemaEntry from the BLDS + schema_id returned by broker.
    SchemaEntry entry;
    entry.schema_id = info->schema_id;
    entry.slot_info.blds = info->blds;
    entry.slot_info.compute_hash();

    std::lock_guard lock(mu_);
    if (!info->schema_id.empty())
        lib_->register_schema(entry);
    return entry;
}

} // namespace pylabhub::schema
