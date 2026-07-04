/**
 * @file test_startup_config.cpp
 * @brief L2 unit tests for `parse_startup_config`.
 *
 * Pattern 1 — pure value-function tests; no LOGGER_*, no lifecycle.
 *
 * Task #327: pins the wire-shape contract for the new
 * `startup.strict_abi_mismatch` bool that gates HEP-CORE-0032 §8.6
 * strict-mode ABI reject on the role side.
 */

#include "utils/config/startup_config.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

using pylabhub::config::parse_startup_config;

namespace
{

TEST(StartupConfig, StrictAbiMismatch_DefaultFalse_WhenAbsent)
{
    // Absent `startup.strict_abi_mismatch` MUST default false — same
    // behaviour as pre-#327 log-only mode.  A future default flip would
    // break every deployment that omits the field.
    const auto j = nlohmann::json::object();
    const auto sc = parse_startup_config(j, "test");
    EXPECT_FALSE(sc.strict_abi_mismatch);
}

TEST(StartupConfig, StrictAbiMismatch_DefaultFalse_WhenStartupPresentButFieldAbsent)
{
    // `startup` object present, no `strict_abi_mismatch` key → default
    // false.  Ensures the wait_for_roles migration path doesn't
    // accidentally trip strict mode.
    const auto j = nlohmann::json::parse(R"({
        "startup": {"wait_for_roles": []}
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_FALSE(sc.strict_abi_mismatch);
}

TEST(StartupConfig, StrictAbiMismatch_ExplicitTrue_PropagatesToStruct)
{
    const auto j = nlohmann::json::parse(R"({
        "startup": {"strict_abi_mismatch": true}
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_TRUE(sc.strict_abi_mismatch);
}

TEST(StartupConfig, StrictAbiMismatch_ExplicitFalse_PropagatesToStruct)
{
    const auto j = nlohmann::json::parse(R"({
        "startup": {"strict_abi_mismatch": false}
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_FALSE(sc.strict_abi_mismatch);
}

TEST(StartupConfig, StrictAbiMismatch_NonBoolean_Throws)
{
    // Non-boolean value → runtime_error with the field name in the
    // message.  Guards against typos like `"strict_abi_mismatch": "true"`
    // silently doing the wrong thing (string is truthy in some parsers
    // but empty-string is falsy — footgun class).
    const auto j = nlohmann::json::parse(R"({
        "startup": {"strict_abi_mismatch": "true"}
    })");
    EXPECT_THROW(parse_startup_config(j, "test"), std::runtime_error);
}

TEST(StartupConfig, StrictAbiMismatch_CoexistsWithWaitForRoles)
{
    // Both keys under `startup`: both parse correctly + no
    // "unknown key" error from the strict-key-list check.
    const auto j = nlohmann::json::parse(R"({
        "startup": {
            "strict_abi_mismatch": true,
            "wait_for_roles": [
                {"uid": "prod.foo.uid00000001", "timeout_ms": 5000}
            ]
        }
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_TRUE(sc.strict_abi_mismatch);
    ASSERT_EQ(sc.wait_for_roles.size(), 1u);
    EXPECT_EQ(sc.wait_for_roles[0].uid, "prod.foo.uid00000001");
}

// ── task #262: shm_require_mutual_auth ────────────────────────────────────────

TEST(StartupConfig, ShmRequireMutualAuth_DefaultFalse_WhenAbsent)
{
    const auto j = nlohmann::json::object();
    const auto sc = parse_startup_config(j, "test");
    EXPECT_FALSE(sc.shm_require_mutual_auth);
}

TEST(StartupConfig, ShmRequireMutualAuth_ExplicitTrue_PropagatesToStruct)
{
    const auto j = nlohmann::json::parse(R"({
        "startup": {"shm_require_mutual_auth": true}
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_TRUE(sc.shm_require_mutual_auth);
}

TEST(StartupConfig, ShmRequireMutualAuth_NonBoolean_Throws)
{
    const auto j = nlohmann::json::parse(R"({
        "startup": {"shm_require_mutual_auth": "yes"}
    })");
    EXPECT_THROW(parse_startup_config(j, "test"), std::runtime_error);
}

TEST(StartupConfig, ShmRequireMutualAuth_CoexistsWithStrictAbiAndWaitForRoles)
{
    // All three `startup` keys populated: verify no "unknown key"
    // error and each flag lands on the right struct field.
    const auto j = nlohmann::json::parse(R"({
        "startup": {
            "strict_abi_mismatch": true,
            "shm_require_mutual_auth": true,
            "wait_for_roles": [
                {"uid": "cons.foo.uid00000001", "timeout_ms": 3000}
            ]
        }
    })");
    const auto sc = parse_startup_config(j, "test");
    EXPECT_TRUE(sc.strict_abi_mismatch);
    EXPECT_TRUE(sc.shm_require_mutual_auth);
    ASSERT_EQ(sc.wait_for_roles.size(), 1u);
    EXPECT_EQ(sc.wait_for_roles[0].uid, "cons.foo.uid00000001");
}

} // anon
