#include "plh_base.hpp"
#include "utils/logger_sinks/sink.hpp"

namespace pylabhub::utils
{

// Returns a string representation for a given log level.
const char *Sink::level_to_string_internal(int lvl)
{
    // A C-style switch on int is used here to avoid a hard dependency on logger.hpp
    // for the Logger::Level enum, keeping this header self-contained.
    constexpr int kTraceLevel = 0;
    constexpr int kDebugLevel = 1;
    constexpr int kInfoLevel = 2;
    constexpr int kWarnLevel = 3;
    constexpr int kErrorLevel = 4;
    constexpr int kSystemLevel = 5;
    switch (lvl)
    {
    case kTraceLevel:
        return "TRACE";
    case kDebugLevel:
        return "DEBUG";
    case kInfoLevel:
        return "INFO";
    case kWarnLevel:
        return "WARN";
    case kErrorLevel:
        return "ERROR";
    case kSystemLevel:
        return "SYSTEM";
    default:
        return "UNK";
    }
}

// Formats a LogMessage into a standardized string format.
std::string Sink::format_logmsg(const LogMessage &msg, Sink::WRITE_MODE mode)
{
    std::string time_str = formatted_time(msg.timestamp);
    if (mode == Sink::ASYNC_WRITE)
    {
        return fmt::format("[LOGGER] [{:<6}] [{}] [PID:{:5} TID:{:5}] {}\n",
                           level_to_string_internal(msg.level), time_str, msg.process_id,
                           msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
    }
    return fmt::format("[LOGGER_SYNC] [{:<6}] [{}] [PID:{:5} TID:{:5}] {}\n",
                       level_to_string_internal(msg.level), time_str, msg.process_id,
                       msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
}

} // namespace pylabhub::utils