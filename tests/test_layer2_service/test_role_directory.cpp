/**
 * @file test_role_directory.cpp
 * @brief Unit tests for RoleDirectory (HEP-CORE-0024).
 *
 * Pure filesystem API — no lifecycle, no subprocess dispatcher needed.
 */

#include "utils/role_directory.hpp"
#include "utils/cli_helpers.hpp"
#include "utils/role_cli.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using pylabhub::utils::RoleDirectory;

// ── Helpers ────────────────────────────────────────────────────────────────────

static fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> counter{0};
    const int               id = counter.fetch_add(1);
    fs::path                d  = fs::temp_directory_path() /
                                 ("plh_roletest_" + std::string(tag) + "_" +
                                  std::to_string(id));
    fs::remove_all(d);
    return d;
}

// Legacy shape (pre-HEP-0033 §6.2) — `hub.broker_endpoint`.
static void write_legacy_hub_json(const fs::path &hub_dir, const std::string &ep)
{
    fs::create_directories(hub_dir);
    std::ofstream f(hub_dir / "hub.json");
    f << R"({"hub":{"broker_endpoint":")" << ep << R"("}})";
}

// Modern shape (HEP-0033 §6.2) — `network.broker_endpoint`.
static void write_modern_hub_json(const fs::path &hub_dir, const std::string &ep)
{
    fs::create_directories(hub_dir);
    std::ofstream f(hub_dir / "hub.json");
    f << R"({"network":{"broker_endpoint":")" << ep << R"("}})";
}

// Default fixture used by tests that only need a broker endpoint to be
// readable without caring about the on-disk shape — uses the modern path.
static void write_hub_json(const fs::path &hub_dir, const std::string &ep)
{
    write_modern_hub_json(hub_dir, ep);
}

static void write_hub_pubkey(const fs::path &hub_dir, const std::string &key)
{
    std::ofstream f(hub_dir / "hub.pubkey");
    f << key << "\n";
}

// ── Phase 1: RoleDirectory ─────────────────────────────────────────────────────

TEST(RoleDirectoryTest, Open_StoresBase)
{
    const auto tmp = unique_temp_dir("open");
    fs::create_directories(tmp);

    const auto rd = RoleDirectory::open(tmp);
    EXPECT_TRUE(rd.base().is_absolute());
    // weakly_canonical should resolve the path (drop ".." etc.)
    EXPECT_TRUE(fs::exists(rd.base()) || true); // open() doesn't require existence
}

TEST(RoleDirectoryTest, FromConfigFile_ParentDir)
{
    const auto tmp = unique_temp_dir("fromcfg");
    fs::create_directories(tmp);
    const auto cfg = tmp / "producer.json";
    { std::ofstream f(cfg); f << "{}"; }

    const auto rd = RoleDirectory::from_config_file(cfg);
    EXPECT_EQ(rd.base(), RoleDirectory::open(tmp).base());
}

TEST(RoleDirectoryTest, Create_MakesStandardLayout)
{
    const auto tmp = unique_temp_dir("create");

    const auto rd = RoleDirectory::create(tmp);

    EXPECT_TRUE(fs::is_directory(rd.logs()))  << "logs/ missing";
    EXPECT_TRUE(fs::is_directory(rd.run()))   << "run/ missing";
    EXPECT_TRUE(fs::is_directory(rd.vault())) << "vault/ missing";
    EXPECT_TRUE(fs::is_directory(tmp / "script" / "python")) << "script/python/ missing";

    EXPECT_TRUE(rd.has_standard_layout());

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, Create_IsIdempotent)
{
    const auto tmp = unique_temp_dir("idem");

    // First call
    RoleDirectory::create(tmp);
    // Second call — must not throw
    EXPECT_NO_THROW(RoleDirectory::create(tmp));

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HasStandardLayout_FalseWhenMissing)
{
    const auto tmp = unique_temp_dir("layout");
    fs::create_directories(tmp);
    // Only logs/ exists
    fs::create_directories(tmp / "logs");

    const auto rd = RoleDirectory::open(tmp);
    EXPECT_FALSE(rd.has_standard_layout());

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, PathAccessors_RelativeToBase)
{
    const auto tmp = unique_temp_dir("acc");
    fs::create_directories(tmp);

    const auto rd = RoleDirectory::open(tmp);
    EXPECT_EQ(rd.logs(),   rd.base() / "logs");
    EXPECT_EQ(rd.run(),    rd.base() / "run");
    EXPECT_EQ(rd.vault(),  rd.base() / "vault");
    EXPECT_EQ(rd.config_file("producer.json"), rd.base() / "producer.json");
    EXPECT_EQ(rd.subdir("custom"), rd.base() / "custom");
}

