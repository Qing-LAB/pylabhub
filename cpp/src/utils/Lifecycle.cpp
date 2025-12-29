#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib> // For std::abort
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>

#include "platform.hpp"
#if defined(PLATFORM_WIN64)
#include <windows.h>
#elif defined(PLATFORM_APPLE)
#include <mach-o/dyld.h>
#include <limits.h> // For PATH_MAX
#include <unistd.h>
#else // Linux
#include <unistd.h>
#include <limits.h> // For PATH_MAX
#endif

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

// The concrete definition of the pImpl struct, which wraps the C++ module definition.
struct pylabhub_module_impl
{
    InternalModuleDef def;
};

// ============================================================================ 
// Public C++ API Implementation (pylabhub_module class)
// ============================================================================ 

pylabhub_module::pylabhub_module(const char *name) : pImpl(std::make_unique<pylabhub_module_impl>()) 
{
    if (name)
    {
        pImpl->def.name = name;
    }
}

// The destructor and move operations MUST be defined in the .cpp file,
// where pylabhub_module_impl is a complete type. Defining them in the header
// would expose the implementation details and break the Pimpl idiom.
pylabhub_module::~pylabhub_module() = default;
pylabhub_module::pylabhub_module(pylabhub_module &&other) noexcept = default;
pylabhub_module &pylabhub_module::operator=(pylabhub_module &&other) noexcept = default;

void pylabhub_module::add_dependency(const char *dependency_name)
{
    if (pImpl && dependency_name)
    {
        pImpl->def.dependencies.emplace_back(dependency_name);
    }
}

void pylabhub_module::set_startup(pylabhub_lifecycle_callback startup_func)
{
    if (pImpl)
    {
        pImpl->def.startup = startup_func;
    }
}

void pylabhub_module::set_shutdown(pylabhub_lifecycle_callback shutdown_func,
                                     unsigned int timeout_ms)
{
    if (pImpl)
    {
        pImpl->def.shutdown.func = shutdown_func;
        pImpl->def.shutdown.timeout = std::chrono::milliseconds(timeout_ms);
    }
}

// ============================================================================ 
// Internal LifecycleManager Implementation
// ============================================================================ 

namespace
{
// Anonymous namespace for helpers local to this translation unit.

// Helper to get the current process ID in a cross-platform way.
long get_pid()
{
#if defined(PLATFORM_WIN64)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

// Helper to automatically discover the current executable's name.
std::string get_executable_name()
{
    try
    {
#if defined(PLATFORM_WIN64)
        wchar_t path[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
        {
            return "unknown_win";
        }
        return std::filesystem::path(path).filename().string();
#elif defined(PLATFORM_LINUX)
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        if (count > 0)
        {
            return std::filesystem::path(std::string(result, count)).filename().string();
        }
        return "unknown_linux";
#elif defined(PLATFORM_APPLE)
        char path[PATH_MAX];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0)
        {
            return std::filesystem::path(path).filename().string();
        }
        return "unknown_apple";
#endif
    }
    catch (...)
    {
        // std::filesystem operations can throw on invalid paths.
    }
    return "unknown";
}
} // namespace

namespace pylabhub::utils {
// The LifecycleManager itself is in a named namespace to be accessible by the
// global API functions within this file, but is not exposed in any header.

struct InternalGraphNode; // Forward declaration

// The internal representation of a module used for building the dependency graph.
struct InternalGraphNode
{
    std::string name;
    std::function<void()> startup;
    InternalModuleShutdownDef shutdown;

    // ---
    // Graph properties
    // ---
    int in_degree = 0;
    std::vector<InternalGraphNode *> dependents;
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

    void registerModule(InternalModuleDef module_def)
    {
        if (m_is_initialized.load(std::memory_order_acquire))
        {
            fmt::print(stderr,
                       "[pylabhub-lifecycle] [{}:{}] FATAL: Attempted to register module '{}' after "
                       "initialization has started. Aborting.",
                       m_app_name, m_pid, module_def.name);
            std::abort();
        }
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        m_registered_modules.push_back(std::move(module_def));
    }

    void initialize()
    {
        if (m_is_initialized.exchange(true, std::memory_order_acq_rel))
        {
            return; // Idempotent
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Initializing application...\n", m_app_name, m_pid);

        try
        {
            buildGraph();
            m_startup_order = topologicalSort();
        }
        catch (const std::runtime_error &e)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] FATAL: Lifecycle dependency error: {}. Aborting.\n",
                       m_app_name, m_pid, e.what());
            std::abort();
        }

        m_shutdown_order = m_startup_order;
        std::reverse(m_shutdown_order.begin(), m_shutdown_order.end());

