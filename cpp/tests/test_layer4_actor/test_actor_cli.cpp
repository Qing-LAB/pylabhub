/**
 * @file test_actor_cli.cpp
 * @brief Integration tests for pylabhub-actor CLI modes.
 *
 * These tests spawn the staged pylabhub-actor binary as an external process and
 * verify its exit code and output for modes that do NOT require a Python interpreter
 * or a live broker connection:
 *
 *   --keygen        — generate actor NaCl keypair file
 *   --register-with — append actor to hub.json known_actors
 *   Config errors   — malformed JSON, missing fields, invalid values
 *
 * ## Binary path discovery
 *
 * `actor_bin()` derives the pylabhub-actor path relative to g_self_exe_path:
 *
 *     stage-debug/tests/test_layer4_actor_cli   (this binary)
 *     stage-debug/bin/pylabhub-actor             (actor binary: ../bin/)
 *
 * ## Why no Python tests here
 *
 * --validate and --list-roles start the Python interpreter and load scripts.
 * Setting up a valid actor directory with a Python package and a staged standalone
 * Python is an integration-level concern better tested manually or in dedicated CI.
 * The config-parse path (steps before LifecycleGuard) is the focus here.
 *
 * ## TmpDir isolation
 *
 * Each test creates a uniquely-named temp directory via TmpDir RAII.
 * All created files are removed on test exit even if assertions fail.
 */

#include "test_entrypoint.h"
#include "test_process_utils.h"

#include "utils/actor_vault.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>   // ::setenv
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>  // getpid()

namespace fs = std::filesystem;
using namespace pylabhub::tests::helper;

// ============================================================================
// Helpers
// ============================================================================

namespace
{

/// Returns the path to the staged pylabhub-actor binary.
/// Derived from the test binary: one directory up from tests/, then into bin/.
fs::path actor_bin()
{
    // g_self_exe_path is set by test_entrypoint.cpp from argv[0].
    // When invoked by CTest from stage-debug/tests/, this is an absolute path.
    return fs::path(g_self_exe_path).parent_path().parent_path() / "bin" / "pylabhub-actor";
}

/// RAII temporary directory: unique path created on construction; removed on destruction.
class TmpDir
{
  public:
    TmpDir()
    {
        static std::atomic<int> s_counter{0};
        path_ = fs::temp_directory_path() /
                ("actor_cli_test_" + std::to_string(::getpid()) +
                 "_" + std::to_string(++s_counter));
        fs::create_directories(path_);
    }

    ~TmpDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

/// Write a string to a file inside a directory.
void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

/// Build minimal actor.json with auth.keyfile configured.
/// NOTE: auth is nested inside the "actor" block, not at top-level.
std::string make_actor_json_with_keyfile(const fs::path& keyfile,
                                         const std::string& uid  = "ACTOR-TEST-AABBCCDD",
                                         const std::string& name = "TestActor")
{
    nlohmann::json j;
    j["actor"]["uid"]              = uid;
    j["actor"]["name"]             = name;
    j["actor"]["auth"]["keyfile"]  = keyfile.string();
    j["roles"]                     = nlohmann::json::object();
    return j.dump(2);
}

/// Build minimal actor.json WITHOUT auth.keyfile.
std::string make_actor_json_no_keyfile(const std::string& uid  = "ACTOR-TEST-AABBCCDD",
                                       const std::string& name = "TestActor")
{
    nlohmann::json j;
    j["actor"]["uid"]  = uid;
    j["actor"]["name"] = name;
    j["roles"]         = nlohmann::json::object();
    return j.dump(2);
}

/// Build minimal hub.json.
std::string make_hub_json(const std::string& name = "test.hub",
                           const std::string& uid  = "HUB-TESTHUB-12345678")
{
    nlohmann::json j;
    j["hub"]["name"] = name;
    j["hub"]["uid"]  = uid;
    return j.dump(2);
}

} // anonymous namespace

// ============================================================================
// --keygen tests
// ============================================================================

/// --keygen with a valid config creates an encrypted vault and prints the public key.
TEST(ActorCliKeygen, WritesJsonKeypair)
{
    TmpDir tmp;
    const fs::path keyfile = tmp.path() / "actor.key";
    const fs::path cfg     = tmp.path() / "actor.json";
    write_file(cfg, make_actor_json_with_keyfile(keyfile));

    // Supply empty password via env var so --keygen does not prompt interactively.
    ::setenv("PYLABHUB_ACTOR_PASSWORD", "", 1);

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string(), "--keygen"});
    ASSERT_TRUE(proc.valid()) << "actor binary not found at: " << actor_bin();
    const int rc = proc.wait_for_exit();
    EXPECT_EQ(rc, 0)
        << "stdout: " << proc.get_stdout() << "\nstderr: " << proc.get_stderr();

    // Vault file must exist (binary format — not JSON).
    ASSERT_TRUE(fs::exists(keyfile)) << "vault file was not created at " << keyfile;
    EXPECT_GT(fs::file_size(keyfile), std::uintmax_t{0});

    // Open the vault with the same empty password to verify contents.
    const auto vault = pylabhub::utils::ActorVault::open(keyfile, "ACTOR-TEST-AABBCCDD", "");
    EXPECT_EQ(vault.actor_uid(), "ACTOR-TEST-AABBCCDD");
    EXPECT_EQ(vault.public_key().size(), 40u) << "public_key must be 40-char Z85";
    EXPECT_EQ(vault.secret_key().size(), 40u) << "secret_key must be 40-char Z85";

    // stdout must confirm the write.
    EXPECT_NE(proc.get_stdout().find("Actor vault written to"), std::string::npos)
        << "stdout: " << proc.get_stdout();
}

