#pragma once
/**
 * @file plh_hub_fixture.h
 * @brief Shared fixture + helpers for plh_hub L4 CLI tests.
 *
 * Mirrors `tests/test_layer4_plh_role/plh_role_fixture.h` shape:
 * each test spawns the staged `plh_hub` binary as a subprocess, exercises
 * one CLI mode (--init / --validate / --keygen / error paths), and
 * inspects exit code + stdout/stderr + filesystem side effects.
 *
 * Run-mode tests (lifecycle loop + SIGTERM + broker coordination) live
 * elsewhere — see docs/todo/MESSAGEHUB_TODO.md "System-level L4 tests"
 * — because they need a live hub-role pipeline; the no-hub tier is
 * what this directory covers.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pylabhub::tests::plh_hub_l4
{

namespace fs = std::filesystem;

/// Path to the staged plh_hub binary, resolved relative to the test
/// executable via `g_self_exe_path` (set by the test framework at startup).
/// Build layout: `tests/` and `bin/` are sibling directories under the
/// stage root, so `<self>/../bin/plh_hub` reaches the hub binary.
inline std::string plh_hub_binary()
{
    return (fs::path(::g_self_exe_path).parent_path()
            / ".." / "bin" / "plh_hub").string();
}

/// Create a uniquely-named temp directory for a single test invocation.
/// The @p prefix appears in the directory name for grep-ability across
/// concurrent test runs.  Caller is responsible for cleanup (the
/// fixture's TearDown handles it).
inline fs::path make_tmp_dir(const std::string &prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);

    // Mirror of plh_role_fixture.h: rooted under
    // <build-stage>/test_artifacts/plh_hub_l4/ instead of /tmp so the
    // rotating log file under <dir>/logs/ survives test failure /
    // ctest re-invocation / tmpfs cleanup (HEP-CORE-0004 §"Shutdown
    // observability" + #258 e2e investigation).  Override with
    // PLH_TEST_ARTIFACTS_DIR for CI lanes that want an archive volume.
    fs::path root;
    if (const char *override_root = std::getenv("PLH_TEST_ARTIFACTS_DIR"))
    {
        root = override_root;
    }
    else
    {
        // fs::absolute is REQUIRED here — g_self_exe_path is argv[0]
        // which is relative when ctest invokes the binary as
        // `./build/stage-debug/tests/test_layer4_plh_hub`.  The path
        // ends up serialized into role/hub config JSON
        // (`out_hub_dir`, `keyfile`, etc.) and read back by
        // subprocesses with different CWDs, so it MUST be absolute or
        // the subprocess resolves "./build/..." against its own CWD
        // and creates path-inside-path (the original #258 self-
        // inflicted bug exposed when the test_artifacts dir was
        // migrated off /tmp in commit 25bed024).
        root = fs::absolute(
                   fs::path(::g_self_exe_path).parent_path().parent_path())
             / "test_artifacts" / "plh_hub_l4";
    }
    fs::path dir = root / ("plh_hub_l4_" + prefix + "_"
                            + std::to_string(::getpid()) + "_"
                            + std::to_string(id));
    std::error_code ec;
    fs::remove_all(dir, ec);   // best-effort: previous aborted run
    fs::create_directories(dir);

    // Scoreboard append: the fixture's TearDown() preserves paths on
    // failure via HasFailure() + a paths_to_clean_ walk, but if the
    // test process CRASHES before TearDown runs (SIGABRT, static-init
    // fault, uncaught exception in a worker dtor, kill by parent
    // ctest), that path preservation never fires — and the evidence
    // is lost.  We defend against that by appending the path to a
    // shared scoreboard file HERE at allocation time.  TearDown()
    // removes the specific entry on PASS.  A subsequent startup sweep
    // preserves orphans (they belong to a prior crashed run).  Best-
    // effort I/O (file lock contention is negligible for L4 tests,
    // which are serial per binary; parallel `-j N` still writes to
    // distinct PIDs so O_APPEND avoids interleaving on POSIX).
    try
    {
        fs::path scoreboard = root / ".pending_paths";
        std::ofstream f(scoreboard, std::ios::app);
        if (f)
        {
            f << dir.string() << "\n";
        }
    }
    catch (...) { /* scoreboard is best-effort; never abort the test */ }

    return dir;
}

