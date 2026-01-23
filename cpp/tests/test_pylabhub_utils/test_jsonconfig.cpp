// tests/test_pylabhub_utils/test_jsonconfig.cpp
/**
 * @file test_jsonconfig.cpp
 * @brief Unit and integration tests for the `pylabhub::utils::JsonConfig` class.
 *
 * This file contains a comprehensive suite of tests for the JsonConfig class,
 * covering its public API, thread and process safety, error handling, and
 * security features. The tests are organized into a `JsonConfigTest` fixture
 * that manages a temporary directory for test artifacts.
 */
#include <fstream>

#include "plh_datahub.hpp"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"

using namespace nlohmann;
using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;
namespace fs = std::filesystem;

namespace
{

// Path for temporary files created during tests.
static fs::path g_temp_dir;

// Helper to read file contents for verification.
static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open())
        return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @class JsonConfigTest
 * @brief Test fixture for `JsonConfig` tests.
 *
 * This fixture sets up a temporary directory (`g_temp_dir`) before the test
 * suite runs and cleans it up afterward. It provides a clean environment for
 * tests that interact with the filesystem. The application lifecycle, including
 * the `JsonConfig` module, is managed by a global `LifecycleGuard` in the main
 * test entry point, ensuring that all necessary modules are initialized.
 */
class JsonConfigTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
        fs::create_directories(g_temp_dir);
    }

    static void TearDownTestSuite()
    {
        try
        {
            fs::remove_all(g_temp_dir);
        }
        catch (...)
        {
        }
    }
};

} // anonymous namespace

/**
 * @brief Tests basic initialization and file creation.
 *
 * Verifies that a `JsonConfig` object can be initialized via `init()` and that
 * it correctly creates the configuration file on disk if `createIfMissing` is true.
 * It also tests re-initialization by passing a path to the constructor.
 */
