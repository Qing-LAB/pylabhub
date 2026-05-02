/**
 * @file role_logging_workers.cpp
 * @brief Worker implementations for role logging tests (Pattern 3).
 *
 * Each worker:
 *   1. Wraps its body in run_gtest_worker() which owns the LifecycleGuard
 *      (Logger + FileLock + JsonConfig) for this subprocess.
 *   2. Registers the three producer/consumer/processor role init entries
 *      inside the worker body — the registry is process-global and a fresh
 *      subprocess starts empty.
 *   3. Operates on a parent-provided unique directory; the parent cleans
 *      it up after wait_for_exit(). No shared state leaks between tests.
 */
#include "role_logging_workers.h"

#include "consumer_init.hpp"
#include "processor_init.hpp"
#include "producer_fields.hpp"
#include "producer_init.hpp"
#include "consumer_fields.hpp"
#include "processor_fields.hpp"

#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/config/logging_config.hpp"
#include "utils/config/role_config.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_main_helpers.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::MakeModDefList;
using pylabhub::utils::RoleDirectory;
using pylabhub::config::LoggingConfig;
using pylabhub::config::RoleConfig;

namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace role_logging
{
namespace
{

/// Ensures producer/consumer/processor init entries are registered exactly
/// once per subprocess. The registry is a process-global map in RoleDirectory;
/// re-registering the same tag throws, so guard with a static bool.
void register_all_roles_once()
{
    static bool registered = false;
    if (registered)
        return;
    pylabhub::producer::register_producer_init();
    pylabhub::consumer::register_consumer_init();
    pylabhub::processor::register_processor_init();
    registered = true;
}

/// Selects the role parser matching @p role ("producer"/"consumer"/"processor").
/// Throws std::runtime_error on unknown role so a test error surfaces clearly.
auto get_role_parser(const std::string &role)
{
    using Parser = RoleConfig::RoleParser;
    if (role == "producer")
        return Parser(pylabhub::producer::parse_producer_fields);
    if (role == "consumer")
        return Parser(pylabhub::consumer::parse_consumer_fields);
    if (role == "processor")
        return Parser(pylabhub::processor::parse_processor_fields);
    throw std::runtime_error("Unknown role: " + role);
}

} // namespace

// ── Roundtrip workers ────────────────────────────────────────────────────────

int roundtrip_defaults_preserved(const std::string &dir, const std::string &role)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X"), 0);
            auto cfg = RoleConfig::load_from_directory(dir, role.c_str(), get_role_parser(role));
            const auto &lc = cfg.logging();
            EXPECT_EQ(lc.max_size_bytes, 10ULL * 1024 * 1024);
            EXPECT_EQ(lc.max_backup_files, 5u);
            EXPECT_TRUE(lc.timestamped);
            EXPECT_TRUE(lc.file_path.empty());
        },
        "role_logging::roundtrip_defaults_preserved",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int roundtrip_max_size(const std::string &dir, const std::string &role,
                       double max_size_mb, std::size_t expected_bytes)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.max_size_mb = max_size_mb;
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X", ov), 0);
            auto cfg = RoleConfig::load_from_directory(dir, role.c_str(), get_role_parser(role));
            EXPECT_EQ(cfg.logging().max_size_bytes, expected_bytes);
        },
        "role_logging::roundtrip_max_size",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int roundtrip_backups(const std::string &dir, const std::string &role, int backups,
                      std::size_t expected_count)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.backups = backups;
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X", ov), 0);
            auto cfg = RoleConfig::load_from_directory(dir, role.c_str(), get_role_parser(role));
            EXPECT_EQ(cfg.logging().max_backup_files, expected_count);
        },
        "role_logging::roundtrip_backups",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int roundtrip_both(const std::string &dir, const std::string &role, double max_size_mb,
                   int backups, std::size_t expected_bytes, std::size_t expected_count)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.max_size_mb = max_size_mb;
            ov.backups     = backups;
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X", ov), 0);
            auto cfg = RoleConfig::load_from_directory(dir, role.c_str(), get_role_parser(role));
            EXPECT_EQ(cfg.logging().max_size_bytes, expected_bytes);
            EXPECT_EQ(cfg.logging().max_backup_files, expected_count);
            EXPECT_TRUE(cfg.logging().timestamped);
        },
        "role_logging::roundtrip_both",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// Helper: assert `RoleConfig::load_from_directory` throws a