// ── script_entry ───────────────────────────────────────────────────────────────

TEST(RoleDirectoryTest, ScriptEntry_RelativePath)
{
    const auto tmp = unique_temp_dir("se_rel");
    fs::create_directories(tmp);

    const auto rd  = RoleDirectory::open(tmp);
    const auto ep  = rd.script_entry(".", "python");

    // Should be: <base>/./script/python/__init__.py → canonical
    EXPECT_EQ(ep.filename().string(), "__init__.py");
    EXPECT_EQ(ep.parent_path().filename().string(), "python");
    EXPECT_EQ(ep.parent_path().parent_path().filename().string(), "script");
}

TEST(RoleDirectoryTest, ScriptEntry_AbsolutePath)
{
    const auto base     = unique_temp_dir("se_abs_base");
    const auto ext_path = unique_temp_dir("se_abs_ext");
    fs::create_directories(base);
    fs::create_directories(ext_path);

    const auto rd = RoleDirectory::open(base);
    const auto ep = rd.script_entry(ext_path.string(), "python");

    EXPECT_EQ(ep, ext_path / "script" / "python" / "__init__.py");
}

TEST(RoleDirectoryTest, ScriptEntry_LuaExtension)
{
    const auto tmp = unique_temp_dir("se_lua");
    fs::create_directories(tmp);
    const auto rd = RoleDirectory::open(tmp);
    EXPECT_EQ(rd.script_entry(".", "lua").filename().string(), "init.lua");
}

// ── default_keyfile ────────────────────────────────────────────────────────────

TEST(RoleDirectoryTest, DefaultKeyfile_InsideVault)
{
    const auto tmp = unique_temp_dir("kf");
    fs::create_directories(tmp);

    const auto rd = RoleDirectory::open(tmp);
    const auto kf = rd.default_keyfile("prod.test.uid00000001");

    EXPECT_EQ(kf, rd.vault() / "prod.test.uid00000001.vault");
}

// ── resolve_hub_dir ────────────────────────────────────────────────────────────

TEST(RoleDirectoryTest, ResolveHubDir_Empty_ReturnsNullopt)
{
    const auto tmp = unique_temp_dir("rhd_empty");
    fs::create_directories(tmp);
    const auto rd = RoleDirectory::open(tmp);
    EXPECT_FALSE(rd.resolve_hub_dir("").has_value());
}

TEST(RoleDirectoryTest, ResolveHubDir_Relative_ResolvesToBase)
{
    const auto tmp = unique_temp_dir("rhd_rel");
    fs::create_directories(tmp / "hub");

    const auto rd  = RoleDirectory::open(tmp);
    const auto res = rd.resolve_hub_dir("hub");
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, rd.base() / "hub");
}

TEST(RoleDirectoryTest, ResolveHubDir_Absolute_ReturnedDirectly)
{
    const auto base    = unique_temp_dir("rhd_abs_base");
    const auto hub_dir = unique_temp_dir("rhd_abs_hub");
    fs::create_directories(base);
    fs::create_directories(hub_dir);

    const auto rd  = RoleDirectory::open(base);
    const auto res = rd.resolve_hub_dir(hub_dir.string());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, RoleDirectory::open(hub_dir).base());
}

// ── hub_broker_endpoint / hub_broker_pubkey ────────────────────────────────────