TEST_F(JsonConfigTest, InitAndCreate)
{
    auto cfg_path = g_temp_dir / "init_create.json";
    fs::remove(cfg_path);

    JsonConfig config;
    ASSERT_FALSE(fs::exists(cfg_path));

    std::error_code ec;
    ASSERT_TRUE(config.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);
    ASSERT_TRUE(fs::exists(cfg_path));

    config.transaction().read(
        [&](const json &j)
        {
            ASSERT_TRUE(j.is_object());
            ASSERT_TRUE(j.empty());
        },
        &ec);
    ASSERT_FALSE(ec);

    JsonConfig config2(cfg_path, false, &ec);
    ASSERT_FALSE(ec);
    config2.transaction().read(
        [&](const json &j)
        {
            ASSERT_TRUE(j.is_object());
            ASSERT_TRUE(j.empty());
        },
        &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests initialization with a non-existent file when `createIfMissing` is false.
 * Verifies that `init()` succeeds and the in-memory representation is an empty
 * object, even if the file does not exist on disk.
 */
TEST_F(JsonConfigTest, InitWithNonExistentFile)
{
    auto cfg_path = g_temp_dir / "non_existent.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, false, &ec));
    ASSERT_FALSE(ec);

    cfg.transaction().read(
        [&](const json &j)
        {
            ASSERT_TRUE(j.is_object());
            ASSERT_TRUE(j.empty());
        },
        &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that constructing a `JsonConfig` object without initializing its
 *        lifecycle module results in a fatal error.
 *
 * This test runs the "uninitialized_behavior" worker in a separate process.
 * That worker attempts to construct a `JsonConfig` object without having a
 * `LifecycleGuard` in place. The expected behavior is for the constructor to
 * call `PLH_PANIC`, which aborts the child process.
 *
 * This test verifies that this protection mechanism is working correctly by
 * checking that the worker process terminates with a non-zero exit code and
 * that the stderr stream contains the expected panic message.
 */
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    WorkerProcess worker(g_self_exe_path, "jsonconfig.uninitialized_behavior", {});
    ASSERT_TRUE(worker.valid());
    const int exit_code = worker.wait_for_exit();

    // The worker process is expected to be terminated by a panic (abort),
    // which results in a non-zero exit code. On POSIX, this is often reported
    // as -1 by our helper, or a value > 128. On Windows, it's an arbitrary code.
    // The key is that it's not 0.
    ASSERT_NE(exit_code, 0);

    // Verify that the stderr output contains the expected panic message.
    const std::string &stderr_output = worker.get_stderr();
    EXPECT_THAT(stderr_output,
                ::testing::HasSubstr("JsonConfig created before its module was initialized"));
    EXPECT_THAT(stderr_output, ::testing::HasSubstr("Aborting"));
}

/**
 * @brief Tests basic in-memory read and write operations using the transaction API.
 * Verifies that data written in one transaction can be read back in a subsequent one.
 */
TEST_F(JsonConfigTest, BasicAccessors)
{
    auto cfg_path = g_temp_dir / "accessors.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    cfg.transaction(JsonConfig::AccessFlags::UnSynced)
        .write(
            [&](json &j)
            {
                j["int_val"] = 42;
                j["str_val"] = "hello";
            },
            &ec);
    ASSERT_FALSE(ec);

    cfg.transaction(JsonConfig::AccessFlags::UnSynced)
        .read(
            [&](const json &j)
            {
                ASSERT_EQ(j.value("int_val", -1), 42);
                ASSERT_EQ(j.value("str_val", std::string{}), "hello");
            },
            &ec); // Read from memory, don't reload
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that `reload()` correctly picks up external file modifications.
 * Verifies that changes made directly to the file on disk are loaded into
 * memory after an explicit call to `reload()`.
 */
TEST_F(JsonConfigTest, ReloadOnDiskChange)
{
    auto cfg_path = g_temp_dir / "reload_on_disk.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["value"] = 1; }, &ec);
    ASSERT_FALSE(ec);

    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // Explicitly reload to pick up the changes from disk.
    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    cfg.transaction().read(
        [&](const json &j)
        {
            ASSERT_EQ(j.value("value", -1), 2);
            ASSERT_EQ(j.value("new_key", std::string{}), "external");
        },
        &ec); // Already reloaded, so read from memory
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests the simplified transaction API flags `ReloadFirst` and `CommitAfter`.
 * Verifies that `CommitAfter` writes to disk and `ReloadFirst` reads the changes back.
 */
TEST_F(JsonConfigTest, SimplifiedApiOverloads)
{
    auto cfg_path = g_temp_dir / "simplified_api.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    ASSERT_TRUE(cfg.init(cfg_path, true));

    // Write and commit to disk
    std::error_code ec;
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["key"] = "value1"; }, &ec);
    ASSERT_FALSE(ec);

    std::string read_value;
    // Reload from disk and read
    cfg.transaction(JsonConfig::AccessFlags::ReloadFirst)
        .read([&](const json &j) { read_value = j.value("key", ""); }, &ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(read_value, "value1");
}

/**
 * @brief Tests the recursion guard mechanism.
 * Verifies that attempting to start a new transaction from within an existing
 * transaction (read-in-read, write-in-write, read-in-write, etc.) fails with
 * `resource_deadlock_would_occur`, preventing deadlocks.
 */
TEST_F(JsonConfigTest, RecursionGuard)
{
    auto cfg_path = g_temp_dir / "recursion.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // 1. Test nested read transactions
    cfg.transaction().read(
        [&]([[maybe_unused]] const json &j)
        {
            std::error_code inner_ec;
            cfg.transaction().read([&](const json &)
                                   { FAIL() << "Inner read lambda should not execute."; },
                                   &inner_ec);
            ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
        },
        &ec);
    ASSERT_FALSE(ec);

    // 2. Test nested write transactions
    cfg.transaction().write(
        [&]([[maybe_unused]] json &j)
        {
            std::error_code inner_ec;
            cfg.transaction().write(
                [&](json &) { FAIL() << "Inner write lambda should not execute."; }, &inner_ec);
            ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
        },
        &ec);
    ASSERT_FALSE(ec);

    // 3. Test read-in-write
    cfg.transaction().write(
        [&]([[maybe_unused]] json &j)
        {
            std::error_code inner_ec;
            cfg.transaction().read([&](const json &)
                                   { FAIL() << "Inner read lambda should not execute."; },
                                   &inner_ec);
            ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
        },
        &ec);
    ASSERT_FALSE(ec);

    // 4. Test write-in-read
    cfg.transaction().read(
        [&]([[maybe_unused]] const json &j)
        {
            std::error_code inner_ec;
            cfg.transaction().write(
                [&](json &) { FAIL() << "Inner write lambda should not execute."; }, &inner_ec);
            ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
        },
        &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that a write transaction correctly rolls back changes on exception.
 * Verifies that if a lambda passed to `write()` throws an exception, the
 * in-memory state of the JSON object is reverted to its state before the
 * transaction began.
 */
TEST_F(JsonConfigTest, WriteTransactionRollsBackOnException)
{
    auto cfg_path = g_temp_dir / "rollback_on_exception.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // 1. Set initial state and commit
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["value"] = 1; }, &ec);
    ASSERT_FALSE(ec);

    // 2. Start a write transaction that throws an exception
    cfg.transaction().write(
        [&](json &j)
        {
            j["value"] = 2;
            throw std::runtime_error("Something went wrong");
        },
        &ec);

    // 3. Verify that the transaction reported an error
    ASSERT_TRUE(ec);
    ASSERT_EQ(ec, std::errc::io_error);

    // 4. Verify that the in-memory state was rolled back
    cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("value", -1), 1); }, &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests error handling when loading a malformed JSON file.
 * Verifies that both `init()` and `reload()` fail with an `io_error` when
 * the on-disk file contains invalid JSON, and that the in-memory state is
 * not corrupted.
 */
TEST_F(JsonConfigTest, LoadMalformedFile)
{
    auto cfg_path = g_temp_dir / "malformed.json";
    fs::remove(cfg_path);

    // Create a malformed JSON file
    {
        std::ofstream out(cfg_path);
        out << "{ \"key\": \"value\""; // Missing closing brace
    }

    JsonConfig cfg;
    std::error_code ec;

    // init should fail because reload fails on a malformed file.
    // The implementation catches the parse error and returns `io_error`.
    ASSERT_FALSE(cfg.init(cfg_path, false, &ec));
    ASSERT_EQ(ec, std::errc::io_error);

    // Test reload() directly as well.
    // First, init with a valid file.
    fs::remove(cfg_path);
    {
        std::ofstream out(cfg_path);
        out << "{}";
    }
    JsonConfig cfg2(cfg_path, false, &ec);
    ASSERT_TRUE(cfg2.is_initialized());
    ASSERT_FALSE(ec);

    // Now, corrupt the file on disk.
    {
        std::ofstream out(cfg_path);
        out << "this is not json";
    }

    // reload() should now fail with an io_error.
    ASSERT_FALSE(cfg2.reload(&ec));
    ASSERT_EQ(ec, std::errc::io_error);
}

/**
 * @brief Stress-tests file contention from multiple threads using separate objects.
 *
 * Each thread creates its own `JsonConfig` instance pointing to the same file.
 * This test primarily validates the process-level `FileLock` mechanism by
 * simulating a multi-process scenario within a single process. It ensures that
 * atomic read-modify-write transactions (`FullSync`) are safe under contention.
 */
TEST_F(JsonConfigTest, MultiThreadFileContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    fs::remove(cfg_path);

    // Pre-populate with initial data and write it to disk.
    {
        JsonConfig setup_cfg;
        std::error_code ec;
        ASSERT_TRUE(setup_cfg.init(cfg_path, true, &ec));
        ASSERT_FALSE(ec);
        setup_cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
            .write(
                [&](json &data)
                {
                    data["counter"] = 0;
                    data["write_log"] = json::array();
                },
                &ec);
        ASSERT_FALSE(ec);
    }

    const int THREADS = 16;
    const int ITERS = 25;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;
    std::atomic<int> successful_writes = 0;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [=, &read_failures, &successful_writes]()
            {
                JsonConfig cfg(cfg_path); // Each thread gets its own object instance.
                int last_read_value = -1;

                for (int j = 0; j < ITERS; ++j)
                {
                    if (std::rand() % 4 == 0) // ~25% chance of being a writer
                    {
                        std::error_code ec;
                        // Atomically reload, modify, and commit.
                        cfg.transaction(JsonConfig::AccessFlags::FullSync)
                            .write(
                                [&](json &data)
                                {
                                    int v = data.value("counter", 0);
                                    data["counter"] = v + 1;
                                    data["write_log"].push_back(fmt::format("T{}-w{}", i, j));
                                },
                                &ec);

                        if (!ec)
                        {
                            successful_writes++;
                        }
                    }
                    else // ~75% chance of being a reader
                    {
                        std::error_code ec;
                        // Reload from disk to get the latest version.
                        cfg.transaction(JsonConfig::AccessFlags::ReloadFirst)
                            .read(
                                [&](const json &data)
                                {
                                    int cur = data.value("counter", -1);
                                    if (cur < last_read_value)
                                    {
                                        read_failures++;
                                    }
                                    last_read_value = cur;
                                },
                                &ec);
                        if (ec)
                            read_failures++;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
                }
            });
    }

    for (auto &t : threads)
        t.join();

    ASSERT_EQ(read_failures.load(), 0);

    // Verify the final state of the file.
    JsonConfig verifier(cfg_path);
    std::error_code ec;
    verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
        .read(
            [&](const json &data)
            {
                int final_counter = data.value("counter", -1);
                json final_log = data.value("write_log", json::array());
                EXPECT_EQ(final_counter, static_cast<int>(final_log.size()));
                EXPECT_GT(final_counter, 0);
            },
            &ec);
    ASSERT_FALSE(ec);
}

#if defined(PLATFORM_WIN64)
constexpr std::string prefix_info_fmt = "win-{}";
#else
constexpr std::string prefix_info_fmt = "posix-{}";
#endif
/**
 * @brief Stress-tests write contention between multiple processes.
 *
 * Spawns multiple child processes, each running the `jsonconfig.write_id` worker.
 * This worker attempts to acquire a file lock and write a unique ID to the shared
 * config file. The test verifies that the process-level `FileLock` correctly
 * serializes access and that all processes succeed in writing their data.
 */
TEST_F(JsonConfigTest, MultiProcessContention)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    // Create the initial empty file.
    JsonConfig creator;
    std::error_code ec;
    ASSERT_TRUE(creator.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);
    const int PROCS = 8;

    // Spawn multiple worker processes that all try to write to the same file.
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "jsonconfig.write_id",
            std::vector<std::string>{
                cfg_path.string(),
                fmt::to_string(pylabhub::format_tools::make_buffer(prefix_info_fmt, i))}));
        ASSERT_TRUE(procs.back()->valid());
    }

    int success_count = 0;
    for (auto &p : procs)
    {
        p->wait_for_exit();
        if (p->exit_code() == 0)
        {
            success_count++;
            expect_worker_ok(*p);
        }
    }

    // All workers should have succeeded.
    ASSERT_EQ(success_count, PROCS);

    // Verify that the file contains entries from all workers.
    JsonConfig verifier(cfg_path);
    verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
        .read(
            [&](const json &data)
            {
                for (int i = 0; i < PROCS; ++i)
                {
                    std::string key =
                        fmt::to_string(pylabhub::format_tools::make_buffer(prefix_info_fmt, i));
                    ASSERT_TRUE(data.contains(key)) << "Worker " << key << " failed to write.";
                }
            },
            &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that `JsonConfig` refuses to operate on a path that is a symbolic link.
 * This is a security measure to prevent a malicious actor from replacing the config
 * file with a symlink to overwrite a sensitive system file (e.g., `/etc/passwd`).
 */
#if PYLABHUB_IS_POSIX
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config_symlink.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    // Create a dummy "sensitive" file.
    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Create a symlink pointing from the config path to the sensitive file.
    fs::create_symlink(real_file, symlink_path);

    JsonConfig cfg;
    std::error_code init_ec;
    // `init` should fail immediately upon detecting the symlink.
    ASSERT_FALSE(cfg.init(symlink_path, false, &init_ec));
    ASSERT_TRUE(init_ec);
    ASSERT_EQ(init_ec, std::errc::operation_not_permitted);

    // Attempting to write should also fail, as the object is not initialized.
    std::error_code write_ec;
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["malicious"] = "data"; }, &write_ec);
    ASSERT_EQ(write_ec, std::errc::not_connected);

    // Confirm the original file was not modified.
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

