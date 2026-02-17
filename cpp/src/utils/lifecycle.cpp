/*******************************************************************************
 * @file lifecycle.cpp
 * @brief Implementation of the dependency-aware application lifecycle manager.
 *
 * @see include/utils/lifecycle.hpp
 * @see include/utils/module_def.hpp (for MAX_MODULE_NAME_LEN and C-string requirements)
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
 * 5.  **Async Dynamic Shutdown**: `unload_module()` computes the full transitive
 *     closure of modules to unload, marks them upfront (preventing new loads),
 *     and schedules them on a dedicated shutdown thread. The shutdown thread
 *     processes modules sequentially with per-module timeouts. A contaminated-
 *     modules set prevents loading modules whose prior unload failed.
 *
 * 6.  **Timed Shutdown**: All module shutdown callbacks are run with real
 *     timeouts using thread+flag+poll+detach (not std::async, whose destructor
 *     blocks even after wait_for returns timeout).
 ******************************************************************************/
#include "plh_base.hpp"

#include "utils/lifecycle.hpp"
#include "utils/module_def.hpp"
#include <chrono>             // For std::chrono::milliseconds, steady_clock
#include <condition_variable> // For std::condition_variable
#include <cstdint>            // For std::uint8_t
#include <fmt/ranges.h>       // For fmt::join on vectors
#include <map>                // For std::map
#include <mutex>              // For std::mutex, std::unique_lock, std::lock_guard
#include <queue>              // For std::queue
#include <set>                // For std::set
#include <stdexcept>          // For std::runtime_error, std::length_error
#include <thread>             // For std::thread

namespace
{
/**
 * @brief Validates a module name string_view: non-empty, within MAX_MODULE_NAME_LEN.
 *
 * @param name       The view to validate.
 * @param param_name For error messages (e.g., "module name", "dependency name").
 * @throws std::invalid_argument if `name` is empty.
 * @throws std::length_error     if `name.size() > MAX_MODULE_NAME_LEN`.
 */
void validate_module_name(std::string_view name, const char *param_name)
{
    if (name.empty())
    {
        throw std::invalid_argument(std::string("Lifecycle: ") + param_name +
                                    " must not be empty.");
    }
    if (name.size() > pylabhub::utils::ModuleDef::MAX_MODULE_NAME_LEN)
    {
        throw std::length_error(std::string("Lifecycle: ") + param_name + " exceeds maximum of " +
                                std::to_string(pylabhub::utils::ModuleDef::MAX_MODULE_NAME_LEN) +
                                " characters.");
    }
}

/**
 * @brief Outcome of a timed shutdown call.
 */
struct ShutdownOutcome
{
    bool success;           ///< true if the callback completed without throwing
    bool timed_out;         ///< true if the callback did not complete within the deadline
    std::string exception_msg; ///< non-empty if the callback threw an exception
};

/**
 * @brief Runs `func` on a thread with a real deadline. Returns without blocking
 *        beyond the deadline even if `func` hangs (the thread is detached on timeout).
 *
 * Unlike `std::async` + `wait_for`, detaching the thread on timeout is safe at process
 * exit (static modules) and acceptable for contaminated dynamic modules (the process
 * continues without them).
 *
 * @param func       The shutdown callback to invoke. Ignored if empty.
 * @param timeout    Maximum time to wait for `func` to complete.
 * @return ShutdownOutcome with success/timed_out/exception_msg fields.
 */
ShutdownOutcome timedShutdown(const std::function<void()> &func,
                              std::chrono::milliseconds timeout)
{
    if (!func)
    {
        return {true, false, {}};
    }

    std::atomic<bool> completed{false};
    std::exception_ptr ex_ptr{nullptr};
    std::thread thread([&func, &completed, &ex_ptr]()
                  {
                      try
                      {
                          func();
                      }
                      catch (...)
                      {
                          ex_ptr = std::current_exception();
                      }
                      completed.store(true, std::memory_order_release);
                  });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!completed.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            thread.detach();
            return {false, true, {}};
        }
        constexpr std::chrono::milliseconds kPollInterval(10);
        std::this_thread::sleep_for(kPollInterval);
    }

    thread.join();

    if (ex_ptr)
    {
        try
        {
            std::rethrow_exception(ex_ptr);
        }
        catch (const std::exception &e)
        {
            return {false, false, e.what()};
        }
        catch (...)
        {
            return {false, false, "unknown exception"};
        }
    }
    return {true, false, {}};
}

} // namespace

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
    bool is_persistent = false;
};
} // namespace pylabhub::utils::lifecycle_internal

