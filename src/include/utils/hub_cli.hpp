#pragma once
/**
 * @file hub_cli.hpp
 * @brief Public CLI parser for the future `plh_hub` binary
 *        (HEP-CORE-0033 §15 Phase 2).
 *
 * Mirrors `role_cli.hpp` (HEP-CORE-0024 §4) — same `--init` /
 * `--validate` / `--keygen` / `--config` / `--name` / `--log-*` /
 * `--help` surface, minus `--role` (the hub is single-kind; there is
 * no role-tag dispatch).
 *
 * Shape:
 *   - `HubArgs` struct holds the parsed flags.
 *   - `parse_hub_args()` returns a `ParseResult{HubArgs, exit_code}`;
 *     does NOT call `std::exit` (the caller decides what to do, and
 *     tests can capture usage / error output via in-memory ostreams).
 *
 * Header-only, no link dependency beyond system headers.
 *
 * Generic CLI helpers (`is_stdin_tty`, password input, init-name
 * resolution) live in `cli_helpers.hpp` (namespace `pylabhub::cli`);
 * call them directly from the eventual `plh_hub_main.cpp` (Phase 9)
 * — no need for forwarders here.
 *
 * The eventual `plh_hub_main.cpp` (Phase 9) consumes the parsed
 * `HubArgs` directly; until that binary lands, the parser has no
 * production caller — only L2 tests exercise it.
 *
 * See: docs/HEP/HEP-CORE-0033-Hub-Character.md §5 (CLI) + §15 (phases).
 */

#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace pylabhub::hub_cli
{

// ============================================================================
// HubArgs — parsed CLI flags
// ============================================================================

/**
 * @brief Parsed CLI flags for the hub binary.
 *
 * Field semantics match `role_cli::RoleArgs` exactly except `role`
 * (omitted — hub is single-kind) and the `*_dir` field is named
 * `hub_dir` instead of `role_dir`.  Mode flags are mutually exclusive
 * (at most one may be set).
 */
struct HubArgs
{
    std::string config_path;    ///< --config <path>
    std::string hub_dir;        ///< positional <hub_dir> or --init [dir]
    std::string init_name;      ///< --name <name>  (for --init)

    /// --log-maxsize <MB>. **Init-only.** Writes `logging.max_size_mb`
    /// into the generated JSON. Empty → use `LoggingConfig` default (10).
    std::optional<double> log_max_size_mb;

    /// --log-backups <N>. **Init-only.** Writes `logging.backups` into
    /// the generated JSON. `-1` = keep all files. Empty → default (5).
    std::optional<int> log_backups;

    bool validate_only{false};  ///< --validate
    bool keygen_only{false};    ///< --keygen
    bool init_only{false};      ///< --init
};

/**
 * @brief Outcome of `parse_hub_args` — caller decides what to do
 *        with the returned exit code (no `std::exit` from the parser).
 *
 * - `exit_code == -1` — success: `args` is valid and populated;
 *                       caller proceeds.
 * - `exit_code ==  0` — clean exit (e.g. `--help` printed usage);
 *                       caller should `return 0;` from main.
 * - `exit_code ==  1` — argument error (message already written to
 *                       `err_stream`); caller should `return 1;`.
 *
 * Tests supply in-memory `ostringstream`s to capture output without
 * spawning a subprocess.
 */
struct ParseResult
{
    HubArgs args;
    int     exit_code = -1;
};

namespace detail
{

inline void print_hub_usage(const char *prog, std::ostream &os = std::cout)
{
    os  << "Usage:\n"
        << "  " << prog << " --init [<hub_dir>]      # Create hub directory\n"
        << "  " << prog << " <hub_dir>                # Run from directory\n"
        << "  " << prog << " --config <path.json> [--validate | --keygen]\n\n"
        << "Modes (at most one; default is run):\n"
        << "  --init [dir]       Create hub directory with config template; exit 0\n"
        << "  --validate         Validate config + script; exit 0 on success\n"
        << "  --keygen           Generate hub vault (broker keypair + admin token); exit 0\n"
        << "\n"
        << "Common options:\n"
        << "  <hub_dir>          Hub directory containing hub.json\n"
        << "  --config <path>    Path to hub JSON config file\n"
        << "  --name <name>      Hub name for --init (skips interactive prompt)\n"
        << "\n"
        << "Init-only options (write into generated logging config):\n"
        << "  --log-maxsize <MB> Rotate when a log file reaches this size (default 10)\n"
        << "  --log-backups <N>  Keep N rotated files (default 5; -1 = keep all)\n"
        << "\n"
        << "  --help             Show this message\n"
        << "\n"
        << "Log destination at run time is always <hub_dir>/logs/<uid>-<ts>.log\n"
        << "(composed from config; not configurable via CLI).\n";
}

} // namespace detail

/**
 * @brief Parse command-line arguments for the hub binary.
 *
 * Does NOT call `std::exit`.  Returns `ParseResult` whose `exit_code`
 * tells the caller whether to proceed, exit cleanly (`--help`), or
 * exit with an error.  Captures usage / error text via the
 * `out_stream` / `err_stream` parameters so tests can use in-memory
 * streams without spawning a subprocess.
 *
 * @param argc        Argument count from main().
 * @param argv        Argument vector from main().
 * @param out_stream  Where `--help` usage is written.  Defaults to std::cout.
 * @param err_stream  Where parse-error messages are written.  Defaults to std::cerr.
 * @return ParseResult — see field doc.
 */
inline ParseResult parse_hub_args(int argc, char *argv[],
                                  std::ostream &out_stream = std::cout,
                                  std::ostream &err_stream = std::cerr)
{
    ParseResult result;
    HubArgs    &args = result.args;

    auto fail_with_usage = [&](std::string_view prefix) -> ParseResult & {
        err_stream << prefix;
        detail::print_hub_usage(argv[0], err_stream);
        result.exit_code = 1;
        return result;
    };

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);

        if (arg == "--help" || arg == "-h")
        {
            detail::print_hub_usage(argv[0], out_stream);
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
            // Optional positional after --init (must not start with '-').
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.hub_dir = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            args.init_name = argv[++i];
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
            if (!args.hub_dir.empty())
                return fail_with_usage(
                    "Error: multiple positional arguments not supported\n\n");
            args.hub_dir = std::string(arg);
        }
        else
        {
            err_stream << "Unknown argument: " << arg << "\n";
            detail::print_hub_usage(argv[0], err_stream);
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
    if (!args.init_only && args.config_path.empty() && args.hub_dir.empty())
        return fail_with_usage(
            "Error: specify a hub directory, --init, or --config <path>\n\n");

    return result;  // exit_code stays -1 → caller proceeds
}

} // namespace pylabhub::hub_cli