#if defined(PLATFORM_WIN64)
/**
 * @brief Tests symlink attack prevention on Windows.
 * @note Creating symlinks on Windows may require administrative privileges or
 *       Developer Mode to be enabled, so the test may be skipped.
 */
TEST_F(JsonConfigTest, SymlinkAttackPreventionWindows)
{
    auto real_file = g_temp_dir / "real_file_win.txt";
    auto symlink_path = g_temp_dir / "config_win.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    // Creating symlinks on Windows may require administrative privileges.
    if (!CreateSymbolicLinkW(symlink_path.wstring().c_str(), real_file.wstring().c_str(), 0))
    {
        GTEST_SKIP() << "Skipping Windows symlink test: could not create symlink. Try running as "
                        "Admin or enabling Developer Mode.";
    }

    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    std::error_code ec;
    // On Windows, the check is deferred to write time. `init` itself may succeed.
    ASSERT_FALSE(cfg.init(symlink_path, true, &ec));
    ASSERT_TRUE(ec);
    ASSERT_EQ(ec, std::errc::operation_not_permitted);
}
#endif

/**
 * @brief Verifies the in-memory thread-safety of a SINGLE shared `JsonConfig` object.
 *
 * This test creates one `JsonConfig` object and shares it by reference with
 * multiple threads that perform concurrent in-memory reads and writes. It is
 * designed to fail with a data race if the internal `std::shared_mutex` is not
 * working correctly to serialize writers and allow concurrent readers.
 *
 * All operations are in-memory only (`UnSynced`) to isolate thread-safety from file I/O.
 */
