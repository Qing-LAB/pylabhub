#pragma once

// Core Google Test include
#include <gtest/gtest.h>

// Standard Library Includes (common across many tests)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib> // For std::rand, std::getenv in some helpers
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Platform-specific headers for process management, etc.
#include "platform.hpp"
#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h> // For waitpid
#include <sys/types.h> // For pid_t
#include <fcntl.h> // For open() and O_* flags
#include <unistd.h> // For write(), getpid()
#include <sys/stat.h> // For stat, etc.
#endif

// Project-specific common includes and using declarations
#include "atomic_guard.hpp"
#include "recursion_guard.hpp"
#include "scope_guard.hpp"
#include "format_tools.hpp" // If used by any test logic directly

#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

// Common using declarations
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Project namespaces used frequently in tests
using namespace pylabhub::basics;
using namespace pylabhub::utils;
// using namespace test_utils; // Removed to avoid forward declaration issues