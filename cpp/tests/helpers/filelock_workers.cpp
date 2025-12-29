#include "test_preamble.h" // New common preamble

#include "worker_filelock.h"     // Keep this specific header
#include "shared_test_helpers.h" // Keep this specific helper header
#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
using namespace test_utils;

namespace worker
{
namespace filelock
{

// Prototypes for helpers
static bool atomic_write_int(const fs::path &target, int value);
static bool native_read_int(const fs::path &target, int &value);

int test_basic_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            {
                FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock.valid());
                ASSERT_FALSE(lock.error_code());

                FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_FALSE(lock2.valid());
            }
            FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
            ASSERT_TRUE(lock3.valid());
        },
        "filelock::test_basic_non_blocking");
}

int test_blocking_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            std::atomic<bool> thread_valid{false};
            std::atomic<bool> thread_saw_block{false};

            auto main_lock =
                std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
            ASSERT_TRUE(main_lock->valid());

            std::thread t1([&]() {
                auto start = std::chrono::steady_clock::now();
                FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
                auto end = std::chrono::steady_clock::now();
                if (thread_lock.valid()) thread_valid.store(true);
                if (std::chrono::duration_cast<std::chrono::milliseconds>(end - start) > 100ms)
                    thread_saw_block.store(true);
            });

            std::this_thread::sleep_for(200ms);
            main_lock.reset(); // Release lock
            t1.join();

            ASSERT_TRUE(thread_valid.load());
            ASSERT_TRUE(thread_saw_block.load());
        },
        "filelock::test_blocking_lock");
}

int test_timed_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            {
                FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
                ASSERT_TRUE(main_lock.valid());

                auto start = std::chrono::steady_clock::now();
                FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
                auto end = std::chrono::steady_clock::now();

                ASSERT_FALSE(timed_lock_fail.valid());
                ASSERT_EQ(timed_lock_fail.error_code(), std::errc::timed_out);
                ASSERT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start),
                          100ms);
            }

            FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
            ASSERT_TRUE(timed_lock_succeed.valid());
        },
        "filelock::test_timed_lock");
}

int test_move_semantics(const std::string &resource1_str, const std::string &resource2_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource1(resource1_str);
            fs::path resource2(resource2_str);

            {
                FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1.valid());
                FileLock lock2(std::move(lock1));
                ASSERT_TRUE(lock2.valid());
                ASSERT_FALSE(lock1.valid());
            }
            {
                FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1_again.valid());
            }
        },
        "filelock::test_move_semantics");
}

int test_directory_creation(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path new_dir(base_dir_str);
            auto resource_to_lock = new_dir / "resource.txt";
            auto actual_lock_file =
                FileLock::get_expected_lock_fullname_for(resource_to_lock, ResourceType::File);

            fs::remove_all(new_dir);
            ASSERT_FALSE(fs::exists(new_dir));
            {
                FileLock lock(resource_to_lock, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock.valid());
                ASSERT_TRUE(fs::exists(new_dir));
                ASSERT_TRUE(fs::exists(actual_lock_file));
            }
        },
        "filelock::test_directory_creation");
}

int test_directory_path_locking(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path base_dir(base_dir_str);
            auto dir_to_lock = base_dir / "dir_to_lock";
            fs::create_directory(dir_to_lock);

            auto expected_dir_lock_file =
                FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
            FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
            ASSERT_TRUE(lock.valid());
            ASSERT_TRUE(fs::exists(expected_dir_lock_file));
        },
        "filelock::test_directory_path_locking");
}

int test_multithreaded_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            const int THREADS = 32;
            std::atomic<int> success_count{0};
            std::vector<std::thread> threads;
            threads.reserve(THREADS);
            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back([&, i]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
                    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                    if (lock.valid())
                    {
                        success_count.fetch_add(1);
                        std::this_thread::sleep_for(50ms);
                    }
                });
            }
            for (auto &t : threads) t.join();
            ASSERT_EQ(success_count.load(), 1);
        },
        "filelock::test_multithreaded_non_blocking");
}