// `std::runtime_error` whose what() contains @p needle.  Tightens the
// EXPECT_THROW Class A check per audit §1.1: LoggingConfig::parse has
// distinct runtime_error throw sites for each kind of bad input
// (logging_config.hpp:70/81/94/114) — type alone could let a regression
// where check A fails to fire and check B fires instead pass silently.
static void expect_load_runtime_error(const std::string &dir,
                                       const std::string &role,
                                       std::string_view needle)
{
    bool threw = false; std::string msg;
    try {
        (void)RoleConfig::load_from_directory(dir, role.c_str(),
                                              get_role_parser(role));
    } catch (const std::runtime_error &e) { threw = true; msg = e.what(); }
    EXPECT_TRUE(threw)
        << "load_from_directory must throw runtime_error for invalid logging config";
    EXPECT_NE(msg.find(needle), std::string::npos)
        << "wrong runtime_error path; expected substring '" << needle
        << "', got: " << msg;
}

int roundtrip_error_backups_zero(const std::string &dir, const std::string &role)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.backups = 0;
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X", ov), 0);
            expect_load_runtime_error(dir, role,
                                      "'logging.backups' must be >= 1");
        },
        "role_logging::roundtrip_error_backups_zero",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int roundtrip_error_maxsize_zero(const std::string &dir, const std::string &role)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.max_size_mb = 0.0;
            ASSERT_EQ(RoleDirectory::init_directory(dir, role, "X", ov), 0);
            expect_load_runtime_error(dir, role,
                                      "'logging.max_size_mb' must be > 0");
        },
        "role_logging::roundtrip_error_maxsize_zero",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── configure_logger_from_config workers ────────────────────────────────────

int configure_auto_composed_path(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);
            auto cfg = RoleConfig::load_from_directory(dir, "producer",
                                                      get_role_parser("producer"));
            const std::string uid = cfg.identity().uid;

            std::error_code ec;
            const bool ok =
                pylabhub::scripting::configure_logger_from_config(cfg, ec, "[test]");
            ASSERT_TRUE(ok) << "configure_logger_from_config failed: " << ec.message();

            LOGGER_INFO("[test] auto-path probe");
            Logger::instance().flush();

            const fs::path logs_dir = fs::path(dir) / "logs";
            ASSERT_TRUE(fs::is_directory(logs_dir));

            bool found = false;
            const std::regex re(
                "^" + uid +
                R"(-\d{4}-\d{2}-\d{2}-\d{2}-\d{2}-\d{2}\.\d+\.log$)");
            for (const auto &ent : fs::directory_iterator(logs_dir))
            {
                if (std::regex_match(ent.path().filename().string(), re))
                {
                    found = true;
                    EXPECT_GT(fs::file_size(ent.path()), 0u);
                    break;
                }
            }
            EXPECT_TRUE(found) << "No <uid>-<ts>.log under " << logs_dir.string();

            // Return logger to console so LifecycleGuard teardown doesn't write
            // into a dir the parent is about to rm -r.
            (void)Logger::instance().set_console();
            Logger::instance().flush();
        },
        "role_logging::configure_auto_composed_path",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int configure_rotation_params(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.max_size_mb = 1.0;
            ov.backups     = 3;
            ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
            auto cfg = RoleConfig::load_from_directory(dir, "producer",
                                                      get_role_parser("producer"));

            EXPECT_EQ(cfg.logging().max_size_bytes, 1ULL * 1024 * 1024);
            EXPECT_EQ(cfg.logging().max_backup_files, 3u);
            EXPECT_TRUE(cfg.logging().timestamped);

            std::error_code ec;
            EXPECT_TRUE(pylabhub::scripting::configure_logger_from_config(cfg, ec,
                                                                         "[test]"));

            (void)Logger::instance().set_console();
            Logger::instance().flush();
        },
        "role_logging::configure_rotation_params",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int configure_keep_all_sentinel(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            RoleDirectory::LogInitOverrides ov;
            ov.backups = -1;
            ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X", ov), 0);
            auto cfg = RoleConfig::load_from_directory(dir, "producer",
                                                      get_role_parser("producer"));
            EXPECT_EQ(cfg.logging().max_backup_files, LoggingConfig::kKeepAllBackups);

            std::error_code ec;
            EXPECT_TRUE(pylabhub::scripting::configure_logger_from_config(cfg, ec,
                                                                         "[test]"));

            (void)Logger::instance().set_console();
            Logger::instance().flush();
        },
        "role_logging::configure_keep_all_sentinel",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int configure_unwritable_dir(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
#if defined(__unix__) || defined(__APPLE__)
            register_all_roles_once();
            ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);
            auto cfg = RoleConfig::load_from_directory(dir, "producer",
                                                      get_role_parser("producer"));

            const fs::path role_dir(dir);
            const fs::path logs = role_dir / "logs";
            ASSERT_TRUE(fs::remove_all(logs));
            fs::permissions(role_dir,
                            fs::perms::owner_read | fs::perms::owner_exec,
                            fs::perm_options::replace);

            std::error_code ec;
            const bool ok =
                pylabhub::scripting::configure_logger_from_config(cfg, ec, "[test]");

            // Restore perms so the parent's rm -r can succeed.
            fs::permissions(role_dir, fs::perms::owner_all,
                            fs::perm_options::replace);

            EXPECT_FALSE(ok);
            EXPECT_TRUE(ec) << "expected non-empty error_code on pre-flight failure";

            (void)Logger::instance().set_console();
            Logger::instance().flush();
