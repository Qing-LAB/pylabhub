#include "test_preamble.h" // New common preamble

#include "test_process_utils.h" // Keep this specific header

#include <fmt/core.h> // Keep this for fmt::print

#if defined(PLATFORM_WIN64)
// Platform-specific implementation is already included in the header

// Helper to convert std::string to std::wstring
static std::wstring s2ws(const std::string& s)
{
    int len;
    int slength = (int)s.length() + 1;
    len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
    std::wstring r(len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, &r[0], len);
    return r;
}

// Helper to convert std::wstring to std::string
// This is added because the GetLastError logging uses it, and it was in the code I just tried to replace.
static std::string ws2s(const std::wstring& s)
{
    int len;
    int slength = (int)s.length() + 1;
    len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, 0, 0, 0, 0);
    std::string r(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, &r[0], len, 0, 0);
    return r;
}

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

        // Convert cmdline to wstring
        std::wstring wcmd = s2ws(cmdline);

        // Get the directory of the executable to use as current directory for the child.
        std::filesystem::path path_exe_path(exe_path);
        std::wstring w_exe_dir = s2ws(path_exe_path.parent_path().string());

        BOOL create_result = CreateProcessW(nullptr,   // No module name (use command line)
                                            &wcmd[0],  // Command line
                                            nullptr,   // Process handle not inheritable
                                            nullptr,   // Thread handle not inheritable
                                            FALSE,     // Set handle inheritance to FALSE
                                            0,         // No creation flags
                                            nullptr,   // Use parent's environment block
                                            w_exe_dir.c_str(), // Set starting directory for the child process
                                            &si,       // STARTUPINFO structure
                                            &pi);      // PROCESS_INFORMATION structure

        if (!create_result)
        {
            DWORD error = GetLastError();
            LPWSTR messageBuffer = nullptr;
            size_t size =
                FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&messageBuffer, 0, NULL);
            // Convert wide string message to narrow string for fmt::print
            std::string errorMessage = ws2s(messageBuffer);
            LocalFree(messageBuffer); // Free the buffer allocated by FormatMessageW

            fmt::print(stderr, "ERROR: CreateProcessW failed. Code: {} - Message: {}\n", error, errorMessage);
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