namespace pylabhub::utils
{
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
        pImpl->def.startup = [startup_func]() { startup_func(nullptr); };
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
        std::string arg_str(arg);
        pImpl->def.startup = [startup_func, arg = std::move(arg_str)]()
        { startup_func(arg.c_str()); };
    }
}
void ModuleDef::set_shutdown(LifecycleCallback shutdown_func, std::chrono::milliseconds timeout)
{
    if (pImpl != nullptr && shutdown_func != nullptr)
    {
        pImpl->def.shutdown.func = [shutdown_func]() { shutdown_func(nullptr); };
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
        std::string arg_str(arg);
        pImpl->def.shutdown.func = [shutdown_func, arg_copy = std::move(arg_str)]()
        { shutdown_func(arg_copy.c_str()); };
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

class LifecycleManagerImpl
{
  public:
    LifecycleManagerImpl()
        : m_pid(pylabhub::platform::get_pid()),
          m_app_name(pylabhub::platform::get_executable_name())
    {
    }

    ~LifecycleManagerImpl();

    enum class ModuleStatus : std::uint8_t
    {
        Registered,
        Initializing,
        Started,
        Failed,
        Shutdown,
        FailedShutdown,
        ShutdownTimeout ///< Shutdown callback did not complete within timeout; thread was detached.
    };
    enum class DynamicModuleStatus : std::uint8_t
    {
        UNLOADED,
        LOADING,
        LOADED,
        FAILED,
        UNLOADING,       ///< Marked for unload; shutdown thread will process it.
        SHUTDOWN_TIMEOUT, ///< Shutdown timed out; module may be in undefined state.
        FAILED_SHUTDOWN  ///< Shutdown threw an exception; module may be in undefined state.
    };

    struct InternalGraphNode
    {
        InternalGraphNode() = default;
        InternalGraphNode(std::string name_in, std::function<void()> startup_in,
                          lifecycle_internal::InternalModuleShutdownDef shutdown_in,
                          std::vector<std::string> dependencies_in, bool is_dynamic_in,
                          bool is_persistent_in)
            : name(std::move(name_in)), startup(std::move(startup_in)),
              shutdown(std::move(shutdown_in)), dependencies(std::move(dependencies_in)),
              is_dynamic(is_dynamic_in), is_persistent(is_persistent_in)
        {
        }

        std::string name;
        std::function<void()> startup;
        lifecycle_internal::InternalModuleShutdownDef shutdown;
        std::vector<std::string> dependencies;
        std::vector<InternalGraphNode *> dependents;
        std::atomic<ModuleStatus> status = {ModuleStatus::Registered};
        bool is_dynamic = false;
        bool is_persistent = false;
        std::atomic<DynamicModuleStatus> dynamic_status = {DynamicModuleStatus::UNLOADED};
        std::atomic<int> ref_count = {0};
    };

    // --- Public API Methods ---
    void registerStaticModule(lifecycle_internal::InternalModuleDef def);
    bool registerDynamicModule(lifecycle_internal::InternalModuleDef def);
    void initialize(std::source_location loc = std::source_location::current());
    void finalize(std::source_location loc = std::source_location::current());
    bool loadModule(std::string_view name, std::source_location loc = std::source_location::current());
    bool unloadModule(std::string_view name, std::source_location loc = std::source_location::current());
    DynModuleState waitForUnload(std::string_view name, std::chrono::milliseconds timeout);
    DynModuleState getDynamicModuleState(std::string_view name);
    void setLifecycleLogSink(std::shared_ptr<LifecycleLogSink> sink);
    void clearLifecycleLogSink() noexcept;
    [[nodiscard]] bool is_initialized() const
    {
        return m_is_initialized.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool is_finalized() const
    {
        return m_is_finalized.load(std::memory_order_acquire);
    }

  private:
    // --- Private Helper Methods ---
    void recalculateReferenceCounts();
    void buildStaticGraph();
    static std::vector<InternalGraphNode *> topologicalSort(const std::vector<InternalGraphNode *> &nodes);
    bool loadModuleInternal(InternalGraphNode &node);
    static void shutdownModuleWithTimeout(InternalGraphNode &mod, std::string &debug_info);
    void printStatusAndAbort(const std::string &msg, const std::string &mod = "");

    // Routes `msg` through the installed log sink (if any) or falls back to PLH_DEBUG.
    // Used only in runtime paths where the logger is expected to be up.
    void lifecycleLog(LifecycleLogLevel level, std::string msg) const;

    // Typed helpers — call like LOGGER_DEBUG / LOGGER_WARN / LOGGER_ERROR.
    template <typename... Args>
    void lifecycleDebug(fmt::format_string<Args...> fmt_str, Args &&...args) const
    {
        lifecycleLog(LifecycleLogLevel::Debug,
               fmt::format(fmt_str, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void lifecycleWarn(fmt::format_string<Args...> fmt_str, Args &&...args) const
    {
        lifecycleLog(LifecycleLogLevel::Warn,
               fmt::format(fmt_str, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void lifecycleError(fmt::format_string<Args...> fmt_str, Args &&...args) const
    {
        lifecycleLog(LifecycleLogLevel::Error,
               fmt::format(fmt_str, std::forward<Args>(args)...));
    }

    // --- Dynamic module shutdown thread ---
    /// Computes the full transitive closure of dynamic modules that would be unloaded
    /// if `root_name` is unloaded (i.e., root + all deps whose ref_count drops to zero).
    /// Caller must hold m_graph_mutation_mutex.
    std::set<std::string> computeUnloadClosure(const std::string &root_name);

    /// Starts the dynamic shutdown thread if not already running.
    /// Caller must hold m_graph_mutation_mutex.
    void startDynShutdownThread();

    /// Main loop of the dedicated dynamic module shutdown thread.
    void dynShutdownThreadMain();

    /// Processes a single dynamic module unload (called from the shutdown thread).
    /// Acquires/releases m_graph_mutation_mutex internally around graph mutations;
    /// runs the shutdown callback WITHOUT holding any mutex.
    void processOneUnloadInThread(const std::string &node_name);

    // --- Member Variables ---
    const uint64_t m_pid;
    const std::string m_app_name;
    std::atomic<bool> m_is_initialized = {false};
    std::atomic<bool> m_is_finalized = {false};
    std::mutex m_registry_mutex;       // Protects m_registered_modules before initialization
    std::mutex m_graph_mutation_mutex; // Protects m_module_graph, m_marked_for_unload,
                                       //   m_contaminated_modules, and m_dyn_shutdown_thread_started
    std::vector<lifecycle_internal::InternalModuleDef> m_registered_modules;
    // std::less<> enables heterogeneous lookup (find/contains with std::string_view)
    // without constructing a temporary std::string.
    std::map<std::string, InternalGraphNode, std::less<>> m_module_graph;
    std::vector<InternalGraphNode *> m_startup_order;
    std::vector<InternalGraphNode *> m_shutdown_order;

    // Modules scheduled for unload (entire closure marked upfront).
    // Protected by m_graph_mutation_mutex.
    std::set<std::string, std::less<>> m_marked_for_unload;

    // Modules whose shutdown failed or timed out. New loads that depend on these are rejected.
    // Protected by m_graph_mutation_mutex.
    std::set<std::string, std::less<>> m_contaminated_modules;

    // Dedicated dynamic module shutdown thread.
    std::thread m_dyn_shutdown_thread;
    bool m_dyn_shutdown_thread_started{false}; // protected by m_graph_mutation_mutex
    std::mutex m_shutdown_queue_mutex;         // Protects m_shutdown_queue only
    std::condition_variable m_shutdown_cv;
    std::queue<std::string> m_shutdown_queue;
    std::atomic<bool> m_dyn_shutdown_thread_stop{false};

    // Condition variable for wait_for_unload() synchronisation.
    // Notified from processOneUnloadInThread() each time a module finishes.
    // Must be waited on under m_graph_mutation_mutex (like the CV patterns below).
    std::condition_variable m_unload_complete_cv;

    // Tracks which closure modules remain unprocessed, keyed by the root name
    // passed to unload_module(). Protected by m_graph_mutation_mutex.
    std::map<std::string, std::set<std::string>, std::less<>> m_pending_unload_closures;

    // Optional log sink injected by the logger module (or any module that provides logging).
    // null → fall back to PLH_DEBUG.
    std::atomic<std::shared_ptr<LifecycleLogSink>> m_lifecycle_log_sink{nullptr};
};

namespace
{
constexpr size_t kDebugInfoReserveBytes = 4096;
} // namespace

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
// loadModule / loadModuleInternal
// ============================================================================

/**
 * @brief Public entry point for loading a dynamic module.
 * @details This method is thread-safe and uses a `RecursionGuard` to prevent
 *          re-entrant calls. It finds the requested module in the graph and
 *          delegates the core loading logic to `loadModuleInternal`.
 * @param name The name of the module to load.
 * @return True if the module was loaded successfully, false otherwise.
 */
bool LifecycleManagerImpl::loadModule(std::string_view name, std::source_location loc)
{
    if (!is_initialized())
    {
        PLH_PANIC("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "     **** loadModule() called from {} ({}:{})\n"
                  "     **** FATAL: load_module called before initialization.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    }
    // Broad catch: validate_module_name may throw std::invalid_argument/std::length_error,
    // and internal map/set operations may throw std::bad_alloc. None should escape a
    // bool-returning function. RAII guards (RecursionGuard, lock_guard) unwind cleanly.
    try
    {
        validate_module_name(name, "load_module name");

        PLH_DEBUG("loadModule: request to load '{}'", name);

        if (pylabhub::basics::RecursionGuard::is_recursing(this))
        {
            PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                      "     **** loadModule() called from {} ({}:{})\n"
                      "     ****  ERROR: Re-entrant call to load_module('{}') detected.",
                      m_app_name, m_pid, loc.function_name(),
                      pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
            return false;
        }
        pylabhub::basics::RecursionGuard guard(this);
        std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);

        if (is_finalized())
        {
            PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                      "     **** loadModule() called from {} ({}:{})\n"
                      "     **** ERROR: load_module called after finalization.",
                      m_app_name, m_pid, loc.function_name(),
                      pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
            return false;
        }

        auto iter = m_module_graph.find(name);
        if (iter == m_module_graph.end() || !iter->second.is_dynamic)
        {
            PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                      "     **** loadModule() called from {} ({}:{})\n"
                      "     ****  ERROR: Dynamic module '{}' not found.",
                      m_app_name, m_pid, loc.function_name(),
                      pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
            return false;
        }

        PLH_DEBUG("loadModule: starting internal load for '{}'", name);
        bool result = loadModuleInternal(iter->second);
        PLH_DEBUG(
            "loadModule: internal load for '{}' finished with result: {}. Recalculating ref counts.",
            name, result);
        recalculateReferenceCounts();
        return result;
    }
    catch (const std::exception &e)
    {
        PLH_DEBUG("[PLH_LifeCycle] loadModule '{}': caught exception: {}", name, e.what());
        return false;
    }
    catch (...)
    {
        PLH_DEBUG("[PLH_LifeCycle] loadModule '{}': caught unknown exception.", name);
        return false;
    }
}

/**
 * @brief Internal recursive implementation for loading a dynamic module.
 *
 * Load gate: rejects any module that is scheduled for unload (UNLOADING) or whose
 * prior unload failed (SHUTDOWN_TIMEOUT / FAILED_SHUTDOWN / contaminated set).
 * This prevents loading into a partially-unloaded dependency chain.
 *
 * @param node The graph node of the module to be loaded.
 * @return True on success, false on failure.
 */
bool LifecycleManagerImpl::loadModuleInternal(InternalGraphNode &node)
{
    PLH_DEBUG("loadModuleInternal: trying to load '{}'. Current dynamic_status: {}", node.name,
              static_cast<int>(node.dynamic_status.load(std::memory_order_acquire)));

    // --- Load gate: check current dynamic_status ---
    switch (node.dynamic_status.load(std::memory_order_acquire))
    {
    case DynamicModuleStatus::LOADED:
        PLH_DEBUG("loadModuleInternal: '{}' is already LOADED.", node.name);
        return true;

    case DynamicModuleStatus::LOADING:
        PLH_DEBUG("loadModuleInternal: circular dependency detected for '{}'.", node.name);
        return false;

    case DynamicModuleStatus::FAILED:
        PLH_DEBUG("loadModuleInternal: **** ERROR: Module '{}' is in FAILED state.", node.name);
        return false;

    case DynamicModuleStatus::UNLOADING:
        PLH_DEBUG("loadModuleInternal: '{}' is scheduled for unload — rejecting load.", node.name);
        return false;

    case DynamicModuleStatus::SHUTDOWN_TIMEOUT:
    case DynamicModuleStatus::FAILED_SHUTDOWN:
        PLH_DEBUG(
            "loadModuleInternal: '{}' previously failed to unload — module is contaminated. "
            "Rejecting load.",
            node.name);
        return false;

    case DynamicModuleStatus::UNLOADED:
    default:
        break; // Proceed with load.
    }

    // Also check the contaminated set (covers the case where a node was erased from the graph
    // and re-registered under the same name — the set persists even if the node was removed).
    if (m_contaminated_modules.contains(node.name))
    {
        PLH_DEBUG(
            "loadModuleInternal: '{}' is in the contaminated set — rejecting load.", node.name);
        node.dynamic_status.store(DynamicModuleStatus::FAILED, std::memory_order_release);
        return false;
    }

    node.dynamic_status.store(DynamicModuleStatus::LOADING, std::memory_order_release);
    PLH_DEBUG("loadModuleInternal: marked '{}' as LOADING. Checking dependencies.", node.name);

    for (const auto &dep_name : node.dependencies)
    {
        PLH_DEBUG("loadModuleInternal: checking dependency '{}' for '{}'", dep_name, node.name);
        auto iter = m_module_graph.find(dep_name);
        if (iter == m_module_graph.end())
        {
            node.dynamic_status.store(DynamicModuleStatus::FAILED, std::memory_order_release);
            PLH_DEBUG("**** ERROR: Undefined dependency '{}' for module '{}'", dep_name, node.name);
            return false;
        }
        auto &dep_node = iter->second;
        if (dep_node.is_dynamic)
        {
            PLH_DEBUG("loadModuleInternal: recursing for dynamic dependency '{}'", dep_name);
            if (!loadModuleInternal(dep_node))
            {
                node.dynamic_status.store(DynamicModuleStatus::FAILED, std::memory_order_release);
                PLH_DEBUG("loadModuleInternal: dynamic dependency '{}' failed to load for '{}'",
                          dep_name, node.name);
                return false;
            }
        }
        else if (dep_node.status.load(std::memory_order_acquire) != ModuleStatus::Started)
        {
            node.dynamic_status.store(DynamicModuleStatus::FAILED, std::memory_order_release);
            PLH_DEBUG("**** ERROR: Static dependency '{}' not started for module '{}'", dep_name,
                      node.name);
            return false;
        }
    }

    PLH_DEBUG("loadModuleInternal: all dependencies for '{}' are loaded. Calling startup.",
              node.name);
    try
    {
        if (node.startup)
        {
            node.startup();
        }
        node.dynamic_status.store(DynamicModuleStatus::LOADED, std::memory_order_release);
        PLH_DEBUG("loadModuleInternal: successfully loaded and started '{}'", node.name);
        return true;
    }
    catch (const std::exception &e)
    {
        node.dynamic_status.store(DynamicModuleStatus::FAILED, std::memory_order_release);
        PLH_DEBUG("loadModuleInternal: module '{}' threw on startup: {}", node.name, e.what());
        return false;
    }
}

// ============================================================================
// unloadModule — async scheduling via the dedicated shutdown thread
// ============================================================================

/**
 * @brief Public entry point for unloading a dynamic module.
 *
 * The unload is asynchronous: this function computes the full transitive closure
 * of modules that will be unloaded, marks them all upfront (preventing any new
 * load_module calls on them), and enqueues the root module for the dedicated
 * shutdown thread. Returns immediately.
 *
 * @param name The name of the module to unload.
 * @return True if the unload was scheduled or the module was already not loaded.
 *         False if the module is in use (ref_count > 0) or cannot be found.
 */
bool LifecycleManagerImpl::unloadModule(std::string_view name, std::source_location loc)
{
    if (!is_initialized())
    {
        PLH_PANIC("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "     **** unloadModule called from {} ({}:{})\n"
                  "     **** FATAL: unload_module called without initialization.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
    }
    // Broad catch: same rationale as loadModule — none of these exceptions should
    // escape a bool-returning function. RAII guards unwind cleanly on exception.
    try
    {
        validate_module_name(name, "unload_module name");

    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "     **** unloadModule called from {} ({}:{})\n"
                  "     ****  ERROR: Re-entrant call to unload_module('{}') detected.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line(), name);
        return false;
    }
    pylabhub::basics::RecursionGuard guard(this);
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);

    if (is_finalized())
    {
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}]\n"
                  "     **** unloadModule() called from {} ({}:{})\n"
                  "     **** ERROR: unload_module called after finalization.",
                  m_app_name, m_pid, loc.function_name(),
                  pylabhub::format_tools::filename_only(loc.file_name()), loc.line());
        return false;
    }

    auto iter = m_module_graph.find(name);
    if (iter == m_module_graph.end() || !iter->second.is_dynamic)
    {
        return false;
    }

    InternalGraphNode &node_to_unload = iter->second;

    if (node_to_unload.dynamic_status.load(std::memory_order_acquire) !=
        DynamicModuleStatus::LOADED)
    {
        PLH_DEBUG("[PLH_LifeCycle] Ignoring unload request for module '{}', which is not LOADED.",
                  name);
        return true;
    }

    if (node_to_unload.is_persistent)
    {
        PLH_DEBUG("[PLH_LifeCycle] Ignoring unload request for persistent module '{}'", name);
        return true;
    }

    if (node_to_unload.ref_count.load(std::memory_order_acquire) > 0)
    {
        lifecycleWarn(
            "[PLH_LifeCycle] Cannot unload module '{}': it is a dependency for {} other module(s).",
            name, node_to_unload.ref_count.load(std::memory_order_acquire));
        return false;
    }

    // Compute the full closure of modules that will be unloaded.
    // This must be done before any status changes so the ref_count simulation is accurate.
    auto closure = computeUnloadClosure(std::string(name));

    // Reject if any module in the closure is already contaminated.
    for (const auto &module_name : closure)
    {
        if (m_contaminated_modules.contains(module_name))
        {
            PLH_DEBUG("[PLH_LifeCycle] Cannot schedule unload of '{}': closure member '{}' is "
                      "contaminated.",
                      name, module_name);
            return false;
        }
    }

    PLH_DEBUG("[PLH_LifeCycle] Scheduling unload of '{}' (closure size: {}).", name, closure.size());

    // Mark the entire closure upfront: prevents new load_module calls on these modules.
    for (const auto &module_name : closure)
    {
        m_marked_for_unload.insert(module_name);
        auto module_iterator = m_module_graph.find(module_name);
        if (module_iterator != m_module_graph.end())
        {
            module_iterator->second.dynamic_status.store(DynamicModuleStatus::UNLOADING,
                                            std::memory_order_release);
        }
    }

    // Record the full closure so wait_for_unload() can track completion.
    // Key = root module name, Value = all modules in the closure (including root).
    m_pending_unload_closures[std::string(name)] = closure;

    // Start the shutdown thread if not already running, then enqueue the root module.
    startDynShutdownThread();
    {
        std::lock_guard<std::mutex> queue_lock(m_shutdown_queue_mutex);
        m_shutdown_queue.emplace(name);
    }
    m_shutdown_cv.notify_one();

    return true;
    }
    catch (const std::exception &e)
    {
        PLH_DEBUG("[PLH_LifeCycle] unloadModule '{}': caught exception: {}", name, e.what());
        return false;
    }
    catch (...)
    {
        PLH_DEBUG("[PLH_LifeCycle] unloadModule '{}': caught unknown exception.", name);
        return false;
    }
}

// ============================================================================
// Timed module shutdown (used for static modules and inline dynamic cleanup)
// ============================================================================

/**
 * @brief Shuts down a module with a per-module timeout.
 *
 * Uses thread+flag+poll+detach (not std::async): correctly handles hangs by
 * detaching the thread and returning immediately after the deadline.
 *
 * Sets mod.status to Shutdown, FailedShutdown, or ShutdownTimeout.
 */
void LifecycleManagerImpl::shutdownModuleWithTimeout(InternalGraphNode &mod,
                                                     std::string &debug_info)
{
    const char *const type_str = mod.is_dynamic ? "dynamic" : "static";
    debug_info += fmt::format("     <- Shutting down {} module: '{}'...", type_str, mod.name);

    auto outcome = timedShutdown(mod.shutdown.func, mod.shutdown.timeout);

    if (outcome.success)
    {
        mod.status.store(ModuleStatus::Shutdown, std::memory_order_release);
        debug_info += "done.\n";
    }
    else if (outcome.timed_out)
    {
        mod.status.store(ModuleStatus::ShutdownTimeout, std::memory_order_release);
        debug_info +=
            fmt::format("TIMEOUT ({}ms)! Thread detached.\n", mod.shutdown.timeout.count());
    }
    else
    {
        mod.status.store(ModuleStatus::FailedShutdown, std::memory_order_release);
        debug_info += fmt::format("\n     **** ERROR: {} module '{}' threw on shutdown: {}\n",
                                  type_str, mod.name, outcome.exception_msg);
    }
}

// ============================================================================
// Dynamic shutdown thread implementation
// ============================================================================

std::set<std::string> LifecycleManagerImpl::computeUnloadClosure(const std::string &root_name)
{
    // BFS: expand the closure by adding any dynamic dep whose ref_count would drop to 0
    // once everything already in the closure is removed.
    // Caller holds m_graph_mutation_mutex.
    std::set<std::string> closure;
    std::queue<std::string> to_visit;

    closure.insert(root_name);
    to_visit.push(root_name);

    while (!to_visit.empty())
    {
        std::string current = std::move(to_visit.front());
        to_visit.pop();

        auto module_iterator = m_module_graph.find(current);
        if (module_iterator == m_module_graph.end())
        {
            continue;
        }

        for (const auto &dep_name : module_iterator->second.dependencies)
        {
            if (closure.contains(dep_name))
            {
                continue; // Already in closure.
            }
            auto dep_it = m_module_graph.find(dep_name);
            if (dep_it == m_module_graph.end())
            {
                continue;
            }
            const InternalGraphNode &dep = dep_it->second;
            if (!dep.is_dynamic || dep.is_persistent)
            {
                continue;
            }
            if (dep.dynamic_status.load(std::memory_order_acquire) != DynamicModuleStatus::LOADED)
            {
                continue;
            }

            // Count LOADED dynamic modules that reference dep_name, excluding closure members.
            int remaining_refs = 0;
            for (const auto &pair : m_module_graph)
            {
                if (!pair.second.is_dynamic)
                {
                    continue;
                }
                if (closure.contains(pair.first))
                {
                    continue; // This module will be removed.
                }
                if (pair.second.dynamic_status.load(std::memory_order_acquire) !=
                    DynamicModuleStatus::LOADED)
                {
                    continue;
                }
                for (const auto &dependency_name : pair.second.dependencies)
                {
                    if (dependency_name == dep_name)
                    {
                        ++remaining_refs;
                    }
                }
            }

            if (remaining_refs == 0)
            {
                closure.insert(dep_name);
                to_visit.push(dep_name);
            }
        }
    }
    return closure;
}

void LifecycleManagerImpl::startDynShutdownThread()
{
    // Caller holds m_graph_mutation_mutex.
    if (!m_dyn_shutdown_thread_started)
    {
        m_dyn_shutdown_thread_started = true;
        m_dyn_shutdown_thread =
            std::thread(&LifecycleManagerImpl::dynShutdownThreadMain, this);
    }
}

void LifecycleManagerImpl::dynShutdownThreadMain()
{
    PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}] Dynamic shutdown thread started.", m_app_name, m_pid);
    while (true)
    {
        std::string name_to_process;
        {
            std::unique_lock<std::mutex> lock(m_shutdown_queue_mutex);
            m_shutdown_cv.wait(lock, [this]()
                               {
                                   return m_dyn_shutdown_thread_stop.load(
                                              std::memory_order_acquire) ||
                                          !m_shutdown_queue.empty();
                               });
            if (m_shutdown_queue.empty())
            {
                // Woke up with an empty queue — either stop signal or spurious wakeup.
                if (m_dyn_shutdown_thread_stop.load(std::memory_order_acquire))
                {
                    break;
                }
                continue;
            }
            name_to_process = std::move(m_shutdown_queue.front());
            m_shutdown_queue.pop();
        }
        // m_shutdown_queue_mutex is released here.
        PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}] Shutdown thread processing '{}'.", m_app_name,
                  m_pid, name_to_process);
        processOneUnloadInThread(name_to_process);
    }
    PLH_DEBUG("[PLH_LifeCycle] [{}]:PID[{}] Dynamic shutdown thread exiting.", m_app_name, m_pid);
}

/**
 * @brief Shuts down a single dynamic module and, on success, recursively processes
 *        its newly-unreferenced dependencies.
 *
 * Called exclusively from the shutdown thread. Acquires/releases m_graph_mutation_mutex
 * around graph mutations only; the shutdown callback is run WITHOUT holding any mutex.
 *
 * On failure (timeout or exception):
 * - Adds the module to m_contaminated_modules.
 * - Logs an ERROR: "Unsuccessful unload of dynamic module may have destabilized the system."
 * - Halts the dependency chain (does not attempt to unload deps).
 */
void LifecycleManagerImpl::processOneUnloadInThread(const std::string &node_name)
{
    // Step 1: Read node info under m_graph_mutation_mutex (short hold, no I/O).
    std::function<void()> shutdown_func;
    std::chrono::milliseconds shutdown_timeout;
    std::vector<std::string> deps_copy;
    {
        std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
        auto module_iterator = m_module_graph.find(node_name);
        if (module_iterator == m_module_graph.end())
        {
            PLH_DEBUG("processOneUnloadInThread: '{}' not found in graph — skipping.", node_name);
            return;
        }
        InternalGraphNode &node = module_iterator->second;
        if (node.dynamic_status.load(std::memory_order_acquire) != DynamicModuleStatus::UNLOADING)
        {
            PLH_DEBUG("processOneUnloadInThread: '{}' is not in UNLOADING state — skipping.",
                      node_name);
            return;
        }
        shutdown_func = node.shutdown.func;
        shutdown_timeout = node.shutdown.timeout;
        deps_copy = node.dependencies;
    }

    // Step 2: Run shutdown callback WITHOUT holding any mutex.
    PLH_DEBUG("processOneUnloadInThread: running shutdown for '{}'.", node_name);
    auto outcome = timedShutdown(shutdown_func, shutdown_timeout);

    // Step 3: Update the graph under m_graph_mutation_mutex based on the outcome.
    std::vector<std::string> deps_to_process;
    {
        std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);

        auto module_iterator = m_module_graph.find(node_name);

        // Remove this module from all pending closure tracking entries.
        // If the entry becomes empty, erase it so wait_for_unload() can observe completion.
        for (auto it = m_pending_unload_closures.begin(); it != m_pending_unload_closures.end();)
        {
            it->second.erase(node_name);
            if (it->second.empty())
            {
                it = m_pending_unload_closures.erase(it);
            }
            else
            {
                ++it;
            }
        }
        m_unload_complete_cv.notify_all();

        if (outcome.success)
        {
            PLH_DEBUG("processOneUnloadInThread: '{}' shut down successfully. Removing from graph.",
                      node_name);
            m_marked_for_unload.erase(node_name);

            // Clean up this node's entry in each dependency's `dependents` list.
            if (module_iterator != m_module_graph.end())
            {
                for (const auto &dep_name : deps_copy)
                {
                    auto dep_it = m_module_graph.find(dep_name);
                    if (dep_it != m_module_graph.end())
                    {
                        auto &dependents_list = dep_it->second.dependents;
                        dependents_list.erase(std::remove_if(dependents_list.begin(), dependents_list.end(),
                                                [&node_name](InternalGraphNode *n)
                                                { return n->name == node_name; }),
                                 dependents_list.end());
                    }
                }
                m_module_graph.erase(module_iterator);
            }

            recalculateReferenceCounts();

            // Collect deps in the closure whose ref_count has now dropped to 0.
            for (const auto &dep_name : deps_copy)
            {
                auto dep_it = m_module_graph.find(dep_name);
                if (dep_it == m_module_graph.end())
                {
                    continue;
                }
                const InternalGraphNode &dep = dep_it->second;
                if (!dep.is_dynamic || dep.is_persistent)
                {
                    continue;
                }
                if (dep.dynamic_status.load(std::memory_order_acquire) !=
                    DynamicModuleStatus::UNLOADING)
                {
                    continue; // Not in the closure.
                }
                if (dep.ref_count.load(std::memory_order_acquire) > 0)
                {
                    continue; // Still referenced by another loaded module.
                }
                deps_to_process.push_back(dep_name);
            }
        }
        else
        {
            // Shutdown failed or timed out.
            m_contaminated_modules.insert(node_name);
            m_marked_for_unload.erase(node_name);

            if (module_iterator != m_module_graph.end())
            {
                if (outcome.timed_out)
                {
                    module_iterator->second.dynamic_status.store(DynamicModuleStatus::SHUTDOWN_TIMEOUT,
                                                    std::memory_order_release);
                    lifecycleError("[PLH_LifeCycle] [{}]:PID[{}] ERROR: Dynamic module '{}' shutdown "
                                   "TIMED OUT ({}ms).",
                                   m_app_name, m_pid, node_name, shutdown_timeout.count());
                }
                else
                {
                    module_iterator->second.dynamic_status.store(DynamicModuleStatus::FAILED_SHUTDOWN,
                                                    std::memory_order_release);
                    lifecycleError("[PLH_LifeCycle] [{}]:PID[{}] ERROR: Dynamic module '{}' shutdown "
                                   "threw: {}",
                                   m_app_name, m_pid, node_name, outcome.exception_msg);
                }
            }

            // Emit the destabilization warning. This is the ONLY place where this
            // warning is logged — it fires exclusively on an unsuccessful unload.
            lifecycleError("[PLH_LifeCycle] [{}]:PID[{}] ERROR: Unsuccessful unload of dynamic module "
                           "'{}' may have destabilized the system! "
                           "Halting unload of its dependency chain.",
                           m_app_name, m_pid, node_name);
            return; // Do NOT continue processing deps on failure.
        }
    }

    // Step 4: Recursively process deps whose ref_count dropped to zero (success path only).
    for (const auto &dep_name : deps_to_process)
    {
        processOneUnloadInThread(dep_name);
    }
}

