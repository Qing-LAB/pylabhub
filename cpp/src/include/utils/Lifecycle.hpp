#pragma once

/*******************************************************************************
 * @file Lifecycle.hpp
 * @brief Manages application startup and shutdown with dependency-aware modules.
 *
 * @see src/utils/Lifecycle.cpp
 * @see tests/test_recursionguard.cpp (for an example of usage)
 *
 * **Design Philosophy**
 *
 * This system provides a robust, centralized mechanism for managing the lifecycle
 * of an application's components (modules). It ensures that modules are started
 * in the correct order based on their dependencies and shut down in the reverse
 * order.
 *
 * 1.  **Dependency Management**: The core of the manager is a dependency graph.
 *     Modules declare their dependencies by name, and the `LifecycleManager`
 *     performs a topological sort to determine the correct initialization
 *     sequence. This prevents race conditions where a module might be used
 *     before it is ready. The system also detects and reports cyclical
 *     dependencies as a fatal error.
 *
 * 2.  **ABI Stability (Pimpl Idiom)**: Both `LifecycleManager` and `ModuleDef`
 *     use the Pointer to Implementation (Pimpl) idiom. All internal data
 *     structures, especially STL containers (`std::vector`, `std::map`, etc.),
 *     are hidden in the private `Impl` classes. The public header exposes only
 *     ABI-stable types (raw pointers, C-style function pointers, `unique_ptr`
 *     with a defined deleter). This is critical for shared libraries, as it
 *     prevents the ABI from breaking when the implementation changes or when
 *     the library is used with a different compiler or standard library version.
 *
 * 3.  **Singleton Pattern**: The `LifecycleManager` is a singleton, providing a
 *     single, globally accessible point for module registration and lifecycle
 *     control. This simplifies integration, as components do not need to have a
 *     manager instance passed to them.
 *
 * 4.  **Graceful Shutdown**: The shutdown process includes a configurable timeout
 *     for each module. This prevents a misbehaving or hanging module from
 *     indefinitely blocking application termination.
 *
 * **Usage**
 *
 * The application's `main` function is responsible for orchestrating the
 * lifecycle. This is best done with the `LifecycleGuard` RAII helper.
 *
 * ```cpp
 * #include "utils/Lifecycle.hpp"
 * #include "utils/Logger.hpp"
 * #include "utils/FileLock.hpp"
 *
 * int main(int argc, char* argv[]) {
 *     // In main(), create a LifecycleGuard and pass it the ModuleDef objects
 *     // from all the utilities the application will use.
 *     pylabhub::utils::LifecycleGuard app_lifecycle(
 *         pylabhub::utils::Logger::GetLifecycleModule(),
 *         pylabhub::utils::FileLock::GetLifecycleModule()
 *         // ... add other modules here ...
 *     );
 *
 *     // The constructor of the guard calls InitializeApp(), which starts all
 *     // modules in the correct dependency order. It is now safe to use them.
 *     LOGGER_INFO("Application started successfully.");
 *
 *     // ... main application logic runs here ...
 *
 *     // When `app_lifecycle` goes out of scope at the end of main, its
 *     // destructor automatically calls FinalizeApp(), ensuring a graceful
 *     // shutdown of all modules.
 *     return 0;
 * }
 * ```
 ******************************************************************************/
#include "plh_base.hpp"
#include "pylabhub_utils_export.h"
#include "utils/ModuleDef.hpp"

#include <memory>
#include <type_traits>
#include <utility>

// Disable warning C4251 on MSVC. This is a common practice for exported classes
// that use std::unique_ptr to a forward-declared (incomplete) type (Pimpl).
// The user of the class cannot access the member, so it's safe.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

// Forward-declare the implementation structs to hide them from the public API,
// which is the essence of the Pimpl idiom.
class LifecycleManagerImpl;

