// tests/test_jsonconfig.cpp
//
// Unit tests for pylabhub::utils::JsonConfig (new callback-based design).

#include "test_preamble.h"

#include "helpers/test_entrypoint.h" // provides extern std::string g_self_exe_path
#include "helpers/workers.h"
#include "helpers/test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
#include "nlohmann/json.hpp"

#include <fmt/core.h>

using namespace nlohmann;
using namespace test_utils;

namespace {

// --- Test Globals & Helpers ---
static fs::path g_temp_dir;

static std::string read_file_contents(const fs::path &p)
{
    std::ifstream in(p);
    if (!in.is_open()) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Test fixture to manage the temp directory and utils lifecycle.
class JsonConfigTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_temp_dir = fs::temp_directory_path() / "pylabhub_jsonconfig_tests";
        fs::create_directories(g_temp_dir);
        fmt::print("Using temporary directory: {}\n", g_temp_dir.string());
    }

    static void TearDownTestSuite() {
        try { fs::remove_all(g_temp_dir); } catch (...) {}
    }
};

} // anonymous namespace

// -----------------------------------------------------------------------------
// Init/Create behavior
// -----------------------------------------------------------------------------
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

    // Read from memory (after init -> reload)
    bool read_ok = config.with_json_read([&](const json &j){
        ASSERT_TRUE(j.is_object());
        ASSERT_TRUE(j.empty());
    }, &ec);
    ASSERT_TRUE(read_ok);
    ASSERT_FALSE(ec);

    // Re-init via constructor
    JsonConfig config2(cfg_path, false, &ec);
    ASSERT_FALSE(ec);
    bool read2_ok = config2.with_json_read([&](const json &j){
        ASSERT_TRUE(j.is_object());
        ASSERT_TRUE(j.empty());
    }, &ec);
    ASSERT_TRUE(read2_ok);
}

// -----------------------------------------------------------------------------
// Uninitialized object behavior
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, UninitializedBehavior)
{
    JsonConfig config;

    std::error_code ec;
    // init not called; is_initialized should be false
    ASSERT_FALSE(config.is_initialized());
    ASSERT_FALSE(config.save(&ec));
    ASSERT_NE(ec.value(), 0); // should set an error code for diagnostics on failure

    // with_json_read/write should return false and set ec
    ec.clear();
    ASSERT_FALSE(config.with_json_read([&](const json &){}, &ec));
    ASSERT_NE(ec.value(), 0);

    ec.clear();
    ASSERT_FALSE(config.with_json_write([&](json &){}, std::chrono::milliseconds{0}, &ec));
    ASSERT_NE(ec.value(), 0);
}

// -----------------------------------------------------------------------------
// Basic read/write access through callbacks
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, BasicAccessors)
{
    auto cfg_path = g_temp_dir / "accessors.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // Write some values using with_json_write
    ASSERT_TRUE(cfg.with_json_write([&](json &j){
        j["int_val"] = 42;
        j["str_val"] = "hello";
        j["obj"] = json::object();
        j["obj"]["x"] = 100;
        j["obj"]["s"] = "world";
    }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    // Read back with with_json_read
    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("int_val", -1), 42);
        ASSERT_EQ(j.value("str_val", std::string{}), "hello");
        ASSERT_TRUE(j.contains("obj"));
    }, &ec));
    ASSERT_FALSE(ec);

    // Update an element with write callback and read it
    ASSERT_TRUE(cfg.with_json_write([&](json &j){
        j["int_val"] = j.value("int_val", 0) + 1;
    }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("int_val", -1), 43);
    }, &ec));
}

// -----------------------------------------------------------------------------
// Reload after external modification
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, ReloadOnDiskChange)
{
    auto cfg_path = g_temp_dir / "reload_on_disk.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // Write initial value
    ASSERT_TRUE(cfg.with_json_write([&](json &j){
        j["value"] = 1;
    }, std::chrono::milliseconds{0}, &ec));
    ASSERT_FALSE(ec);

    // Externally modify file
    {
        std::ofstream out(cfg_path);
        out << R"({ "value": 2, "new_key": "external" })";
    }

    // Call reload() to pick up external change and verify via read callback
    ASSERT_TRUE(cfg.reload(&ec));
    ASSERT_FALSE(ec);

    ASSERT_TRUE(cfg.with_json_read([&](const json &j){
        ASSERT_EQ(j.value("value", -1), 2);
        ASSERT_EQ(j.value("new_key", std::string{}), "external");
    }, &ec));
    ASSERT_FALSE(ec);
}

// -----------------------------------------------------------------------------
// Recursion guard for nested reads
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, RecursionGuardForReads)
{
    auto cfg_path = g_temp_dir / "recursion_reads.json";
    fs::remove(cfg_path);

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    // Outer read should succeed; inner attempt to call with_json_read should be refused
    bool outer_ok = cfg.with_json_read([&]([[maybe_unused]] const json &j){
        // nested call should be refused (RecursionGuard)
        std::error_code inner_ec;
        bool nested = cfg.with_json_read([&](const json &) {
            // should not run
        }, &inner_ec);
        ASSERT_FALSE(nested);
        ASSERT_NE(inner_ec.value(), 0);
    }, &ec);
    ASSERT_TRUE(outer_ok);
}

