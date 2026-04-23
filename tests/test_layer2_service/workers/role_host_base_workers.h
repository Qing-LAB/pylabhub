#pragma once
/**
 * @file role_host_base_workers.h
 * @brief Worker functions for RoleHostBase lifecycle tests (Pattern 3).
 *
 * Each worker runs in a freshly spawned subprocess. The "abort" workers
 * deliberately trigger PLH_PANIC → std::abort(); the parent verifies
 * exit_code != 0 and "EngineHost destructor entered without shutdown_"
 * appears in stderr. All other workers wrap their body in run_gtest_worker()
 * which owns the LifecycleGuard (Logger + FileLock + JsonConfig).
 */

#include <string>

namespace pylabhub::tests::worker
{
namespace role_host_base
{

// ── Happy-path + construction workers ───────────────────────────────────────

/** Construct only; assert is_running/script_load_ok false, role_tag == "test". */
int construct_not_running(const std::string &dir);

/** Startup → worker enters loop → shutdown joins cleanly. */
int startup_run_shutdown(const std::string &dir);

/** Worker reports ready=false; host stays not-running. */
int startup_ready_false(const std::string &dir);

/** Validate mode: ready=true but worker exits without entering loop. */
int validate_mode(const std::string &dir);

/** Shutdown is idempotent (multiple calls harmless). */
int shutdown_idempotent(const std::string &dir);

/** Shutdown before startup_ is a valid no-op. */
int shutdown_before_startup(const std::string &dir);

/** Virtual shutdown override that forwards to base — normal clean exit. */
int virtual_shutdown_override_forwards(const std::string &dir);

/** External shutdown flag flips → loop exits via core_.is_process_exit_requested. */
int external_shutdown_flag(const std::string &dir);

/** config() + role_tag() accessor stability across calls. */
int accessors_config_and_role_tag(const std::string &dir);

/** wait_for_wakeup returns within bounded time. */
int wait_for_wakeup_honours_timeout(const std::string &dir);

// ── Abort workers (deliberately die via PLH_PANIC → abort()) ─────────────────
//
// These do NOT use run_gtest_worker: we WANT the LifecycleGuard teardown
// path to be skipped and the process to die so the parent can inspect the
// panic signature. The workers set up Logger lifecycle manually, run the
// aborting body, and never reach a clean return.

/** Drop host without calling shutdown_ → base dtor PLH_PANIC. */
int dtor_missing_shutdown_aborts(const std::string &dir);

/** Override shutdown_ but don't call RoleHostBase::shutdown_() → base dtor panics. */
int virtual_shutdown_no_base_aborts(const std::string &dir);

} // namespace role_host_base
} // namespace pylabhub::tests::worker
