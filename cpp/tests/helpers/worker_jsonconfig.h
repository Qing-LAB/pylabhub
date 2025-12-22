#pragma once
#include <string>

// Declares worker functions for JsonConfig multi-process tests.
namespace worker
{
    namespace jsonconfig
    {
        int write_id(const std::string& cfgpath, const std::string& worker_id);
    }
}
