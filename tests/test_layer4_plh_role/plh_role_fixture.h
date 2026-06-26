#pragma once
/**
 * @file plh_role_fixture.h
 * @brief Shared test fixture + helpers for plh_role L4 CLI tests.
 *
 * Scope (no-hub tier): each test spawns `plh_role` as a subprocess, exercises
 * a single CLI mode (--init / --validate / --keygen / error paths), and
 * inspects exit code + stdout/stderr + file-system side effects.
 *
 * Run-mode tests (lifecycle loop + SIGTERM + broker round-trip) deliberately
 * live elsewhere (see docs/todo/TESTING_TODO.md — deferred L4 broker tests)
 * because they need hubshell coordination.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

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

namespace pylabhub::tests::plh_role_l4
{

namespace fs = std::filesystem;

/// Path to the staged plh_role binary, resolved relative to the test
/// executable via `g_self_exe_path` (set by the test framework at startup).
///
/// Build layout: tests/ and bin/ are sibling directories under the stage
/// root, so `<self>/../bin/plh_role` reaches the unified role binary.
inline std::string plh_role_binary()
{
    // g_self_exe_path is a global in the root namespace (test_entrypoint.h).
    return (fs::path(::g_self_exe_path).parent_path()
            / ".." / "bin" / "plh_role").string();
}

/// Create a uniquely-named test scratch directory.
///
/// Default root is `<build-stage>/test_artifacts/plh_role_l4/` (sibling
/// of the test binary's bin/ dir), NOT `/tmp/`, so:
///   1. Logs survive across runs and ctest re-invocations.
///   2. tmpfs cleanup (which can race with concurrent tests in -j N
///      sweeps) can't yank the dir from under us.
///   3. Forensics from a hung / SIGTERMed binary stay inspectable —
///      especially the rotating-file log sink, which is the only
///      place stderr ends up once the role binary switches sinks.
///
/// Override the root with env var `PLH_TEST_ARTIFACTS_DIR=<path>` for
/// CI lanes that want to redirect outputs into an archive volume.
///
/// The @p prefix appears in the directory name for grep-ability.
/// Caller is responsible for cleanup; @c PlhRoleCliTest::TearDown
/// preserves dirs on test failure (forensics) and removes them on
/// success.
inline fs::path make_tmp_dir(const std::string &prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);

    fs::path root;
    if (const char *override_root = std::getenv("PLH_TEST_ARTIFACTS_DIR"))
    {
        root = override_root;
    }
    else
    {
        // Test binary lives at  <build-stage>/tests/test_layer4_plh_role
        // so <build-stage> == binary's parent's parent.
        root = fs::path(::g_self_exe_path).parent_path().parent_path()
             / "test_artifacts" / "plh_role_l4";
    }

    fs::path dir = root / ("plh_role_l4_" + prefix + "_"
                            + std::to_string(::getpid()) + "_"
                            + std::to_string(id));
    std::error_code ec;
    fs::remove_all(dir, ec);    // best-effort: previous aborted run
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

/// Read a JSON file at @p path; returns parsed value.  gtest-fatal on
/// open failure so the calling test aborts cleanly.
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

/// Build a minimal valid <role>.json config at @p cfg_path, pointing at
/// @p script_path as the script directory.  Each role gets the minimum
/// fields its parser requires; callers can override via the @p overrides
/// merge-in.
///
/// This is what --validate runs against — its pass/fail behavior is what
/// every validate test uses.
inline void write_minimal_config(const fs::path       &cfg_path,
                                 const std::string    &role,
                                 const fs::path       &script_path,
                                 const nlohmann::json &overrides =
                                     nlohmann::json::object())
{
    nlohmann::json j;

    // NOTE: The identity fields (uid, name, auth, log_level) live
    // INSIDE the role-tagged block (producer/consumer/processor),
    // not at top level.  auth.keyfile is REQUIRED (HEP-CORE-0024
    // §3.4) and must be non-empty; this helper writes a placeholder
    // path that config-load itself does not touch (parsing reads the
    // string; only `--keygen` writes to the path and only run-time
    // `RoleConfig::load_keypair()` reads the contents).  Under the
    // gatekeeper/clearance model (HEP-CORE-0024 §3.4.2 + HEP-CORE-0033
    // §6.5, finalized 2026-06-04), `--validate` and run BOTH unlock
    // the vault — tests that exercise either must pre-keygen
    // explicitly via `keygen_minimal_role` below.
    //
    // Build the JSON via explicit assignment (not nested initializer
    // lists) — nlohmann::json interprets `{"key", {...}}` in a nested
    // context as an ambiguous structure and may produce the wrong
    // shape.  Assignment is unambiguous.

    auto slot_schema = nlohmann::json::object();
    auto fields = nlohmann::json::array();
    fields.push_back(nlohmann::json{{"name", "v"}, {"type", "float32"}});
    slot_schema["packing"] = "aligned";  // HEP-CORE-0034 §6.2 — required
    slot_schema["fields"] = fields;

    j["script"]["type"] = "python";
    j["script"]["path"] = script_path.generic_string();
    // loop_timing is "max_rate" for all 3 roles intentionally — the
    // role parsers accept it for every role.  Init templates emit role-
    // specific defaults (producer=fixed_rate, cons/proc=max_rate); this
    // helper optimizes for "minimal-valid", not "canonical".
    j["loop_timing"]   = "max_rate";

    if (role == "producer")
    {
        j["producer"]["uid"]  = "prod.l4test.uid00000001";
        j["producer"]["name"] = "L4Test";
        j["producer"]["auth"]["keyfile"] = "vault/placeholder.vault";
        j["out_channel"]     = "lab.l4.test";
        j["out_slot_schema"] = slot_schema;
    }
    else if (role == "consumer")
    {
        j["consumer"]["uid"]  = "cons.l4test.uid00000001";
        j["consumer"]["name"] = "L4Test";
        j["consumer"]["auth"]["keyfile"] = "vault/placeholder.vault";
        j["in_channel"]     = "lab.l4.test";
        j["in_slot_schema"] = slot_schema;
    }
    else if (role == "processor")
    {
        j["processor"]["uid"]  = "proc.l4test.uid00000001";
        j["processor"]["name"] = "L4Test";
        j["processor"]["auth"]["keyfile"] = "vault/placeholder.vault";
        j["in_channel"]      = "lab.l4.in";
        j["out_channel"]     = "lab.l4.out";
        j["in_slot_schema"]  = slot_schema;
        j["out_slot_schema"] = slot_schema;
    }
    else
    {
        ADD_FAILURE() << "unknown role tag for test helper: " << role;
        return;
    }

    if (!overrides.is_null())
        j.merge_patch(overrides);
    write_file(cfg_path, j.dump(2));
}

/// Canonical minimal Python script — defines the callback required by
/// whatever role loads it.  Single file with every role's callback is
/// simpler than 3 per-role variants; unused callbacks are ignored by
/// the engine.
inline std::string minimal_python_script()
{
    return
        "def on_init(api):\n"
        "    pass\n"
        "\n"
        "def on_produce(tx, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_consume(rx, msgs, api):\n"
        "    return True\n"
        "\n"
        "def on_process(rx, tx, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_stop(api):\n"
        "    pass\n";
}

/// Write the canonical script at @p script_dir / "__init__.py".
/// Also creates @p script_dir itself + any missing parents.
inline void write_minimal_script(const fs::path &script_dir)
{
    write_file(script_dir / "__init__.py", minimal_python_script());
}

/// Base fixture for plh_role CLI tests.  Extends IsolatedProcessTest so
/// that paths_to_clean_ cleanup happens automatically.
class PlhRoleCliTest : public pylabhub::tests::IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        // Preserve scratch dirs on failure so the rotating-file log,
        // generated config files, vault file, etc. can be inspected
        // post-mortem.  See make_tmp_dir() docstring.
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

/// RAII helper for PYLABHUB_ROLE_PASSWORD (mirrors plh_hub side).
/// Tests that exercise vault unlock declare one of these.
struct ScopedRolePassword
{
    explicit ScopedRolePassword(const std::string &pw)
    {
        ::setenv("PYLABHUB_ROLE_PASSWORD", pw.c_str(), /*overwrite=*/1);
    }
    ~ScopedRolePassword() { ::unsetenv("PYLABHUB_ROLE_PASSWORD"); }
};

