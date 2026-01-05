/*******************************************************************************
 * @file Lifecycle.cpp
 * @brief Implementation of the dependency-aware application lifecycle manager.
 *
 * @see include/utils/Lifecycle.hpp
 *
 * **Implementation Details**
 *
 * This file contains the private implementation of the `LifecycleManager` and
 * `ModuleDef` classes. The design is centered around the Pimpl idiom to
 * provide a stable ABI.
 *
 * 1.  **Pimpl Classes (`ModuleDefImpl`, `LifecycleManagerImpl`)**:
 *     - These classes contain all the implementation details, including STL
 *       containers (`std::string`, `std::vector`, `std::map`), which are not
 *       ABI-stable. By hiding them from the header, we ensure binary
 *       compatibility for the library's users.
 *     - `InternalModuleDef` is the C++-native representation of a module,
 *       using `std::function` for callbacks and `std::chrono` for timeouts.
 *       The public `ModuleDef` class is a wrapper that forwards calls to it.
 *
 * 2.  **Two-Phase Initialization**:
 *     - **Phase 1: Registration**: Before `initialize()` is called, modules
 *       are registered and stored in a temporary `m_registered_modules` vector.
 *       Registration is thread-safe via `m_registry_mutex`. Attempting to
 *       register a module after initialization has started is a fatal error.
 *     - **Phase 2: Graph Building & Sorting**: When `initialize()` is first
 *       called, the manager builds a dependency graph from the registered
 *       modules and then performs a topological sort to determine the startup
 *       order. This ensures dependencies are met.
 *
 * 3.  **Topological Sort (Kahn's Algorithm)**:
 *     - The `topologicalSort` method implements Kahn's algorithm. It first
 *       identifies all nodes with an in-degree of zero (no dependencies) and
 *       adds them to a queue.
 *     - It then processes the queue, and for each processed node, it decrements
 *       the in-degree of its dependents. If a dependent's in-degree becomes
 *       zero, it is added to the queue.
 *     - If, after processing, the number of sorted nodes does not match the
 *       total number of nodes, a cycle has been detected, and the application
 *       aborts with an error.
 *
 * 4.  **Timed Shutdown**:
 *     - During `finalize()`, each module's shutdown callback is invoked via
 *       `std::async`.
 *     - `future.wait_for()` is used to honor the user-specified timeout. If a
 *       module's shutdown function hangs, the manager will print a warning and
 *       continue, preventing a single faulty module from deadlocking the entire
 *       application's termination.
 *     - Any exception thrown by a shutdown function is caught and logged, but
 *       does not halt the finalization of other modules.
 ******************************************************************************/
#include "utils/Lifecycle.hpp"
#include "debug_info.hpp"
#include "platform.hpp"
#include "utils/Logger.hpp" // For LOGGER_ERROR, etc.

#include <algorithm> // For std::reverse
#include <future>    // For std::async, std::future
#include <map>       // For std::map
#include <mutex>     // For std::mutex, std::lock_guard
#include <stdexcept> // For std::runtime_error, std::length_error
#include <string>    // For std::string
#include <vector>    // For std::vector

#include <fmt/core.h>   // For fmt::print
#include <fmt/ranges.h> // For fmt::join on vectors

// ============================================================================
// Internal C++ Definitions (Hidden from Public API)
// ============================================================================

namespace pylabhub::utils::lifecycle_internal
{
// These are the internal C++ representations of the module definitions.
// They use STL types and are hidden from the ABI by the Pimpl idiom.

// Internal representation of a module's shutdown behavior.
struct InternalModuleShutdownDef
{
    std::function<void()> func;
    std::chrono::milliseconds timeout;
};

// Internal, C++-native representation of a module definition.
// This uses std::function and std::string and is hidden from the ABI.
struct InternalModuleDef
{
    std::string name;
    std::vector<std::string> dependencies;
    std::function<void()> startup;
    InternalModuleShutdownDef shutdown;
};

} // namespace pylabhub::utils::lifecycle_internal

