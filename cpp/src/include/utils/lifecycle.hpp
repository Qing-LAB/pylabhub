#pragma once

/*******************************************************************************
 * @file lifecycle.hpp
 * @brief Manages application startup and shutdown with dependency-aware modules.
 *
 * @see src/utils/lifecycle.cpp
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
 * #include "utils/lifecycle.hpp"
 * #include "utils/logger.hpp"
 * #include "utils/file_lock.hpp"
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
 * @brief Severity levels for lifecycle manager internal log messages.
 *
 * Used by `LifecycleLogSink` callbacks installed via `SetLifecycleLogSink()`.
 * The values are intentionally sparse to allow future levels without ABI breakage.
 */
enum class LifecycleLogLevel : int
{
    Debug = 1, ///< Verbose/diagnostic messages.
    Warn  = 3, ///< Warnings: recoverable policy violations or unexpected states.
    Error = 4  ///< Errors: unload failures, contamination, destabilization.
};

/**
 * @brief Callback type for the lifecycle log sink.
 *
 * Install one via `SetLifecycleLogSink()`. When set, the lifecycle manager
 * routes its runtime log messages through this function instead of PLH_DEBUG.
 * The callback must be thread-safe (it may be invoked from the shutdown thread).
 */
using LifecycleLogSink = std::function<void(LifecycleLogLevel, const std::string &)>;

/**
 * @brief Runtime state of a dynamic module.
 *
 * Returned by `LifecycleManager::dynamic_module_state()` to allow callers to
 * inspect or poll the lifecycle of a dynamic module without blocking.
 */
enum class DynModuleState : int
{
    NotRegistered,  ///< Module not found in the manager (never registered, or already removed).
    Unloaded,       ///< Registered but never loaded (or successfully removed from graph).
    Loading,        ///< Startup callback is currently running.
    Loaded,         ///< Fully loaded and running.
    Unloading,      ///< Async shutdown is in progress.
    ShutdownTimeout, ///< Shutdown timed out; thread was detached. Module may be destabilized.
    ShutdownFailed  ///< Shutdown threw an exception. Module may be destabilized.
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
    [[nodiscard]] bool is_initialized();

    /**
     * @brief Checks if the lifecycle manager has been finalized.
     *
     * This allows callers to verify if `finalize()` has completed.
     *
     * @return `true` if `finalize()` has been called, `false` otherwise.
     */
    [[nodiscard]] bool is_finalized();

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
    [[nodiscard]] bool register_dynamic_module(ModuleDef &&module_def);

    /**
     * @brief Loads a dynamic module and its dependencies.
     *
     * This function is thread-safe and idempotent. If the module is already
     * loaded, it immediately returns true. If not, it recursively loads all
     * dependencies and then runs the module's startup callback. This function must
     * not be called from within another module's startup or shutdown callback.
     *
     * @note The module's internal reference count is managed automatically. This
     *       count tracks how many other *loaded dynamic modules* depend on it.
     *       A direct call to `load_module` does not increment this count; it
     *       simply ensures the module is in the `LOADED` state.
     *
     * @param name Name of the module to load. At most `ModuleDef::MAX_MODULE_NAME_LEN`
     *             characters; empty name returns `false`.
     * @return `true` if the module was loaded successfully, `false` otherwise.
     */
    [[nodiscard]] bool load_module(std::string_view name, std::source_location loc);

    /**
     * @brief Schedules an async unload of a dynamic module and its closure.
     *
     * This function is thread-safe and idempotent. It will only succeed if no
     * other loaded dynamic modules depend on the target module (i.e., its
     * internal reference count is zero). A successful call performs:
     * 1.  Marks the full transitive closure of modules upfront (preventing new loads).
     * 2.  Enqueues them for the dedicated shutdown thread.
     * 3.  Returns immediately — the shutdown callbacks run asynchronously.
     *
     * Use `wait_for_unload()` to synchronise when you need to know the shutdown
     * callbacks have actually completed.
     *
     * To reload a module that has been unloaded, it must be registered again via
     * `register_dynamic_module()`. Must not be called from within a callback.
     *
     * @param name Name of the module to unload. At most `ModuleDef::MAX_MODULE_NAME_LEN`
     *             characters; empty name returns `false`.
     * @return `true` if the unload was scheduled (or the module was already not loaded).
     *         `false` if the module is still in use by another loaded module.
     */
    [[nodiscard]] bool unload_module(std::string_view name, std::source_location loc);