/// HEP-CORE-0035 §2 + HEP-CORE-0024 §3.4.2 gatekeeper helper.
/// `plh_role --validate` and run both require a provisioned role
/// home (vault file exists at auth.keyfile).  This helper mints the
/// vault via `plh_role <role> --config <cfg_path> --keygen`.
///
/// Caller MUST hold a live `ScopedRolePassword` before invoking.
inline void keygen_minimal_role(std::string_view role,
                                  const std::filesystem::path &cfg_path)
{
    pylabhub::tests::helper::WorkerProcess kg(
        plh_role_binary(),
        "--role",
        {std::string(role), "--config", cfg_path.string(), "--keygen"});
    const int rc = kg.wait_for_exit();
    if (rc != 0)
    {
        ADD_FAILURE() << "keygen_minimal_role: plh_role " << role
                      << " --keygen failed (rc=" << rc << ") for cfg '"
                      << cfg_path << "'; stderr:\n" << kg.get_stderr();
    }
}

/// Class D gate for plh_role binary tests.  Pattern-3 workers get
/// `[ERROR ]`-line scanning for free via `expect_worker_ok`; the
/// plh_role L4 tests bypass that helper because they spawn a real
/// binary (not a `run_gtest_worker` worker), so the worker-completion
/// markers don't apply.  Call this from every happy-path test to
/// catch stray ERROR-level logs the binary may have emitted while
/// still exiting 0 (the silent-fallback signature documented in
/// REVIEW_TestAudit_2026-05-01.md §0 Class D).
///
/// Error-path tests (those expecting non-zero exit code) intentionally
/// do NOT call this — the binary's own diagnostic output is part of
/// the contract there, and the test pins specific error substrings
/// via Class A path-discrimination.
inline void expect_no_unexpected_errors(
    const pylabhub::tests::helper::WorkerProcess &p)
{
    const std::string &err = p.get_stderr();
    std::istringstream lines(err);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.find("[ERROR ]") != std::string::npos)
            ADD_FAILURE() << "Unexpected ERROR-level log in plh_role "
                             "stderr (Class D gate, audit §0):\n  "
                          << line;
    }
}

} // namespace pylabhub::tests::plh_role_l4
