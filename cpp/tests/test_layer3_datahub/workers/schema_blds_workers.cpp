// tests/test_layer3_datahub/schema_blds_workers.cpp
#include "schema_blds_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cstdint>

using namespace pylabhub::schema;
using namespace pylabhub::tests::helper;

// Structs used across worker functions.
// Defined once here so all workers share them.
struct WorkerSimpleSchema
{
    int32_t a;
    char b;
    uint64_t c;
};
PYLABHUB_SCHEMA_BEGIN(WorkerSimpleSchema)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(b)
PYLABHUB_SCHEMA_MEMBER(c)
PYLABHUB_SCHEMA_END(WorkerSimpleSchema)

struct WorkerOtherSchema
{
    float x;
    double y;
};
PYLABHUB_SCHEMA_BEGIN(WorkerOtherSchema)
PYLABHUB_SCHEMA_MEMBER(x)
PYLABHUB_SCHEMA_MEMBER(y)
PYLABHUB_SCHEMA_END(WorkerOtherSchema)

namespace pylabhub::tests::worker::schema_blds
{

static auto crypto_module()
{
    return pylabhub::crypto::GetLifecycleModule();
}

int schema_info_sets_name_version_size()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info =
                generate_schema_info<WorkerSimpleSchema>("Test.Simple", SchemaVersion{1, 0, 0});
            EXPECT_EQ(info.name, "Test.Simple");
            EXPECT_EQ(info.version.major, 1);
            EXPECT_EQ(info.version.minor, 0);
            EXPECT_EQ(info.version.patch, 0);
            EXPECT_EQ(info.struct_size, sizeof(WorkerSimpleSchema));
        },
        "schema_info_sets_name_version_size", crypto_module());
}

int schema_info_blds_format()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info =
                generate_schema_info<WorkerSimpleSchema>("Test.Simple", SchemaVersion{1, 0, 0});
            EXPECT_EQ(info.blds, "a:i32;b:c;c:u64");
        },
        "schema_info_blds_format", crypto_module());
}

int schema_info_hash_is_deterministic()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info1 =
                generate_schema_info<WorkerSimpleSchema>("Test.Simple", SchemaVersion{1, 0, 0});
            SchemaInfo info2 =
                generate_schema_info<WorkerSimpleSchema>("Test.Simple", SchemaVersion{1, 0, 0});
            EXPECT_EQ(info1.hash, info2.hash) << "Same schema must produce same hash";
        },
        "schema_info_hash_is_deterministic", crypto_module());
}

int schema_info_different_struct_different_hash()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info1 =
                generate_schema_info<WorkerSimpleSchema>("A", SchemaVersion{1, 0, 0});
            SchemaInfo info2 = generate_schema_info<WorkerOtherSchema>("B", SchemaVersion{1, 0, 0});
            EXPECT_NE(info1.hash, info2.hash) << "Different structs must produce different hashes";
        },
        "schema_info_different_struct_different_hash", crypto_module());
}

int schema_info_matches()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo a = generate_schema_info<WorkerSimpleSchema>("A", SchemaVersion{1, 0, 0});
            SchemaInfo b = generate_schema_info<WorkerSimpleSchema>("B", SchemaVersion{1, 0, 0});
            EXPECT_TRUE(a.matches(b)) << "Same struct layout should match by hash";
        },
        "schema_info_matches", crypto_module());
}

int schema_info_matches_hash()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info =
                generate_schema_info<WorkerSimpleSchema>("Test", SchemaVersion{1, 0, 0});
            EXPECT_TRUE(info.matches_hash(info.hash));
        },
        "schema_info_matches_hash", crypto_module());
}

int validate_schema_match_same_does_not_throw()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo a = generate_schema_info<WorkerSimpleSchema>("A", SchemaVersion{1, 0, 0});
            SchemaInfo b = generate_schema_info<WorkerSimpleSchema>("B", SchemaVersion{1, 0, 0});
            EXPECT_NO_THROW(validate_schema_match(a, b));
        },
        "validate_schema_match_same_does_not_throw", crypto_module());
}

int validate_schema_match_different_throws()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo a = generate_schema_info<WorkerSimpleSchema>("A", SchemaVersion{1, 0, 0});
            SchemaInfo b = generate_schema_info<WorkerOtherSchema>("B", SchemaVersion{1, 0, 0});
            EXPECT_THROW(validate_schema_match(a, b), SchemaValidationException);
        },
        "validate_schema_match_different_throws", crypto_module());
}

int validate_schema_hash_matching_does_not_throw()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info =
                generate_schema_info<WorkerSimpleSchema>("Test", SchemaVersion{1, 0, 0});
            EXPECT_NO_THROW(validate_schema_hash(info, info.hash));
        },
        "validate_schema_hash_matching_does_not_throw", crypto_module());
}

int validate_schema_hash_mismatch_throws()
{
    return run_gtest_worker(
        []()
        {
            SchemaInfo info =
                generate_schema_info<WorkerSimpleSchema>("Test", SchemaVersion{1, 0, 0});
            std::array<uint8_t, 32> wrong_hash{};
            wrong_hash.fill(0xff);
            EXPECT_THROW(validate_schema_hash(info, wrong_hash), SchemaValidationException);
        },
        "validate_schema_hash_mismatch_throws", crypto_module());
}

} // namespace pylabhub::tests::worker::schema_blds

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct SchemaBLDSWorkerRegistrar
{
    SchemaBLDSWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "schema_blds")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::schema_blds;
                if (scenario == "schema_info_name_version_size")
                    return schema_info_sets_name_version_size();
                if (scenario == "schema_info_blds_format")
                    return schema_info_blds_format();
                if (scenario == "schema_info_hash_deterministic")
                    return schema_info_hash_is_deterministic();
                if (scenario == "schema_info_different_hash")
                    return schema_info_different_struct_different_hash();
                if (scenario == "schema_info_matches")
                    return schema_info_matches();
                if (scenario == "schema_info_matches_hash")
                    return schema_info_matches_hash();
                if (scenario == "validate_match_same_ok")
                    return validate_schema_match_same_does_not_throw();
                if (scenario == "validate_match_diff_throws")
                    return validate_schema_match_different_throws();
                if (scenario == "validate_hash_match_ok")
                    return validate_schema_hash_matching_does_not_throw();
                if (scenario == "validate_hash_mismatch_throws")
                    return validate_schema_hash_mismatch_throws();
                fmt::print(stderr, "ERROR: Unknown schema_blds scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static SchemaBLDSWorkerRegistrar g_schema_blds_registrar;
} // namespace
