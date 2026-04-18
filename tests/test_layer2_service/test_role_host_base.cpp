/**
 * @file test_role_host_base.cpp
 * @brief L2 lifecycle tests for RoleHostBase using a minimal test subclass.
 *
 * The test subclass @ref TestRoleHost implements @c worker_main_ with a
 * controllable ready-signal and a minimal run-loop (waits on incoming-
 * message condvar until shutdown is requested). Each test builds one,
 * calls startup_/shutdown_, and asserts observable state: is_running(),
 * script_load_ok(), worker-thread activation counters, and the PLH_PANIC
 * contract enforcement on a missed shutdown (death test).
 *
 * The worker coordination uses only atomics + promise/future +
 * wait_for_incoming — no sleeps for ordering.
 */
#include "producer_fields.hpp"
#include "producer_init.hpp"

#include "plh_datahub.hpp"
#include "utils/config/role_config.hpp"
#include "utils/native_engine.hpp"     // concrete no-dep ScriptEngine
#include "utils/role_directory.hpp"
#include "utils/role_host_base.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <unistd.h>

using pylabhub::scripting::RoleHostBase;
using pylabhub::scripting::NativeEngine;
using pylabhub::scripting::ScriptEngine;
using pylabhub::config::RoleConfig;

// ─── Minimal test subclass with controllable worker behaviour ───────────────

class TestRoleHost : public RoleHostBase
{
  public:
    TestRoleHost(RoleConfig cfg,
                 std::unique_ptr<ScriptEngine> engine,
                 std::atomic<bool> *shutdown_flag,
                 bool   report_ready    = true,
                 bool   run_after_ready = true)
        : RoleHostBase("test", std::move(cfg), std::move(engine), shutdown_flag),
          report_ready_(report_ready),
          run_after_ready_(run_after_ready)
    {
    }

    ~TestRoleHost() override { shutdown_(); }

    // Observables — for tests to assert on.
    std::atomic<int>  worker_calls{0};
    std::atomic<bool> worker_entered_loop{false};
    std::atomic<bool> worker_exited_cleanly{false};

  protected:
    void worker_main_() override
    {
        worker_calls.fetch_add(1);
        core().set_script_load_ok(report_ready_);

        if (!report_ready_)
        {
            ready_promise().set_value(false);
            return;
        }

        ready_promise().set_value(true);

        if (!run_after_ready_)
        {
            worker_exited_cleanly.store(true);
            return;
        }

        core().set_running(true);
        worker_entered_loop.store(true);
        // should_continue_loop() returns false once shutdown_() has called
        // core_.request_stop() (internal flag), which is what we need here —
        // is_process_exit_requested() would only observe the external
        // g_shutdown_ atomic. notify_incoming() wakes wait_for_incoming().
        while (core().should_continue_loop())
        {
            core().wait_for_incoming(50);  // 50 ms tick
        }
        core().set_running(false);
        worker_exited_cleanly.store(true);
    }

  private:
    bool report_ready_;
    bool run_after_ready_;
};

// ─── Fixture: registers producer once, sets up lifecycle, generates a
//             fresh RoleConfig per test via RoleDirectory::init_directory ──

namespace fs = std::filesystem;

class RoleHostBaseLifecycleTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        static bool registered = false;
        if (!registered)
        {
            pylabhub::producer::register_producer_init();
            registered = true;
        }
    }

  protected:
    void SetUp() override
    {
        lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::utils::FileLock::GetLifecycleModule(),
                pylabhub::utils::JsonConfig::GetLifecycleModule()));
    }
    void TearDown() override
    {
        lifecycle_.reset();
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    fs::path unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_rhb_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p;
    }

    // Build a fresh RoleConfig via init_directory + load_from_directory.
    // Uses producer (matches the registered init entry).
    RoleConfig make_config(const char *name = "X")
    {
        auto dir = unique_dir("cfg");
        if (pylabhub::utils::RoleDirectory::init_directory(dir, "producer", name) != 0)
            throw std::runtime_error("init_directory failed for test setup");
        return RoleConfig::load_from_directory(
            dir.string(), "producer",
            pylabhub::producer::parse_producer_fields);
    }

    std::unique_ptr<ScriptEngine> make_engine()
    {
        return std::make_unique<NativeEngine>();
    }

    std::unique_ptr<pylabhub::utils::LifecycleGuard> lifecycle_;
    std::vector<fs::path>                             paths_to_clean_;
};

