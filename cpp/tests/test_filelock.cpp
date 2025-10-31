// test_filelock.cpp
#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <string>
#include <cassert>
#include <filesystem>
#include "FileLock.hpp"

using namespace pylabhub::fileutil;
namespace fs = std::filesystem;

#if defined(_WIN32)
#include <windows.h>
#endif

static void panic(const std::string& m) {
    std::cerr << "FAIL: " << m << std::endl;
    std::exit(2);
}

// Helper: child-mode - acquire (blocking) lock and hold for seconds then exit
int run_child_hold_lock(const std::string& path, int seconds) {
    FileLock flock(path, LockMode::Blocking);
    if (!flock.valid()) {
        auto ec = flock.error_code();
        std::cerr << "child: failed to acquire blocking lock: code=" << ec.value()
            << " msg=\"" << ec.message() << "\"\n";
        return 1;
    }
    std::cout << "child: holding lock for " << seconds << " seconds\n";
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::cout << "child: done\n";
    return 0;
}

// Spawn same executable in child-hold-lock mode
int spawn_lock_holder(const fs::path& exe, const fs::path& target, int seconds) {
#if defined(_WIN32)
    // Build command line: "<exe>" --hold-lock "<target>" <seconds>
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
        // child: exe replaced by current program in child mode
        std::string s_exe = exe.string();
        std::string s_target = target.string();
        execl(s_exe.c_str(), s_exe.c_str(), "--hold-lock", s_target.c_str(), std::to_string(seconds).c_str(), (char*)nullptr);
        // if execl fails:
        std::cerr << "execl failed\n";
        _exit(127);
    }
    // parent: pid is child pid
    return pid; // return child pid (>0)
#endif
}

int main(int argc, char** argv) {
    // Child helper mode:
    if (argc >= 2 && std::string(argv[1]) == "--hold-lock") {
        if (argc < 4) {
            std::cerr << "usage: --hold-lock <path> <seconds>\n";
            return 1;
        }
        std::string path = argv[2];
        int sec = std::atoi(argv[3]);
        return run_child_hold_lock(path, sec);
    }

    std::cout << "TEST: FileLock basic behaviors\n";

    // prepare temp file path
    fs::path tmpdir = fs::temp_directory_path();
    fs::path target = tmpdir / ("test_filelock_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".locktest");

    // ensure parent exists
    fs::create_directories(target.parent_path());

    // 1) Acquire lock non-blocking should succeed
    {
        FileLock f(target, LockMode::NonBlocking);
        if (!f.valid()) panic("NonBlocking lock failed unexpectedly (first acquisition).");
        std::cout << "NonBlocking acquire succeeded\n";
        // destructor releases
    }

    // 2) Spawn child that holds lock and test non-blocking failure
    // spawn child process to hold lock for 4 seconds
    fs::path exe = fs::canonical(argv[0]);
    std::cout << "Spawning lock-holder child\n";
#if defined(_WIN32)
    if (spawn_lock_holder(exe, target, 4) != 0) panic("spawn failed");
    // Give the child a moment to acquire the lock
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#else
    pid_t child = spawn_lock_holder(exe, target, 4);
    if (child <= 0) panic("spawn failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

    // Now attempt to acquire non-blocking lock: should fail
    {
        FileLock f2(target, LockMode::NonBlocking);
        if (f2.valid()) {
            panic("NonBlocking lock should have failed while child holds the lock");
        }
        else {
            auto ec = f2.error_code();
            std::cout << "NonBlocking lock correctly failed: code=" << ec.value() << " msg=\"" << ec.message() << "\"\n";
        }
    }

    // Wait for child to exit (or sleep longer)
#if defined(_WIN32)
    std::this_thread::sleep_for(std::chrono::seconds(5));
#else
    int status = 0;
    waitpid(child, &status, 0);
#endif

    // After child exit, acquiring non-blocking lock should succeed again
    {
        FileLock f3(target, LockMode::NonBlocking);
        if (!f3.valid()) panic("NonBlocking lock failed after child exit");
        std::cout << "NonBlocking reacquire after child exit succeeded\n";
    }

    // Cleanup
    try {
        if (fs::exists(target)) fs::remove(target);
        fs::path lockfile = target.parent_path() / (target.filename().string() + ".lock");
        if (fs::exists(lockfile)) fs::remove(lockfile);
    }
    catch (...) {}

    std::cout << "FileLock tests passed\n";
    return 0;
}
