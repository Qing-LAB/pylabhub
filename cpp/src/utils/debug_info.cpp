/**
 * @file debug_info.cpp
 * @brief Cross-platform stack trace printing for pylabhub::debug::print_stack_trace()
 *
 * Macro assumptions:
 * - PYLABHUB_PLATFORM_WIN64 : defined when building for Windows x64
 * - PYLABHUB_IS_POSIX       : defined as 1 (true) or 0 (false) for POSIX-like platforms
 * - PYLABHUB_PLATFORM_APPLE : defined when building for macOS (in addition to PYLABHUB_IS_POSIX==1)
 *
 * The implementation aims to maximize code reuse between macOS and other POSIX systems.
 */

#include "plh_base.hpp"

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

#elif defined(PYLABHUB_IS_POSIX) && (PYLABHUB_IS_POSIX)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>   // __cxa_demangle
#include <dlfcn.h>    // dladdr
#include <execinfo.h> // backtrace, backtrace_symbols
#include <limits.h>   // PATH_MAX
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h> // access
#include <unordered_map>
#include <utility>
#include <vector>

#else

// Fallback platform: still include minimal headers that library/core code expects.
#include <string>
#include <vector>

#endif // platform selection

#include "utils/debug_info.hpp"

namespace pylabhub::debug
{

#if defined(PYLABHUB_PLATFORM_WIN64)
namespace
{
class DbgHelpInitializer
{
  public:
    DbgHelpInitializer()
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        // Best-effort initialize; failures tolerated.
        (void)SymInitialize(process, nullptr, TRUE);
    }

    ~DbgHelpInitializer() { SymCleanup(GetCurrentProcess()); }
};

static DbgHelpInitializer g_dbghelp_initializer;

} // namespace
#endif

// -------------------------------
// POSIX helpers (when PYLABHUB_IS_POSIX == 1)
// -------------------------------
#if defined(PYLABHUB_IS_POSIX) && (PYLABHUB_IS_POSIX)

namespace internal
{

[[nodiscard]] static bool find_in_path(std::string_view name) noexcept
{
    const char *pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return false;
    std::string_view path(pathEnv);
    size_t start = 0;
    while (start < path.size())
    {
        size_t pos = path.find(':', start);
        std::string_view dir =
            (pos == std::string_view::npos) ? path.substr(start) : path.substr(start, pos - start);
        start = (pos == std::string_view::npos) ? path.size() : pos + 1;
        if (dir.empty())
            dir = ".";
        std::string candidate;
        candidate.reserve(dir.size() + 1 + name.size());
        candidate.append(dir);
        candidate.push_back('/');
        candidate.append(name);
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] static std::string shell_quote(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''"; // close, escape, reopen
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

[[nodiscard]] static std::vector<std::string> read_popen_lines(const std::string &cmd)
{
    std::vector<std::string> lines;
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp)
        return lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp))
    {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        lines.push_back(std::move(s));
    }
    pclose(fp);
    return lines;
}

// Helper to distribute output lines from a symbolizer tool among the expected frames.
// This is a fallback for when the tool's output format is not as expected.
[[nodiscard]] static std::vector<std::string>
distribute_popen_lines(const std::vector<std::string> &lines, size_t expected_count)
{
    std::vector<std::string> perFrame;
    if (expected_count == 0)
    {
        return perFrame;
    }
    perFrame.reserve(expected_count);

    // Ideal case: at least one line per frame
    if (!lines.empty() && lines.size() >= expected_count)
    {
        for (size_t j = 0; j < expected_count; ++j)
        {
            perFrame.push_back(lines[j]);
        }
    }
    // Fallback: distribute the lines we have as evenly as possible
    else
    {
        size_t per = (lines.empty() ? 1 : (lines.size() + expected_count - 1) / expected_count);
        size_t cur = 0;
        for (size_t fi = 0; fi < expected_count; ++fi)
        {
            std::ostringstream acc;
            for (size_t k = 0; k < per && cur < lines.size(); ++k, ++cur)
            {
                if (k > 0)
                    acc << " ";
                acc << lines[cur];
            }
            perFrame.push_back(acc.str());
        }
    }
    // Ensure the output vector has the expected size
    while (perFrame.size() < expected_count)
    {
        perFrame.emplace_back();
    }
    return perFrame;
}

