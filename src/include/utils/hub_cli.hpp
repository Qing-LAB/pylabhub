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
#include <vector>

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
    bool skeleton_only{false};  ///< --skeleton (HEP-CORE-0033 §6.5 layout-only path)

    /// --uid <value>. Hub UID supplied at the CLI boundary.  Resolved
    /// by `cli::get_required_uid` against env / TTY prompt per
    /// HEP-CORE-0033 §6.5 source-priority chain.  Empty = unsupplied.
    std::string hub_uid;

    /// --vault-path <path>. Optional override for `hub.auth.keyfile`
    /// at init time.  Empty = use the template default
    /// (`vault/<uid>.vault` inside hub_dir).
    std::string vault_path;

    /// --no-prompt. When true, suppress interactive TTY prompts even
    /// when stdin is a TTY (HEP-CORE-0033 §6.5).  Useful for
    /// systemd / Docker entrypoints with an attached TTY that
    /// shouldn't accept operator input.
    bool no_prompt{false};

    // ── PeerAdmission Phase B — operator allowlist CLI ─────────────────
    /// --add-known-role <name> <uid> <role> <pubkey_z85>
    bool add_known_role_only{false};
    /// --revoke-known-role <uid>
    bool revoke_known_role_only{false};
    /// --list-known-roles
    bool list_known_roles_only{false};
    /// --migrate-known-roles (one-shot: plaintext known_roles.json → vault)
    bool migrate_known_roles_only{false};

    /// Populated positional args for the three known-role ops above.
    /// Order is invariant per op:
    ///   add-known-role:    {name, uid, role, pubkey_z85}
    ///   revoke-known-role: {uid}
    ///   list-known-roles:  {}
    /// Parser enforces arity; dispatcher consumes by position.
    std::vector<std::string> known_role_args;
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

