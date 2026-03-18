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

TEST_F(DatahubMutexTest, TryLockFor_TimeoutConvention)
{
    // Verifies the try_lock_for() API contract for all three parameter classes
    // on a free mutex.  The bug was that both Windows and POSIX mapped -1
    // (infinite wait) to non-blocking, causing try_lock_for(-1) to return false
    // when the mutex was held.  On a free mutex, all three modes must succeed.
    //
    // Single-process, no timing dependencies — purely tests return values.
    std::string shm_name = make_test_channel_name("DBMutexTimeout");
    pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);

    // -1 = infinite wait: must succeed immediately on free mutex
    ASSERT_TRUE(mutex.try_lock_for(-1)) << "try_lock_for(-1) must succeed on free mutex";
    mutex.unlock();

    // 0 = non-blocking: must succeed immediately on free mutex
    ASSERT_TRUE(mutex.try_lock_for(0)) << "try_lock_for(0) must succeed on free mutex";
    mutex.unlock();

    // >0 = timed wait: must succeed immediately on free mutex
    ASSERT_TRUE(mutex.try_lock_for(100)) << "try_lock_for(100) must succeed on free mutex";
    mutex.unlock();

    // Verify lock() (infinite, separate code path) also works
    mutex.lock();
    mutex.unlock();
}

// Helper: extract a uint64_t value from stderr output matching "KEY=<value>"
static uint64_t extract_timestamp(std::string_view stderr_out, const char *key)
{
    std::string prefix = std::string(key) + "=";
    auto pos = stderr_out.find(prefix);
    EXPECT_NE(pos, std::string_view::npos) << "Missing " << key << " in stderr output";
    if (pos == std::string_view::npos)
        return 0;
    auto val_start = pos + prefix.size();
    auto val_end = stderr_out.find('\n', val_start);
    std::string val_str(stderr_out.substr(val_start, val_end - val_start));
    return std::stoull(val_str);
}

TEST_F(DatahubMutexTest, TryLockForInfinite_BlocksUnderContention)
{
    // Multi-process test for try_lock_for(-1) under contention.
    //
    // Three timestamped events:
    //   T1 (TRY_TS):      attacher prints just before calling try_lock_for(-1)
    //   T2 (RELEASE_TS):  creator prints just before unlock()
    //   T3 (ACQUIRED_TS): attacher prints just after try_lock_for(-1) returns true
    //
    // Invariants:
    //   T1 < T2:  attacher started trying before creator released (creator holds 500ms)
    //   T2 <= T3: attacher acquired after creator released
    //   T3 - T1 >= 200ms: attacher was blocked for a substantial time (not instant return)
    //
    // If the bug existed (try_lock_for(-1) → non-blocking), either:
    //   - try_lock_for returns false → attacher exits 1 → test fails, or
    //   - T3 ≈ T1 (instant return) and T3 < T2 → ordering invariant fails.
    std::string shm_name = make_test_channel_name("DBMutexInfContend");
    auto creator = SpawnWorkerWithReadySignal("datablock_mutex.acquire_creator_signal_then_hold",
                                              {shm_name});
    creator.wait_for_ready(); // Creator holds lock now
    auto attacher = SpawnWorker("datablock_mutex.try_lock_infinite_with_timestamps", {shm_name});
    creator.wait_for_exit();
    attacher.wait_for_exit();

    ASSERT_EQ(creator.exit_code(), 0) << "Creator failed:\n" << creator.get_stderr();
    ASSERT_EQ(attacher.exit_code(), 0) << "Attacher failed:\n" << attacher.get_stderr();

    // Parse timestamps from worker stderr output
    uint64_t t1_try      = extract_timestamp(attacher.get_stderr(), "TRY_TS");
    uint64_t t2_release   = extract_timestamp(creator.get_stderr(), "RELEASE_TS");
    uint64_t t3_acquired  = extract_timestamp(attacher.get_stderr(), "ACQUIRED_TS");

    // Print parsed values for diagnostic verification
    std::fprintf(stderr,
        "  Creator stderr:  %s\n"
        "  Attacher stderr: %s\n"
        "  T1(try)=%lu  T2(release)=%lu  T3(acquired)=%lu\n"
        "  T2-T1 = %lu ms (lock held after attacher started trying)\n"
        "  T3-T2 = %lu ms (acquire latency after release)\n"
        "  T3-T1 = %lu ms (total blocked time)\n",
        std::string(creator.get_stderr()).c_str(),
        std::string(attacher.get_stderr()).c_str(),
        static_cast<unsigned long>(t1_try),
        static_cast<unsigned long>(t2_release),
        static_cast<unsigned long>(t3_acquired),
        static_cast<unsigned long>((t2_release - t1_try) / 1'000'000),
        static_cast<unsigned long>((t3_acquired - t2_release) / 1'000'000),
        static_cast<unsigned long>((t3_acquired - t1_try) / 1'000'000));

    EXPECT_LT(t1_try, t2_release)
        << "Attacher must have started trying (T1) before creator released (T2)";
    EXPECT_LE(t2_release, t3_acquired)
        << "Attacher must have acquired (T3) after creator released (T2)";

    // Attacher was blocked for at least 200ms (creator holds for 500ms after ready signal,
    // attacher is spawned after ready, so there's ample contention window).
    uint64_t blocked_ns = t3_acquired - t1_try;
    constexpr uint64_t kMinBlockedNs = 200'000'000; // 200ms
    EXPECT_GE(blocked_ns, kMinBlockedNs)
        << "Attacher should have been blocked >= 200ms, but only blocked "
        << (blocked_ns / 1'000'000) << "ms — try_lock_for(-1) may not be waiting";
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