// ============================================================================
// waitForUnload / getDynamicModuleState
// ============================================================================

DynModuleState LifecycleManagerImpl::waitForUnload(std::string_view name,
                                                    std::chrono::milliseconds timeout)
{
    if (name.empty())
    {
        return DynModuleState::NotRegistered;
    }

    // Helper: read dynamic_status from node to DynModuleState while caller holds the mutex.
    auto read_state = [this](std::string_view n) -> DynModuleState
    {
        const auto it = m_module_graph.find(n);
        if (it == m_module_graph.end() || !it->second.is_dynamic)
        {
            return DynModuleState::NotRegistered;
        }
        switch (it->second.dynamic_status.load(std::memory_order_acquire))
        {
        case DynamicModuleStatus::UNLOADED:         return DynModuleState::Unloaded;
        case DynamicModuleStatus::LOADING:          return DynModuleState::Loading;
        case DynamicModuleStatus::LOADED:           return DynModuleState::Loaded;
        case DynamicModuleStatus::UNLOADING:        return DynModuleState::Unloading;
        case DynamicModuleStatus::SHUTDOWN_TIMEOUT: return DynModuleState::ShutdownTimeout;
        case DynamicModuleStatus::FAILED_SHUTDOWN:  return DynModuleState::ShutdownFailed;
        default:                                    return DynModuleState::Unloaded;
        }
    };

    std::unique_lock<std::mutex> lock(m_graph_mutation_mutex);

    // Not scheduled → nothing to wait for; return current state now.
    if (!m_pending_unload_closures.contains(name))
    {
        return read_state(name);
    }

    // Capture name by value (string_view is cheap to copy; caller's string outlives the wait).
    auto predicate = [this, name]()
    { return !m_pending_unload_closures.contains(name); };

    if (timeout.count() == 0)
    {
        // Zero means wait indefinitely.
        m_unload_complete_cv.wait(lock, predicate);
    }
    else if (!m_unload_complete_cv.wait_for(lock, timeout, predicate))
    {
        // Timed out — still unloading.
        return DynModuleState::Unloading;
    }

    // Wait completed normally. Return the final observed state (lock still held).
    // A successfully unloaded module is removed from the graph → NotRegistered.
    return read_state(name);
}