/// --keygen with no auth.keyfile in config must print an error and exit non-zero.
TEST(ActorCliKeygen, MissingKeyfileFieldErrors)
{
    TmpDir tmp;
    const fs::path cfg = tmp.path() / "actor.json";
    write_file(cfg, make_actor_json_no_keyfile());

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string(), "--keygen"});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("keyfile"), std::string::npos)
        << "expected 'keyfile' in stderr; got: " << proc.get_stderr();
}

/// --keygen creates parent directories for the keyfile if they don't already exist.
TEST(ActorCliKeygen, CreatesParentDirectoryIfNeeded)
{
    TmpDir tmp;
    // Nested key path — parent directory does not yet exist.
    const fs::path keyfile = tmp.path() / "security" / "keys" / "actor.key";
    const fs::path cfg     = tmp.path() / "actor.json";
    write_file(cfg, make_actor_json_with_keyfile(keyfile));

    ::setenv("PYLABHUB_ACTOR_PASSWORD", "", 1);

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string(), "--keygen"});
    ASSERT_TRUE(proc.valid());
    EXPECT_EQ(proc.wait_for_exit(), 0) << "stderr: " << proc.get_stderr();
    EXPECT_TRUE(fs::exists(keyfile)) << "vault file not found at " << keyfile;
}

/// Running --keygen twice on the same keyfile path overwrites the existing vault.
TEST(ActorCliKeygen, OverwritesExistingKeyfile)
{
    TmpDir tmp;
    const fs::path keyfile = tmp.path() / "actor.key";
    const fs::path cfg     = tmp.path() / "actor.json";
    write_file(cfg, make_actor_json_with_keyfile(keyfile));

    ::setenv("PYLABHUB_ACTOR_PASSWORD", "", 1);

    // First keygen.
    WorkerProcess proc1(actor_bin().string(), "--config", {cfg.string(), "--keygen"});
    ASSERT_TRUE(proc1.valid());
    ASSERT_EQ(proc1.wait_for_exit(), 0);
    const std::string first_pub =
        pylabhub::utils::ActorVault::open(keyfile, "ACTOR-TEST-AABBCCDD", "").public_key();

    // Second keygen — new keypair (different keys expected).
    WorkerProcess proc2(actor_bin().string(), "--config", {cfg.string(), "--keygen"});
    ASSERT_TRUE(proc2.valid());
    ASSERT_EQ(proc2.wait_for_exit(), 0);
    const std::string second_pub =
        pylabhub::utils::ActorVault::open(keyfile, "ACTOR-TEST-AABBCCDD", "").public_key();

    // Keys must differ (two independent CSPRNG draws).
    EXPECT_NE(first_pub, second_pub)
        << "identical public keys from two independent keygen calls is extremely unlikely";
}

// ============================================================================
// --register-with tests
// ============================================================================

/// --register-with appends an actor entry to hub.json known_actors.
TEST(ActorCliRegister, AppendsActorToHubJson)
{
    TmpDir actor_dir;
    TmpDir hub_dir;

    const std::string uid  = "ACTOR-REGTEST-11223344";
    const std::string name = "RegTestActor";
    write_file(actor_dir.path() / "actor.json", make_actor_json_no_keyfile(uid, name));
    write_file(hub_dir.path()   / "hub.json",   make_hub_json());

    WorkerProcess proc(actor_bin().string(), "--register-with",
                       {hub_dir.path().string(), actor_dir.path().string()});
    ASSERT_TRUE(proc.valid()) << "actor binary not found at: " << actor_bin();
    EXPECT_EQ(proc.wait_for_exit(), 0) << "stderr: " << proc.get_stderr();

    // hub.json must contain exactly one entry with the actor's uid and name.
    std::ifstream hf(hub_dir.path() / "hub.json");
    const auto hj = nlohmann::json::parse(hf);
    ASSERT_TRUE(hj.contains("hub"));
    ASSERT_TRUE(hj.at("hub").contains("known_actors"));
    const auto& actors = hj.at("hub").at("known_actors");
    ASSERT_EQ(actors.size(), 1u);
    EXPECT_EQ(actors[0].value("uid",  ""), uid);
    EXPECT_EQ(actors[0].value("name", ""), name);

    // stdout must confirm registration.
    EXPECT_NE(proc.get_stdout().find("Registered actor"), std::string::npos)
        << "stdout: " << proc.get_stdout();
}