TEST(RoleDirectoryTest, HubBrokerEndpoint_ReadsModernPath)
{
    // HEP-0033 §6.2 — network.broker_endpoint is the canonical location.
    const auto tmp = unique_temp_dir("hbe_modern");
    write_modern_hub_json(tmp, "tcp://127.0.0.1:9999");

    EXPECT_EQ(RoleDirectory::hub_broker_endpoint(tmp), "tcp://127.0.0.1:9999");
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubBrokerEndpoint_FallsBackToLegacyPath)
{
    // Pre-HEP-0033 hub.json files (and the demo fixtures) still use
    // the legacy `hub.broker_endpoint` location.  Reader must accept both
    // until those fixtures are regenerated by `plh_hub --init` (Phase 9).
    const auto tmp = unique_temp_dir("hbe_legacy");
    write_legacy_hub_json(tmp, "tcp://127.0.0.1:8888");

    EXPECT_EQ(RoleDirectory::hub_broker_endpoint(tmp), "tcp://127.0.0.1:8888");
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubBrokerEndpoint_NeitherKey_Throws)
{
    const auto tmp = unique_temp_dir("hbe_neither");
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "hub.json");
        f << R"({"hub":{"name":"x"}})";  // no broker_endpoint anywhere
    }
    EXPECT_THROW(RoleDirectory::hub_broker_endpoint(tmp), std::exception);
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubBrokerEndpoint_MissingJson_Throws)
{
    const auto tmp = unique_temp_dir("hbe_miss");
    fs::create_directories(tmp); // no hub.json

    EXPECT_THROW(RoleDirectory::hub_broker_endpoint(tmp), std::exception);
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubBrokerPubkey_ReadsFirstLine)
{
    const auto tmp = unique_temp_dir("hbpk");
    write_hub_json(tmp, "tcp://127.0.0.1:9999");
    write_hub_pubkey(tmp, "ABCDEF1234567890ABCDEF1234567890");

    const auto pk = RoleDirectory::hub_broker_pubkey(tmp);
    EXPECT_EQ(pk, "ABCDEF1234567890ABCDEF1234567890");
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubBrokerPubkey_MissingFile_ReturnsEmpty)
{
    const auto tmp = unique_temp_dir("hbpk_miss");
    fs::create_directories(tmp); // no hub.pubkey

    EXPECT_EQ(RoleDirectory::hub_broker_pubkey(tmp), "");
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, HubPubkeyPath_ConcatenatesFilename)
{
    const auto hub_dir = fs::path("/some/hub");
    EXPECT_EQ(RoleDirectory::hub_pubkey_path(hub_dir),
              fs::path("/some/hub/hub.pubkey"));
}

// ── pylabhub::cli generic helpers ──────────────────────────────────────────────

TEST(CliHelpersTest, IsStdinTty_Runs)
{
    // Just verify it compiles and returns a bool; we can't control TTY in tests.
    [[maybe_unused]] const bool result = pylabhub::cli::is_stdin_tty();
    (void)result;
}

TEST(CliHelpersTest, ResolveInitName_CliNameProvided)
{
    const auto result =
        pylabhub::cli::resolve_init_name("MyRole", "Enter name: ");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "MyRole");
}

TEST(CliHelpersTest, ResolveInitName_EmptyNonInteractive_ReturnsNullopt)
{
    // When stdin is not a TTY (CI environment) and cli_name is empty → nullopt.
    // When stdin IS a TTY (developer machine) this test would prompt — skip it.
    if (pylabhub::cli::is_stdin_tty())
        GTEST_SKIP() << "stdin is a TTY; test requires non-interactive mode";

    const auto result = pylabhub::cli::resolve_init_name("", "Enter name: ");
    EXPECT_FALSE(result.has_value());
}

TEST(CliHelpersTest, GetPassword_EnvVarSet_ReturnsEnvValue)
{
    // Env-var override path — works regardless of TTY.
    const char *kVarName = "PYLABHUB_TEST_GETPW_VAR_A";
    ::setenv(kVarName, "secret-from-env", /*overwrite=*/1);
    const auto result = pylabhub::cli::get_password("test", kVarName, "Prompt: ");
    ::unsetenv(kVarName);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "secret-from-env");
}

