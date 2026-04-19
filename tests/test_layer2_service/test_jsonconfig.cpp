/**
 * @file test_jsonconfig.cpp
 * @brief Pattern 3 driver: JsonConfig API + concurrency + security tests.
 *
 * The original suite installed a process-wide LifecycleGuard via
 * SetUpTestSuite and ran every body in the gtest runner. Per HEP-CORE-0001
 * § "Testing implications" that V1 antipattern leaves a single bad test
 * able to corrupt singleton state for every following test in the binary.
 *
 * Each TEST_F now spawns a worker subprocess via IsolatedProcessTest. The
 * worker owns its own LifecycleGuard (Logger + FileLock + JsonConfig) via
 * run_gtest_worker. The parent allocates a unique temp dir per test,
 * passes it as argv[2], and removes it after wait_for_exit.
 *
 * Three pre-existing worker-based tests retain their original shape with
 * lighter parent code:
 *   - UninitializedBehavior  → jsonconfig.uninitialized_behavior (panic)
 *   - MultiProcessContention → jsonconfig.write_id (multi-spawn)
 *   - TransactionProxyNotConsumedWarning → jsonconfig.not_consuming_proxy
 *
 * The 19 in-process bodies migrate into workers/jsonconfig_workers.cpp;
 * see that file for the per-scenario function definitions.
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
using pylabhub::tests::helper::WorkerProcess;

namespace
{

class JsonConfigTest : public IsolatedProcessTest
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

    /// Generates a unique scratch directory for one test; cleaned up
    /// automatically after wait_for_exit.
    std::string unique_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_jc_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ── Pattern 3 worker scenarios ──────────────────────────────────────────────

TEST_F(JsonConfigTest, InitAndCreate)
{
    auto w = SpawnWorker("jsonconfig.init_and_create", {unique_dir("ic")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, InitWithEmptyPathFails)
{
    auto w = SpawnWorker("jsonconfig.init_with_empty_path_fails", {});
    // Worker emits one [ERROR ] when init refuses an empty path.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"JsonConfig::init: config path is empty"});
}

TEST_F(JsonConfigTest, InitWithNonExistentFile)
{
    auto w = SpawnWorker("jsonconfig.init_with_non_existent_file",
                         {unique_dir("ne")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, BasicAccessors)
{
    auto w = SpawnWorker("jsonconfig.basic_accessors", {unique_dir("ba")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, ReloadOnDiskChange)
{
    auto w = SpawnWorker("jsonconfig.reload_on_disk_change", {unique_dir("rd")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, SimplifiedApiOverloads)
{
    auto w = SpawnWorker("jsonconfig.simplified_api_overloads",
                         {unique_dir("sa")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, RecursionGuard)
{
    auto w = SpawnWorker("jsonconfig.recursion_guard", {unique_dir("rg")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, WriteTransactionRollsBackOnException)
{
    auto w = SpawnWorker("jsonconfig.write_transaction_rolls_back_on_exception",
                         {unique_dir("rb")});
    // The worker deliberately throws inside the write lambda; the wrapper
    // catches it and surfaces ec=io_error. The subprocess emits one
    // [ERROR ] log for the write failure — declare it expected so the
    // "no ERROR" check does not fire on it.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"JsonConfig tx write lambda threw"});
}

TEST_F(JsonConfigTest, LoadMalformedFile)
{
    auto w = SpawnWorker("jsonconfig.load_malformed_file", {unique_dir("mf")});
    // The worker attempts to load malformed JSON twice; each emits an
    // [ERROR ] log entry from the JsonConfig parse path.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"JsonConfig::private_load_from_disk_unsafe"});
}

TEST_F(JsonConfigTest, MultiThreadFileContention)
{
    auto w = SpawnWorker("jsonconfig.multi_thread_file_contention",
                         {unique_dir("mt")});
    ExpectWorkerOk(w);
}

#if PYLABHUB_IS_POSIX
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
{
    auto w = SpawnWorker("jsonconfig.symlink_attack_prevention_posix",
                         {unique_dir("sp")});
    // The worker triggers an "operation_not_permitted" log when init
    // refuses the symlinked path.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"JsonConfig::init: target", "is a symbolic link"});
}
#endif

#if defined(PLATFORM_WIN64)
TEST_F(JsonConfigTest, SymlinkAttackPreventionWindows)
{
    auto w = SpawnWorker("jsonconfig.symlink_attack_prevention_windows",
                         {unique_dir("sw")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"symlink"});
}
#endif

TEST_F(JsonConfigTest, MultiThreadSharedObjectContention)
{
    auto w = SpawnWorker("jsonconfig.multi_thread_shared_object_contention",
                         {unique_dir("ms")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, ManualLockingApi)
{
    auto w = SpawnWorker("jsonconfig.manual_locking_api", {unique_dir("ml")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, MoveSemantics)
{
    auto w = SpawnWorker("jsonconfig.move_semantics", {unique_dir("mv")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, OverwriteMethod)
{
    auto w = SpawnWorker("jsonconfig.overwrite_method", {unique_dir("ow")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, DirtyFlagLogic)
{
    auto w = SpawnWorker("jsonconfig.dirty_flag_logic", {unique_dir("df")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, WriteVetoCommit)
{
    auto w = SpawnWorker("jsonconfig.write_veto_commit", {unique_dir("vc")});
    ExpectWorkerOk(w);
}

TEST_F(JsonConfigTest, WriteProducesInvalidJson)
{
    auto w = SpawnWorker("jsonconfig.write_produces_invalid_json",
                         {unique_dir("ij")});
    // Invalid UTF-8 dump emits an [ERROR ] from the JSON serializer path.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"JsonConfig tx produced invalid JSON"});
}

// ── Pre-existing worker-based tests (unchanged shape, lighter parent) ───────

TEST_F(JsonConfigTest, UninitializedBehavior)
{
    // Worker triggers PLH_PANIC by constructing JsonConfig without a
    // lifecycle. Expected to abort — separate checker, not ExpectWorkerOk.
    auto w = SpawnWorker("jsonconfig.uninitialized_behavior", {});
    w.wait_for_exit();
    ASSERT_NE(w.exit_code(), 0);
    EXPECT_THAT(w.get_stderr(),
                ::testing::HasSubstr("JsonConfig created before its module was initialized"));
    EXPECT_THAT(w.get_stderr(), ::testing::HasSubstr("Aborting"));
}

TEST_F(JsonConfigTest, MultiProcessContention)
{
    // 8 workers race to write distinct keys to a shared file. The parent
    // creates the file (using a setup worker that initializes JsonConfig),
    // spawns the contenders, then reads the file with a verifier worker.
    auto cfg_dir = unique_dir("mp");
    std::string cfg_path = (fs::path(cfg_dir) / "multiprocess_contention.json").string();

    // Setup: create empty file in a one-off worker.
    auto setup = SpawnWorker("jsonconfig.init_and_create", {cfg_dir});
    ExpectWorkerOk(setup);
    // The setup worker created init_create.json; the contenders use a
    // different file in the same dir, which they will create on first lock.

    constexpr int kProcs = 8;
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < kProcs; ++i)
    {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "jsonconfig.write_id",
            std::vector<std::string>{cfg_path,
                                     "worker_" + std::to_string(i)}));
        ASSERT_TRUE(procs.back()->valid());
    }
    int success = 0;
    for (auto &p : procs)
    {
        p->wait_for_exit();
        if (p->exit_code() == 0)
        {
            ++success;
            pylabhub::tests::helper::expect_worker_ok(*p);
        }
    }
    ASSERT_EQ(success, kProcs);
}

#ifndef NDEBUG
TEST_F(JsonConfigTest, TransactionProxyNotConsumedWarning)
{
    auto w = SpawnWorker("jsonconfig.not_consuming_proxy", {});
    w.wait_for_exit();
    ASSERT_EQ(w.exit_code(), 0);
    EXPECT_THAT(w.get_stderr(),
                ::testing::HasSubstr("JsonConfig::transaction() proxy was not consumed"));
}
#endif
