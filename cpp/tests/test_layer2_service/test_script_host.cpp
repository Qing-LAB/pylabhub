/**
 * @file test_script_host.cpp
 * @brief Unit tests for ScriptHost threading model (abstract base class).
 *
 * ScriptHost is in pylabhub-utils (scripting/script_host.cpp).  It has no
 * lifecycle dependency — tests create mock subclasses directly.
 *
 * ## Coverage
 *
 * Threaded mode (owns_dedicated_thread = true, Python-style):
 *   1. IsReadyFalseOnCreation        — is_ready() false before any startup
 *   2. ThreadedStartupSetsReady      — base_startup_() → is_ready() true
 *   3. ThreadedShutdownClearsReady   — base_shutdown_() → is_ready() false
 *   4. ThreadedShutdownIdempotent    — second base_shutdown_() is safe
 *   5. ThreadedEarlyStop             — stop_ set before startup; do_initialize detects and exits
 *   6. ThreadedExceptionPropagates   — do_initialize() throws → base_startup_() rethrows
 *   7. ThreadedReturnFalseNoSignal   — do_initialize() returns false without signal_ready_() → throws
 *
 * Direct mode (owns_dedicated_thread = false, Lua-style):
 *   8. DirectModeStartupSetsReady    — do_initialize called on calling thread; base signals ready
 *   9. DirectModeShutdownFinalizes   — base_shutdown_() calls do_finalize on calling thread
 *  10. DirectModeFailureThrows       — do_initialize returns false → base_startup_() throws
 *
 * Test isolation: each TEST_F creates a fresh mock instance (gtest per-test fixture).
 */
#include "utils/script_host.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using pylabhub::utils::ScriptHost;

namespace
{

// ============================================================================
// Mock helpers
// ============================================================================

/**
 * @class ThreadedMock
 * @brief Threaded ScriptHost mock: calls signal_ready_(), waits for stop_.
 *
 * Exposes public wrappers around the protected base machinery so tests can
 * call startup/shutdown without going through a lifecycle module.
 */
class ThreadedMock : public ScriptHost
{
  public:
    bool owns_dedicated_thread() const noexcept override { return true; }

    bool do_initialize(const fs::path& /*path*/) override
    {
        if (stop_.load(std::memory_order_acquire))
        {
            signal_ready_(); // early-exit path: signal even on early stop
            return true;
        }
        signal_ready_();
        while (!stop_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return true;
    }

    void do_finalize() noexcept override { finalize_called_.store(true); }

    // Public wrappers for tests
    void startup(const fs::path& p = {}) { base_startup_(p); }
    void shutdown() noexcept             { base_shutdown_(); }
    void set_stop() noexcept             { stop_.store(true, std::memory_order_release); }

    std::atomic<bool> finalize_called_{false};
};

/**
 * @class ThreadedExceptionMock
 * @brief Throws std::runtime_error in do_initialize() without calling signal_ready_().
 */
class ThreadedExceptionMock : public ScriptHost
{
  public:
    bool owns_dedicated_thread() const noexcept override { return true; }

    bool do_initialize(const fs::path&) override
    {
        // Deliberately does NOT call signal_ready_() — thread_fn_ propagates the exception.
        throw std::runtime_error("test_script_host: intentional init failure");
    }

    void do_finalize() noexcept override {}

    void startup(const fs::path& p = {}) { base_startup_(p); }
    void shutdown() noexcept             { base_shutdown_(); }
};

/**
 * @class ThreadedFalseMock
 * @brief Returns false from do_initialize() without calling signal_ready_().
 */
class ThreadedFalseMock : public ScriptHost
{
  public:
    bool owns_dedicated_thread() const noexcept override { return true; }

    bool do_initialize(const fs::path&) override
    {
        // Returns false without signaling — thread_fn_ sets exception on promise.
        return false;
    }

    void do_finalize() noexcept override {}

    void startup(const fs::path& p = {}) { base_startup_(p); }
    void shutdown() noexcept             { base_shutdown_(); }
};

/**
 * @class DirectMock
 * @brief Direct-mode (Lua-style) mock: do_initialize called on calling thread.
 */
class DirectMock : public ScriptHost
{
  public:
    bool owns_dedicated_thread() const noexcept override { return false; }

