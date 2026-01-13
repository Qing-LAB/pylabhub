/*******************************************************************************
 * @file Lifecycle.cpp
 * @brief Implementation of the dependency-aware application lifecycle manager.
 *
 * @see include/utils/Lifecycle.hpp
 *
 * **Implementation Details**
 *
 * This file contains the private implementation of the `LifecycleManager` and
 * `ModuleDef` classes, following the Pimpl idiom for ABI stability.
 *
 * 1.  **Static Initialization**: `initialize()` builds a graph from
 *     pre-registered static modules and starts them in topological order. This
 *     is done once.
 *
 * 2.  **Dynamic Registration**: `register_dynamic_module()` can be called after
 *     initialization. It locks the graph and adds a new dynamic module node,
 *     validating its dependencies against existing nodes. This is a runtime
 *     graph modification.
 *
 * 3.  **Unified Graph Model**: A single graph (`m_module_graph`) stores all
 *     modules. A flag on each node (`is_dynamic`) differentiates static from
 *     dynamic modules.
 *
 * 4.  **Dynamic Loading**: `load_module()` finds a dynamic module in the graph
 *     and recursively loads it and its dependencies, using reference counting.
 *     Failures are handled gracefully and do not panic the application.
 *
 * 5.  **Timed Shutdown**: `finalize()` first shuts down all active dynamic
 *     modules, then the static modules, using timeouts to prevent hangs.
 ******************************************************************************/
#include "utils/Lifecycle.hpp"
#include "debug_info.hpp"
#include "format_tools.hpp"
#include "platform.hpp"
#include "recursion_guard.hpp"
#include "utils/Logger.hpp" // For LOGGER_ERROR, etc.

#include <algorithm> // For std::reverse, std::sort
#include <future>    // For std::async, std::future
#include <map>       // For std::map
#include <mutex>     // For std::mutex, std::lock_guard
#include <stdexcept> // For std::runtime_error, std::length_error
#include <string>    // For std::string
#include <vector>    // For std::vector

#include <fmt/core.h>   // For fmt::print
#include <fmt/ranges.h> // For fmt::join on vectors

namespace pylabhub::utils::lifecycle_internal
{
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
} // namespace pylabhub::utils::lifecycle_internal

namespace pylabhub::utils
{
class ModuleDefImpl
{
  public:
    lifecycle_internal::InternalModuleDef def;
};

ModuleDef::ModuleDef(const char *name) : pImpl(std::make_unique<ModuleDefImpl>())
{
    if (name)
    {
        pImpl->def.name = name;
    }
}
ModuleDef::~ModuleDef() = default;
ModuleDef::ModuleDef(ModuleDef &&other) noexcept = default;
ModuleDef &ModuleDef::operator=(ModuleDef &&other) noexcept = default;
void ModuleDef::add_dependency(const char *dependency_name)
{
    if (pImpl && dependency_name)
        pImpl->def.dependencies.emplace_back(dependency_name);
}
void ModuleDef::set_startup(LifecycleCallback startup_func)
{
    if (pImpl && startup_func)
        pImpl->def.startup = [startup_func]() { startup_func(nullptr); };
}
void ModuleDef::set_startup(LifecycleCallback startup_func, const char *data, size_t len)
{
    if (pImpl && startup_func)
    {
        if (len > MAX_CALLBACK_PARAM_STRLEN)
            throw std::length_error("Lifecycle: Startup argument length exceeds MAX_LEN.");
        std::string arg_str(data, len);
        pImpl->def.startup = [startup_func, arg = std::move(arg_str)]()
        { startup_func(arg.c_str()); };
    }
}
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms)
{
    if (pImpl && shutdown_func)
    {
        pImpl->def.shutdown.func = [shutdown_func]() { shutdown_func(nullptr); };
        pImpl->def.shutdown.timeout = std::chrono::milliseconds(timeout_ms);
    }
}
void ModuleDef::set_shutdown(LifecycleCallback s, unsigned int t, const char *d, size_t l)
{
    if (pImpl && s)
    {
        if (l > MAX_CALLBACK_PARAM_STRLEN)
            throw std::length_error("Lifecycle: Shutdown argument length exceeds MAX_LEN.");
        std::string arg(d, l);
        pImpl->def.shutdown.func = [s, a = std::move(arg)]() { s(a.c_str()); };
        pImpl->def.shutdown.timeout = std::chrono::milliseconds(t);
    }
}