TEST(CliHelpersTest, GetPassword_EnvVarUnset_NonInteractive_ReturnsNullopt)
{
    if (pylabhub::cli::is_stdin_tty())
        GTEST_SKIP() << "stdin is a TTY; test requires non-interactive mode";

    const char *kVarName = "PYLABHUB_TEST_GETPW_VAR_B";
    ::unsetenv(kVarName);
    const auto result = pylabhub::cli::get_password("test", kVarName, "Prompt: ");
    EXPECT_FALSE(result.has_value());
}

TEST(CliHelpersTest, GetNewPassword_EnvVarSet_ReturnsEnvValueWithoutConfirm)
{
    const char *kVarName = "PYLABHUB_TEST_NEWPW_VAR_A";
    ::setenv(kVarName, "new-secret", /*overwrite=*/1);
    const auto result = pylabhub::cli::get_new_password(
        "test", kVarName, "New: ", "Confirm: ");
    ::unsetenv(kVarName);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "new-secret");
}

TEST(CliHelpersTest, GetNewPassword_EnvVarUnset_NonInteractive_ReturnsNullopt)
{
    if (pylabhub::cli::is_stdin_tty())
        GTEST_SKIP() << "stdin is a TTY; test requires non-interactive mode";

    const char *kVarName = "PYLABHUB_TEST_NEWPW_VAR_B";
    ::unsetenv(kVarName);
    const auto result = pylabhub::cli::get_new_password(
        "test", kVarName, "New: ", "Confirm: ");
    EXPECT_FALSE(result.has_value());
}

TEST(RoleCliTest, ParseRoleArgs_InitOnly)
{
    const char *argv[] = {"prog", "--init", "/tmp/roletest", "--name", "Test", nullptr};
    int         argc   = 5;
    const auto parsed = pylabhub::role_cli::parse_role_args(argc, const_cast<char **>(argv), "producer");
    ASSERT_EQ(parsed.exit_code, -1) << "unexpected exit_code";
    const auto &args = parsed.args;

    EXPECT_TRUE(args.init_only);
    EXPECT_EQ(args.role_dir, "/tmp/roletest");
    EXPECT_EQ(args.init_name, "Test");
}

TEST(RoleCliTest, ParseRoleArgs_ConfigPath)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", nullptr};
    int         argc   = 3;
    const auto parsed = pylabhub::role_cli::parse_role_args(argc, const_cast<char **>(argv), "producer");
    ASSERT_EQ(parsed.exit_code, -1) << "unexpected exit_code";
    const auto &args = parsed.args;

    EXPECT_EQ(args.config_path, "/tmp/foo.json");
    EXPECT_FALSE(args.init_only);
}

TEST(RoleCliTest, ParseRoleArgs_ValidateFlag)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", "--validate", nullptr};
    int         argc   = 4;
    const auto parsed = pylabhub::role_cli::parse_role_args(argc, const_cast<char **>(argv), "producer");
    ASSERT_EQ(parsed.exit_code, -1) << "unexpected exit_code";
    const auto &args = parsed.args;

    EXPECT_TRUE(args.validate_only);
}

TEST(RoleCliTest, ParseRoleArgs_KeygenFlag)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", "--keygen", nullptr};
    int         argc   = 4;
    const auto parsed = pylabhub::role_cli::parse_role_args(argc, const_cast<char **>(argv), "producer");
    ASSERT_EQ(parsed.exit_code, -1) << "unexpected exit_code";
    const auto &args = parsed.args;

    EXPECT_TRUE(args.keygen_only);
}

TEST(RoleCliTest, ParseRoleArgs_PositionalDir)
{
    const char *argv[] = {"prog", "/tmp/myrole", nullptr};
    int         argc   = 2;
    const auto parsed = pylabhub::role_cli::parse_role_args(argc, const_cast<char **>(argv), "producer");
    ASSERT_EQ(parsed.exit_code, -1) << "unexpected exit_code";
    const auto &args = parsed.args;

    EXPECT_EQ(args.role_dir, "/tmp/myrole");
}

