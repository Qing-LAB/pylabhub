/**
 * @file test_filelock.cpp
 * @brief Pattern 3 driver: FileLock multi-process tests.
 *
 * Original V1: the suite installed a process-wide LifecycleGuard via
 * SetUpTestSuite so some test bodies could hold a FileLock in the gtest
 * runner while spawning a contender worker.  Per HEP-CORE-0001
 * § "Testing implications" the parent must not own the lifecycle; every
 * FileLock construction now happens inside a worker subprocess.
 *
 * For tests that need cross-process contention, the parent spawns TWO
 * workers: a `filelock.lock_holder` that acquires the lock and signals
 * ready, then a contender worker (nonblocking_acquire / try_lock_-
 * nonblocking / parent_child_block) that races the still-held lock.
 * Both are spawned and awaited by this parent; neither requires any
 * in-process lifecycle.
 */
#include "test_patterns.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

class FileLockTest : public IsolatedProcessTest
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

    fs::path unique_dir(const char *label)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_fl_" + std::string(label) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p;
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ── Single-worker tests: parent just spawns + waits ─────────────────────────

TEST_F(FileLockTest, BasicNonBlocking)
{
    auto dir = unique_dir("basic");
    auto path = dir / "basic_resource.txt";
    auto w = SpawnWorker("filelock.test_basic_non_blocking", {path.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, BlockingLock)
{
    auto dir = unique_dir("block");
    auto path = dir / "blocking_resource.txt";
    auto w = SpawnWorker("filelock.test_blocking_lock", {path.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, TimedLock)
{
    auto dir = unique_dir("timed");
    auto path = dir / "timed.txt";
    auto w = SpawnWorker("filelock.test_timed_lock", {path.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, MoveSemantics)
{
    auto dir = unique_dir("move");
    auto p1 = dir / "move1.txt";
    auto p2 = dir / "move2.txt";
    auto w  = SpawnWorker("filelock.test_move_semantics",
                          {p1.string(), p2.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, DirectoryCreation)
{
    auto dir = unique_dir("dircreate");
    auto new_dir = dir / "new_dir_for_lock";
    auto w = SpawnWorker("filelock.test_directory_creation",
                         {new_dir.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, DirectoryPathLocking)
{
    auto dir = unique_dir("dirpath");
    auto to_lock = dir / "dir_to_lock_parent";
    auto w = SpawnWorker("filelock.test_directory_path_locking",
                         {to_lock.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto dir = unique_dir("mt");
    auto path = dir / "multithread.txt";
    auto w = SpawnWorker("filelock.test_multithreaded_non_blocking",
                         {path.string()});
    ExpectWorkerOk(w);
}

// ── Two-worker contention tests: holder + contender ─────────────────────────
//
// These replace the V1 pattern where the parent held the lock in-process
// and spawned a single worker contender. Now:
//   1. Parent spawns `filelock.lock_holder` with ready-signal.
//   2. Holder acquires the lock and signals ready.
//   3. Parent waits for the signal, then spawns the contender worker.
//   4. Contender races the still-held lock.
//   5. Parent awaits both.

TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    auto dir = unique_dir("mp_nb");
    auto path = dir / "multiprocess.txt";

    // Holder acquires + holds for 300ms so the contender's non-blocking
    // try_lock fails while the lock is still held.
    auto holder = SpawnWorkerWithReadySignal(
        "filelock.lock_holder", {path.string(), "300"});
    holder.wait_for_ready();

    auto contender = SpawnWorker("filelock.nonblocking_acquire",
                                 {path.string()});
    ExpectWorkerOk(contender);

    holder.wait_for_exit();
    ExpectWorkerOk(holder);
}

TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto dir = unique_dir("mpbc");
    auto resource_path = dir / "contention_resource.txt";
    auto log_path      = dir / "contention_log.txt";

    constexpr int kProcs = 8;
    constexpr int kIters = 100;

    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < kProcs; ++i)
    {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "filelock.contention_log_access",
            std::vector<std::string>{resource_path.string(),
                                     log_path.string(),
                                     std::to_string(kIters)}));
        ASSERT_TRUE(procs.back()->valid());
    }
    for (auto &p : procs)
    {
        p->wait_for_exit();
        pylabhub::tests::helper::expect_worker_ok(*p);
    }

    // Parse the log and verify mutual exclusion (no overlap between
    // ACQUIRE and RELEASE events across processes).
    struct LogEntry { long long ts; long pid; std::string action; };
    std::ifstream log_stream(log_path);
    ASSERT_TRUE(log_stream.is_open());

    std::vector<LogEntry> entries;
    std::string line;
    while (std::getline(log_stream, line))
    {
        std::stringstream ss(line);
        LogEntry e;
        if (ss >> e.ts >> e.pid >> e.action)
            entries.push_back(e);
    }
    ASSERT_EQ(entries.size(), static_cast<size_t>(kProcs * kIters * 2));

    int lock_held = 0;
    long last_holder = -1;
    for (const auto &e : entries)
    {
        if (e.action == "ACQUIRE")
        {
            ASSERT_EQ(lock_held, 0)
                << "Lock acquired while already held; PID " << e.pid
                << " while " << last_holder << " held at ts=" << e.ts;
            ++lock_held;
            last_holder = e.pid;
        }
        else if (e.action == "RELEASE")
        {
            ASSERT_EQ(lock_held, 1)
                << "Lock released while not held; PID " << e.pid
                << " at ts=" << e.ts;
            ASSERT_EQ(e.pid, last_holder)
                << "Release PID mismatch (holder " << last_holder
                << ", releaser " << e.pid << ")";
            --lock_held;
        }
    }
    ASSERT_EQ(lock_held, 0);
}

TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    auto dir = unique_dir("mp_pc");
    auto path = dir / "parent_child_block.txt";

    // Holder holds for 300ms; contender (parent_child_block) attempts a
    // blocking lock and measures that it blocked for >= 100ms.
    auto holder = SpawnWorkerWithReadySignal(
        "filelock.lock_holder", {path.string(), "300"});
    holder.wait_for_ready();

    auto contender = SpawnWorkerWithReadySignal(
        "filelock.parent_child_block", {path.string()});
    contender.wait_for_ready();

    // Both workers will complete on their own timing; just wait.
    holder.wait_for_exit();
    ExpectWorkerOk(holder);

    contender.wait_for_exit();
    ExpectWorkerOk(contender);
}

TEST_F(FileLockTest, MultiProcessTryLock)
{
    auto dir = unique_dir("mp_try");
    auto path = dir / "multiprocess_try_lock.txt";

    auto holder = SpawnWorkerWithReadySignal(
        "filelock.lock_holder", {path.string(), "300"});
    holder.wait_for_ready();

    auto contender = SpawnWorker("filelock.try_lock_nonblocking",
                                 {path.string()});
    ExpectWorkerOk(contender);

    holder.wait_for_exit();
    ExpectWorkerOk(holder);
}

// ── Single-subprocess tests (no cross-process contention) ───────────────────

TEST_F(FileLockTest, TryLockPattern)
{
    auto dir = unique_dir("trypat");
    auto path = dir / "try_lock_pattern.txt";
    auto w = SpawnWorker("filelock.try_lock_pattern", {path.string()});
    ExpectWorkerOk(w);
}

TEST_F(FileLockTest, UseWithoutLifecycleAborts)
{
    // Worker deliberately aborts via PLH_PANIC. Don't use ExpectWorkerOk —
    // assert non-zero exit + panic text ourselves (same pattern as the
    // RoleHostBase dtor-contract death tests).
    auto w = SpawnWorker("filelock.use_without_lifecycle_aborts", {});
    w.wait_for_exit();
    ASSERT_NE(w.exit_code(), 0);
    EXPECT_THAT(w.get_stderr(),
                ::testing::HasSubstr(
                    "FileLock created before its module was initialized"));
}

TEST_F(FileLockTest, InvalidResourcePath)
{
    auto w = SpawnWorker("filelock.invalid_resource_path", {});
    ExpectWorkerOk(w);
}
