/**
 * @file debug_info.cpp
 * @brief Implements cross-platform debugging utilities for stack trace printing.
 *
 * This file provides the concrete implementation of `pylabhub::debug::print_stack_trace()`.
 * It includes platform-specific code for Windows (using DbgHelp API) and POSIX
 * systems (using `execinfo.h` and `dlfcn.h` for backtrace and symbol resolution,
 * and `addr2line` for source location lookup).
 *
 * This module is part of the `pylabhub-basic` static library and offers fundamental
 * debugging capabilities crucial for error diagnosis.
 */

#include "platform.hpp"
#include <fmt/core.h>

#if defined(PYLABHUB_PLATFORM_WIN64)

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 5105) // macro expansion producing 'defined' has undefined behavior
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <dbghelp.h>
#include <memory>
#include <string>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
#elif defined(PYLABHUB_IS_POSIX)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>   // __cxa_demangle
#include <dlfcn.h>    // dladdr
#include <execinfo.h> // backtrace, backtrace_symbols
#include <limits.h>   // PATH_MAX
#include <map>
#include <sstream>
#include <string>
#include <unistd.h> // readlink, access
#include <unordered_map>
#include <utility>
#include <vector>
#endif

#include "debug_info.hpp"

namespace pylabhub::debug
{

#if defined(PYLABHUB_PLATFORM_WIN64)
namespace
{ // Anonymous namespace for internal linkage

class DbgHelpInitializer
{
  public:
    DbgHelpInitializer()
    {
        // Initialize DbgHelp for this process.
        // This is done once when the library is loaded.
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        if (!SymInitialize(process, nullptr, TRUE))
        {
            // If initialization fails, we can't get stack traces, but we shouldn't
            // prevent the program from running. A message will be printed inside
            // print_stack_trace when symbol functions fail.
        }
    }

    ~DbgHelpInitializer()
    {
        // Cleanup DbgHelp for this process.
        SymCleanup(GetCurrentProcess());
    }
};

// A static instance to ensure DbgHelp is initialized once per process lifetime.
static DbgHelpInitializer g_dbghelp_initializer;

} // namespace
#endif

// Helper: find an executable in PATH (returns true if found and executable).
static bool find_in_path(const std::string &name)
{
    const char *pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return false;
    std::string pathStr(pathEnv);
    size_t start = 0;
    while (start < pathStr.size())
    {
        size_t pos = pathStr.find(':', start);
        std::string dir;
        if (pos == std::string::npos)
        {
            dir = pathStr.substr(start);
            start = pathStr.size();
        }
        else
        {
            dir = pathStr.substr(start, pos - start);
            start = pos + 1;
        }
        if (dir.empty())
            dir = ".";
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

// Quote a path for shell (basic)
static std::string shell_quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}

// New helper function to encapsulate addr2line logic
static std::vector<std::string>
resolve_symbols_with_addr2line(const std::string &binary, const std::vector<uintptr_t> &offsets,
                               bool prefer_llvm_addr2line)
{
    std::ostringstream cmd;
    if (prefer_llvm_addr2line && find_in_path("llvm-addr2line"))
        cmd << "llvm-addr2line";
    else if (find_in_path("addr2line"))
        cmd << "addr2line";
    else
        return std::vector<std::string>(); // No addr2line tool found

    cmd << " -f -C -e " << shell_quote(binary);

    for (uintptr_t offset : offsets)
    {
        cmd << " 0x" << std::hex << offset;
    }

    FILE *fp = popen(cmd.str().c_str(), "r");
    if (!fp)
    {
        return std::vector<std::string>();
    }

    std::vector<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp))
    {
        std::string s(buf);
        // trim newline
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        lines.push_back(std::move(s));
    }
    pclose(fp);

    std::vector<std::string> perFrame;
    perFrame.reserve(offsets.size());

