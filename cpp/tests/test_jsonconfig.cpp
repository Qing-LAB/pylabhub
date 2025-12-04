// test_jsonconfig.cpp
// Unit test for JsonConfig: function tests, multithread and multiprocess tests.
//
// Usage:
//  ./test_jsonconfig                 <- run all tests (master)
//  ./test_jsonconfig worker <path>   <- worker mode used by multiprocess test
//
// Exit code 0 = success. Non-zero indicates failure in one of the test cases.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fileutil/JsonConfig.hpp"

using namespace pylabhub::fileutil;
namespace fs = std::filesystem;

static std::string temp_dir()
{
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetTempPathA(MAX_PATH, buf))
        return std::string(buf);
    return ".\\";
#else
    const char *t = std::getenv("TMPDIR");
    return std::string(t ? t : "/tmp/");
#endif
}

// Helper: read a top-level string key from JSON file; return empty string if not found
static std::string read_key_as_string(const fs::path &p, const std::string &key)
{
    std::ifstream in(p);
    if (!in.is_open())
        return "";
    try
    {
        nlohmann::json j;
        in >> j;
        if (!j.is_object())
            return "";
        if (!j.contains(key))
            return "";
        if (j[key].is_string())
            return j[key].get<std::string>();
        // convert non-string to string representation
        std::ostringstream ss;
        ss << j[key];
        return ss.str();
    }
    catch (...)
    {
        return "";
    }
}

// Worker mode: each worker will attempt to write a unique marker into the config via
// with_json_write + save. Return code 0 indicates it succeeded in saving (i.e., acquisition of
// cross-process lock and atomic write).
static int worker_mode(const std::string &cfgpath, const std::string &worker_id)
{
    try
    {
        JsonConfig cfg(cfgpath);
        // Try to perform a quick write under with_json_write; if another process holds the file
        // lock, the atomic save may fail (non-blocking FileLock in save_locked). We return 0 only
        // on success.
        bool ok = cfg.with_json_write(
            [&]()
            {
                // mutate in-memory
                cfg.set("worker", worker_id);
                // ensure we call save() inside with_json_write to exercise fast-path
                bool s = cfg.save();
                return s;
            });
        return ok ? 0 : 2;
    }
    catch (...)
    {
        return 3;
    }
}

// Utility to spawn another process (worker)
// Returns process handle/identifier in platform-appropriate type, or -1 on failure
#if defined(_WIN32)
static HANDLE spawn_worker_process(const std::string &exe, const std::string &cfgpath,
                                   const std::string &worker_id)
{
    std::ostringstream cmd;
    cmd << '"' << exe << "\" worker \"" << cfgpath << "\" \"" << worker_id << '"';
    std::string cmdline = cmd.str();

    // Convert to wide string
    int wide = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0);
    std::wstring wcmd(wide, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, &wcmd[0], wide);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
static pid_t spawn_worker_process(const std::string &exe, const std::string &cfgpath,
                                  const std::string &worker_id)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child
        execl(exe.c_str(), exe.c_str(), "worker", cfgpath.c_str(), worker_id.c_str(), nullptr);
        _exit(127); // exec failed
    }
    return pid;
}
#endif

