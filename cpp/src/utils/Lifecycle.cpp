#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib> // For std::abort
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>

namespace
{

struct InternalModule; // Forward declaration

// The internal representation of a module used for building the dependency graph.
struct InternalModule
{
    std::string name;
    std::function<void()> startup;
    pylabhub::utils::ModuleShutdown shutdown;

    // ---
    // Graph properties
    // ---
    // The number of modules this one depends on.
    int in_degree = 0;
    // A list of modules that depend on this one.
    std::vector<InternalModule *> dependents;
};

class LifecycleManager
{
public:
    static LifecycleManager &instance()
    {
        static LifecycleManager inst;
        return inst;
    }

    LifecycleManager(const LifecycleManager &) = delete;
    LifecycleManager &operator=(const LifecycleManager &) = delete;

    void registerModule(pylabhub::utils::Module module)
    {
        if (m_is_initialized.load(std::memory_order_acquire))
        {
            fmt::print(stderr,
                       "[pylabhub-lifecycle] FATAL: Attempted to register module '{}' after "
                       "initialization has started. Aborting.\n",
                       module.name);
            std::abort();
        }
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        m_registered_modules.push_back(std::move(module));
    }

    void initialize()
    {
        if (m_is_initialized.exchange(true, std::memory_order_acq_rel))
        {
            return; // Idempotent
        }

        fmt::print(stderr, "[pylabhub-lifecycle] Initializing application...\n");

        try
        {
            buildGraph();
            m_startup_order = topologicalSort();
        }
        catch (const std::runtime_error &e)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] FATAL: Lifecycle dependency error: {}. Aborting.\n",
                       e.what());
            std::abort();
        }

        m_shutdown_order = m_startup_order;
        std::reverse(m_shutdown_order.begin(), m_shutdown_order.end());

        fmt::print(stderr,
                   "[pylabhub-lifecycle] Startup sequence determined for {} modules:\n",
                   m_startup_order.size());
        for (size_t i = 0; i < m_startup_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle]   ({}/{}) -> {}\n", i + 1,
                       m_startup_order.size(), m_startup_order[i]->name);
        }

        for (const auto *module : m_startup_order)
        {
            try
            {
                fmt::print(stderr, "[pylabhub-lifecycle] -> Starting module: '{}'\n", module->name);
                module->startup();
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] FATAL: Module '{}' threw an exception during "
                           "startup: {}. Aborting.\n",
                           module->name, e.what());
                std::abort();
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] FATAL: Module '{}' threw an unknown exception "
                           "during startup. Aborting.\n",
                           module->name);
                std::abort();
            }
        }
        fmt::print(stderr, "[pylabhub-lifecycle] Application initialization complete.\n");
    }

    void finalize()
    {
        if (!m_is_initialized.load(std::memory_order_acquire) ||
            m_is_finalized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        fmt::print(stderr, "[pylabhub-lifecycle] Finalizing application...\n");
        fmt::print(stderr,
                   "[pylabhub-lifecycle] Shutdown sequence determined for {} modules:\n",
                   m_shutdown_order.size());
        for (size_t i = 0; i < m_shutdown_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle]   ({}/{}) <- {}\n", i + 1,
                       m_shutdown_order.size(), m_shutdown_order[i]->name);
        }

        for (const auto *module : m_shutdown_order)
        {
            try
            {
                fmt::print(stderr, "[pylabhub-lifecycle] <- Shutting down module: '{}'\n",
                           module->name);
                std::future<void> future = std::async(std::launch::async, module->shutdown.func);
                auto status = future.wait_for(module->shutdown.timeout);

                if (status == std::future_status::timeout)
                {
                    fmt::print(stderr,
                               "[pylabhub-lifecycle] WARNING: Shutdown function for module '{}' "
                               "timed out after {}ms.\n",
                               module->name, module->shutdown.timeout.count());
                }
                else
                {
                    future.get(); // re-throws exceptions
                }
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] ERROR: Module '{}' threw an exception during "
                           "shutdown: {}\n",
                           module->name, e.what());
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] ERROR: Module '{}' threw an unknown exception "
                           "during shutdown.\n",
                           module->name);
            }
        }

        fmt::print(stderr, "[pylabhub-lifecycle] Application finalization complete.\n");
    }

private:
    LifecycleManager() = default;
    ~LifecycleManager() = default;

    void buildGraph()
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);

        // 1. Create nodes for all registered modules
        for (const auto &mod_def : m_registered_modules)
        {
            if (m_module_graph.count(mod_def.name))
            {
                throw std::runtime_error("Duplicate module name detected: '" + mod_def.name + "'.");
            }
            m_module_graph[mod_def.name] = {mod_def.name, mod_def.startup, mod_def.shutdown};
        }

        // 2. Link dependencies and calculate in-degrees
        for (const auto &mod_def : m_registered_modules)
        {
            InternalModule &module = m_module_graph.at(mod_def.name);
            module.in_degree = mod_def.dependencies.size();

            for (const auto &dep_name : mod_def.dependencies)
            {
                auto it = m_module_graph.find(dep_name);
                if (it == m_module_graph.end())
                {
                    throw std::runtime_error("Module '" + mod_def.name +
                                             "' has an undefined dependency: '" + dep_name + "'.");
                }
                // Add 'module' to the list of dependents for 'dep_name'
                it->second.dependents.push_back(&module);
            }
        }
        // Original registration list no longer needed
        m_registered_modules.clear();
    }

    std::vector<InternalModule *> topologicalSort()
    {
        std::vector<InternalModule *> sorted_order;
        sorted_order.reserve(m_module_graph.size());
        std::vector<InternalModule *> queue;

        // 1. Find all nodes with in-degree 0 and add them to the queue
        for (auto &pair : m_module_graph)
        {
            if (pair.second.in_degree == 0)
            {
                queue.push_back(&pair.second);
            }
        }

        // 2. Process the queue (Kahn's algorithm)
        size_t head = 0;
        while (head < queue.size())
        {
            InternalModule *u = queue[head++];
            sorted_order.push_back(u);

            for (InternalModule *v : u->dependents)
            {
                v->in_degree--;
                if (v->in_degree == 0)
                {
                    queue.push_back(v);
                }
            }
        }

        // 3. Check for cycles
        if (sorted_order.size() != m_module_graph.size())
        {
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

    // ---
    // State
    // ---
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};
    std::mutex m_registry_mutex;

    // ---
    // Data for lifecycle management
    // ---
    std::vector<pylabhub::utils::Module> m_registered_modules;
    std::map<std::string, InternalModule> m_module_graph;
    std::vector<InternalModule *> m_startup_order;
    std::vector<InternalModule *> m_shutdown_order;
};

} // namespace

namespace pylabhub::utils
{

void RegisterModule(Module module)
{
    LifecycleManager::instance().registerModule(std::move(module));
}

void InitializeApplication()
{
    LifecycleManager::instance().initialize();
}

void FinalizeApplication()
{
    LifecycleManager::instance().finalize();
}

} // namespace pylabhub::utils