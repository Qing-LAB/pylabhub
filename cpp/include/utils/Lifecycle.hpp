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
 * Modules are typically registered at static initialization time, and the
 * application's `main` function is responsible for invoking the initialization
 * and finalization routines.
 *
 * ```cpp
 * // ---- in core/Database.cpp ----
 * #include "utils/Lifecycle.hpp"
 *
 * void connect_to_db() { ... }
 * void disconnect_from_db() { ... }
 *
 * // At static initialization, this object will register its module.
 * static struct DatabaseModule {
 *     DatabaseModule() {
 *         // Define the "Database" module.
 *         pylabhub::utils::ModuleDef module("Database");
 *         // It depends on the Logger, so Logger will start first.
 *         module.add_dependency("Logger");
 *         module.set_startup(&connect_to_db);
 *         module.set_shutdown(&disconnect_from_db, 2000); // 2s timeout
 *
 *         // Register it with the central manager.
 *         pylabhub::lifecycle::RegisterModule(std::move(module));
 *     }
 * } g_db_module_registrar;
 *
 * // ---- in main.cpp ----
 * #include "utils/Lifecycle.hpp"
 *
 * int main(int argc, char* argv[]) {
 *     // All modules are now registered. Initialize the application.
 *     // This will run all startup callbacks in the correct order.
 *     pylabhub::lifecycle::InitializeApp();
 *
 *     // ... main application logic runs here ...
 *
 *     // Before exiting, finalize the application.
 *     // This will run all shutdown callbacks in reverse order.
 *     pylabhub::lifecycle::FinalizeApp();
 *
 *     return 0;
 * }
 * ```
 ******************************************************************************/

#include <memory> // For std::unique_ptr

#include "pylabhub_utils_export.h"

// Disable warning C4251 on MSVC. This is a common practice for exported classes
// that use std::unique_ptr to a forward-declared (incomplete) type (Pimpl).
// The user of the class cannot access the member, so it's safe.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

// Forward-declare the implementation structs to hide them from the public API,
// which is the essence of the Pimpl idiom.
class ModuleDefImpl;
class LifecycleManagerImpl;

/**
 * @brief A function pointer type for module startup and shutdown callbacks.
 *
 * Using a C-style function pointer is essential for ABI stability, as it has a
 * standardized calling convention, unlike `std::function`.
 */
typedef void (*LifecycleCallback)();

/**
 * @class ModuleDef
 * @brief An ABI-safe object for defining a lifecycle module's properties.
 *
 * This class acts as a builder for a module definition. It uses the Pimpl idiom
 * to hide its internal `std::string` and `std::vector` members, making it safe
 * to create and pass this object across shared library boundaries.
 *
 * It is movable but not copyable, enforcing a clear ownership model. Once a
 * `ModuleDef` is registered with the `LifecycleManager`, ownership is transferred.
 */
class PYLABHUB_UTILS_EXPORT ModuleDef
{
  public:
    /**
     * @brief Constructs a module definition with a given name.
     * @param name The unique name for this module (e.g., "Logger", "Database").
     *             This name is used for dependency resolution. Must not be null.
     */
    explicit ModuleDef(const char *name);

    /// @brief Destructor. Must be defined in the .cpp file for the Pimpl idiom.
    ~ModuleDef();

    // --- Rule of Five: Movable, but not Copyable ---
    // The move constructor and assignment operators are defaulted in the .cpp file
    // to ensure they are generated where ModuleDefImpl is a complete type.
    ModuleDef(ModuleDef &&other) noexcept;
    ModuleDef &operator=(ModuleDef &&other) noexcept;
    ModuleDef(const ModuleDef &) = delete;
    ModuleDef &operator=(const ModuleDef &) = delete;

    /**
     * @brief Adds a dependency to this module.
     * @param dependency_name The name of the module that this module depends on.
     *                        The manager will ensure that the dependency is
     *                        started before this module.
     */
    void add_dependency(const char *dependency_name);

    /**
     * @brief Sets the startup callback for this module.
     * @param startup_func A C-style function pointer to be called on startup.
     *                     This function should contain the module's initialization logic.
     */
    void set_startup(LifecycleCallback startup_func);

    /**
     * @brief Sets the shutdown callback for this module.
     * @param shutdown_func A C-style function pointer to be called on shutdown.
     *                      This function should contain the module's cleanup logic.
     * @param timeout_ms The time in milliseconds to wait for the shutdown function
     *                   to complete before it is considered timed out.
     */
    void set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms);

  private:
    // This friend declaration allows LifecycleManager to access the private pImpl
    // member to extract the module definition. This is a controlled way to
    // break encapsulation for the Pimpl pattern without exposing implementation
    // details publicly.
    friend class LifecycleManager;

    std::unique_ptr<ModuleDefImpl> pImpl;
};

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
     * `pylabhub::lifecycle::InitializeApp()` is called. Registration after
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
     * @note It is recommended to use the `pylabhub::lifecycle::InitializeApp()`
     *       helper function instead of calling this directly.
     */
    void initialize();

    /**
     * @brief Shuts down the application by executing module shutdown functions.
     *
     * This function should be called once at the end of the application's
     * lifecycle. It is idempotent. Shutdown proceeds in the reverse order of
     * initialization.
     *
     * @note It is recommended to use the `pylabhub::lifecycle::FinalizeApp()`
     *       helper function instead of calling this directly.
     */
    void finalize();

    // --- Rule of Five: Singleton, not Copyable or Assignable ---
    LifecycleManager(const LifecycleManager &) = delete;
    LifecycleManager &operator=(const LifecycleManager &) = delete;

  private:
    // Private constructor/destructor to enforce the singleton pattern.
    LifecycleManager();
    ~LifecycleManager();

    std::unique_ptr<LifecycleManagerImpl> pImpl;
};

namespace pylabhub {
namespace lifecycle {

/**
 * @brief A convenience function to register a module with the LifecycleManager.
 *
 * This is the preferred, type-safe way to register a module. It is a simple
 * inline wrapper around the `LifecycleManager::instance()` call.
 *
 * @param module_def A ModuleDef object, which will be moved into the manager.
 */
inline void RegisterModule(ModuleDef&& module_def)
{
    // The `::` prefix ensures we call the global LifecycleManager class.
    ::LifecycleManager::instance().register_module(std::move(module_def));
}

/**
 * @brief A convenience function to initialize the application.
 *
 * This is the recommended entry point for starting the application lifecycle.
 */
inline void InitializeApp()
{
    ::LifecycleManager::instance().initialize();
}

/**
 * @brief A convenience function to finalize the application.
 *
 * This is the recommended entry point for shutting down the application lifecycle.
 */
inline void FinalizeApp()
{
    ::LifecycleManager::instance().finalize();
}

} // namespace lifecycle
} // namespace pylabhub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif