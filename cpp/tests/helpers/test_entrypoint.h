#pragma once
#include <string>
#include <filesystem>

// This global is set by main() and used by test functions that need to
// re-launch the test executable in a worker mode.
extern std::string g_self_exe_path;
