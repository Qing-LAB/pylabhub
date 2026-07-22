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

#include "utils/naming.hpp"
#include "utils/uid_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#if defined(PYLABHUB_IS_POSIX) || defined(__unix__) || defined(__APPLE__)
#ifndef PYLABHUB_IS_POSIX
#define PYLABHUB_IS_POSIX 1
#endif
#include <unistd.h> // isatty, STDIN_FILENO, getpass
#elif defined(_WIN32)
#include <io.h>      // _isatty, _fileno
#include <windows.h> // SetConsoleMode, GetConsoleMode, ENABLE_ECHO_INPUT
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
    HANDLE hStdin = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    const bool is_con =
        (hStdin != INVALID_HANDLE_VALUE) && (::GetConsoleMode(hStdin, &old_mode) != 0);
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
inline std::optional<std::string> get_password(const char *name, const char *env_var,
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
inline std::optional<std::string> get_new_password(const char *name, const char *env_var,
                                                   const char *prompt, const char *confirm_prompt)
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

    const std::string pw = read_password_interactive(name, prompt);
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
inline std::optional<std::string> resolve_init_name(const std::string &cli_name, const char *prompt)
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

// ============================================================================
// Required-UID resolution (HEP-CORE-0033 §6.5 — gatekeeper/clearance)
// ============================================================================

/**
 * @brief Resolve a required UID via the canonical source-priority chain.
 *
 * Implements the contract from HEP-CORE-0033 §6.5 "Validator wiring":
 *   1. CLI flag value (`cli_value`) — wins if non-empty.
 *   2. Environment variable (`env_var`) — read next; if non-empty and
 *      passes `is_valid_identifier(value, kind)`, returned.
 *   3. Interactive prompt — only when stdin is a TTY AND `no_prompt` is
 *      false.  An inline default `[default: <auto-gen>]` is shown; ENTER
 *      accepts the suggestion (generated via the matching `uid::generate_*_uid(name)`
 *      helper).  Validates each typed answer; re-prompts on failure (up
 *      to 3 attempts).
 *   4. Otherwise — error to stderr and return nullopt.
 *
 * @param side       Subject label for diagnostics + name-based auto-gen:
 *                   "hub" / "prod" / "cons" / "proc".
 * @param env_var    Environment-variable name to consult (e.g.
 *                   "PYLABHUB_HUB_UID", "PYLABHUB_ROLE_UID").
 * @param prompt     Prompt text for the TTY path (e.g. "Hub uid: ").
 * @param kind       Identifier kind for `is_valid_identifier` validation:
 *                   `IdentifierKind::PeerUid` (hub) or `::RoleUid` (role).
 * @param cli_value  Value passed via the matching CLI flag (e.g. `--uid X`);
 *                   pass empty when absent.
 * @param name_hint  Display name used as the basis for the auto-gen
 *                   suggestion shown inline at the prompt; pass empty
 *                   to suppress the suggestion (operator must type a
 *                   value).
 * @param no_prompt  When true, suppress the TTY prompt path even if stdin
 *                   is a TTY (matches `--no-prompt` CLI flag).
 *
 * Same validator (`is_valid_identifier`) runs regardless of source — no
 * parallel format-checking code is permitted in the CLI layer.  Returns
 * nullopt on unresolvable error; diagnostic already on stderr.
 */
inline std::optional<std::string> get_required_uid(std::string_view side, std::string_view env_var,
                                                   std::string_view prompt,
                                                   ::pylabhub::hub::IdentifierKind kind,
                                                   std::string_view cli_value,
                                                   std::string_view name_hint, bool no_prompt)
{
    // The diagnostic format is shared across every source so operator
    // errors look uniform regardless of how the value arrived.
    auto invalid_msg = [&](std::string_view value) -> std::string
    {
        const char *grammar = (kind == ::pylabhub::hub::IdentifierKind::PeerUid)
                                  ? "PeerUid 'hub.<name>.uid<8hex>'"
                                  : "RoleUid '<tag>.<name>.uid<8hex>'";
        std::string msg;
        msg.reserve(160);
        msg.append("invalid ").append(side).append(" uid '");
        msg.append(value).append("': must match HEP-CORE-0033 §G2.2.0b ");
        msg.append(grammar);
        return msg;
    };

    // Source 1: explicit CLI flag.
    if (!cli_value.empty())
    {
        const std::string v(cli_value);
        if (::pylabhub::hub::is_valid_identifier(v, kind))
            return v;
        std::fprintf(stderr, "Error: %s\n", invalid_msg(v).c_str());
        return std::nullopt;
    }

    // Source 2: environment variable.
    if (!env_var.empty())
    {
        if (const char *env = std::getenv(std::string(env_var).c_str()))
        {
            const std::string v(env);
            if (!v.empty())
            {
                if (::pylabhub::hub::is_valid_identifier(v, kind))
                    return v;
                std::fprintf(stderr, "Error: %s (from %.*s)\n", invalid_msg(v).c_str(),
                             static_cast<int>(env_var.size()), env_var.data());
                return std::nullopt;
            }
        }
    }

    // Source 3: interactive TTY prompt — only when stdin is a TTY AND
    // the caller did not pass --no-prompt.  Re-prompt on validation
    // failure; cap retries to avoid unbounded loops on a pipe that
    // pretends to be a TTY.
    if (!no_prompt && is_stdin_tty())
    {
        // Pre-compute the auto-gen suggestion ONCE so the prompt's
        // visible default stays stable across re-prompts (operator
        // can scroll up and see the same value they're typing over).
        std::string suggestion;
        if (!name_hint.empty())
        {
            const std::string nm(name_hint);
            if (kind == ::pylabhub::hub::IdentifierKind::PeerUid)
                suggestion = ::pylabhub::uid::generate_hub_uid(nm);
            else if (side == "prod")
                suggestion = ::pylabhub::uid::generate_producer_uid(nm);
            else if (side == "cons")
                suggestion = ::pylabhub::uid::generate_consumer_uid(nm);
            else if (side == "proc")
                suggestion = ::pylabhub::uid::generate_processor_uid(nm);
        }

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            std::cout << prompt;
            if (!suggestion.empty())
                std::cout << "[default: " << suggestion << "] ";
            std::string line;
            if (!std::getline(std::cin, line))
                break; // EOF — fall through to error path
            // ENTER on empty input accepts the suggestion when one is offered.
            if (line.empty())
            {
                if (!suggestion.empty())
                    return suggestion;
                std::fprintf(stderr,
                             "Error: %s uid required (no default available; "
                             "supply --name first or type a uid)\n",
                             std::string(side).c_str());
                continue;
            }
            if (::pylabhub::hub::is_valid_identifier(line, kind))
                return line;
            std::fprintf(stderr, "  %s; try again.\n", invalid_msg(line).c_str());
        }
        std::fprintf(stderr, "Error: %s uid not provided after 3 attempts\n",
                     std::string(side).c_str());
        return std::nullopt;
    }

    // Source 4 (terminal): no source succeeded.
    std::fprintf(stderr,
                 "Error: %s uid required; supply via --uid <uid>, %.*s "
                 "environment variable, or attach a TTY for interactive prompt\n",
                 std::string(side).c_str(), static_cast<int>(env_var.size()), env_var.data());
    return std::nullopt;
}

} // namespace pylabhub::cli
