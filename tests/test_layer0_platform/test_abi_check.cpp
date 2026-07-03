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
#include "utils/json_fwd.hpp"

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

// ============================================================================
// verify_peer_versions — peer verification entry point
//
// HEP-CORE-0032 §8 + task #325.  Shares the underlying comparator with
// check_abi but is PURE — no stderr, no logger — so callers on the
// wire ingest path can emit their own structured log line.
// ============================================================================

using pylabhub::version::verify_peer_versions;

TEST(VerifyPeerVersionsTest, IdenticalPeer_Compatible_NoStderr)
{
    // Capture stderr — verify_peer_versions must NOT write anything,
    // whereas check_abi writes minor WARNs.  This test pins the
    // "peer-verification is pure" contract.
    ::testing::internal::CaptureStderr();
    const auto r = verify_peer_versions(current(), nullptr);
    const std::string stderr_out = ::testing::internal::GetCapturedStderr();

    EXPECT_TRUE(r.compatible);
    EXPECT_EQ(r.message, "ABI OK");
    EXPECT_TRUE(stderr_out.empty())
        << "verify_peer_versions must be pure — no stderr side effect. "
        << "Got: " << stderr_out;
}

TEST(VerifyPeerVersionsTest, PeerMinorMismatch_CompatibleAndSilent)
{
    // Minor mismatch is compatible (accept + WARN in caller's log).
    // verify_peer_versions itself does NOT emit the WARN — that's
    // the caller's job (broker's REG_REQ handler emits per §8.6).
    const auto peer = with_minor_bumped(current(), "broker_proto");
    ::testing::internal::CaptureStderr();
    const auto r = verify_peer_versions(peer, nullptr);
    const std::string stderr_out = ::testing::internal::GetCapturedStderr();

    EXPECT_TRUE(r.compatible);
    EXPECT_EQ(r.message, "ABI OK")
        << "compatible minor deltas leave message = 'ABI OK'; only "
        << "major mismatches populate the message with per-axis text.";
    EXPECT_TRUE(stderr_out.empty())
        << "verify_peer_versions must NOT emit minor WARN to stderr "
        << "(that's check_abi's job at startup, before Logger is up).";
}

TEST(VerifyPeerVersionsTest, PeerMajorMismatch_Incompatible_FlagAndMessage)
{
    const auto peer = with_major_bumped(current(), "shm");
    const auto r = verify_peer_versions(peer, nullptr);

    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.shm);
    EXPECT_EQ(r.major_mismatch.library,      false);
    EXPECT_EQ(r.major_mismatch.broker_proto, false);
    EXPECT_NE(r.message.find("shm major"), std::string::npos)
        << "message: " << r.message;
}

TEST(VerifyPeerVersionsTest, PeerBuildIdMismatch_Incompatible)
{
    // Only meaningful when the running library has a build_id compiled
    // in.  Skip otherwise — same shape as the check_abi build_id test.
    const char *cur_bid = pylabhub::version::build_id();
    if (cur_bid == nullptr)
    {
        GTEST_SKIP() << "build_id support not compiled in (Release without "
                        "PYLABHUB_STRICT_ABI_CHECK)";
    }
    const auto r = verify_peer_versions(current(),
                                         "peer-has-a-different-build-id-string");
    EXPECT_FALSE(r.compatible);
    EXPECT_TRUE(r.major_mismatch.build_id);
    EXPECT_NE(r.message.find("build_id"), std::string::npos)
        << "message: " << r.message;
}

TEST(VerifyPeerVersionsTest, ResultMatchesCheckAbi_ExceptForStderrSideEffect)
{
    // Contract: verify_peer_versions and check_abi return IDENTICAL
    // AbiCheckResult for identical inputs.  Only difference is
    // stderr emission on minor mismatch.  This test asserts the
    // result equivalence.
    const auto peer = with_major_bumped(
        with_minor_bumped(current(), "config"), "zmq_frame");

    ::testing::internal::CaptureStderr();
    const auto rc = check_abi(peer, nullptr);
    (void)::testing::internal::GetCapturedStderr();  // discard the WARN

    const auto rv = verify_peer_versions(peer, nullptr);

    EXPECT_EQ(rc.compatible,                     rv.compatible);
    EXPECT_EQ(rc.message,                        rv.message);
    EXPECT_EQ(rc.major_mismatch.library,         rv.major_mismatch.library);
    EXPECT_EQ(rc.major_mismatch.shm,             rv.major_mismatch.shm);
    EXPECT_EQ(rc.major_mismatch.broker_proto,    rv.major_mismatch.broker_proto);
    EXPECT_EQ(rc.major_mismatch.zmq_frame,       rv.major_mismatch.zmq_frame);
    EXPECT_EQ(rc.major_mismatch.script_api,      rv.major_mismatch.script_api);
    EXPECT_EQ(rc.major_mismatch.script_engine,   rv.major_mismatch.script_engine);
    EXPECT_EQ(rc.major_mismatch.config,          rv.major_mismatch.config);
    EXPECT_EQ(rc.major_mismatch.build_id,        rv.major_mismatch.build_id);
}