class LifecycleManagerImpl
{
  public:
    LifecycleManagerImpl()
        : m_pid(pylabhub::platform::get_pid()),
          m_app_name(pylabhub::platform::get_executable_name())
    {
    }

    enum class ModuleStatus
    {
        Registered,
        Initializing,
        Started,
        Failed,
        Shutdown
    };
    enum class DynamicModuleStatus
    {
        UNLOADED,
        LOADING,
        LOADED,
        FAILED
    };

    struct InternalGraphNode
    {
        std::string name;
        std::function<void()> startup;
        lifecycle_internal::InternalModuleShutdownDef shutdown;
        std::vector<std::string> dependencies;
        size_t in_degree = 0;
        std::vector<InternalGraphNode *> dependents;
        ModuleStatus status = ModuleStatus::Registered;
        bool is_dynamic = false;
        DynamicModuleStatus dynamic_status = DynamicModuleStatus::UNLOADED;
        int ref_count = 0;
    };

    // --- Public API Methods ---
    void registerStaticModule(lifecycle_internal::InternalModuleDef def);
    bool registerDynamicModule(lifecycle_internal::InternalModuleDef def);
    void initialize(std::source_location loc = std::source_location::current());
    void finalize(std::source_location loc = std::source_location::current());
    bool loadModule(const char *name, std::source_location loc = std::source_location::current());
    bool unloadModule(const char *name, std::source_location loc = std::source_location::current());
    bool is_initialized() const { return m_is_initialized.load(std::memory_order_acquire); }
    bool is_finalized() const { return m_is_finalized.load(std::memory_order_acquire); }

  private:
    // --- Private Helper Methods ---
    void buildStaticGraph();
    std::vector<InternalGraphNode *> topologicalSort(const std::vector<InternalGraphNode *> &nodes);
    bool loadModuleInternal(InternalGraphNode &node);
    void unloadModuleInternal(InternalGraphNode &node);
    void printStatusAndAbort(const std::string &msg, const std::string &mod = "");

    // --- Member Variables ---
    const long m_pid;
    const std::string m_app_name;
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};
    std::mutex m_registry_mutex;       // Protects m_registered_modules before initialization
    std::mutex m_graph_mutation_mutex; // Protects m_module_graph after initialization
    std::vector<lifecycle_internal::InternalModuleDef> m_registered_modules;
    std::map<std::string, InternalGraphNode> m_module_graph;
    std::vector<InternalGraphNode *> m_startup_order;
    std::vector<InternalGraphNode *> m_shutdown_order;
};

void LifecycleManagerImpl::registerStaticModule(lifecycle_internal::InternalModuleDef def)
{
    if (m_is_initialized.load(std::memory_order_acquire))
        PLH_PANIC("[PLH_LifeCycle]\nEXEC[{}]:PID[{}]\n  "
                  "  **  FATAL: register_module called after initialization.",
                  m_app_name, m_pid);
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    m_registered_modules.push_back(std::move(def));
}

/**
 * @brief Registers a new dynamic module at runtime.
 * @details This method is thread-safe. It acquires a lock on the module graph
 *          and adds the new module, validating that its name is unique and all
 *          its dependencies already exist in the graph.
 * @param def The definition of the dynamic module to register.
 * @return True if registration was successful, false otherwise.
 */
bool LifecycleManagerImpl::registerDynamicModule(lifecycle_internal::InternalModuleDef def)
{
    if (!m_is_initialized.load(std::memory_order_acquire))
    {
        PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                  "  **  ERROR: register_dynamic_module called before initialization.",
                  m_app_name, m_pid);
        return false;
    }
    if (m_is_finalized.load(std::memory_order_acquire))
    {
        PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                  "  **  ERROR: register_dynamic_module called after finalization.",
                  m_app_name, m_pid);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);

    if (m_module_graph.count(def.name))
    {
        PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                  "  **  ERROR: Module '{}' is already registered.",
                  m_app_name, m_pid, def.name);
        return false;
    }

    for (const auto &dep_name : def.dependencies)
    {
        if (m_module_graph.find(dep_name) == m_module_graph.end())
        {
            PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                      "  **  ERROR: Dependency '{}' for module '{}' not found.",
                      m_app_name, m_pid, dep_name, def.name);
            return false;
        }
    }

    auto &node = m_module_graph[def.name];
    node.name = def.name;
    node.startup = def.startup;
    node.shutdown = def.shutdown;
    node.dependencies = def.dependencies;
    node.is_dynamic = true;

    for (const auto &dep_name : def.dependencies)
    {
        auto it = m_module_graph.find(dep_name);
        if (it != m_module_graph.end())
        {
            it->second.dependents.push_back(&node);
        }
    }
    PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}] --> Registered dynamic module '{}'.", m_app_name,
              m_pid, def.name);
    return true;
}

