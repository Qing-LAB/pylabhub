#pragma once
/**
 * @file cli_helpers.hpp
 * @brief Generic CLI helpers shared by all pyLabHub binaries.
 *
 * These utilities are deliberately decoupled from any specific binary
 * (role / hub / future tooling).  Both `role_cli.hpp` (HEP-CORE-0024 §4)
 * and `hub_cli.hpp` (HEP-CORE-0033 §5) live alongside this header and
 * call into it as peers; neither depends on the other's namespace.
 *
 * Provides:
 *   - TTY detection (`is_stdin_tty`)
 *   - Secure password input (`read_password_interactive`, `get_password`,
 *     `get_new_password` — env-var name supplied by caller so each binary
 *     keeps its own override variable)
 *   - Init-name resolution (`resolve_init_name`)
 *
 * Header-only; no link dependency beyond system headers.
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#if defined(PYLABHUB_IS_POSIX) || defined(__unix__) || defined(__APPLE__)
#  ifndef PYLABHUB_IS_POSIX
#    define PYLABHUB_IS_POSIX 1
#  endif
#  include <unistd.h> // isatty, STDIN_FILENO, getpass
#elif defined(_WIN32)
#  include <io.h>      // _isatty, _fileno
#  include <windows.h> // SetConsoleMode, GetConsoleMode, ENABLE_ECHO_INPUT
#endif

namespace pylabhub::cli
{

// ============================================================================
// TTY detection
// ============================================================================

/**
 * @brief Return true when stdin is an interactive terminal (not a pipe or
 *        redirect).
 *
 * Used to gate prompts: if stdin is not a TTY, interactive prompts are
 * skipped and callers must supply values via CLI flags or environment
 * variables.
 */
inline bool is_stdin_tty()
{
#if defined(PYLABHUB_IS_POSIX)
    return ::isatty(STDIN_FILENO) != 0;
#elif defined(PYLABHUB_PLATFORM_WIN64)
    // _isatty() returns true for any character device (including NUL),
    // which gives a false positive when stdin is redirected to NUL.
    // GetConsoleMode() only succeeds for actual console handles.
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return false;
#endif
}

// ============================================================================
// Password helpers
// ============================================================================

/**
 * @brief Read a password from the terminal without echoing.
 *
 * POSIX: uses `getpass()`. Windows: temporarily disables ENABLE_ECHO_INPUT
 * on the console handle.  Only call this when `is_stdin_tty()` is true.
 *
 * @param name    Subject name used in error messages (e.g. "producer", "hub").
 * @param prompt  Prompt displayed to the user.
 * @return Password string; empty if the user pressed Enter with no input.
 */
inline std::string read_password_interactive(const char *name, const char *prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char *pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "%s: failed to read password from terminal\n", name);
        return {};
    }
    return pw;
#elif defined(_WIN32)
    (void)name;
    HANDLE hStdin   = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD  old_mode = 0;
    const bool is_con =
        (hStdin != INVALID_HANDLE_VALUE) &&
        (::GetConsoleMode(hStdin, &old_mode) != 0);
    if (is_con)
        ::SetConsoleMode(hStdin, old_mode & ~ENABLE_ECHO_INPUT);
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    if (is_con)
    {
        ::SetConsoleMode(hStdin, old_mode);
        std::fprintf(stderr, "\n");
    }
    return pw;
#else
    (void)name;
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
#endif
}

/**
 * @brief Resolve a vault password from the environment or by prompting.
 *
 * Priority: @p env_var (if set in the environment) → interactive terminal
 * prompt.  Returns nullopt when stdin is not a TTY and the env var is not
 * set; the caller should treat nullopt as a fatal error (message already
 * on stderr).
 *
 * @param name    Subject name used in error messages.
 * @param env_var Environment-variable name to consult first
 *                (e.g. "PYLABHUB_ROLE_PASSWORD", "PYLABHUB_HUB_PASSWORD").
 * @param prompt  Prompt displayed when falling back to interactive input.
 */
inline std::optional<std::string> get_password(const char *name,
                                                const char *env_var,
                                                const char *prompt)
{
    if (const char *env = std::getenv(env_var))
        return std::string(env);
    if (!is_stdin_tty())
    {
        std::fprintf(stderr,
                     "%s: vault password required; set %s "
                     "for non-interactive use\n",
                     name, env_var);
        return std::nullopt;
    }
    return read_password_interactive(name, prompt);
}

/**
 * @brief Read a new vault password with confirmation.
 *
 * When stdin is a TTY: prompts twice and verifies the passwords match.
 * Otherwise falls back to @p env_var (no confirmation possible).
 *
 * Returns nullopt on mismatch or when no source is available (error
 * already on stderr).
 *
 * @param name           Subject name used in error messages.
 * @param env_var        Environment-variable name to consult first.
 * @param prompt         First prompt string.
 * @param confirm_prompt Confirmation prompt string.
 */
inline std::optional<std::string> get_new_password(const char *name,
                                                    const char *env_var,
                                                    const char *prompt,
                                                    const char *confirm_prompt)
{
    if (const char *env = std::getenv(env_var))
        return std::string(env);

    if (!is_stdin_tty())
    {
        std::fprintf(stderr,
                     "%s: vault password required; set %s "
                     "for non-interactive use\n",
                     name, env_var);
        return std::nullopt;
    }

    const std::string pw      = read_password_interactive(name, prompt);
    const std::string confirm = read_password_interactive(name, confirm_prompt);
    if (pw != confirm)
    {
        std::fprintf(stderr, "%s: passwords do not match\n", name);
        return std::nullopt;
    }
    return pw;
}

// ============================================================================
// Init-name resolution
// ============================================================================

/**
 * @brief Resolve the `--name` / interactive prompt flow for `--init` modes.
 *
 * - @p cli_name non-empty → returned directly.
 * - stdin is a TTY → prompt with @p prompt and read a line.
 * - Otherwise → prints error to stderr and returns nullopt.
 *
 * @param cli_name  Value of the `--name` flag (may be empty).
 * @param prompt    Prompt displayed when falling back to interactive input.
 */
inline std::optional<std::string> resolve_init_name(const std::string &cli_name,
                                                     const char        *prompt)
{
    if (!cli_name.empty())
        return cli_name;

    if (is_stdin_tty())
    {
        std::cout << prompt;
        std::string name;
        std::getline(std::cin, name);
        return name;
    }

    std::fprintf(stderr, "Error: --name is required in non-interactive mode\n");
    return std::nullopt;
}

} // namespace pylabhub::cli
