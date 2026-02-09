#pragma once
/**
 * @file test_patterns.h
 * @brief Provides three standard test patterns for PyLabHub test suite.
 *
 * Pattern 1: Pure API/Function Tests
 *   - No lifecycle management
 *   - No module dependencies
 *   - Fast, isolated unit tests
 *   - Use: class MyTest : public PureApiTest
 *
 * Pattern 2: Lifecycle-Managed Tests
 *   - Lifecycle management with module dependencies
 *   - Thread safety and correctness testing
 *   - Single process only
 *   - Use: class MyTest : public LifecycleManagedTest
 *
 * Pattern 3: Multi-Process Tests
 *   - Independent worker processes
 *   - Optional lifecycle per process
 *   - Crash/error handling validation
 *   - Use: class MyTest : public MultiProcessTest
 */

#include "gtest/gtest.h"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include <memory>
#include <vector>

namespace pylabhub::tests
{

// ============================================================================
// Pattern 1: Pure API/Function Tests
// ============================================================================

/**
 * @brief Base class for pure API/function tests.
 * @details No lifecycle management, no module dependencies.
 *          Fast, isolated unit tests for testing pure functions and APIs.
 *
 * Usage:
 * @code
 * class MyApiTest : public PureApiTest {
 * protected:
 *     void SetUp() override {
 *         PureApiTest::SetUp();
 *         // Your setup...
 *     }
 * };
 *
 * TEST_F(MyApiTest, FunctionReturnsCorrectValue) {
 *     EXPECT_EQ(my_function(42), 84);
 * }
 * @endcode
 */
class PureApiTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // No lifecycle initialization needed
    }

    void TearDown() override
    {
        // No lifecycle cleanup needed
    }
};

// ============================================================================
// Pattern 2: Lifecycle-Managed Tests
// ============================================================================

/**
 * @brief Base class for lifecycle-managed tests.
 * @details Provides automatic lifecycle management with module dependencies.
 *          Suitable for testing thread safety, correctness, integration between modules.
 *          Single process only (no worker processes).
 *
 * Usage:
 * @code
 * class MyServiceTest : public LifecycleManagedTest {
 * protected:
 *     void SetUp() override {
 *         // Register required modules
 *         RegisterModule(Logger::GetLifecycleModule());
 *         RegisterModule(FileLock::GetLifecycleModule());
 *
 *         // Initialize lifecycle
 *         LifecycleManagedTest::SetUp();
 *
 *         // Your setup...
 *     }
 * };
 *
 * TEST_F(MyServiceTest, LoggerWorksCorrectly) {
 *     LOGGER_INFO("Test message");  // Logger initialized automatically
 * }
 * @endcode
 */
class LifecycleManagedTest : public ::testing::Test
{
protected:
    /**
     * @brief Registers a module for lifecycle management.
     * @param module The ModuleDef to register
     * @note Must be called BEFORE SetUp()
     */
    void RegisterModule(pylabhub::utils::ModuleDef module)
    {
        modules_.push_back(std::move(module));
    }

    /**
     * @brief Initializes lifecycle with registered modules.
     * @note Call this at the END of your derived SetUp() method
     */
    void SetUp() override
    {
        // Create lifecycle guard with registered modules
        auto module_list = pylabhub::utils::MakeModDefList();
        for (auto& mod : modules_) {
            module_list.push_back(std::move(mod));
        }

        lifecycle_guard_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            std::move(module_list)
        );
    }

    void TearDown() override
    {
        // Lifecycle guard destructor handles shutdown in reverse order
        lifecycle_guard_.reset();
    }

private:
    std::vector<pylabhub::utils::ModuleDef> modules_;
    std::unique_ptr<pylabhub::utils::LifecycleGuard> lifecycle_guard_;
};

// ============================================================================
// Pattern 3: Multi-Process Tests
// ============================================================================

/**
 * @brief Configuration for a worker process.
 */
struct WorkerConfig
{
    std::string worker_name;                           ///< Unique worker name
    std::vector<pylabhub::utils::ModuleDef> modules;   ///< Lifecycle modules for this worker
    bool enable_lifecycle = true;                      ///< Whether to use lifecycle management
    int timeout_ms = 30000;                            ///< Worker timeout (30 seconds default)
};

/**
 * @brief Result from a worker process execution.
 */