TEST_F(JsonConfigTest, MultiThreadSharedObjectContention)
{
    auto cfg_path = g_temp_dir / "multithread_shared_object.json";
    fs::remove(cfg_path);

    // 1. Create a SINGLE JsonConfig object to be shared by all threads.
    JsonConfig shared_cfg(cfg_path, true);
    ASSERT_TRUE(shared_cfg.is_initialized());

    // 2. Pre-populate with initial data (in-memory only).
    shared_cfg.transaction().write([&](json &data) { data["counter"] = 0; }, nullptr);

    const int WRITER_THREADS = 4;
    const int READER_THREADS = 8;
    const int ITERS_PER_WRITER = 50;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;

    // 3. Spawn writer threads that increment a counter (in-memory only).
    for (int i = 0; i < WRITER_THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                for (int j = 0; j < ITERS_PER_WRITER; ++j)
                {
                    shared_cfg.transaction().write(
                        [&](json &data)
                        {
                            int v = data.value("counter", 0);
                            data["counter"] = v + 1;
                        },
                        nullptr); // In-memory only
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
                }
            });
    }

    // 4. Spawn reader threads that verify the counter is always increasing (monotonic).
    for (int i = 0; i < READER_THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
                int last_read_value = -1;
                auto start_time = std::chrono::steady_clock::now();
                // Run readers for a fixed duration while writers are active.
                while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(1))
                {
                    shared_cfg.transaction().read(
                        [&](const json &data)
                        {
                            int cur = data.value("counter", -1);
                            if (cur < last_read_value)
                            {
                                read_failures++;
                            }
                            last_read_value = cur;
                        },
                        nullptr); // In-memory only
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    // 5. Final verification.
    ASSERT_EQ(read_failures.load(), 0)
        << "Reader threads detected non-monotonic counter changes, indicating a race condition.";

    int final_counter = 0;
    shared_cfg.transaction().read([&](const json &data)
                                  { final_counter = data.value("counter", -1); }, nullptr);

    EXPECT_EQ(final_counter, WRITER_THREADS * ITERS_PER_WRITER);
}