// Basic functional tests: templates, read/write, dirty behavior inferred
static bool basic_functional_tests(const fs::path &cfgfile)
{
    std::cout << "[TEST] basic functional tests\n";
    std::error_code ec;
    fs::remove(cfgfile, ec);

    JsonConfig cfg(cfgfile);
    // set/get/get_or/has/erase/update
    if (!cfg.set("int_val", 42))
    {
        std::cerr << "set failed\n";
        return false;
    }
    try
    {
        int v = cfg.get<int>("int_val");
        if (v != 42)
        {
            std::cerr << "get returned wrong value\n";
            return false;
        }
    }
    catch (...)
    {
        std::cerr << "get threw\n";
        return false;
    }

    int v2 = cfg.get_or<int>("nonexistent", 7);
    if (v2 != 7)
    {
        std::cerr << "get_or returned wrong default\n";
        return false;
    }

    if (!cfg.has("int_val"))
    {
        std::cerr << "has failed\n";
        return false;
    }

    if (!cfg.erase("int_val"))
    {
        std::cerr << "erase failed\n";
        return false;
    }
    if (cfg.has("int_val"))
    {
        std::cerr << "erase did not remove key\n";
        return false;
    }

    bool upd = cfg.update("obj",
                          [](nlohmann::json &j)
                          {
                              j["x"] = 100;
                              j["s"] = "hello";
                          });
    if (!upd)
    {
        std::cerr << "update failed\n";
        return false;
    }

    try
    {
        auto j = cfg.get<nlohmann::json>("obj");
        if (!j.is_object() || j["x"] != 100)
        {
            std::cerr << "update content wrong\n";
            return false;
        }
    }
    catch (...)
    {
        std::cerr << "get after update threw\n";
        return false;
    }

    std::cout << "[OK] basic functional tests\n";
    return true;
}

// Test with_json_read: callback receives const json& and can read without copying
static bool test_with_json_read(const fs::path &cfgfile)
{
    std::cout << "[TEST] with_json_read\n";
    std::error_code ec;
    fs::remove(cfgfile, ec);

    JsonConfig cfg(cfgfile);
    cfg.set("k", "v");
    // with_json_read returns true and callback reads the const ref
    bool ok = cfg.with_json_read(
        [&](const nlohmann::json &ref)
        {
            if (!ref.is_object())
                throw std::runtime_error("expected object");
            if (!ref.contains("k"))
                throw std::runtime_error("missing k");
            if (ref["k"] != "v")
                throw std::runtime_error("unexpected value");
        });
    if (!ok)
    {
        std::cerr << "with_json_read returned false\n";
        return false;
    }

    // ensure exceptions thrown inside callback are swallowed and with_json_read returns false
    bool threwBad = cfg.with_json_read([&](const nlohmann::json &ref) -> void
                                       { throw std::runtime_error("test-ex"); });
    if (threwBad)
    {
        std::cerr << "with_json_read did not return false on exception\n";
        return false;
    }

    std::cout << "[OK] with_json_read\n";
    return true;
}

// Test with_json_write nested detection and save-in-callback behavior
static bool test_with_json_write_and_save(const fs::path &cfgfile)
{
    std::cout << "[TEST] with_json_write and save behavior\n";
    std::error_code ec;
    fs::remove(cfgfile, ec);

    JsonConfig cfg(cfgfile);

    // nested with_json_write on same instance should be refused
    bool outer_ok = cfg.with_json_write(
        [&]()
        {
            bool inner_ok = cfg.with_json_write([]() { return true; });
            if (inner_ok)
            {
                std::cerr << "nested with_json_write unexpectedly allowed\n";
                return false;
            }
            return true;
        });
    if (!outer_ok)
    {
        std::cerr << "outer with_json_write failed\n";
        return false;
    }

    // save() inside with_json_write should not deadlock and should persist
    bool ok = cfg.with_json_write(
        [&]()
        {
            cfg.set("inner", "yes");
            bool s = cfg.save();
            return s;
        });
    if (!ok)
    {
        std::cerr << "with_json_write+save failed\n";
        return false;
    }

    // read file to verify persistence
    std::string val = read_key_as_string(cfgfile, "inner");
    if (val != "yes")
    {
        std::cerr << "inner not persisted, val='" << val << "'\n";
        return false;
    }

    std::cout << "[OK] with_json_write and save\n";
    return true;
}

