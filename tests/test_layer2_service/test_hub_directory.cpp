/**
 * @file test_hub_directory.cpp
 * @brief Unit tests for HubDirectory (HEP-CORE-0033 §7).
 *
 * Pure filesystem API — no lifecycle, no subprocess dispatcher needed.
 */

#include "utils/hub_directory.hpp"
#include "utils/naming.hpp"  // is_valid_identifier (PeerUid validation)

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using pylabhub::utils::HubDirectory;

// ── Helpers ──────────────────────────────────────────────────────────────────

static fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path d   = fs::temp_directory_path() /
                   ("plh_hubdir_" + std::string(tag) + "_" +
                    std::to_string(id));
    fs::remove_all(d);
    return d;
}

static nlohmann::json read_json(const fs::path &p)
{
    std::ifstream f(p);
    return nlohmann::json::parse(f);
}

// ── Path accessors ───────────────────────────────────────────────────────────

TEST(HubDirectoryTest, Open_StoresBaseAndAccessors)
{
    const auto tmp = unique_temp_dir("open");
    fs::create_directories(tmp);

    const auto hd = HubDirectory::open(tmp);
    EXPECT_TRUE(hd.base().is_absolute());
    EXPECT_EQ(hd.config_file(),    hd.base() / "hub.json");
    EXPECT_EQ(hd.logs(),           hd.base() / "logs");
    EXPECT_EQ(hd.run(),            hd.base() / "run");
    EXPECT_EQ(hd.vault(),          hd.base() / "vault");
    EXPECT_EQ(hd.schemas(),        hd.base() / "schemas");
    EXPECT_EQ(hd.hub_vault_file(), hd.base() / "vault" / "hub.vault");

    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, FromConfigFile_ParentDir)
{
    const auto tmp = unique_temp_dir("from_cfg");
    fs::create_directories(tmp);
    const auto cfg_path = tmp / "hub.json";

    const auto hd = HubDirectory::from_config_file(cfg_path);
    EXPECT_EQ(hd.base(), fs::weakly_canonical(tmp));

    fs::remove_all(tmp);
}

// ── create() ─────────────────────────────────────────────────────────────────

TEST(HubDirectoryTest, Create_MakesStandardLayout)
{
    const auto tmp = unique_temp_dir("create");
    HubDirectory::create(tmp);

    EXPECT_TRUE(fs::is_directory(tmp / "logs"));
    EXPECT_TRUE(fs::is_directory(tmp / "run"));
    EXPECT_TRUE(fs::is_directory(tmp / "vault"));
    EXPECT_TRUE(fs::is_directory(tmp / "schemas"));
    EXPECT_TRUE(fs::is_directory(tmp / "script" / "python"));

#if defined(__unix__) || defined(__APPLE__)
    // vault/ should be 0700.
    struct stat st{};
    ASSERT_EQ(0, ::stat((tmp / "vault").c_str(), &st));
    EXPECT_EQ(st.st_mode & 0777, 0700);
#endif

    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, Create_IsIdempotent)
{
    const auto tmp = unique_temp_dir("idempotent");
    HubDirectory::create(tmp);
    EXPECT_NO_THROW(HubDirectory::create(tmp));
    EXPECT_TRUE(fs::is_directory(tmp / "logs"));

    fs::remove_all(tmp);
}

// ── has_standard_layout() ────────────────────────────────────────────────────

TEST(HubDirectoryTest, HasStandardLayout_TrueAfterCreate)
{
    const auto tmp = unique_temp_dir("layout_yes");
    auto hd = HubDirectory::create(tmp);
    EXPECT_TRUE(hd.has_standard_layout());
    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, HasStandardLayout_FalseWhenMissing)
{
    const auto tmp = unique_temp_dir("layout_no");
    fs::create_directories(tmp);
    auto hd = HubDirectory::open(tmp);
    EXPECT_FALSE(hd.has_standard_layout());
    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, HasStandardLayout_TrueWithoutOptionalSchemasAndScript)
{
    // schemas/ and script/ are OPTIONAL per HEP-0033 §7 — layout check
    // must accept their absence.
    const auto tmp = unique_temp_dir("layout_opt");
    fs::create_directories(tmp / "logs");
    fs::create_directories(tmp / "run");
    fs::create_directories(tmp / "vault");
    auto hd = HubDirectory::open(tmp);
    EXPECT_TRUE(hd.has_standard_layout());
    fs::remove_all(tmp);
}