/**
 * @brief Tests the manual locking API (`lock_for_read`/`lock_for_write`).
 * Verifies that users can manually acquire and release locks for fine-grained
 * control over read and write operations, including an explicit commit via the
 * `WriteLock` object.
 */
TEST_F(JsonConfigTest, ManualLockingApi)
{
    auto cfg_path = g_temp_dir / "manual_locking.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // 1. Get a write lock
    auto w_lock_opt = cfg.lock_for_write(&ec);
    ASSERT_TRUE(w_lock_opt.has_value());
    ASSERT_FALSE(ec);
    auto w_lock = std::move(*w_lock_opt);

    // 2. Modify data through the lock
    w_lock.json()["manual"] = true;
    w_lock.json()["value"] = "test";

    // 3. Commit changes to disk using the lock's method
    ASSERT_TRUE(w_lock.commit(&ec));
    ASSERT_FALSE(ec);

    // w_lock is now invalid as commit() consumes the lock.

    // 4. Verify with a separate config object and a read lock
    JsonConfig verifier_cfg(cfg_path, false, &ec);
    ASSERT_TRUE(verifier_cfg.is_initialized());
    ASSERT_FALSE(ec);

    // Manually reload to ensure we get the committed data
    ASSERT_TRUE(verifier_cfg.reload(&ec));
    ASSERT_FALSE(ec);

    // 5. Get a read lock
    auto r_lock_opt = verifier_cfg.lock_for_read(&ec);
    ASSERT_TRUE(r_lock_opt.has_value());
    ASSERT_FALSE(ec);
    auto r_lock = std::move(*r_lock_opt);

    // 6. Verify the data
    const auto &j = r_lock.json();
    ASSERT_TRUE(j.value("manual", false));
    ASSERT_EQ(j.value("value", ""), "test");
}

