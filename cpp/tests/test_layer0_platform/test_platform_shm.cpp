/**
 * @file test_platform_shm.cpp
 * @brief Layer 0 tests for platform shared memory API (shm_create, shm_attach, shm_close, shm_unlink).
 *
 * Part 0 of DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md: foundational APIs used by DataBlock.
 * These tests must run on all supported platforms (Windows, Linux, macOS, FreeBSD).
 */
#include "plh_platform.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>

using namespace pylabhub::platform;

namespace
{
/** Returns a process-unique name for shared memory. Portable: POSIX uses leading slash. */
std::string unique_shm_name()
{
    static std::atomic<uint64_t> counter{0};
    uint64_t id = get_pid() * 1000000u + counter.fetch_add(1, std::memory_order_relaxed);
#if defined(PYLABHUB_IS_POSIX)
    return "/pylabhub_test_shm_" + std::to_string(id);
#else
    return "pylabhub_test_shm_" + std::to_string(id);
#endif
}
} // namespace

// ============================================================================
// shm_create
// ============================================================================

TEST(PlatformShmTest, ShmCreate_ReturnsValidHandle)
{
    std::string name = unique_shm_name();
    ShmHandle h = shm_create(name.c_str(), 4096, SHM_CREATE_UNLINK_FIRST);
    EXPECT_NE(h.base, nullptr) << "shm_create should return non-null base";
    EXPECT_EQ(h.size, 4096u);
    shm_close(&h);
    shm_unlink(name.c_str());
    EXPECT_EQ(h.base, nullptr);
}

TEST(PlatformShmTest, ShmCreate_ZeroSize_Fails)
{
    std::string name = unique_shm_name();
    ShmHandle h = shm_create(name.c_str(), 0);
    EXPECT_EQ(h.base, nullptr);
    EXPECT_EQ(h.size, 0u);
}

TEST(PlatformShmTest, ShmCreate_NullName_Fails)
{
    ShmHandle h = shm_create(nullptr, 4096);
    EXPECT_EQ(h.base, nullptr);
}

// ============================================================================
// shm_attach (same process: create then attach)
// ============================================================================

TEST(PlatformShmTest, ShmAttach_AfterCreate_SameProcess_Succeeds)
{
    std::string name = unique_shm_name();
    ShmHandle creator = shm_create(name.c_str(), 8192, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(creator.base, nullptr);

    ShmHandle attacher = shm_attach(name.c_str());
    EXPECT_NE(attacher.base, nullptr) << "shm_attach should succeed when segment exists";
    EXPECT_EQ(attacher.size, 8192u);

    shm_close(&attacher);
    shm_close(&creator);
    shm_unlink(name.c_str());
}

TEST(PlatformShmTest, ShmAttach_Nonexistent_Fails)
{
    std::string name = unique_shm_name();
    ShmHandle h = shm_attach(name.c_str());
    EXPECT_EQ(h.base, nullptr) << "shm_attach to nonexistent segment should fail";
}

// ============================================================================
// Read/write and close/unlink
// ============================================================================

TEST(PlatformShmTest, ShmCreate_WriteThenAttach_ReadSameData)
{
    std::string name = unique_shm_name();
    const size_t size = 4096;
    ShmHandle creator = shm_create(name.c_str(), size, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(creator.base, nullptr);

    const char *msg = "hello shared memory";
    std::memcpy(creator.base, msg, std::strlen(msg) + 1);

    ShmHandle reader = shm_attach(name.c_str());
    ASSERT_NE(reader.base, nullptr);
    EXPECT_EQ(reader.size, size);
    EXPECT_STREQ(static_cast<const char *>(reader.base), msg);

    shm_close(&reader);
    shm_close(&creator);
    shm_unlink(name.c_str());
}

TEST(PlatformShmTest, ShmClose_InvalidatesHandle)
{
    std::string name = unique_shm_name();
    ShmHandle h = shm_create(name.c_str(), 4096, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(h.base, nullptr);
    shm_close(&h);
    EXPECT_EQ(h.base, nullptr);
    EXPECT_EQ(h.size, 0u);
    shm_unlink(name.c_str());
}

TEST(PlatformShmTest, ShmUnlink_AfterClose_AttachFails)
{
    std::string name = unique_shm_name();
    ShmHandle h = shm_create(name.c_str(), 4096, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(h.base, nullptr);
    shm_close(&h);
    shm_unlink(name.c_str());

    ShmHandle h2 = shm_attach(name.c_str());
    EXPECT_EQ(h2.base, nullptr) << "After unlink, attach should fail (POSIX); on Windows name may still be valid until refcount drops.";
}

// ============================================================================
// SHM_CREATE_EXCLUSIVE (create only if segment does not exist)
// ============================================================================

TEST(PlatformShmTest, ShmCreate_Exclusive_FailsWhenSegmentExists)
{
    std::string name = unique_shm_name();
    ShmHandle h1 = shm_create(name.c_str(), 1024, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(h1.base, nullptr);

    // Second create with EXCLUSIVE should fail when segment already exists
    ShmHandle h2 = shm_create(name.c_str(), 2048, SHM_CREATE_EXCLUSIVE);
    EXPECT_EQ(h2.base, nullptr) << "SHM_CREATE_EXCLUSIVE should fail when segment exists";

    shm_close(&h1);
    shm_unlink(name.c_str());
}

// ============================================================================
// SHM_CREATE_UNLINK_FIRST (POSIX: clean slate; Windows: no-op)
// ============================================================================

TEST(PlatformShmTest, ShmCreate_UnlinkFirst_AllowsRecreate)
{
    std::string name = unique_shm_name();
    ShmHandle h1 = shm_create(name.c_str(), 1024, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(h1.base, nullptr);
    shm_close(&h1);
    shm_unlink(name.c_str());

    ShmHandle h2 = shm_create(name.c_str(), 2048, SHM_CREATE_UNLINK_FIRST);
    EXPECT_NE(h2.base, nullptr) << "Recreate with UNLINK_FIRST should succeed";
    EXPECT_EQ(h2.size, 2048u);
    shm_close(&h2);
    shm_unlink(name.c_str());
}

// ============================================================================
// Multi-process shared memory (POSIX: parent creates, child attaches)
// ============================================================================

#if defined(PYLABHUB_IS_POSIX)
#include <unistd.h>
#include <sys/wait.h>

TEST(PlatformShmTest, ShmCreate_InParent_AttachInChild_MultiProcess)
{
    std::string name = unique_shm_name();
    const size_t size = 4096;
    const char *msg = "parent-to-child";

    ShmHandle creator = shm_create(name.c_str(), size, SHM_CREATE_UNLINK_FIRST);
    ASSERT_NE(creator.base, nullptr);
    std::memcpy(creator.base, msg, std::strlen(msg) + 1);

    pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed";

    if (pid == 0)
    {
        // Child: attach and read
        ShmHandle attacher = shm_attach(name.c_str());
        if (attacher.base == nullptr)
            _exit(1);
        if (attacher.size != size)
            _exit(2);
        if (std::strcmp(static_cast<const char *>(attacher.base), msg) != 0)
            _exit(3);
        shm_close(&attacher);
        _exit(0);
    }

    // Parent: wait for child
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "Child exit code: " << WEXITSTATUS(status);

    shm_close(&creator);
    shm_unlink(name.c_str());
}
#endif // PYLABHUB_IS_POSIX
