#pragma once
/**
 * @file role_cli.hpp
 * @brief Public CLI parser for pyLabHub role binaries (HEP-CORE-0024 §4).
 *
 * Provides:
 *   - `RoleArgs` struct holding the parsed flags
 *   - `parse_role_args()` returning a `ParseResult{RoleArgs, exit_code}`;
 *     never calls `std::exit` — caller decides what to do, and tests can
 *     capture usage / error output via in-memory ostreams.
 *
 * Generic CLI helpers (`is_stdin_tty`, `read_password_interactive`,
 * `get_password`, `get_new_password`, `resolve_init_name`) live in
 * `cli_helpers.hpp` (namespace `pylabhub::cli`) so role and hub
 * binaries can call them as peers.
 *
 * Header-only; no link dependency beyond system headers.
 *
 * See: docs/HEP/HEP-CORE-0024-Role-Directory-Service.md §4
 */

#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace pylabhub::role_cli
{

// ============================================================================
// Argument parsing
// ============================================================================

/**
 * @brief Parsed CLI arguments common to all role binaries.
 *
 * Fields left at their defaults indicate that the corresponding flag was not
 * supplied.
 */
struct RoleArgs
{
    std::string config_path;    ///< --config <path>
    std::string role_dir;       ///< positional <role_dir> or --init [dir]
    std::string init_name;      ///< --name <name>  (for --init)

    /// --role <tag>. Long-form role name ("producer"/"consumer"/"processor").
    /// Required by @c plh_role's map-based dispatch; ignored by the legacy
    /// per-role binaries (or validated to match their preset name).
    std::string role;

    /// --log-maxsize <MB>. **Init-only.** Writes @c logging.max_size_mb
    /// into the generated JSON. Empty → use LoggingConfig default (10).
    std::optional<double> log_max_size_mb;

    /// --log-backups <N>. **Init-only.** Writes @c logging.backups into
    /// the generated JSON. @c -1 = keep all files. Empty → default (5).
    std::optional<int> log_backups;

    bool validate_only{false};  ///< --validate
    bool keygen_only{false};    ///< --keygen
    bool init_only{false};      ///< --init
};

namespace detail
{

inline void print_role_usage(const char *prog, const char *role_name,
                              std::ostream &os = std::cout)
{
    os  << "Usage:\n"
        << "  " << prog << " --init [<" << role_name << "_dir>]  # Create role directory\n"
        << "  " << prog << " <" << role_name << "_dir>             # Run from directory\n"
        << "  " << prog << " --config <path.json> [--validate | --keygen]\n\n"
        << "Modes (at most one; default is run):\n"
        << "  --init [dir]       Create role directory with config template; exit 0\n"
        << "  --validate         Validate config + script; exit 0 on success\n"
        << "  --keygen           Generate NaCl keypair at auth.keyfile path; exit 0\n"
        << "\n"
        << "Common options:\n"
        << "  <role_dir>         Role directory containing <role>.json\n"
        << "  --config <path>    Path to role JSON config file\n"
        << "  --role <tag>       Role type (required by plh_role; otherwise preset)\n"
        << "  --name <name>      Role name for --init (skips interactive prompt)\n"
        << "\n"
        << "Init-only options (write into generated logging config):\n"
        << "  --log-maxsize <MB> Rotate when a log file reaches this size (default 10)\n"
        << "  --log-backups <N>  Keep N rotated files (default 5; -1 = keep all)\n"
        << "\n"
        << "  --help             Show this message\n"
        << "\n"
        << "Log destination at run time is always <role_dir>/logs/<uid>-<ts>.log\n"
        << "(composed from config; not configurable via CLI).\n";
}

} // namespace detail

/**
 * @brief Outcome of @ref parse_role_args — callers do not exit from the parser.
 *
 * @c exit_code == -1 means "continue running": @c args is valid and populated.
 * @c exit_code ==  0 means "clean exit (e.g. --help printed usage)": caller
 *                     should return that code.
 * @c exit_code ==  1 means "argument error": an explanatory message was
 *                     written to @c err_stream; caller should return 1.
 *
 * Tests supply in-memory ostreams (ostringstream) to capture output without
 * spawning a subprocess — no @c std::exit from the parser.
 */
struct ParseResult
{
    RoleArgs args;
    int      exit_code = -1;
};

/**
 * @brief Parse command-line arguments for a role binary.
 *
 * Does NOT call @c std::exit. Returns @ref ParseResult whose @c exit_code
 * tells the caller whether to proceed, exit cleanly (e.g. `--help`), or
 * exit with an error. Captures usage/error text via the @p out_stream /
 * @p err_stream parameters so tests can use in-memory streams without
 * spawning a subprocess.
 *
 * @param argc        Argument count from main().
 * @param argv        Argument vector from main().
 * @param role_name   Short role name, e.g. "producer" (used in usage text).
 * @param out_stream  Where `--help` usage is written. Defaults to std::cout.
 * @param err_stream  Where parse-error messages are written. Defaults to std::cerr.
 * @return ParseResult — on success @c exit_code==-1 and @c args is populated;
 *         on --help @c exit_code==0 (usage already printed);
 *         on error  @c exit_code==1 (message already printed).
 */
inline ParseResult parse_role_args(int argc, char *argv[],
                                     const char *role_name,
                                     std::ostream &out_stream = std::cout,
                                     std::ostream &err_stream = std::cerr)
{
    ParseResult result;
    RoleArgs   &args = result.args;

    auto fail_with_usage = [&](std::string_view prefix) -> ParseResult & {
        err_stream << prefix;
        detail::print_role_usage(argv[0], role_name, err_stream);
        result.exit_code = 1;
        return result;
    };

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);

        if (arg == "--help" || arg == "-h")
        {
            detail::print_role_usage(argv[0], role_name, out_stream);
            result.exit_code = 0;
            return result;
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
        else if (arg == "--role" && i + 1 < argc)
        {
            args.role = argv[++i];
        }
        else if (arg == "--log-maxsize" && i + 1 < argc)
        {
            try { args.log_max_size_mb = std::stod(argv[++i]); }
            catch (const std::exception &)
            {
                err_stream << "Error: --log-maxsize expects a number (MB)\n";
                result.exit_code = 1;
                return result;
            }
        }
        else if (arg == "--log-backups" && i + 1 < argc)
        {
            try { args.log_backups = std::stoi(argv[++i]); }
            catch (const std::exception &)
            {
                err_stream << "Error: --log-backups expects an integer "
                              "(-1 = keep all)\n";
                result.exit_code = 1;
                return result;
            }
        }
        else if (arg == "--validate")
        {
            args.validate_only = true;
        }
        else if (arg == "--keygen")
        {
            args.keygen_only = true;
        }
        else if (arg[0] != '-')
        {
            if (!args.role_dir.empty())
                return fail_with_usage(
                    "Error: multiple positional arguments not supported\n\n");
            args.role_dir = std::string(arg);
        }
        else
        {
            err_stream << "Unknown argument: " << arg << "\n";
            detail::print_role_usage(argv[0], role_name, err_stream);
            result.exit_code = 1;
            return result;
        }
    }

    // ── Mode exclusion: at most one of --init / --validate / --keygen ──
    const int mode_count = static_cast<int>(args.init_only) +
                           static_cast<int>(args.validate_only) +
                           static_cast<int>(args.keygen_only);
    if (mode_count > 1)
        return fail_with_usage(
            "Error: --init, --validate, and --keygen are mutually "
            "exclusive (at most one mode).\n\n");

    // ── Init-only flags must not appear outside --init ─────────────────
    if (!args.init_only &&
        (args.log_max_size_mb.has_value() || args.log_backups.has_value() ||
         !args.init_name.empty()))
        return fail_with_usage(
            "Error: --name, --log-maxsize, and --log-backups are "
            "only valid with --init.\n\n");

    // ── Required positional for non-init modes ─────────────────────────
    if (!args.init_only && args.config_path.empty() && args.role_dir.empty())
        return fail_with_usage(
            "Error: specify a role directory, --init, or --config <path>\n\n");

    return result;  // exit_code stays -1 → caller proceeds
}

} // namespace pylabhub::role_cli
