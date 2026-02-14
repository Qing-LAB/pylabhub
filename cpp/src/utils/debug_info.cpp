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

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
#include <climits>    // PATH_MAX
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
#include "utils/format_tools.hpp"

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

// Struct to hold collected debug info for a single frame on Windows
struct WinFrameMeta
{
    int idx{};
    uintptr_t addr{};
    std::string symbol_name;
    uintptr_t symbol_displacement{};
    std::string file_name;
    DWORD line_number{};
    std::string module_full_path;
    std::string module_file_name;
    uintptr_t module_base_address{};
    uintptr_t module_offset{};
};

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
    const char *path_env = std::getenv("PATH");
    if (path_env == nullptr)
    {
        return false;
    }
    std::string_view path(path_env);
    size_t start = 0;
    while (start < path.size())
    {
        size_t pos = path.find(':', start);
        std::string_view dir =
            (pos == std::string_view::npos) ? path.substr(start) : path.substr(start, pos - start);
        start = (pos == std::string_view::npos) ? path.size() : pos + 1;
        if (dir.empty())
        {
            dir = ".";
        }
        std::string candidate;
        candidate.reserve(dir.size() + 1 + name.size());
        candidate.append(dir);
        candidate.push_back('/');
        candidate.append(name);
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] static std::string shell_quote(std::string_view str)
{
    std::string out;
    out.reserve(str.size() + 2);
    out.push_back('\'');
    for (char character : str)
    {
        if (character == '\'')
        {
            out += "'\\''"; // close, escape, reopen
        }
        else
        {
            out.push_back(character);
        }
    }
    out.push_back('\'');
    return out;
}

namespace
{
constexpr std::size_t kPopenReadBufSize = 1024;
}

