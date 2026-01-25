#pragma once

#include "Sink.hpp"
#include <fmt/core.h>

namespace pylabhub::utils
{

class ConsoleSink : public Sink
{
  public:
    void write(const LogMessage &msg, Sink::WRITE_MODE mode) override
    {
        fmt::print(stderr, "{}", pylabhub::utils::Sink::format_logmsg(msg, mode));
    }
    void flush() override { fflush(stderr); }
    std::string description() const override { return "Console"; }
};

} // namespace pylabhub::utils
