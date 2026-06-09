/**
 * @file test_curve_keypair.cpp
 * @brief L2 unit tests for `Z85PublicKey` strong type (HEP-CORE-0040
 *        §8.4 + AUTH_TODO §C2, #158).
 *
 * Pattern 1 — pure value-type tests; no LOGGER_*, no lifecycle.
 *
 * Coverage:
 *   - Construction from valid 40-char Z85 string succeeds.
 *   - Default ctor produces a sentinel `empty()` value (40 zero bytes).
 *   - Construction from wrong length (39 / 41 / 0) throws
 *     `std::invalid_argument` with diagnostic that names the actual
 *     length so the misuse is fixable from the error message alone.
 *   - Construction from string with non-Z85 character throws
 *     `std::invalid_argument` with diagnostic that names the
 *     position and byte value.
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

/// Verify the constant we're handing to the ctor is the right length
/// for the test to mean what it says.
static_assert(kValidPub40.size() == 40,
              "kValidPub40 must be exactly 40 chars");

} // namespace

// ─── Length validation ──────────────────────────────────────────────────────

TEST(Z85PublicKeyTest, kZ85Chars_IsExactly40)
{
    // Mutation pin: if a future refactor weakens the length contract
    // (say to 32 for raw bytes, or 41 for a NUL-terminated buffer),
    // this assertion catches it.  The 40-char invariant is wire-load
    // bearing — libzmq's `zmq::sockopt::curve_serverkey` expects
    // exactly 40 ASCII chars.
    EXPECT_EQ(Z85PublicKey::kZ85Chars, 40u);
}

TEST(Z85PublicKeyTest, Construct_ValidLength_Succeeds)
{
    EXPECT_NO_THROW(Z85PublicKey p{kValidPub40});
    Z85PublicKey p{kValidPub40};
    EXPECT_EQ(p.view(), kValidPub40);
    EXPECT_EQ(p.str().size(), 40u);
}

TEST(Z85PublicKeyTest, Construct_TooShort_Throws)
{
    const std::string short39(39, 'A');
    EXPECT_THROW(Z85PublicKey{short39}, std::invalid_argument);
}

TEST(Z85PublicKeyTest, Construct_TooLong_Throws)
{
    const std::string long41(41, 'A');
    EXPECT_THROW(Z85PublicKey{long41}, std::invalid_argument);
}

TEST(Z85PublicKeyTest, Construct_Empty_Throws)
{
    // The default ctor produces an empty-equivalent sentinel; passing
    // an actual empty std::string_view to the validating ctor MUST
    // fail (length 0 != 40) so the sentinel path is exclusive.
    EXPECT_THROW(Z85PublicKey{std::string{}}, std::invalid_argument);
    EXPECT_THROW(Z85PublicKey{std::string_view{}}, std::invalid_argument);
}

TEST(Z85PublicKeyTest, Construct_LengthError_DiagnosticNamesActualSize)
{
    try
    {
        Z85PublicKey p{std::string(7, 'A')};
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
    }
}

// ─── Z85 alphabet validation ───────────────────────────────────────────────

TEST(Z85PublicKeyTest, Construct_NonZ85Character_Throws)
{
    // 40 chars all valid except position 5 is a backtick (not in Z85).
    std::string bad(40, 'A');
    bad[5] = '`';
    EXPECT_THROW(Z85PublicKey{bad}, std::invalid_argument);
}

TEST(Z85PublicKeyTest, Construct_NonZ85_DiagnosticNamesPosition)
{
    std::string bad(40, 'A');
    bad[17] = '\x01';  // SOH control char — clearly not in Z85.
    try
    {
        Z85PublicKey p{bad};
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

TEST(Z85PublicKeyTest, Construct_NulCharacter_Rejected)
{
    // The default ctor's sentinel is 40 zero bytes; if the validating
    // ctor accepted '\0' chars, an attacker could craft a "validated"
    // pubkey indistinguishable from the sentinel.  Pin the rejection.
    std::string with_nul(40, 'A');
    with_nul[20] = '\0';
    EXPECT_THROW(Z85PublicKey{with_nul}, std::invalid_argument);
}

TEST(Z85PublicKeyTest, Construct_AllZ85Punctuation_Succeeds)
{
    // The 23 Z85 punctuation chars from RFC 32 §4.  All must be
    // accepted (a refactor that dropped any of them would be a
    // wire-level regression).
    const std::string z85_punct = ".-:+=^!/*?&<>()[]{}@%$#";
    ASSERT_EQ(z85_punct.size(), 23u);
    std::string fortys(40 - 23, 'A');
    fortys += z85_punct;
    ASSERT_EQ(fortys.size(), 40u);
    EXPECT_NO_THROW(Z85PublicKey{fortys});
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

TEST(Z85PublicKeyTest, ValidatedKey_IsNotEmpty)
{
    Z85PublicKey p{kValidPub40};
    EXPECT_FALSE(p.empty());
}

// ─── Equality, copy, move ──────────────────────────────────────────────────

TEST(Z85PublicKeyTest, Equality_SameStringEqual_DifferentStringNotEqual)
{
    Z85PublicKey a{kValidPub40};
    Z85PublicKey b{kValidPub40};
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);

    // Differ in one char at position 0.
    std::string other(kValidPub40);
    other[0] = 'B';
    Z85PublicKey c{other};
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
    Z85PublicKey a{kValidPub40};
    Z85PublicKey b{a};        // copy ctor
    EXPECT_EQ(a, b);

    Z85PublicKey c;
    c = a;                    // copy assignment
    EXPECT_EQ(a, c);
}

TEST(Z85PublicKeyTest, Move_PreservesValue)
{
    Z85PublicKey a{kValidPub40};
    Z85PublicKey b{std::move(a)};   // move ctor
    EXPECT_EQ(b.view(), kValidPub40);
    EXPECT_EQ(b.str().size(), 40u);

    Z85PublicKey c{kValidPub40};
    Z85PublicKey d;
    d = std::move(c);               // move assignment
    EXPECT_EQ(d.view(), kValidPub40);
    EXPECT_EQ(d.str().size(), 40u);
}

// ─── string_view stability ──────────────────────────────────────────────────

TEST(Z85PublicKeyTest, View_PointsIntoStorage_StableAcrossReads)
{
    Z85PublicKey p{kValidPub40};
    auto v1 = p.view();
    auto v2 = p.view();
    EXPECT_EQ(v1.data(), v2.data())
        << "view() must return a stable pointer into the underlying "
           "storage so callers can pass it to libzmq sockopt without "
           "an intermediate std::string copy.";
}
