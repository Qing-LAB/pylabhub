#include "test_process_utils.h"

#include <fmt/core.h>
#include <vector>
#include <string>

#if defined(PLATFORM_WIN64)
// Platform-specific implementation is already included in the header
#else
#include <unistd.h> // for fork, execv
#endif

namespace test_utils
{
    #if defined(PLATFORM_WIN64)
    ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                       const std::vector<std::string> &args)
    {
        // Build a quoted commandline: "<exe>" <mode> "arg1" "arg2" ...
        std::string cmdline = fmt::format("\"{}\" {}", exe_path, mode);
        for (const auto &arg : args)
        {
            cmdline += fmt::format(" \"{}\"", arg);
        }

        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        int wide = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, nullptr, 0);
        std::wstring wcmd(wide, 0);
        MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(), -1, &wcmd[0], wide);

        if (!CreateProcessW(nullptr, &wcmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    #else
    ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                       const std::vector<std::string> &args)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            // Child process: execv expects char* const*
            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(exe_path.c_str()));
            argv.push_back(const_cast<char *>(mode.c_str()));
            for (const auto &arg : args)
            {
                argv.push_back(const_cast<char *>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execv(exe_path.c_str(), argv.data());
            _exit(127); // execv failed
        }
        return pid;
    }
    #endif

} // namespace test_utils