namespace pylabhub::utils
{

// ============================================================================
// ModuleDef Class Implementation (Pimpl Forwarding)
// ============================================================================

// The concrete definition of the ModuleDefImpl struct. This is the private
// implementation that holds all non-ABI-stable data.
class ModuleDefImpl
{
  public:
    lifecycle_internal::InternalModuleDef def;
};

/**
 * @brief Constructs a ModuleDef with the given name.
 * @param name The unique name for the module.
 */
ModuleDef::ModuleDef(const char *name) : pImpl(std::make_unique<ModuleDefImpl>())
{
    if (name)
    {
        pImpl->def.name = name;
    }
}

// The destructor and move operations MUST be defined in the .cpp file,
// where ModuleDefImpl is a complete type. Defining them in the header
// would expose the implementation details and break the Pimpl idiom.
ModuleDef::~ModuleDef() = default;
ModuleDef::ModuleDef(ModuleDef &&other) noexcept = default;
ModuleDef &ModuleDef::operator=(ModuleDef &&other) noexcept = default;

/**
 * @brief Adds a dependency to this module.
 * @param dependency_name The name of the module this module depends on.
 */
void ModuleDef::add_dependency(const char *dependency_name)
{
    if (pImpl && dependency_name)
    {
        pImpl->def.dependencies.emplace_back(dependency_name);
    }
}

/**
 * @brief Sets the startup callback for this module (no argument version).
 *
 * The internal lambda will call `startup_func(nullptr)`.
 * @param startup_func The callback to be called on startup.
 */
void ModuleDef::set_startup(LifecycleCallback startup_func)
{
    if (pImpl && startup_func)
    {
        // Store a lambda that calls the C-style function pointer with a null argument.
        pImpl->def.startup = [startup_func]() { startup_func(nullptr); };
    }
}

/**
 * @brief Sets the startup callback for this module (with string argument version).
 *
 * The `data` and `len` are used to safely construct an internal `std::string`,
 * which is then passed as a null-terminated C-string to `startup_func`.
 * @param startup_func The callback to be called on startup.
 * @param data A pointer to the string data.
 * @param len The length of the string data.
 * @throws std::length_error if `len` > `MAX_CALLBACK_PARAM_STRLEN`.
 */
void ModuleDef::set_startup(LifecycleCallback startup_func, const char *data, size_t len)
{
    if (pImpl && startup_func)
    {
        if (len > MAX_CALLBACK_PARAM_STRLEN)
        {
            throw std::length_error("Lifecycle: Startup argument length exceeds "
                                    "MAX_CALLBACK_PARAM_STRLEN.");
        }
        // Safely construct an std::string from data and length.
        std::string arg_str(data, len);
        // Store a lambda that captures the std::string and passes its c_str() to the callback.
        pImpl->def.startup = [startup_func, arg = std::move(arg_str)]()
        { startup_func(arg.c_str()); };
    }
}

/**
 * @brief Sets the shutdown callback for this module (no argument version).
 *
 * The internal lambda will call `shutdown_func(nullptr)`.
 * @param shutdown_func The callback to be called on shutdown.
 * @param timeout_ms The timeout in milliseconds for the shutdown function.
 */
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms)
{
    if (pImpl && shutdown_func)
    {
        // Store a lambda that calls the C-style function pointer with a null argument.
        pImpl->def.shutdown.func = [shutdown_func]() { shutdown_func(nullptr); };
        pImpl->def.shutdown.timeout = std::chrono::milliseconds(timeout_ms);
    }
}

/**
 * @brief Sets the shutdown callback for this module (with string argument version).
 *
 * The `data` and `len` are used to safely construct an internal `std::string`,
 * which is then passed as a null-terminated C-string to `shutdown_func`.
 * @param shutdown_func The callback to be called on shutdown.
 * @param timeout_ms The timeout in milliseconds for the shutdown function.
 * @param data A pointer to the string data.
 * @param len The length of the string data.
 * @throws std::length_error if `len` > `MAX_CALLBACK_PARAM_STRLEN`.
 */
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms,
                             const char *data, size_t len)
{
    if (pImpl && shutdown_func)
    {
        if (len > MAX_CALLBACK_PARAM_STRLEN)
        {
            throw std::length_error("Lifecycle: Shutdown argument length exceeds "
                                    "MAX_CALLBACK_PARAM_STRLEN.");
        }
        // Safely construct an std::string from data and length.
        std::string arg_str(data, len);
        // Store a lambda that captures the std::string and passes its c_str() to the callback.
        pImpl->def.shutdown.func = [shutdown_func, arg = std::move(arg_str)]()
        { shutdown_func(arg.c_str()); };
        pImpl->def.shutdown.timeout = std::chrono::milliseconds(timeout_ms);
    }
}

