/**
 * @file lifecycle_dynamic.cpp
 * @brief Dynamic module load/unload, shutdown thread, and reference counting.
 *
 * Contains: loadModule, loadModuleInternal, unloadModule, shutdownModuleWithTimeout,
 *           computeUnloadClosure, startDynShutdownThread, dynShutdownThreadMain,
 *           processOneUnloadInThread, waitForUnload, getDynamicModuleState,
 *           recalculateReferenceCounts.
 *
 * Private header: lifecycle_impl.hpp
 */
#include "lifecycle_impl.hpp"

namespace pylabhub::utils
{
using lifecycle_internal::validate_module_name;
using lifecycle_internal::timedShutdown;
using lifecycle_internal::ShutdownOutcome;
using pylabhub::utils::UserDataCallback;

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
        // Dispatch startup: userdata callback (new) or legacy callback.
        if (node.user_data_key != 0 && node.userdata_startup)
        {
            auto entry = resolveUserData(node.user_data_key);
            if (!entry || !entry->validate ||
                !entry->validate(entry->ptr, node.user_data_key))
            {
                node.dynamic_status.store(DynamicModuleStatus::FAILED,
                                          std::memory_order_release);
                lifecycleError("loadModuleInternal: '{}' user data validation failed",
                               node.name);
                return false;
            }
            node.userdata_startup(entry->ptr);
        }
        else if (node.startup)
        {
            node.startup();
        }
        node.init_thread_id = std::this_thread::get_id();
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
    uint64_t ud_key{0};
    UserDataCallback ud_shutdown{nullptr};
    bool thread_safe{true};
    std::thread::id init_tid{};
    lifecycle_internal::ShutdownOutcome outcome;
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
        // Copy dynamic module extension fields for thread/userdata checks.
        ud_key = node.user_data_key;
        ud_shutdown = node.userdata_shutdown;
        thread_safe = node.thread_safe_finalize;
        init_tid = node.init_thread_id;
    }

    // Step 1b: Thread awareness check.
    if (!thread_safe && init_tid != std::thread::id{} &&
        std::this_thread::get_id() != init_tid)
    {
        lifecycleWarn("processOneUnloadInThread: module '{}' requires finalize on "
                      "init thread — skipping shutdown callback (marking contaminated)",
                      node_name);
        std::lock_guard<std::mutex> lock(m_graph_mutation_mutex);
        m_contaminated_modules.insert(node_name);
        // Fall through to Step 3 cleanup with FailedShutdown outcome.
        auto module_iterator2 = m_module_graph.find(node_name);
        if (module_iterator2 != m_module_graph.end())
            module_iterator2->second.dynamic_status.store(
                DynamicModuleStatus::FAILED_SHUTDOWN, std::memory_order_release);
        // Still need to process deps and notify — jump to step 3.
        goto step3_cleanup;
    }

    // Step 2: Run shutdown callback WITHOUT holding any mutex.
    PLH_DEBUG("processOneUnloadInThread: running shutdown for '{}'.", node_name);
    if (ud_key != 0 && ud_shutdown)
    {
        // User data callback path — resolve and validate.
        auto entry = resolveUserData(ud_key);
        if (entry && entry->validate && entry->validate(entry->ptr, ud_key))
        {
            std::function<void()> ud_fn = [ud_shutdown, ptr = entry->ptr]() {
                ud_shutdown(ptr);
            };
            outcome = timedShutdown(ud_fn, shutdown_timeout);
        }
        else
        {
            lifecycleWarn("processOneUnloadInThread: module '{}' user data validation "
                          "failed — skipping shutdown callback", node_name);
            outcome = {false, false, "user data validation failed"};
        }
    }
    else
    {
        outcome = timedShutdown(shutdown_func, shutdown_timeout);
    }

step3_cleanup:
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
            // NOTE: m_module_graph.erase(module_iterator) at line 605 runs AFTER this loop
            // completes, so all `n->name` dereferences inside the lambda are valid — the
            // removed node's map entry still exists for the duration of this loop.
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

} // namespace pylabhub::utils