    bool do_initialize(const fs::path&) override
    {
        init_called_.store(true);
        return true;
    }

    void do_finalize() noexcept override { finalize_called_.store(true); }

    void startup(const fs::path& p = {}) { base_startup_(p); }
    void shutdown() noexcept             { base_shutdown_(); }

    std::atomic<bool> init_called_{false};
    std::atomic<bool> finalize_called_{false};
};

/**
 * @class DirectFailMock
 * @brief Direct-mode mock that returns false from do_initialize().
 */
class DirectFailMock : public ScriptHost
{
  public:
    bool owns_dedicated_thread() const noexcept override { return false; }
    bool do_initialize(const fs::path&) override { return false; }
    void do_finalize() noexcept override {}

    void startup(const fs::path& p = {}) { base_startup_(p); }
};

} // namespace

// ============================================================================
// Threaded mode tests
// ============================================================================

TEST(ScriptHostTest, IsReadyFalseOnCreation)
{
    ThreadedMock host;
    EXPECT_FALSE(host.is_ready());
}

TEST(ScriptHostTest, ThreadedStartupSetsReady)
{
    ThreadedMock host;
    ASSERT_NO_THROW(host.startup());
    EXPECT_TRUE(host.is_ready());
    host.shutdown();
}

TEST(ScriptHostTest, ThreadedShutdownClearsReady)
{
    ThreadedMock host;
    host.startup();
    ASSERT_TRUE(host.is_ready());

    host.shutdown();
    // thread_fn_ stores ready_=false after do_initialize returns.
    EXPECT_FALSE(host.is_ready());
}

TEST(ScriptHostTest, ThreadedShutdownIdempotent)
{
    ThreadedMock host;
    host.startup();
    host.shutdown();
    // Second shutdown: stop_ already set, thread not joinable → no-op.
    EXPECT_NO_THROW(host.shutdown());
}

TEST(ScriptHostTest, ThreadedEarlyStop)
{
    // Set stop_ before startup() so do_initialize() exits immediately after signal_ready_().
    ThreadedMock host;
    host.set_stop(); // simulates signal fired before interpreter thread runs

    // startup() must not block or throw — thread signals ready then exits quickly.
    ASSERT_NO_THROW(host.startup());

    // The thread may already have cleared ready_ (it sets ready_=false before do_finalize).
    // Calling shutdown() is safe regardless: if thread is done, join is instant.
    EXPECT_NO_THROW(host.shutdown());

    // After shutdown(), ready_ is false (thread cleared it after do_initialize returned).
    EXPECT_FALSE(host.is_ready());
}

TEST(ScriptHostTest, ThreadedExceptionPropagates)
{
    ThreadedExceptionMock host;
    EXPECT_THROW(host.startup(), std::runtime_error);

    // After exception, thread has exited; shutdown() must be safe.
    EXPECT_NO_THROW(host.shutdown());
}

TEST(ScriptHostTest, ThreadedReturnFalseNoSignal)
{
    // do_initialize() returns false without calling signal_ready_() →
    // thread_fn_ sets exception on init_promise_ → base_startup_() throws.
    ThreadedFalseMock host;
    EXPECT_THROW(host.startup(), std::runtime_error);

    EXPECT_NO_THROW(host.shutdown());
}

// ============================================================================
// Direct mode tests
// ============================================================================

TEST(ScriptHostTest, DirectModeStartupSetsReady)
{
    DirectMock host;
    ASSERT_NO_THROW(host.startup());
    EXPECT_TRUE(host.init_called_.load());
    EXPECT_TRUE(host.is_ready()); // signal_ready_() called by base_startup_() in direct mode
    host.shutdown();
}

TEST(ScriptHostTest, DirectModeShutdownFinalizes)
{
    DirectMock host;
    host.startup();
    EXPECT_FALSE(host.finalize_called_.load()); // not yet finalized

    host.shutdown(); // base_shutdown_() calls do_finalize on calling thread
    EXPECT_TRUE(host.finalize_called_.load());
    EXPECT_FALSE(host.is_ready()); // ready_ cleared before do_finalize
}

TEST(ScriptHostTest, DirectModeFailureThrows)
{
    DirectFailMock host;
    EXPECT_THROW(host.startup(), std::runtime_error);
}
