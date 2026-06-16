/**
 * @file test_curve_keypair.cpp
 * @brief L2 unit tests for `Z85PublicKey` strong type (HEP-CORE-0040
 *        §8.4 + §8.4.1, #158 + #231).
 *
 * Pattern 1 — pure value-type tests; no LOGGER_*, no lifecycle.
 *
 * Coverage:
 *   - Default ctor produces a sentinel `empty()` value (40 zero bytes).
 *     This is the ONLY way to construct an "unset" Z85PublicKey.
 *   - `Z85PublicKey::validate(s)` returns a wrapped key on valid input,
 *     throws `std::invalid_argument` on bad input.  `main()` catches
 *     at the phase boundary; the single string → Z85PublicKey path
 *     enforces type safety.
 *   - `validate()` throw diagnostics name the actual length / position
 *     so misuse is fixable from the error message alone.
 *   - There is no string-taking constructor — verified at compile time
 *     by the fact that the test compiles only when the build uses the
 *     factory-only API (the deleted ctor would have allowed terser
 *     test code; its absence is exactly what enforces the §8.4.1 contract).
 *   - `view()` returns the underlying chars without copy.
 *   - `empty()` distinguishes default sentinel from a real pubkey.
 *   - Equality / inequality reflect string-level comparison.
 *   - Copy + move preserve the underlying value.
 *
 * Verified literals pinned (mutation defenses):
 *   - kZ85Chars == 40 (would catch any silent length drift if the
 *     RFC 32 §4 contract were ever weakened).
 *   - The Z85 alphabet includes the 23 punctuation chars per RFC 32.
 *   - A `'\0'`-containing string is rejected (default-ctor sentinel
 *     cannot be confused with a validated pubkey).
 */

#include "utils/security/curve_keypair.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using pylabhub::utils::security::Z85PublicKey;

namespace
{

/// 40 chars of a valid Z85 alphabet string ('A'..'Z' + '0'..'9' +
/// some punctuation).  Tests that need a "real" pubkey use this
/// instead of a libsodium-generated keypair (we're testing the type
/// validation, not the crypto).
constexpr std::string_view kValidPub40 =
    "ABCDEFGHIJabcdefghij0123456789.-:+=^!/*?";

/// Verify the constant we're handing to the factory is the right length
/// for the test to mean what it says.
static_assert(kValidPub40.size() == 40,
              "kValidPub40 must be exactly 40 chars");

} // namespace

// ─── Length contract ───────────────────────────────────────────────────────

TEST(Z85PublicKeyTest, kZ85Chars_IsExactly40)
{
    // Mutation pin: if a future refactor weakens the length contract
    // (say to 32 for raw bytes, or 41 for a NUL-terminated buffer),
    // this assertion catches it.  The 40-char invariant is wire-load
    // bearing — libzmq's `zmq::sockopt::curve_serverkey` expects
    // exactly 40 ASCII chars.
    EXPECT_EQ(Z85PublicKey::kZ85Chars, 40u);
}

// ─── validate() factory — throwing variant ─────────────────────────────────

TEST(Z85PublicKeyTest, Validate_ValidLength_Succeeds)
{
    EXPECT_NO_THROW((void)Z85PublicKey::validate(kValidPub40));
    const auto p = Z85PublicKey::validate(kValidPub40);
    EXPECT_EQ(p.view(), kValidPub40);
    EXPECT_EQ(p.str().size(), 40u);
}

TEST(Z85PublicKeyTest, Validate_TooShort_Throws)
{
    const std::string short39(39, 'A');
    EXPECT_THROW(Z85PublicKey::validate(short39), std::invalid_argument);
}

TEST(Z85PublicKeyTest, Validate_TooLong_Throws)
{
    const std::string long41(41, 'A');
    EXPECT_THROW(Z85PublicKey::validate(long41), std::invalid_argument);
}

TEST(Z85PublicKeyTest, Validate_Empty_Throws)
{
    // The default ctor `Z85PublicKey{}` produces the Standby sentinel;
    // the validate() factory MUST reject empty input — there's no
    // string-taking ctor for an alternative "soft" interpretation of
    // empty.  Empty input to validate() is a programmer error.
    EXPECT_THROW(Z85PublicKey::validate(std::string{}), std::invalid_argument);
    EXPECT_THROW(Z85PublicKey::validate(std::string_view{}),
                 std::invalid_argument);
}

TEST(Z85PublicKeyTest, Validate_LengthError_DiagnosticNamesActualSize)
{
    try
    {
        (void)Z85PublicKey::validate(std::string(7, 'A'));
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument &e)
    {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("length 7"), std::string::npos)
            << "Diagnostic must name the actual length so the caller "
               "can locate the misuse without re-reading the code.  "
               "Got: " << msg;
        EXPECT_NE(msg.find("40"), std::string::npos)
            << "Diagnostic must name the required length.  Got: "
            << msg;
        EXPECT_NE(msg.find("validate"), std::string::npos)
            << "Diagnostic must name the factory method so the caller "
               "knows which entry point rejected the input.  Got: "
            << msg;
    }
}

