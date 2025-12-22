#pragma once
#include <string>

// Declares worker functions for Logger multi-process tests.
namespace worker
{
    namespace logger
    {
        void stress_log(const std::string& log_path, int msg_count);
    }
}