// ============================================================================
// LifecycleManager Class Implementation
// ============================================================================

// The concrete definition of the LifecycleManagerImpl struct. This holds all state.
class LifecycleManagerImpl
{
  public:
    LifecycleManagerImpl()
        : m_pid(pylabhub::platform::get_pid()),
          m_app_name(pylabhub::platform::get_executable_name())
    {
    }

    /**
     * @brief Represents the current status of a module during its lifecycle.
     */
    enum class ModuleStatus
    {
        Registered,   /// Module has been registered but not yet processed.
        Initializing, /// Module's startup function is currently being executed.
        Started,      /// Module has successfully started.
        Failed,       /// Module's startup failed or a dependency error occurred.
        Shutdown      /// Module has successfully shut down.
    };

    /**
     * @brief Internal graph node for topological sort and enhanced error reporting.
     *
     * Stores module details, its status, dependencies, and a list of modules
     * that depend on it (dependents).
     */
    struct InternalGraphNode
    {
        std::string name;
        std::function<void()> startup;
        lifecycle_internal::InternalModuleShutdownDef shutdown;
        std::vector<std::string> dependencies; // Original dependencies for printing

        size_t in_degree = 0;                           // Number of unresolved dependencies
        std::vector<InternalGraphNode *> dependents;    // Modules that depend on this one
        ModuleStatus status = ModuleStatus::Registered; // Current status of the module
    };

    // Phase 1: Collect module definitions. This is thread-safe.
    void registerModule(lifecycle_internal::InternalModuleDef module_def)
    {
        // Fail fast if registration is attempted after initialization has begun.
        if (m_is_initialized.load(std::memory_order_acquire))
        {
            PLH_PANIC("[pylabhub-lifecycle] [{}:{}] FATAL: Attempted to register module '{}' after "
                      "initialization has started. Aborting.",
                      m_app_name, m_pid, module_def.name);
        }
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        m_registered_modules.push_back(std::move(module_def));
    }

    // Phase 2: Initialize the application by building graph and executing startups.
    void initialize();

    // Phase 3: Finalize the application by executing shutdowns.
    void finalize();

  private: // Reverting to private
    /**
     * @brief Builds the internal dependency graph from registered modules.
     * @throws std::runtime_error on duplicate module names or undefined dependencies.
     */
    void buildGraph();

    /**
     * @brief Performs a topological sort to determine module startup order.
     * @return A vector of InternalGraphNode pointers in sorted order.
     * @throws std::runtime_error if a circular dependency is detected.
     */
    std::vector<InternalGraphNode *> topologicalSort();

    /**
     * @brief Prints detailed status of all modules and aborts the application.
     *
     * This function is called on fatal errors during initialization or dependency
     * resolution to provide comprehensive debugging information, including a
     * module graph and stack trace.
     *
     * @param error_msg The primary error message to display.
     * @param failed_module The name of the module that directly caused the failure (if any).
     */
    void printStatusAndAbort(const std::string &error_msg, const std::string &failed_module = "");

    // Diagnostic info (process ID and application name for logging context)
    const long m_pid;
    const std::string m_app_name;

    // State flags to control lifecycle flow (atomic for thread-safety)
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};

    // Mutex to protect access to m_registered_modules during concurrent registration
    std::mutex m_registry_mutex;

    // Data structures for building and managing the dependency graph
    std::vector<lifecycle_internal::InternalModuleDef>
        m_registered_modules; // Temporary store for initial definitions
    std::map<std::string, InternalGraphNode> m_module_graph; // The actual dependency graph
    std::vector<InternalGraphNode *> m_startup_order; // Modules in topological order for startup
    std::vector<InternalGraphNode *>
        m_shutdown_order; // Modules in reverse topological order for shutdown

  public: // Only this function should be public for LifecycleManager to access
    bool is_initialized() const { return m_is_initialized.load(std::memory_order_acquire); }
    bool is_finalized() const { return m_is_finalized.load(std::memory_order_acquire); }
};

/**
 * @brief Prints detailed status of all modules and aborts the application.
 *
 * This function is called on fatal errors during initialization or dependency
 * resolution to provide comprehensive debugging information, including a
 * module graph and stack trace.
 *
 * @param error_msg The primary error message to display.
 * @param failed_module The name of the module that directly caused the failure (if any).
 */
