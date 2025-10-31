// test_jsonconfig.cpp
#include <iostream>
#include <cassert>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <string>
#include "JsonConfig.hpp"
#include "FileLock.hpp"

using namespace pylabhub::fileutil;
namespace fs = std::filesystem;

#if defined(_WIN32)
#include <windows.h>
#endif

static void panic(const std::string& m) {
    std::cerr << "FAIL: " << m << std::endl;
    std::exit(3);
}

// child helper: hold lock on file for seconds (same as in filelock test)
int run_child_hold_lock(const std::string& path, int seconds) {
    FileLock flock(path, LockMode::Blocking);
    if (!flock.valid()) {
        auto ec = flock.error_code();
        std::cerr << "child: failed to acquire blocking lock: " << ec.message() << "\n";
        return 1;
    }
    std::cout << "child: holding lock for " << seconds << "s\n";
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::cout << "child: done\n";
    return 0;
}

int spawn_lock_holder(const fs::path& exe, const fs::path& target, int seconds) {
#if defined(_WIN32)
    std::wstring cmd = L"\"";
    cmd += exe.wstring();
    cmd += L"\" --hold-lock \"";
    cmd += target.wstring();
    cmd += L"\" ";
    cmd += std::to_wstring(seconds);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "CreateProcessW failed: " << GetLastError() << "\n";
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::string s_exe = exe.string();
        std::string s_target = target.string();
        execl(s_exe.c_str(), s_exe.c_str(), "--hold-lock", s_target.c_str(), std::to_string(seconds).c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
#endif
}

int main(int argc, char** argv) {
    // child holder mode
    if (argc >= 2 && std::string(argv[1]) == "--hold-lock") {
        if (argc < 4) {
            std::cerr << "usage: --hold-lock <path> <seconds>\n";
            return 1;
        }
        std::string path = argv[2];
        int sec = std::atoi(argv[3]);
        return run_child_hold_lock(path, sec);
    }

    std::cout << "TEST: JsonConfig behaviors\n";

    fs::path tmpdir = fs::temp_directory_path();
    fs::path configPath = tmpdir / ("test_jsonconfig_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".json");

    // ensure dir
    fs::create_directories(configPath.parent_path());

    // 1) init(createIfMissing=true)
    {
        JsonConfig cfg;
        bool ok = cfg.init(configPath, true);
        if (!ok) panic("init(createIfMissing) failed");
        std::cout << "init(createIfMissing) ok\n";
    }

    // 2) set/get/get_optional/get_or
    {
        JsonConfig cfg(configPath);
        cfg.set("a.b.c", 123);
        cfg.set("a.str", std::string("hello"));
        auto v = cfg.get_optional<int>("a.b.c");
        if (!v || *v != 123) panic("get_optional failed or wrong value");
        if (cfg.get_or<int>("a.x", 999) != 999) panic("get_or failed");
        int x = cfg.get<int>("a.b.c");
        if (x != 123) panic("get returned wrong value");
        std::cout << "set/get/get_optional/get_or OK\n";
    }

    // 3) with_json_read / with_json_write
    {
        JsonConfig cfg(configPath);
        bool wrote = cfg.with_json_write([](json& j) {
            j["rw_test"] = 42;
            });
        if (!wrote) panic("with_json_write failed");
        bool read_ok = cfg.with_json_read([](const json& j) {
            if (!j.contains("rw_test") || j["rw_test"].get<int>() != 42) panic("with_json_read observed wrong data");
            });
        if (!read_ok) panic("with_json_read returned false");
        std::cout << "with_json_read/write OK\n";
    }

    // 4) save then reload
    {
        JsonConfig cfg(configPath);
        cfg.set("persist.me", std::string("on_disk"));
        if (!cfg.save()) panic("save failed");
        // create a fresh instance and reload
        JsonConfig cfg2;
        if (!cfg2.init(configPath, false)) panic("init on reload failed");
        if (!cfg2.reload()) panic("reload failed");
        auto s = cfg2.get<std::string>("persist.me");
        if (s != "on_disk") panic("reload did not persist value");
        std::cout << "save/reload OK\n";
    }

    // 5) replace atomic behavior + non-blocking lock failure
    {
        JsonConfig cfg(configPath);
        json newcfg = json::object();
        newcfg["replaced"] = true;

        // spawn a helper process to hold the lock
        fs::path exe = fs::canonical(argv[0]);
        std::cout << "Spawning lock-holder child for replace test\n";
        int child = spawn_lock_holder(exe, configPath, 4);
        if (child <= 0) panic("spawn failed");

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        bool rep = cfg.replace(newcfg);
        if (rep) panic("replace should have failed because lock is held by child");
        std::cout << "replace failed as expected due to lock\n";

#if defined(_WIN32)
        std::this_thread::sleep_for(std::chrono::seconds(5));
#else
        int status = 0;
        waitpid(child, &status, 0);
#endif

        // Now replace should succeed
        if (!cfg.replace(newcfg)) panic("replace failed after child release");
        std::cout << "replace succeeded after lock released\n";

        // verify on-disk matches
        JsonConfig cfg2;
        cfg2.init(configPath, false);
        cfg2.reload();
        if (!cfg2.has("replaced")) panic("disk reload missing replaced");
        if (!cfg2.get<bool>("replaced")) panic("value on disk not true");
    }

    // 6) remove and has
    {
        JsonConfig cfg(configPath);
        cfg.set("to.remove.x", 10);
        if (!cfg.has("to.remove.x")) panic("has() missing key");
        if (!cfg.remove("to.remove.x")) panic("remove returned false");
        if (cfg.has("to.remove.x")) panic("has() still true after remove");
        std::cout << "remove/has OK\n";
    }

    // cleanup
    try { if (fs::exists(configPath)) fs::remove(configPath); }
    catch (...) {}

    std::cout << "JsonConfig tests passed\n";
    return 0;
}