        fmt::print(stderr,
                   "[pylabhub-lifecycle] [{}:{}] Startup sequence determined for {} modules:\n",
                   m_app_name, m_pid, m_startup_order.size());
        for (size_t i = 0; i < m_startup_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}]   ({}/{}) -> {}\n", m_app_name, m_pid, i + 1,
                       m_startup_order.size(), m_startup_order[i]->name);
        }

        for (const auto *module : m_startup_order)
        {
            try
            {
                fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] -> Starting module: '{}'\n", m_app_name, m_pid, module->name);
                if (module->startup) module->startup();
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] FATAL: Module '{}' threw an exception during "
                           "startup: {}. Aborting.",
                           m_app_name, m_pid, module->name, e.what());
                std::abort();
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] FATAL: Module '{}' threw an unknown exception "
                           "during startup. Aborting.",
                           m_app_name, m_pid, module->name);
                std::abort();
            }
        }
        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Application initialization complete.\n", m_app_name, m_pid);
    }

    void finalize()
    {
        if (!m_is_initialized.load(std::memory_order_acquire) ||
            m_is_finalized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Finalizing application...\n", m_app_name, m_pid);
        fmt::print(stderr,
                   "[pylabhub-lifecycle] [{}:{}] Shutdown sequence determined for {} modules:\n",
                   m_app_name, m_pid, m_shutdown_order.size());
        for (size_t i = 0; i < m_shutdown_order.size(); ++i)
        {
            fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}]   ({}/{}) <- {}\n", m_app_name, m_pid, i + 1,
                       m_shutdown_order.size(), m_shutdown_order[i]->name);
        }

        for (const auto *module : m_shutdown_order)
        {
            try
            {
                if (module->shutdown.func)
                {
                    fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] <- Shutting down module: '{}'\n",
                               m_app_name, m_pid, module->name);
                    std::future<void> future = std::async(std::launch::async, module->shutdown.func);
                    auto status = future.wait_for(module->shutdown.timeout);

                    if (status == std::future_status::timeout)
                    {
                        fmt::print(stderr,
                                   "[pylabhub-lifecycle] [{}:{}] WARNING: Shutdown for module '{}' "
                                   "timed out after {}ms.\n",
                                   m_app_name, m_pid, module->name, module->shutdown.timeout.count());
                    }
                    else
                    {
                        future.get(); // re-throws exceptions
                    }
                }
            }
            catch (const std::exception &e)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an exception during "
                           "shutdown: {}\n",
                           m_app_name, m_pid, module->name, e.what());
            }
            catch (...)
            {
                fmt::print(stderr,
                           "[pylabhub-lifecycle] [{}:{}] ERROR: Module '{}' threw an unknown exception "
                           "during shutdown.\n",
                           m_app_name, m_pid, module->name);
            }
        }

        fmt::print(stderr, "[pylabhub-lifecycle] [{}:{}] Application finalization complete.\n", m_app_name, m_pid);
    }

private:
    LifecycleManager() : m_pid(get_pid()), m_app_name(get_executable_name()) {}
    ~LifecycleManager() = default;

    void buildGraph()
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);

        for (const auto &mod_def : m_registered_modules)
        {
            if (m_module_graph.count(mod_def.name))
            {
                throw std::runtime_error("Duplicate module name detected: '" + mod_def.name + "'.");
            }
            m_module_graph[mod_def.name] = {mod_def.name, mod_def.startup, mod_def.shutdown};
        }

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
                it->second.dependents.push_back(&module_node);
            }
        }
        m_registered_modules.clear();
    }

    std::vector<InternalGraphNode *> topologicalSort()
    {
        std::vector<InternalGraphNode *> sorted_order;
        sorted_order.reserve(m_module_graph.size());
        std::vector<InternalGraphNode *> queue;

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

            for (InternalGraphNode *v : u->dependents)
            {
                if (--(v->in_degree) == 0)
                {
                    queue.push_back(v);
                }
            }
        }

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

    const long m_pid;
    const std::string m_app_name;
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};
    std::mutex m_registry_mutex;

    std::vector<InternalModuleDef> m_registered_modules;
    std::map<std::string, InternalGraphNode> m_module_graph;
    std::vector<InternalGraphNode *> m_startup_order;
    std::vector<InternalGraphNode *> m_shutdown_order;
};

} // namespace pylabhub::utils

// ============================================================================ 
// Public C++ API Implementation (Global Functions)
// ============================================================================ 

void pylabhub_register_module(pylabhub_module &&module)
{
    if (!module.pImpl) return;

    // The LifecycleManager takes ownership of the module definition via move.
    // The pylabhub_module object that was passed in is now empty and will be
    // safely destroyed by the caller.
    pylabhub::utils::LifecycleManager::instance().registerModule(std::move(module.pImpl->def));
}

void pylabhub_initialize_application()
{
    pylabhub::utils::LifecycleManager::instance().initialize();
}

void pylabhub_finalize_application()
{
    pylabhub::utils::LifecycleManager::instance().finalize();
}