void LifecycleManagerImpl::printStatusAndAbort(const std::string &error_msg,
                                               const std::string &failed_module)
{
    fmt::print(stderr, "\n[pylabhub-lifecycle] [{}:{}] FATAL: {}. Aborting.\n", m_app_name, m_pid,
               error_msg);
    if (!failed_module.empty())
    {
        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Module '{}' was the point of failure.\n",
                   m_app_name, m_pid, failed_module);
    }

    fmt::print(stderr, "\n--- Module Dependency Graph and Status ---\n");
    // Sort modules by name for consistent output
    std::vector<InternalGraphNode *> sorted_nodes_for_print;
    for (auto const &[key, val] : m_module_graph)
    {
        sorted_nodes_for_print.push_back(const_cast<InternalGraphNode *>(&val));
    }
    std::sort(sorted_nodes_for_print.begin(), sorted_nodes_for_print.end(),
              [](const InternalGraphNode *a, const InternalGraphNode *b)
              { return a->name < b->name; });

    for (const auto *node_ptr : sorted_nodes_for_print)
    {
        const auto &node = *node_ptr;
        const char *status_str = "Unknown";
        switch (node.status)
        {
        case ModuleStatus::Registered:
            status_str = "Registered";
            break;
        case ModuleStatus::Initializing:
            status_str = "Initializing...";
            break;
        case ModuleStatus::Started:
            status_str = "Started (OK)";
            break;
        case ModuleStatus::Failed:
            status_str = "FAILED!!!";
            break;
        case ModuleStatus::Shutdown:
            status_str = "Shutdown (OK)";
            break; // Should not happen during init failures
        }

        fmt::print(stderr, "  - Module: '{}'\n", node.name);
        fmt::print(stderr, "    - Status: {}\n", status_str);
        if (!node.dependencies.empty())
        {
            fmt::print(stderr, "    - Depends on: [{}]\n", fmt::join(node.dependencies, ", "));
        }
        if (!node.dependents.empty())
        {
            std::vector<std::string> dependent_names;
            for (const auto *dep : node.dependents)
            {
                dependent_names.push_back(dep->name);
            }
            fmt::print(stderr, "    - Depended on by: [{}]\n", fmt::join(dependent_names, ", "));
        }
    }
    fmt::print(stderr, "----------------------------------------\n\n");

    pylabhub::debug::print_stack_trace();
    std::abort();
}

/**
 * @brief Initializes the application by building the dependency graph and
 *        executing module startup functions in topological order.
 *
 * Ensures idempotency, detects dependency issues, and provides detailed error
 * reporting and stack traces on fatal failures.
 */
void LifecycleManagerImpl::initialize()
{
    // Idempotency check: only run initialization once.
    if (m_is_initialized.exchange(true, std::memory_order_acq_rel))
    {
        PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Initialization already performed."
                  "Extra initialization should not be attempted. Nothing will be done and stack "
                  "trace printed for debugging.",
                  m_app_name, m_pid);
        print_stack_trace();
        return;
    }

    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Initializing application...", m_app_name, m_pid);

    try
    {
        buildGraph();
        m_startup_order = topologicalSort();
    }
    catch (const std::runtime_error &e)
    {
        // Catch graph-building or topological sort errors and abort with full status.
        printStatusAndAbort(e.what());
    }

    // The shutdown order is the exact reverse of the startup order.
    m_shutdown_order = m_startup_order;
    std::reverse(m_shutdown_order.begin(), m_shutdown_order.end());

    // Log the determined startup sequence for diagnostics.
    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Startup sequence determined for {} modules:",
              m_app_name, m_pid, m_startup_order.size());
    for (size_t i = 0; i < m_startup_order.size(); ++i)
    {
        PLH_DEBUG("[pylabhub-lifecycle] [{}:{}]   ({}/{}) -> {}", m_app_name, m_pid, i + 1,
                  m_startup_order.size(), m_startup_order[i]->name);
    }

    // Phase 2, Part 2: Execute startup callbacks in the sorted order.
    for (auto *module : m_startup_order)
    {
        try
        {
            PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] -> Starting module: '{}'", m_app_name, m_pid,
                      module->name);
            module->status = ModuleStatus::Initializing; // Mark module as currently initializing
            if (module->startup)
            {
                module->startup();
            }
            module->status = ModuleStatus::Started; // Mark module as successfully started
        }
        catch (const std::exception &e)
        {
            // Catch specific exceptions during startup, mark module as failed, and abort.
            module->status = ModuleStatus::Failed;
            printStatusAndAbort(
                fmt::format("Module threw an exception during startup: {}", e.what()),
                module->name);
        }
        catch (...)
        {
            // Catch unknown exceptions during startup, mark module as failed, and abort.
            module->status = ModuleStatus::Failed;
            printStatusAndAbort("Module threw an unknown exception during startup.", module->name);
        }
    }
    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Application initialization complete.", m_app_name,
              m_pid);
}

