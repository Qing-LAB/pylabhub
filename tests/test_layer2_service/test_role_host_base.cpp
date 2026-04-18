/**
 * @file test_role_host_base.cpp
 * @brief Pattern 3 driver: RoleHostBase lifecycle tests.
 *
 * Each TEST_F spawns a worker subprocess (IsolatedProcessTest). Happy-path
 * workers own their lifecycle via run_gtest_worker() and assert observable
 * state (is_running, script_load_ok, worker-thread activation counters).
 * The two "abort" workers install a LifecycleGuard manually and drop a
 * deliberately-misconfigured host so PLH_PANIC → std::abort() terminates
 * the subprocess; the parent asserts exit_code != 0 and the panic message
 * in stderr.
 *
 * See docs/tech_draft/test_compliance_audit.md for the framework contract
 * this file now follows after correction from the earlier V1 violation.
 */
#include "test_patterns.h"
#include "test_process_utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class RoleHostBaseLifecycleTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_rhb_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p.string();
    }

    /// Abort-test helper: spawn worker, expect non-zero exit + panic line.
    /// The worker is expected to call std::abort() via PLH_PANIC.
    void expect_panic_abort(const std::string &scenario, const std::string &dir,
                            const std::string &panic_substring)
    {
        // redirect_stderr_to_console=false so the panic goes to a captured
        // temp file — we inspect it after exit. with_ready_signal=false.
        auto w = SpawnWorker(scenario, {dir});
        w.wait_for_exit();
        EXPECT_NE(w.exit_code(), 0) << "worker was expected to abort but exited cleanly";
        EXPECT_THAT(w.get_stderr(), ::testing::HasSubstr(panic_substring))
            << "stderr did not contain expected panic text. Captured stderr:\n"
            << w.get_stderr();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ─── Construction + trivial accessors ───────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Construct_NotRunning_NotLoaded)
{
    auto dir = unique_dir("construct");
    auto w   = SpawnWorker("role_host_base.construct_not_running", {dir});
    ExpectWorkerOk(w);
}

// ─── Happy path: startup → worker runs → shutdown ───────────────────────────

TEST_F(RoleHostBaseLifecycleTest, StartupRun_WorkerEntersLoop_ShutdownJoinsCleanly)
{
    auto dir = unique_dir("run_shutdown");
    auto w   = SpawnWorker("role_host_base.startup_run_shutdown", {dir});
    ExpectWorkerOk(w);
}

// ─── Worker signals ready=false ─────────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, StartupFailure_ReadyFalse_ApiReset_NotRunning)
{
    auto dir = unique_dir("ready_false");
    auto w   = SpawnWorker("role_host_base.startup_ready_false", {dir});
    ExpectWorkerOk(w);
}

// ─── Validate-only: worker reports ready=true then exits ────────────────────

TEST_F(RoleHostBaseLifecycleTest, ValidateMode_ReadyThenExitsWithoutLoop)
{
    auto dir = unique_dir("validate");
    auto w   = SpawnWorker("role_host_base.validate_mode", {dir});
    ExpectWorkerOk(w);
}

// ─── shutdown_ is idempotent ────────────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Shutdown_Idempotent)
{
    auto dir = unique_dir("idempotent");
    auto w   = SpawnWorker("role_host_base.shutdown_idempotent", {dir});
    ExpectWorkerOk(w);
}

// ─── Shutdown before startup_ is valid and harmless ─────────────────────────

TEST_F(RoleHostBaseLifecycleTest, ShutdownBeforeStartup_Harmless)
{
    auto dir = unique_dir("before_startup");
    auto w   = SpawnWorker("role_host_base.shutdown_before_startup", {dir});
    ExpectWorkerOk(w);
}

// ─── Dtor contract: missing shutdown_ call triggers PLH_PANIC ──────────────
//
// The worker deliberately drops a host without calling shutdown_(). The base
// dtor's missing-flag check invokes PLH_PANIC → std::abort(), so the worker
// exits with a signal and the parent sees non-zero exit + the panic message
// in stderr. This replaces the in-process EXPECT_DEATH in the old version.

TEST_F(RoleHostBaseLifecycleTest, DtorContract_MissingShutdown_Aborts)
{
    auto dir = unique_dir("dtor_missing");
    expect_panic_abort("role_host_base.dtor_missing_shutdown_aborts", dir,
                       "RoleHostBase destructor entered without shutdown_");
}

// ─── Virtual shutdown_ override calling base is allowed ─────────────────────

TEST_F(RoleHostBaseLifecycleTest, VirtualShutdown_Override_ForwardsToBase)
{
    auto dir = unique_dir("ovr_ok");
    auto w   = SpawnWorker("role_host_base.virtual_shutdown_override_forwards",
                           {dir});
    ExpectWorkerOk(w);
}

// ─── Virtual shutdown_ override WITHOUT base call → abort ───────────────────

TEST_F(RoleHostBaseLifecycleTest, VirtualShutdown_OverrideWithoutBase_Aborts)
{
    auto dir = unique_dir("ovr_no_base");
    expect_panic_abort("role_host_base.virtual_shutdown_no_base_aborts", dir,
                       "RoleHostBase destructor entered without shutdown_");
}

// ─── External shutdown-flag wiring: triggers loop exit via core_ ────────────

TEST_F(RoleHostBaseLifecycleTest, ExternalShutdownFlag_PropagatesToCore)
{
    auto dir = unique_dir("ext_flag");
    auto w   = SpawnWorker("role_host_base.external_shutdown_flag", {dir});
    ExpectWorkerOk(w);
}

// ─── config() + role_tag() accessors ────────────────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, Accessors_ConfigAndRoleTag)
{
    auto dir = unique_dir("accessors");
    auto w   = SpawnWorker("role_host_base.accessors_config_and_role_tag", {dir});
    ExpectWorkerOk(w);
}

// ─── wait_for_wakeup returns within the timeout ─────────────────────────────

TEST_F(RoleHostBaseLifecycleTest, WaitForWakeup_HonoursTimeoutWithoutHang)
{
    auto dir = unique_dir("wakeup");
    auto w   = SpawnWorker("role_host_base.wait_for_wakeup_honours_timeout",
                           {dir});
    ExpectWorkerOk(w);
}
