/**
 * @file lifecycle.cpp
 * @brief Core lifecycle management: module registration, initialization, finalization,
 *        log sink, and public API delegation.
 *
 * Split layout:
 *   lifecycle.cpp          — this file: ModuleDef API, register/init/finalize,
 *                             log sink, LifecycleManager public delegation
 *   lifecycle_topology.cpp — dependency graph construction and topological sort
 *   lifecycle_dynamic.cpp  — dynamic module load/unload and shutdown thread
 *
 * @see include/utils/lifecycle.hpp
 * @see include/utils/module_def.hpp
 */
#include "lifecycle_impl.hpp"

namespace
{
constexpr size_t kDebugInfoReserveBytes = 4096;
} // namespace

namespace pylabhub::utils
{
using lifecycle_internal::validate_module_name;
using lifecycle_internal::timedShutdown;
using lifecycle_internal::ShutdownOutcome;

class ModuleDefImpl
{
  public:
    lifecycle_internal::InternalModuleDef def;
};

ModuleDef::ModuleDef(std::string_view name) : pImpl(std::make_unique<ModuleDefImpl>())
{
    validate_module_name(name, "module name");
    pImpl->def.name = std::string(name);
}
ModuleDef::ModuleDef(std::string_view name, void *userdata, UserDataValidateFn validate)
    : pImpl(std::make_unique<ModuleDefImpl>())
{
    validate_module_name(name, "module name");
    pImpl->def.name = std::string(name);
    pImpl->def.userdata = userdata;
    pImpl->def.userdata_validate = validate;
    if (userdata)
    {
        uint64_t key;
        do {
            key = LifecycleManager::next_unique_key();
        } while (key == 0u);
        pImpl->def.userdata_key = key;
    }
}
uint64_t ModuleDef::userdata_key() const noexcept
{
    return pImpl ? pImpl->def.userdata_key : 0;
}
ModuleDef::~ModuleDef() = default;
ModuleDef::ModuleDef(ModuleDef &&other) noexcept = default;
ModuleDef &ModuleDef::operator=(ModuleDef &&other) noexcept = default;
void ModuleDef::add_dependency(std::string_view dependency_name)
{
    if (pImpl != nullptr && !dependency_name.empty())
    {
        if (dependency_name.size() > MAX_MODULE_NAME_LEN)
        {
            throw std::length_error("Lifecycle: dependency name exceeds maximum length.");
        }
        pImpl->def.dependencies.emplace_back(dependency_name);
    }
}
void ModuleDef::set_startup(LifecycleCallback startup_func)
{
    if (pImpl != nullptr && startup_func != nullptr)
    {
        void *ud = pImpl->def.userdata;
        pImpl->def.startup = [startup_func, ud]() { startup_func(nullptr, ud); };
    }
}
void ModuleDef::set_startup(LifecycleCallback startup_func, std::string_view arg)
{
    if (pImpl != nullptr && startup_func != nullptr)
    {
        if (arg.size() > MAX_CALLBACK_PARAM_STRLEN)
        {
            throw std::length_error("Lifecycle: startup argument length exceeds MAX_CALLBACK_PARAM_STRLEN.");
        }
        void *ud = pImpl->def.userdata;
        std::string arg_str(arg);
        pImpl->def.startup = [startup_func, arg = std::move(arg_str), ud]()
        { startup_func(arg.c_str(), ud); };
    }
}
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, std::chrono::milliseconds timeout)
{
    if (pImpl != nullptr && shutdown_func != nullptr)
    {
        void *ud = pImpl->def.userdata;
        pImpl->def.shutdown.func = [shutdown_func, ud]() { shutdown_func(nullptr, ud); };
        pImpl->def.shutdown.timeout = timeout;
    }
}
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, std::chrono::milliseconds timeout,
                             std::string_view arg)
{
    if (pImpl != nullptr && shutdown_func != nullptr)
    {
        if (arg.size() > MAX_CALLBACK_PARAM_STRLEN)
        {
            throw std::length_error("Lifecycle: shutdown argument length exceeds MAX_CALLBACK_PARAM_STRLEN.");
        }
        void *ud = pImpl->def.userdata;
        std::string arg_str(arg);
        pImpl->def.shutdown.func = [shutdown_func, arg_copy = std::move(arg_str), ud]()
        { shutdown_func(arg_copy.c_str(), ud); };
        pImpl->def.shutdown.timeout = timeout;
    }
}