// ── script_entry() ───────────────────────────────────────────────────────────

TEST(HubDirectoryTest, ScriptEntry_Python)
{
    const auto tmp = unique_temp_dir("script_py");
    fs::create_directories(tmp);
    const auto hd = HubDirectory::open(tmp);

    const auto entry = hd.script_entry(".", "python");
    EXPECT_EQ(entry.filename(), "__init__.py");
    EXPECT_EQ(entry.parent_path().filename(), "python");

    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, ScriptEntry_Lua)
{
    const auto tmp = unique_temp_dir("script_lua");
    fs::create_directories(tmp);
    const auto hd = HubDirectory::open(tmp);

    const auto entry = hd.script_entry(".", "lua");
    EXPECT_EQ(entry.filename(), "init.lua");
    EXPECT_EQ(entry.parent_path().filename(), "lua");

    fs::remove_all(tmp);
}

// ── init_directory() ─────────────────────────────────────────────────────────

TEST(HubDirectoryTest, InitDirectory_WritesParseableTemplate)
{
    const auto tmp = unique_temp_dir("init_basic");
    const int rc = HubDirectory::init_directory(tmp, "TestHub");
    ASSERT_EQ(rc, 0);

    // Directory structure exists.
    EXPECT_TRUE(fs::is_directory(tmp / "logs"));
    EXPECT_TRUE(fs::is_directory(tmp / "vault"));

    // hub.json exists and has the expected top-level shape.
    const auto cfg = tmp / "hub.json";
    ASSERT_TRUE(fs::exists(cfg));
    const auto j = read_json(cfg);
    EXPECT_TRUE(j.contains("hub"));
    EXPECT_TRUE(j.contains("logging"));
    EXPECT_TRUE(j.contains("network"));
    EXPECT_TRUE(j.contains("admin"));
    EXPECT_TRUE(j.contains("broker"));
    EXPECT_TRUE(j.contains("federation"));
    EXPECT_TRUE(j.contains("state"));

    EXPECT_EQ(j["hub"]["name"].get<std::string>(), "TestHub");

    // UID is auto-generated and validates as PeerUid.
    const std::string uid = j["hub"]["uid"].get<std::string>();
    EXPECT_FALSE(uid.empty());
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::PeerUid));

    // Auth fields deferred to HEP-0035 must NOT be in the template.
    EXPECT_FALSE(j["broker"].contains("default_channel_policy"));
    EXPECT_FALSE(j["broker"].contains("known_roles"));
    EXPECT_FALSE(j["broker"].contains("federation_trust_mode"));

    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, InitDirectory_LogOverridesLand)
{
    const auto tmp = unique_temp_dir("init_log");
    HubDirectory::LogInitOverrides log;
    log.max_size_mb = 50.0;
    log.backups     = -1;

    const int rc = HubDirectory::init_directory(tmp, "OverrideHub", log);
    ASSERT_EQ(rc, 0);

    const auto j = read_json(tmp / "hub.json");
    EXPECT_DOUBLE_EQ(j["logging"]["max_size_mb"].get<double>(), 50.0);
    EXPECT_EQ(j["logging"]["backups"].get<int>(), -1);

    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, InitDirectory_EmptyName_Fails)
{
    const auto tmp = unique_temp_dir("init_empty");
    fs::create_directories(tmp);
    EXPECT_NE(HubDirectory::init_directory(tmp, ""), 0);
    fs::remove_all(tmp);
}

TEST(HubDirectoryTest, InitDirectory_PreExistingHubJson_Fails)
{
    const auto tmp = unique_temp_dir("init_exists");
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "hub.json");
        f << "{}\n";
    }
    EXPECT_NE(HubDirectory::init_directory(tmp, "Conflict"), 0);

    // The existing file must be preserved (not overwritten).
    const auto j = read_json(tmp / "hub.json");
    EXPECT_TRUE(j.is_object());
    EXPECT_FALSE(j.contains("hub"));

    fs::remove_all(tmp);
}