    /**
     * @brief Blocks until the unload triggered by `unload_module(name)` completes.
     *
     * `unload_module()` is asynchronous. Call this when you need to be sure the
     * shutdown callbacks for `name` **and its entire dependency closure** have
     * finished before proceeding (e.g. before checking side-effects in tests, or
     * before re-registering the module).
     *
     * Returns immediately if the module was never scheduled for unload, has already
     * finished unloading, or if `name` is empty.
     *
     * @param name    Root module name passed to `unload_module()`.
     * @param timeout Maximum time to wait.  `{}` (zero duration) means wait
     *                indefinitely.
     * @return The final `DynModuleState` of the module after the wait:
     *         - `NotRegistered` — module was removed from the graph (successful unload)
     *           or name was empty / never registered.
     *         - `Unloading`     — timeout expired; async shutdown is still in progress.
     *         - `ShutdownTimeout` / `ShutdownFailed` — shutdown completed but failed;
     *           module remains in the graph in a contaminated state.
     */
    DynModuleState wait_for_unload(std::string_view name,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{});

    /**
     * @brief Returns the current runtime state of a dynamic module.
     *
     * Thread-safe snapshot.  Returns `DynModuleState::NotRegistered` for unknown
     * names, empty names, or static modules.
     *
     * @param name Module name (at most `ModuleDef::MAX_MODULE_NAME_LEN` characters).
     */
    DynModuleState dynamic_module_state(std::string_view name);

    /**
     * @brief Installs a log sink for lifecycle internal messages.
     *
     * Once installed, the lifecycle manager routes its runtime log messages
     * through `sink` instead of PLH_DEBUG. The sink is invoked from whatever
     * thread generates the message (including the dedicated async shutdown
     * thread), so it must be thread-safe.
     *
     * Passing an empty/null `LifecycleLogSink` is equivalent to calling
     * `clear_lifecycle_log_sink()`.
     *
     * Typical usage: call from the logger module's startup callback so that
     * lifecycle events appear in the application log.
     *
     * @param sink Callable matching `LifecycleLogSink`; replaces any prior sink.
     */
    void set_lifecycle_log_sink(LifecycleLogSink sink);

    /**
     * @brief Removes the installed log sink. Lifecycle messages fall back to PLH_DEBUG.
     *
     * Call from the logger module's shutdown callback before the logger tears down.
     */
    void clear_lifecycle_log_sink() noexcept;

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
 * @brief Loads a dynamic module and its dependencies.
 * @see LifecycleManager::load_module
 * @param name Module name; at most `ModuleDef::MAX_MODULE_NAME_LEN` characters.
 * @return `true` if loaded successfully, `false` otherwise.
 */
[[nodiscard]] inline bool LoadModule(std::string_view name,
                                     std::source_location loc = std::source_location::current())
{
    return LifecycleManager::instance().load_module(name, loc);
}

/**
 * @brief Schedules an async unload of a dynamic module and its dependency closure.
 * @see LifecycleManager::unload_module
 *
 * Returns immediately — use `WaitForUnload()` to synchronise on completion.
 *
 * @param name Module name; at most `ModuleDef::MAX_MODULE_NAME_LEN` characters.
 * @return `true` if the unload was scheduled, `false` if the module is still in use.
 */
[[nodiscard]] inline bool UnloadModule(std::string_view name,
                                       std::source_location loc = std::source_location::current())
{
    return LifecycleManager::instance().unload_module(name, loc);
}

/**
 * @brief Blocks until the unload of `name` (and its full closure) completes.
 * @see LifecycleManager::wait_for_unload
 *
 * @param name    Root module name passed to `UnloadModule()`.
 * @param timeout Maximum wait time.  `{}` means wait indefinitely.
 * @return The final `DynModuleState` of the module:
 *         `NotRegistered` on successful removal, `Unloading` on timeout,
 *         `ShutdownTimeout`/`ShutdownFailed` if the shutdown callback failed.
 */
inline DynModuleState WaitForUnload(std::string_view name,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds{})
{
    return LifecycleManager::instance().wait_for_unload(name, timeout);
}

/**
 * @brief Returns the current runtime state of a dynamic module (non-blocking).
 * @see LifecycleManager::dynamic_module_state
 * @param name Module name; at most `ModuleDef::MAX_MODULE_NAME_LEN` characters.
 */
inline DynModuleState GetDynamicModuleState(std::string_view name)
{
    return LifecycleManager::instance().dynamic_module_state(name);
}

/**
 * @brief Installs a lifecycle log sink so internal messages route through the application logger.
 * @see LifecycleManager::set_lifecycle_log_sink
 */
inline void SetLifecycleLogSink(LifecycleLogSink sink)
{
    LifecycleManager::instance().set_lifecycle_log_sink(std::move(sink));
}

/**
 * @brief Removes the installed lifecycle log sink. Messages fall back to PLH_DEBUG.
 * @see LifecycleManager::clear_lifecycle_log_sink
 */
inline void ClearLifecycleLogSink() noexcept
{
    LifecycleManager::instance().clear_lifecycle_log_sink();
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
