/**
 * @file test_interactive_signal_handler.cpp
 * @brief Unit tests for InteractiveSignalHandler lifecycle (no signal injection).
 *
 * Pattern split:
 *   - 8 plain TEST() cases exercise InteractiveSignalHandler directly. Its
 *     install/uninstall path does not touch any lifecycle module (LOGGER_*
 *     calls live inside signal handlers, which these tests never trigger).
 *     They run in-process under the framework's no-lifecycle main().
 *   - The single LifecycleModuleUninstallsOnFinalize test exercises the
 *     handler ↔ LifecycleManager integration: register_dynamic_module,
 *     load_module, then a LifecycleGuard tear-down. That body is
 *     lifecycle-dependent and runs inside a worker subprocess (Pattern 3)
 *     to honour the test framework contract.
 *
 * All tests use force_daemon=true to avoid stdin reads. Single handler per
 * test (global signal state).
 */
#include "test_patterns.h"
#include "utils/interactive_signal_handler.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <source_location>

using pylabhub::InteractiveSignalHandler;
using pylabhub::SignalHandlerConfig;
using pylabhub::tests::IsolatedProcessTest;

// ── Constructor / initial state ─────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, ConstructorStoresConfigNotInstalled)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    EXPECT_FALSE(handler.is_installed());
    EXPECT_FALSE(shutdown.load());
}

// ── set_status_callback before install ──────────────────────────────────────

TEST(InteractiveSignalHandlerTest, SetStatusCallbackBeforeInstallNoThrow)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    EXPECT_NO_THROW(handler.set_status_callback([]() { return "status"; }));
}

// ── Install / uninstall toggle ──────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, InstallUninstallTogglesIsInstalled)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    EXPECT_FALSE(handler.is_installed());
    handler.install();
    EXPECT_TRUE(handler.is_installed());
    handler.uninstall();
    EXPECT_FALSE(handler.is_installed());
}

// ── Install idempotent ──────────────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, InstallIdempotent)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    handler.install();
    EXPECT_TRUE(handler.is_installed());
    // Second install should be a no-op.
    EXPECT_NO_THROW(handler.install());
    EXPECT_TRUE(handler.is_installed());
    handler.uninstall();
}

// ── Uninstall idempotent ────────────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, UninstallIdempotent)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    handler.install();
    handler.uninstall();
    EXPECT_FALSE(handler.is_installed());
    // Second uninstall should be a no-op.
    EXPECT_NO_THROW(handler.uninstall());
    EXPECT_FALSE(handler.is_installed());
}

// ── RAII: destructor on installed handler ────────────────────────────────────

TEST(InteractiveSignalHandlerTest, DestructorOnInstalledHandlerNoCrash)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};

    // Handler goes out of scope while still installed — destructor must uninstall.
    {
        InteractiveSignalHandler handler(cfg, &shutdown);
        handler.install();
        EXPECT_TRUE(handler.is_installed());
    }
    // If we get here, destructor didn't crash.
    SUCCEED();
}

// ── Force-daemon config cycle ───────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, ForceDaemonInstallUninstallCycle)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-daemon", 0, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);

    handler.set_status_callback([]() { return "daemon mode"; });
    handler.install();
    EXPECT_TRUE(handler.is_installed());
    handler.uninstall();
    EXPECT_FALSE(handler.is_installed());
    EXPECT_FALSE(shutdown.load());
}

// ── make_lifecycle_module ────────────────────────────────────────────────────

TEST(InteractiveSignalHandlerTest, MakeLifecycleModuleReturnsValid)
{
    std::atomic<bool> shutdown{false};
    SignalHandlerConfig cfg{"test-binary", 5, false, true};
    InteractiveSignalHandler handler(cfg, &shutdown);
    handler.install();

    // make_lifecycle_module() must not throw and must return a usable ModuleDef.
    EXPECT_NO_THROW({ auto mod = handler.make_lifecycle_module(); });

    handler.uninstall();
}

// ── Lifecycle module uninstalls on finalize (Pattern 3 — worker) ─────────────

class InteractiveSignalHandlerLifecycleTest : public IsolatedProcessTest
{
};

TEST_F(InteractiveSignalHandlerLifecycleTest, LifecycleModuleUninstallsOnFinalize)
{
    auto w = SpawnWorker(
        "signal_handler.lifecycle_module_uninstalls_on_finalize", {});
    ExpectWorkerOk(w);
}