DynModuleState LifecycleManagerImpl::getDynamicModuleState(std::string_view name)
{
    if (name.empty() || name.size() > ModuleDef::MAX_MODULE_NAME_LEN)
    {
        return DynModuleState::NotRegistered;
    }
    std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
    const auto it = m_module_graph.find(name);
    if (it == m_module_graph.end() || !it->second.is_dynamic)
    {
        return DynModuleState::NotRegistered;
    }
    switch (it->second.dynamic_status.load(std::memory_order_acquire))
    {
    case DynamicModuleStatus::UNLOADED:
        return DynModuleState::Unloaded;
    case DynamicModuleStatus::LOADING:
        return DynModuleState::Loading;
    case DynamicModuleStatus::LOADED:
        return DynModuleState::Loaded;
    case DynamicModuleStatus::UNLOADING:
        return DynModuleState::Unloading;
    case DynamicModuleStatus::SHUTDOWN_TIMEOUT:
        return DynModuleState::ShutdownTimeout;
    case DynamicModuleStatus::FAILED_SHUTDOWN:
        return DynModuleState::ShutdownFailed;
    case DynamicModuleStatus::FAILED:
    default:
        return DynModuleState::Unloaded; // FAILED → effectively unloaded/unusable.
    }
}

// ============================================================================
// Reference counting
// ============================================================================