struct WorkerResult
{
    int exit_code = -1;          ///< Worker process exit code (0 = success)
    bool timed_out = false;      ///< Whether worker timed out
    bool crashed = false;        ///< Whether worker crashed (signal/exception)
    std::string worker_name;     ///< Worker name for identification

    bool succeeded() const { return exit_code == 0 && !timed_out && !crashed; }
};

/**
 * @brief Base class for multi-process tests.
 * @details Provides infrastructure for spawning independent worker processes.
 *          Each worker can have its own lifecycle configuration.
 *          Useful for testing crash handling, process isolation, true IPC scenarios.
 *
 * Usage:
 * @code
 * class MyMultiProcessTest : public MultiProcessTest {
 * protected:
 *     void SetUp() override {
 *         MultiProcessTest::SetUp();
 *     }
 * };
 *
 * TEST_F(MyMultiProcessTest, ProducerConsumerIPC) {
 *     // Define producer worker
 *     WorkerConfig producer_cfg;
 *     producer_cfg.worker_name = "producer";
 *     producer_cfg.modules.push_back(Logger::GetLifecycleModule());
 *
 *     // Define consumer worker
 *     WorkerConfig consumer_cfg;
 *     consumer_cfg.worker_name = "consumer";
 *     consumer_cfg.enable_lifecycle = false;  // No modules needed
 *
 *     // Spawn workers
 *     auto producer_result = SpawnWorker(producer_cfg, []() {
 *         // Producer logic...
 *         return 0;  // Success
 *     });
 *
 *     auto consumer_result = SpawnWorker(consumer_cfg, []() {
 *         // Consumer logic...
 *         return 0;
 *     });
 *
 *     EXPECT_TRUE(producer_result.succeeded());
 *     EXPECT_TRUE(consumer_result.succeeded());
 * }
 * @endcode
 */
class MultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Prepare for multi-process testing
    }

    void TearDown() override
    {
        // Cleanup any remaining worker processes
        for (auto& worker : active_workers_) {
            // Wait for or terminate worker
            // TODO: Implement worker cleanup
        }
        active_workers_.clear();
    }

    /**
     * @brief Spawns a worker process with the given configuration.
     * @tparam Fn Worker function type (must return int)
     * @param config Worker configuration
     * @param worker_fn Worker function to execute (return 0 for success)
     * @return WorkerResult with exit code and status
     *
     * @note This is a placeholder. Actual implementation should integrate
     *       with test_process_utils.h (TestProcess class) and worker dispatcher.
     */
    template <typename Fn>
    WorkerResult SpawnWorker(const WorkerConfig& config, Fn&& worker_fn)
    {
        WorkerResult result;
        result.worker_name = config.worker_name;

        // TODO: Implement actual worker spawning using TestProcess
        // For now, this is a placeholder that calls the function in-process
        // Real implementation should:
        // 1. Fork/spawn independent process
        // 2. Set up lifecycle if config.enable_lifecycle
        // 3. Execute worker_fn in child process
        // 4. Collect exit code and detect crashes
        // 5. Return WorkerResult

        try {
            if (config.enable_lifecycle) {
                // Create lifecycle guard for worker
                pylabhub::utils::LifecycleGuard guard(
                    pylabhub::utils::MakeModDefList(config.modules)
                );
                result.exit_code = worker_fn();
            } else {
                // No lifecycle
                result.exit_code = worker_fn();
            }
        } catch (const std::exception& e) {
            result.crashed = true;
            result.exit_code = 1;
            LOGGER_ERROR("[Worker:{}] Crashed with exception: {}",
                        config.worker_name, e.what());
        } catch (...) {
            result.crashed = true;
            result.exit_code = 1;
            LOGGER_ERROR("[Worker:{}] Crashed with unknown exception",
                        config.worker_name);
        }

        return result;
    }

private:
    std::vector<std::string> active_workers_;  ///< Track active worker processes
};

// ============================================================================
// Helper: Determine Test Pattern from Test Class
// ============================================================================

/**
 * @brief Type trait to determine which test pattern a test uses.
 */
template <typename TestClass>
struct test_pattern
{
    static constexpr bool is_pure_api =
        std::is_base_of_v<PureApiTest, TestClass>;
    static constexpr bool is_lifecycle_managed =
        std::is_base_of_v<LifecycleManagedTest, TestClass>;
    static constexpr bool is_multi_process =
        std::is_base_of_v<MultiProcessTest, TestClass>;
};

} // namespace pylabhub::tests