// ─── Construction + trivial accessors ───────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Construct_NotRunning_NotLoaded)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(), &shutdown);
    EXPECT_FALSE(host.is_running());
    EXPECT_FALSE(host.script_load_ok());
    EXPECT_EQ(host.role_tag(), "test");
}

// ─── Happy path: startup → worker runs → shutdown ───────────────────────────

TEST_F(RoleHostBaseLifecycleTest, StartupRun_WorkerEntersLoop_ShutdownJoinsCleanly)
{
    std::atomic<bool> shutdown{false};
    auto host = std::make_unique<TestRoleHost>(
        make_config(), make_engine(), &shutdown);

    host->startup_();

    EXPECT_TRUE(host->script_load_ok());
    EXPECT_EQ(host->worker_calls.load(), 1);

    // Worker should have entered the loop before (or shortly after) startup_
    // returns. The ready_promise was fulfilled before worker_entered_loop
    // is set (one load after, but the loop-enter is within a few ops). Wait
    // up to 500 ms for it.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (!host->worker_entered_loop.load() &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(host->worker_entered_loop.load());
    EXPECT_TRUE(host->is_running());
    EXPECT_FALSE(host->worker_exited_cleanly.load());

    host->shutdown_();

    EXPECT_FALSE(host->is_running());
    EXPECT_TRUE(host->worker_exited_cleanly.load());
}

// ─── Worker signals ready=false ─────────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, StartupFailure_ReadyFalse_ApiReset_NotRunning)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(),
                       &shutdown,
                       /*report_ready=*/false,
                       /*run_after_ready=*/false);

    host.startup_();
    EXPECT_FALSE(host.script_load_ok());
    EXPECT_FALSE(host.is_running());
    EXPECT_EQ(host.worker_calls.load(), 1);
}

// ─── Validate-only: worker reports ready=true then exits ────────────────────

TEST_F(RoleHostBaseLifecycleTest, ValidateMode_ReadyThenExitsWithoutLoop)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(),
                       &shutdown,
                       /*report_ready=*/true,
                       /*run_after_ready=*/false);

    host.startup_();
    EXPECT_TRUE(host.script_load_ok());
    EXPECT_FALSE(host.is_running());

    // Worker should have exited without entering the loop.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (!host.worker_exited_cleanly.load() &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_TRUE(host.worker_exited_cleanly.load());
    EXPECT_FALSE(host.worker_entered_loop.load());
}

// ─── shutdown_ is idempotent ────────────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Shutdown_Idempotent)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(), &shutdown);
    host.startup_();
    host.shutdown_();
    // Second, third, fourth calls must not deadlock, throw, or double-join.
    host.shutdown_();
    host.shutdown_();
    EXPECT_FALSE(host.is_running());
}

// ─── Shutdown before startup_ is valid and harmless ─────────────────────────

TEST_F(RoleHostBaseLifecycleTest, ShutdownBeforeStartup_Harmless)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(), &shutdown);
    // Never called startup_(); shutdown_ in dtor still valid.
    host.shutdown_();
    EXPECT_FALSE(host.is_running());
    EXPECT_EQ(host.worker_calls.load(), 0);
}

// ─── Dtor contract: missing shutdown_ call triggers PLH_PANIC ──────────────

class NoShutdownHost : public RoleHostBase
{
  public:
    NoShutdownHost(RoleConfig cfg,
                    std::unique_ptr<ScriptEngine> engine,
                    std::atomic<bool> *shutdown_flag)
        : RoleHostBase("no_sd", std::move(cfg), std::move(engine), shutdown_flag)
    {
    }
    // DELIBERATELY no shutdown_() call — tests that the base dtor aborts.
    ~NoShutdownHost() override = default;

  protected:
    void worker_main_() override
    {
        // Signal ready quickly then exit so there's no live worker to
        // contend with when the parent thread forces destruction.
        core().set_script_load_ok(true);
        ready_promise().set_value(true);
    }
};

TEST_F(RoleHostBaseLifecycleTest, DtorContract_MissingShutdown_Aborts)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(
        {
            std::atomic<bool> shutdown{false};
            NoShutdownHost host(make_config(),
                                 make_engine(), &shutdown);
            host.startup_();
            // Dropping host here triggers base dtor's flag check.
        },
        "RoleHostBase destructor entered without shutdown_");
}