// -----------------------------------------------------------------------------
// Multi-threaded contention
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, MultiThreadContention)
{
    auto cfg_path = g_temp_dir / "multithread_contention.json";
    fs::remove(cfg_path);

    // Pre-populate with initial data using a dedicated instance.
    {
        JsonConfig setup_cfg;
        std::error_code ec;
        ASSERT_TRUE(setup_cfg.init(cfg_path, true, &ec));
        ASSERT_FALSE(ec);
        ASSERT_TRUE(setup_cfg.with_json_write([&](json &data){
            data["counter"] = 0;
            data["write_log"] = json::array();
        }, std::chrono::milliseconds{0}, &ec));
        ASSERT_FALSE(ec);
    }

    const int THREADS = 16;
    const int ITERS = 100;
    std::vector<std::thread> threads;
    std::atomic<int> read_failures = 0;
    std::atomic<int> successful_writes = 0;

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([=, &read_failures, &successful_writes]() {
            JsonConfig cfg(cfg_path);
            std::srand(static_cast<unsigned int>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()) + i));
            int last_read_value = -1;

            for (int j = 0; j < ITERS; ++j)
            {
                if (std::rand() % 4 == 0) // writer
                {
                    std::error_code ec;
                    bool ok = cfg.with_json_write([&](json &data) {
                        int v = data.value("counter", 0);
                        data["counter"] = v + 1;
                        std::string id = fmt::format("T{}-{}", i, j);
                        data["write_log"].push_back(id);
                    }, std::chrono::milliseconds{0}, &ec);

                    if (ok && ec.value() == 0)
                        successful_writes++;
                    // If write failed due to contention or other reasons, it's acceptable;
                    // the test relies on eventual progress and no deadlocks.
                }
                else // reader
                {
                    std::error_code ec;
                    bool ok = cfg.with_json_read([&](const json &data) {
                        int cur = data.value("counter", -1);
                        if (cur < last_read_value)
                            read_failures++;
                        last_read_value = cur;
                    }, &ec);
                    if (!ok) read_failures++;
                }

                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_EQ(read_failures.load(), 0);

    // Final verification
    JsonConfig verifier(cfg_path);
    std::error_code ec;
    ASSERT_TRUE(verifier.with_json_read([&](const json &data) {
        int final_counter = data.value("counter", -1);
        json final_log = data.value("write_log", json::array());
        EXPECT_EQ(final_counter, static_cast<int>(final_log.size()));
        EXPECT_GT(final_counter, 0);
    }, &ec));
}

// -----------------------------------------------------------------------------
// Multi-process contention using worker helper (spawned child processes)
// -----------------------------------------------------------------------------
TEST_F(JsonConfigTest, MultiProcessContention)
{
    auto cfg_path = g_temp_dir / "multiprocess_contention.json";
    fs::remove(cfg_path);

    JsonConfig creator;
    std::error_code ec;
    ASSERT_TRUE(creator.init(cfg_path, true, &ec));
    ASSERT_FALSE(ec);

    const int PROCS = 8; // reduce for CI reliability; bump to 16 if desired
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        HANDLE h = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("win-{}", i)});
        ASSERT_TRUE(h != nullptr);
        procs.push_back(h);
    }
    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 0) success_count++;
        CloseHandle(h);
    }
#else
    std::vector<ProcessHandle> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        pid_t pid = spawn_worker_process(
            g_self_exe_path, "jsonconfig.write_id",
            {cfg_path.string(), fmt::format("posix-{}", i)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }
    for (auto pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success_count++;
    }
#endif

    ASSERT_EQ(success_count, PROCS);

    JsonConfig verifier(cfg_path);
    // ec is already declared at the beginning of the test function

#if defined(PLATFORM_WIN64)
    std::string s_key_fmt = "win-{}";
#else
    std::string s_key_fmt = "posix-{}";
#endif

    std::vector<std::string> keys;
    for (int i = 0; i < PROCS; ++i)
    {
        keys.push_back(fmt::format(fmt::runtime(s_key_fmt), i));
    }

    ASSERT_TRUE(verifier.with_json_read([&](const json &data) {
        for (int i = 0; i < PROCS; ++i)
        {
            std::string key = keys[i];

            ASSERT_TRUE(data.contains(key)) << "Worker " << key << " failed to write.";
        }
    }, &ec));
}

// -----------------------------------------------------------------------------
// Symlink attack prevention
// -----------------------------------------------------------------------------
#if PYLABHUB_IS_POSIX
TEST_F(JsonConfigTest, SymlinkAttackPreventionPosix)
{
    auto real_file = g_temp_dir / "real_file.txt";
    auto symlink_path = g_temp_dir / "config_symlink.json";
    fs::remove(real_file);
    fs::remove(symlink_path);

    // Create a "sensitive" real file
    {
        std::ofstream out(real_file);
        out << R"({ "original": "data" })";
    }

    fs::create_symlink(real_file, symlink_path);
    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(symlink_path, false, &ec));
    ASSERT_FALSE(ec);

    bool ok = cfg.with_json_write([&](json &j){
        j["malicious"] = "data";
    }, std::chrono::milliseconds{0}, &ec);
    ASSERT_FALSE(ok);
    ASSERT_NE(ec.value(), 0);

    // Confirm real file unchanged
    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

#if defined(PLATFORM_WIN64)
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

    // Creating symlink on Windows may require privileges; skip if cannot create.
    if (!CreateSymbolicLinkW(symlink_path.wstring().c_str(), real_file.wstring().c_str(), 0))
    {
        GTEST_SKIP() << "Skipping Windows symlink test: insufficient privileges.";
    }

    ASSERT_TRUE(fs::is_symlink(symlink_path));

    JsonConfig cfg;
    std::error_code ec;
    ASSERT_TRUE(cfg.init(symlink_path, false, &ec));
    ASSERT_FALSE(ec);

    bool ok = cfg.with_json_write([&](json &j){
        j["malicious"] = "data";
    }, std::chrono::milliseconds{0}, &ec);

    ASSERT_FALSE(ok);
    ASSERT_NE(ec.value(), 0);

    json j = json::parse(read_file_contents(real_file));
    ASSERT_EQ(j["original"], "data");
    ASSERT_EQ(j.find("malicious"), j.end());
}
#endif

// End of file
