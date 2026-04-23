/**
 * @file role_host_base_workers.cpp
 * @brief Worker implementations for RoleHostBase lifecycle tests.
 *
 * Runs one subprocess per test. The non-aborting workers use run_gtest_worker
 * to own a LifecycleGuard (Logger + FileLock + JsonConfig). The two abort
 * workers install a LifecycleGuard manually, set up the failing case, and
 * allow PLH_PANIC → std::abort() to terminate the subprocess — the parent
 * asserts exit_code != 0 and the panic message appears in stderr.
 *
 * The minimal TestRoleHost / NoShutdownHost / OverridingShutdownHost /
 * ForgetfulShutdownHost subclasses are defined here so each worker can
 * build one fresh per subprocess (no cross-test state).
 */
#include "role_host_base_workers.h"

#include "producer_fields.hpp"
#include "producer_init.hpp"

#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/config/role_config.hpp"
#include "utils/native_engine.hpp"
#include "utils/role_directory.hpp"
#include "utils/engine_host.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using pylabhub::config::RoleConfig;
using pylabhub::scripting::NativeEngine;
using pylabhub::scripting::RoleHostBase;
using pylabhub::scripting::ScriptEngine;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::Logger;
using pylabhub::utils::MakeModDefList;
using pylabhub::utils::RoleDirectory;

namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace role_host_base
{
namespace
{

/// Minimal host with controllable ready-signal and a condvar-driven loop.
class TestRoleHost : public RoleHostBase
{
  public:
    TestRoleHost(RoleConfig cfg, std::unique_ptr<ScriptEngine> engine,
                 std::atomic<bool> *shutdown_flag, bool report_ready = true,
                 bool run_after_ready = true)
        : RoleHostBase("test", std::move(cfg), std::move(engine), shutdown_flag),
          report_ready_(report_ready), run_after_ready_(run_after_ready)
    {
    }
    ~TestRoleHost() override { shutdown_(); }

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
        while (core().should_continue_loop())
            core().wait_for_incoming(50);
        core().set_running(false);
        worker_exited_cleanly.store(true);
    }

  private:
    bool report_ready_;
    bool run_after_ready_;
};

/// Host that fails to call shutdown_() in its dtor → base dtor aborts.
class NoShutdownHost : public RoleHostBase
{
  public:
    NoShutdownHost(RoleConfig cfg, std::unique_ptr<ScriptEngine> engine,
                   std::atomic<bool> *shutdown_flag)
        : RoleHostBase("no_sd", std::move(cfg), std::move(engine), shutdown_flag)
    {
    }
    ~NoShutdownHost() override = default;  // deliberately no shutdown_()

  protected:
    void worker_main_() override
    {
        core().set_script_load_ok(true);
        ready_promise().set_value(true);
    }
};

/// Host whose override forwards to base — expected-correct path.
class OverridingShutdownHost : public RoleHostBase
{
  public:
    OverridingShutdownHost(RoleConfig cfg, std::unique_ptr<ScriptEngine> engine,
                           std::atomic<bool> *shutdown_flag)
        : RoleHostBase("ovr", std::move(cfg), std::move(engine), shutdown_flag)
    {
    }
    ~OverridingShutdownHost() override { shutdown_(); }

    std::atomic<int> derived_shutdown_calls{0};

    void shutdown_() noexcept override
    {
        derived_shutdown_calls.fetch_add(1);
        RoleHostBase::shutdown_();
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

/// Host whose override forgets to call base → base dtor aborts.
class ForgetfulShutdownHost : public RoleHostBase
{
  public:
    ForgetfulShutdownHost(RoleConfig cfg, std::unique_ptr<ScriptEngine> engine,
                          std::atomic<bool> *shutdown_flag)
        : RoleHostBase("forget", std::move(cfg), std::move(engine), shutdown_flag)
    {
    }
    ~ForgetfulShutdownHost() override { shutdown_(); }

    void shutdown_() noexcept override
    {
        // Deliberately does NOT forward to RoleHostBase::shutdown_().
    }

  protected:
    void worker_main_() override
    {
        core().set_script_load_ok(true);
        ready_promise().set_value(true);
    }
};

/// Registers the producer role init entry exactly once per subprocess.
void register_producer_once()
{
    static bool registered = false;
    if (registered)
        return;
    pylabhub::producer::register_producer_init();
    registered = true;
}

/// Builds a fresh RoleConfig via init_directory + load_from_directory into
/// the parent-provided @p dir. Each subprocess owns the dir for its lifetime.
RoleConfig build_config(const std::string &dir)
{
    if (RoleDirectory::init_directory(dir, "producer", "X") != 0)
        throw std::runtime_error("init_directory failed for test setup");
    return RoleConfig::load_from_directory(dir, "producer",
                                           pylabhub::producer::parse_producer_fields);
}

/// Waits up to @p timeout_ms for @p pred to become true (yield-spin).
template <typename F>
bool wait_for(F pred, int timeout_ms = 500)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

} // namespace

// ── Happy-path workers ──────────────────────────────────────────────────────

int construct_not_running(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown);
            EXPECT_FALSE(host.is_running());
            EXPECT_FALSE(host.script_load_ok());
            EXPECT_EQ(host.role_tag(), "test");
        },
        "role_host_base::construct_not_running",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int startup_run_shutdown(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            auto host = std::make_unique<TestRoleHost>(
                build_config(dir), std::make_unique<NativeEngine>(), &shutdown);

            host->startup_();
            EXPECT_TRUE(host->script_load_ok());
            EXPECT_EQ(host->worker_calls.load(), 1);

            ASSERT_TRUE(wait_for([&]() { return host->worker_entered_loop.load(); }));
            EXPECT_TRUE(host->is_running());
            EXPECT_FALSE(host->worker_exited_cleanly.load());

            host->shutdown_();
            EXPECT_FALSE(host->is_running());
            EXPECT_TRUE(host->worker_exited_cleanly.load());
        },
        "role_host_base::startup_run_shutdown",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int startup_ready_false(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown,
                              /*report_ready=*/false,
                              /*run_after_ready=*/false);
            host.startup_();
            EXPECT_FALSE(host.script_load_ok());
            EXPECT_FALSE(host.is_running());
            EXPECT_EQ(host.worker_calls.load(), 1);
        },
        "role_host_base::startup_ready_false",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int validate_mode(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown,
                              /*report_ready=*/true,
                              /*run_after_ready=*/false);
            host.startup_();
            EXPECT_TRUE(host.script_load_ok());
            EXPECT_FALSE(host.is_running());

            ASSERT_TRUE(wait_for([&]() { return host.worker_exited_cleanly.load(); }));
            EXPECT_FALSE(host.worker_entered_loop.load());
        },
        "role_host_base::validate_mode",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int shutdown_idempotent(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown);
            host.startup_();
            host.shutdown_();
            host.shutdown_();
            host.shutdown_();
            EXPECT_FALSE(host.is_running());
        },
        "role_host_base::shutdown_idempotent",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int shutdown_before_startup(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown);
            host.shutdown_();
            EXPECT_FALSE(host.is_running());
            EXPECT_EQ(host.worker_calls.load(), 0);
        },
        "role_host_base::shutdown_before_startup",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int virtual_shutdown_override_forwards(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            OverridingShutdownHost host(build_config(dir),
                                        std::make_unique<NativeEngine>(), &shutdown);
            host.startup_();
            host.shutdown_();
            EXPECT_EQ(host.derived_shutdown_calls.load(), 1);
            EXPECT_FALSE(host.is_running());
        },
        "role_host_base::virtual_shutdown_override_forwards",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int external_shutdown_flag(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            auto host = std::make_unique<TestRoleHost>(
                build_config(dir), std::make_unique<NativeEngine>(), &shutdown);
            host->startup_();
            ASSERT_TRUE(wait_for([&]() { return host->worker_entered_loop.load(); }));

            shutdown.store(true);
            host->shutdown_();
            EXPECT_TRUE(host->worker_exited_cleanly.load());
        },
        "role_host_base::external_shutdown_flag",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int accessors_config_and_role_tag(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown);
            EXPECT_EQ(host.role_tag(), "test");
            EXPECT_EQ(&host.config(), &host.config());
        },
        "role_host_base::accessors_config_and_role_tag",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int wait_for_wakeup_honours_timeout(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_producer_once();
            std::atomic<bool> shutdown{false};
            TestRoleHost host(build_config(dir),
                              std::make_unique<NativeEngine>(), &shutdown);
            host.startup_();

            const auto t0 = std::chrono::steady_clock::now();
            host.wait_for_wakeup(20);
            const auto elapsed = std::chrono::steady_clock::now() - t0;

            EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                          .count(),
                      500);
            host.shutdown_();
        },
        "role_host_base::wait_for_wakeup_honours_timeout",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Abort workers ───────────────────────────────────────────────────────────
