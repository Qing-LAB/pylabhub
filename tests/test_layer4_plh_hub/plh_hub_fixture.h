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

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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
    fs::path dir = fs::temp_directory_path()
                 / ("plh_hub_l4_" + prefix + "_"
                    + std::to_string(::getpid()) + "_"
                    + std::to_string(id));
    std::error_code ec;
    fs::remove_all(dir, ec);   // best-effort: previous aborted run
    fs::create_directories(dir);
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
///   - hub.identity: uid + name + auth.keyfile=""
///   - admin: enabled, ephemeral loopback endpoint, token_required=false
///   - broker: heartbeat-multiplier defaults
///   - federation/state: reasonable defaults
///   - script: type=python, path=base_dir
///   - logging + loop_timing
///
/// `auth.keyfile=""` keeps --validate non-interactive (no vault
/// prompt).  Admin endpoint uses `tcp://127.0.0.1:0` so each test gets
/// an ephemeral port; broker endpoint similarly.  Operators get
/// fully-fledged values via `plh_hub --init` — this helper is the
/// minimum the hub.json schema accepts.
inline void write_minimal_config(const fs::path &cfg_path,
                                  const fs::path &base_dir,
                                  const nlohmann::json &overrides =
                                      nlohmann::json::object())
{
    nlohmann::json j;

    j["hub"]["uid"]        = "hub.l4test.uid00000001";
    j["hub"]["name"]       = "L4Test";
    j["hub"]["log_level"]  = "info";
    j["hub"]["auth"]["keyfile"] = "";

    // admin.admin_token is RUNTIME-ONLY (populated from the vault by
    // HubConfig::load_keypair); never written to hub.json.  Only the
    // three operator-tunable fields go into the JSON.
    j["admin"]["enabled"]        = true;
    j["admin"]["endpoint"]       = "tcp://127.0.0.1:0";
    j["admin"]["token_required"] = false;

    j["broker"]["heartbeat_interval_ms"]    = 500;
    j["broker"]["ready_miss_heartbeats"]    = 10;
    j["broker"]["pending_miss_heartbeats"]  = 10;
    j["broker"]["grace_heartbeats"]         = 4;

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
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
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
