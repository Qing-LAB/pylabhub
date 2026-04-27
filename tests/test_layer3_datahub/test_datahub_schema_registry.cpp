// test_datahub_schema_registry.cpp
//
// Unit tests for SchemaStore (HEP-CORE-0016 Phase 4).
//
// SchemaStore is a lifecycle-managed singleton wrapping SchemaLibrary
// with thread-safe access.  Tests exercise the lifecycle, in-memory
// registration, lookup, reload, and search dir paths.
//
// Test suite: SchemaRegistryTest (~11 tests)

#include "plh_datahub_client.hpp"
#include "utils/schema_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

using namespace pylabhub::schema;

// ============================================================================
// JSON schema strings — reused from schema_library tests
// ============================================================================

static constexpr const char kTempRawJson[] = R"json({
  "id":      "lab.sensors.temperature.raw",
  "version": 1,
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  }
})json";

static constexpr const char kSamplesJson[] = R"json({
  "id":      "lab.sensors.samples",
  "version": 1,
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",      "type": "float64"},
      {"name": "samples", "type": "float32", "count": 8}
    ]
  }
})json";

// ============================================================================
// Test fixture — lifecycle with Logger + SchemaStore
// ============================================================================

class SchemaRegistryTest : public ::testing::Test
{
protected:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;

    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                SchemaStore::GetLifecycleModule()), std::source_location::current());
    }

    static void TearDownTestSuite()
    {
        s_lifecycle_.reset();
    }
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> SchemaRegistryTest::s_lifecycle_;

// ============================================================================
// Test 1: instance() returns the same address on repeated calls
// ============================================================================

TEST_F(SchemaRegistryTest, GetInstance_SameAddress)
{
    auto &a = SchemaStore::instance();
    auto &b = SchemaStore::instance();
    EXPECT_EQ(&a, &b);
}

// ============================================================================
// Test 2: lifecycle_initialized() is true after SetUpTestSuite
// ============================================================================

TEST_F(SchemaRegistryTest, LifecycleInitialized_True)
{
    EXPECT_TRUE(SchemaStore::lifecycle_initialized());
}

// ============================================================================
// Test 3: register_schema() → get() returns it
// ============================================================================

TEST_F(SchemaRegistryTest, RegisterAndGet)
{
    auto &reg = SchemaStore::instance();
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    reg.register_schema(e);

    const auto found = reg.get("$lab.sensors.temperature.raw.v1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->schema_id, "$lab.sensors.temperature.raw.v1");
    EXPECT_EQ(found->slot_info.blds, "ts:f64;value:f32");
}

// ============================================================================
// Test 4: register_schema() → identify(hash) returns schema_id
// ============================================================================

TEST_F(SchemaRegistryTest, IdentifyByHash)
{
    auto &reg = SchemaStore::instance();
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    reg.register_schema(e);

    const auto id = reg.identify(e.slot_info.hash);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "$lab.sensors.temperature.raw.v1");
}

// ============================================================================
// Test 5: get() with unknown ID returns nullopt
// ============================================================================

TEST_F(SchemaRegistryTest, GetUnknown_Nullopt)
{
    auto &reg = SchemaStore::instance();
    const auto found = reg.get("$does.not.exist.v99");
    EXPECT_FALSE(found.has_value());
}

// ============================================================================
// Test 6: identify() with unknown hash returns nullopt
// ============================================================================

TEST_F(SchemaRegistryTest, IdentifyUnknown_Nullopt)
{
    auto &reg = SchemaStore::instance();
    const std::array<uint8_t, 32> random_hash = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
        0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C};
    const auto id = reg.identify(random_hash);
    EXPECT_FALSE(id.has_value());
}

// ============================================================================
// Test 7: reload() clears in-memory registrations and re-scans dirs
// ============================================================================

TEST_F(SchemaRegistryTest, Reload_ClearsAndReloads)
{
    auto &reg = SchemaStore::instance();
    // Register an in-memory schema
    const SchemaEntry e = SchemaLibrary::load_from_string(kSamplesJson);
    reg.register_schema(e);

    const auto before = reg.get("$lab.sensors.samples.v1");
    ASSERT_TRUE(before.has_value());

    // reload() replaces the library — in-memory entry is gone
    // (no schema files on disk in test env, so reload returns 0)
    reg.reload();

    const auto after = reg.get("$lab.sensors.samples.v1");
    EXPECT_FALSE(after.has_value()) << "In-memory registration should be cleared by reload()";
}

// ============================================================================
// Test 8: list() returns all registered schema IDs
// ============================================================================

TEST_F(SchemaRegistryTest, ListSchemas)
{
    auto &reg = SchemaStore::instance();
    // Start fresh
    reg.reload();

    reg.register_schema(SchemaLibrary::load_from_string(kTempRawJson));
    reg.register_schema(SchemaLibrary::load_from_string(kSamplesJson));

    const auto ids = reg.list();
    EXPECT_EQ(ids.size(), 2u);

    const auto has = [&](const std::string &id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    EXPECT_TRUE(has("$lab.sensors.temperature.raw.v1"));
    EXPECT_TRUE(has("$lab.sensors.samples.v1"));
}

// ============================================================================
// Test 9: set_search_dirs() + reload() loads from custom path
// ============================================================================

TEST_F(SchemaRegistryTest, SetSearchDirs_LoadsFromCustomPath)
{
    namespace fs = std::filesystem;
    auto &reg = SchemaStore::instance();

    // Create a temp dir with a schema file in the expected path:
    // <dir>/lab/sensors/temperature.raw.v1.json
    const auto tmpdir =
        fs::temp_directory_path() / ("plh_schema_test_" + std::to_string(getpid()));
    const auto schema_dir = tmpdir / "lab" / "sensors";
    fs::create_directories(schema_dir);

    // Write schema JSON
    {
        std::ofstream ofs(schema_dir / "temperature.raw.v1.json");
        ofs << kTempRawJson;
    }

    reg.set_search_dirs({tmpdir.string()});
    reg.reload();

    const auto found = reg.get("$lab.sensors.temperature.raw.v1");
    EXPECT_TRUE(found.has_value()) << "Schema should be found after set_search_dirs + reload";

    // Cleanup
    fs::remove_all(tmpdir);
}

// ============================================================================
// Test 10: set_search_dirs() with empty dir clears schemas
// ============================================================================

TEST_F(SchemaRegistryTest, SetSearchDirs_OverridesDefault)
{
    namespace fs = std::filesystem;
    auto &reg = SchemaStore::instance();

    // Register something in-memory first
    reg.register_schema(SchemaLibrary::load_from_string(kTempRawJson));
    ASSERT_TRUE(reg.get("$lab.sensors.temperature.raw.v1").has_value());

    // Create an empty temp dir
    const auto empty_dir =
        fs::temp_directory_path() / ("plh_schema_empty_" + std::to_string(getpid()));
    fs::create_directories(empty_dir);

    reg.set_search_dirs({empty_dir.string()});
    reg.reload();

    const auto ids = reg.list();
    EXPECT_TRUE(ids.empty()) << "After reload with empty search dir, list should be empty";

    // Cleanup
    fs::remove_all(empty_dir);
}