void ModuleDef::set_as_persistent(bool persistent)
{
    if (pImpl)
    {
        pImpl->def.is_persistent = persistent;
    }
}

// ============================================================================
// LifecycleManager::next_unique_key
// ============================================================================

uint64_t LifecycleManager::next_unique_key()
{
    return instance().pImpl->nextUserdataKey();
}

// ============================================================================
// LifecycleManagerImpl destructor
// ============================================================================

LifecycleManagerImpl::~LifecycleManagerImpl()
{
    // If the shutdown thread was started (by any unload_module call), signal it
    // to stop and wait for it to finish. If finalize() was called first (normal
    // usage), the thread will have already drained and will exit immediately.
    if (!m_dyn_shutdown_thread_started || !m_dyn_shutdown_thread.joinable())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_shutdown_queue_mutex);
        m_dyn_shutdown_thread_stop.store(true, std::memory_order_release);
    }
    m_shutdown_cv.notify_all();
    m_dyn_shutdown_thread.join();
}

// ============================================================================
// Static module management
// ============================================================================

void LifecycleManagerImpl::registerStaticModule(lifecycle_internal::InternalModuleDef def)
{
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    if (m_is_initialized.load(std::memory_order_acquire))
    {
        PLH_PANIC("[PLH_LifeCycle]\nEXEC[{}]:PID[{}]\n  "
                  "    **  FATAL: register_module called after initialization.",
                  m_app_name, m_pid);
    }
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
                  "    **  ERROR: register_dynamic_module called before initialization.",
                  m_app_name, m_pid);
        return false;
    }
    if (m_is_finalized.load(std::memory_order_acquire))
    {
        PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                  "    **  ERROR: register_dynamic_module called after finalization.",
                  m_app_name, m_pid);
        return false;
    }

    std::fflush(stderr);
    PLH_DEBUG("-> registerDynamicModule: trying to lock mutex for '{}'", def.name);
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
    PLH_DEBUG("-> registerDynamicModule: mutex locked for '{}'", def.name);

    if (m_module_graph.contains(def.name))
    {
        PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                  "    **  ERROR: Module '{}' is already registered.",
                  m_app_name, m_pid, def.name);
        return false;
    }

    for (const auto &dep_name : def.dependencies)
    {
        if (!m_module_graph.contains(dep_name))
        {
            PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                      "    **  ERROR: Dependency '{}' for module '{}' not found.",
                      m_app_name, m_pid, dep_name, def.name);
            return false;
        }
    }

    // Reject registration if any dependency is contaminated.
    for (const auto &dep_name : def.dependencies)
    {
        if (m_contaminated_modules.contains(dep_name))
        {
            PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}]\n  "
                      "    **  ERROR: Cannot register '{}': dependency '{}' is contaminated.",
                      m_app_name, m_pid, def.name, dep_name);
            return false;
        }
    }

    auto &node = m_module_graph[def.name];
    node.name = def.name;
    node.startup = def.startup;
    node.shutdown = def.shutdown;
    node.dependencies = def.dependencies;
    node.is_dynamic = true;
    node.is_persistent = def.is_persistent;
    node.userdata = def.userdata;
    node.userdata_key = def.userdata_key;
    node.userdata_validate = def.userdata_validate;

    // `&node` is a pointer to the newly-inserted map value. std::map provides
    // reference/pointer stability: insertions do not invalidate references to
    // existing elements, and `node` itself was just inserted above. The pointer
    // remains valid until the element is explicitly erased (see unregister path
    // in lifecycle_dynamic.cpp::processOneUnloadInThread).
    for (const auto &dep_name : def.dependencies)
    {
        auto iter = m_module_graph.find(dep_name);
        if (iter != m_module_graph.end())
        {
            iter->second.dependents.push_back(&node);
        }
    }
    PLH_DEBUG("[PLH_LifeCycle]\n[{}]:PID[{}] -> Registered dynamic module '{}'.", m_app_name, m_pid,
              def.name);
    return true;
}