/// @brief Helper factory: constructs a vector<ModuleDef> by moving the supplied ModuleDef args.
// Call-site: MakeModDefList(std::move(a), std::move(b)) or MakeModDefList(MyFactory(), ...)
template <typename... Mods> inline std::vector<ModuleDef> MakeModDefList(Mods &&...mods)
{
    static_assert((std::is_same_v<std::decay_t<Mods>, ModuleDef> && ...),
                  "MakeModDefList: all arguments must be ModuleDef (rvalues or prvalues)");

    std::vector<ModuleDef> modules;
    modules.reserve(sizeof...(mods));
    (modules.emplace_back(std::forward<Mods>(mods)), ...);
    return modules; // NRVO / move
}

/**
 * @class LifecycleManager
 * @brief The ABI-safe singleton manager for the application lifecycle.
 *
 * This class orchestrates the initialization and finalization of all registered
 * modules. It is implemented as a singleton to provide global access. Like
 * `ModuleDef`, it uses the Pimpl idiom to ensure ABI stability.
 */
class PYLABHUB_UTILS_EXPORT LifecycleManager
{
  public:
    /**
     * @brief Accessor for the singleton instance.
     * @return A reference to the single global instance of the LifecycleManager.
     */
    static LifecycleManager &instance();

    /**
     * @brief Registers a module with the lifecycle system.
     *
     * After this call, the lifecycle system takes ownership of the module
     * definition. All modules must be registered *before* `initialize()` or
     * `pylabhub::utils::InitializeApp()` is called. Registration after
     * initialization has begun will cause a fatal error.
     *
     * @param module_def The fully configured module object, passed by rvalue-reference
     *                   to indicate a transfer of ownership.
     */
    void register_module(ModuleDef &&module_def);

    /**
     * @brief Initializes the application by executing module startup functions.
     *
     * This function should be called once at the start of the application. It is
     * idempotent (safe to call multiple times). It performs a topological sort on
     * registered modules and will abort the application if a dependency cycle is
     * detected.
     *
     * @note It is recommended to use the `pylabhub::utils::InitializeApp()`
     *       helper function instead of calling this directly.
     */
    void initialize(std::source_location loc);

    /**
     * @brief Shuts down the application by executing module shutdown functions.
     *
     * This function should be called once at the end of the application's
     * lifecycle. It is idempotent. Shutdown proceeds in the reverse order of
     * initialization.
     *
     * @note It is recommended to use the `pylabhub::utils::FinalizeApp()`
     *       helper function instead of calling this directly.
     */
    void finalize(std::source_location loc);

    /**
     * @brief Checks if the lifecycle manager has been initialized.
     *
     * This allows callers to verify if `initialize()` has completed. It is useful
     * for components that might be loaded late (e.g., plugins) to check if it is
     * too late to register new modules.
     *
     * @return `true` if `initialize()` has been called, `false` otherwise.
     */
    bool is_initialized();

    /**
     * @brief Checks if the lifecycle manager has been finalized.
     *
     * This allows callers to verify if `finalize()` has completed.
     *
     * @return `true` if `finalize()` has been called, `false` otherwise.
     */
    bool is_finalized();

    /**
     * @brief Registers a dynamic module with the lifecycle system.
     * @details Dynamic modules can be registered at runtime after the static core of the
     * application has been initialized. They can be loaded and unloaded on-demand.
     * This call is only valid *after* `initialize()` has been called and *before*
     * `finalize()` is called. It is thread-safe.
     *
     * @param module_def The fully configured module object, passed by rvalue-reference
     *                   to indicate a transfer of ownership.
     * @return `true` if the module was successfully registered, `false` if registration
     *         failed (e.g., a module with the same name exists, or a dependency
     *         is not found).
     */
    bool register_dynamic_module(ModuleDef &&module_def);

    /**
     * @brief Loads a dynamic module and its dependencies.
     *
     * This function is thread-safe and idempotent. If the module is already
     * loaded, it will increment its internal reference count and return true.
     * It recursively loads all dependencies. This function must not be called
     * from within a startup or shutdown callback.
     *
     * @param name The name of the module to load.
     * @return `true` if the module was loaded successfully, `false` otherwise.
     */
    bool load_module(const char *name, std::source_location loc);

