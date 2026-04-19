// tests/test_pylabhub_utils/jsonconfig_workers.cpp
/**
 * @file jsonconfig_workers.cpp
 * @brief Implements the worker functions for JsonConfig tests (Pattern 3).
 *
 * The original 22-test JsonConfigTest fixture used SetUpTestSuite to install
 * a shared LifecycleGuard so each test could call JsonConfig directly in the
 * gtest runner. That is the V1 antipattern HEP-CORE-0001 § "Testing
 * implications" forbids — a test that finalizes early or panics corrupts
 * singleton state for the next test in the suite. Every body now lives in
 * its own subprocess.
 */
#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "jsonconfig_workers.h"
#include "test_entrypoint.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "gtest/gtest.h"

using nlohmann::json;
using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;

namespace pylabhub::tests::worker
{
namespace jsonconfig
{

int write_id(const std::string &cfgpath, const std::string &worker_id)
{
    return run_gtest_worker(
        [&]()
        {
            // Each worker repeatedly attempts to acquire a write lock and modify the file.
            // This simulates high-contention scenarios for the JsonConfig class.
            JsonConfig cfg(cfgpath);
            const int max_retries = 200;
            bool success = false;

            // Seed random number generator for sleep intervals to vary contention.
            std::srand(static_cast<unsigned int>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                std::chrono::system_clock::now().time_since_epoch().count()));

            for (int attempt = 0; attempt < max_retries; ++attempt)
            {
                std::error_code ec;
                // Attempt a non-blocking write. The lambda is only executed if the
                // file lock is acquired successfully.
                cfg.transaction(JsonConfig::AccessFlags::FullSync)
                    .write(
                        [&](json &data)
                        {
                            int attempts = data.value("total_attempts", 0);
                            data["total_attempts"] = attempts + 1;
                            data[worker_id] = true;
                            data["last_worker_id"] = worker_id;
                        },
                        &ec);

                if (ec.value() == 0)
                {
                    success = true;
                    break;
                }

                // If the write failed (e.g., lock not acquired), sleep for a random
                // duration before retrying to reduce hot-looping.
                std::this_thread::sleep_for(std::chrono::milliseconds(10 + (std::rand() % 40)));
            }

            ASSERT_TRUE(success);
        },
        "jsonconfig::write_id", JsonConfig::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int uninitialized_behavior()
{
    // This worker function is designed to test the fatal error that occurs when
    // a JsonConfig object is constructed before its lifecycle module is initialized.
    // There is no LifecycleGuard here, so the JsonConfig module is not started.
    // The following line is expected to call PLH_PANIC and abort the process.
    JsonConfig config;

    // The lines below should be unreachable. If the process exits with 0,
    // the test will fail.
    return 0;
}

int not_consuming_proxy()
{
    return run_gtest_worker(
        [&]()
        {
            // The test fixture will create a temporary directory, but the file doesn't need to
            // exist for this test. We just need a valid, initialized JsonConfig object.
            auto temp_dir = std::filesystem::temp_directory_path() / "pylabub_jsonconfig_workers";
            std::filesystem::create_directories(temp_dir);
            JsonConfig cfg(temp_dir / "dummy.json", true);

            // Create a transaction proxy and let it go out of scope without being consumed.
            // This should trigger the destructor's warning message in a debug build.
            // NODISCARD: We intentionally do not consume the proxy (no .read()/.write()) to
            // exercise the "proxy not consumed" warning; see docs/NODISCARD_DECISIONS.md.
            (void)cfg.transaction();
        },
        "jsonconfig::not_consuming_proxy", JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

// ── Pattern 3 conversions of the in-process JsonConfigTest ──────────────────

namespace
{
namespace fs = std::filesystem;

/// Build a JsonConfig lifecycle module list once per worker.
auto json_mods()
{
    return MakeModDefList(JsonConfig::GetLifecycleModule(),
                          FileLock::GetLifecycleModule(),
                          Logger::GetLifecycleModule());
}

/// Helper: reads a file's full contents into a string. Returns empty on
/// open failure (matches the in-process helper that the parent test
/// previously used).
std::string read_file_to_string(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open())
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int init_and_create(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "init_create.json";
            ASSERT_FALSE(fs::exists(cfg_path));

            JsonConfig config;
            std::error_code ec;
            ASSERT_TRUE(config.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);
            ASSERT_TRUE(fs::exists(cfg_path));

            config.transaction().read(
                [&](const json &j) {
                    ASSERT_TRUE(j.is_object());
                    ASSERT_TRUE(j.empty());
                },
                &ec);
            ASSERT_FALSE(ec);

            JsonConfig config2(cfg_path, false, &ec);
            ASSERT_FALSE(ec);
            config2.transaction().read(
                [&](const json &j) {
                    ASSERT_TRUE(j.is_object());
                    ASSERT_TRUE(j.empty());
                },
                &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::init_and_create", JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int init_with_empty_path_fails()
{
    return run_gtest_worker(
        [&]()
        {
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_FALSE(cfg.init(fs::path(), false, &ec));
            ASSERT_TRUE(ec);
        },
        "jsonconfig::init_with_empty_path_fails",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int init_with_non_existent_file(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "non_existent.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, false, &ec));
            ASSERT_FALSE(ec);
            cfg.transaction().read(
                [&](const json &j) {
                    ASSERT_TRUE(j.is_object());
                    ASSERT_TRUE(j.empty());
                },
                &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::init_with_non_existent_file",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int basic_accessors(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "accessors.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::UnSynced)
                .write([&](json &j) {
                    j["int_val"] = 42;
                    j["str_val"] = "hello";
                }, &ec);
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::UnSynced)
                .read([&](const json &j) {
                    ASSERT_EQ(j.value("int_val", -1), 42);
                    ASSERT_EQ(j.value("str_val", std::string{}), "hello");
                }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::basic_accessors", JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int reload_on_disk_change(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "reload_on_disk.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["value"] = 1; }, &ec);
            ASSERT_FALSE(ec);

            { std::ofstream out(cfg_path); out << R"({ "value": 2, "new_key": "external" })"; }

            ASSERT_TRUE(cfg.reload(&ec));
            ASSERT_FALSE(ec);
            cfg.transaction().read(
                [&](const json &j) {
                    ASSERT_EQ(j.value("value", -1), 2);
                    ASSERT_EQ(j.value("new_key", std::string{}), "external");
                }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::reload_on_disk_change",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int simplified_api_overloads(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "simplified_api.json";
            JsonConfig cfg;
            ASSERT_TRUE(cfg.init(cfg_path, true));
            std::error_code ec;
            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["key"] = "value1"; }, &ec);
            ASSERT_FALSE(ec);
            std::string read_value;
            cfg.transaction(JsonConfig::AccessFlags::ReloadFirst)
                .read([&](const json &j) { read_value = j.value("key", ""); }, &ec);
            ASSERT_FALSE(ec);
            ASSERT_EQ(read_value, "value1");
        },
        "jsonconfig::simplified_api_overloads",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int recursion_guard(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "recursion.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);

            // 1. Nested read transactions
            cfg.transaction().read(
                [&]([[maybe_unused]] const json &j) {
                    std::error_code inner_ec;
                    cfg.transaction().read(
                        [&](const json &) { FAIL() << "Inner read lambda should not execute."; },
                        &inner_ec);
                    ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
                }, &ec);
            ASSERT_FALSE(ec);

            // 2. Nested write transactions
            cfg.transaction().write(
                [&]([[maybe_unused]] json &j) {
                    std::error_code inner_ec;
                    cfg.transaction().write(
                        [&](json &) { FAIL() << "Inner write lambda should not execute."; },
                        &inner_ec);
                    ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
                }, &ec);
            ASSERT_FALSE(ec);

            // 3. read-in-write
            cfg.transaction().write(
                [&]([[maybe_unused]] json &j) {
                    std::error_code inner_ec;
                    cfg.transaction().read(
                        [&](const json &) { FAIL() << "Inner read lambda should not execute."; },
                        &inner_ec);
                    ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
                }, &ec);
            ASSERT_FALSE(ec);

            // 4. write-in-read
            cfg.transaction().read(
                [&]([[maybe_unused]] const json &j) {
                    std::error_code inner_ec;
                    cfg.transaction().write(
                        [&](json &) { FAIL() << "Inner write lambda should not execute."; },
                        &inner_ec);
                    ASSERT_EQ(inner_ec, std::errc::resource_deadlock_would_occur);
                }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::recursion_guard", JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int write_transaction_rolls_back_on_exception(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "rollback_on_exception.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["value"] = 1; }, &ec);
            ASSERT_FALSE(ec);

            cfg.transaction().write(
                [&](json &j) {
                    j["value"] = 2;
                    throw std::runtime_error("Something went wrong");
                }, &ec);
            ASSERT_TRUE(ec);
            ASSERT_EQ(ec, std::errc::io_error);

            cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("value", -1), 1); }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::write_transaction_rolls_back_on_exception",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int load_malformed_file(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "malformed.json";
            { std::ofstream out(cfg_path); out << "{ \"key\": \"value\""; }
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_FALSE(cfg.init(cfg_path, false, &ec));
            ASSERT_EQ(ec, std::errc::io_error);

            fs::remove(cfg_path);
            { std::ofstream out(cfg_path); out << "{}"; }
            JsonConfig cfg2(cfg_path, false, &ec);
            ASSERT_TRUE(cfg2.is_initialized());
            ASSERT_FALSE(ec);

            { std::ofstream out(cfg_path); out << "this is not json"; }
            ASSERT_FALSE(cfg2.reload(&ec));
            ASSERT_EQ(ec, std::errc::io_error);
        },
        "jsonconfig::load_malformed_file",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int multi_thread_file_contention(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "multithread_contention.json";
            {
                JsonConfig setup_cfg;
                std::error_code ec;
                ASSERT_TRUE(setup_cfg.init(cfg_path, true, &ec));
                ASSERT_FALSE(ec);
                setup_cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                    .write([&](json &data) {
                        data["counter"] = 0;
                        data["write_log"] = json::array();
                    }, &ec);
                ASSERT_FALSE(ec);
            }

            constexpr int kThreads = 16;
            constexpr int kIters = 25;
            std::vector<std::thread> threads;
            std::atomic<int> read_failures{0};
            std::atomic<int> successful_writes{0};

            for (int i = 0; i < kThreads; ++i)
            {
                threads.emplace_back([=, &read_failures, &successful_writes]() {
                    JsonConfig cfg(cfg_path);
                    int last_read_value = -1;
                    for (int j = 0; j < kIters; ++j)
                    {
                        if (std::rand() % 4 == 0)
                        {
                            std::error_code ec;
                            cfg.transaction(JsonConfig::AccessFlags::FullSync)
                                .write([&](json &data) {
                                    int v = data.value("counter", 0);
                                    data["counter"] = v + 1;
                                    data["write_log"].push_back(fmt::format("T{}-w{}", i, j));
                                }, &ec);
                            if (!ec) successful_writes++;
                        }
                        else
                        {
                            std::error_code ec;
                            cfg.transaction(JsonConfig::AccessFlags::ReloadFirst)
                                .read([&](const json &data) {
                                    int cur = data.value("counter", -1);
                                    if (cur < last_read_value) read_failures++;
                                    last_read_value = cur;
                                }, &ec);
                            if (ec) read_failures++;
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500));
                    }
                });
            }
            for (auto &t : threads) t.join();
            ASSERT_EQ(read_failures.load(), 0);

            JsonConfig verifier(cfg_path);
            std::error_code ec;
            verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
                .read([&](const json &data) {
                    int final_counter = data.value("counter", -1);
                    json final_log = data.value("write_log", json::array());
                    EXPECT_EQ(final_counter, static_cast<int>(final_log.size()));
                    EXPECT_GT(final_counter, 0);
                }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::multi_thread_file_contention",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int symlink_attack_prevention_posix(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
#if PYLABHUB_IS_POSIX
            const fs::path real_file = fs::path(dir) / "real_file.txt";
            const fs::path symlink_path = fs::path(dir) / "config_symlink.json";
            { std::ofstream out(real_file); out << R"({ "original": "data" })"; }
            fs::create_symlink(real_file, symlink_path);

            JsonConfig cfg;
            std::error_code init_ec;
            ASSERT_FALSE(cfg.init(symlink_path, false, &init_ec));
            ASSERT_TRUE(init_ec);
            ASSERT_EQ(init_ec, std::errc::operation_not_permitted);

            std::error_code write_ec;
            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["malicious"] = "data"; }, &write_ec);
            ASSERT_EQ(write_ec, std::errc::not_connected);

            json j = json::parse(read_file_to_string(real_file));
            ASSERT_EQ(j["original"], "data");
            ASSERT_EQ(j.find("malicious"), j.end());
#else
            (void)dir;
            GTEST_SKIP() << "POSIX-only test";
#endif
        },
        "jsonconfig::symlink_attack_prevention_posix",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int symlink_attack_prevention_windows(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
#if defined(PLATFORM_WIN64)
            const fs::path real_file = fs::path(dir) / "real_file_win.txt";
            const fs::path symlink_path = fs::path(dir) / "config_win.json";
            { std::ofstream out(real_file); out << R"({ "original": "data" })"; }
            if (!CreateSymbolicLinkW(symlink_path.wstring().c_str(),
                                     real_file.wstring().c_str(), 0))
            {
                GTEST_SKIP() << "could not create symlink (Admin/Developer Mode required)";
            }
            ASSERT_TRUE(fs::is_symlink(symlink_path));
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_FALSE(cfg.init(symlink_path, true, &ec));
            ASSERT_TRUE(ec);
            ASSERT_EQ(ec, std::errc::operation_not_permitted);
#else
            (void)dir;
            GTEST_SKIP() << "Windows-only test";
#endif
        },
        "jsonconfig::symlink_attack_prevention_windows",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int multi_thread_shared_object_contention(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "multithread_shared_object.json";
            JsonConfig shared_cfg(cfg_path, true);
            ASSERT_TRUE(shared_cfg.is_initialized());
            shared_cfg.transaction().write([&](json &data) { data["counter"] = 0; }, nullptr);

            constexpr int kWriterThreads = 4;
            constexpr int kReaderThreads = 8;
            constexpr int kIterPerWriter = 50;
            std::vector<std::thread> threads;
            std::atomic<int> read_failures{0};

            for (int i = 0; i < kWriterThreads; ++i)
            {
                threads.emplace_back([&]() {
                    for (int j = 0; j < kIterPerWriter; ++j)
                    {
                        shared_cfg.transaction().write([&](json &data) {
                            int v = data.value("counter", 0);
                            data["counter"] = v + 1;
                        }, nullptr);
                        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
                    }
                });
            }
            for (int i = 0; i < kReaderThreads; ++i)
            {
                threads.emplace_back([&]() {
                    int last_read_value = -1;
                    auto start_time = std::chrono::steady_clock::now();
                    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(1))
                    {
                        shared_cfg.transaction().read([&](const json &data) {
                            int cur = data.value("counter", -1);
                            if (cur < last_read_value) read_failures++;
                            last_read_value = cur;
                        }, nullptr);
                        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
                    }
                });
            }
            for (auto &t : threads) t.join();
            ASSERT_EQ(read_failures.load(), 0);
            int final_counter = 0;
            shared_cfg.transaction().read(
                [&](const json &data) { final_counter = data.value("counter", -1); }, nullptr);
            EXPECT_EQ(final_counter, kWriterThreads * kIterPerWriter);
        },
        "jsonconfig::multi_thread_shared_object_contention",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int manual_locking_api(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "manual_locking.json";
            JsonConfig cfg;
            std::error_code ec;
            ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
            ASSERT_FALSE(ec);

            auto w_lock_opt = cfg.lock_for_write(&ec);
            ASSERT_TRUE(w_lock_opt.has_value());
            ASSERT_FALSE(ec);
            auto w_lock = std::move(*w_lock_opt);
            w_lock.json()["manual"] = true;
            w_lock.json()["value"] = "test";
            ASSERT_TRUE(w_lock.commit(&ec));
            ASSERT_FALSE(ec);

            JsonConfig verifier_cfg(cfg_path, false, &ec);
            ASSERT_TRUE(verifier_cfg.is_initialized());
            ASSERT_FALSE(ec);
            ASSERT_TRUE(verifier_cfg.reload(&ec));
            ASSERT_FALSE(ec);
            auto r_lock_opt = verifier_cfg.lock_for_read(&ec);
            ASSERT_TRUE(r_lock_opt.has_value());
            ASSERT_FALSE(ec);
            auto r_lock = std::move(*r_lock_opt);
            const auto &j = r_lock.json();
            ASSERT_TRUE(j.value("manual", false));
            ASSERT_EQ(j.value("value", ""), "test");
        },
        "jsonconfig::manual_locking_api",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int move_semantics(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path1 = fs::path(dir) / "move_semantics1.json";
            const fs::path cfg_path2 = fs::path(dir) / "move_semantics2.json";

            std::error_code ec;
            JsonConfig cfg1(cfg_path1, true, &ec);
            ASSERT_FALSE(ec);
            cfg1.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = 1; }, &ec);
            ASSERT_FALSE(ec);

            JsonConfig cfg_moved_to = std::move(cfg1);
            ASSERT_FALSE(cfg1.is_initialized());
            ASSERT_TRUE(cfg_moved_to.is_initialized());
            ASSERT_EQ(cfg_moved_to.config_path(), cfg_path1);
            cfg_moved_to.transaction().read(
                [&](const json &j) { ASSERT_EQ(j.value("val", 0), 1); }, &ec);
            ASSERT_FALSE(ec);

            JsonConfig cfg2(cfg_path2, true, &ec);
            ASSERT_FALSE(ec);
            cfg2.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = 2; }, &ec);
            ASSERT_FALSE(ec);

            cfg_moved_to = std::move(cfg2);
            ASSERT_FALSE(cfg2.is_initialized());
            ASSERT_TRUE(cfg_moved_to.is_initialized());
            ASSERT_EQ(cfg_moved_to.config_path(), cfg_path2);
            cfg_moved_to.transaction().read(
                [&](const json &j) { ASSERT_EQ(j.value("val", 0), 2); }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::move_semantics",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int overwrite_method(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "overwrite.json";
            JsonConfig cfg(cfg_path, true);
            std::error_code ec;
            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = "initial"; }, &ec);
            ASSERT_FALSE(ec);
            cfg.transaction().write([&](json &j) { j["val"] = "in-memory"; }, &ec);
            ASSERT_FALSE(ec);
            ASSERT_TRUE(cfg.is_dirty());
            { std::ofstream out(cfg_path); out << R"({ "val": "external" })"; }
            ASSERT_TRUE(cfg.overwrite(&ec));
            ASSERT_FALSE(ec);
            ASSERT_FALSE(cfg.is_dirty());
            JsonConfig verifier(cfg_path);
            verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
                .read([&](const json &j) { ASSERT_EQ(j.value("val", ""), "in-memory"); }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::overwrite_method",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int dirty_flag_logic(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "dirty_flag.json";
            std::error_code ec;
            JsonConfig cfg(cfg_path, true, &ec);
            ASSERT_FALSE(ec);
            ASSERT_FALSE(cfg.is_dirty());

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["a"] = 1; }, &ec);
            ASSERT_FALSE(ec);
            ASSERT_FALSE(cfg.is_dirty());

            cfg.transaction().write([&](json &j) { j["b"] = 2; }, &ec);
            ASSERT_FALSE(ec);
            ASSERT_TRUE(cfg.is_dirty());

            cfg.reload(&ec);
            ASSERT_FALSE(ec);
            ASSERT_FALSE(cfg.is_dirty());
            cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.count("b"), 0); });

            {
                auto wlock = cfg.lock_for_write(&ec);
                ASSERT_TRUE(wlock.has_value());
                wlock->json()["c"] = 3;
            }
            ASSERT_TRUE(cfg.is_dirty());

            {
                auto wlock = cfg.lock_for_write(&ec);
                ASSERT_TRUE(wlock.has_value());
                wlock->json()["c"] = 4;
                ASSERT_TRUE(wlock->commit(&ec));
                ASSERT_FALSE(ec);
            }
            ASSERT_FALSE(cfg.is_dirty());

            cfg.transaction().write([&](json &j) { j["e"] = 5; }, &ec);
            ASSERT_TRUE(cfg.is_dirty());
            cfg.overwrite(&ec);
            ASSERT_FALSE(ec);
            ASSERT_FALSE(cfg.is_dirty());
        },
        "jsonconfig::dirty_flag_logic",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int write_veto_commit(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "veto_commit.json";
            std::error_code ec;
            JsonConfig cfg(cfg_path, true, &ec);
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = 1; }, &ec);
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) -> bool {
                    j["val"] = 2;
                    return false;
                }, &ec);
            ASSERT_FALSE(ec);
            ASSERT_TRUE(cfg.is_dirty());

            cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 2); }, &ec);
            ASSERT_FALSE(ec);

            JsonConfig verifier(cfg_path);
            verifier.transaction(JsonConfig::AccessFlags::ReloadFirst)
                .read([&](const json &j) { ASSERT_EQ(j.value("val", 0), 1); }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::write_veto_commit",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int write_produces_invalid_json(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path cfg_path = fs::path(dir) / "invalid_json_write.json";
            JsonConfig cfg(cfg_path, true);
            std::error_code ec;
            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = "good"; }, &ec);
            ASSERT_FALSE(ec);

            cfg.transaction(JsonConfig::AccessFlags::CommitAfter)
                .write([&](json &j) { j["val"] = "bad\xDE\xAD\xBE\xEF"; }, &ec);
            ASSERT_TRUE(ec);
            ASSERT_EQ(ec, std::errc::invalid_argument);

            cfg.transaction().read([&](const json &j) { ASSERT_EQ(j.value("val", ""), "good"); }, &ec);
            ASSERT_FALSE(ec);

            JsonConfig verifier(cfg_path);
            verifier.reload(&ec);
            ASSERT_FALSE(ec);
            verifier.transaction().read(
                [&](const json &j) { ASSERT_EQ(j.value("val", ""), "good"); }, &ec);
            ASSERT_FALSE(ec);
        },
        "jsonconfig::write_produces_invalid_json",
        JsonConfig::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

} // namespace jsonconfig
} // namespace pylabhub::tests::worker

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct JsonConfigWorkerRegistrar
{
    JsonConfigWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "jsonconfig")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::jsonconfig;
                if (scenario == "write_id" && argc > 3)
                    return write_id(argv[2], argv[3]);
                if (scenario == "uninitialized_behavior")
                    return uninitialized_behavior();
                if (scenario == "not_consuming_proxy")
                    return not_consuming_proxy();

                // Pattern 3 conversions of in-process JsonConfigTest bodies.
                // All take a parent-provided unique temp dir as argv[2].
                auto need_dir = [&]() -> bool {
                    if (argc <= 2) {
                        fmt::print(stderr,
                                   "jsonconfig.{}: missing required <dir> arg\n",
                                   scenario);
                        return false;
                    }
                    return true;
                };
                if (scenario == "init_and_create" && need_dir())
                    return init_and_create(argv[2]);
                if (scenario == "init_with_empty_path_fails")
                    return init_with_empty_path_fails();
                if (scenario == "init_with_non_existent_file" && need_dir())
                    return init_with_non_existent_file(argv[2]);
                if (scenario == "basic_accessors" && need_dir())
                    return basic_accessors(argv[2]);
                if (scenario == "reload_on_disk_change" && need_dir())
                    return reload_on_disk_change(argv[2]);
                if (scenario == "simplified_api_overloads" && need_dir())
                    return simplified_api_overloads(argv[2]);
                if (scenario == "recursion_guard" && need_dir())
                    return recursion_guard(argv[2]);
                if (scenario == "write_transaction_rolls_back_on_exception" && need_dir())
                    return write_transaction_rolls_back_on_exception(argv[2]);
                if (scenario == "load_malformed_file" && need_dir())
                    return load_malformed_file(argv[2]);
                if (scenario == "multi_thread_file_contention" && need_dir())
                    return multi_thread_file_contention(argv[2]);
                if (scenario == "symlink_attack_prevention_posix" && need_dir())
                    return symlink_attack_prevention_posix(argv[2]);
                if (scenario == "symlink_attack_prevention_windows" && need_dir())
                    return symlink_attack_prevention_windows(argv[2]);
                if (scenario == "multi_thread_shared_object_contention" && need_dir())
                    return multi_thread_shared_object_contention(argv[2]);
                if (scenario == "manual_locking_api" && need_dir())
                    return manual_locking_api(argv[2]);
                if (scenario == "move_semantics" && need_dir())
                    return move_semantics(argv[2]);
                if (scenario == "overwrite_method" && need_dir())
                    return overwrite_method(argv[2]);
                if (scenario == "dirty_flag_logic" && need_dir())
                    return dirty_flag_logic(argv[2]);
                if (scenario == "write_veto_commit" && need_dir())
                    return write_veto_commit(argv[2]);
                if (scenario == "write_produces_invalid_json" && need_dir())
                    return write_produces_invalid_json(argv[2]);

                fmt::print(stderr, "ERROR: Unknown jsonconfig scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static JsonConfigWorkerRegistrar g_jsonconfig_registrar;
} // namespace