/// Allowed values for `<role>` in `--add-known-role`.  The role kind
/// is operator-facing metadata recorded on the `KnownRole` entry in the
/// vault allowlist; an arbitrary string (typo) would persist into the
/// allowlist and mislead operator tooling / logs.  Validating at parse
/// time surfaces operator typos immediately rather than after the fact.
///
/// Empty string is accepted as a synonym for "any" — matches the
/// KnownRole struct's `role` field doc: `empty = "any"`.
inline bool is_valid_known_role_kind(std::string_view r)
{
    return r.empty() ||
           r == "producer" || r == "consumer" ||
           r == "processor" || r == "any";
}

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
        << "Known-roles allowlist (PeerAdmission Phase B; HEP-CORE-0035 §4.8):\n"
        << "  The allowlist lives INSIDE the encrypted hub vault; every command\n"
        << "  below unlocks it with the master password (PYLABHUB_HUB_PASSWORD or\n"
        << "  interactive prompt) and re-encrypts on change.\n"
        << "  --add-known-role <name> <uid> <role> <pubkey_z85>\n"
        << "                     Add a role to the vault allowlist; exit 0.\n"
        << "                     <role> ∈ {producer, consumer, processor, any}.\n"
        << "                     <pubkey_z85> is the 40-char Z85-encoded CURVE pubkey\n"
        << "                     (obtain via `plh_role --print-pubkey`).\n"
        << "                     Re-add with same <uid> rotates the pubkey (no error).\n"
        << "  --revoke-known-role <uid>\n"
        << "                     Remove the entry matching <uid>; exit 0 even if absent.\n"
        << "  --list-known-roles Print the allowlist in tabular form; exit 0.\n"
        << "  --migrate-known-roles\n"
        << "                     One-shot: import a legacy plaintext\n"
        << "                     <hub_dir>/vault/known_roles.json into the vault and\n"
        << "                     delete it (the hub refuses to start while it exists).\n"
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
        else if (arg == "--skeleton")
        {
            args.skeleton_only = true;
            // Optional positional after --skeleton (must not start with '-').
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.hub_dir = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            args.init_name = argv[++i];
        }
        else if (arg == "--uid" && i + 1 < argc)
        {
            args.hub_uid = argv[++i];
        }
        else if (arg == "--vault-path" && i + 1 < argc)
        {
            args.vault_path = argv[++i];
        }
        else if (arg == "--no-prompt")
        {
            args.no_prompt = true;
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
        else if (arg == "--add-known-role")
        {
            // Expect exactly 4 positionals after: name, uid, role, pubkey.
            if (i + 4 >= argc)
                return fail_with_usage(
                    "Error: --add-known-role requires 4 args: "
                    "<name> <uid> <role> <pubkey_z85>\n\n");
            // H-K4: enum-validate <role> at the input boundary.  An
            // arbitrary string here (e.g. typo'd "prodcer") would
            // persist into the allowlist and silently never match the
            // broker's case-sensitive role string comparison; surface
            // the error immediately instead.
            const std::string_view role_arg(argv[i + 3]);
            if (!detail::is_valid_known_role_kind(role_arg))
            {
                err_stream
                    << "Error: --add-known-role <role> must be one of "
                       "{producer, consumer, processor, any} (empty also "
                       "accepted, meaning 'any'); got '"
                    << role_arg << "'\n\n";
                detail::print_hub_usage(argv[0], err_stream);
                result.exit_code = 1;
                return result;
            }
            args.add_known_role_only = true;
            args.known_role_args.assign({
                std::string(argv[i + 1]),
                std::string(argv[i + 2]),
                std::string(argv[i + 3]),
                std::string(argv[i + 4]),
            });
            i += 4;
        }
        else if (arg == "--revoke-known-role")
        {
            if (i + 1 >= argc)
                return fail_with_usage(
                    "Error: --revoke-known-role requires 1 arg: <uid>\n\n");
            args.revoke_known_role_only = true;
            args.known_role_args.assign({std::string(argv[i + 1])});
            i += 1;
        }
        else if (arg == "--list-known-roles")
        {
            args.list_known_roles_only = true;
        }
        else if (arg == "--migrate-known-roles")
        {
            args.migrate_known_roles_only = true;
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

    // ── Mode exclusion: at most one mode flag ──────────────────────────
    const int mode_count = static_cast<int>(args.init_only) +
                           static_cast<int>(args.skeleton_only) +
                           static_cast<int>(args.validate_only) +
                           static_cast<int>(args.keygen_only) +
                           static_cast<int>(args.add_known_role_only) +
                           static_cast<int>(args.revoke_known_role_only) +
                           static_cast<int>(args.list_known_roles_only);
    if (mode_count > 1)
        return fail_with_usage(
            "Error: mode flags (--init, --skeleton, --validate, --keygen, "
            "--add-known-role, --revoke-known-role, --list-known-roles) "
            "are mutually exclusive (at most one mode).\n\n");

    // ── Init/skeleton-only flags must not appear outside those modes ───
    const bool init_or_skeleton = args.init_only || args.skeleton_only;
    if (!init_or_skeleton &&
        (args.log_max_size_mb.has_value() || args.log_backups.has_value() ||
         !args.init_name.empty() || !args.hub_uid.empty() ||
         !args.vault_path.empty()))
        return fail_with_usage(
            "Error: --name, --uid, --vault-path, --log-maxsize, and "
            "--log-backups are only valid with --init or --skeleton.\n\n");

    // ── Required positional for non-init/skeleton modes ────────────────
    // The known-role ops need hub_dir (to locate
    // <hub_dir>/vault/known_roles.json); all other run modes need it
    // too.  Only --init / --skeleton can synthesize a new hub_dir.
    if (!init_or_skeleton && args.config_path.empty() && args.hub_dir.empty())
        return fail_with_usage(
            "Error: specify a hub directory, --init, --skeleton, "
            "or --config <path>\n\n");

    return result;  // exit_code stays -1 → caller proceeds
}

} // namespace pylabhub::hub_cli
