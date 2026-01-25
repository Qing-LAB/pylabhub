#include "plh_base.hpp"
#include "utils/logger_sinks/Sink.hpp"

namespace pylabhub::utils
{

// Returns a string representation for a given log level.
const char *Sink::level_to_string_internal(int lvl)
{
    // A C-style switch on int is used here to avoid a hard dependency on Logger.hpp
    // for the Logger::Level enum, keeping this header self-contained.
    switch (lvl)
    {
    case 0:
        return "TRACE";
    case 1:
        return "DEBUG";
    case 2:
        return "INFO";
    case 3:
        return "WARN";
    case 4:
        return "ERROR";
    case 5:
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
    else
    {
        return fmt::format("[LOGGER_SYNC] [{:<6}] [{}] [PID:{:5} TID:{:5}] {}\n",
                           level_to_string_internal(msg.level), time_str, msg.process_id,
                           msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
    }
}

} // namespace pylabhub::utils