// Generic addr2line resolver (handles addr2line and llvm-addr2line)
[[nodiscard]] static std::vector<std::string>
resolve_with_addr2line(std::string_view binary, std::span<const uintptr_t> offsets,
                       bool prefer_llvm) noexcept
{
    // pick tool
    std::string cmd;
    if (prefer_llvm && find_in_path("llvm-addr2line"))
        cmd = "llvm-addr2line";
    else if (find_in_path("addr2line"))
        cmd = "addr2line";
    else
        return {};

    cmd += " -f -C -e ";
    cmd += shell_quote(binary);

    std::ostringstream oss;
    oss << cmd;
    for (uintptr_t off : offsets)
        oss << " 0x" << std::hex << off;

    auto lines = read_popen_lines(oss.str());

    // addr2line with -f usually emits 2 lines per address (function, file:line).
    if (!lines.empty() && lines.size() >= offsets.size() * 2)
    {
        std::vector<std::string> perFrame;
        perFrame.reserve(offsets.size());
        for (size_t j = 0; j + 1 < lines.size() && perFrame.size() < offsets.size(); j += 2)
            perFrame.push_back(lines[j] + " at " + lines[j + 1]);
        while (perFrame.size() < offsets.size())
        {
            perFrame.emplace_back();
        }
        return perFrame;
    }

    // Fallback to simple distribution if output format is not 2 lines per frame
    return distribute_popen_lines(lines, offsets.size());
}

#if defined(PYLABHUB_PLATFORM_APPLE)
// macOS atos resolver: expects VM addresses and optional -l slide
[[nodiscard]] static std::vector<std::string> resolve_with_atos(std::string_view binary,
                                                                std::span<const uintptr_t> addrs,
                                                                uintptr_t base_for_bin) noexcept
{
    std::ostringstream oss;
    oss << "atos -o " << shell_quote(binary);
    if (base_for_bin != 0)
        oss << " -l 0x" << std::hex << base_for_bin;
    for (uintptr_t a : addrs)
        oss << " 0x" << std::hex << a;

    auto lines = read_popen_lines(oss.str());
    return distribute_popen_lines(lines, addrs.size());
}
#endif // PYLABHUB_PLATFORM_APPLE

} // namespace internal

#endif // PYLABHUB_IS_POSIX

namespace // anonymous namespace
{
// Try to format into a fixed stack buffer first. If the output was truncated,
// attempt one heap-format fallback. Returns true on success, false on formatting error.
template <typename... Args>
inline bool safe_format_to_stderr(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    try
    {
        // Tunable stack buffer size: choose a value large enough for common lines.
        constexpr std::size_t STACK_BUF_SZ = 2048;
        char stack_buf[STACK_BUF_SZ];

        // format_to_n writes up to STACK_BUF_SZ bytes and does not allocate.
        // It may still throw fmt::format_error on an invalid format specification.
        auto result =
            fmt::format_to_n(stack_buf, STACK_BUF_SZ, fmt_str, std::forward<Args>(args)...);

        const std::size_t needed = static_cast<std::size_t>(result.size); // total required
        const std::size_t have = needed < STACK_BUF_SZ ? needed : STACK_BUF_SZ;

        if (have > 0)
        {
            std::fwrite(stack_buf, 1, have, stderr);
        }
        return true; // success
    }
    catch (const fmt::format_error &)
    {
        // invalid format spec -> caller bail
        return false;
    }
    catch (const std::bad_alloc &)
    {
        // unexpected allocation during formatting (very unlikely here) -> bail
        return false;
    }
    catch (...)
    {
        // any other unexpected failure -> bail conservative
        return false;
    }
}

} // namespace

// -------------------------------
// Public API: print_stack_trace
// -------------------------------
/**
 * @brief Prints the current call stack (stack trace) to `stderr`.
 *
 * This function provides a platform-specific implementation to capture and print
 * the program's call stack. On Windows, it uses `CaptureStackBackTrace` and `DbgHelp`
 * functions to resolve symbols and line numbers. On POSIX systems, it uses
 * `backtrace`, `backtrace_symbols`, `dladdr`, and `addr2line` (if available)
 * to provide detailed stack information.
 *
 * Errors during stack trace capture or symbol resolution are reported to `stderr`.
 *
 * @warning This function is NOT async-signal-safe. It uses functions like `fmt::print`,
 *          `popen`, memory allocation (`new`, `std::string`), and `dladdr`/`abi::__cxa_demangle`
 *          which are not guaranteed to be safe to call from within a signal handler.
 *          Calling it from a signal handler may lead to deadlocks or other undefined behavior.
 * @warning This function is NOT thread-safe across multiple concurrent calls, especially
 *          on POSIX systems where `popen` and `pclose` might not be thread-safe if shared
 *          resources or file descriptors interact. Prefer calling it from a single thread
 *          or from crash handlers that are aware of these limitations.
 */
