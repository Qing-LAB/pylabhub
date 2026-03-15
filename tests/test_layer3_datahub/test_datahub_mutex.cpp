/**
 * @file test_datablock_mutex.cpp
 * @brief DataBlockMutex multi-process tests: normal paths and error paths.
 *
 * Tests cross-process mutex semantics: creator/attacher acquire-release,
 * zombie owner recovery (EOWNERDEAD/WAIT_ABANDONED), and attach-failure error path.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <ctime>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubMutexTest : public IsolatedProcessTest
{
};

TEST_F(DatahubMutexTest, CreatorAcquiresAndReleases)
{
    std::string shm_name = make_test_channel_name("DBMutexCreator");
    auto proc = SpawnWorker("datablock_mutex.acquire_and_release_creator", {shm_name});
    ExpectWorkerOk(proc, {"Mutex acquired", "Mutex released"});
}

TEST_F(DatahubMutexTest, AttacherAcquiresAfterCreator)
{
    // Creator unlinks shm on exit; attacher must attach while creator is still alive.
    // Use ready-signal pipe for deterministic ordering: creator signals when mutex is created and
    // held, parent spawns attacher, attacher blocks until creator releases.
    std::string shm_name = make_test_channel_name("DBMutexSeq");
    auto creator = SpawnWorkerWithReadySignal("datablock_mutex.acquire_and_release_creator_hold_long",
                                              {shm_name});
    creator.wait_for_ready();
    auto attacher = SpawnWorker("datablock_mutex.acquire_and_release_attacher", {shm_name});
    creator.wait_for_exit();
    attacher.wait_for_exit();
    pylabhub::tests::helper::expect_worker_ok(creator, {"Mutex acquired", "Mutex released"});
    pylabhub::tests::helper::expect_worker_ok(attacher, {"Mutex acquired", "Mutex released"});
}

TEST_F(DatahubMutexTest, ZombieOwnerRecovery)
{
#if !PYLABHUB_IS_POSIX
    GTEST_SKIP() << "Zombie owner (EOWNERDEAD) recovery tested on POSIX only";
#elif defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "ZombieOwnerRecovery skipped under ThreadSanitizer: robust mutex EOWNERDEAD "
                    "recovery triggers a false positive (unlock of unlocked mutex)";
#else
    std::string shm_name = make_test_channel_name("DBMutexZombie");
    auto zombie = SpawnWorker("datablock_mutex.zombie_creator_acquire_then_exit", {shm_name});
    zombie.wait_for_exit();
    EXPECT_EQ(zombie.exit_code(), 0) << "Zombie worker should exit 0 (clean _exit)";

    auto recoverer = SpawnWorker("datablock_mutex.zombie_attacher_recovers", {shm_name});
    ExpectWorkerOk(recoverer, {"Mutex acquired", "Mutex released"});
#endif
}

TEST_F(DatahubMutexTest, AttachNonexistentFails)
{
    std::string shm_name =
        "test_nonexistent_mutex_" + std::to_string(static_cast<unsigned long>(std::time(nullptr)));
    auto proc = SpawnWorker("datablock_mutex.attach_nonexistent_fails", {shm_name});
    proc.wait_for_exit();
    EXPECT_NE(proc.exit_code(), 0) << "Attach/open of nonexistent mutex should fail";
    std::string_view stderr_out = proc.get_stderr();
    EXPECT_TRUE(stderr_out.find("attach") != std::string_view::npos ||
                stderr_out.find("open") != std::string_view::npos ||
                stderr_out.find("Failed") != std::string_view::npos)
        << "Stderr should mention attach/open failure";
}
