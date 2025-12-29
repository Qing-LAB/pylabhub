#pragma once

#include <gtest/gtest.h>
#include <fmt/core.h>
#include <chrono>
#include <filesystem>
#include <string>

#include "utils/Lifecycle.hpp"
#include "scope_guard.hpp"

namespace fs = std::filesystem;

// Helper functions for tests, extracted from the anonymous namespace in workers.cpp.

bool read_file_contents(const std::string &path, std::string &out);

size_t count_lines(const std::string &s);

bool wait_for_string_in_file(const fs::path &path, const std::string &expected,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(15));

std::string test_scale();

int scaled_value(int original, int small_value);

template <typename Fn>
int run_gtest_worker(Fn test_logic, const char *test_name)
{
    pylabhub::utils::InitializeApplication();
    auto finalizer =
        pylabhub::basics::make_scope_guard([] { pylabhub::utils::FinalizeApplication(); });

    try
    {
        test_logic();
    }
    catch (const ::testing::AssertionException &e)
    {
        fmt::print(stderr, "[WORKER FAILURE] GTest assertion failed in {}: \n", test_name,
                   e.what());
        return 1;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "[WORKER FAILURE] {} threw an exception: {}\n", test_name, e.what());
        return 2;
    }
    catch (...)
    {
        fmt::print(stderr, "[WORKER FAILURE] {} threw an unknown exception.\n", test_name);
        return 3;
    }
    return 0; // Success
}
