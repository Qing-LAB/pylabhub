// tests/helpers/worker_jsonconfig.h
#pragma once
#include <string>

// Declares worker functions for JsonConfig multi-process tests.
namespace worker
{
    namespace jsonconfig
    {
        // Worker invoked as a child process by the tests.
        // Parameters:
        //   cfgpath - path to the config file
        //   worker_id - unique id for this worker (used as a key written to the config)
        int write_id(const std::string &cfgpath, const std::string &worker_id);
    }
}