    // Heuristic for addr2line output: 2 lines per address (function, file:line) or 1 line.
    if (!lines.empty() && lines.size() >= offsets.size() * 2)
    {
        for (size_t j = 0; j + 1 < lines.size() && perFrame.size() < offsets.size(); j += 2)
        {
            perFrame.push_back(lines[j] + "\n" + lines[j + 1]);
        }
    }
    else if (!lines.empty() && lines.size() >= offsets.size())
    {
        // Fallback for one line per frame
        for (size_t j = 0; j < offsets.size(); ++j)
        {
            perFrame.push_back(lines[j]);
        }
    }
    else
    {
        // General fallback heuristic: distribute lines evenly
        size_t per = (lines.empty() ? 1 : (lines.size() + offsets.size() - 1) / offsets.size());
        size_t cur = 0;
        for (size_t fi = 0; fi < offsets.size(); ++fi)
        {
            std::ostringstream acc;
            for (size_t k = 0; k < per && cur < lines.size(); ++k, ++cur)
            {
                if (k)
                    acc << "\n";
                acc << lines[cur];
            }
            perFrame.push_back(acc.str());
        }
    }

    // Pad if necessary
    while (perFrame.size() < offsets.size())
        perFrame.emplace_back();

    return perFrame;
}

void print_stack_trace() noexcept
{
    fmt::print(stderr, "Stack Trace (most recent call first):\n");

#if defined(PYLABHUB_PLATFORM_WIN64)
    /**
     * @brief Windows-specific implementation of `print_stack_trace`.
     *
     * This section uses the Windows DbgHelp API to capture and resolve stack frames.
     * It relies on `CaptureStackBackTrace` for frame capture, `SymInitialize` to
     * initialize symbol handling, `SymFromAddr` to get symbol names, and
     * `SymGetLineFromAddr64` to retrieve source file and line numbers.
     * PDB files are required for detailed source information.
     */
    constexpr int kMaxFrames = 62;
    void *frames[kMaxFrames] = {nullptr};
    USHORT framesCaptured = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);

    HANDLE process = GetCurrentProcess();

    // Allocate SYMBOL_INFO buffer with room for long name
    constexpr size_t kNameBuf = 1024;
    const size_t symbolBufferSize = sizeof(SYMBOL_INFO) + kNameBuf;
    std::unique_ptr<uint8_t[]> symbolArea(new (std::nothrow) uint8_t[symbolBufferSize]);
    SYMBOL_INFO *symbol = nullptr;
    if (symbolArea)
    {
        symbol = reinterpret_cast<SYMBOL_INFO *>(symbolArea.get());
        std::memset(symbol, 0, sizeof(SYMBOL_INFO));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = static_cast<ULONG>(kNameBuf - 1);
    }

    IMAGEHLP_LINE64 lineInfo;
    std::memset(&lineInfo, 0, sizeof(lineInfo));
    lineInfo.SizeOfStruct = sizeof(lineInfo);

    for (USHORT i = 0; i < framesCaptured; ++i)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        fmt::print(stderr, "  #{:02}  {:#018x}  ", i, static_cast<unsigned long long>(addr));

        bool printed = false;

        // Try symbol name
        DWORD64 displacement = 0;
        if (symbol && SymFromAddr(process, static_cast<DWORD64>(addr), &displacement, symbol))
        {
            // Name is undecorated if SYMOPT_UNDNAME set
            fmt::print(stderr, "{} + {:#x}", symbol->Name,
                       static_cast<unsigned long long>(displacement));
            printed = true;
        }

        // Try source file + line (requires pdbs and SYMOPT_LOAD_LINES)
        DWORD displacementLine = 0;
        if (SymGetLineFromAddr64(process, static_cast<DWORD64>(addr), &displacementLine, &lineInfo))
        {
            const char *fname = lineInfo.FileName ? lineInfo.FileName : "(unknown)";
            fmt::print(stderr, " -- {}:{}", fname, lineInfo.LineNumber);
            printed = true;
        }

        // If nothing printed, try to show module + offset as fallback
        if (!printed)
        {
            IMAGEHLP_MODULE64 modInfo;
            std::memset(&modInfo, 0, sizeof(modInfo));
            modInfo.SizeOfStruct = sizeof(modInfo);
            if (SymGetModuleInfo64(process, static_cast<DWORD64>(addr), &modInfo))
            {
                const char *img = modInfo.ImageName         ? modInfo.ImageName
                                  : modInfo.LoadedImageName ? modInfo.LoadedImageName
                                                            : "(unknown)";
                uintptr_t base = static_cast<uintptr_t>(modInfo.BaseOfImage);
                fmt::print(stderr, "(module: {}) + {:#x}", img,
                           static_cast<unsigned long long>(addr - base));
            }
            else
            {
                fmt::print(stderr, "[symbol unknown]");
            }
        }

        fmt::print(stderr, "\n");
    }

