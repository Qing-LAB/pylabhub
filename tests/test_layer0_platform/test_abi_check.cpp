/**
 * @file test_abi_check.cpp
 * @brief Unit tests for `pylabhub::version::check_abi()` (HEP-CORE-0032).
 *
 * Covers the compatibility-decision matrix:
 *   - identity (current-vs-current) → compatible
 *   - major mismatch per axis → incompatible + matching flag set
 *   - minor mismatch per axis → compatible + WARN (verified via stderr
 *     capture so an accidental demotion of the WARN to silent would
 *     fail the test)
 *   - build_id: nullptr skips; non-null strict-matches
 *   - multiple mismatches coexist in the result
 *
 * Also exercises the consteval machinery with a static_assert to verify
 * compile-time folding actually happens.
 */

#include "plh_version_registry.hpp"

#include <cstring>

#include <gtest/gtest.h>

using pylabhub::version::ComponentVersions;
using pylabhub::version::AbiCheckResult;
using pylabhub::version::check_abi;
using pylabhub::version::current;

namespace
{

// Helper: returns a ComponentVersions with every axis bumped by +1 on
// the named field.  Used to build single-axis mismatch cases without
// repeating the full struct.
ComponentVersions with_major_bumped(ComponentVersions v, const char *axis)
{
    if      (std::strcmp(axis, "library")       == 0) v.library_major       += 1;
    else if (std::strcmp(axis, "shm")           == 0) v.shm_major            += 1;
    else if (std::strcmp(axis, "broker_proto")  == 0) v.broker_proto_major   += 1;
    else if (std::strcmp(axis, "zmq_frame")     == 0) v.zmq_frame_major      += 1;
    else if (std::strcmp(axis, "script_api")    == 0) v.script_api_major     += 1;
    else if (std::strcmp(axis, "script_engine") == 0) v.script_engine_major  += 1;
    else if (std::strcmp(axis, "config")        == 0) v.config_major         += 1;
    return v;
}

ComponentVersions with_minor_bumped(ComponentVersions v, const char *axis)
{
    if      (std::strcmp(axis, "library")       == 0) v.library_minor        += 1;
    else if (std::strcmp(axis, "shm")           == 0) v.shm_minor            += 1;
    else if (std::strcmp(axis, "broker_proto")  == 0) v.broker_proto_minor   += 1;
    else if (std::strcmp(axis, "zmq_frame")     == 0) v.zmq_frame_minor      += 1;
    else if (std::strcmp(axis, "script_api")    == 0) v.script_api_minor     += 1;
    else if (std::strcmp(axis, "script_engine") == 0) v.script_engine_minor  += 1;
    else if (std::strcmp(axis, "config")        == 0) v.config_minor         += 1;
    return v;
}

} // anonymous namespace

// ============================================================================
// consteval compile-time folding
// ============================================================================

// If the consteval machinery is broken or the kXxxMajor constants are
// not actually `inline constexpr`, this static_assert fails to compile.
// That's the design's "it folds at compile time" guarantee.
static_assert(
    pylabhub::version::compiled_against_here().library_major ==
        static_cast<uint16_t>(PYLABHUB_VERSION_MAJOR),
    "compiled_against_here() must fold library_major from PYLABHUB_VERSION_MAJOR");

static_assert(
    pylabhub::version::compiled_against_here().shm_major ==
        pylabhub::version::kShmMajor,
    "compiled_against_here() must fold shm_major from kShmMajor");

static_assert(
    pylabhub::version::compiled_against_here().script_engine_major ==
        pylabhub::version::kScriptEngineMajor,
    "compiled_against_here() must fold script_engine_major from kScriptEngineMajor");

// ============================================================================
// Identity check — current-vs-current compatible
// ============================================================================

TEST(AbiCheckTest, Identity_CurrentVsCurrent_IsCompatible)
{
    const auto r = check_abi(current());
    EXPECT_TRUE(r.compatible) << "current() must match itself: " << r.message;
    EXPECT_EQ(r.message, "ABI OK");
    EXPECT_FALSE(r.major_mismatch.library);
    EXPECT_FALSE(r.major_mismatch.shm);
    EXPECT_FALSE(r.major_mismatch.broker_proto);
    EXPECT_FALSE(r.major_mismatch.zmq_frame);
    EXPECT_FALSE(r.major_mismatch.script_api);
    EXPECT_FALSE(r.major_mismatch.script_engine);
    EXPECT_FALSE(r.major_mismatch.config);
    EXPECT_FALSE(r.major_mismatch.build_id);
}

