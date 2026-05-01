#pragma once
/**
 * @file lifecycle_impl.hpp
 * @brief Private implementation header for the lifecycle subsystem.
 *
 * Defines LifecycleManagerImpl and associated internal types shared between
 * the lifecycle compilation units. Not for public installation.
 *
 * Included by: lifecycle.cpp, lifecycle_topology.cpp, lifecycle_dynamic.cpp
 */
#include "utils/lifecycle.hpp"
#include "utils/module_def.hpp"
#include <chrono>             // For std::chrono::milliseconds, steady_clock
#include <condition_variable> // For std::condition_variable
#include <cstdint>            // For std::uint8_t
#include <fmt/ranges.h>       // For fmt::join on vectors
#include <map>                // For std::map
#include <mutex>              // For std::mutex, std::unique_lock, std::lock_guard
#include "portable_atomic_shared_ptr.hpp"
#include <queue>              // For std::queue
#include <set>                // For std::set
#include <stdexcept>          // For std::runtime_error, std::length_error
#include <thread>             // For std::thread

// ---------------------------------------------------------------------------
// Internal helper declarations (bodies in lifecycle_helpers.cpp — one definition,
// shared between lifecycle.cpp, lifecycle_topology.cpp, lifecycle_dynamic.cpp).
// These helpers are NOT part of the public API and are not installed.
// ---------------------------------------------------------------------------

namespace pylabhub::utils::lifecycle_internal
{

/**
 * @brief Outcome of a timed shutdown call.
 */
struct ShutdownOutcome
{
    bool        success;       ///< true if the callback completed without throwing
    bool        timed_out;     ///< true if the callback did not complete within the deadline
    std::string exception_msg; ///< non-empty if the callback threw an exception
};

/**
 * @brief Validates a module name: non-empty, within MAX_MODULE_NAME_LEN.
 * @throws std::invalid_argument / std::length_error on failure.
 * @note Defined in lifecycle_helpers.cpp (one definition, shared by all lifecycle TUs).
 */
void validate_module_name(std::string_view name, const char *param_name);

/**
 * @brief Runs `func` on a thread with a real deadline.
 *        Returns without blocking beyond the deadline (detaches thread on timeout).
 * @note Defined in lifecycle_helpers.cpp.
 */
ShutdownOutcome timedShutdown(const std::function<void()> &func,
                              std::chrono::milliseconds    timeout);

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

    /// Opt-in: owner-managed teardown (HEP-CORE-0001 §"Owner-managed
    /// teardown" exception).  When `true`, the module declares that its
    /// C++ owner performs the real teardown synchronously and clears the
    /// validator's userdata as a deliberate "I'm gone, skip my callback"
    /// signal.  In that case, validator-fail at unload is treated as a
    /// success-without-callback rather than as an anomaly: no
    /// `FAILED_SHUTDOWN`, no contamination, full graph cleanup so the
    /// module name can be re-registered.  Default `false` preserves the
    /// HEP-0001 anomaly semantics for every module that does not opt in.
    bool owner_managed_teardown = false;

    // User data (set at ModuleDef construction, immutable).
    void *userdata{nullptr};
    uint64_t userdata_key{0};
    pylabhub::utils::UserDataValidateFn userdata_validate{nullptr};
};
} // namespace pylabhub::utils::lifecycle_internal

namespace pylabhub::utils
{
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
        /// Mirror of `InternalModuleDef::owner_managed_teardown` (set
        /// at registration, immutable thereafter).  See that field's
        /// doc for semantics.  Read-only on the unload path.
        bool owner_managed_teardown = false;
        std::atomic<DynamicModuleStatus> dynamic_status = {DynamicModuleStatus::UNLOADED};
        std::atomic<int> ref_count = {0};

        // User data (copied from InternalModuleDef at registration).
        void *userdata{nullptr};
        uint64_t userdata_key{0};
        pylabhub::utils::UserDataValidateFn userdata_validate{nullptr};
    };

    // --- Internal helpers ---

    /// Performs the post-callback graph cleanup that the success path
    /// runs (and that owner-managed-teardown validator-fail also runs,
    /// since both treat the module as cleanly torn down).  Caller MUST
    /// hold `m_graph_mutation_mutex` for the duration of this call.
    /// Populates @p deps_to_process with dependencies whose ref_count
    /// dropped to 0 as a result of removing this module from the graph;
    /// the caller is responsible for invoking
    /// `processOneUnloadInThread` on each of those names AFTER
    /// releasing the mutex.
    void cleanupAfterUnload_(const std::string              &node_name,
                             const std::vector<std::string> &deps_copy,
                             std::vector<std::string>       &deps_to_process);

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
    pylabhub::utils::detail::PortableAtomicSharedPtr<LifecycleLogSink> m_lifecycle_log_sink;

    // User-data generation key counter (monotonic, skip-zero).
    std::atomic<uint64_t> m_next_userdata_key{1};
  public:
    uint64_t nextUserdataKey() { return m_next_userdata_key.fetch_add(1, std::memory_order_relaxed); }
};

} // namespace pylabhub::utils