/// Write @p content to @p path, creating parent directories as needed.
inline void write_file(const fs::path &path, const std::string &content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    f << content;
}

/// Read a JSON file at @p path; returns parsed value (no exceptions).
/// gtest-fatal on open failure so the calling test surfaces the issue
/// cleanly.
inline nlohmann::json read_json(const fs::path &path)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        ADD_FAILURE() << "failed to open " << path << " for reading";
        return {};
    }
    return nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
}

/// Canonical minimal Python script — defines `on_init` / `on_stop` as
/// no-ops.  Same shape as the role-side fixture's helper.  The hub
/// engine's `load_script` only requires the entry-point file to be
/// importable; an empty body would also work, but defining the
/// lifecycle pair keeps the script consistent with what an operator
/// sees in `--init` output.
inline std::string minimal_python_script()
{
    return
        "def on_init(api):\n"
        "    pass\n"
        "\n"
        "def on_stop(api):\n"
        "    pass\n";
}

/// Write the canonical script at `<hub_dir>/script/python/__init__.py`.
inline void write_minimal_script(const fs::path &hub_dir)
{
    write_file(hub_dir / "script" / "python" / "__init__.py",
               minimal_python_script());
}

/// Build a minimal-valid hub.json at @p cfg_path.
///
/// What "minimal valid" covers:
///   - hub.identity: uid + name + auth.keyfile (placeholder path)
///   - admin: enabled, ephemeral loopback endpoint (token is mandatory)
///   - broker: heartbeat-multiplier defaults
///   - federation/state: reasonable defaults
///   - script: type=python, path=base_dir
///   - logging + loop_timing
///
/// auth.keyfile is REQUIRED (HEP-CORE-0033 §7.1) and must be
/// non-empty; this helper writes a placeholder path that
/// config-load itself does not touch (parsing reads the string;
/// only `--keygen` writes to the path and only run-time
/// `HubConfig::load_keypair()` reads the contents).  Under the
/// gatekeeper/clearance model (HEP-CORE-0033 §6.5, finalized
/// 2026-06-04), `--validate` and run BOTH unlock the vault — tests
/// that exercise either must pre-keygen explicitly via
/// `keygen_minimal_hub` below.  Admin endpoint uses
/// `tcp://127.0.0.1:0` so each test gets an ephemeral port; broker
/// endpoint similarly.
inline void write_minimal_config(const fs::path &cfg_path,
                                  const fs::path &base_dir,
                                  const nlohmann::json &overrides =
                                      nlohmann::json::object())
{
    nlohmann::json j;

    j["hub"]["uid"]        = "hub.l4test.uid00000001";
    j["hub"]["name"]       = "L4Test";
    j["hub"]["log_level"]  = "info";
    j["hub"]["auth"]["keyfile"] = "vault/placeholder.vault";

    // admin.admin_token is RUNTIME-ONLY (populated from the vault by
    // HubConfig::load_keypair); never written to hub.json.  Only the
    // two operator-tunable fields go into the JSON.
    j["admin"]["enabled"]        = true;
    j["admin"]["endpoint"]       = "tcp://127.0.0.1:0";

    j["broker"]["heartbeat_interval_ms"]    = 500;
    j["broker"]["ready_miss_heartbeats"]    = 10;
    j["broker"]["pending_miss_heartbeats"]  = 10;

    j["federation"]["enabled"]           = false;
    j["federation"]["forward_timeout_ms"] = 2000;
    j["federation"]["peers"]             = nlohmann::json::array();

    j["network"]["broker_bind"]      = true;
    j["network"]["broker_endpoint"]  = "tcp://127.0.0.1:0";
    j["network"]["zmq_io_threads"]   = 1;

    j["state"]["disconnected_grace_ms"]    = 60000;
    j["state"]["max_disconnected_entries"] = 1000;

    j["logging"]["backups"]      = 5;
    j["logging"]["file_path"]    = "";
    j["logging"]["max_size_mb"]  = 10;
    j["logging"]["timestamped"]  = true;

    j["script"]["type"]      = "python";
    j["script"]["path"]      = base_dir.generic_string();
    j["python_venv"]         = "";
    j["loop_timing"]         = "fixed_rate";
    j["target_period_ms"]    = 1000;
    j["stop_on_script_error"] = false;

    if (!overrides.is_null())
        j.merge_patch(overrides);
    write_file(cfg_path, j.dump(2));
}

/// Base fixture for plh_hub CLI tests.  Extends IsolatedProcessTest so
/// subprocess paths_to_clean_ teardown happens automatically.
class PlhHubCliTest : public pylabhub::tests::IsolatedProcessTest
{
protected:
    void TearDown() override
    {
        // Mirror of plh_role_fixture: preserve dirs on failure
        // (forensics for L4 e2e investigation, #258) OR when
        // PLH_KEEP_TEST_ARTIFACTS is set.  Otherwise clean up.
        const bool keep =
            ::testing::Test::HasFailure() ||
            (std::getenv("PLH_KEEP_TEST_ARTIFACTS") != nullptr);
        if (keep)
        {
            for (const auto &p : paths_to_clean_)
            {
                fmt::print(stderr,
                            "[PLH_TEST_ARTIFACTS] preserved on failure: {}\n",
                            p.string());
            }
        }
        else
        {
            for (const auto &p : paths_to_clean_)
            {
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        }
        paths_to_clean_.clear();
    }

    /// Allocate a temp dir and register it for cleanup at TearDown.
    fs::path tmp(const std::string &prefix)
    {
        auto p = make_tmp_dir(prefix);
        paths_to_clean_.push_back(p);
        return p;
    }

    std::vector<fs::path> paths_to_clean_;
};

/// RAII helper for PYLABHUB_HUB_PASSWORD (mirrors the local copy in
/// test_plh_hub_keygen.cpp).  Tests that exercise the vault unlock
/// path declare one of these at the top of the test body.
struct ScopedHubPassword
{
    explicit ScopedHubPassword(const std::string &pw)
    {
        ::setenv("PYLABHUB_HUB_PASSWORD", pw.c_str(), /*overwrite=*/1);
    }
    ~ScopedHubPassword() { ::unsetenv("PYLABHUB_HUB_PASSWORD"); }
};

/// HEP-CORE-0035 §2 + HEP-CORE-0033 §6.5 gatekeeper helper.
/// Under the unconditional-CURVE contract (finalized 2026-06-04),
/// `--validate` and `run` are clearance / production verbs that
/// both require the vault file to already exist (the "gatekeeper"
/// boundary at `--keygen`).  Tests that exercise validate or run
/// must mint the vault first.  This helper runs `plh_hub --config
/// <cfg_path> --keygen` and asserts success.
///
/// Caller MUST hold a live `ScopedHubPassword` (or otherwise have
/// `PYLABHUB_HUB_PASSWORD` exported) before invoking — the spawned
/// `plh_hub --keygen` reads it from the environment.
///
/// **Outcome check (added 2026-06-16 per task #243 precedent).**  rc
/// alone is unreliable: a rc=143 (SIGTERM at wait_for_exit deadline)
/// looks identical to "keygen failed" from the parent's view, but the
/// vault file may have been written successfully and only the process
/// teardown hung (see task #242 for the Logger-shutdown detach pattern
/// that produces exactly this).  We resolve the ambiguity by checking
/// the vault path on disk after wait_for_exit returns, and distinguish
/// three diagnostic cases:
///   1. rc=0           → success
///   2. rc!=0 + vault present → keygen produced the artifact but
///      teardown hung; surface as a #242-style failure
///   3. rc!=0 + vault absent  → real keygen failure
///
/// Cost: one Argon2id INTERACTIVE derivation (~100ms locally).
inline void keygen_minimal_hub(const std::filesystem::path &cfg_path)
{
    pylabhub::tests::helper::WorkerProcess kg(
        plh_hub_binary(), "--config",
        {cfg_path.string(), "--keygen"});
    const int rc = kg.wait_for_exit();

    if (rc == 0)
        return;

    // Resolve the vault path the keygen was supposed to produce.
    // hub.auth.keyfile is stored relative to hub_dir (= cfg_path's
    // parent dir, per write_minimal_config above).  If the cfg cannot
    // be parsed here, we degrade gracefully — the rc!=0 failure still
    // gets reported, just without the "did the vault land on disk"
    // breakdown.
    fs::path vault_abs;
    bool     vault_exists = false;
    {
        std::ifstream f(cfg_path);
        if (f.is_open())
        {
            auto j = nlohmann::json::parse(f, nullptr, /*allow_exc=*/false);
            if (!j.is_discarded()
                && j.contains("hub")
                && j["hub"].contains("auth")
                && j["hub"]["auth"].contains("keyfile"))
            {
                const std::string kf =
                    j["hub"]["auth"]["keyfile"].get<std::string>();
                if (!kf.empty())
                {
                    fs::path kf_path = kf;
                    vault_abs = kf_path.is_absolute()
                              ? kf_path
                              : cfg_path.parent_path() / kf_path;
                    // Existence AND non-zero size: a 0-byte file at the
                    // vault path (partial-write failure) would otherwise
                    // read as "vault present" and get misclassified as a
                    // #242 teardown hang.  Real vaults are hundreds of
                    // bytes minimum (HEP-CORE-0035 §4.6 vault layout).
                    std::error_code ec;
                    vault_exists = fs::exists(vault_abs, ec)
                                && fs::file_size(vault_abs, ec) > 0;
                }
            }
        }
    }

    if (vault_exists)
    {
        ADD_FAILURE()
            << "keygen_minimal_hub: plh_hub --keygen produced the vault "
            << "at '" << vault_abs << "' but the process did NOT exit "
            << "cleanly within wait_for_exit deadline (rc=" << rc
            << ").  This matches the Logger-shutdown detach hang pattern "
            << "tracked as task #242 — keygen WORK SUCCEEDED, only the "
            << "process teardown hung.  Treat as #242 follow-up, not a "
            << "regression in --keygen logic.  cfg='" << cfg_path
            << "'; stderr:\n" << kg.get_stderr();
    }
    else
    {
        ADD_FAILURE()
            << "keygen_minimal_hub: plh_hub --keygen FAILED to produce "
            << "the vault (rc=" << rc << ", vault absent at '"
            << vault_abs << "').  Real keygen-path failure — investigate "
            << "the stderr below.  cfg='" << cfg_path << "'; stderr:\n"
            << kg.get_stderr();
    }
}

/// Class D gate — happy-path tests must FAIL on stray ERROR-level log
/// lines in the binary's stderr.  Mirrors the role-side check.  Error-
/// path tests do NOT call this — their contract is the diagnostic
/// substring pinned via Class A path-discrimination.
inline void expect_no_unexpected_errors(
    const pylabhub::tests::helper::WorkerProcess &p)
{
    const std::string &err = p.get_stderr();
    std::istringstream lines(err);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.find("[ERROR ]") != std::string::npos)
            ADD_FAILURE() << "Unexpected ERROR-level log in plh_hub "
                             "stderr (Class D gate):\n  " << line;
    }
}

} // namespace pylabhub::tests::plh_hub_l4