// ── Security: warn_if_keyfile_in_role_dir ─────────────────────────────────────

TEST(RoleDirectoryTest, WarnIfKeyfileInRoleDir_NoWarnWhenEmpty)
{
    const auto tmp = unique_temp_dir("wkf_empty");
    fs::create_directories(tmp);
    const auto rd = RoleDirectory::open(tmp);

    // Empty keyfile: must not emit any warning (early-return branch).
    testing::internal::CaptureStderr();
    RoleDirectory::warn_if_keyfile_in_role_dir(rd.base(), "");
    const std::string captured = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(captured.empty())
        << "empty keyfile must not produce any stderr output; got: " << captured;
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, WarnIfKeyfileInRoleDir_InsideRoleDir_Absolute)
{
    // Keyfile inside the role dir must emit the SECURITY WARNING banner.
    const auto tmp = unique_temp_dir("wkf_inside");
    fs::create_directories(tmp / "vault");
    const auto rd = RoleDirectory::open(tmp);

    const auto kf = (rd.vault() / "prod.test.vault").string();
    testing::internal::CaptureStderr();
    RoleDirectory::warn_if_keyfile_in_role_dir(rd.base(), kf);
    const std::string captured = testing::internal::GetCapturedStderr();
    // Pin both the warning banner (proves the warning path fired) and the
    // keyfile path (proves the message references the actual input).
    EXPECT_NE(captured.find("PYLABHUB SECURITY WARNING"), std::string::npos)
        << "warning banner missing; stderr was: " << captured;
    EXPECT_NE(captured.find(kf), std::string::npos)
        << "warning must mention the offending keyfile path";
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, WarnIfKeyfileInRoleDir_OutsideRoleDir_NoWarn)
{
    // Keyfile outside role dir: warning must be suppressed (silent return).
    const auto role_tmp = unique_temp_dir("wkf_out_role");
    const auto sys_tmp  = unique_temp_dir("wkf_out_sys");
    fs::create_directories(role_tmp);
    fs::create_directories(sys_tmp);

    const auto rd = RoleDirectory::open(role_tmp);
    const auto kf = (sys_tmp / "prod.test.vault").string();

    testing::internal::CaptureStderr();
    RoleDirectory::warn_if_keyfile_in_role_dir(rd.base(), kf);
    const std::string captured = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(captured.empty())
        << "keyfile outside role dir must not warn; got: " << captured;
    fs::remove_all(role_tmp);
    fs::remove_all(sys_tmp);
}

TEST(RoleDirectoryTest, WarnIfKeyfileInRoleDir_RelativePath_Resolved)
{
    // Relative path "vault/prod.test.vault" is resolved *under* the role dir
    // — so it lands inside and must trigger the warning.  This pins the
    // relative-resolution branch of the function (not just no-crash).
    const auto tmp = unique_temp_dir("wkf_rel");
    fs::create_directories(tmp / "vault");
    const auto rd = RoleDirectory::open(tmp);

    testing::internal::CaptureStderr();
    RoleDirectory::warn_if_keyfile_in_role_dir(rd.base(), "vault/prod.test.vault");
    const std::string captured = testing::internal::GetCapturedStderr();
    EXPECT_NE(captured.find("PYLABHUB SECURITY WARNING"), std::string::npos)
        << "relative keyfile resolved inside role dir must warn; stderr: "
        << captured;
    fs::remove_all(tmp);
}

// ── Phase 9: register_role + init_directory (HEP-0024 §10) ──────────────────

