// test_filelock.cpp
// Test for FileLock: multi-threaded and multi-process checks.
//
// Usage:
//   ./test_filelock                <-- master: runs both thread & process tests
//   ./test_filelock worker <path>  <-- child worker mode (attempt NonBlocking lock on <path>)

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fileutil/FileLock.hpp"

using namespace pylabhub::fileutil;
namespace fs = std::filesystem;

static std::string make_temp_base() {
    std::string tmp;
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetTempPathA(MAX_PATH, buf))
        tmp = std::string(buf);
    else
        tmp = ".\\";
#else
    const char *t = std::getenv("TMPDIR");
    tmp = t ? t : "/tmp/";
#endif
    return tmp;
}

#if defined(_WIN32)
// Windows create process helper - returns process HANDLE on success (must CloseHandle)
static HANDLE spawn_process_w(const std::string &exe, const std::string &arg0, const std::string &arg1) {
    std::string cmdline = '"' + exe + "\" " + arg0 + " \"" + arg1 + '"';
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    // convert to wide
    int wide = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0);
    std::wstring wcmd(wide, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, &wcmd[0], wide);
    if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
static pid_t spawn_process_posix(const std::string &exe, const std::string &arg0, const std::string &arg1) {
    pid_t pid = fork();
    if (pid == 0) {
        // child
        execl(exe.c_str(), exe.c_str(), arg0.c_str(), arg1.c_str(), nullptr);
        // If exec failed:
        _exit(127);
    }
    return pid;
}
#endif

// Worker mode: attempt NonBlocking FileLock on provided lock path and write file on success
static int worker_filelock_mode(const std::string &lockpath, const std::string &outfile) {
    FileLock lock(fs::path(lockpath), LockMode::NonBlocking);
    if (!lock.valid()) {
        // failed to obtain in-process/OS lock
        std::cout << "WORKER: failed to acquire lock: " << lock.error_code().message() << "\n";
        return 2;
    }
    // got it; write our pid/thread id into outfile
    std::ofstream out(outfile, std::ios::app);
    if (out.is_open()) {
#if defined(_WIN32)
        DWORD pid = GetCurrentProcessId();
        out << "pid:" << pid << "\n";
#else
        pid_t pid = getpid();
        out << "pid:" << pid << "\n";
#endif
        out.close();
    }
    // Keep lock for a short moment so other workers likely fail
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "worker-filelock") == 0) {
        if (argc < 4) return 3;
        return worker_filelock_mode(argv[2], argv[3]);
    }

    std::cout << "*** test_filelock: start\n";

    // Prepare temp files
    std::string tmpbase = make_temp_base();
    fs::path lockfile = fs::path(tmpbase) / "test_filelock.lock";
    fs::path outfile = fs::path(tmpbase) / "test_filelock.out";

    // ensure clean
    std::error_code ec;
    fs::remove(lockfile, ec);
    fs::remove(outfile, ec);

    // ----- Threaded test -----
    {
        const int THREADS = 12;
        std::atomic<int> success_count{0};
        std::vector<std::thread> thr;
        for (int i = 0; i < THREADS; ++i) {
            thr.emplace_back([&](){
                FileLock lock(lockfile, LockMode::NonBlocking);
                if (lock.valid()) {
                    // success
                    success_count.fetch_add(1, std::memory_order_relaxed);
                    std::ofstream out(outfile, std::ios::app);
                    if (out.is_open()) out << "thread:" << std::this_thread::get_id() << "\n";
                }
            });
        }
        for (auto &t : thr) t.join();
        int succ = success_count.load();
        std::cout << "threaded test: success_count=" << succ << "\n";
        if (succ != 1) {
            std::cerr << "threaded test failed: expected 1 thread to acquire lock non-blocking\n";
            return 2;
        }
    }

    // remove outfile for process run
    fs::remove(outfile, ec);

    // ----- Process test -----
    {
        const int PROCS = 9;
#if defined(_WIN32)
        std::string exe = argv[0];
        std::vector<HANDLE> procs;
        for (int i = 0; i < PROCS; ++i) {
            HANDLE h = spawn_process_w(exe, "worker-filelock", lockfile.string() + "\x1f" + outfile.string());
            if (!h) {
                std::cerr << "spawn failed\n";
                return 5;
            }
            procs.push_back(h);
            // slight stagger
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Wait for children
        int success_count = 0;
        for (auto h : procs) {
            DWORD st = WaitForSingleObject(h, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(h, &code);
            if (code == 0) ++success_count;
            CloseHandle(h);
        }
#else
        std::string exe = argv[0];
        std::vector<pid_t> pids;
        for (int i = 0; i < PROCS; ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                // child: exec same program in worker mode
                execl(exe.c_str(), exe.c_str(), "worker-filelock", lockfile.c_str(), outfile.c_str(), nullptr);
                _exit(127);
            } else if (pid < 0) {
                std::cerr << "fork failed\n";
                return 6;
            }
            pids.push_back(pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        int success_count = 0;
        for (pid_t pid : pids) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ++success_count;
        }
#endif
        // On POSIX branch we counted success_count; define here for both branches:
#if defined(_WIN32)
        int successes = 0;
        // read outfile to count lines written
        std::ifstream in(outfile);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) ++successes;
        }
        std::cout << "process test: successes(file lines)=" << successes << "\n";
        if (successes != 1) {
            std::cerr << "process test failed: expected exactly 1 process to acquire lock\n";
            return 7;
        }
#else
        std::cout << "process test: success_count=" << success_count << "\n";
        if (success_count != 1) {
            std::cerr << "process test failed: expected exactly 1 process to acquire lock\n";
            return 7;
        }
#endif
    }

    std::cout << "test_filelock: OK\n";
    return 0;
}