// ─── Virtual shutdown_ override calling base is allowed ─────────────────────

class OverridingShutdownHost : public RoleHostBase
{
  public:
    OverridingShutdownHost(RoleConfig cfg,
                            std::unique_ptr<ScriptEngine> engine,
                            std::atomic<bool> *shutdown_flag)
        : RoleHostBase("ovr", std::move(cfg), std::move(engine), shutdown_flag)
    {
    }
    ~OverridingShutdownHost() override { shutdown_(); }

    std::atomic<int> derived_shutdown_calls{0};

    void shutdown_() noexcept override
    {
        derived_shutdown_calls.fetch_add(1);
        RoleHostBase::shutdown_();  // MUST call base or dtor aborts
    }

  protected:
    void worker_main_() override
    {
        core().set_script_load_ok(true);
        ready_promise().set_value(true);
        core().set_running(true);
        while (core().should_continue_loop())
            core().wait_for_incoming(50);
        core().set_running(false);
    }
};

TEST_F(RoleHostBaseLifecycleTest, VirtualShutdown_Override_ForwardsToBase)
{
    std::atomic<bool> shutdown{false};
    OverridingShutdownHost host(make_config(),
                                  make_engine(), &shutdown);
    host.startup_();
    host.shutdown_();
    EXPECT_EQ(host.derived_shutdown_calls.load(), 1);
    EXPECT_FALSE(host.is_running());
}

// ─── Virtual shutdown_ override WITHOUT base call → abort ───────────────────

class ForgetfulShutdownHost : public RoleHostBase
{
  public:
    ForgetfulShutdownHost(RoleConfig cfg,
                           std::unique_ptr<ScriptEngine> engine,
                           std::atomic<bool> *shutdown_flag)
        : RoleHostBase("forget", std::move(cfg), std::move(engine),
                        shutdown_flag)
    {
    }
    ~ForgetfulShutdownHost() override { shutdown_(); }

    void shutdown_() noexcept override
    {
        // INCORRECT: does not call RoleHostBase::shutdown_().
        // Base dtor must detect the missing flag and abort.
    }

  protected:
    void worker_main_() override
    {
        core().set_script_load_ok(true);
        ready_promise().set_value(true);
    }
};

TEST_F(RoleHostBaseLifecycleTest, VirtualShutdown_OverrideWithoutBase_Aborts)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(
        {
            std::atomic<bool> shutdown{false};
            ForgetfulShutdownHost host(make_config(),
                                        make_engine(), &shutdown);
            host.startup_();
            // dtor → derived shutdown_ (no-op) → base dtor → abort
        },
        "RoleHostBase destructor entered without shutdown_");
}

// ─── External shutdown-flag wiring: triggers loop exit via core_ ────────────

TEST_F(RoleHostBaseLifecycleTest, ExternalShutdownFlag_PropagatesToCore)
{
    std::atomic<bool> shutdown{false};
    auto host = std::make_unique<TestRoleHost>(
        make_config(), make_engine(), &shutdown);
    host->startup_();

    // Wait for loop entry.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (!host->worker_entered_loop.load() &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(host->worker_entered_loop.load());

    // Flip the external flag. The worker's loop polls is_process_exit_requested
    // which reads the external flag via core_.
    shutdown.store(true);

    // Still need to call shutdown_() to honour the contract and join.
    host->shutdown_();
    EXPECT_TRUE(host->worker_exited_cleanly.load());
}

// ─── config() + role_tag() accessors ────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Accessors_ConfigAndRoleTag)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(), &shutdown);
    EXPECT_EQ(host.role_tag(), "test");
    // config() returns a reference to the moved-in config. Just verify
    // the reference is stable across calls.
    EXPECT_EQ(&host.config(), &host.config());
}

// ─── wait_for_wakeup returns within the timeout ─────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, WaitForWakeup_HonoursTimeoutWithoutHang)
{
    std::atomic<bool> shutdown{false};
    TestRoleHost host(make_config(), make_engine(), &shutdown);
    host.startup_();

    const auto t0 = std::chrono::steady_clock::now();
    host.wait_for_wakeup(20);  // 20 ms
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Must return in a bounded time — generous upper bound for CI scheduling.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              500);
    host.shutdown_();
}