/**
 * @brief Shuts down the application by executing module shutdown functions
 *        in reverse topological order.
 *
 * Ensures idempotency and logs exceptions during shutdown. Shutdown failures
 * are logged with stack traces but are not fatal, allowing other modules to
 * finalize.
 */
void LifecycleManagerImpl::finalize()
{
    // Idempotency check: only run finalization once, and only if initialized.
    if (!m_is_initialized.load(std::memory_order_acquire) ||
        m_is_finalized.exchange(true, std::memory_order_acq_rel))
    {
        PLH_DEBUG(
            "[pylabhub-lifecycle] [{}:{}] Finalization already performed or "
            "initialization not done. Nothing will be done and stack trace printed for debugging.",
            m_app_name, m_pid);
        print_stack_trace();
        return;
    }

    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Finalizing application...", m_app_name, m_pid);
    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Shutdown sequence determined for {} modules:",
              m_app_name, m_pid, m_shutdown_order.size());
    for (size_t i = 0; i < m_shutdown_order.size(); ++i)
    {
        PLH_DEBUG("[pylabhub-lifecycle] [{}:{}]   ({}/{}) <- {}", m_app_name, m_pid, i + 1,
                  m_shutdown_order.size(), m_shutdown_order[i]->name);
    }

    // Execute shutdown callbacks in reverse startup order.
    for (auto *module : m_shutdown_order)
    {
        try
        {
            // Only attempt to shut down modules that successfully started.
            // (Modules that failed during startup are already marked as such).
            if (module->status == ModuleStatus::Started && module->shutdown.func)
            {
                PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] <- Shutting down module: '{}'", m_app_name,
                          m_pid, module->name);

                // Run the shutdown function asynchronously to handle timeouts.
                std::future<void> future = std::async(std::launch::async, module->shutdown.func);
                auto status = future.wait_for(module->shutdown.timeout);

                if (status == std::future_status::timeout)
                {
                    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] WARNING: Shutdown for module '{}' "
                              "timed out after {}ms. Continuing with other modules.",
                              m_app_name, m_pid, module->name, module->shutdown.timeout.count());
                    // Module status is left as Started or whatever it was if it timed out,
                    // as it's not a fatal error for others.
                }
                else
                {
                    future.get(); // Re-throws any exception from the shutdown function.
                    module->status = ModuleStatus::Shutdown; // Mark as successfully shut down
                }
            }
            else if (module->status == ModuleStatus::Registered ||
                     module->status == ModuleStatus::Initializing)
            {
                // Modules that were not started or were initializing should not attempt shutdown
                PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Module '{}' was not fully started (status: "
                          "{}) -> Skipping shutdown.",
                          m_app_name, m_pid, module->name,
                          static_cast<int>(module->status)); // Cast to int for printing enum value
            }
            // Modules with status Failed are ignored during shutdown.
        }
        catch (const std::exception &e)
        {
            // Exceptions during shutdown are logged with a stack trace but are not fatal.
            PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an exception during "
                      "shutdown: {}.",
                      m_app_name, m_pid, module->name, e.what());
            pylabhub::debug::print_stack_trace();
        }
        catch (...)
        {
            PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an unknown exception "
                      "during shutdown.",
                      m_app_name, m_pid, module->name);
            pylabhub::debug::print_stack_trace();
        }
    }

    PLH_DEBUG("[pylabhub-lifecycle] [{}:{}] Application finalization complete.", m_app_name, m_pid);
}

/**
 * @brief Converts the flat list of registered modules into a graph structure.
 *
 * This involves creating graph nodes for each module and establishing
 * dependency relationships.
 * @throws std::runtime_error on duplicate module names or undefined dependencies.
 */