/**
 * @brief Tests the move constructor and move assignment operator.
 * Verifies that a JsonConfig object can be moved, that the moved-to object
 * is valid and functional, and that the moved-from object is left in a safe,
 * uninitialized state.
 */
TEST_F(JsonConfigTest, MoveSemantics)
{
    auto cfg_path1 = g_temp_dir / "move_semantics1.json";
    auto cfg_path2 = g_temp_dir / "move_semantics2.json";
    fs::remove(cfg_path1);
    fs::remove(cfg_path2);

    // 1. Test move constructor
    std::error_code ec;
    JsonConfig cfg1(cfg_path1, true, &ec);
    ASSERT_FALSE(ec);
    cfg1.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = 1; }, &ec);
    ASSERT_FALSE(ec);

    JsonConfig cfg_moved_to = std::move(cfg1);
    ASSERT_FALSE(cfg1.is_initialized()); // Original is invalid
    ASSERT_TRUE(cfg_moved_to.is_initialized());
    ASSERT_EQ(cfg_moved_to.config_path(), cfg_path1);

    cfg_moved_to.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 1); }, &ec);
    ASSERT_FALSE(ec);

    // 2. Test move assignment
    JsonConfig cfg2(cfg_path2, true, &ec);
    ASSERT_FALSE(ec);
    cfg2.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = 2; }, &ec);
    ASSERT_FALSE(ec);

    cfg_moved_to = std::move(cfg2);
    ASSERT_FALSE(cfg2.is_initialized());
    ASSERT_TRUE(cfg_moved_to.is_initialized());
    ASSERT_EQ(cfg_moved_to.config_path(), cfg_path2);
    cfg_moved_to.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 2); }, &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests the `overwrite()` method.
 * Verifies that `overwrite()` correctly writes the current in-memory state to
 * disk, overwriting any external modifications, and resets the dirty flag.
 */