[[nodiscard]] static std::vector<std::string> read_popen_lines(const std::string &cmd)
{
    std::vector<std::string> lines;
    FILE *stream = popen(cmd.c_str(), "r");
    if (stream == nullptr)
    {
        return lines;
    }
    std::array<char, kPopenReadBufSize> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), stream) != nullptr)
    {
        std::string line(buf.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    pclose(stream);
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
                {
                    acc << " ";
                }
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
    {
        cmd = "llvm-addr2line";
    }
    else if (find_in_path("addr2line"))
    {
        cmd = "addr2line";
    }
    else
    {
        return {};
    }

    cmd += " -f -C -e ";
    cmd += shell_quote(binary);

    std::ostringstream oss;
    oss << cmd;
    for (uintptr_t off : offsets)
    {
        oss << " 0x" << std::hex << off;
    }

    auto lines = read_popen_lines(oss.str());

    // addr2line with -f usually emits 2 lines per address (function, file:line).
    if (!lines.empty() && lines.size() >= offsets.size() * 2)
    {
        std::vector<std::string> perFrame;
        perFrame.reserve(offsets.size());
        for (size_t j = 0; j + 1 < lines.size() && perFrame.size() < offsets.size(); j += 2)
        {
            perFrame.push_back(lines[j] + " at " + lines[j + 1]);
        }
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
    {
        oss << " -l 0x" << std::hex << base_for_bin;
    }
    for (uintptr_t a : addrs)
    {
        oss << " 0x" << std::hex << a;
    }

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
        constexpr std::size_t kStackBufSz = 2048;
        std::array<char, kStackBufSz> stack_buf{};

        // format_to_n writes up to kStackBufSz bytes and does not allocate.
        // It may still throw fmt::format_error on an invalid format specification.
        auto result =
            fmt::format_to_n(stack_buf.data(), kStackBufSz, fmt_str, std::forward<Args>(args)...);

        const auto needed = static_cast<std::size_t>(result.size); // total required
        const std::size_t have = needed < kStackBufSz ? needed : kStackBufSz;

        if (have > 0)
        {
            std::fwrite(stack_buf.data(), 1, have, stderr);
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
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- platform branches and symbol resolution
void print_stack_trace(bool use_external_tools) noexcept
{
    try
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        (void)use_external_tools; // This flag has no effect on Windows
        // Windows implementation: two-phase approach
        constexpr int kMaxFrames = 62;
        void *frames[kMaxFrames] = {nullptr};
        USHORT framesCaptured = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
        HANDLE process = GetCurrentProcess();

        constexpr size_t kNameBuf = 1024;
        const size_t symbolBufferSize =
            sizeof(SYMBOL_INFO) + (kNameBuf - 1) * sizeof(((SYMBOL_INFO *)0)->Name[0]);
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

        std::vector<WinFrameMeta> metas;
        metas.reserve(framesCaptured);

        for (USHORT i = 0; i < framesCaptured; ++i)
        {
            WinFrameMeta m;
            m.idx = i;
            m.addr = reinterpret_cast<uintptr_t>(frames[i]);

            DWORD64 displacement = 0;
            if (symbol && SymFromAddr(process, static_cast<DWORD64>(m.addr), &displacement, symbol))
            {
                m.symbol_name = symbol->Name;
                m.symbol_displacement = static_cast<uintptr_t>(displacement);
            }

            DWORD displacementLine = 0;
            if (SymGetLineFromAddr64(process, static_cast<DWORD64>(m.addr), &displacementLine,
                                     &lineInfo))
            {
                m.file_name = lineInfo.FileName ? lineInfo.FileName : "";
                m.line_number = lineInfo.LineNumber;
            }

            IMAGEHLP_MODULE64 modInfo;
            std::memset(&modInfo, 0, sizeof(modInfo));
            modInfo.SizeOfStruct = sizeof(modInfo);
            if (SymGetModuleInfo64(process, static_cast<DWORD64>(m.addr), &modInfo))
            {
                m.module_full_path = modInfo.ImageName         ? modInfo.ImageName
                                     : modInfo.LoadedImageName ? modInfo.LoadedImageName
                                                               : "";
                m.module_file_name = pylabhub::format_tools::filename_only(m.module_full_path);
                m.module_base_address = static_cast<uintptr_t>(modInfo.BaseOfImage);
                m.module_offset = m.addr - m.module_base_address;
            }
            metas.push_back(std::move(m));
        }

        // --- Phase 1: Print safe, concise information immediately ---
        safe_format_to_stderr("Stack Trace (most recent call first):\n");
        for (const auto &m : metas)
        {
            safe_format_to_stderr("  #{:02}  {:#018x}  ", m.idx,
                                  static_cast<unsigned long long>(m.addr));
            bool printed_symbol_or_line = false;
            if (!m.symbol_name.empty())
            {
                safe_format_to_stderr("{} + {:#x}", m.symbol_name,
                                      static_cast<unsigned long long>(m.symbol_displacement));
                printed_symbol_or_line = true;
            }

            if (!m.file_name.empty())
            {
                if (!printed_symbol_or_line)
                { // If symbol wasn't printed, start with file:line
                    safe_format_to_stderr("{}:{}", m.file_name, m.line_number);
                }
                else
                { // If symbol was printed, append file:line
                    safe_format_to_stderr(" -- {}:{}", m.file_name, m.line_number);
                }
                printed_symbol_or_line = true;
            }

            if (!printed_symbol_or_line)
            {
                if (!m.module_file_name.empty())
                {
                    safe_format_to_stderr("(module: {} @ {:#018x} + {:#x})", m.module_file_name,
                                          static_cast<unsigned long long>(m.module_base_address),
                                          static_cast<unsigned long long>(m.module_offset));
                }
                else
                {
                    safe_format_to_stderr("[symbol unknown]");
                }
            }
            safe_format_to_stderr("\n");
        }
        std::fflush(stderr); // Ensure safe info is visible before risky phase

        if (!use_external_tools)
        {
            return;
        }

        // --- Phase 2: Print verbose, detailed information (Windows DbgHelp) ---
        // This is a more detailed view of the same data collected in Phase 1.
        safe_format_to_stderr("\n--- DbgHelp Detailed Output ---\n");
        for (const auto &m : metas)
        {
            safe_format_to_stderr("  #{:02} -> {:#018x} ", m.idx,
                                  static_cast<unsigned long long>(m.addr));
            if (!m.symbol_name.empty())
            {
                safe_format_to_stderr("[{} + {:#x} at", m.symbol_name,
                                      static_cast<unsigned long long>(m.symbol_displacement));
            }
            if (!m.file_name.empty())
            {
                safe_format_to_stderr("{}:{} ", m.file_name, m.line_number);
            }
            if (!m.module_file_name.empty())
            {
                safe_format_to_stderr(", (module: {} base: {:#018x}, offset: {:#x})]",
                                      m.module_file_name,
                                      static_cast<unsigned long long>(m.module_base_address),
                                      static_cast<unsigned long long>(m.module_offset));
            }
            safe_format_to_stderr("\n");
        }
        std::fflush(stderr);

#elif defined(PYLABHUB_IS_POSIX) && (PYLABHUB_IS_POSIX)

        // POSIX implementation (shared for macOS and other POSIX)
        constexpr int kMaxFrames = 200;
        std::array<void *, kMaxFrames> callstack{};
        int nframes = backtrace(callstack.data(), kMaxFrames);
        if (nframes <= 0)
        {
            safe_format_to_stderr("  [No stack frames available]\n");
            return;
        }

        char **symbols = backtrace_symbols(callstack.data(), nframes);
        const std::string exe_path = pylabhub::platform::get_executable_name(true);

        struct FrameMeta
        {
            int idx{};
            uintptr_t addr{};
            uintptr_t offset{};    // offset within module or absolute
            void *saddr{};         // symbol address returned by dladdr
            std::string demangled; // demangled symbol name if available
            std::string dli_fname; // dli fname
            std::string module_file_name;
            uintptr_t dli_fbase{}; // module base
        };

        std::vector<FrameMeta> metas;
        metas.reserve(static_cast<size_t>(nframes));
        for (int i = 0; i < nframes; ++i)
        {
            FrameMeta meta;
            meta.idx = i;
            meta.addr = reinterpret_cast<uintptr_t>(callstack[static_cast<size_t>(i)]);
            meta.offset = meta.addr;
            meta.saddr = nullptr;
            meta.dli_fbase = 0;

            Dl_info dlinfo;
            if (dladdr(callstack[static_cast<size_t>(i)], &dlinfo) != 0)
            {
                if (dlinfo.dli_sname != nullptr)
                {
                    int status = 0;
                    char *dem = abi::__cxa_demangle(dlinfo.dli_sname, nullptr, nullptr, &status);
                    if (status == 0 && dem != nullptr)
                    {
                        meta.demangled = dem;
                        std::free(dem);
                    }
                    else
                    {
                        meta.demangled = dlinfo.dli_sname;
                    }
                }
                meta.saddr = dlinfo.dli_saddr;
                if (dlinfo.dli_fname != nullptr)
                {
                    meta.dli_fname = dlinfo.dli_fname;
                    meta.module_file_name = pylabhub::format_tools::filename_only(meta.dli_fname);
                    auto base = reinterpret_cast<uintptr_t>(dlinfo.dli_fbase);
                    meta.dli_fbase = base;
                    if (base != 0)
                    {
                        meta.offset = meta.addr - base;
                    }
                    else
                    {
                        meta.offset = meta.addr;
                    }
                }
            }
            metas.push_back(std::move(meta));
        }

        // --- Phase 1: Print safe, in-process information immediately ---
        safe_format_to_stderr("Stack Trace (most recent call first):\n");
        for (const auto &meta : metas)
        {
            safe_format_to_stderr("  #{:02}  {:#018x}  ", meta.idx,
                                  static_cast<unsigned long long>(meta.addr));
            bool printed = false;
            if (!meta.demangled.empty())
            {
                auto sym_addr = reinterpret_cast<uintptr_t>(meta.saddr);
                if (sym_addr != 0U)
                {
                    safe_format_to_stderr("{} + {:#x}", meta.demangled,
                                          static_cast<unsigned long long>(meta.addr - sym_addr));
                }
                else
                {
                    safe_format_to_stderr("{}", meta.demangled);
                }
                printed = true;
            }
            else if (symbols != nullptr && meta.idx < nframes && symbols[meta.idx] != nullptr)
            {
                safe_format_to_stderr("{}", symbols[meta.idx]);
                printed = true;
            }

            if (!printed)
            {
                if (!meta.module_file_name.empty())
                {
                    safe_format_to_stderr("(module: {} @ {:#018x} + {:#x})", meta.module_file_name,
                                          static_cast<unsigned long long>(meta.dli_fbase),
                                          static_cast<unsigned long long>(meta.offset));
                }
                else
                {
                    safe_format_to_stderr("[symbol unknown]");
                }
            }
            safe_format_to_stderr("\n");
        }
        if (symbols != nullptr)
        {
            std::free(reinterpret_cast<void *>(symbols));
            symbols = nullptr;
        }
        std::fflush(stderr); // Ensure safe info is visible before risky phase

        if (!use_external_tools)
        {
            return;
        }

        // --- Phase 2: Attempt to resolve symbols with external tools (e.g., addr2line) ---
        safe_format_to_stderr("\n--- External Symbol Resolution ---\n");

        // We need a map from actual frame index to the resolved string.
        std::map<int, std::string> resolved_symbols_by_frame_idx;

        std::map<std::string, std::vector<int>> binToIdx;
        for (size_t i = 0; i < metas.size(); ++i)
        {
            // Use the module's full path for grouping, as addr2line/atos expect it.
            const std::string &fname = metas[i].dli_fname.empty() ? exe_path : metas[i].dli_fname;
            std::string key = fname.empty() ? std::string("[unknown]") : fname;
            binToIdx[key].push_back(static_cast<int>(i));
        }

        for (auto &entry : binToIdx)
        {
            const std::string &binary = entry.first;
            const std::vector<int> &indices = entry.second; // These are frame indices for this binary
            std::vector<std::string> tool_results;       // Raw output from addr2line/atos

            if (binary == "[unknown]" || binary.empty())
            {
                // We cannot symbolize unknown binaries
                for (int frame_idx : indices)
                {
                    resolved_symbols_by_frame_idx[frame_idx] =
                        "[could not resolve - unknown binary]";
                }
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
                    addrs.push_back(metas[static_cast<size_t>(idx)].addr);
                    if (base_for_bin == 0 && metas[static_cast<size_t>(idx)].dli_fbase != 0)
                    {
                        base_for_bin = metas[static_cast<size_t>(idx)].dli_fbase;
                    }
                }
                tool_results = internal::resolve_with_atos(
                    binary, std::span(addrs.data(), addrs.size()), base_for_bin);
            }
            else // fallback to addr2line on mac
            {
                std::vector<uintptr_t> offsets;
                offsets.reserve(indices.size());
                for (int idx : indices)
                {
                    offsets.push_back(metas[static_cast<size_t>(idx)].offset);
                }
                tool_results = internal::resolve_with_addr2line(
                    binary, std::span(offsets.data(), offsets.size()),
                    internal::find_in_path("llvm-addr2line"));
            }
#else
            std::vector<uintptr_t> offsets;
            offsets.reserve(indices.size());
            for (int idx : indices)
            {
                offsets.push_back(metas[static_cast<size_t>(idx)].offset);
            }
            tool_results = internal::resolve_with_addr2line(
                binary, std::span(offsets.data(), offsets.size()), false);
#endif
            // Map tool_results back to frame indices
            for (size_t i = 0; i < tool_results.size(); ++i)
            {
                if (i < indices.size() && !tool_results[i].empty() && tool_results[i] != "??" &&
                    tool_results[i].find("??") == std::string::npos)
                {
                    resolved_symbols_by_frame_idx[indices[i]] = tool_results[i];
                }
                else
                {
                    resolved_symbols_by_frame_idx[indices[i]] = "[could not resolve]";
                }
            }
        } // end of binToIdx loop

        // Now print all frames using the resolved_symbols_by_frame_idx map
        for (const auto &meta : metas)
        {
            std::string resolved_str =
                resolved_symbols_by_frame_idx.contains(meta.idx)
                    ? resolved_symbols_by_frame_idx.at(meta.idx)
                    : "[not processed by external tools]";

            // Attempt to parse resolved_str
            std::string symbol_name;
            std::string file_line; // file:line or just file

            // addr2line format: "function_name at file:line"
            // atos format: "function_name (file:line)" or "function_name"
            // atos format with offset: "function_name + 0xSOMETHING (file:line)"

            // Try to find " at " first (addr2line style)
            size_t at_pos = resolved_str.find(" at ");
            if (at_pos != std::string::npos)
            {
                symbol_name = resolved_str.substr(0, at_pos);
                file_line = resolved_str.substr(at_pos + 4);
            }
            else
            {
                // Then try parsing for atos-like output: "function_name (file:line)"
                size_t open_paren = resolved_str.find('(');
                size_t close_paren = resolved_str.rfind(')');
                if (open_paren != std::string::npos && close_paren != std::string::npos &&
                    close_paren > open_paren)
                {
                    symbol_name = resolved_str.substr(0, open_paren);
                    // Trim trailing space from symbol_name
                    symbol_name.erase(symbol_name.find_last_not_of(' ') + 1);
                    file_line = resolved_str.substr(open_paren + 1, close_paren - (open_paren + 1));
                }
                else
                {
                    // Fallback: entire string is symbol_name, no file:line
                    symbol_name = resolved_str;
                    file_line = "";
                }
            }

            safe_format_to_stderr("  #{:02} -> {:#018x} ", meta.idx,
                                  static_cast<unsigned long long>(meta.addr));
            safe_format_to_stderr("[");

            // Print symbol info
            if (!symbol_name.empty() && symbol_name != "[could not resolve]" &&
                symbol_name != "[could not resolve - unknown binary]")
            {
                // If dladdr provided a symbol address, calculate offset from that.
                // Otherwise, the tool_results might have an offset already (e.g., atos) or we just
                // use module offset.
                uintptr_t print_offset = meta.offset; // Default to module offset
                if (meta.saddr != nullptr)
                {
                    print_offset = meta.addr - reinterpret_cast<uintptr_t>(meta.saddr);
                }
                safe_format_to_stderr("{} + {:#x}", symbol_name,
                                      static_cast<unsigned long long>(print_offset));
            }
            else
            {
                safe_format_to_stderr(
                    "{}",
                    resolved_str); // Print raw resolved_str if parsing failed or generic message
            }

            // Print file:line if available
            if (!file_line.empty())
            {
                safe_format_to_stderr(" at {}", file_line);
            }

            // Print module info (always try to include if available from dladdr)
            if (!meta.module_file_name.empty())
            {
                safe_format_to_stderr(" , (module: {} base: {:#018x}, offset: {:#x})",
                                      meta.module_file_name,
                                      static_cast<unsigned long long>(meta.dli_fbase),
                                      static_cast<unsigned long long>(meta.offset));
            }
            safe_format_to_stderr("]\n");
        } // end of printing loop
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