#elif defined(PYLABHUB_IS_POSIX)

    /**
     * @brief POSIX-specific implementation of `print_stack_trace`.
     *
     * This section uses `backtrace` to capture stack addresses and attempts to resolve
     * symbol names and source file/line information using `dladdr` and `addr2line`.
     * `dladdr` provides information about the shared object and function symbol,
     * while `addr2line` (a separate utility executed via `popen`) is used to get
     * file and line numbers, which are often more accurate for compiled binaries.
     */
    constexpr int kMaxFrames = 200;
    void *callstack[kMaxFrames];
    int frames = backtrace(callstack, kMaxFrames);
    if (frames <= 0)
    {
        fmt::print(stderr, "  [No stack frames available]\n");
        return;
    }

    // fallback textual names
    char **symbols = backtrace_symbols(callstack, frames);

    const std::string exe_path = pylabhub::platform::get_executable_name(true);

    // Collect frame meta (dladdr info, computed offsets)
    struct FrameMeta
    {
        int idx;
        uintptr_t addr;
        uintptr_t offset;      // offset within module or absolute
        void *saddr;           // symbol address returned by dladdr
        std::string demangled; // demangled symbol name if available
        std::string dli_fname; // object filename from dladdr
        uintptr_t dli_fbase; // module base (if dladdr provided it) - added to support macOS/atos -l
    };

    std::vector<FrameMeta> metas;
    metas.reserve(frames);

    for (int i = 0; i < frames; ++i)
    {
        FrameMeta m;
        m.idx = i;
        m.addr = reinterpret_cast<uintptr_t>(callstack[i]);
        m.offset = m.addr; // default; may be changed
        m.saddr = nullptr;
        m.demangled.clear();
        m.dli_fname.clear();
        m.dli_fbase = 0;

        Dl_info dlinfo;
        /**
         * @brief Use `dladdr` to get information about the address.
         *
         * `dladdr` attempts to resolve the given address to the nearest symbol
         * and the shared object it belongs to. This provides the symbol name (`dli_sname`),
         * its address (`dli_saddr`), and the containing shared object's filename (`dli_fname`)
         * and base address (`dli_fbase`).
         */
        if (dladdr(callstack[i], &dlinfo))
        {
            if (dlinfo.dli_sname)
            {
                int status = 0;
                /**
                 * @brief Demangle C++ symbol names.
                 *
                 * C++ compilers often "mangle" function names to encode type information.
                 * `abi::__cxa_demangle` attempts to convert these mangled names back to
                 * human-readable form.
                 */
                char *dem = abi::__cxa_demangle(dlinfo.dli_sname, nullptr, nullptr, &status);
                if (status == 0 && dem)
                {
                    m.demangled = dem;
                    std::free(dem);
                }
                else
                {
                    m.demangled = dlinfo.dli_sname;
                }
            }
            m.saddr = dlinfo.dli_saddr;
            if (dlinfo.dli_fname)
            {
                m.dli_fname = dlinfo.dli_fname;
                // compute offset relative to module base if we can
                uintptr_t base = reinterpret_cast<uintptr_t>(dlinfo.dli_fbase);
                m.dli_fbase = base;
                if (base != 0)
                    m.offset = m.addr - base;
                else
                    m.offset = m.addr;
            }
            else
            {
                // no dli_fname -> leave offset as absolute (fallback)
                m.offset = m.addr;
                m.dli_fbase = 0;
            }
        }
        metas.push_back(std::move(m));
    }

    // Group frames by binary (prefer dli_fname, fallback to exe_path, else "[unknown]")
    std::map<std::string, std::vector<int>> binToFrameIndices;
    for (size_t i = 0; i < metas.size(); ++i)
    {
        const std::string &fname = metas[i].dli_fname.empty() ? exe_path : metas[i].dli_fname;
        std::string key = fname.empty() ? std::string("[unknown]") : fname;
        binToFrameIndices[key].push_back(static_cast<int>(i));
    }

    // For each binary, call addr2line (or atos on macOS) once with all offsets
    std::unordered_map<std::string, std::vector<std::string>> binResults;
    binResults.reserve(binToFrameIndices.size());

    for (auto &kv : binToFrameIndices)
    {
        const std::string &binary = kv.first;
        const std::vector<int> &indices = kv.second;

        if (binary == "[unknown]" || binary.empty())
        {
            binResults[binary] = std::vector<std::string>(indices.size(), std::string());
            continue;
        }

#if defined(__APPLE__)
        // macOS: prefer atos (works with dSYM). Build command:
        // atos -o "<binary>" [-l 0x<base>] 0x<addr1> 0x<addr2> ...
        // We'll include -l when dlinfo provides a non-zero base (ASLR slide).
        bool has_atos = find_in_path("atos");
        bool has_llvm_addr2line = find_in_path("llvm-addr2line");
        bool has_addr2line = find_in_path("addr2line");

        if (!has_atos && !has_llvm_addr2line && !has_addr2line)
        {
            // no symbolizer found -- store empty results
            binResults[binary] = std::vector<std::string>(indices.size(), std::string());
            continue;
        }

        if (has_atos)
        {
            std::ostringstream cmd;
            cmd << "atos -o " << shell_quote(binary);

            // If we have a dli_fbase for this module, supply -l (load address / slide)
            // Find first non-zero base among frames for this binary (if any)
            uintptr_t base_for_this_bin = 0;
            for (int idx : indices)
            {
                if (metas[idx].dli_fbase != 0)
                {
                    base_for_this_bin = metas[idx].dli_fbase;
                    break;
                }
            }
            if (base_for_this_bin != 0)
            {
                cmd << " -l 0x" << std::hex << base_for_this_bin;
            }

            // Append absolute addresses (atos expects VM addresses)
            for (int idx : indices)
            {
                cmd << " 0x" << std::hex << metas[idx].addr;
            }

            FILE *fp = popen(cmd.str().c_str(), "r");
            if (!fp)
            {
                binResults[binary] = std::vector<std::string>(indices.size(), std::string());
                continue;
            }

            std::vector<std::string> lines;
            char buf[1024];
            while (fgets(buf, sizeof(buf), fp))
            {
                std::string s(buf);
                // trim newline
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                    s.pop_back();
                lines.push_back(std::move(s));
            }
            pclose(fp);

            std::vector<std::string> perFrame;
            perFrame.reserve(indices.size());

            // atos typically prints one line per address (symbol and file:line if dSYM available)
            if ((int)lines.size() >= (int)indices.size())
            {
                for (size_t j = 0; j < indices.size() && j < lines.size(); ++j)
                {
                    perFrame.push_back(lines[j]);
                }
                // pad if necessary
                while (perFrame.size() < indices.size())
                    perFrame.emplace_back();
            }
            else
            {
                // fallback heuristic: distribute lines evenly
                size_t per =
                    (lines.empty() ? 1 : (lines.size() + indices.size() - 1) / indices.size());
                size_t cur = 0;
                for (size_t fi = 0; fi < indices.size(); ++fi)
                {
                    std::ostringstream acc;
                    for (size_t k = 0; k < per && cur < lines.size(); ++k, ++cur)
                    {
                        if (k)
                            acc << "\n";
                        acc << lines[cur];
                    }
                    perFrame.push_back(acc.str());
                }
                while (perFrame.size() < indices.size())
                    perFrame.emplace_back();
            }

            binResults[binary] = std::move(perFrame);
        }
        else
        {
            std::vector<uintptr_t> offsets;
            offsets.reserve(indices.size());
            for (int idx : indices)
            {
                offsets.push_back(metas[idx].offset);
            }
            binResults[binary] = resolve_symbols_with_addr2line(binary, offsets, true);
        }

#else // non-Apple POSIX systems (Linux, etc.): existing addr2line logic

        std::vector<uintptr_t> offsets;
        offsets.reserve(indices.size());
        for (int idx : indices)
        {
            offsets.push_back(metas[idx].offset);
        }
        binResults[binary] = resolve_symbols_with_addr2line(binary, offsets, false);

#endif // __APPLE__ / non-Apple POSIX

    } // end for each binary

    // Print frames with the best available info
    for (size_t i = 0; i < metas.size(); ++i)
    {
        const FrameMeta &m = metas[i];
        fmt::print(stderr, "  #{:02}  {:#018x}  ", m.idx, static_cast<unsigned long long>(m.addr));

        bool printed = false;

        // Try printing demangled C++ symbol name
        if (!m.demangled.empty())
        {
            uintptr_t saddr = reinterpret_cast<uintptr_t>(m.saddr);
            // If saddr is non-null, print offset from symbol; else just show name
            if (saddr)
            {
                fmt::print(stderr, "{} + {:#x}", m.demangled,
                           static_cast<unsigned long long>(m.addr - saddr));
            }
            else
            {
                fmt::print(stderr, "{}", m.demangled);
            }
            printed = true;
        }
        else if (symbols && symbols[i])
        {
            // Fallback to original `backtrace_symbols` output if demangling failed
            fmt::print(stderr, "{}", symbols[i]);
            printed = true;
        }

        // Look up addr2line/atos result for this frame
        const std::string &binaryKey =
            m.dli_fname.empty() ? (exe_path.empty() ? std::string("[unknown]") : exe_path)
                                : m.dli_fname;
        auto binIt = binToFrameIndices.find(binaryKey);
        if (binIt != binToFrameIndices.end())
        {
            // find position of this frame in the vector of indices for this binary
            const std::vector<int> &vec = binIt->second;
            auto it = std::find(vec.begin(), vec.end(), static_cast<int>(i));
            if (it != vec.end())
            {
                size_t pos = static_cast<size_t>(std::distance(vec.begin(), it));
                auto resIt = binResults.find(binaryKey);
                if (resIt != binResults.end() && pos < resIt->second.size())
                {
                    const std::string &addr2lineText = resIt->second[pos];
                    if (!addr2lineText.empty())
                    {
                        fmt::print(stderr, "  \n         -> {}", addr2lineText);
                        printed = true;
                    }
                }
            }
        }

        // Final fallback if no detailed info was printed
        if (!printed)
        {
            if (!m.dli_fname.empty())
            {
                fmt::print(stderr, "({}) + {:#x}", m.dli_fname,
                           static_cast<unsigned long long>(m.offset));
            }
            else
            {
                fmt::print(stderr, "[unknown]");
            }
        }

        fmt::print(stderr, "\n");
    }

    if (symbols)
        std::free(symbols);

#else

    /**
     * @brief Fallback for unsupported platforms.
     *
     * If the current platform is neither Windows nor a supported POSIX system,
     * a message indicating that stack trace is unavailable is printed.
     */
    fmt::print(stderr, "  [Stack trace not available on this platform]\n");

#endif
}

} // namespace pylabhub::debug