TEST_F(JsonConfigTest, OverwriteMethod)
{
    auto cfg_path = g_temp_dir / "overwrite.json";
    fs::remove(cfg_path);

    JsonConfig cfg(cfg_path, true);
    std::error_code ec;

    // 1. Write initial state to disk.
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = "initial"; }, &ec);
    ASSERT_FALSE(ec);

    // 2. Modify in-memory only.
    cfg.transaction().write([&](json &j) { j["val"] = "in-memory"; }, &ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(cfg.is_dirty());

    // 3. Modify file on disk externally.
    {
        std::ofstream out(cfg_path);
        out << R"({ "val": "external" })";
    }

    // 4. Call overwrite() to force in-memory state to disk.
    ASSERT_TRUE(cfg.overwrite(&ec));
    ASSERT_FALSE(ec);
    ASSERT_FALSE(cfg.is_dirty());

    // 5. Verify file on disk.
    JsonConfig verifier(cfg_path);
    verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
        .read([&](const json &j) { ASSERT_EQ(j.value("val", ""), "in-memory"); }, &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Verifies the behavior of the `is_dirty()` flag.
 * Checks that the dirty flag is correctly set and cleared by various
 * operations like writing, committing, reloading, and overwriting.
 */
TEST_F(JsonConfigTest, DirtyFlagLogic)
{
    auto cfg_path = g_temp_dir / "dirty_flag.json";
    fs::remove(cfg_path);
    std::error_code ec;

    // 1. Initial state
    JsonConfig cfg(cfg_path, true, &ec);
    ASSERT_FALSE(ec);
    ASSERT_FALSE(cfg.is_dirty());

    // 2. Write with commit
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter).write([&](json &j) { j["a"] = 1; }, &ec);
    ASSERT_FALSE(ec);
    ASSERT_FALSE(cfg.is_dirty());

    // 3. Write without commit (should become dirty)
    cfg.transaction().write([&](json &j) { j["b"] = 2; }, &ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(cfg.is_dirty());

    // 4. Reload (should become clean and discard changes)
    cfg.reload(&ec);
    ASSERT_FALSE(ec);
    ASSERT_FALSE(cfg.is_dirty());
    cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.count("b"), 0); });

    // 5. Manual write lock access (should become dirty)
    {
        auto wlock = cfg.lock_for_write(&ec);
        ASSERT_TRUE(wlock.has_value());
        wlock->json()["c"] = 3;
    } // lock is released
    ASSERT_TRUE(cfg.is_dirty());

    // 6. Manual commit (should become clean)
    {
        auto wlock = cfg.lock_for_write(&ec);
        ASSERT_TRUE(wlock.has_value());
        wlock->json()["c"] = 4; // Modify data before commit
        ASSERT_TRUE(wlock->commit(&ec));
        ASSERT_FALSE(ec);
    } // lock is released
    ASSERT_FALSE(cfg.is_dirty());

    // 7. Overwrite (should become clean)
    cfg.transaction().write([&](json &j) { j["e"] = 5; }, &ec);
    ASSERT_TRUE(cfg.is_dirty());
    cfg.overwrite(&ec);
    ASSERT_FALSE(ec);
    ASSERT_FALSE(cfg.is_dirty());
}

