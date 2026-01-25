#include "utils/logger_sinks/FileSink.hpp"
#include "utils/logger_sinks/Sink.hpp"
#include <stdexcept>
#include <fmt/format.h>

namespace pylabhub::utils
{

FileSink::FileSink(const std::string &path, bool use_flock)
{
    try
    {
        open(path, use_flock);
    }
    catch (const std::system_error &e)
    {
        throw std::runtime_error(
            fmt::format("Failed to open log file '{}': {}", path, e.what()));
    }
}

FileSink::~FileSink() = default;

void FileSink::write(const LogMessage &msg, Sink::WRITE_MODE mode)
{
    auto strmsg = format_logmsg(msg, mode);
    BaseFileSink::fwrite(strmsg);
}

void FileSink::flush()
{
    BaseFileSink::fflush();
}

std::string FileSink::description() const
{
    return "File: " + path().string();
}

} // namespace pylabhub::utils
