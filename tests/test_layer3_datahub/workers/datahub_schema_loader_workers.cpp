/**
 * @file datahub_schema_loader_workers.cpp
 * @brief Worker bodies for the `load_all_from_dirs` file-walker tests
 *        (HEP-CORE-0034 §2.4 I2; Pattern 3).  Migrated 2026-05-14
 *        from the in-process `SetUpTestSuite`-owned `LifecycleGuard`
 *        antipattern.
 *
 * The 12 plain `TEST(DatahubSchemaParser, …)` bodies in the parent
 * file stay Pattern 1 — they call only the stateless static parser
 * `SchemaLibrary::load_from_string` (and the `generate_schema_info<T>`
 * compile-time macro path) and touch no lifecycle module.  Only the
 * 4 file-walker `TEST_F` bodies migrate here, because
 * `load_all_from_dirs` emits `LOGGER_WARN` on invalid-JSON-skip and
 * duplicate-schema_id-skip — Logger is a lifecycle module so its
 * test paths must run inside a worker subprocess.
 *
 * Module surface: **Logger only** (matches the original
 * SetUpTestSuite exactly).  No FileLock / JsonConfig because
 * `load_all_from_dirs` iterates the filesystem directly via
 * `std::filesystem` and parses JSON via nlohmann without going
 * through the JsonConfig module's file-lock-and-version surface.
 * Smallest module list in the wave.
 *
 * Real production wiring per feedback_test_layering_and_no_mocks.md:
 * real `load_all_from_dirs` walking real temp directories with real
 * JSON files written via `std::ofstream`.  No mocks.
 *
 * @see HEP-CORE-0034 §2.4 I2 (walker entry-point) + I5 (stateless parser)
 * @see src/utils/schema_loader.cpp (load_all_from_dirs implementation)
 */

#include "datahub_schema_loader_workers.h"

#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/logger.hpp"
#include "utils/schema_loader.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace datahub_schema_loader
{

namespace
{

/// Create a per-worker temp directory keyed by tag + pid + counter.
fs::path make_tmp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_schema_loader_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_json(const fs::path &dir, const fs::path &relative,
                const std::string &content)
{
    auto full_path = dir / relative;
    fs::create_directories(full_path.parent_path());
    std::ofstream ofs(full_path);
    ofs << content;
}

void remove_tree(const fs::path &p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

} // namespace

int load_all_from_dirs_single_file()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const fs::path tmpdir = make_tmp_dir("single");
            const std::string schema_json = R"({
                "id": "test.simple",
                "version": 1,
                "slot": {
                    "packing": "aligned",
                    "fields": [{"name": "value", "type": "float32"}]
                }
            })";
            write_json(tmpdir, "test.simple.v1.json", schema_json);

            const auto entries = pylabhub::schema::load_all_from_dirs(
                {tmpdir.string()});
            ASSERT_EQ(entries.size(), 1u);
            EXPECT_EQ(entries[0].second.schema_id, "$test.simple.v1");
            EXPECT_NE(entries[0].first.find("test.simple.v1.json"),
                      std::string::npos);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(tmpdir);
        },
        "datahub_schema_loader::load_all_from_dirs_single_file",
        Logger::GetLifecycleModule());
}

int load_all_from_dirs_nested_path()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const fs::path tmpdir = make_tmp_dir("nested");
            const std::string schema_json = R"({
                "id": "lab.sensors.temperature.raw",
                "version": 1,
                "slot": {
                    "packing": "aligned",
                    "fields": [
                        {"name": "ts", "type": "uint64"},
                        {"name": "temperature", "type": "float64"}
                    ]
                }
            })";
            write_json(tmpdir,
                       fs::path("lab") / "sensors" / "temperature.raw.v1.json",
                       schema_json);

            const auto entries = pylabhub::schema::load_all_from_dirs(
                {tmpdir.string()});
            ASSERT_EQ(entries.size(), 1u);
            EXPECT_EQ(entries[0].second.schema_id,
                      "$lab.sensors.temperature.raw.v1");

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(tmpdir);
        },
        "datahub_schema_loader::load_all_from_dirs_nested_path",
        Logger::GetLifecycleModule());
}

int load_all_from_dirs_invalid_json_skipped()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogWarn("Failed to parse schema file");

            const fs::path tmpdir = make_tmp_dir("invalid_json");
            const std::string valid_json = R"({
                "id": "test.valid",
                "version": 1,
                "slot": {
                    "packing": "aligned",
                    "fields": [{"name": "x", "type": "int32"}]
                }
            })";
            write_json(tmpdir, "valid.json", valid_json);
            write_json(tmpdir, "broken.json", "{ this is not valid JSON }}}");

            const auto entries = pylabhub::schema::load_all_from_dirs(
                {tmpdir.string()});
            ASSERT_EQ(entries.size(), 1u)
                << "Valid schema should still load despite broken "
                   "JSON file";
            EXPECT_EQ(entries[0].second.schema_id, "$test.valid.v1");

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(tmpdir);
        },
        "datahub_schema_loader::load_all_from_dirs_invalid_json_skipped",
        Logger::GetLifecycleModule());
}

int load_all_from_dirs_first_match_wins_across_dirs()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogWarn("Duplicate schema_id");

            const fs::path tmpdir = make_tmp_dir("first_match");
            // Two directories: dir_a has the "winning" version; dir_b
            // has a duplicate schema_id with a different field. The
            // walker should keep dir_a's copy and skip dir_b's.
            const auto dir_a = tmpdir / "dir_a";
            const auto dir_b = tmpdir / "dir_b";
            fs::create_directories(dir_a);
            fs::create_directories(dir_b);

            {
                std::ofstream(dir_a / "shared.v1.json") << R"({
                    "id": "shared",
                    "version": 1,
                    "slot": {
                        "packing": "aligned",
                        "fields": [{"name": "x", "type": "int32"}]
                    }
                })";
            }
            {
                std::ofstream(dir_b / "shared.v1.json") << R"({
                    "id": "shared",
                    "version": 1,
                    "slot": {
                        "packing": "aligned",
                        "fields": [{"name": "x", "type": "float64"}]
                    }
                })";
            }

            const auto entries = pylabhub::schema::load_all_from_dirs(
                {dir_a.string(), dir_b.string()});
            ASSERT_EQ(entries.size(), 1u);
            EXPECT_EQ(entries[0].second.slot.fields[0].type, "int32")
                << "first-match-wins: dir_a should be retained over "
                   "dir_b";
            EXPECT_NE(entries[0].first.find("dir_a"), std::string::npos);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(tmpdir);
        },
        "datahub_schema_loader::load_all_from_dirs_first_match_wins_across_dirs",
        Logger::GetLifecycleModule());
}

} // namespace datahub_schema_loader
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct DatahubSchemaLoaderRegistrar
{
    DatahubSchemaLoaderRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "datahub_schema_loader")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::datahub_schema_loader;

                if (sc == "load_all_from_dirs_single_file")
                    return load_all_from_dirs_single_file();
                if (sc == "load_all_from_dirs_nested_path")
                    return load_all_from_dirs_nested_path();
                if (sc == "load_all_from_dirs_invalid_json_skipped")
                    return load_all_from_dirs_invalid_json_skipped();
                if (sc == "load_all_from_dirs_first_match_wins_across_dirs")
                    return load_all_from_dirs_first_match_wins_across_dirs();
                return -1;
            });
    }
} g_registrar;

} // namespace