TEST(Z85PublicKeyTest, Validate_NonZ85Character_Throws)
{
    // 40 chars all valid except position 5 is a backtick (not in Z85).
    std::string bad(40, 'A');
    bad[5] = '`';
    EXPECT_THROW(Z85PublicKey::validate(bad), std::invalid_argument);
}

TEST(Z85PublicKeyTest, Validate_NonZ85_DiagnosticNamesPosition)
{
    std::string bad(40, 'A');
    bad[17] = '\x01';  // SOH control char — clearly not in Z85.
    try
    {
        (void)Z85PublicKey::validate(bad);
        FAIL() << "Expected std::invalid_argument";
    }
    catch (const std::invalid_argument &e)
    {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("position 17"), std::string::npos)
            << "Diagnostic must name the offending position so the "
               "caller can localize.  Got: " << msg;
        EXPECT_NE(msg.find("0x01"), std::string::npos)
            << "Diagnostic must name the offending byte value.  Got: "
            << msg;
    }
}

TEST(Z85PublicKeyTest, Validate_NulCharacter_Rejected)
{
    // The default ctor's sentinel is 40 zero bytes; if validate()
    // accepted '\0' chars, an attacker could craft a "validated"
    // pubkey indistinguishable from the sentinel.  Pin the rejection.
    std::string with_nul(40, 'A');
    with_nul[20] = '\0';
    EXPECT_THROW(Z85PublicKey::validate(with_nul), std::invalid_argument);
}

TEST(Z85PublicKeyTest, Validate_AllZ85Punctuation_Succeeds)
{
    // The 23 Z85 punctuation chars from RFC 32 §4.  All must be
    // accepted (a refactor that dropped any of them would be a
    // wire-level regression).
    const std::string z85_punct = ".-:+=^!/*?&<>()[]{}@%$#";
    ASSERT_EQ(z85_punct.size(), 23u);
    std::string fortys(40 - 23, 'A');
    fortys += z85_punct;
    ASSERT_EQ(fortys.size(), 40u);
    EXPECT_NO_THROW((void)Z85PublicKey::validate(fortys));
}

// ─── Default sentinel ──────────────────────────────────────────────────────

TEST(Z85PublicKeyTest, DefaultCtor_ProducesEmptyValue)
{
    Z85PublicKey p;
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.str().size(), 40u);
    // Underlying storage is 40 zero bytes — important so the type is
    // always exactly 40 chars wide on the wire if someone serializes
    // its `view()`, and so that `empty()` cannot be ambiguous with a
    // real pubkey (real pubkeys cannot contain '\0').
    for (char c : p.str())
        EXPECT_EQ(c, '\0');
}

TEST(Z85PublicKeyTest, DefaultCtor_IsNoexcept)
{
    // The Standby sentinel must be a noexcept construction — it's
    // used in default member initializers across the codebase.
    static_assert(noexcept(Z85PublicKey{}),
                  "Default ctor must be noexcept — Standby sentinel "
                  "appears in default-initialized fields");
}

TEST(Z85PublicKeyTest, ValidatedKey_IsNotEmpty)
{
    const auto p = Z85PublicKey::validate(kValidPub40);
    EXPECT_FALSE(p.empty());
}

// ─── Equality, copy, move ──────────────────────────────────────────────────

TEST(Z85PublicKeyTest, Equality_SameStringEqual_DifferentStringNotEqual)
{
    const auto a = Z85PublicKey::validate(kValidPub40);
    const auto b = Z85PublicKey::validate(kValidPub40);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);

    // Differ in one char at position 0.
    std::string other(kValidPub40);
    other[0] = 'B';
    const auto c = Z85PublicKey::validate(other);
    EXPECT_NE(a, c);
}

TEST(Z85PublicKeyTest, Equality_DefaultCtorEqualToDefaultCtor)
{
    Z85PublicKey a;
    Z85PublicKey b;
    EXPECT_EQ(a, b);
    EXPECT_TRUE(a.empty() && b.empty());
}

TEST(Z85PublicKeyTest, Copy_PreservesValue)
{
    const auto a = Z85PublicKey::validate(kValidPub40);
    Z85PublicKey b{a};        // copy ctor
    EXPECT_EQ(a, b);

    Z85PublicKey c;
    c = a;                    // copy assignment
    EXPECT_EQ(a, c);
}

TEST(Z85PublicKeyTest, Move_PreservesValue)
{
    auto a = Z85PublicKey::validate(kValidPub40);
    Z85PublicKey b{std::move(a)};   // move ctor
    EXPECT_EQ(b.view(), kValidPub40);
    EXPECT_EQ(b.str().size(), 40u);

    auto c = Z85PublicKey::validate(kValidPub40);
    Z85PublicKey d;
    d = std::move(c);               // move assignment
    EXPECT_EQ(d.view(), kValidPub40);
    EXPECT_EQ(d.str().size(), 40u);
}

// ─── string_view stability ──────────────────────────────────────────────────

TEST(Z85PublicKeyTest, View_PointsIntoStorage_StableAcrossReads)
{
    const auto p = Z85PublicKey::validate(kValidPub40);
    auto v1 = p.view();
    auto v2 = p.view();
    EXPECT_EQ(v1.data(), v2.data())
        << "view() must return a stable pointer into the underlying "
           "storage so callers can pass it to libzmq sockopt without "
           "an intermediate std::string copy.";
}
