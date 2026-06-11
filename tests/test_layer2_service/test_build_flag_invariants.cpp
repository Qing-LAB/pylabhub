/**
 * @file test_build_flag_invariants.cpp
 * @brief Compile-time invariant pins for the test-only / debug-only
 *        backdoor surfaces — #205 (2026-06-11).
 *
 * Every test-only relaxation in pyLabHub MUST be gated by the double-
 * check `defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)` (or the
 * equivalent `PYLABHUB_WITH_TEST`-based form for HEP-CORE-0036 §I10).
 * This file pins that contract: it asserts the test surface IS present
 * in Debug+BUILD_TESTS builds, and that the same surface would NOT be
 * present in Release.  The Release branch is reachable only when the
 * binary itself is built Release+BUILD_TESTS=ON — an operator-footgun
 * configuration that the CMake configure-time warning catches first.
 *
 * If a future edit drops the NDEBUG portion of the gate (e.g. someone
 * weakens `RoleHostCore::test_set_*` to `#ifdef PYLABHUB_BUILD_TESTS`
 * alone), the conditional assertion below catches the drift in CI.
 *
 * See HEP-CORE-0032 §3.2 for the deployment-posture rationale.
 */

#include "utils/role_host_core.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace pylabhub::scripting
{

// ── Detection idiom for the test_set_out_slots_written member ────────

template <typename T, typename = void>
struct has_test_set_out_slots_written : std::false_type {};

template <typename T>
struct has_test_set_out_slots_written<
    T,
    std::void_t<decltype(std::declval<T &>().test_set_out_slots_written(0))>>
    : std::true_type {};

template <typename T, typename = void>
struct has_test_set_in_slots_received : std::false_type {};

template <typename T>
struct has_test_set_in_slots_received<
    T,
    std::void_t<decltype(std::declval<T &>().test_set_in_slots_received(0))>>
    : std::true_type {};

template <typename T, typename = void>
struct has_test_set_out_drop_count : std::false_type {};

template <typename T>
struct has_test_set_out_drop_count<
    T,
    std::void_t<decltype(std::declval<T &>().test_set_out_drop_count(0))>>
    : std::true_type {};

TEST(BuildFlagInvariants, RoleHostCore_TestSetMutators_MatchDebugNDebugDisposition)
{
    // The test_set_* mutators on RoleHostCore are gated by
    //   #if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
    // at role_host_core.hpp.  This test runs only when BUILD_TESTS=ON
    // (since it's part of the test binary), so the contract simplifies
    // to: present iff !defined(NDEBUG).
#if defined(NDEBUG)
    // Release branch — reachable only if operator forced
    // Release+BUILD_TESTS=ON despite the configure-time warning.  The
    // mutators MUST be absent: counter-forgery surface (HEP-CORE-0019)
    // does not widen.
    EXPECT_FALSE(has_test_set_out_slots_written<RoleHostCore>::value)
        << "RoleHostCore::test_set_out_slots_written must be ABSENT in "
           "Release builds (NDEBUG defined).  Compile-time gate drifted — "
           "see HEP-CORE-0032 §3.2.";
    EXPECT_FALSE(has_test_set_in_slots_received<RoleHostCore>::value)
        << "RoleHostCore::test_set_in_slots_received must be ABSENT in Release.";
    EXPECT_FALSE(has_test_set_out_drop_count<RoleHostCore>::value)
        << "RoleHostCore::test_set_out_drop_count must be ABSENT in Release.";
#else
    // Debug branch (the normal CI test build).  The mutators MUST be
    // present so the engine workers (python/lua) can exercise the
    // metrics-readback contracts.
    EXPECT_TRUE(has_test_set_out_slots_written<RoleHostCore>::value)
        << "RoleHostCore::test_set_out_slots_written must be PRESENT in "
           "Debug+BUILD_TESTS builds.  Compile-time gate drifted.";
    EXPECT_TRUE(has_test_set_in_slots_received<RoleHostCore>::value)
        << "RoleHostCore::test_set_in_slots_received must be PRESENT in Debug+BUILD_TESTS.";
    EXPECT_TRUE(has_test_set_out_drop_count<RoleHostCore>::value)
        << "RoleHostCore::test_set_out_drop_count must be PRESENT in Debug+BUILD_TESTS.";
#endif
}

TEST(BuildFlagInvariants, PYLABHUB_BUILD_TESTS_IsDefinedInTestBuild)
{
    // This file is built only by the test binary, so PYLABHUB_BUILD_TESTS
    // MUST be defined.  If a future CMake refactor accidentally drops
    // the `add_compile_definitions(PYLABHUB_BUILD_TESTS)` step, this
    // test fires.
#if defined(PYLABHUB_BUILD_TESTS)
    SUCCEED();
#else
    FAIL() << "PYLABHUB_BUILD_TESTS macro is not defined in the test "
              "build.  Check cmake/ToplevelOptions.cmake "
              "add_compile_definitions(PYLABHUB_BUILD_TESTS) under the "
              "BUILD_TESTS=ON branch — see HEP-CORE-0032 §3.2.";
#endif
}

} // namespace pylabhub::scripting