    /**
     * @brief Unloads a dynamic module.
     *
     * This function is thread-safe and idempotent. It decrements the module's
     * reference count. The module is only fully shut down if its reference
     * count reaches zero. Unloading a module will recursively trigger an unload
     * on its dependencies. This function must not be called from within a
     * startup or shutdown callback.
     *
     * @param name The name of the module to unload.
     * @return `true` if the module was considered for unloading, `false` if it
     *         was not found or not loaded.
     */
    bool unload_module(const char *name, std::source_location loc);

    // --- Rule of Five: Singleton, not Copyable or Assignable ---
    LifecycleManager(const LifecycleManager &) = delete;
    LifecycleManager &operator=(const LifecycleManager &) = delete;

  private:
    // Private constructor/destructor to enforce the singleton pattern.
    LifecycleManager();
    ~LifecycleManager();

    std::unique_ptr<LifecycleManagerImpl> pImpl;
};

/**
 * The following functions and classes are the actual interface for the users.
 */

/**
 * @brief A convenience function to register a module with the LifecycleManager.
 *
 * This is the preferred, type-safe way to register a module. It is a simple
 * inline wrapper around the `LifecycleManager::instance()` call.
 *
 * @param module_def A ModuleDef object, which will be moved into the manager.
 */
inline void RegisterModule(ModuleDef &&module_def)
{
    // The `::` prefix ensures we call the global LifecycleManager class.
    LifecycleManager::instance().register_module(std::move(module_def));
}

/**
 * @brief A convenience function to check if the application is initialized.
 * @return `true` if `initialize()` has been called, `false` otherwise.
 */
inline bool IsAppInitialized()
{
    return LifecycleManager::instance().is_initialized();
}

inline bool IsAppFinalized()
{
    return LifecycleManager::instance().is_finalized();
}

/**
 * @brief A convenience function to initialize the application.
 *
 * This is the recommended entry point for starting the application lifecycle.
 */
inline void InitializeApp(std::source_location loc = std::source_location::current())
{
    LifecycleManager::instance().initialize(loc);
}

/**
 * @brief A convenience function to finalize the application.
 *
 * This is the recommended entry point for shutting down the application lifecycle.
 */
inline void FinalizeApp(std::source_location loc = std::source_location::current())
{
    LifecycleManager::instance().finalize(loc);
}

/**
 * @brief A convenience function to register a dynamic module.
 * @details This function is a wrapper around
 * `LifecycleManager::instance().register_dynamic_module()`. Dynamic modules can be registered at
 * runtime after the static core of the application has been initialized. This call is only valid
 * *after* `InitializeApp()` has been called.
 * @see LifecycleManager::register_dynamic_module
 *
 * @param module_def A ModuleDef object, which will be moved into the manager.
 * @return `true` on successful registration, `false` otherwise.
 */
inline bool RegisterDynamicModule(ModuleDef &&module_def)
{
    return LifecycleManager::instance().register_dynamic_module(std::move(module_def));
}

/**
 * @brief A convenience function to load a dynamic module.
 * @see LifecycleManager::load_module
 *
 * @param name The name of the module to load.
 * @return `true` if the module was loaded successfully, `false` otherwise.
 */
inline bool LoadModule(const char *name, std::source_location loc = std::source_location::current())
{
    return LifecycleManager::instance().load_module(name, loc);
}

/**
 * @brief A convenience function to unload a dynamic module.
 * @see LifecycleManager::unload_module
 *
 * @param name The name of the module to unload.
 * @return `true` if the module was considered for unloading.
 */
inline bool UnloadModule(const char *name,
                         std::source_location loc = std::source_location::current())
{
    return LifecycleManager::instance().unload_module(name, loc);
}

class LifecycleGuard
{
  private:
    std::source_location m_loc;

  public:
    // Default: no modules provided. If this is the first guard, InitializeApp() is called.
    LifecycleGuard(std::source_location loc = std::source_location::current()) : m_loc(loc)
    {
        PLH_DEBUG(
            "[PLH_LifeCycle] LifecycleGuard constructed in function {} with no modules. ({}:{})",
            m_loc.function_name(), pylabhub::format_tools::filename_only(m_loc.file_name()),
            m_loc.line());
        init_owner_if_first({});
    }