void LifecycleManagerImpl::recalculateReferenceCounts()
{
    // 1. Reset all dynamic module ref_counts to 0.
    for (auto &pair : m_module_graph)
    {
        if (pair.second.is_dynamic)
        {
            pair.second.ref_count.store(0, std::memory_order_release);
        }
    }

    // 2. Iterate through each LOADED dynamic module.
    for (auto &pair : m_module_graph)
    {
        InternalGraphNode &source_node = pair.second;
        if (source_node.is_dynamic && source_node.dynamic_status.load(std::memory_order_acquire) ==
                                          DynamicModuleStatus::LOADED)
        {
            // 3. For each direct dependency...
            for (const auto &dep_name : source_node.dependencies)
            {
                auto iter = m_module_graph.find(dep_name);
                if (iter != m_module_graph.end())
                {
                    InternalGraphNode &dep_node = iter->second;
                    // 4. If the dependency is a non-persistent dynamic module, increment its
                    // ref_count.
                    if (dep_node.is_dynamic && !dep_node.is_persistent)
                    {
                        dep_node.ref_count.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Graph construction and topology
// ============================================================================

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
        if (m_module_graph.contains(def.name))
        {
            throw std::runtime_error("Duplicate module name: " + def.name);
        }
        m_module_graph.emplace(std::piecewise_construct, std::forward_as_tuple(def.name),
                               std::forward_as_tuple(def.name, def.startup, def.shutdown,
                                                     def.dependencies, false /*is_dynamic*/,
                                                     def.is_persistent));
    }
    for (auto &entry : m_module_graph)
    {
        for (const auto &dep_name : entry.second.dependencies)
        {
            auto iter = m_module_graph.find(dep_name);
            if (iter == m_module_graph.end())
            {
                throw std::runtime_error("Undefined dependency: " + dep_name);
            }
            iter->second.dependents.push_back(&entry.second);
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
    std::vector<InternalGraphNode *> zero_degree_queue;
    std::map<InternalGraphNode *, size_t> in_degrees;
    for (auto *node : nodes)
    {
        in_degrees[node] = 0;
    }
    for (auto *node : nodes)
    {
        for (auto *dep : node->dependents)
        {
            if (in_degrees.contains(dep))
            {
                in_degrees[dep]++;
            }
        }
    }
    for (auto *node : nodes)
    {
        if (in_degrees[node] == 0)
        {
            zero_degree_queue.push_back(node);
        }
    }
    size_t head = 0;
    while (head < zero_degree_queue.size())
    {
        InternalGraphNode *current = zero_degree_queue[head++];
        sorted_order.push_back(current);
        for (InternalGraphNode *dependent : current->dependents)
        {
            if (in_degrees.contains(dependent) && --in_degrees[dependent] == 0)
            {
                zero_degree_queue.push_back(dependent);
            }
        }
    }
    if (sorted_order.size() != nodes.size())
    {
        std::vector<std::string> cycle_nodes;
        for (auto const &[cycle_node, degree] : in_degrees)
        {
            if (degree > 0)
            {
                cycle_nodes.push_back(cycle_node->name);
            }
        }
        throw std::runtime_error("Circular dependency detected involving: " +
                                 fmt::format("{}", fmt::join(cycle_nodes, ", ")));
    }
    return sorted_order;
}

void LifecycleManagerImpl::printStatusAndAbort(const std::string &msg, const std::string &mod)
{
    fmt::print(stderr, "\n\n[PLH_LifeCycle] FATAL: {}. Aborting.\n", msg);
    if (!mod.empty())
    {
        fmt::print(stderr, "[PLH_LifeCycle] Module '{}' was point of failure.\n", mod);
    }
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