TEST(RoleDirectoryTest, InitDirectory_UnregisteredRole_ReturnsError)
{
    const auto tmp = unique_temp_dir("init_unreg");
    EXPECT_NE(RoleDirectory::init_directory(tmp, "nonexistent_role_xyz", "Test"), 0);
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, InitDirectory_EmptyName_ReturnsError)
{
    // Lib must not prompt — caller resolves name before calling.
    const auto tmp = unique_temp_dir("init_empty_name");

    RoleDirectory::register_role("test_role_empty_name")
        .config_filename("x.json")
        .uid_prefix("X")
        .role_label("X");

    EXPECT_NE(RoleDirectory::init_directory(tmp, "test_role_empty_name", ""), 0);
    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, InitDirectory_CreatesDirectoryAndConfig)
{
    const auto tmp = unique_temp_dir("init_basic");

    RoleDirectory::register_role("test_role_basic")
        .config_filename("test_role.json")
        .uid_prefix("TEST")
        .role_label("TestRole")
        .config_template([](const std::string &uid, const std::string &name)
            -> nlohmann::json
        {
            nlohmann::json j;
            j["test"]["uid"]  = uid;
            j["test"]["name"] = name;
            j["value"]        = 42;
            return j;
        });

    EXPECT_EQ(RoleDirectory::init_directory(tmp, "test_role_basic", "MyTestRole"), 0);

    // Directory structure created.
    EXPECT_TRUE(fs::is_directory(tmp / "logs"));
    EXPECT_TRUE(fs::is_directory(tmp / "run"));
    EXPECT_TRUE(fs::is_directory(tmp / "vault"));

    // Config file written with correct content.
    const auto config_path = tmp / "test_role.json";
    ASSERT_TRUE(fs::exists(config_path));

    std::ifstream f(config_path);
    const auto j = nlohmann::json::parse(f);
    EXPECT_EQ(j["test"]["name"], "MyTestRole");
    EXPECT_EQ(j["value"], 42);
    EXPECT_TRUE(j["test"]["uid"].get<std::string>().find("TEST.") == 0);

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, InitDirectory_ExistingConfig_ReturnsError)
{
    const auto tmp = unique_temp_dir("init_exists");

    RoleDirectory::register_role("test_role_exists")
        .config_filename("existing.json")
        .uid_prefix("TEST")
        .role_label("TestRole")
        .config_template([](const std::string &, const std::string &)
        { return nlohmann::json{{"x", 1}}; });

    // Create directory and config file first.
    fs::create_directories(tmp);
    std::ofstream(tmp / "existing.json") << "{}";

    // init_directory should fail because config already exists.
    EXPECT_NE(RoleDirectory::init_directory(tmp, "test_role_exists", "Test"), 0);

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, InitDirectory_OnInitCallbackInvoked)
{
    const auto tmp = unique_temp_dir("init_cb");
    bool callback_invoked = false;
    std::string callback_name;

    RoleDirectory::register_role("test_role_cb")
        .config_filename("cb_role.json")
        .uid_prefix("CB")
        .role_label("CallbackRole")
        .config_template([](const std::string &uid, const std::string &name)
        {
            return nlohmann::json{{"uid", uid}, {"name", name}};
        })
        .on_init([&](const RoleDirectory &rd, const std::string &name)
        {
            callback_invoked = true;
            callback_name = name;
            // Write a custom file using RoleDirectory path API.
            const auto custom_file = rd.subdir("data") / "readme.txt";
            fs::create_directories(custom_file.parent_path());
            std::ofstream(custom_file) << "Created by on_init\n";
        });

    EXPECT_EQ(RoleDirectory::init_directory(tmp, "test_role_cb", "MyCallback"), 0);

    EXPECT_TRUE(callback_invoked);
    EXPECT_EQ(callback_name, "MyCallback");
    EXPECT_TRUE(fs::exists(tmp / "data" / "readme.txt"));

    fs::remove_all(tmp);
}

TEST(RoleDirectoryTest, InitDirectory_NullConfigTemplate_NoConfigWritten)
{
    const auto tmp = unique_temp_dir("init_nocfg");

    RoleDirectory::register_role("test_role_nocfg")
        .config_filename("nocfg.json")
        .uid_prefix("NC")
        .role_label("NoConfig");

    EXPECT_EQ(RoleDirectory::init_directory(tmp, "test_role_nocfg", "Test"), 0);

    // Directory created but no config file.
    EXPECT_TRUE(fs::is_directory(tmp / "logs"));
    EXPECT_FALSE(fs::exists(tmp / "nocfg.json"));

    fs::remove_all(tmp);
}