/**
 * @brief Initializes the static modules of the application.
 * @details This method is idempotent. On the first call, it builds the dependency
 *          graph from all pre-registered static modules, performs a topological
 *          sort to determine the startup order, and then executes the startup
 *          callback for each module in sequence. Any failure during this process
 *          is considered fatal and will abort the application.
 */
void LifecycleManagerImpl::initialize(std::source_location loc)
{
    if (m_is_initialized.exchange(true, std::memory_order_acq_rel))
        return;
    std::string debug_info;
    debug_info.reserve(4096);

    debug_info += fmt::format("[PLH_LifeCycle] [{}]:PID[{}]\n"
                              "  **** initialize() triggered from {} ({}:{})\n"
                              "  --> Initializing application...\n",
                              m_app_name, m_pid, loc.function_name(),
                              pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    try
    {
        buildStaticGraph();
        std::vector<InternalGraphNode *> static_nodes;
        for (auto &p : m_module_graph)
            if (!p.second.is_dynamic)
                static_nodes.push_back(&p.second);
        m_startup_order = topologicalSort(static_nodes);
    }
    catch (const std::runtime_error &e)
    {
        printStatusAndAbort(e.what());
    }

    m_shutdown_order = m_startup_order;
    std::reverse(m_shutdown_order.begin(), m_shutdown_order.end());

    for (auto *mod : m_startup_order)
    {
        try
        {
            debug_info += fmt::format("   --> Starting static module: '{}'...", mod->name);
            mod->status = ModuleStatus::Initializing;
            if (mod->startup)
                mod->startup();
            mod->status = ModuleStatus::Started;
            debug_info += "done.\n";
        }
        catch (const std::exception &e)
        {
            mod->status = ModuleStatus::Failed;
            PLH_DEBUG("{}", debug_info);
            printStatusAndAbort("\n  **** Exception during startup: " + std::string(e.what()),
                                mod->name);
            debug_info = "";
        }
        catch (...)
        {
            mod->status = ModuleStatus::Failed;
            PLH_DEBUG("{}", debug_info);
            printStatusAndAbort("\n  **** Unknown exception during startup.", mod->name);
            debug_info = "";
        }
    }
    debug_info += "  --> Application_initialization complete.\n";
    PLH_DEBUG("{}", debug_info);
}

/**
 * @brief Shuts down all application modules.
 * @details This method is idempotent. It first finds all currently loaded dynamic
 *          modules, determines the correct shutdown order, and calls their shutdown
 *          callbacks. It then shuts down all static modules in the reverse of
 *          their startup order. Shutdown for each module is performed with a
 *          timeout to prevent hangs.
 */
void LifecycleManagerImpl::finalize(std::source_location loc)
{
    if (!m_is_initialized.load() || m_is_finalized.exchange(true))
        return;

    std::string debug_info;
    debug_info.reserve(4096);

    debug_info += fmt::format("[PLH_LifeCycle] [{}]:PID[{}]\n"
                              "  **** finalize() triggered from {} ({}:{}):\n"
                              "  <-- Finalizing application...\n",
                              m_app_name, m_pid, loc.function_name(),
                              pylabhub::format_tools::filename_only(loc.file_name()), loc.line());

    std::vector<InternalGraphNode *> loaded_dyn_nodes;
    {
        std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
        for (auto &p : m_module_graph)
            if (p.second.is_dynamic && p.second.dynamic_status == DynamicModuleStatus::LOADED)
                loaded_dyn_nodes.push_back(&p.second);
    }
    if (!loaded_dyn_nodes.empty())
    {
        auto dyn_shutdown_order = topologicalSort(loaded_dyn_nodes);
        std::reverse(dyn_shutdown_order.begin(), dyn_shutdown_order.end());
        debug_info += "  --> Unloading dynamic modules...\n";
        for (auto *mod : dyn_shutdown_order)
        {
            try
            {
                debug_info += fmt::format("    <-- Unloading dynamic module '{}'...", mod->name);
                if (mod->shutdown.func)
                {
                    auto fut = std::async(std::launch::async, mod->shutdown.func);
                    if (fut.wait_for(mod->shutdown.timeout) == std::future_status::timeout)
                    {
                        debug_info += "TIMOUT!\n";
                    }
                    else
                    {
                        fut.get();
                        debug_info += "done.\n";
                    }
                    mod->status = ModuleStatus::Shutdown;
                }
                else
                {
                    mod->status = ModuleStatus::Shutdown;
                    debug_info += "(no-op) done.\n";
                }
            }
            catch (const std::exception &e)
            {
                debug_info +=
                    fmt::format("\n  **** ERROR: Dynamic module '{}' threw on unload: {}\n",
                                mod->name, e.what());
            }
            catch (...)
            {
                debug_info += fmt::format(
                    "\n  **** ERROR: Dynamic module '{}' threw an unknown exception on unload.\n",
                    mod->name);
            }
        }
        debug_info += "  --- Dynamic module unload complete ---\n";
    }
    else
    {
        debug_info += "  --- No dynamic modules to unload ---\n";
    }

    debug_info += "\n  <-- Shutting down static modules...\n";
    for (auto *mod : m_shutdown_order)
    {
        try
        {
            debug_info += fmt::format("    <-- Shutting down static module: '{}'...", mod->name);
            if (mod->status == ModuleStatus::Started && mod->shutdown.func)
            {
                auto fut = std::async(std::launch::async, mod->shutdown.func);
                if (fut.wait_for(mod->shutdown.timeout) == std::future_status::timeout)
                {
                    debug_info += "TIMEOUT!\n";
                }
                else
                {
                    fut.get();
                    debug_info += "done.\n";
                }
                mod->status = ModuleStatus::Shutdown;
            }
            else
            {
                mod->status = ModuleStatus::Shutdown;
                debug_info += "(no-op) done.\n";
            }
        }
        catch (const std::exception &e)
        {
            debug_info += fmt::format("\n  **** ERROR: Static module '{}' threw on shutdown: {}\n",
                                      mod->name, e.what());
        }
        catch (...)
        {
            debug_info += fmt::format(
                "\n  **** ERROR: Static module '{}' threw an unknown exception on shutdown.\n",
                mod->name);
        }
    }
    debug_info += "\n  --- Static module shutdown complete ---\n"
                  "  --> Application finalization complete.\n";
    PLH_DEBUG("{}", debug_info);
}

/**
 * @brief Public entry point for loading a dynamic module.
 * @details This method is thread-safe and uses a `RecursionGuard` to prevent
 *          re-entrant calls. It finds the requested module in the graph and
 *          delegates the core loading logic to `loadModuleInternal`.
 * @param name The name of the module to load.
 * @return True if the module was loaded successfully, false otherwise.
 */
bool LifecycleManagerImpl::loadModule(const char *name, std::source_location loc)
{
    if (!is_initialized())
        PLH_PANIC("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "  **** loadModule() called from {} ({}:{})\n"
                  "  **** FATAL: load_module called before initialization.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    if (!name)
        return false;

    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "  **** loadModule() called from {} ({}:{})\n"
                  "  ****  ERROR: Re-entrant call to load_module('{}') detected.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
        return false;
    }
    pylabhub::basics::RecursionGuard guard(this);
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
    auto it = m_module_graph.find(name);
    if (it == m_module_graph.end() || !it->second.is_dynamic)
    {
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "  **** loadModule() called from {} ({}:{})\n"
                  "  ****  ERROR: Dynamic module '{}' not found.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
        return false;
    }
    return loadModuleInternal(it->second);
}

/**
 * @brief Internal recursive implementation for loading a dynamic module.
 * @details This function performs the core logic of loading a module. It handles
 *          different module statuses, detects circular dependencies, recursively
 *          loads dependencies, and increments reference counts.
 * @param node The graph node of the module to be loaded.
 * @return True on success, false on failure (e.g., circular dependency,
 *         unmet dependency, or exception during startup).
 */
bool LifecycleManagerImpl::loadModuleInternal(InternalGraphNode &node)
{
    if (node.dynamic_status == DynamicModuleStatus::LOADED)
    {
        node.ref_count++;
        return true;
    }
    if (node.dynamic_status == DynamicModuleStatus::LOADING)
    {
        PLH_DEBUG("**** ERROR: Circular dependency on module '{}'", node.name);
        return false;
    }
    if (node.dynamic_status == DynamicModuleStatus::FAILED)
    {
        PLH_DEBUG("**** ERROR: Module '{}' is in a failed state.", node.name);
        return false;
    }
    node.dynamic_status = DynamicModuleStatus::LOADING;
    std::vector<InternalGraphNode *> dyn_deps;
    for (const auto &dep_name : node.dependencies)
    {
        auto it = m_module_graph.find(dep_name);
        if (it == m_module_graph.end())
        {
            node.dynamic_status = DynamicModuleStatus::FAILED;
            PLH_DEBUG("**** ERROR: Undefined dependency '{}' for module '{}'", dep_name, node.name);
            return false;
        }
        auto &dep_node = it->second;
        if (dep_node.is_dynamic)
        {
            if (!loadModuleInternal(dep_node))
            {
                node.dynamic_status = DynamicModuleStatus::FAILED;
                return false;
            }
            dyn_deps.push_back(&dep_node);
        }
        else if (dep_node.status != ModuleStatus::Started)
        {
            node.dynamic_status = DynamicModuleStatus::FAILED;
            PLH_DEBUG("**** ERROR: Static dependency '{}' not started for module '{}'", dep_name,
                      node.name);
            return false;
        }
    }
    try
    {
        if (node.startup)
            node.startup();
        node.dynamic_status = DynamicModuleStatus::LOADED;
        node.ref_count = 1;
        for (auto *dep : dyn_deps)
            dep->ref_count++;
        PLH_DEBUG("[PLH_LifeCycle] {}:PID[{}]\n  --> Loaded dynamic module '{}'", m_app_name, m_pid,
                  node.name);
        return true;
    }
    catch (const std::exception &e)
    {
        node.dynamic_status = DynamicModuleStatus::FAILED;
        PLH_DEBUG("**** ERROR: Module '{}' threw on startup: {}", node.name, e.what());
        return false;
    }
}

/**
 * @brief Public entry point for unloading a dynamic module.
 * @details This method is thread-safe and uses a `RecursionGuard` to prevent
 *          re-entrant calls. It finds the requested module and delegates the
 *          core unloading logic to `unloadModuleInternal`.
 * @param name The name of the module to unload.
 * @return True if the module was found and considered for unloading.
 */
bool LifecycleManagerImpl::unloadModule(const char *name, std::source_location loc)
{
    if (!is_initialized())
        PLH_PANIC("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "  **** unloadModule called from {} ({}:{})\n"
                  "  **** FATAL: unload_module called without initialization.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    if (!name)
        return false;
    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "  **** unloadModule called from {} ({}:{})\n"
                  "  ****  ERROR: Re-entrant call to unload_module('{}') detected.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
        return false;
    }
    pylabhub::basics::RecursionGuard guard(this);
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
    auto it = m_module_graph.find(name);
    if (it == m_module_graph.end() || !it->second.is_dynamic)
        return false;
    unloadModuleInternal(it->second);
    return true;
}

/**
 * @brief Internal recursive implementation for unloading a dynamic module.
 * @details This function decrements the module's reference count. If the count
 *          reaches zero, it executes the shutdown callback and then recursively
 *          calls itself for the module's dependencies.
 * @param node The graph node of the module to be unloaded.
 */
void LifecycleManagerImpl::unloadModuleInternal(InternalGraphNode &node)
{
    if (node.dynamic_status != DynamicModuleStatus::LOADED)
        return;
    if (--node.ref_count > 0)
        return;
    PLH_DEBUG("<- Unloading dynamic module '{}'", node.name);
    try
    {
        if (node.shutdown.func)
            node.shutdown.func();
    }
    catch (const std::exception &e)
    {
        PLH_DEBUG("ERROR: Module '{}' threw on shutdown: {}", node.name, e.what());
    }
    node.dynamic_status = DynamicModuleStatus::UNLOADED;
    for (const auto &dep_name : node.dependencies)
    {
        auto it = m_module_graph.find(dep_name);
        if (it != m_module_graph.end() && it->second.is_dynamic)
            unloadModuleInternal(it->second);
    }
}

/**
 * @brief Constructs the initial dependency graph from pre-registered static modules.
 * @details This function is called once during `initialize`. It populates the
 *          `m_module_graph` with all modules from `m_registered_modules`,
 *          then connects the dependency links between them.
 * @throws std::runtime_error If a duplicate module name is found or a
 *         dependency points to an undefined module.
 */
void LifecycleManagerImpl::buildStaticGraph()
{
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    for (const auto &def : m_registered_modules)
    {
        if (m_module_graph.count(def.name))
            throw std::runtime_error("Duplicate module name: " + def.name);
        m_module_graph[def.name] = {def.name,
                                    def.startup,
                                    def.shutdown,
                                    def.dependencies,
                                    0,
                                    {},
                                    ModuleStatus::Registered,
                                    false,
                                    DynamicModuleStatus::UNLOADED,
                                    0};
    }
    for (auto &p : m_module_graph)
    {
        for (const auto &dep_name : p.second.dependencies)
        {
            auto it = m_module_graph.find(dep_name);
            if (it == m_module_graph.end())
                throw std::runtime_error("Undefined dependency: " + dep_name);
            it->second.dependents.push_back(&p.second);
        }
    }
    m_registered_modules.clear();
}

/**
 * @brief Performs a topological sort on a given set of graph nodes.
 * @details Uses Kahn's algorithm to determine a valid linear ordering of nodes
 *          based on their dependencies.
 * @param nodes A vector of pointers to the nodes to be sorted.
 * @return A vector of node pointers in a valid topological order.
 * @throws std::runtime_error If a circular dependency is detected in the graph.
 */
std::vector<LifecycleManagerImpl::InternalGraphNode *>
LifecycleManagerImpl::topologicalSort(const std::vector<InternalGraphNode *> &nodes)
{
    std::vector<InternalGraphNode *> sorted_order;
    sorted_order.reserve(nodes.size());
    std::vector<InternalGraphNode *> q;
    std::map<InternalGraphNode *, size_t> in_degrees;
    for (auto *n : nodes)
        in_degrees[n] = 0;
    for (auto *n : nodes)
        for (auto *dep : n->dependents)
            if (in_degrees.count(dep))
                in_degrees[dep]++;
    for (auto *n : nodes)
        if (in_degrees[n] == 0)
            q.push_back(n);
    size_t head = 0;
    while (head < q.size())
    {
        InternalGraphNode *u = q[head++];
        sorted_order.push_back(u);
        for (InternalGraphNode *v : u->dependents)
            if (in_degrees.count(v) && --in_degrees[v] == 0)
                q.push_back(v);
    }
    if (sorted_order.size() != nodes.size())
    {
        std::vector<std::string> cycle_nodes;
        for (auto const &[n, d] : in_degrees)
            if (d > 0)
                cycle_nodes.push_back(n->name);
        throw std::runtime_error("Circular dependency detected involving: " +
                                 fmt::format("{}", fmt::join(cycle_nodes, ", ")));
    }
    return sorted_order;
}

void LifecycleManagerImpl::printStatusAndAbort(const std::string &msg, const std::string &mod)
{
    fmt::print(stderr, "\n[PLH_LifeCycle] FATAL: {}. Aborting.\n", msg);
    if (!mod.empty())
        fmt::print(stderr, "[PLH_LifeCycle] Module '{}' was point of failure.\n", mod);
    fmt::print(stderr, "\n--- Module Status ---\n");
    for (auto const &[name, node] : m_module_graph)
    {
        fmt::print(stderr, "  - '{}' [{}]\n", name, node.is_dynamic ? "Dynamic" : "Static");
    }
    fmt::print(stderr, "---------------------\n\n");
    pylabhub::debug::print_stack_trace();
    std::fflush(stderr);
    std::abort();
}

LifecycleManager::LifecycleManager() : pImpl(std::make_unique<LifecycleManagerImpl>()) {}
LifecycleManager::~LifecycleManager() = default;
LifecycleManager &LifecycleManager::instance()
{
    static LifecycleManager instance;
    return instance;
}
void LifecycleManager::register_module(ModuleDef &&def)
{
    if (def.pImpl)
        pImpl->registerStaticModule(std::move(def.pImpl->def));
}
bool LifecycleManager::register_dynamic_module(ModuleDef &&def)
{
    if (def.pImpl)
        return pImpl->registerDynamicModule(std::move(def.pImpl->def));
    return false;
}
void LifecycleManager::initialize(std::source_location loc)
{
    pImpl->initialize(loc);
}
void LifecycleManager::finalize(std::source_location loc)
{
    pImpl->finalize(loc);
}
bool LifecycleManager::is_initialized()
{
    return pImpl->is_initialized();
}
bool LifecycleManager::is_finalized()
{
    return pImpl->is_finalized();
}
bool LifecycleManager::load_module(const char *name, std::source_location loc)
{
    return pImpl->loadModule(name, loc);
}
bool LifecycleManager::unload_module(const char *name, std::source_location loc)
{
    return pImpl->unloadModule(name, loc);
}

} // namespace pylabhub::utils
