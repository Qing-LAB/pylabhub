/**
 * @file test_filelock_singleprocess.cpp
 * @brief Pattern 3 driver: in-process FileLock tests run in worker subprocesses.
 *
 * Each TEST_F spawns one worker via IsolatedProcessTest::SpawnWorker. The
 * worker owns its own LifecycleGuard (Logger + FileLock) and constructs
 * FileLock objects against a parent-provided unique resource path. The
 * parent removes the resource and its .lock / .dir.lock siblings after
 * wait_for_exit. This honours the contract from
 * docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md § "Testing implications".
 */
#include "test_patterns.h"
#include "utils/file_lock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::utils::FileLock;

namespace
{

class FileLockSingleProcessTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                auto file_lock = FileLock::get_expected_lock_fullname_for(p);
                if (fs::exists(file_lock))
                    fs::remove(file_lock);
                auto dir_lock =
                    FileLock::get_expected_lock_fullname_for(p, /*is_directory=*/true);
                if (fs::exists(dir_lock))
                    fs::remove(dir_lock);
                if (fs::exists(p))
                {
                    if (fs::is_directory(p))
                        fs::remove_all(p);
                    else
                        fs::remove(p);
                }
            }
            catch (...)
            {
            }
        }
        paths_to_clean_.clear();
    }

    /// Returns a unique resource path; auto-cleaned after the test.
    std::string make_resource(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_flsp_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)) + ".txt");
        paths_to_clean_.push_back(p);
        return p.string();
    }

    /// Returns a unique directory path; auto-cleaned after the test.
    std::string make_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_flsp_dir_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(FileLockSingleProcessTest, BasicNonBlocking)
{
    auto p = make_resource("basic_nonblocking");
    auto w = SpawnWorker("filelock_singleprocess.basic_nonblocking", {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, BlockingLockTimeout)
{
    auto p = make_resource("blocking_timeout");
    auto w = SpawnWorker("filelock_singleprocess.blocking_lock_timeout", {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, MultiThreadedContention)
{
    auto p = make_resource("multithread");
    auto w = SpawnWorker("filelock_singleprocess.multi_threaded_contention", {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, MoveSemantics)
{
    auto p1 = make_resource("move1");
    auto p2 = make_resource("move2");
    auto w  = SpawnWorker("filelock_singleprocess.move_semantics", {p1, p2});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, DirectoryPathLocking)
{
    auto d = make_dir("dirlock");
    auto w = SpawnWorker("filelock_singleprocess.directory_path_locking", {d});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, TimedLock)
{
    auto p = make_resource("timed");
    auto w = SpawnWorker("filelock_singleprocess.timed_lock", {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, SequentialAcquireRelease)
{
    auto p = make_resource("sequential");
    auto w = SpawnWorker("filelock_singleprocess.sequential_acquire_release",
                         {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, GetExpectedLockFullnameFor)
{
    auto fp = make_resource("get_expected_file");
    auto dp = make_dir("get_expected_dir");
    auto w  = SpawnWorker(
        "filelock_singleprocess.get_expected_lock_fullname_for", {fp, dp});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, GetLockedResourceAndCanonicalLockPath)
{
    auto p = make_resource("path_getters");
    auto w = SpawnWorker(
        "filelock_singleprocess.get_locked_resource_and_canonical_lock_path",
        {p});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, GetPathsReturnEmptyWhenInvalid)
{
    auto w = SpawnWorker(
        "filelock_singleprocess.get_paths_return_empty_when_invalid", {});
    ExpectWorkerOk(w);
}

TEST_F(FileLockSingleProcessTest, InvalidResourcePath)
{
    auto w = SpawnWorker("filelock_singleprocess.invalid_resource_path", {});
    ExpectWorkerOk(w);
}