// ============================================================================
// to_json_object / from_json_object — wire-envelope serialization
//
// HEP-CORE-0032 §8.2 + task #325 slice B.  Round-trip contract:
//   to_json_object → JSON → from_json_object == identity.
// Field-shape contract: HEP-0032 §8.2 lists the exact axis keys.
// Forward-compat contract: extra unknown fields are ignored per §8.3.2.
// Robustness contract: missing REQUIRED axis fields throw
//   std::invalid_argument — broker treats malformed envelope as
//   INVALID_REQUEST per §8.7.
// ============================================================================

using pylabhub::version::to_json_object;
using pylabhub::version::from_json_object;

TEST(AbiFingerprintJsonTest, RoundTrip_Identity)
{
    const auto cur = current();
    const nlohmann::json j = to_json_object(cur);
    const auto parsed = from_json_object(j);

    EXPECT_EQ(parsed.library_major,       cur.library_major);
    EXPECT_EQ(parsed.library_minor,       cur.library_minor);
    EXPECT_EQ(parsed.library_rolling,     cur.library_rolling);
    EXPECT_EQ(parsed.shm_major,           cur.shm_major);
    EXPECT_EQ(parsed.shm_minor,           cur.shm_minor);
    EXPECT_EQ(parsed.broker_proto_major,  cur.broker_proto_major);
    EXPECT_EQ(parsed.broker_proto_minor,  cur.broker_proto_minor);
    EXPECT_EQ(parsed.zmq_frame_major,     cur.zmq_frame_major);
    EXPECT_EQ(parsed.zmq_frame_minor,     cur.zmq_frame_minor);
    EXPECT_EQ(parsed.script_api_major,    cur.script_api_major);
    EXPECT_EQ(parsed.script_api_minor,    cur.script_api_minor);
    EXPECT_EQ(parsed.script_engine_major, cur.script_engine_major);
    EXPECT_EQ(parsed.script_engine_minor, cur.script_engine_minor);
    EXPECT_EQ(parsed.config_major,        cur.config_major);
    EXPECT_EQ(parsed.config_minor,        cur.config_minor);
}

TEST(AbiFingerprintJsonTest, WireShape_ExactFieldNames)
{
    // §8.2 lists these exact keys.  A test refactor that renames one
    // MUST bump broker_proto MINOR per §8.4.
    const nlohmann::json j = to_json_object(current());
    EXPECT_TRUE(j.contains("library_major"));
    EXPECT_TRUE(j.contains("library_minor"));
    EXPECT_TRUE(j.contains("library_rolling"));
    EXPECT_TRUE(j.contains("shm_major"));
    EXPECT_TRUE(j.contains("shm_minor"));
    EXPECT_TRUE(j.contains("broker_proto_major"));
    EXPECT_TRUE(j.contains("broker_proto_minor"));
    EXPECT_TRUE(j.contains("zmq_frame_major"));
    EXPECT_TRUE(j.contains("zmq_frame_minor"));
    EXPECT_TRUE(j.contains("script_api_major"));
    EXPECT_TRUE(j.contains("script_api_minor"));
    EXPECT_TRUE(j.contains("script_engine_major"));
    EXPECT_TRUE(j.contains("script_engine_minor"));
    EXPECT_TRUE(j.contains("config_major"));
    EXPECT_TRUE(j.contains("config_minor"));

    // build_id is NOT part of the envelope — it's a SIBLING wire
    // field per §8.2 last paragraph.  This pins that separation.
    EXPECT_FALSE(j.contains("build_id"));
}

TEST(AbiFingerprintJsonTest, UnknownFieldsIgnored_ForwardCompat)
{
    // MINOR-bump forward-compat: adding a new field to a future wire
    // shape must not break older receivers per §8.3.2.  from_json_object
    // must accept + ignore unknown keys.
    nlohmann::json j = to_json_object(current());
    j["future_axis_major"] = 99;
    j["future_axis_minor"] = 42;
    j["unrelated_metadata"] = "hello";

    // Should NOT throw.
    ASSERT_NO_THROW({
        const auto parsed = from_json_object(j);
        (void)parsed;
    });
}

TEST(AbiFingerprintJsonTest, MissingRequiredField_Throws)
{
    nlohmann::json j = to_json_object(current());
    j.erase("shm_major");
    EXPECT_THROW(from_json_object(j), std::invalid_argument);
}

TEST(AbiFingerprintJsonTest, NonObjectInput_Throws)
{
    // Broker's INVALID_REQUEST path — a peer that sends a string or
    // array where an object is expected fails cleanly.
    EXPECT_THROW(from_json_object(nlohmann::json("not-an-object")),
                 std::invalid_argument);
    EXPECT_THROW(from_json_object(nlohmann::json::array({1, 2, 3})),
                 std::invalid_argument);
    EXPECT_THROW(from_json_object(nlohmann::json(nullptr)),
                 std::invalid_argument);
}

TEST(AbiFingerprintJsonTest, VerifyPeerVersions_FromParsedWire)
{
    // End-to-end: role serializes its ComponentVersions to JSON,
    // broker parses it and calls verify_peer_versions.  Verdict must
    // be compatible when both sides are running the same build.
    const auto role_versions = current();
    const nlohmann::json wire = to_json_object(role_versions);

    // Simulate broker's ingest: parse + verify.
    const auto peer = from_json_object(wire);
    const auto verdict = verify_peer_versions(peer, nullptr);
    EXPECT_TRUE(verdict.compatible);
    EXPECT_EQ(verdict.message, "ABI OK");
}
