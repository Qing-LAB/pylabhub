/**
 * @file test_processor_cli.cpp
 * @brief CLI integration tests for pylabhub-processor (binary invoked via WorkerProcess).
 *
 * Spawns the staged `pylabhub-processor` binary for each test.  The binary
 * manages its own lifecycle; the test process only inspects exit code and output.
 *
 * Binary path: <test-binary-dir>/../bin/pylabhub-processor
 *
 * Pattern 3 (IsolatedProcessTest) — subprocess-per-test.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace pylabhub::tests::helper;

// ── Helpers ──────────────────────────────────────────────────────────────────

static fs::path unique_temp_dir(const std::string &prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                   / ("plh_proccli_" + prefix + "_" + std::to_string(id));
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path &path, const std::string &content)
{
    std::ofstream f(path);
    f << content;
}

/// Path to the staged pylabhub-processor binary, relative to this test binary.
static std::string processor_binary()
{
    return (fs::path(g_self_exe_path).parent_path() / ".." / "bin" / "pylabhub-processor")
               .string();
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class ProcessorCliTest : public pylabhub::tests::IsolatedProcessTest {};

// ── Tests ────────────────────────────────────────────────────────────────────

/// --init creates the canonical directory structure and processor.json.
TEST_F(ProcessorCliTest, Init_CreatesDirectoryStructure)
{
    const auto tmp = unique_temp_dir("init");

    WorkerProcess proc(processor_binary(), "--init", {tmp.string()});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

    EXPECT_TRUE(fs::exists(tmp / "processor.json"))          << "processor.json missing";
    EXPECT_TRUE(fs::exists(tmp / "script" / "python" / "__init__.py"))
                                                               << "__init__.py missing";
    EXPECT_TRUE(fs::is_directory(tmp / "vault"))               << "vault/ missing";
    EXPECT_TRUE(fs::is_directory(tmp / "logs"))                << "logs/ missing";
    EXPECT_TRUE(fs::is_directory(tmp / "run"))                 << "run/ missing";

    fs::remove_all(tmp);
}

/// --init default values: uid starts with PROC-, script.path=".", stop_on_script_error=false.
TEST_F(ProcessorCliTest, Init_DefaultValues)
{
    const auto tmp = unique_temp_dir("initdef");

    WorkerProcess proc(processor_binary(), "--init", {tmp.string()});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

    // Read and parse the generated processor.json
    std::ifstream f(tmp / "processor.json");
    ASSERT_TRUE(f.is_open()) << "processor.json not created";
    nlohmann::json j = nlohmann::json::parse(f);

    // uid has PROC- prefix
    const std::string uid = j["processor"]["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind("PROC-", 0), 0u)
        << "Generated uid does not start with PROC-, got: " << uid;

    // script.path == "."
    EXPECT_EQ(j["script"]["path"].get<std::string>(), ".");

    // stop_on_script_error defaults to false
    EXPECT_FALSE(j["validation"]["stop_on_script_error"].get<bool>());

    // Processor-specific: both channels should be present
    EXPECT_TRUE(j.contains("in_channel"));
    EXPECT_TRUE(j.contains("out_channel"));

    // overflow_policy should be present
    EXPECT_TRUE(j.contains("overflow_policy"));

    fs::remove_all(tmp);
}

/// --keygen creates the vault file at the path specified in auth.keyfile.
TEST_F(ProcessorCliTest, Keygen_WritesVaultFile)
{
    const auto tmp        = unique_temp_dir("keygen");
    const auto cfg_path   = tmp / "processor.json";
    const auto vault_path = tmp / "vault" / "test.vault";

    // Write a config that has an absolute vault path so --keygen can find it
    write_file(cfg_path,
        "{\n"
        "  \"processor\": {\n"
        "    \"uid\": \"PROC-KGTEST-00000001\",\n"
        "    \"name\": \"KgTest\",\n"
        "    \"auth\": { \"keyfile\": \"" + vault_path.string() + "\" }\n"
        "  },\n"
        "  \"in_channel\":  \"lab.keygen.in\",\n"
        "  \"out_channel\": \"lab.keygen.out\"\n"
        "}\n");

    // ActorVault::create() creates parent dirs automatically
#if defined(PYLABHUB_IS_POSIX)
    ::setenv("PYLABHUB_ACTOR_PASSWORD", "test-vault-password", 1);
#endif

    WorkerProcess proc(processor_binary(), "--config",
                       {cfg_path.string(), "--keygen"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

#if defined(PYLABHUB_IS_POSIX)
    ::unsetenv("PYLABHUB_ACTOR_PASSWORD");
#endif

    EXPECT_TRUE(fs::exists(vault_path))
        << "Vault file not created at " << vault_path.string();

    // stdout should mention the vault path and public_key
    const auto out = proc.get_stdout();
    EXPECT_NE(out.find("Processor vault written to"), std::string::npos)
        << "Expected 'Processor vault written to' in stdout, got:\n" << out;
    EXPECT_NE(out.find("public_key"), std::string::npos)
        << "Expected 'public_key' in stdout, got:\n" << out;

    fs::remove_all(tmp);
}

/// --validate loads config + script, prints "Validation passed.", exits 0.
TEST_F(ProcessorCliTest, Validate_ExitZero)
{
    const auto tmp      = unique_temp_dir("val");
    const auto cfg_path = tmp / "processor.json";

    // script_path is the *parent* of the script/<type>/ tree.
    // The binary resolves: <script_path>/script/<type>/__init__.py
    // So create tmp/script/python/__init__.py and set script_path = tmp.
    fs::create_directories(tmp / "script" / "python");

    // Minimal script that does nothing harmful
    write_file(tmp / "script" / "python" / "__init__.py",
        "import pylabhub_processor as proc\n"
        "\n"
        "def on_init(api):\n"
        "    pass\n"
        "\n"
        "def on_process(in_slot, out_slot, flexzone, messages, api):\n"
        "    return True\n"
        "\n"
        "def on_stop(api):\n"
        "    pass\n");

    // Write config; script_path = tmp (parent of script/)
    write_file(cfg_path,
        "{\n"
        "  \"processor\": { \"uid\": \"PROC-VALTEST-00000001\", \"name\": \"ValTest\" },\n"
        "  \"in_channel\":  \"lab.validate.in\",\n"
        "  \"out_channel\": \"lab.validate.out\",\n"
        "  \"script\": { \"path\": \"" + tmp.string() + "\", \"type\": \"python\" }\n"
        "}\n");

    WorkerProcess proc(processor_binary(), "--config",
                       {cfg_path.string(), "--validate"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr()
        << "\nstdout:\n" << proc.get_stdout();

    EXPECT_NE(proc.get_stdout().find("Validation passed"), std::string::npos)
        << "Expected 'Validation passed' in stdout, got:\n" << proc.get_stdout();

    fs::remove_all(tmp);
}

/// Malformed JSON config: binary exits non-zero and prints "Config error" to stderr.
TEST_F(ProcessorCliTest, Config_MalformedJson_NonZeroExit)
{
    const auto tmp      = unique_temp_dir("badjs");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({ "processor": { broken json )");

    WorkerProcess proc(processor_binary(), "--config", {cfg_path.string()});
    EXPECT_NE(proc.wait_for_exit(), 0)
        << "Expected non-zero exit for malformed JSON";
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "Expected 'Config error' in stderr, got:\n" << proc.get_stderr();

    fs::remove_all(tmp);
}

/// Non-existent config file: binary exits non-zero and writes error to stderr.
TEST_F(ProcessorCliTest, Config_FileNotFound_NonZeroExit)
{
    WorkerProcess proc(processor_binary(), "--config",
                       {"/no/such/path/does/not/exist/processor.json"});
    EXPECT_NE(proc.wait_for_exit(), 0)
        << "Expected non-zero exit for missing config file";
    EXPECT_FALSE(proc.get_stderr().empty())
        << "Expected error text in stderr";
}
