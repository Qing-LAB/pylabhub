// --- Helper functions for string formatting ---

#include "format_tools.hpp"

namespace pylabhub::basic::tools {

// --- Helper: formatted local time with sub-second resolution (robust) ---
// Replaces previous formatted_time(...) implementation.
// Behaviour:
//  - If the build system detected fmt chrono subseconds support (HAVE_FMT_CHRONO_SUBSECONDS),
//    use single-step fmt formatting on a microsecond-truncated time_point.
//  - Otherwise, fall back to computing the fractional microsecond part and append it
//    manually using a two-step format.
std::string formatted_time(std::chrono::system_clock::time_point timestamp)
{
#if defined(HAVE_FMT_CHRONO_SUBSECONDS) && HAVE_FMT_CHRONO_SUBSECONDS
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
#if defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 1)
    // use %f
    return fmt::format("{:%Y-%m-%d %H:%M:%S.%f}", tp_us);
#elif defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 2)
    // fmt prints fraction without %f
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", tp_us);
#else
    // defensive fallback to manual two-step
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
    int fractional_us = static_cast<int>((tp_us - secs).count());
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
#else
    // no runtime support detected — fallback to manual two-step method
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
    int fractional_us = static_cast<int>((tp_us - secs).count());
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
}

} // namespace pylabhub::basic::tools