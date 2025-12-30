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
#include "platform.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>


using namespace pylabhub::platform;

// ============================================================================
// Internal C++ Definitions (Hidden from Public API)
// ============================================================================

namespace
{
// These are the internal C++ representations of the module definitions.
// They use STL types and are hidden from the ABI by the Pimpl idiom.
struct InternalModuleShutdownDef
{
    std::function<void()> func;
    std::chrono::milliseconds timeout;
};

struct InternalModuleDef
{
    std::string name;
    std::vector<std::string> dependencies;
    std::function<void()> startup;
    InternalModuleShutdownDef shutdown;
};
} // namespace

// ============================================================================
// ModuleDef Class Implementation (Pimpl Forwarding)
// ============================================================================

// The concrete definition of the ModuleDefImpl struct. This is the private
// implementation that holds all non-ABI-stable data.
class ModuleDefImpl
{
  public:
    InternalModuleDef def;
};

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

void ModuleDef::add_dependency(const char *dependency_name)
{
    if (pImpl && dependency_name)
    {
        pImpl->def.dependencies.emplace_back(dependency_name);
    }
}

void ModuleDef::set_startup(LifecycleCallback startup_func)
{
    if (pImpl)
    {
        pImpl->def.startup = startup_func;
    }
}

void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms)
{
    if (pImpl)
    {
        pImpl->def.shutdown.func = shutdown_func;
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
    LifecycleManagerImpl() : m_pid(get_pid()), m_app_name(get_executable_name()) {}

    // Internal graph node for topological sort.
    struct InternalGraphNode
    {
        std::string name;
        std::function<void()> startup;
        InternalModuleShutdownDef shutdown;

        int in_degree = 0;
        std::vector<InternalGraphNode *> dependents;
    };

    // Phase 1: Collect module definitions. This is thread-safe.
    void registerModule(InternalModuleDef module_def)
    {
        // Fail fast if registration is attempted after initialization has begun.
        if (m_is_initialized.load(std::memory_order_acquire))
        {
            fmt::print(stderr,
                       "[pylabhub-lifecycle] [{}:{}] FATAL: Attempted to register module '{}' after "
                       "initialization has started. Aborting.\n",
                       m_app_name, m_pid, module_def.name);
            std::abort();
        }
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        m_registered_modules.push_back(std::move(module_def));
    }

    // Phase 2, Part 1: Build the dependency graph.
    void initialize()
    {
        // Idempotency check: only run initialization once.
        if (m_is_initialized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Initializing application...\n", m_app_name,
                   m_pid);

        try
        {
            buildGraph();
            m_startup_order = topologicalSort();
        }
        catch (const std::runtime_error &e)
        {
            fmt::print(
                stderr,
                "[pylabhub-lifecycle] [{}:{}] FATAL: Lifecycle dependency error: {}. Aborting.\n",
                m_app_name, m_pid, e.what());
            std::abort();
        }

        // The shutdown order is the exact reverse of the startup order.
        m_shutdown_order = m_startup_order;
        std::reverse(m_shutdown_order.begin(), m_shutdown_order.end());

        // Log the determined startup sequence for diagnostics.
        fmt::print(stderr,
                   "[pylabhub-lifecycle] [{}:{}] Startup sequence determined for {} modules:\n",
                   m_app_name, m_pid, m_startup_order.size());
        for (size_t i = 0; i < m_startup_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}]   ({}/{}) -> {}\n", m_app_name, m_pid,
                       i + 1, m_startup_order.size(), m_startup_order[i]->name);
        }

        // Phase 2, Part 2: Execute startup callbacks in the sorted order.
        for (const auto *module : m_startup_order)
        {
            try
            {
                fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] -> Starting module: '{}'\n",
                           m_app_name, m_pid, module->name);
                if (module->startup)
                    module->startup();
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] FATAL: Module '{}' threw an exception during "
                           "startup: {}. Aborting.\n",
                           m_app_name, m_pid, module->name, e.what());
                std::abort();
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] FATAL: Module '{}' threw an unknown "
                           "exception during startup. Aborting.\n",
                           m_app_name, m_pid, module->name);
                std::abort();
            }
        }
        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Application initialization complete.\n",
                   m_app_name, m_pid);
    }

    void finalize()
    {
        // Idempotency check: only run finalization once.
        if (!m_is_initialized.load(std::memory_order_acquire) ||
            m_is_finalized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Finalizing application...\n", m_app_name,
                   m_pid);
        fmt::print(stderr,
                   "[pylabhub-lifecycle] [{}:{}] Shutdown sequence determined for {} modules:\n",
                   m_app_name, m_pid, m_shutdown_order.size());
        for (size_t i = 0; i < m_shutdown_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}]   ({}/{}) <- {}\n", m_app_name, m_pid,
                       i + 1, m_shutdown_order.size(), m_shutdown_order[i]->name);
        }

        // Execute shutdown callbacks in reverse startup order.
        for (const auto *module : m_shutdown_order)
        {
            try
            {
                if (module->shutdown.func)
                {
                    fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] <- Shutting down module: '{}'\n",
                               m_app_name, m_pid, module->name);

                    // Run the shutdown function asynchronously to handle timeouts.
                    std::future<void> future =
                        std::async(std::launch::async, module->shutdown.func);
                    auto status = future.wait_for(module->shutdown.timeout);

                    if (status == std::future_status::timeout)
                    {
                        fmt::print(stderr,
                                   "[pylabhub-lifecycle] [{}:{}] WARNING: Shutdown for module '{}' "
                                   "timed out after {}ms.\n",
                                   m_app_name, m_pid, module->name,
                                   module->shutdown.timeout.count());
                    }
                    else
                    {
                        future.get(); // Re-throws any exception from the shutdown function.
                    }
                }
            }
            catch (const std::exception &e)
            {
                // Exceptions during shutdown are logged but are not fatal.
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an exception during "
                           "shutdown: {}\n",
                           m_app_name, m_pid, module->name, e.what());
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an unknown "
                           "exception during shutdown.\n",
                           m_app_name, m_pid, module->name);
            }
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Application finalization complete.\n",
                   m_app_name, m_pid);
    }

  private:
    // Converts the flat list of registered modules into a graph structure.
    void buildGraph()
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);

        // First pass: create all nodes in the graph map.
        for (const auto &mod_def : m_registered_modules)
        {
            if (m_module_graph.count(mod_def.name))
            {
                throw std::runtime_error("Duplicate module name detected: '" + mod_def.name +
                                         "'.");
            }
            m_module_graph[mod_def.name] = {mod_def.name, mod_def.startup,
                                            mod_def.shutdown, 0, {}};
        }

        // Second pass: connect dependencies and calculate in-degrees.
        for (const auto &mod_def : m_registered_modules)
        {
            InternalGraphNode &module_node = m_module_graph.at(mod_def.name);
            module_node.in_degree = mod_def.dependencies.size();

            for (const auto &dep_name : mod_def.dependencies)
            {
                auto it = m_module_graph.find(dep_name);
                if (it == m_module_graph.end())
                {
                    throw std::runtime_error("Module '" + mod_def.name +
                                             "' has an undefined dependency: '" + dep_name + "'.");
                }
                // `it->second` is the dependency. This module (`module_node`) depends on it,
                // so this module is a "dependent" of the dependency.
                it->second.dependents.push_back(&module_node);
            }
        }
        // The temporary registration list is no longer needed.
        m_registered_modules.clear();
    }

    // Sorts the graph nodes using Kahn's algorithm for topological sorting.
    std::vector<InternalGraphNode *> topologicalSort()
    {
        std::vector<InternalGraphNode *> sorted_order;
        sorted_order.reserve(m_module_graph.size());
        std::vector<InternalGraphNode *> queue; // The queue of nodes with in-degree 0.

        // Initialize the queue with all nodes that have no dependencies.
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
            InternalGraphNode *u = queue[head++];
            sorted_order.push_back(u);

            // For each dependent of the current node, decrement its in-degree.
            for (InternalGraphNode *v : u->dependents)
            {
                if (--(v->in_degree) == 0)
                {
                    // If a dependent's in-degree drops to 0, it's now ready.
                    queue.push_back(v);
                }
            }
        }

        // If the sorted order contains fewer nodes than the graph, there's a cycle.
        if (sorted_order.size() != m_module_graph.size())
        {
            // Find a node that's part of the cycle to include in the error message.
            std::string cycle_node_name;
            for (const auto &pair : m_module_graph)
            {
                if (pair.second.in_degree > 0)
                {
                    cycle_node_name = pair.first;
                    break;
                }
            }
            throw std::runtime_error("Circular dependency detected in modules. Module '" +
                                     cycle_node_name + "' is part of a cycle.");
        }
        return sorted_order;
    }

    // Diagnostic info
    const long m_pid;
    const std::string m_app_name;

    // State flags
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};

    // Thread-safety for registration
    std::mutex m_registry_mutex;

    // Data structures for building the graph
    std::vector<InternalModuleDef> m_registered_modules;
    std::map<std::string, InternalGraphNode> m_module_graph;
    std::vector<InternalGraphNode *> m_startup_order;
    std::vector<InternalGraphNode *> m_shutdown_order;
};

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
        return;
    pImpl->registerModule(std::move(module_def.pImpl->def));
}

void LifecycleManager::initialize() { pImpl->initialize(); }

void LifecycleManager::finalize() { pImpl->finalize(); }