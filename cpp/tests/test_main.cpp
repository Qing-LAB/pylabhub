#include "test_main.h"
#include "workers.h"

#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <filesystem>
#include <cstring>

// Define the global for the executable path
std::string g_self_exe_path;

// This is required by the logger's multi-process test.
// It is defined in test_logger.cpp and used there by the test fixture,
// but the worker needs to have it set.
namespace fs = std::filesystem;
extern fs::path g_multiproc_log_path;

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
        
        // --- Dispatch to jsonconfig worker ---
        if (mode == "worker") {
            return (argc < 4) ? 2 : jsonconfig_worker_main(argv[2], argv[3]);
        }

        // --- Dispatch to logger worker ---
        if (strcmp(argv[1], "--multiproc-child") == 0) {
            if (argc < 4) return 3;
            g_multiproc_log_path = argv[2]; // Set the path for the worker
            int count = std::stoi(argv[3]);
            multiproc_child_main(count);
            return 0;
        }
    }

    // If not in worker mode, run the tests
    g_self_exe_path = argv[0];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