void print_stack_trace(bool use_external_tools) noexcept
{
    try
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        (void)use_external_tools; // This flag has no effect on Windows
        safe_format_to_stderr("Stack Trace (most recent call first):\n");

        // Windows implementation: unchanged behavior, kept concise
        constexpr int kMaxFrames = 62;
        void *frames[kMaxFrames] = {nullptr};
        USHORT framesCaptured = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
        HANDLE process = GetCurrentProcess();

        constexpr size_t kNameBuf = 1024;
        const size_t symbolBufferSize =
            sizeof(SYMBOL_INFO) + (kNameBuf - 1) * sizeof((SYMBOL_INFO *)0)->Name[0]);
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
            safe_format_to_stderr("  #{:02}  {:#018x}  ", i, static_cast<unsigned long long>(addr));
            bool printed = false;

            DWORD64 displacement = 0;
            if (symbol && SymFromAddr(process, static_cast<DWORD64>(addr), &displacement, symbol))
            {
                safe_format_to_stderr("{} + {:#x}", symbol->Name,
                                      static_cast<unsigned long long>(displacement));
                printed = true;
            }

            DWORD displacementLine = 0;
            if (SymGetLineFromAddr64(process, static_cast<DWORD64>(addr), &displacementLine,
                                     &lineInfo))
            {
                const char *fname = lineInfo.FileName ? lineInfo.FileName : "(unknown)";
                safe_format_to_stderr(" -- {}:{}", fname, lineInfo.LineNumber);
                printed = true;
            }

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
                    safe_format_to_stderr("(module: {}) + {:#x}", img,
                                          static_cast<unsigned long long>(addr - base));
                }
                else
                {
                    safe_format_to_stderr("[symbol unknown]");
                }
            }
            safe_format_to_stderr("\n");
        }

