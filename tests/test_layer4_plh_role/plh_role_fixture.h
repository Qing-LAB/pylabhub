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
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
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

/// Create a uniquely-named temp directory under the OS temp root.
/// The @p prefix appears in the directory name for grep-ability.
/// Caller is responsible for cleanup (fixture does it automatically).
inline fs::path make_tmp_dir(const std::string &prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                 / ("plh_role_l4_" + prefix + "_"
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
    // not at top level.  Callers wanting to set auth.keyfile can
    // either pass it via @p overrides or leave it empty.
    //
    // Build the JSON via explicit assignment (not nested initializer
    // lists) — nlohmann::json interprets `{"key", {...}}` in a nested
    // context as an ambiguous structure and may produce the wrong
    // shape.  Assignment is unambiguous.

    auto slot_schema = nlohmann::json::object();
    auto fields = nlohmann::json::array();
    fields.push_back(nlohmann::json{{"name", "v"}, {"type", "float32"}});
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
        j["producer"]["uid"]  = "prod.l4test.u00000001";
        j["producer"]["name"] = "L4Test";
        j["producer"]["auth"]["keyfile"] = "";
        j["out_channel"]     = "lab.l4.test";
        j["out_slot_schema"] = slot_schema;
    }
    else if (role == "consumer")
    {
        j["consumer"]["uid"]  = "cons.l4test.u00000001";
        j["consumer"]["name"] = "L4Test";
        j["consumer"]["auth"]["keyfile"] = "";
        j["in_channel"]     = "lab.l4.test";
        j["in_slot_schema"] = slot_schema;
    }
    else if (role == "processor")
    {
        j["processor"]["uid"]  = "proc.l4test.u00000001";
        j["processor"]["name"] = "L4Test";
        j["processor"]["auth"]["keyfile"] = "";
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

} // namespace pylabhub::tests::plh_role_l4
