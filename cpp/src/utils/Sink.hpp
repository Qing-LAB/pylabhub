#pragma once

#include "format_tools.hpp"
#include "platform.hpp"
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::utils
{

using namespace pylabhub::format_tools;

// Forward declare to avoid circular dependencies
class Logger;

// Represents a single log message event.
struct LogMessage
{
    std::chrono::system_clock::time_point timestamp;
    uint64_t process_id;
    uint64_t thread_id;
    int level; // Use int to avoid including all of Logger.hpp for the enum.
    fmt::memory_buffer body;
};

// Abstract interface for a log message destination.
class Sink
{
  public:
    enum WRITE_MODE{
        ASYNC_WRITE,
        SYNC_WRITE
    };

    virtual ~Sink() = default;
    virtual void write(const LogMessage &msg, Sink::WRITE_MODE mode) = 0;
    virtual void flush() = 0;
    virtual std::string description() const = 0;
};

// Returns a string representation for a given log level.
static const char *level_to_string_internal(int lvl)
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
static std::string format_logmsg(const LogMessage &msg, Sink::WRITE_MODE mode)
{
    std::string time_str = formatted_time(msg.timestamp);
    if (mode == Sink::ASYNC_WRITE) {
        return fmt::format("[LOGGER] [{:<6}] [{}] [PID:{:5} TID:{:5}] {}\n", level_to_string_internal(msg.level), time_str,
                       msg.process_id, msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
    }
    else {
        return fmt::format("[LOGGER_SYNC] [{:<6}] [{}] [PID:{:5} TID:{:5}] {}\n", level_to_string_internal(msg.level), time_str,
                       msg.process_id, msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
    }
}

} // namespace pylabhub::utils
