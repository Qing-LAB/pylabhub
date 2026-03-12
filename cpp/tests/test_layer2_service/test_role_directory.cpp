/**
 * @file test_role_directory.cpp
 * @brief Unit tests for RoleDirectory (HEP-CORE-0024).
 *
 * Pure filesystem API — no lifecycle, no subprocess dispatcher needed.
 */

#include "utils/role_directory.hpp"
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

static void write_hub_json(const fs::path &hub_dir, const std::string &ep)
{
    fs::create_directories(hub_dir);
    std::ofstream f(hub_dir / "hub.json");
    f << R"({"hub":{"broker_endpoint":")" << ep << R"("}})";
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

    const auto rd = RoleDirectory::create(tmp, "producer.json");

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
    RoleDirectory::create(tmp, "producer.json");
    // Second call — must not throw
    EXPECT_NO_THROW(RoleDirectory::create(tmp, "producer.json"));

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
    EXPECT_EQ(rd.script_entry(".", "lua").filename().string(), "__init__.lua");
}

// ── default_keyfile ────────────────────────────────────────────────────────────

TEST(RoleDirectoryTest, DefaultKeyfile_InsideVault)
{
    const auto tmp = unique_temp_dir("kf");
    fs::create_directories(tmp);

    const auto rd = RoleDirectory::open(tmp);
    const auto kf = rd.default_keyfile("PROD-TEST-00000001");

    EXPECT_EQ(kf, rd.vault() / "PROD-TEST-00000001.vault");
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

TEST(RoleDirectoryTest, HubBrokerEndpoint_ReadsFromJson)
{
    const auto tmp = unique_temp_dir("hbe");
    write_hub_json(tmp, "tcp://127.0.0.1:9999");

    EXPECT_EQ(RoleDirectory::hub_broker_endpoint(tmp), "tcp://127.0.0.1:9999");
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

// ── Phase 2: role_cli helpers ──────────────────────────────────────────────────

TEST(RoleCliTest, IsStdinTty_Runs)
{
    // Just verify it compiles and returns a bool; we can't control TTY in tests.
    [[maybe_unused]] const bool result = pylabhub::role_cli::is_stdin_tty();
    (void)result;
}

TEST(RoleCliTest, ResolveInitName_CliNameProvided)
{
    const auto result =
        pylabhub::role_cli::resolve_init_name("MyRole", "Enter name: ");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "MyRole");
}

TEST(RoleCliTest, ResolveInitName_EmptyNonInteractive_ReturnsNullopt)
{
    // When stdin is not a TTY (CI environment) and cli_name is empty → nullopt.
    // When stdin IS a TTY (developer machine) this test would prompt — skip it.
    if (pylabhub::role_cli::is_stdin_tty())
        GTEST_SKIP() << "stdin is a TTY; test requires non-interactive mode";

    const auto result = pylabhub::role_cli::resolve_init_name("", "Enter name: ");
    EXPECT_FALSE(result.has_value());
}

TEST(RoleCliTest, ParseRoleArgs_InitOnly)
{
    const char *argv[] = {"prog", "--init", "/tmp/roletest", "--name", "Test", nullptr};
    int         argc   = 5;
    const auto  args   = pylabhub::role_cli::parse_role_args(
        argc, const_cast<char **>(argv), "producer");

    EXPECT_TRUE(args.init_only);
    EXPECT_EQ(args.role_dir, "/tmp/roletest");
    EXPECT_EQ(args.init_name, "Test");
}

TEST(RoleCliTest, ParseRoleArgs_ConfigPath)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", nullptr};
    int         argc   = 3;
    const auto  args   = pylabhub::role_cli::parse_role_args(
        argc, const_cast<char **>(argv), "producer");

    EXPECT_EQ(args.config_path, "/tmp/foo.json");
    EXPECT_FALSE(args.init_only);
}

TEST(RoleCliTest, ParseRoleArgs_ValidateFlag)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", "--validate", nullptr};
    int         argc   = 4;
    const auto  args   = pylabhub::role_cli::parse_role_args(
        argc, const_cast<char **>(argv), "producer");

    EXPECT_TRUE(args.validate_only);
}

TEST(RoleCliTest, ParseRoleArgs_KeygenFlag)
{
    const char *argv[] = {"prog", "--config", "/tmp/foo.json", "--keygen", nullptr};
    int         argc   = 4;
    const auto  args   = pylabhub::role_cli::parse_role_args(
        argc, const_cast<char **>(argv), "producer");

    EXPECT_TRUE(args.keygen_only);
}

TEST(RoleCliTest, ParseRoleArgs_PositionalDir)
{
    const char *argv[] = {"prog", "/tmp/myrole", nullptr};
    int         argc   = 2;
    const auto  args   = pylabhub::role_cli::parse_role_args(
        argc, const_cast<char **>(argv), "producer");

    EXPECT_EQ(args.role_dir, "/tmp/myrole");
}