int nonblocking_acquire(const std::string &resource_path_str)
{
    return run_gtest_worker( [&]() {
        FileLock lock(resource_path_str, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock.valid());
     }, "filelock::nonblocking_acquire");
}

int contention_increment(const std::string &counter_path_str, int num_iterations)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(counter_path_str);
            for (int i = 0; i < num_iterations; ++i)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
                FileLock filelock(resource_path, ResourceType::File, LockMode::Blocking);
                ASSERT_TRUE(filelock.valid());
                auto locked_path_opt = filelock.get_locked_resource_path();
                ASSERT_TRUE(locked_path_opt);
                int current_value = 0;
                native_read_int(*locked_path_opt, current_value);
                ASSERT_TRUE(atomic_write_int(*locked_path_opt, current_value + 1));
            }
        },
        "filelock::contention_increment");
}

int parent_child_block(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            auto start = std::chrono::steady_clock::now();
            FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
            auto end = std::chrono::steady_clock::now();
            ASSERT_TRUE(lock.valid());
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            ASSERT_GE(dur.count(), 100);
        },
        "filelock::parent_child_block");
}

// --- Helper implementations that are not tests themselves ---
static bool atomic_write_int(const fs::path &target, int value)
{
    auto tmp = target;
    tmp += ".tmp." + std::to_string(
#if defined(PLATFORM_WIN64)
                           static_cast<unsigned long>(GetCurrentProcessId())
#else
                           static_cast<unsigned long>(getpid())
#endif
                       );
#if defined(PLATFORM_WIN64)
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto guard = pylabhub::basics::make_scope_guard([&]() { CloseHandle(h); std::error_code ec; fs::remove(tmp, ec); });
    std::string val_str = std::to_string(value);
    DWORD bytes_written = 0;
    if (!WriteFile(h, val_str.c_str(), static_cast<DWORD>(val_str.length()), &bytes_written, NULL) || bytes_written != val_str.length()) return false;
    if (!FlushFileBuffers(h)) return false;
#else
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) return false;
    auto guard = pylabhub::basics::make_scope_guard([&]() { ::close(fd); std::error_code ec; fs::remove(tmp, ec); });
    std::string val_str = std::to_string(value);
    if (::write(fd, val_str.c_str(), val_str.length()) != static_cast<ssize_t>(val_str.length())) return false;
    if (::fsync(fd) != 0) return false;
#endif
#if !defined(PLATFORM_WIN64)
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) return false;
#else
    if (!::MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
#endif
    guard.invoke();
#if !defined(PLATFORM_WIN64)
    int dfd = ::open(target.parent_path().c_str(), O_DIRECTORY | O_RDONLY);
    if (dfd >= 0) { fsync(dfd); ::close(dfd); }
#endif
    return true;
}
static bool native_read_int(const fs::path &target, int &value)
{
#if defined(PLATFORM_WIN64)
    HANDLE h = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { value = 0; return false; }
    auto guard = pylabhub::basics::make_scope_guard([&]() { CloseHandle(h); });
    char buffer[128];
    DWORD bytes_read = 0;
    if (!ReadFile(h, buffer, sizeof(buffer) - 1, &bytes_read, NULL)) { value = 0; return false; }
    buffer[bytes_read] = '\0';
    try { value = std::stoi(buffer); } catch (...) { value = 0; return false; }
    guard.invoke();
    return true;
#else
    int fd = ::open(target.c_str(), O_RDONLY);
    if (fd == -1) { value = 0; return false; }
    auto guard = pylabhub::basics::make_scope_guard([&]() { ::close(fd); });
    char buffer[128];
    ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) { value = 0; return false; }
    buffer[bytes_read] = '\0';
    try { value = std::stoi(buffer); } catch (...) { value = 0; return false; }
    guard.invoke();
    return true;
#endif
}

} // namespace filelock
} // namespace worker