/// Registering the same actor uid twice is idempotent: exit 0, no duplicate entry.
TEST(ActorCliRegister, DuplicateRegistrationIsIdempotent)
{
    TmpDir actor_dir;
    TmpDir hub_dir;

    const std::string uid = "ACTOR-DUP-AABBCCDD";
    write_file(actor_dir.path() / "actor.json", make_actor_json_no_keyfile(uid, "DupActor"));

    // Hub already has this actor.
    {
        nlohmann::json hj;
        hj["hub"]["name"] = "test.hub";
        hj["hub"]["uid"]  = "HUB-TESTHUB-12345678";
        nlohmann::json entry;
        entry["uid"]  = uid;
        entry["name"] = "DupActor";
        entry["role"] = "any";
        hj["hub"]["known_actors"] = nlohmann::json::array({entry});
        write_file(hub_dir.path() / "hub.json", hj.dump(2));
    }

    WorkerProcess proc(actor_bin().string(), "--register-with",
                       {hub_dir.path().string(), actor_dir.path().string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_EQ(proc.wait_for_exit(), 0) << "stderr: " << proc.get_stderr();

    // known_actors must still have exactly one entry.
    std::ifstream hf(hub_dir.path() / "hub.json");
    const auto hj2 = nlohmann::json::parse(hf);
    EXPECT_EQ(hj2.at("hub").at("known_actors").size(), 1u);
}

/// --register-with with a missing actor.json must exit non-zero with an error.
TEST(ActorCliRegister, MissingActorJsonErrors)
{
    TmpDir actor_dir;  // no actor.json created
    TmpDir hub_dir;
    write_file(hub_dir.path() / "hub.json", make_hub_json());

    WorkerProcess proc(actor_bin().string(), "--register-with",
                       {hub_dir.path().string(), actor_dir.path().string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("actor.json"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}

/// --register-with with a missing hub.json must exit non-zero with an error.
TEST(ActorCliRegister, MissingHubJsonErrors)
{
    TmpDir actor_dir;
    TmpDir hub_dir;  // no hub.json created
    write_file(actor_dir.path() / "actor.json",
               make_actor_json_no_keyfile("ACTOR-TEST-AABBCCDD", "TestActor"));

    WorkerProcess proc(actor_bin().string(), "--register-with",
                       {hub_dir.path().string(), actor_dir.path().string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("hub.json"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}

// ============================================================================
// Config error cases (exit before Python / lifecycle starts)
// ============================================================================

/// A completely malformed JSON config must print "Config error" and exit non-zero.
TEST(ActorCliConfigError, MalformedJsonErrors)
{
    TmpDir tmp;
    const fs::path cfg = tmp.path() / "actor.json";
    write_file(cfg, "{ not : valid json }");

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}

/// Missing "roles" key (the only mandatory top-level key) must print "Config error".
TEST(ActorCliConfigError, MissingRolesKeyErrors)
{
    TmpDir tmp;
    const fs::path cfg = tmp.path() / "actor.json";
    nlohmann::json j;
    j["actor"]["uid"] = "ACTOR-TEST-AABBCCDD";
    write_file(cfg, j.dump(2));

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}

/// An unrecognised role kind ("relay") must cause a "Config error".
TEST(ActorCliConfigError, InvalidRoleKindErrors)
{
    TmpDir tmp;
    const fs::path cfg = tmp.path() / "actor.json";
    nlohmann::json j;
    j["actor"]["uid"]            = "ACTOR-TEST-AABBCCDD";
    j["roles"]["out"]["kind"]    = "relay";
    j["roles"]["out"]["channel"] = "lab.test";
    write_file(cfg, j.dump(2));

    WorkerProcess proc(actor_bin().string(), "--config", {cfg.string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}

/// A config file that doesn't exist must print "Config error" and exit non-zero.
TEST(ActorCliConfigError, FileNotFoundErrors)
{
    const fs::path nonexistent =
        fs::temp_directory_path() / "actor_cli_test_NONEXISTENT.json";

    WorkerProcess proc(actor_bin().string(), "--config", {nonexistent.string()});
    ASSERT_TRUE(proc.valid());
    EXPECT_NE(proc.wait_for_exit(), 0);
    EXPECT_NE(proc.get_stderr().find("Config error"), std::string::npos)
        << "stderr: " << proc.get_stderr();
}
