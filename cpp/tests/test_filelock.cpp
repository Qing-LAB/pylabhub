// test_filelock.cpp
// Tests FileLock behavior: non-blocking lock detects contention.
// This test uses two FileLock instances in the same process to the same path.

#include <cassert>
#include <filesystem>
#include <iostream>

#include "fileutil/FileLock.hpp"

using namespace pylabhub::fileutil;

int main()
{
    namespace fs = std::filesystem;
    fs::path tmpdir = fs::temp_directory_path();
    fs::path target = tmpdir / "pylabhub_test_filelock.json";

    // Ensure the target exists
    {
        std::ofstream o(target);
        o << "{}\n";
    }

    // Acquire first lock (non-blocking) - should succeed
    FileLock lock1(target, LockMode::NonBlocking);
    if (!lock1.valid())
    {
        std::cerr << "test_filelock: failed to acquire first lock: " << lock1.error_code().message()
                  << std::endl;
        return 2;
    }

    // Acquire second lock (non-blocking) on same path - should fail
    FileLock lock2(target, LockMode::NonBlocking);
    if (lock2.valid())
    {
        std::cerr << "test_filelock: second non-blocking lock unexpectedly succeeded\n";
        return 3;
    }
    else
    {
        std::cout << "test_filelock: second non-blocking lock correctly failed with: "
                  << lock2.error_code().message() << std::endl;
    }

    // Release first lock by destroying it, then try again
    // (lock1 goes out of scope at end of block)
    // Recreate lock1 via scope
    {
        FileLock l(target, LockMode::NonBlocking);
        if (!l.valid())
        {
            std::cerr << "test_filelock: re-acquire after release failed: "
                      << l.error_code().message() << std::endl;
            return 4;
        }
    }

    // Clean up
    try
    {
        std::filesystem::remove(target);
    }
    catch (...)
    {
    }

    std::cout << "test_filelock: OK\n";
    return 0;
}