// ============================================================================
// initialize / finalize
// ============================================================================

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
    {
        return;
    }
    std::string debug_info;
    debug_info.reserve(kDebugInfoReserveBytes);

    debug_info += fmt::format("[PLH_LifeCycle] [{}]:PID[{}]\n"
                              "     **** initialize() triggered from {} ({}:{})\n"
                              "     -> Initializing application...\n",
                              m_app_name, m_pid, loc.function_name(),
                              pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    try
    {
        buildStaticGraph();
        std::vector<InternalGraphNode *> static_nodes;
        for (auto &entry : m_module_graph)
        {
            if (!entry.second.is_dynamic)
            {
                static_nodes.push_back(&entry.second);
            }
        }
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
            debug_info += fmt::format("     -> Starting static module: '{}'...", mod->name);
            mod->status.store(ModuleStatus::Initializing, std::memory_order_release);
            if (mod->startup)
            {
                mod->startup();
            }
            mod->status.store(ModuleStatus::Started, std::memory_order_release);
            debug_info += "done.\n";
        }
        catch (const std::exception &e)
        {
            mod->status.store(ModuleStatus::Failed, std::memory_order_release);
            PLH_DEBUG("{}", debug_info);
            printStatusAndAbort("\n     **** Exception during startup: " + std::string(e.what()),
                                mod->name);
            debug_info = "";
        }
        catch (...)
        {
            mod->status.store(ModuleStatus::Failed, std::memory_order_release);
            PLH_DEBUG("{}", debug_info);
            printStatusAndAbort("\n     **** Unknown exception during startup.", mod->name);
            debug_info = "";
        }
    }
    debug_info += "     -> Application_initialization complete.\n";
    PLH_DEBUG("{}", debug_info);
}

/**
 * @brief Shuts down all application modules.
 *
 * Sequence:
 * 1. Signal the dynamic shutdown thread to drain and stop; join with a bounded wait.
 * 2. Shut down any remaining LOADED dynamic modules inline (those not previously
 *    scheduled for async unload).
 * 3. Shut down static modules in reverse startup order, each with a per-module timeout.
 *
 * All shutdown callbacks use thread+flag+poll+detach, so no shutdown call can block
 * finalize() beyond its configured timeout.
 */
void LifecycleManagerImpl::finalize(std::source_location loc)
{
    if (!m_is_initialized.load(std::memory_order_acquire) ||
        m_is_finalized.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    std::string debug_info;
    debug_info.reserve(kDebugInfoReserveBytes);

    debug_info +=
        fmt::format("[PLH_LifeCycle] [{}]:PID[{}]\n"
                    "     **** finalize() called, associated with a constructor from {} ({}:{}):\n"
                    "     <- Finalizing application...\n",
                    m_app_name, m_pid, loc.function_name(),
                    pylabhub::format_tools::filename_only(loc.file_name()), loc.line());

    // Phase 1: Drain the dynamic shutdown thread.
    // Signal it to stop (after it empties its queue) and join.
    // The join is bounded because each item processed by the thread has a per-module
    // timeout enforced by timedShutdown().
    {
        std::lock_guard<std::mutex> queue_lock(m_shutdown_queue_mutex);
        m_dyn_shutdown_thread_stop.store(true, std::memory_order_release);
    }
    m_shutdown_cv.notify_all();

    if (m_dyn_shutdown_thread_started && m_dyn_shutdown_thread.joinable())
    {
        debug_info += "     -> Waiting for dynamic shutdown thread to drain...\n";
        m_dyn_shutdown_thread.join(); // bounded by per-module shutdown timeouts
        debug_info += "     --- Dynamic shutdown thread drained ---\n";
    }

    // Phase 2: Handle any remaining LOADED dynamic modules (those not scheduled via
    // unload_module — e.g., the application forgot to unload them before finalize).
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);

    std::vector<InternalGraphNode *> loaded_dyn_nodes;
    for (auto &entry : m_module_graph)
    {
        if (entry.second.is_dynamic &&
            entry.second.dynamic_status.load(std::memory_order_acquire) ==
                DynamicModuleStatus::LOADED)
        {
            loaded_dyn_nodes.push_back(&entry.second);
        }
    }

    if (!loaded_dyn_nodes.empty())
    {
        auto dyn_shutdown_order = topologicalSort(loaded_dyn_nodes);
        std::reverse(dyn_shutdown_order.begin(), dyn_shutdown_order.end());
        debug_info += "     -> Shutting down remaining dynamic modules...\n";
        for (auto *mod : dyn_shutdown_order)
        {
            shutdownModuleWithTimeout(*mod, debug_info);
        }
        debug_info += "     --- Remaining dynamic module shutdown complete ---\n";
    }
    else
    {
        debug_info += "     --- No remaining dynamic modules to shut down ---\n";
    }

    // Phase 3: Shut down static modules in reverse startup order.
    debug_info += "\n     <- Shutting down static modules...\n";
    for (auto *mod : m_shutdown_order)
    {
        if (mod->status.load(std::memory_order_acquire) == ModuleStatus::Started)
        {
            shutdownModuleWithTimeout(*mod, debug_info);
        }
        else
        {
            mod->status.store(ModuleStatus::Shutdown, std::memory_order_release);
            debug_info += fmt::format("     <- Shutting down static module: '{}'...(no-op) done.\n",
                                      mod->name);
        }
    }
    debug_info += "\n     --- Static module shutdown complete ---\n     -> Application "
                  "finalization complete.\n";
    PLH_DEBUG("{}", debug_info);
}