#elif defined(PYLABHUB_IS_POSIX) && (PYLABHUB_IS_POSIX)

        // POSIX implementation (shared for macOS and other POSIX)
        constexpr int kMaxFrames = 200;
        void *callstack[kMaxFrames];
        int nframes = backtrace(callstack, kMaxFrames);
        if (nframes <= 0)
        {
            safe_format_to_stderr("  [No stack frames available]\n");
            return;
        }

        char **symbols = backtrace_symbols(callstack, nframes);
        const std::string exe_path = pylabhub::platform::get_executable_name(true);

        struct FrameMeta
        {
            int idx{};
            uintptr_t addr{};
            uintptr_t offset{};    // offset within module or absolute
            void *saddr{};         // symbol address returned by dladdr
            std::string demangled; // demangled symbol name if available
            std::string dli_fname; // dli fname
            uintptr_t dli_fbase{}; // module base
        };

        std::vector<FrameMeta> metas;
        metas.reserve(static_cast<size_t>(nframes));
        for (int i = 0; i < nframes; ++i)
        {
            FrameMeta m;
            m.idx = i;
            m.addr = reinterpret_cast<uintptr_t>(callstack[i]);
            m.offset = m.addr;
            m.saddr = nullptr;
            m.dli_fbase = 0;

            Dl_info dlinfo;
            if (dladdr(callstack[i], &dlinfo))
            {
                if (dlinfo.dli_sname)
                {
                    int status = 0;
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
                    uintptr_t base = reinterpret_cast<uintptr_t>(dlinfo.dli_fbase);
                    m.dli_fbase = base;
                    if (base != 0)
                        m.offset = m.addr - base;
                    else
                        m.offset = m.addr;
                }
            }
            metas.push_back(std::move(m));
        }

        // --- Phase 1: Print safe, in-process information immediately ---
        safe_format_to_stderr("Stack Trace (most recent call first):\n");
        for (const auto &m : metas)
        {
            safe_format_to_stderr("  #{:02}  {:#018x}  ", m.idx,
                                  static_cast<unsigned long long>(m.addr));
            bool printed = false;
            if (!m.demangled.empty())
            {
                uintptr_t saddr = reinterpret_cast<uintptr_t>(m.saddr);
                if (saddr)
                    safe_format_to_stderr("{} + {:#x}", m.demangled,
                                          static_cast<unsigned long long>(m.addr - saddr));
                else
                    safe_format_to_stderr("{}", m.demangled);
                printed = true;
            }
            else if (symbols && m.idx < nframes && symbols[m.idx])
            {
                safe_format_to_stderr("{}", symbols[m.idx]);
                printed = true;
            }

            if (!printed)
            {
                if (!m.dli_fname.empty())
                    safe_format_to_stderr("({}) + {:#x}", m.dli_fname,
                                          static_cast<unsigned long long>(m.offset));
                else
                    safe_format_to_stderr("[unknown]");
            }
            safe_format_to_stderr("\n");
        }
        if (symbols)
        {
            std::free(symbols);
            symbols = nullptr;
        }
        std::fflush(stderr); // Ensure safe info is visible before risky phase

        if (!use_external_tools)
        {
            return;
        }

        // --- Phase 2: Attempt to resolve symbols with external tools ---
        safe_format_to_stderr("\n--- External Symbol Resolution ---\n");

        std::map<std::string, std::vector<int>> binToIdx;
        for (size_t i = 0; i < metas.size(); ++i)
        {
            const std::string &fname = metas[i].dli_fname.empty() ? exe_path : metas[i].dli_fname;
            std::string key = fname.empty() ? std::string("[unknown]") : fname;
            binToIdx[key].push_back(static_cast<int>(i));
        }

        for (auto &kv : binToIdx)
        {
            const std::string &binary = kv.first;
            const std::vector<int> &indices = kv.second;
            std::vector<std::string> results;

            if (binary == "[unknown]" || binary.empty())
            {
                continue;
            }

#if defined(PYLABHUB_PLATFORM_APPLE)
            const bool has_atos = internal::find_in_path("atos");
            if (has_atos)
            {
                std::vector<uintptr_t> addrs;
                addrs.reserve(indices.size());
                uintptr_t base_for_bin = 0;
                for (int idx : indices)
                {
                    addrs.push_back(metas[idx].addr);
                    if (base_for_bin == 0 && metas[idx].dli_fbase != 0)
                        base_for_bin = metas[idx].dli_fbase;
                }
                results = internal::resolve_with_atos(binary, std::span(addrs.data(), addrs.size()),
                                                      base_for_bin);
            }
            else // fallback to addr2line on mac
            {
                std::vector<uintptr_t> offsets;
                offsets.reserve(indices.size());
                for (int idx : indices)
                    offsets.push_back(metas[idx].offset);
                results = internal::resolve_with_addr2line(
                    binary, std::span(offsets.data(), offsets.size()),
                    internal::find_in_path("llvm-addr2line"));
            }
#else
            std::vector<uintptr_t> offsets;
            offsets.reserve(indices.size());
            for (int idx : indices)
                offsets.push_back(metas[idx].offset);
            results = internal::resolve_with_addr2line(
                binary, std::span(offsets.data(), offsets.size()), false);
#endif
            // Print results for this binary
            for (size_t i = 0; i < results.size(); ++i)
            {
                if (i < indices.size() && !results[i].empty() && results[i] != "??" &&
                    results[i].find("??") == std::string::npos)
                {
                    safe_format_to_stderr("  #{:02} -> [{}]\n", indices[i], results[i]);
                }
            }
        }
        std::fflush(stderr);

#else
        (void)use_external_tools;
        safe_format_to_stderr("  [Stack trace not available on this platform]\n");
#endif
    }
    catch (const fmt::format_error &e)
    {
        std::fputs("Error: Stack trace generation failed with fmt::format_error.\n", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        std::fflush(stderr);
        return;
    }
    catch (const std::bad_alloc &)
    {
        std::fputs("Error: Stack trace generation failed with std::bad_alloc.\n", stderr);
        std::fflush(stderr);
        return;
    }
    catch (...)
    {
        std::fputs("Error: Stack trace generation failed with unknown error.\n", stderr);
        std::fflush(stderr);
        return;
    }
}

} // namespace pylabhub::debug
