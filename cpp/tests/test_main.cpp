#include "test_main.h"
#include "workers.h"

#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cstdlib> // for std::stoi

// Define the global for the executable path
std::string g_self_exe_path;

// g_multiproc_log_path is declared in test_main.h (defined in test_logger_gtest.cpp)
namespace fs = std::filesystem;

int main(int argc, char **argv) {
    // Handle worker process modes first
    if (argc > 1) {
        std::string mode = argv[1];

        // --- Dispatch to filelock workers ---
        if (mode == "nonblocking_worker") {
            return (argc < 3) ? 2 : worker_main_nonblocking_test(argv[2]);
        }
        if (mode == "blocking_worker") {
            return (argc < 4) ? 2 : worker_main_blocking_contention(argv[2], std::stoi(argv[3]));
        }
        if (mode == "parent_child_worker") {
            return (argc < 3) ? 2 : worker_main_parent_child(argv[2]);
        }
        
        // --- Dispatch to jsonconfig worker ---
        if (mode == "worker") {
            return (argc < 4) ? 2 : jsonconfig_worker_main(argv[2], argv[3]);
        }

        // --- Dispatch to logger worker ---
        if (strcmp(argv[1], "--multiproc-child") == 0) {
            if (argc < 4) return 3;
            // Set the path for the worker (global declared in test_main.h)
            g_multiproc_log_path = fs::path(argv[2]);
            int count = 0;
            try {
                count = std::stoi(argv[3]);
            } catch (...) {
                count = 200; // fallback default
            }
            multiproc_child_main(count);
            return 0;
        }
    }

    // If not in worker mode, run the tests
    if (argc >= 1) g_self_exe_path = argv[0];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