// ============================================================================
// Log sink — internal runtime logging
// ============================================================================

void LifecycleManagerImpl::setLifecycleLogSink(std::shared_ptr<LifecycleLogSink> sink)
{
    m_lifecycle_log_sink.store(std::move(sink), std::memory_order_release);
}

void LifecycleManagerImpl::clearLifecycleLogSink() noexcept
{
    m_lifecycle_log_sink.store(nullptr, std::memory_order_release);
}

void LifecycleManagerImpl::lifecycleLog(LifecycleLogLevel level, std::string msg) const
{
    auto sink_ptr = m_lifecycle_log_sink.load(std::memory_order_acquire);
    if (sink_ptr && *sink_ptr)
    {
        (*sink_ptr)(level, msg);
        return;
    }
    // No sink installed — fall back to PLH_DEBUG for all levels.
    PLH_DEBUG("[Lifecycle] {}", msg);
}

// ============================================================================
// LifecycleManager public API (thin delegation layer)
// ============================================================================

LifecycleManager::LifecycleManager() : pImpl(std::make_unique<LifecycleManagerImpl>()) {}
LifecycleManager::~LifecycleManager() = default;
LifecycleManager &LifecycleManager::instance()
{
    static LifecycleManager instance;
    return instance;
}
void LifecycleManager::register_module(ModuleDef &&def)
{
    if (def.pImpl != nullptr)
    {
        pImpl->registerStaticModule(std::move(def.pImpl->def));
    }
}
bool LifecycleManager::register_dynamic_module(ModuleDef &&def)
{
    if (def.pImpl != nullptr)
    {
        return pImpl->registerDynamicModule(std::move(def.pImpl->def));
    }
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
bool LifecycleManager::load_module(std::string_view name, std::source_location loc)
{
    return pImpl->loadModule(name, loc);
}
bool LifecycleManager::unload_module(std::string_view name, std::source_location loc)
{
    return pImpl->unloadModule(name, loc);
}
DynModuleState LifecycleManager::wait_for_unload(std::string_view name,
                                                  std::chrono::milliseconds timeout)
{
    return pImpl->waitForUnload(name, timeout);
}
DynModuleState LifecycleManager::dynamic_module_state(std::string_view name)
{
    return pImpl->getDynamicModuleState(name);
}
void LifecycleManager::set_lifecycle_log_sink(LifecycleLogSink sink)
{
    if (sink)
    {
        pImpl->setLifecycleLogSink(std::make_shared<LifecycleLogSink>(std::move(sink)));
    }
    else
    {
        pImpl->clearLifecycleLogSink();
    }
}
void LifecycleManager::clear_lifecycle_log_sink() noexcept
{
    pImpl->clearLifecycleLogSink();
}


} // namespace pylabhub::utils
