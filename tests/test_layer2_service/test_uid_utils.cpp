/**
 * @file test_uid_utils.cpp
 * @brief Unit tests for uid_utils.hpp (HEP-0033 §G2.2.0b format).
 *
 * Every generated uid must match the HEP-0033 naming grammar —
 * `prod.<name>.u<8hex>` for roles and `hub.<name>.u<8hex>` for peers,
 * all lowercase, dot-separated, suffix letter-prefixed.  These tests
 * are the executable form of that contract.
 *
 * Pure header-only API test — no lifecycle, no workers.
 */
#include "utils/naming.hpp"
#include "utils/uid_utils.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>

using namespace pylabhub::uid;

class UidUtilsTest : public ::testing::Test
{
};

// ============================================================================
// Generators — prefix and grammar compliance
// ============================================================================

TEST_F(UidUtilsTest, GenerateHubUid_HasPrefixAndIsValidPeerUid)
{
    auto uid = generate_hub_uid();
    EXPECT_TRUE(uid.starts_with("hub.")) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::PeerUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateProducerUid_HasPrefixAndIsValidRoleUid)
{
    auto uid = generate_producer_uid();
    EXPECT_TRUE(uid.starts_with("prod.")) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::RoleUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateConsumerUid_HasPrefixAndIsValidRoleUid)
{
    auto uid = generate_consumer_uid();
    EXPECT_TRUE(uid.starts_with("cons.")) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::RoleUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateProcessorUid_HasPrefixAndIsValidRoleUid)
{
    auto uid = generate_processor_uid();
    EXPECT_TRUE(uid.starts_with("proc.")) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::RoleUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateUid_ArbitraryTag)
{
    // generate_uid is tag-agnostic — but any tag that isn't in the
    // reserved set still produces a structurally valid <tag>.<name>.<suffix>
    // string. Naming validation is the caller's responsibility.
    auto uid = generate_uid("prod", "TestNode");
    EXPECT_TRUE(uid.starts_with("prod.")) << "uid=" << uid;
    EXPECT_NE(uid.find("testnode"), std::string::npos) << "uid=" << uid;
}

// ============================================================================
// Generators — with name
// ============================================================================

TEST_F(UidUtilsTest, GenerateHubUid_WithName_IsValidPeerUid)
{
    auto uid = generate_hub_uid("myHub");
    EXPECT_TRUE(uid.starts_with("hub.")) << "uid=" << uid;
    // Sanitized name "myHub" → "myhub" (lowercased)
    EXPECT_NE(uid.find("myhub"), std::string::npos) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::PeerUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateProducerUid_WithName_IsValidRoleUid)
{
    auto uid = generate_producer_uid("TempSensor");
    EXPECT_TRUE(uid.starts_with("prod.")) << "uid=" << uid;
    EXPECT_NE(uid.find("tempsensor"), std::string::npos) << "uid=" << uid;
    EXPECT_TRUE(pylabhub::hub::is_valid_identifier(
        uid, pylabhub::hub::IdentifierKind::RoleUid)) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateUid_Uniqueness)
{
    // 100 calls must all produce unique UIDs.
    std::set<std::string> uids;
    for (int i = 0; i < 100; ++i) uids.insert(generate_hub_uid("test"));
    EXPECT_EQ(uids.size(), 100u);
}

TEST_F(UidUtilsTest, GenerateUid_SuffixIsLetterPrefixed)
{
    // HEP-0033 grammar requires every NameComponent to start with a
    // letter. The random suffix is letter-prefixed with `u` for
    // exactly this reason; verify by parsing.
    auto uid   = generate_producer_uid("x");
    auto parts = pylabhub::hub::parse_role_uid(uid);
    ASSERT_TRUE(parts.has_value()) << "uid=" << uid;
    EXPECT_FALSE(parts->unique.empty());
    EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(parts->unique.front())))
        << "unique=" << parts->unique;
}

// ============================================================================
// Prefix helpers
// ============================================================================

TEST_F(UidUtilsTest, HasHubPrefix_Valid)
{
    EXPECT_TRUE (has_hub_prefix("hub.foo.uabcd1234"));
    EXPECT_FALSE(has_hub_prefix("prod.foo.u1234abcd"));
    EXPECT_FALSE(has_hub_prefix("hub"));           // no dot after tag
    EXPECT_FALSE(has_hub_prefix("hub."));          // nothing after dot
    EXPECT_FALSE(has_hub_prefix(""));
    EXPECT_FALSE(has_hub_prefix("HUB.foo.u1"));    // uppercase — case-sensitive
}

TEST_F(UidUtilsTest, HasProducerPrefix_Valid)
{
    EXPECT_TRUE (has_producer_prefix("prod.temp.uaabbccdd"));
    EXPECT_FALSE(has_producer_prefix("cons.temp.uaabbccdd"));
}

TEST_F(UidUtilsTest, HasConsumerPrefix_Valid)
{
    EXPECT_TRUE (has_consumer_prefix("cons.log.u11223344"));
    EXPECT_FALSE(has_consumer_prefix("proc.log.u11223344"));
}

TEST_F(UidUtilsTest, HasProcessorPrefix_Valid)
{
    EXPECT_TRUE (has_processor_prefix("proc.node.u12345678"));
    EXPECT_FALSE(has_processor_prefix("prod.node.u12345678"));
}

// ============================================================================
// sanitize_name_part — HEP-0033 NameComponent compliance
// ============================================================================

TEST_F(UidUtilsTest, Sanitize_LowercasesAlpha)
{
    EXPECT_EQ(detail::sanitize_name_part("myNode"), "mynode");
    EXPECT_EQ(detail::sanitize_name_part("MixedCase"), "mixedcase");
}

TEST_F(UidUtilsTest, Sanitize_KeepsDigitsAndCollapsesSpecials)
{
    auto result = detail::sanitize_name_part("my.node!v2");
    // "my" + "-" + "node" + "-" + "v2"
    EXPECT_EQ(result, "my-node-v2");
}

TEST_F(UidUtilsTest, Sanitize_Empty_FallsBackToNode)
{
    EXPECT_EQ(detail::sanitize_name_part(""),     "node");
    EXPECT_EQ(detail::sanitize_name_part("---"),  "node");
    EXPECT_EQ(detail::sanitize_name_part("!!!!"), "node");
}

TEST_F(UidUtilsTest, Sanitize_Truncates)
{
    auto result = detail::sanitize_name_part("abcdefghijklmnopqrst", /*max_len=*/8U);
    EXPECT_EQ(result, "abcdefgh");
    EXPECT_LE(result.size(), 8u);
}

TEST_F(UidUtilsTest, Sanitize_StripsLeadingTrailingDash)
{
    EXPECT_EQ(detail::sanitize_name_part("---foo---"), "foo");
}

TEST_F(UidUtilsTest, Sanitize_PrependsLetter_WhenStartsWithDigit)
{
    // Raw sanitization of "123Temp" would give "123temp" — first char
    // a digit fails NameComponent. sanitize_name_part must prepend 'n'
    // so the output is a valid component.
    auto result = detail::sanitize_name_part("123Temp");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(result.front())))
        << "result=" << result;
    EXPECT_NE(result.find("123temp"), std::string::npos) << "result=" << result;
}
