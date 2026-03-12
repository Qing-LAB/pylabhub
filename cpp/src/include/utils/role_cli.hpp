#pragma once
/**
 * @file role_cli.hpp
 * @brief Public CLI helpers for pyLabHub role binaries (HEP-CORE-0024).
 *
 * Provides:
 *   - TTY detection (is_stdin_tty)
 *   - Secure password input (read_password_interactive, get_role_password,
 *     get_new_role_password)
 *   - Generic argument parser (RoleArgs, parse_role_args, resolve_init_name)
 *
 * These utilities cover the identical CLI boilerplate found in
 * pylabhub-producer, pylabhub-consumer, and pylabhub-processor.  They can also
 * be used when building custom role binaries against the pyLabHub public API.
 *
 * All functions are inline/header-only (no link dependency beyond system headers).
 *
 * See: docs/HEP/HEP-CORE-0024-Role-Directory-Service.md §4
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#if defined(PYLABHUB_IS_POSIX) || defined(__unix__) || defined(__APPLE__)
#  ifndef PYLABHUB_IS_POSIX
#    define PYLABHUB_IS_POSIX 1 // fallback when plh_platform.hpp not included
#  endif
#  include <unistd.h> // isatty, STDIN_FILENO, getpass
#elif defined(_WIN32)
#  include <io.h>      // _isatty, _fileno
#  include <windows.h> // SetConsoleMode, GetConsoleMode, ENABLE_ECHO_INPUT
#endif

namespace pylabhub::role_cli
{

// ============================================================================
// TTY detection
// ============================================================================

/**
 * @brief Return true when stdin is an interactive terminal (not a pipe or
 *        redirect).
 *
 * Used to gate prompts: if stdin is not a TTY, interactive prompts are skipped
 * and callers must supply values via CLI flags or environment variables.
 */
inline bool is_stdin_tty()
{
#if defined(PYLABHUB_IS_POSIX)
    return ::isatty(STDIN_FILENO) != 0;
#elif defined(_WIN32)
    return ::_isatty(::_fileno(stdin)) != 0;
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
 * POSIX: uses getpass(). Windows: temporarily disables ENABLE_ECHO_INPUT on
 * the console handle.  Only call this when is_stdin_tty() is true.
 *
 * @param role_name  Role name used in error messages (e.g. "producer").
 * @param prompt     Prompt displayed to the user.
 * @return Password string; empty if the user pressed Enter with no input.
 */
inline std::string read_password_interactive(const char *role_name,
                                              const char *prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char *pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "%s: failed to read password from terminal\n",
                     role_name);
        return {};
    }
    return pw;
#elif defined(_WIN32)
    (void)role_name;
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
    (void)role_name;
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
#endif
}

/**
 * @brief Return the vault password from the environment or by prompting.
 *
 * Priority: PYLABHUB_ACTOR_PASSWORD env var → interactive terminal prompt.
 * Returns nullopt when stdin is not a TTY and the env var is not set; the
 * caller should treat nullopt as a fatal error (message already on stderr).
 *
 * @param role_name  Role name used in error messages.
 * @param prompt     Prompt displayed when falling back to interactive input.
 */
inline std::optional<std::string> get_role_password(const char *role_name,
                                                      const char *prompt)
{
    if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        return std::string(env);
    if (!is_stdin_tty())
    {
        std::fprintf(stderr,
                     "%s: vault password required; set PYLABHUB_ACTOR_PASSWORD "
                     "for non-interactive use\n",
                     role_name);
        return std::nullopt;
    }
    return read_password_interactive(role_name, prompt);
}

/**
 * @brief Read a new vault password with confirmation.
 *
 * When stdin is a TTY: prompts twice and verifies the passwords match.
 * Otherwise falls back to PYLABHUB_ACTOR_PASSWORD (no confirmation).
 *
 * Returns nullopt on mismatch or when no source is available (error on stderr).
 *
 * @param role_name       Role name used in error messages.
 * @param prompt          First prompt string.
 * @param confirm_prompt  Confirmation prompt string.
 */
