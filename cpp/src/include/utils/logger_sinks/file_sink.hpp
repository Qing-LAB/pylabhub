#pragma once

#include "base_file_sink.hpp"
#include "sink.hpp"
#include <string>

namespace pylabhub::utils
{

class FileSink : public Sink, private BaseFileSink
{
  public:
    FileSink(const std::string &path, bool use_flock);

    ~FileSink() override;

    void write(const LogMessage &msg, Sink::WRITE_MODE mode) override;

    void flush() override;

    std::string description() const override;
};

} // namespace pylabhub::utils
