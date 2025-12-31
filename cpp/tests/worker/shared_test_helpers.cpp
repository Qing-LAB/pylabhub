#include "platform.hpp"

// Standard Library
#include <chrono>
#include <cstdlib> // For std::getenv
#include <fstream> // For std::ifstream
#include <sstream> // For std::ostringstream
#include <string>
#include <thread> // For std::this_thread

// Third-party
#include <fmt/core.h> // Keep this for fmt::print

// Project-specific
#include "shared_test_helpers.h" // Keep this specific header

namespace pylabhub::tests::helper
{

bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

size_t count_lines(const std::string &s)
{
    size_t count = 0;
    for (char c : s)
        if (c == '\n') ++count;
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
            if (contents.find(expected) != std::string::npos) return true;
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
    if (test_scale() == "small") return small_value;
    return original;
}

// Explicit template instantiation, if needed, or ensure it's used in a way that
// the compiler can see the definition. Since it's header-only, this might not be
// strictly necessary, but can be good practice.
// (No instantiations needed for now as it will be implicitly instantiated)

} // namespace pylabhub::tests::helpers