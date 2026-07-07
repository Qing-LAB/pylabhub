/**
 * @file plh_role_main.cpp
 * @brief plh_role — unified role binary that dispatches on --role tag.
 *
 * Replaces the three per-role executables (pylabhub-producer, -consumer,
 * -processor) with a single binary whose role is selected at runtime.
 *
 * Dispatch uses a static map from long-form role tag to a pair of
 * registration functions (init-time + runtime). **Only the role matched
 * by --role gets registered** — unused roles never touch RoleDirectory /
 * RoleRegistry, in line with HEP-0024 §12 (CLI↔Config separation).
 *
 * ## Usage
 *
 *     plh_role --init --role producer [<role_dir>] [--name <n>] \
 *                       [--log-maxsize <MB>] [--log-backups <N>]
 *     plh_role --role producer <role_dir>                   # Run from directory
 *     plh_role --role producer --config <path> [--validate | --keygen]
 *
 * ## Role types (registered on demand)
 *
 *   - producer
 *   - consumer
 *   - processor
 *
 * Custom roles: write a third-party `register_<role>_init()` +
 * `register_<role>_runtime()` pair and extend `kRegistrars` in your fork.
 */

#include "producer_init.hpp"       // src/producer/ (via CMake PRIVATE include)
#include "consumer_init.hpp"       // src/consumer/
#include "processor_init.hpp"      // src/processor/

// engine_factory.hpp removed — engine is constructed in host's
// worker_main_ Step 0 via the forward-declared scripting::create_engine
// (see HEP-CORE-0011 §"Engine Construction Lifecycle").  main() no
// longer constructs engines.
#include "utils/cli_helpers.hpp"
#include "utils/config/role_config.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/role_cli.hpp"
#include "utils/role_directory.hpp"
#include "utils/engine_host.hpp"
#include "utils/role_main_helpers.hpp"
#include "utils/role_registry.hpp"
#include "utils/script_engine_factory.hpp"  // scripting::init_scripting / ensure_python
#include "utils/security/key_store.hpp"               // HEP-CORE-0040 §172
#include "utils/security/secure_subsystem.hpp" // HEP-CORE-0040 §4
#include "utils/security/zap_router.hpp"              // ZapPumpThread (AUTH-2 / #162)
#include "utils/thread_manager.hpp"  // process_detached_count for exit code
#include "../scripting/python_interpreter_module.hpp"  // ensure_python_interpreter_loaded

#include "plh_datahub.hpp"   // LifecycleGuard + hub/utils prelude
#include "plh_version_registry.hpp"  // HEP-CORE-0032 ABI check

#include <atomic>
#include <chrono>
#include <cstdio>      // fprintf in check_abi failure branch
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

using namespace pylabhub::utils;
namespace scripting = pylabhub::scripting;
namespace cli       = pylabhub::cli;
namespace role_cli  = pylabhub::role_cli;

// ── Dispatch map: role tag → (init registrar, runtime registrar) ─────────────

