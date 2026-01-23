// tests/test_harness/shared_test_helpers.cpp
/**
 * @file shared_test_helpers.cpp
 * @brief Implements common helper functions and utilities for test cases.
 */

#include "plh_base.hpp"

#include <fstream>

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

} // namespace pylabhub::tests::helper