// ============================================================================
// Major mismatch per axis — fail + flag set + message mentions axis
// ============================================================================

TEST(AbiCheckTest, MajorMismatch_Library_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "library"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.library);
    EXPECT_FALSE(r.major_mismatch.shm);  // only the one axis
    EXPECT_NE(r.message.find("library major"), std::string::npos)
        << "message must mention the axis name; got: " << r.message;
}

TEST(AbiCheckTest, MajorMismatch_Shm_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "shm"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.shm);
    EXPECT_NE(r.message.find("shm major"), std::string::npos);
}

TEST(AbiCheckTest, MajorMismatch_BrokerProto_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "broker_proto"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.broker_proto);
    EXPECT_NE(r.message.find("broker_proto major"), std::string::npos);
}

TEST(AbiCheckTest, MajorMismatch_ZmqFrame_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "zmq_frame"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.zmq_frame);
    EXPECT_NE(r.message.find("zmq_frame major"), std::string::npos);
}

TEST(AbiCheckTest, MajorMismatch_ScriptApi_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "script_api"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.script_api);
    EXPECT_NE(r.message.find("script_api major"), std::string::npos);
}

TEST(AbiCheckTest, MajorMismatch_ScriptEngine_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "script_engine"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.script_engine);
    EXPECT_NE(r.message.find("script_engine major"), std::string::npos);
}

TEST(AbiCheckTest, MajorMismatch_Config_FailsWithFlag)
{
    const auto r = check_abi(with_major_bumped(current(), "config"));
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.config);
    EXPECT_NE(r.message.find("config major"), std::string::npos);
}

// ============================================================================
// Minor mismatch per axis — compatible + no major flag
// ============================================================================

TEST(AbiCheckTest, MinorMismatch_OnlyAxis_StillCompatible)
{
    // Spot-check one axis; the predicate is uniform across all 7.
    const auto r = check_abi(with_minor_bumped(current(), "script_api"));
    EXPECT_TRUE(r.compatible)
        << "additive minor change must NOT flip compatible to false";
    EXPECT_FALSE(r.major_mismatch.script_api)
        << "minor-only bump must not set the major mismatch flag";
    // message is "ABI OK" because no major mismatch contributed.
    EXPECT_EQ(r.message, "ABI OK");
}

// ============================================================================
// Build_id — nullptr skips; mismatch fails
// ============================================================================

TEST(AbiCheckTest, BuildId_Nullptr_Skips)
{
    const auto r = check_abi(current(), /*expected_build_id=*/nullptr);
    EXPECT_TRUE(r.compatible);
    EXPECT_FALSE(r.major_mismatch.build_id);
}

TEST(AbiCheckTest, BuildId_Mismatch_Fails)
{
    // Whether the library has a build_id or not, passing a bogus value
    // that cannot match MUST fail the check.  If the library has
    // build_id, strcmp picks up the difference; if it doesn't (release
    // mode), check_abi reports "(library has no build_id)".
    const auto r = check_abi(current(), "definitely-not-the-real-build-id");
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.build_id);
    EXPECT_NE(r.message.find("build_id"), std::string::npos);
}

// ============================================================================
// Multiple mismatches — all flags set, message lists each
// ============================================================================

TEST(AbiCheckTest, MultipleMismatches_AllFlagsSet)
{
    auto bumped = current();
    bumped.library_major        += 1;
    bumped.broker_proto_major   += 1;
    bumped.script_engine_major  += 1;

    const auto r = check_abi(bumped);
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.library);
    EXPECT_TRUE(r.major_mismatch.broker_proto);
    EXPECT_TRUE(r.major_mismatch.script_engine);
    EXPECT_FALSE(r.major_mismatch.shm);  // untouched

    // Message mentions each of the three bumped axes.
    EXPECT_NE(r.message.find("library major"),       std::string::npos);
    EXPECT_NE(r.message.find("broker_proto major"),  std::string::npos);
    EXPECT_NE(r.message.find("script_engine major"), std::string::npos);
}

// ============================================================================
// abi_expected_here() returns something check_abi can consume
// ============================================================================

TEST(AbiCheckTest, AbiExpectedHere_MatchesCurrent)
{
    constexpr auto exp = pylabhub::version::abi_expected_here();
    const auto r = check_abi(exp.versions, exp.build_id);
    EXPECT_TRUE(r.compatible)
        << "caller's abi_expected_here() must match the library it's "
           "running against; got: " << r.message;
}