namespace
{

using RegistrarPair = std::pair<void (*)(), void (*)()>;

const std::unordered_map<std::string_view, RegistrarPair> &kRegistrars()
{
    // NOTE on lazy init: static local map built at first call so
    // initialization order with other static objects is deterministic.
    static const std::unordered_map<std::string_view, RegistrarPair> m = {
        {"producer",
         {&pylabhub::producer::register_producer_init,
          &pylabhub::producer::register_producer_runtime}},
        {"consumer",
         {&pylabhub::consumer::register_consumer_init,
          &pylabhub::consumer::register_consumer_runtime}},
        {"processor",
         {&pylabhub::processor::register_processor_init,
          &pylabhub::processor::register_processor_runtime}},
    };
    return m;
}

void print_available_roles(std::ostream &os)
{
    os << "Available roles: ";
    bool first = true;
    for (const auto &[tag, _] : kRegistrars())
    {
        if (!first) os << ", ";
        os << tag;
        first = false;
    }
    os << "\n";
}

/// Resolve + register the role named by @p args.role. Returns the
/// pointer to the RoleRuntimeInfo on success, nullptr on failure
/// (prints diagnostic to stderr).
const RoleRuntimeInfo *register_and_lookup(const role_cli::RoleArgs &args)
{
    if (args.role.empty())
    {
        std::cerr << "Error: --role <tag> is required (plh_role has no default role).\n";
        print_available_roles(std::cerr);
        return nullptr;
    }

    const auto &map = kRegistrars();
    auto it = map.find(args.role);
    if (it == map.end())
    {
        std::cerr << "Error: unknown role '" << args.role << "'.\n";
        print_available_roles(std::cerr);
        return nullptr;
    }

    // Register BOTH the init content (for --init subcommand) and the
    // runtime content (for run/validate/keygen). Cheap; harmless if
    // only one is actually needed.
    (it->second.first)();   // register_<role>_init
    (it->second.second)();  // register_<role>_runtime

    const RoleRuntimeInfo *info = RoleRegistry::get_runtime(args.role);
    if (!info)
    {
        // Would only happen if register_<role>_runtime misbehaved.
        std::cerr << "Internal error: runtime registration failed for '"
                  << args.role << "'.\n";
        return nullptr;
    }
    return info;
}

// ── --init flow ──────────────────────────────────────────────────────────────

int do_init(const role_cli::RoleArgs &args)
{
    namespace fs = std::filesystem;
    const fs::path dir = args.role_dir.empty()
                             ? fs::current_path()
                             : fs::path(args.role_dir);

    const auto name_opt = cli::resolve_init_name(
        args.init_name,
        "Role name (human-readable, e.g. 'TempSensor'): ");
    if (!name_opt)
        return 1;

    RoleDirectory::LogInitOverrides log_overrides;
    log_overrides.max_size_mb = args.log_max_size_mb;
    log_overrides.backups     = args.log_backups;

    // args.role here is the long form and matches the RoleDirectory key
    // registered by register_<role>_init().
    return RoleDirectory::init_directory(
        dir, args.role, *name_opt, log_overrides);
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // ── ABI compatibility check (HEP-CORE-0032) ───────────────────────
    // Runs BEFORE LifecycleGuard, signal handler, and any library call
    // that could exercise a mismatched vtable or protocol field.  If
    // the caller binary was compiled against an older pylabhub-utils
    // header than the one actually linked at runtime (the bug class
    // that caused the 2026-04-21 ProcessorCliTest SIGSEGV), this check
    // refuses to run with an actionable diagnostic.  In Debug or with
    // PYLABHUB_STRICT_ABI_CHECK=ON, the build_id comparison also
    // catches silent "stale binary + fresh library at same declared
    // version" cases the per-axis check cannot see.
    {
        constexpr auto kAbiExpected = pylabhub::version::abi_expected_here();
        const auto abi = pylabhub::version::check_abi(
            kAbiExpected.versions, kAbiExpected.build_id);
        if (!abi.compatible)
        {
            std::fprintf(stderr,
                "[plh_role] ABI mismatch — refusing to run.\n"
                "  %s\n"
                "  Rebuild this binary against the installed library, "
                "or reinstall a matching library.\n",
                abi.message.c_str());
            return 2;
        }
    }

    std::atomic<bool> g_shutdown{false};

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "plh_role"}, &g_shutdown);
    signal_handler.install();

    // Register the dispatching ScriptEngine factory with the utils-side
    // plugin registry (HEP-CORE-0011 §"Engine Construction Lifecycle").
    // Must run BEFORE any role host is constructed — host worker_main_
    // Step 0 calls `scripting::create_engine` which returns nullptr if
    // no factory has been registered.  Idempotent.
    pylabhub::scripting::init_scripting();

    // Parse args. The role_name passed to parse_role_args is only used
    // in the usage text ("<role_dir>"); the real selector is args.role.
    auto parsed = role_cli::parse_role_args(argc, argv, "role");
    if (parsed.exit_code >= 0)
        return parsed.exit_code;
    const role_cli::RoleArgs &args = parsed.args;

    const RoleRuntimeInfo *info = register_and_lookup(args);
    if (!info)
        return 1;

    // ── Lifecycle init (covers --init / --keygen / --validate / run) ──
    // Hoisted above the --init dispatch so every mode shares the same
    // LOGGER_* / Logger-rotation / Crypto-init semantics.  Previously
    // --init exited BEFORE LifecycleGuard, which made LOGGER_* a silent
    // no-op in that path and broke the L4 Class D gate (audit
    // 2026-05-02): a regression that emitted LOGGER_WARN from
    // RoleDirectory::init_directory would have gone undetected because
    // Logger's worker thread was never started.  Uniform lifecycle
    // means uniform error-emission guarantees.
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules());
    scripting::register_signal_handler_lifecycle(signal_handler, "[plh_role]");
    scripting::log_version_info("[plh_role]");

    // ── Init mode ─────────────────────────────────────────────────────
    // HEP-CORE-0024 §3.4.2: mirrors HEP-CORE-0033 §6.5 — `--skeleton`
    // is the layout-only verb; the manual path equivalent of `--init`
    // (current implementation is template-write only, which matches
    // the skeleton semantic).  Both flags dispatch to the same writer;
    // the one-shot `--init` bundling is documented but lands in a
    // follow-up commit.
    if (args.init_only || args.skeleton_only)
        return do_init(args);

    // ── Load config ───────────────────────────────────────────────────
    std::optional<pylabhub::config::RoleConfig> config;
    try
    {
        if (!args.role_dir.empty())
            config.emplace(pylabhub::config::RoleConfig::load_from_directory(
                args.role_dir, info->role_type.c_str(), info->config_parser));
        else
            config.emplace(pylabhub::config::RoleConfig::load(
                args.config_path, info->role_type.c_str(), info->config_parser));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }
    auto &c = *config;
    const std::string config_dir = c.base_dir().string();

    // ── Attach rotating log sink (path auto-composed from config) ─────
    {
        std::error_code ec;
        scripting::configure_logger_from_config(c, ec, "[plh_role]");
    }

    // ── Keygen mode ───────────────────────────────────────────────────
    if (args.keygen_only)
    {
        if (c.auth().keyfile.empty())
        {
            std::cerr << "Error: --keygen requires '" << info->role_type
                      << ".auth.keyfile' in config\n";
            return 1;
        }

        const auto pw_opt = cli::get_new_password(
            info->role_type.c_str(),
            "PYLABHUB_ROLE_PASSWORD",
            "Role vault password (empty = no encryption): ",
            "Confirm password: ");
        if (!pw_opt)
            return 1;

        try
        {
            const auto pubkey = c.create_keypair(*pw_opt);
            std::cout << info->role_label << " vault written to: "
                      << c.auth().keyfile << "\n"
                      << "  role_uid   : " << c.identity().uid << "\n"
                      << "  public_key : " << pubkey << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── KeyStore (HEP-CORE-0040 §5) ────────────────────────────────────
    // SecureSubsystem is brought up by the LifecycleGuard's mod pack
    // (`role_lifecycle_modules()`) — SEC-Fold-2 Phase A/B landing.
    // KeyStore's dep on "SecureSubsystem" resolves against the static
    // registration in that mod pack.
    namespace sec = pylabhub::utils::security;

    // ── ZapPumpThread lifecycle module (AUTH-2 / #162) ─────────────────
    // HEP-CORE-0036 §7.1 + §7.4 — every CURVE-server socket in this
    // process needs the singleton ZAP REP socket pumped.  Roles use
    // the dedicated `ZapPumpThread` lifecycle module (which depends
    // on `ZapRouter`) because the role process has multiple BRC
    // poll loops on dual-hub processors; running pump_one on any one
    // BRC would race the others on the single REP socket (single-
    // pumper invariant PANIC).  The lifecycle-managed jthread is
    // process-singleton by construction; multi-BRC processors are
    // served by the one thread for free.
    //
    // `plh_hub_main` deliberately does NOT load this module — the
    // broker has its own integrated `pump_one(0ms)` call in its main
    // poll loop (`broker_service.cpp`), and loading both would PANIC.
    sec::ZapPumpThread::ensure_registered_and_loaded();

    // ── Auth: unlock vault (run AND --validate) ────────────────────────
    // HEP-CORE-0035 §2 (gatekeeper / clearance model) +
    // HEP-CORE-0024 §3.4.2 (revised 2026-06-04, mirrors HEP-CORE-0033
    // §6.5): --validate is the clearance check on a provisioned role
    // home; both run and --validate go through the same load_keypair
    // path.  RoleHost::set_validate_only(true) below gates broker
    // connection (a separate, non-CURVE concern), but vault unlock
    // happens here unconditionally — a role without a vault is not a
    // runnable role.
    if (!c.auth().keyfile.empty())
    {
        const auto vault_password = cli::get_password(
            info->role_type.c_str(),
            "PYLABHUB_ROLE_PASSWORD",
            "Role vault password: ");
        if (!vault_password)
            return 1;
        try
        {
            c.load_keypair(*vault_password);
        }
        catch (const std::exception &e)
        {
            // Symmetric with the hub-side path at plh_hub_main.cpp.
            // load_keypair throws on wrong password / corrupt vault /
            // missing file (HEP-CORE-0024 §3.4 "non-empty + file
            // absent" row); catching here gives the operator a
            // formatted diagnostic instead of std::terminate /
            // SIGABRT from the unhandled exception.
            std::cerr << "Vault unlock failed: " << e.what() << "\n";
            return 1;
        }
    }

    // ── PythonInterpreter conditional load (Option E final design) ────
    // HEP-CORE-0011 §"Engine Construction Lifecycle": main loads the
    // PythonInterpreter dynamic lifecycle module on THIS thread iff the
    // config selects a Python script.  pi_startup runs Py_InitializeFromConfig
    // on main and releases the GIL via a stored py::gil_scoped_release;
    // the role host's worker thread acquires it via PythonGilLease at
    // the top of worker_main_.  The module is registered as persistent —
    // it stays loaded until LifecycleGuard tears down at process exit,
    // where Phase 2 of finalize() runs Py_FinalizeEx synchronously on
    // THIS thread (same thread as Py_InitializeFromConfig).
    //
    // Native and Lua deployments skip this entirely; Py_Initialize cost
    // is paid only when actually needed.
    if (c.script().type == "python" || c.script().type.empty())
    {
        if (!pylabhub::scripting::ensure_python_interpreter_loaded())
        {
            std::cerr << "Failed to load PythonInterpreter — see logs.\n";
            return 1;
        }
    }

    // ── Host via registry factory ─────────────────────────────────────
    // Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    // engine is NOT constructed here.  The host's worker_main_ Step 0
    // constructs it via scripting::create_engine(config_.script()) on
    // the worker thread, after PythonGilLease acquires the GIL on that
    // thread.
    auto host = info->host_factory(std::move(*config), &g_shutdown);
    host->set_validate_only(args.validate_only);

    try { host->startup_(); }
    catch (const std::exception &e)
    {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }

    if (!host->script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        host->shutdown_();
        return 1;
    }

    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        host->shutdown_();
        return 0;
    }

    if (!host->is_running())
    {
        std::cerr << "Failed to start " << info->role_type
                  << " — loop did not start.\n";
        host->shutdown_();
        return 1;
    }

    // ── Status callback (generic: shows role + channel info from config) ──
    const auto start_time = std::chrono::steady_clock::now();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const auto &cfg = host->config();
        // Show input/output channels when set; cfg.in_channel() /
        // out_channel() return empty for sides the role doesn't use.
        std::string channels;
        if (!cfg.in_channel().empty())
            channels += fmt::format("  in_channel:  {}\n", cfg.in_channel());
        if (!cfg.out_channel().empty())
            channels += fmt::format("  out_channel: {}\n", cfg.out_channel());
        return fmt::format(
            "  pyLabHub {} (plh_role, role={}, script={})\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "{}"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            info->role_type, cfg.script().type,
            config_dir, cfg.identity().uid,
            channels,
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, *host, "[plh_role]");
    host->shutdown_();

    // HEP-CORE-0031 §4.2 — surface unclean teardown in the exit code.
    // process_detached_count() is monotonic across every ThreadManager
    // in this process; nonzero means at least one managed thread
    // exceeded its bounded-join timeout and had to be detached.  The
    // OS will reap the detached thread at process exit, but the
    // operator (init system, orchestration layer) should see this
    // surfaced rather than ignored.
    if (pylabhub::utils::ThreadManager::process_detached_count() > 0)
    {
        std::cerr << "[plh_role] WARNING: "
                  << pylabhub::utils::ThreadManager::process_detached_count()
                  << " thread(s) were detached during shutdown — see "
                     "earlier ERROR log(s) from ThreadManager for the "
                     "stuck thread name(s).\n";
        return 2;
    }
    return 0;
}
