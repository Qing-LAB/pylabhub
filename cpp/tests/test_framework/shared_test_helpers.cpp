// tests/test_harness/shared_test_helpers.cpp
/**
 * @file shared_test_helpers.cpp
 * @brief Implements common helper functions and utilities for test cases.
 */

#include "plh_base.hpp"

#include <chrono> // Required for std::chrono::milliseconds
#include <fstream>
#include <thread> // Required for std::this_thread::sleep_for

#if PYLABHUB_IS_POSIX
#include <sys/mman.h> // For shm_unlink
#include <cerrno>     // For errno
#endif

#include "plh_service.hpp"
#include "shared_test_helpers.h"

namespace pylabhub::tests::helper
{

bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

#include <span>

#include <optional>
#include <string_view>

size_t count_lines(std::string_view text, std::optional<std::string_view> must_include,
                   std::optional<std::string_view> must_exclude)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < text.size())
    {
        auto end = text.find('\n', pos);
        auto line = text.substr(pos, end - pos);

        if ((!must_include || line.find(*must_include) != std::string_view::npos) &&
            (!must_exclude || line.find(*must_exclude) == std::string_view::npos))
        {
            ++count;
        }

        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }

    return count;
}

bool wait_for_string_in_file(const fs::path &path, const std::string &expected,
                             std::chrono::milliseconds timeout)
{
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout)
    {
        std::string contents;
        if (read_file_contents(path.string(), contents))
        {
            if (contents.find(expected) != std::string::npos)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // 'getenv': This function or variable may be unsafe.
#endif
std::string test_scale()
{
    const char *v = std::getenv("PYLAB_TEST_SCALE");
    return v ? std::string(v) : std::string();
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

int scaled_value(int original, int small_value)
{
    if (test_scale() == "small")
        return small_value;
    return original;
}

// Note: The `run_gtest_worker` template function is defined in the header
// `shared_test_helpers.h` as it needs to be available to multiple cpp files.

// ============================================================================
// DataBlock Test Utilities Implementation
// ============================================================================

std::string make_test_channel_name(const char *test_name)
{
    // Generate unique name with timestamp
    auto now = std::chrono::steady_clock::now();
    auto timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    return fmt::format("test_{}_{}", test_name, timestamp);
}

bool cleanup_test_datablock(const std::string &channel_name)
{
    // Attempt to clean up shared memory for the given channel
    // Platform-specific cleanup

#if PYLABHUB_IS_POSIX
    // POSIX: Use shm_unlink
    std::string shm_path = "/" + channel_name;
    int result = shm_unlink(shm_path.c_str());

    if (result == 0)
    {
        return true;
    }
    else if (errno == ENOENT)
    {
        // Already doesn't exist, that's fine
        return true;
    }
    else
    {
        // Other error
        LOGGER_WARN("[TestCleanup] Failed to unlink shared memory '{}': errno={}", shm_path, errno);
        return false;
    }
#elif PYLABHUB_IS_WINDOWS
    // Windows: Shared memory cleanup handled by OS when last handle closed
    // Nothing to do explicitly here
    return true;
#else
    // Unknown platform
    LOGGER_WARN("[TestCleanup] Cleanup not implemented for this platform");
    return false;
#endif
}

} // namespace pylabhub::tests::helper