void LifecycleManagerImpl::buildGraph()
{
    std::lock_guard<std::mutex> lock(m_registry_mutex);

    // First pass: create all nodes in the graph map.
    // Store original dependencies and set initial status.
    for (const auto &mod_def : m_registered_modules)
    {
        if (m_module_graph.count(mod_def.name))
        {
            throw std::runtime_error("Duplicate module name detected: '" + mod_def.name + "'");
        }
        m_module_graph[mod_def.name] = {mod_def.name,
                                        mod_def.startup,
                                        mod_def.shutdown,
                                        mod_def.dependencies, // Store original dependencies
                                        0,
                                        {},
                                        ModuleStatus::Registered};
    }

    // Second pass: connect dependencies and calculate in-degrees.
    for (const auto &mod_def : m_registered_modules)
    {
        InternalGraphNode &module_node = m_module_graph.at(mod_def.name);
        module_node.in_degree = mod_def.dependencies.size(); // Initial in-degree from explicit deps

        for (const auto &dep_name : mod_def.dependencies)
        {
            auto it = m_module_graph.find(dep_name);
            if (it == m_module_graph.end())
            {
                throw std::runtime_error("Module '" + mod_def.name +
                                         "' has an undefined dependency: '" + dep_name + "'");
            }
            // `it->second` is the dependency. This module (`module_node`) depends on it,
            // so this module is a "dependent" of the dependency.
            it->second.dependents.push_back(&module_node);
        }
    }
    // The temporary registration list is no longer needed after graph construction.
    m_registered_modules.clear();
}

/**
 * @brief Sorts the graph nodes using Kahn's algorithm for topological sorting.
 * @return A vector of InternalGraphNode pointers in sorted order,
 *         representing the correct startup sequence.
 * @throws std::runtime_error if a circular dependency is detected.
 */
std::vector<LifecycleManagerImpl::InternalGraphNode *> LifecycleManagerImpl::topologicalSort()
{
    std::vector<InternalGraphNode *> sorted_order;
    sorted_order.reserve(m_module_graph.size());
    std::vector<InternalGraphNode *> queue; // The queue of nodes with in-degree 0.

    // Initialize the queue with all nodes that have no dependencies (in-degree 0).
    for (auto &pair : m_module_graph)
    {
        if (pair.second.in_degree == 0)
        {
            queue.push_back(&pair.second);
        }
    }

    size_t head = 0;
    while (head < queue.size())
    {
        InternalGraphNode *u = queue[head++]; // Dequeue node 'u'
        sorted_order.push_back(u);

        // For each dependent 'v' of the current node 'u', decrement its in-degree.
        for (InternalGraphNode *v : u->dependents)
        {
            if (--(v->in_degree) == 0)
            {
                // If a dependent's in-degree drops to 0, it's now ready to be processed.
                queue.push_back(v);
            }
        }
    }

    // If the sorted order contains fewer nodes than the graph, there's a cycle.
    if (sorted_order.size() != m_module_graph.size())
    {
        // Collect all nodes that still have an in-degree > 0, indicating they are part of a cycle.
        std::vector<std::string> cycle_nodes;
        for (const auto &pair : m_module_graph)
        {
            if (pair.second.in_degree > 0)
            {
                cycle_nodes.push_back(pair.first);
            }
        }
        throw std::runtime_error("Circular dependency detected. Modules involved in cycle: " +
                                 fmt::format("{}", fmt::join(cycle_nodes, ", ")));
    }
    return sorted_order;
}

// Private constructor/destructor for the LifecycleManager (singleton pattern)
LifecycleManager::LifecycleManager() : pImpl(std::make_unique<LifecycleManagerImpl>()) {}
LifecycleManager::~LifecycleManager() = default;

// Public singleton accessor
LifecycleManager &LifecycleManager::instance()
{
    // Function-local static ensures thread-safe initialization on first call.
    static LifecycleManager instance;
    return instance;
}

// Public API methods forward to the implementation
void LifecycleManager::register_module(ModuleDef &&module_def)
{
    if (!module_def.pImpl)
        return; // Handle case where ModuleDef was moved from or not properly constructed.
    pImpl->registerModule(std::move(module_def.pImpl->def));
}

void LifecycleManager::initialize()
{
    pImpl->initialize();
}

void LifecycleManager::finalize()
{
    pImpl->finalize();
}

bool LifecycleManager::is_initialized()
{
    return pImpl->is_initialized();
}

bool LifecycleManager::is_finalized()
{
    return pImpl->is_finalized();
}

} // namespace pylabhub::utils