    // Single module constructor
    // Usage: LifecycleGuard guard(ModuleDef("MyModule"));
    explicit LifecycleGuard(ModuleDef &&module,
                            std::source_location loc = std::source_location::current())
        : m_loc(loc)
    {
        PLH_DEBUG("[PLH_LifeCycle] LifecycleGuard constructed in function {}. ({}:{})",
                  m_loc.function_name(), pylabhub::format_tools::filename_only(m_loc.file_name()),
                  m_loc.line());
        std::vector<ModuleDef> modules;
        modules.emplace_back(std::move(module));
        init_owner_if_first(std::move(modules));
    }

    // Multiple modules constructor
    // Usage: LifecycleGuard guard(MakeModDefList(ModuleDef("Mod1"), ModuleDef("Mod2")));
    explicit LifecycleGuard(std::vector<ModuleDef> &&modules,
                            std::source_location loc = std::source_location::current())
        : m_loc(loc)
    {
        PLH_DEBUG("[PLH_LifeCycle] LifecycleGuard constructed in function {}. ({}:{})",
                  m_loc.function_name(), pylabhub::format_tools::filename_only(m_loc.file_name()),
                  m_loc.line());
        init_owner_if_first(std::move(modules));
    }

    // Non-copyable, non-movable
    LifecycleGuard(const LifecycleGuard &) = delete;
    LifecycleGuard &operator=(const LifecycleGuard &) = delete;
    LifecycleGuard(LifecycleGuard &&) = delete;
    LifecycleGuard &operator=(LifecycleGuard &&) = delete;

    /**
     * @brief Destructor that finalizes the application if this guard is the owner.
     *
     * @warning The C++ standard does not guarantee the destruction order of static
     *          objects across different translation units. If you have a static
     *          object whose destructor depends on a lifecycle-managed service
     *          (like a logger), it may be destroyed *after* the main `LifecycleGuard`
     *          has already shut down those services.
     *
     *          **Recommendation**: Avoid designs that rely on the destructors of
     *          static objects to interact with lifecycle services. Instead, manage
     *          such objects' lifecycles explicitly or ensure they are cleaned up
     *          before the `LifecycleGuard` goes out of scope.
     */
    ~LifecycleGuard() noexcept
    {
        if (m_is_owner)
        {
            PLH_DEBUG("[PLH_LifeCycle] LifecycleGuard is being destructed as owner. "
                      "Constructor was located in function {}. ({}:{}) ",
                      m_loc.function_name(),
                      pylabhub::format_tools::filename_only(m_loc.file_name()), m_loc.line());
            pylabhub::utils::FinalizeApp(m_loc);
        }
    }

  private:
    // Function-local static atomic flag (ODR-safe header-only)
    static std::atomic_bool &owner_flag()
    {
        static std::atomic_bool flag{false};
        return flag;
    }

    // Core logic: attempt to become owner; if successful, register modules and always initialize.
    void init_owner_if_first(std::vector<ModuleDef> &&modules)
    {
        bool expected = false;
        if (owner_flag().compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            // This guard is the owner
            m_is_owner = true;

            // Register modules (move each into manager). If modules is empty, this loop is skipped.
            for (auto &m : modules)
            {
                pylabhub::utils::RegisterModule(std::move(m));
            }

            // IMPORTANT: always initialize now, even if no modules were supplied.
            // This guarantees the lifecycle starts as soon as the first guard is established.
            pylabhub::utils::InitializeApp(m_loc);
        }
        else
        {
            // Not the owner: warn and ignore supplied modules.
            m_is_owner = false;
            const auto app_name = pylabhub::platform::get_executable_name();
            const auto pid = pylabhub::platform::get_pid();
            PLH_DEBUG(
                "[PLH_LifeCycle] [{}:{}] WARNING: LifecycleGuard constructed but an "
                "owner already exists. This guard is a no-op; provided modules (if any) "
                "were ignored.\n[PLH_Lifecycle] Constructor was located in function {}. ({}:{}).",
                app_name, pid, m_loc.function_name(),
                pylabhub::format_tools::filename_only(m_loc.file_name()), m_loc.line());
            pylabhub::debug::print_stack_trace();
        }
    }

    bool m_is_owner{false};
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
