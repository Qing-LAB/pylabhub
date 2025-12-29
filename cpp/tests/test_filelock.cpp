// tests/test_filelock.cpp
//
// Test harness for pylabhub::utils::FileLock.
// Each test case is executed in a separate worker process to ensure full
// isolation of the lifecycle-managed components.

#include "test_preamble.h"

// Specific includes for this test file that are not covered by the preamble
#include "helpers/test_entrypoint.h"
#include "helpers/test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
using namespace test_utils;

class FileLockTest : public ::testing::Test {
protected:
    static fs::path g_temp_dir_;

    static void SetUpTestSuite()
    {
        g_temp_dir_ = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir_);
    }

    static void TearDownTestSuite()
    {
        try
        {
            fs::remove_all(g_temp_dir_);
        }
        catch (...)
        {
        }
    }

    fs::path temp_dir() const { return g_temp_dir_; }

    void clear_lock_file(const fs::path &resource_path, pylabhub::utils::ResourceType type)
    {
        try
        {
            fs::remove(pylabhub::utils::FileLock::get_expected_lock_fullname_for(resource_path, type));
        }
        catch (...)
        {
        }
    }
};

fs::path FileLockTest::g_temp_dir_;

TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = temp_dir() / "basic_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_basic_non_blocking",
                                              {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = temp_dir() / "blocking_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_blocking_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, TimedLock)
{
    auto resource_path = temp_dir() / "timed.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_timed_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MoveSemantics)
{
    auto resource1 = temp_dir() / "move1.txt";
    auto resource2 = temp_dir() / "move2.txt";
    clear_lock_file(resource1, pylabhub::utils::ResourceType::File);
    clear_lock_file(resource2, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_move_semantics", {resource1.string(), resource2.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = temp_dir() / "new_dir_for_lock";
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_directory_creation",
                                              {new_dir.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, DirectoryPathLocking)
{
    auto dir_to_lock = temp_dir() / "dir_to_lock_parent";
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_directory_path_locking", {dir_to_lock.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto resource_path = temp_dir() / "multithread.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_multithreaded_non_blocking", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    auto resource_path = temp_dir() / "multiprocess.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);

    pylabhub::utils::FileLock main_lock(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(main_lock.valid());

    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.nonblocking_acquire", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto counter_path = temp_dir() / "counter.txt";
    fs::remove(counter_path);
    clear_lock_file(counter_path, pylabhub::utils::ResourceType::File);
    {
        std::ofstream ofs(counter_path);
        ofs << 0;
    }

    const int PROCS = 8; // Reduced for faster test runs
    const int ITERS_PER_WORKER = 25;
    
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle h = spawn_worker_process(g_self_exe_path, "filelock.contention_increment",
                                        {counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        ASSERT_NE(h, NULL_PROC_HANDLE);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        ASSERT_EQ(wait_for_worker_and_get_exit_code(h), 0);
    }

    int final_value = 0;
    {
        std::ifstream ifs(counter_path);
        if (ifs.is_open()) { ifs >> final_value; }
    }
    ASSERT_EQ(final_value, PROCS * ITERS_PER_WORKER);
}

TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    auto resource_path = temp_dir() / "parent_child_block.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    
    auto parent_lock = std::make_unique<pylabhub::utils::FileLock>(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(parent_lock->valid());

    ProcessHandle child_proc = spawn_worker_process(g_self_exe_path, "filelock.parent_child_block", {resource_path.string()});
    ASSERT_NE(child_proc, NULL_PROC_HANDLE);

    std::this_thread::sleep_for(200ms);
    parent_lock.reset(); 

    ASSERT_EQ(wait_for_worker_and_get_exit_code(child_proc), 0);
}
