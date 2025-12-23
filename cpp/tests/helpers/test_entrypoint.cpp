#include "platform.hpp"
#include "test_entrypoint.h"
#include "workers.h"
#include "utils/Lifecycle.hpp"

#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cstdlib> // for std::stoi

// Define the global for the executable path
std::string g_self_exe_path;

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    // Handle worker process modes first
    if (argc > 1) {
        std::string mode_str = argv[1];
        size_t dot_pos = mode_str.find('.');
        if (dot_pos != std::string::npos) {
            std::string module = mode_str.substr(0, dot_pos);
            std::string scenario = mode_str.substr(dot_pos + 1);

            if (module == "filelock") {
                if (scenario == "nonblocking_acquire" && argc > 2) {
                    return worker::filelock::nonblocking_acquire(argv[2]);
                }
                if (scenario == "contention_increment" && argc > 3) {
                    return worker::filelock::contention_increment(argv[2], std::stoi(argv[3]));
                }
                if (scenario == "parent_child_block" && argc > 2) {
                    return worker::filelock::parent_child_block(argv[2]);
                }
            } else if (module == "jsonconfig") {
                if (scenario == "write_id" && argc > 3) {
                    return worker::jsonconfig::write_id(argv[2], argv[3]);
                }
            } else if (module == "logger") {
                if (scenario == "stress_log" && argc > 3) {
                    worker::logger::stress_log(argv[2], std::stoi(argv[3]));
                    return 0;
                }
            }
        }
        // If mode not recognized, fall through to running tests, which will likely fail
        // in a way that indicates an incorrect worker mode was passed.
    }

    // If not in worker mode, or if worker dispatch fails, run the tests.
    if (argc >= 1) g_self_exe_path = argv[0];
    pylabhub::utils::Initialize();
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    pylabhub::utils::Finalize();
    return result;
}