// Test multithread behavior: thread A holds with_json_write and sleeps; thread B calls save() and
// must finish afterwards.
static bool test_multithread_save_ordering(const fs::path &cfgfile)
{
    std::cout << "[TEST] multithread save ordering\n";
    std::error_code ec;
    fs::remove(cfgfile, ec);

    JsonConfig cfg(cfgfile);
    std::atomic<bool> a_has{false}, b_done{false};

    std::thread tA(
        [&]()
        {
            cfg.with_json_write(
                [&]()
                {
                    // mark and hold mutex for a short time
                    cfg.set("th", 1);
                    a_has.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    return true;
                });
        });

    std::thread tB(
        [&]()
        {
            // wait until A has acquired with_json_write
            while (!a_has.load())
                std::this_thread::yield();
            // call save(); it will block until A releases the _initMutex and then proceed
            bool s = cfg.save();
            if (!s)
            {
                std::cerr << "threaded save() failed\n";
                std::exit(101);
            }
            b_done.store(true);
        });

    tA.join();
    tB.join();
    if (!b_done.load())
    {
        std::cerr << "threaded B did not complete\n";
        return false;
    }
    std::cout << "[OK] multithread save ordering\n";
    return true;
}

// Multiprocess test: spawn WORKERS worker processes that attempt to write (non-blocking).
// Exactly one should succeed (others will fail to acquire cross-process FileLock).
static bool test_multiprocess(const fs::path &cfgfile, const std::string &self_exe)
{
    std::cout << "[TEST] multiprocess exclusive writes\n";
    std::error_code ec;
    fs::remove(cfgfile, ec);

    const int WORKERS = 6;
#if defined(_WIN32)
    std::vector<HANDLE> handles;
    for (int i = 0; i < WORKERS; ++i)
    {
        std::ostringstream id;
        id << "w" << i;
        HANDLE h = spawn_worker_process(self_exe, cfgfile.string(), id.str());
        if (!h)
        {
            std::cerr << "CreateProcess failed\n";
            return false;
        }
        handles.push_back(h);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    int successes = 0;
    for (auto h : handles)
    {
        DWORD code = 0;
        WaitForSingleObject(h, INFINITE);
        GetExitCodeProcess(h, &code);
        CloseHandle(h);
        if (code == 0)
            ++successes;
    }
    if (successes != 1)
    {
        std::cerr << "multiprocess: expected 1 success, got " << successes << "\n";
        return false;
    }
#else
    std::vector<pid_t> pids;
    for (int i = 0; i < WORKERS; ++i)
    {
        std::ostringstream id;
        id << "w" << i;
        pid_t pid = spawn_worker_process(self_exe, cfgfile.string(), id.str());
        if (pid <= 0)
        {
            std::cerr << "fork failed\n";
            return false;
        }
        pids.push_back(pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int successes = 0;
    for (pid_t pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ++successes;
    }
    if (successes != 1)
    {
        std::cerr << "multiprocess: expected 1 success, got " << successes << "\n";
        return false;
    }
#endif

    // Verify that the file contains a "worker" key (the winning worker wrote it)
    std::string winner = read_key_as_string(cfgfile, "worker");
    if (winner.empty())
    {
        std::cerr << "multiprocess: file missing worker key\n";
        return false;
    }

    std::cout << "[OK] multiprocess exclusive writes, winner='" << winner << "'\n";
    return true;
}

int main(int argc, char **argv)
{
    // Worker mode: invoked by master to test multiprocess behavior
    if (argc >= 2 && std::strcmp(argv[1], "worker") == 0)
    {
        if (argc < 4)
        {
            std::cerr << "worker usage: worker <cfgpath> <worker_id>\n";
            return 2;
        }
        std::string cfg = argv[2];
        std::string id = argv[3];
        return worker_mode(cfg, id);
    }

    std::cout << "=== test_jsonconfig starting ===\n";
    fs::path cfgfile = fs::path(temp_dir()) / "test_jsonconfig_unit.json";

    // Run tests, bail out on first failure and return non-zero
    if (!basic_functional_tests(cfgfile))
        return 1;
    if (!test_with_json_read(cfgfile))
        return 2;
    if (!test_with_json_write_and_save(cfgfile))
        return 3;
    if (!test_multithread_save_ordering(cfgfile))
        return 4;

    // Multiprocess requires we know our exe path. argv[0] may be relative; use it directly.
    std::string exe_path = argv[0];
    if (!test_multiprocess(cfgfile, exe_path))
        return 5;

    std::cout << "=== ALL TESTS PASSED ===\n";
    return 0;
}