//
// These DO NOT use run_gtest_worker. We install a LifecycleGuard manually so
// setup (init_directory, RoleConfig load) works, then run the deliberately-
// aborting scenario. PLH_PANIC → std::abort() terminates the process before
// any cleanup. The parent inspects exit_code + stderr for the panic message.

int dtor_missing_shutdown_aborts(const std::string &dir)
{
    LifecycleGuard guard(MakeModDefList(Logger::GetLifecycleModule(),
                                        FileLock::GetLifecycleModule(),
                                        JsonConfig::GetLifecycleModule()));
    register_producer_once();
    std::atomic<bool> shutdown{false};
    {
        NoShutdownHost host(build_config(dir),
                            std::make_unique<NativeEngine>(), &shutdown);
        host.startup_();
        // Dropping host here → base dtor sees missing flag → PLH_PANIC → abort.
    }
    // Should never reach here.
    fmt::print(stderr, "[WORKER FAILURE] expected abort did not occur\n");
    return 99;
}

int virtual_shutdown_no_base_aborts(const std::string &dir)
{
    LifecycleGuard guard(MakeModDefList(Logger::GetLifecycleModule(),
                                        FileLock::GetLifecycleModule(),
                                        JsonConfig::GetLifecycleModule()));
    register_producer_once();
    std::atomic<bool> shutdown{false};
    {
        ForgetfulShutdownHost host(build_config(dir),
                                   std::make_unique<NativeEngine>(), &shutdown);
        host.startup_();
        // dtor → derived shutdown_ (no-op) → base dtor → PLH_PANIC → abort.
    }
    fmt::print(stderr, "[WORKER FAILURE] expected abort did not occur\n");
    return 99;
}

} // namespace role_host_base
} // namespace pylabhub::tests::worker

// ── Self-registering dispatcher ─────────────────────────────────────────────

namespace
{

struct RoleHostBaseWorkerRegistrar
{
    RoleHostBaseWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 3)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_host_base")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_host_base;
                const std::string dir = argv[2];

                if (sc == "construct_not_running")
                    return construct_not_running(dir);
                if (sc == "startup_run_shutdown")
                    return startup_run_shutdown(dir);
                if (sc == "startup_ready_false")
                    return startup_ready_false(dir);
                if (sc == "validate_mode")
                    return validate_mode(dir);
                if (sc == "shutdown_idempotent")
                    return shutdown_idempotent(dir);
                if (sc == "shutdown_before_startup")
                    return shutdown_before_startup(dir);
                if (sc == "virtual_shutdown_override_forwards")
                    return virtual_shutdown_override_forwards(dir);
                if (sc == "external_shutdown_flag")
                    return external_shutdown_flag(dir);
                if (sc == "accessors_config_and_role_tag")
                    return accessors_config_and_role_tag(dir);
                if (sc == "wait_for_wakeup_honours_timeout")
                    return wait_for_wakeup_honours_timeout(dir);

                if (sc == "dtor_missing_shutdown_aborts")
                    return dtor_missing_shutdown_aborts(dir);
                if (sc == "virtual_shutdown_no_base_aborts")
                    return virtual_shutdown_no_base_aborts(dir);

                fmt::print(stderr,
                           "[role_host_base] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static RoleHostBaseWorkerRegistrar g_role_host_base_registrar;

} // namespace
