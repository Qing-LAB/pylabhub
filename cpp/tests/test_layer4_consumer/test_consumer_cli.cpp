/**
 * @file test_consumer_cli.cpp
 * @brief CLI integration tests for pylabhub-consumer (binary invoked via WorkerProcess).
 *
 * Spawns the staged `pylabhub-consumer` binary for each test.
 *
 * Binary path: <test-binary-dir>/../bin/pylabhub-consumer
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
                   / ("plh_conscli_" + prefix + "_" + std::to_string(id));
    fs::remove_all(dir);          // start fresh regardless of previous runs
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path &path, const std::string &content)
{
    std::ofstream f(path);
    f << content;
}

/// Path to the staged pylabhub-consumer binary, relative to this test binary.
static std::string consumer_binary()
{
    return (fs::path(g_self_exe_path).parent_path() / ".." / "bin" / "pylabhub-consumer")
               .string();
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class ConsumerCliTest : public pylabhub::tests::IsolatedProcessTest {};

// ── Tests ────────────────────────────────────────────────────────────────────

/// --init creates the canonical directory structure and consumer.json.
TEST_F(ConsumerCliTest, Init_CreatesDirectoryStructure)
{
    const auto tmp = unique_temp_dir("init");

    WorkerProcess proc(consumer_binary(), "--init",
                       {tmp.string(), "--name", "TestConsumer"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

    EXPECT_TRUE(fs::exists(tmp / "consumer.json"))          << "consumer.json missing";
    EXPECT_TRUE(fs::exists(tmp / "script" / "python" / "__init__.py"))
                                                             << "__init__.py missing";
    EXPECT_TRUE(fs::is_directory(tmp / "vault"))             << "vault/ missing";
    EXPECT_TRUE(fs::is_directory(tmp / "logs"))              << "logs/ missing";
    EXPECT_TRUE(fs::is_directory(tmp / "run"))               << "run/ missing";

    fs::remove_all(tmp);
}

/// --init default values: uid starts with CONS-, script.path=".", stop_on_script_error=false.
TEST_F(ConsumerCliTest, Init_DefaultValues)
{
    const auto tmp = unique_temp_dir("initdef");

    WorkerProcess proc(consumer_binary(), "--init",
                       {tmp.string(), "--name", "DefaultTest"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

    std::ifstream f(tmp / "consumer.json");
    ASSERT_TRUE(f.is_open()) << "consumer.json not created";
    nlohmann::json j = nlohmann::json::parse(f);

    // uid has CONS- prefix
    const std::string uid = j["consumer"]["uid"].get<std::string>();
    EXPECT_EQ(uid.rfind("CONS-", 0), 0u)
        << "Generated uid does not start with CONS-, got: " << uid;

    // script.path == "."
    EXPECT_EQ(j["script"]["path"].get<std::string>(), ".");

    // script.type == "python"
    EXPECT_EQ(j["script"]["type"].get<std::string>(), "python");

    // stop_on_script_error defaults to false
    EXPECT_FALSE(j["validation"]["stop_on_script_error"].get<bool>());

    fs::remove_all(tmp);
}

/// --keygen creates the vault file at the path specified in auth.keyfile.
TEST_F(ConsumerCliTest, Keygen_WritesVaultFile)
{
    const auto tmp        = unique_temp_dir("keygen");
    const auto cfg_path   = tmp / "consumer.json";
    const auto vault_path = tmp / "vault" / "test.vault";

    write_file(cfg_path,
        "{\n"
        "  \"consumer\": {\n"
        "    \"uid\": \"CONS-KGTEST-00000001\",\n"
        "    \"name\": \"KgTest\",\n"
        "    \"auth\": { \"keyfile\": \"" + vault_path.string() + "\" }\n"
        "  },\n"
        "  \"channel\": \"lab.keygen.test\"\n"
        "}\n");

#if defined(PYLABHUB_IS_POSIX)
    ::setenv("PYLABHUB_ACTOR_PASSWORD", "test-vault-password", 1);
#endif

    WorkerProcess proc(consumer_binary(), "--config",
                       {cfg_path.string(), "--keygen"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr();

#if defined(PYLABHUB_IS_POSIX)
    ::unsetenv("PYLABHUB_ACTOR_PASSWORD");
#endif

    EXPECT_TRUE(fs::exists(vault_path))
        << "Vault file not created at " << vault_path.string();

    const auto out = proc.get_stdout();
    EXPECT_NE(out.find("Consumer vault written to"), std::string::npos)
        << "Expected 'Consumer vault written to' in stdout, got:\n" << out;
    EXPECT_NE(out.find("public_key"), std::string::npos)
        << "Expected 'public_key' in stdout, got:\n" << out;

    fs::remove_all(tmp);
}

/// --validate loads config + script, prints "Validation passed.", exits 0.
TEST_F(ConsumerCliTest, Validate_ExitZero)
{
    const auto tmp      = unique_temp_dir("val");
    const auto cfg_path = tmp / "consumer.json";

    // script_path is the *parent* of the script/<type>/ tree.
    // The binary resolves: <script_path>/script/<type>/__init__.py
    // So create tmp/script/python/__init__.py and set script_path = tmp.
    fs::create_directories(tmp / "script" / "python");

    write_file(tmp / "script" / "python" / "__init__.py",
        "import pylabhub_consumer as cons\n"
        "\n"
        "def on_init(api):\n"
        "    pass\n"
        "\n"
        "def on_consume(in_slot, flexzone, messages, api):\n"
        "    pass\n"
        "\n"
        "def on_stop(api):\n"
        "    pass\n");

    // Write config; script_path = tmp (parent of script/)
    write_file(cfg_path,
        "{\n"
        "  \"consumer\": { \"uid\": \"CONS-VALTEST-00000001\", \"name\": \"ValTest\" },\n"
        "  \"channel\": \"lab.validate.test\",\n"
        "  \"script\": { \"path\": \"" + tmp.string() + "\", \"type\": \"python\" }\n"
        "}\n");

    WorkerProcess proc(consumer_binary(), "--config",
                       {cfg_path.string(), "--validate"});
    EXPECT_EQ(proc.wait_for_exit(), 0)
        << "stderr:\n" << proc.get_stderr()
        << "\nstdout:\n" << proc.get_stdout();

    EXPECT_NE(proc.get_stdout().find("Validation passed"), std::string::npos)
        << "Expected 'Validation passed' in stdout, got:\n" << proc.get_stdout();

    fs::remove_all(tmp);
}

/// Malformed JSON config: binary exits non-zero and prints "Config error" to stderr.
TEST_F(ConsumerCliTest, Config_MalformedJson_NonZeroExit)
{
    const auto tmp      = unique_temp_dir("badjs");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({ "consumer": { broken json )");

    WorkerProcess proc(consumer_binary(), "--config", {cfg_path.string()});
    EXPECT_NE(proc.wait_for_exit(), 0)
        << "Expected non-zero exit for malformed JSON";
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "Expected 'Config error' in stderr, got:\n" << proc.get_stderr();

    fs::remove_all(tmp);
}

/// Non-existent config file: binary exits non-zero and writes error to stderr.
TEST_F(ConsumerCliTest, Config_FileNotFound_NonZeroExit)
{
    WorkerProcess proc(consumer_binary(), "--config",
                       {"/no/such/path/does/not/exist/consumer.json"});
    EXPECT_NE(proc.wait_for_exit(), 0)
        << "Expected non-zero exit for missing config file";
    EXPECT_FALSE(proc.get_stderr().empty())
        << "Expected error text in stderr";
}

/// --init without --name in non-interactive mode (subprocess stdin is not a TTY):
/// binary must exit non-zero and print a useful error message.
TEST_F(ConsumerCliTest, Init_NonInteractiveNoName_ExitsWithError)
{
    const auto tmp = unique_temp_dir("noname");

    WorkerProcess proc(consumer_binary(), "--init", {tmp.string()});
    EXPECT_NE(proc.wait_for_exit(), 0)
        << "Expected non-zero exit when --name not provided in non-interactive mode";
    EXPECT_NE(proc.get_stderr().find("--name"), std::string::npos)
        << "Expected '--name' in error message, got:\n" << proc.get_stderr();

    fs::remove_all(tmp);
}
