#pragma once
#include <string>
#include <filesystem>

// This header provides common variables for tests that need to spawn workers.
extern std::string g_self_exe_path;

// Multiprocess tests use this global to communicate the child log path.
extern std::filesystem::path g_multiproc_log_path;