inline std::optional<std::string> get_new_role_password(
    const char *role_name,
    const char *prompt,
    const char *confirm_prompt)
{
    if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        return std::string(env);

    if (!is_stdin_tty())
    {
        std::fprintf(stderr,
                     "%s: vault password required; set PYLABHUB_ACTOR_PASSWORD "
                     "for non-interactive use\n",
                     role_name);
        return std::nullopt;
    }

    const std::string pw      = read_password_interactive(role_name, prompt);
    const std::string confirm = read_password_interactive(role_name, confirm_prompt);
    if (pw != confirm)
    {
        std::fprintf(stderr, "%s: passwords do not match\n", role_name);
        return std::nullopt;
    }
    return pw;
}

// ============================================================================
// Argument parsing
// ============================================================================

/**
 * @brief Parsed CLI arguments common to all four role binaries.
 *
 * Fields left at their defaults indicate that the corresponding flag was not
 * supplied.
 */
struct RoleArgs
{
    std::string config_path;    ///< --config <path>
    std::string role_dir;       ///< positional <role_dir> or --init [dir]
    std::string init_name;      ///< --name <name>  (for --init)
    bool        validate_only{false};
    bool        keygen_only{false};
    bool        init_only{false};
};

namespace detail
{

inline void print_role_usage(const char *prog, const char *role_name)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --init [<" << role_name << "_dir>]"
                                        "            # Create role directory\n"
        << "  " << prog << " <" << role_name << "_dir>"
                                        "                     # Run from directory\n"
        << "  " << prog << " --config <path.json>"
                                        " [--validate | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --init [dir]    Create role directory with config template; exit 0\n"
        << "  --name <name>   Role name for --init (skips interactive prompt)\n"
        << "  <role_dir>      Role directory containing <role>.json\n"
        << "  --config <path> Path to role JSON config file\n"
        << "  --validate      Validate config + script; exit 0 on success\n"
        << "  --keygen        Generate NaCl keypair at auth.keyfile path; exit 0\n"
        << "  --run           Explicit run mode (default when no other mode given)\n"
        << "  --help          Show this message\n";
}

} // namespace detail

/**
 * @brief Parse command-line arguments for a role binary.
 *
 * On --help or parse error, prints usage and calls std::exit(0/1).
 *
 * @param argc       Argument count from main().
 * @param argv       Argument vector from main().
 * @param role_name  Short role name, e.g. "producer" (used in usage text).
 */
inline RoleArgs parse_role_args(int argc, char *argv[], const char *role_name)
{
    RoleArgs args;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);

        if (arg == "--help" || arg == "-h")
        {
            detail::print_role_usage(argv[0], role_name);
            std::exit(0);
        }
        if (arg == "--config" && i + 1 < argc)
        {
            args.config_path = argv[++i];
        }
        else if (arg == "--init")
        {
            args.init_only = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.role_dir = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            args.init_name = argv[++i];
        }
        else if (arg == "--validate")
        {
            args.validate_only = true;
        }
        else if (arg == "--keygen")
        {
            args.keygen_only = true;
        }
        else if (arg == "--run")
        {
            // explicit run mode — no-op
        }
        else if (arg[0] != '-')
        {
            if (!args.role_dir.empty())
            {
                std::cerr << "Error: multiple positional arguments not supported\n\n";
                detail::print_role_usage(argv[0], role_name);
                std::exit(1);
            }
            args.role_dir = std::string(arg);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            detail::print_role_usage(argv[0], role_name);
            std::exit(1);
        }
    }

    if (!args.init_only && args.config_path.empty() && args.role_dir.empty())
    {
        std::cerr << "Error: specify a role directory, --init, or --config <path>\n\n";
        detail::print_role_usage(argv[0], role_name);
        std::exit(1);
    }
    return args;
}

/**
 * @brief Resolve the `--name` / interactive prompt flow for --init.
 *
 * - @p cli_name non-empty → returned directly.
 * - stdin is a TTY → prompt with @p prompt and read a line.
 * - Otherwise → prints error and returns nullopt.
 *
 * @param cli_name  Value of the `--name` flag (may be empty).
 * @param prompt    Prompt string displayed when falling back to interactive input.
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

} // namespace pylabhub::role_cli
