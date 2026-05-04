/**
 * @file hub_config_workers.cpp
 * @brief Worker bodies for the HubConfig L2 test suite (Pattern 3).
 *
 * Each worker takes a parent-provided unique scratch directory, writes a
 * `hub.json` inside it, calls HubConfig::load* to parse it, and asserts on
 * the resulting fields.  run_gtest_worker owns the LifecycleGuard
 * (Logger + FileLock + JsonConfig), matching the role_config worker shape.
 */
#include "hub_config_workers.h"

#include "utils/config/hub_config.hpp"
#include "utils/config/hub_state_config.hpp"  // for kInfiniteGrace sentinel
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using pylabhub::config::HubConfig;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace hub_config
{
namespace
{

/// Write @p content as JSON to <dir>/hub.json; return the full path.
fs::path write_hub_json(const std::string &dir, const nlohmann::json &content)
{
    fs::path full = fs::path(dir) / "hub.json";
    std::ofstream f(full);
    f << content.dump(2);
    f.close();
    return full;
}

/// Minimal config with just enough for HubConfig::load to succeed.
/// `loop_timing` is required by `parse_timing_config` (HEP-CORE-0033
/// Phase 7 Commit B); use `max_rate` here to avoid pulling in a
/// `target_period_ms` value that the minimal-config tests don't care
/// about.
nlohmann::json minimal_hub_json(const std::string &uid = "hub.test.uid00000001")
{
    return {
        {"hub", {{"uid", uid}, {"name", "TestHub"}}},
        {"loop_timing", "max_rate"},
    };
}

/// Full config exercising every section.
nlohmann::json full_hub_json()
{
    return {
        {"hub", {
            {"uid", "hub.full.uid00000002"},
            {"name", "FullHub"},
            {"log_level", "warn"},
            {"auth", {{"keyfile", "vault/hub.vault"}}},
        }},
        {"script", {{"type", "python"}, {"path", "."}}},
        {"stop_on_script_error", true},
        {"loop_timing", "fixed_rate"},
        {"target_period_ms", 1000},
        {"logging", {{"file_path", ""}, {"max_size_mb", 20}, {"backups", 3}}},
        {"network", {
            {"broker_endpoint", "tcp://0.0.0.0:5571"},
            {"broker_bind", true},
            {"zmq_io_threads", 2},
        }},
        {"admin", {
            {"enabled", true},
            {"endpoint", "tcp://127.0.0.1:5601"},
            {"token_required", true},
        }},
        {"broker", {
            {"heartbeat_interval_ms",    250},
            {"ready_miss_heartbeats",     12},
            {"pending_miss_heartbeats",    8},
            {"grace_heartbeats",           2},
            {"ready_timeout_ms",        4000},  // explicit override
        }},
        {"federation", {
            {"enabled", true},
            {"forward_timeout_ms", 1500},
            {"peers", nlohmann::json::array({
                {{"uid", "hub.peer.uidaabbccdd"},
                 {"endpoint", "tcp://10.0.0.2:5570"},
                 {"pubkey", ""}},
            })},
        }},
        {"state", {{"disconnected_grace_ms", 30000},
                    {"max_disconnected_entries", 500}}},
    };
}

} // anonymous namespace

// ── load_full ────────────────────────────────────────────────────────────────

int load_full(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            write_hub_json(dir, full_hub_json());
            auto cfg = HubConfig::load_from_directory(dir);

            EXPECT_EQ(cfg.identity().uid,        "hub.full.uid00000002");
            EXPECT_EQ(cfg.identity().name,       "FullHub");
            EXPECT_EQ(cfg.identity().log_level,  "warn");
            EXPECT_EQ(cfg.auth().keyfile,        "vault/hub.vault");

            EXPECT_EQ(cfg.network().broker_endpoint, "tcp://0.0.0.0:5571");
            EXPECT_TRUE(cfg.network().broker_bind);
            EXPECT_EQ(cfg.network().zmq_io_threads, 2);

            EXPECT_TRUE(cfg.admin().enabled);
            EXPECT_EQ(cfg.admin().endpoint, "tcp://127.0.0.1:5601");
            EXPECT_TRUE(cfg.admin().token_required);

            EXPECT_EQ(cfg.broker().heartbeat_interval_ms,     250);
            EXPECT_EQ(cfg.broker().ready_miss_heartbeats,      12u);
            EXPECT_EQ(cfg.broker().pending_miss_heartbeats,     8u);
            EXPECT_EQ(cfg.broker().grace_heartbeats,            2u);
            ASSERT_TRUE(cfg.broker().ready_timeout_ms.has_value());
            EXPECT_EQ(*cfg.broker().ready_timeout_ms, 4000);
            EXPECT_FALSE(cfg.broker().pending_timeout_ms.has_value());
            EXPECT_FALSE(cfg.broker().grace_ms.has_value());

            EXPECT_TRUE(cfg.federation().enabled);
            EXPECT_EQ(cfg.federation().forward_timeout_ms, 1500);
            ASSERT_EQ(cfg.federation().peers.size(), 1u);
            EXPECT_EQ(cfg.federation().peers[0].uid, "hub.peer.uidaabbccdd");
            EXPECT_EQ(cfg.federation().peers[0].endpoint, "tcp://10.0.0.2:5570");

            EXPECT_EQ(cfg.state().disconnected_grace_ms,    30000);
            EXPECT_EQ(cfg.state().max_disconnected_entries,   500);

            // Timing — full config sets fixed_rate at 1000 ms.
            // HEP-CORE-0033 Phase 7 Commit B: hub script tick uses
            // the same TimingConfig + parser as role data loops.
            EXPECT_EQ(cfg.timing().loop_timing,
                      ::pylabhub::LoopTimingPolicy::FixedRate);
            EXPECT_EQ(cfg.timing().period_us, 1000.0 * 1000.0);  // 1000 ms in µs

            // base_dir() returns the directory containing hub.json.
            EXPECT_EQ(cfg.base_dir(), fs::path(dir));

            // raw() returns the JSON we wrote.
            EXPECT_EQ(cfg.raw().at("hub").at("name").get<std::string>(), "FullHub");
        },
        "hub_config::load_full",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── load_minimal ─────────────────────────────────────────────────────────────

int load_minimal(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            write_hub_json(dir, minimal_hub_json());
            auto cfg = HubConfig::load_from_directory(dir);

            // Defaults take over for absent sections (HEP-0033 §6.3).
            EXPECT_EQ(cfg.network().broker_endpoint, "tcp://0.0.0.0:5570");
            EXPECT_TRUE(cfg.network().broker_bind);
            EXPECT_EQ(cfg.network().zmq_io_threads, 1);

            EXPECT_TRUE(cfg.admin().enabled);
            EXPECT_EQ(cfg.admin().endpoint, "tcp://127.0.0.1:5600");
            EXPECT_TRUE(cfg.admin().token_required);

            EXPECT_EQ(cfg.broker().heartbeat_interval_ms,
                      ::pylabhub::kDefaultHeartbeatIntervalMs);
            EXPECT_EQ(cfg.broker().ready_miss_heartbeats,
                      ::pylabhub::kDefaultReadyMissHeartbeats);
            EXPECT_EQ(cfg.broker().pending_miss_heartbeats,
                      ::pylabhub::kDefaultPendingMissHeartbeats);
            EXPECT_EQ(cfg.broker().grace_heartbeats,
                      ::pylabhub::kDefaultGraceHeartbeats);
            EXPECT_FALSE(cfg.broker().ready_timeout_ms.has_value());
            EXPECT_FALSE(cfg.broker().pending_timeout_ms.has_value());
            EXPECT_FALSE(cfg.broker().grace_ms.has_value());

            EXPECT_FALSE(cfg.federation().enabled);
            EXPECT_TRUE(cfg.federation().peers.empty());
            EXPECT_EQ(cfg.federation().forward_timeout_ms, 2000);

            // Timing — minimal config sets max_rate (no period field).
            EXPECT_EQ(cfg.timing().loop_timing,
                      ::pylabhub::LoopTimingPolicy::MaxRate);
            EXPECT_EQ(cfg.timing().period_us, 0.0);

            EXPECT_EQ(cfg.state().disconnected_grace_ms,    60000);
            EXPECT_EQ(cfg.state().max_disconnected_entries,  1000);
        },
        "hub_config::load_minimal",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── strict_unknown_top_level ────────────────────────────────────────────────

int strict_unknown_top_level(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            auto j = minimal_hub_json();
            j["bogus_top_level"] = 7;
            write_hub_json(dir, j);

            try {
                HubConfig::load_from_directory(dir);
                FAIL() << "Expected std::runtime_error for unknown top-level key";
            } catch (const std::runtime_error &ex) {
                EXPECT_THAT(ex.what(), testing::HasSubstr("unknown config key"));
                EXPECT_THAT(ex.what(), testing::HasSubstr("bogus_top_level"));
            }
        },
        "hub_config::strict_unknown_top_level",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── strict_unknown_in_section ───────────────────────────────────────────────

int strict_unknown_in_section(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            auto j = minimal_hub_json();
            j["admin"] = {{"enabled", true}, {"bogus_admin_key", 1}};
            write_hub_json(dir, j);

            try {
                HubConfig::load_from_directory(dir);
                FAIL() << "Expected std::runtime_error for unknown admin sub-key";
            } catch (const std::runtime_error &ex) {
                // Diagnostic must qualify the path so the operator can find it.
                EXPECT_THAT(ex.what(), testing::HasSubstr("admin.bogus_admin_key"));
            }
        },
        "hub_config::strict_unknown_in_section",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── section_not_object ──────────────────────────────────────────────────────

int section_not_object(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            auto j = minimal_hub_json();
            j["network"] = "this is not an object";
            write_hub_json(dir, j);

            try {
                HubConfig::load_from_directory(dir);
                FAIL() << "Expected std::runtime_error for non-object 'network'";
            } catch (const std::runtime_error &ex) {
                EXPECT_THAT(ex.what(), testing::HasSubstr("'network'"));
                EXPECT_THAT(ex.what(), testing::HasSubstr("must be an object"));
            }
        },
        "hub_config::section_not_object",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── uid_auto_generated ──────────────────────────────────────────────────────

int uid_auto_generated(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            // Omit hub.uid; provide just hub.name so generator has a base.
            // loop_timing is required by parse_timing_config — same shape
            // as minimal_hub_json above; max_rate keeps the fixture small.
            const nlohmann::json j = {
                {"hub", {{"name", "MyHub"}}},
                {"loop_timing", "max_rate"},
            };
            write_hub_json(dir, j);

            auto cfg = HubConfig::load_from_directory(dir);

            // Auto-generated uid must follow HEP-0033 §G2.2.0a form
            // `hub.<sanitized-name>.uid<8hex>`.
            const auto &uid = cfg.identity().uid;
            EXPECT_THAT(uid, testing::StartsWith("hub."));
            EXPECT_THAT(uid, testing::HasSubstr(".uid"));
            // tag.name.uid<8hex> — at least 3 components separated by dots.
            EXPECT_GE(std::count(uid.begin(), uid.end(), '.'), 2);
            // Hex suffix length is 8 → total tail is "uid" + 8 = 11 chars.
            EXPECT_GE(uid.size(), std::string("hub.x.uid01234567").size());
        },
        "hub_config::uid_auto_generated",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── state_grace_sentinel ────────────────────────────────────────────────────

int state_grace_sentinel(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            auto j = minimal_hub_json();
            j["state"] = {{"disconnected_grace_ms", -1}};
            write_hub_json(dir, j);

            auto cfg = HubConfig::load_from_directory(dir);
            EXPECT_EQ(cfg.state().disconnected_grace_ms,
                      pylabhub::config::kInfiniteGrace);
        },
        "hub_config::state_grace_sentinel",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── load_from_directory ─────────────────────────────────────────────────────

int load_from_directory(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            auto path = write_hub_json(dir, minimal_hub_json("hub.dir.uid00000003"));

            // Both factories should produce equivalent state.
            auto by_path = HubConfig::load(path.string());
            auto by_dir  = HubConfig::load_from_directory(dir);

            EXPECT_EQ(by_path.identity().uid, by_dir.identity().uid);
            EXPECT_EQ(by_path.base_dir(), by_dir.base_dir());
            EXPECT_EQ(by_path.base_dir(), fs::path(dir));
        },
        "hub_config::load_from_directory",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── reload_if_changed ───────────────────────────────────────────────────────

int reload_if_changed(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;
            write_hub_json(dir, minimal_hub_json("hub.rel.uid00000004"));
            auto cfg = HubConfig::load_from_directory(dir);
            EXPECT_EQ(cfg.identity().name, "TestHub");

            // No-change: reload returns false, state unchanged.
            EXPECT_FALSE(cfg.reload_if_changed());
            EXPECT_EQ(cfg.identity().name, "TestHub");

            // Modify the file (sleep 1.1s for filesystem mtime resolution
            // — JsonConfig::reload uses mtime to detect changes).
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            auto j2 = minimal_hub_json("hub.rel.uid00000004");
            j2["hub"]["name"] = "RenamedHub";
            write_hub_json(dir, j2);

            EXPECT_TRUE(cfg.reload_if_changed());
            EXPECT_EQ(cfg.identity().name, "RenamedHub");
        },
        "hub_config::reload_if_changed",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── init_template_loads_via_hubconfig ───────────────────────────────────────

int init_template_loads_via_hubconfig(const char *tmpdir)
{
    return run_gtest_worker(
        [&]() {
            const std::string dir = tmpdir;

            // 1. HubDirectory::init_directory writes the template.
            const int rc = pylabhub::utils::HubDirectory::init_directory(
                dir, "RoundTripHub");
            ASSERT_EQ(rc, 0);

            // 2. HubConfig::load_from_directory must parse it without throwing.
            //    This locks in the contract that the template's keys match
            //    the parser's strict whitelist (top level + every sub-section).
            HubConfig cfg = HubConfig::load_from_directory(dir);

            // 3. Spot-check fields the template populates.
            EXPECT_EQ(cfg.identity().name, "RoundTripHub");
            EXPECT_FALSE(cfg.identity().uid.empty());
            EXPECT_EQ(cfg.network().broker_endpoint, "tcp://0.0.0.0:5570");
            EXPECT_TRUE(cfg.network().broker_bind);
            EXPECT_EQ(cfg.broker().heartbeat_interval_ms,
                      ::pylabhub::kDefaultHeartbeatIntervalMs);
            EXPECT_EQ(cfg.broker().ready_miss_heartbeats,
                      ::pylabhub::kDefaultReadyMissHeartbeats);
            EXPECT_FALSE(cfg.federation().enabled);
            EXPECT_EQ(cfg.state().disconnected_grace_ms, 60000);
        },
        "hub_config::init_template_loads_via_hubconfig",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

} // namespace hub_config
} // namespace pylabhub::tests::worker

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct HubConfigWorkerRegistrar
{
    HubConfigWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "hub_config")
                    return -1;
                std::string sc(mode.substr(dot + 1));

                if (argc <= 2) {
                    fmt::print(stderr,
                               "hub_config.{}: missing required <dir> arg\n", sc);
                    return 1;
                }
                const char *dir = argv[2];

                using namespace pylabhub::tests::worker::hub_config;
                if (sc == "load_full")                 return load_full(dir);
                if (sc == "load_minimal")              return load_minimal(dir);
                if (sc == "strict_unknown_top_level")  return strict_unknown_top_level(dir);
                if (sc == "strict_unknown_in_section") return strict_unknown_in_section(dir);
                if (sc == "section_not_object")        return section_not_object(dir);
                if (sc == "uid_auto_generated")        return uid_auto_generated(dir);
                if (sc == "state_grace_sentinel")      return state_grace_sentinel(dir);
                if (sc == "load_from_directory")       return load_from_directory(dir);
                if (sc == "reload_if_changed")         return reload_if_changed(dir);
                if (sc == "init_template_loads_via_hubconfig")
                    return init_template_loads_via_hubconfig(dir);
                fmt::print(stderr, "hub_config: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};

static HubConfigWorkerRegistrar g_hub_config_registrar; // NOLINT(cert-err58-cpp)

} // namespace
