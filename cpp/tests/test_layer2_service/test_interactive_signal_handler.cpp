/**
 * @file test_interactive_signal_handler.cpp
 * @brief Unit tests for InteractiveSignalHandler lifecycle (no signal injection).
 *
 * All tests use force_daemon=true to avoid stdin reads.
 * Single handler per test (global signal state).
 */
#include "utils/interactive_signal_handler.hpp"
#include <gtest/gtest.h>

#include <atomic>

using pylabhub::InteractiveSignalHandler;
using pylabhub::SignalHandlerConfig;

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