#else
            (void)dir;
            GTEST_SKIP() << "POSIX-only permission test";
#endif
        },
        "role_logging::configure_unwritable_dir",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int configure_explicit_file_path(const std::string &dir)
{
    return run_gtest_worker(
        [&]()
        {
            register_all_roles_once();
            ASSERT_EQ(RoleDirectory::init_directory(dir, "producer", "X"), 0);

            const fs::path role_dir(dir);
            const fs::path explicit_log = role_dir / "logs" / "custom_name.log";
            {
                std::ifstream in(role_dir / "producer.json");
                nlohmann::json j = nlohmann::json::parse(in);
                j["logging"]["file_path"]  = explicit_log.string();
                j["logging"]["timestamped"] = false;
                in.close();
                std::ofstream out(role_dir / "producer.json");
                out << j.dump(2);
            }

            auto cfg = RoleConfig::load_from_directory(dir, "producer",
                                                      get_role_parser("producer"));
            EXPECT_EQ(cfg.logging().file_path, explicit_log.string());

            std::error_code ec;
            ASSERT_TRUE(pylabhub::scripting::configure_logger_from_config(cfg, ec,
                                                                         "[test]"))
                << ec.message();

            LOGGER_INFO("[test] explicit-path probe");
            Logger::instance().flush();

            EXPECT_TRUE(fs::exists(explicit_log))
                << "expected explicit log at " << explicit_log.string();

            (void)Logger::instance().set_console();
            Logger::instance().flush();
        },
        "role_logging::configure_explicit_file_path",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

} // namespace role_logging
} // namespace pylabhub::tests::worker

// ── Self-registering dispatcher ─────────────────────────────────────────────

namespace
{

int parse_int_arg(const char *s) { return std::stoi(s); }
double parse_double_arg(const char *s) { return std::stod(s); }
std::size_t parse_size_arg(const char *s)
{
    return static_cast<std::size_t>(std::stoull(s));
}

struct RoleLoggingWorkerRegistrar
{
    RoleLoggingWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_logging")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_logging;

                // All scenarios take at least <dir> as argv[2].
                auto need = [&](int n) -> bool {
                    if (argc < n) {
                        fmt::print(stderr,
                                   "role_logging.{}: expected {} args, got {}\n",
                                   sc, n - 2, argc - 2);
                        return false;
                    }
                    return true;
                };

                if (sc == "roundtrip_defaults_preserved" && need(4))
                    return roundtrip_defaults_preserved(argv[2], argv[3]);
                if (sc == "roundtrip_max_size" && need(6))
                    return roundtrip_max_size(argv[2], argv[3],
                                              parse_double_arg(argv[4]),
                                              parse_size_arg(argv[5]));
                if (sc == "roundtrip_backups" && need(6))
                    return roundtrip_backups(argv[2], argv[3],
                                             parse_int_arg(argv[4]),
                                             parse_size_arg(argv[5]));
                if (sc == "roundtrip_both" && need(8))
                    return roundtrip_both(argv[2], argv[3],
                                          parse_double_arg(argv[4]),
                                          parse_int_arg(argv[5]),
                                          parse_size_arg(argv[6]),
                                          parse_size_arg(argv[7]));
                if (sc == "roundtrip_error_backups_zero" && need(4))
                    return roundtrip_error_backups_zero(argv[2], argv[3]);
                if (sc == "roundtrip_error_maxsize_zero" && need(4))
                    return roundtrip_error_maxsize_zero(argv[2], argv[3]);

                if (sc == "configure_auto_composed_path" && need(3))
                    return configure_auto_composed_path(argv[2]);
                if (sc == "configure_rotation_params" && need(3))
                    return configure_rotation_params(argv[2]);
                if (sc == "configure_keep_all_sentinel" && need(3))
                    return configure_keep_all_sentinel(argv[2]);
                if (sc == "configure_unwritable_dir" && need(3))
                    return configure_unwritable_dir(argv[2]);
                if (sc == "configure_explicit_file_path" && need(3))
                    return configure_explicit_file_path(argv[2]);

                fmt::print(stderr,
                           "[role_logging] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static RoleLoggingWorkerRegistrar g_role_logging_registrar;

} // namespace