/**
 * @brief Tests the ability to veto a commit from within a write transaction.
 * Verifies that returning `CommitDecision::SkipCommit` from a write lambda
 * prevents the changes from being written to disk, even when `CommitAfter`
 * is specified.
 */
TEST_F(JsonConfigTest, WriteVetoCommit)
{
    auto cfg_path = g_temp_dir / "veto_commit.json";
    fs::remove(cfg_path);
    std::error_code ec;

    JsonConfig cfg(cfg_path, true, &ec);
    ASSERT_FALSE(ec);

    // 1. Initial commit
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = 1; }, &ec);
    ASSERT_FALSE(ec);

    // 2. Transaction that vetoes its own commit
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write(
            [&](json &j) -> JsonConfig::CommitDecision
            {
                j["val"] = 2; // Change in-memory
                return JsonConfig::CommitDecision::SkipCommit;
            },
            &ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(cfg.is_dirty()); // Should be dirty because commit was skipped

    // 3. Verify in-memory value is updated
    cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 2); }, &ec);
    ASSERT_FALSE(ec);

    // 4. Verify file on disk was NOT updated
    JsonConfig verifier(cfg_path);
    verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
        .read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 1); }, &ec);
    ASSERT_FALSE(ec);
}

/**
 * @brief Tests that a transaction is rolled back if the user lambda produces invalid JSON.
 * Verifies that if a write lambda creates a state that cannot be serialized,
 * the transaction fails and the in-memory and on-disk state are rolled back.
 */
TEST_F(JsonConfigTest, WriteProducesInvalidJson)
{
    auto cfg_path = g_temp_dir / "invalid_json_write.json";
    fs::remove(cfg_path);

    JsonConfig cfg(cfg_path, true);
    std::error_code ec;
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = "good"; }, &ec);
    ASSERT_FALSE(ec);

    // Attempt to write a string with an invalid UTF-8 sequence.
    // nlohmann::json's dump() will throw a type_error for this, which our wrapper catches.
    cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
        .write([&](json &j) { j["val"] = "bad\xDE\xAD\xBE\xEF"; }, &ec);

    // The transaction should fail and report an error.
    ASSERT_TRUE(ec);
    ASSERT_EQ(ec, std::errc::invalid_argument);

    // Verify that the config (both in-memory and on-disk) was rolled back.
    cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", ""), "good"); }, &ec);
    ASSERT_FALSE(ec);

    JsonConfig verifier(cfg_path);
    verifier.reload(&ec);
    ASSERT_FALSE(ec);
    verifier.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", ""), "good"); }, &ec);
    ASSERT_FALSE(ec);
}

// This test is only active in debug builds where the check is enabled.
#ifndef NDEBUG
/**
 * @brief Tests the warning for an unconsumed transaction proxy (Debug only).
 * Runs a worker process that creates a `TransactionProxy` and lets it be
 * destroyed without calling `.read()` or `.write()`. Verifies that the
 * expected warning message is printed to stderr.
 */
TEST_F(JsonConfigTest, TransactionProxyNotConsumedWarning)
{
    WorkerProcess worker(g_self_exe_path, "jsonconfig.not_consuming_proxy", {});
    ASSERT_TRUE(worker.valid());
    const int exit_code = worker.wait_for_exit();

    // The worker should exit cleanly, but print a warning to stderr.
    ASSERT_EQ(exit_code, 0);
    // expect_worker_ok checks for an empty stderr, which we don't want here.
    // We just check the exit code is 0.

    const std::string &stderr_output = worker.get_stderr();
    EXPECT_THAT(stderr_output,
                ::testing::HasSubstr("JsonConfig::transaction() proxy was not consumed"));
}
#endif
