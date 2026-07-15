// tests/test_layer1_base/test_wire_dispatch_table.cpp
//
// L1 regression pin for wire_dispatch::kDispatchTable.
//
// Purpose: guard against silent row-drop or accidental msg_type/tier
// reassignment.  A dropped row loses envelope-hash gating for that
// msg_type on the receive side (falls through to UNKNOWN_MSG_TYPE);
// a wrong tier routes a msg to the wrong gate set.  Both are
// regression-invisible in end-to-end tests when the affected feature
// is exercised on a happy path.
//
// If this test fails: someone changed kDispatchTable.  Verify the
// change is intentional; update the pinned map below to match.
// The C11 attempt (2026-07-14) to drop CHANNEL_BROADCAST_SEND_NOTIFY
// is the incident that motivated this pin.

#include "utils/wire_dispatch.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <unordered_map>

namespace wd = pylabhub::wire::dispatch;

namespace
{

// Expected msg_type → tier-name mapping.  Every row in kDispatchTable
// must appear here; adding a row without updating this map is a test
// failure by construction (size mismatch below).
const std::unordered_map<std::string_view, std::string_view> kExpectedTiers = {
    // Full-gate REG-family
    {"REG_REQ",                        "RegReq"},
    {"CONSUMER_REG_REQ",               "ConsumerRegReq"},

    // Authenticated REG-family
    {"DEREG_REQ",                      "AuthReg_Dereg"},
    {"CONSUMER_DEREG_REQ",             "AuthReg_ConsumerDereg"},
    {"ENDPOINT_UPDATE_REQ",            "AuthReg_EndpointUpdate"},
    {"CHANNEL_AUTH_APPLIED_REQ",       "AuthReg_ChanAuthApplied"},

    // Control tier
    {"HEARTBEAT_NOTIFY",               "Control_HeartbeatNotify"},
    {"GET_CHANNEL_AUTH_REQ",           "Control_GetChannelAuth"},
    {"DISC_REQ",                       "Control_Disc"},

    // Control_EnvelopeWithRoleUid — body role_uid = caller's own uid;
    // identity_match + grammar + role-tag policy.  A row slipping to
    // plain EnvelopeOnly loses role_uid grammar + identity match —
    // the 2026-07-14 regression this pin now catches.
    {"CHECK_PEER_READY_REQ",           "Control_EnvelopeWithRoleUid"},
    {"BAND_JOIN_REQ",                  "Control_EnvelopeWithRoleUid"},
    {"BAND_LEAVE_REQ",                 "Control_EnvelopeWithRoleUid"},
    {"BAND_BROADCAST_SEND_NOTIFY",     "Control_EnvelopeWithRoleUid"},

    // Control_EnvelopeWithQueryRoleUid — body role_uid = queried
    // subject.  Grammar + role-tag policy only; NO identity_match
    // (probes legitimately ask about other roles).
    {"ROLE_PRESENCE_REQ",              "Control_EnvelopeWithQueryRoleUid"},
    {"ROLE_INFO_REQ",                  "Control_EnvelopeWithQueryRoleUid"},

    // EnvelopeOnly tier — body has no identity fields (or
    // CHANNEL_BROADCAST_SEND_NOTIFY's legacy `sender_uid` naming).
    {"SCHEMA_REQ",                     "EnvelopeOnly"},
    {"CHANNEL_LIST_REQ",               "EnvelopeOnly"},
    {"METRICS_REQ",                    "EnvelopeOnly"},
    {"SHM_BLOCK_QUERY_REQ",            "EnvelopeOnly"},
    {"BAND_MEMBERS_REQ",               "EnvelopeOnly"},
    {"CHANNEL_BROADCAST_SEND_NOTIFY",  "EnvelopeOnly"},
};

}  // namespace

TEST(WireDispatchTable, SizeMatchesExpectedMap)
{
    EXPECT_EQ(wd::dispatch_table_size(), kExpectedTiers.size())
        << "kDispatchTable row count changed.  If intentional, update "
           "kExpectedTiers in this test to match.  If unintentional, "
           "a msg_type row was silently dropped (loses envelope-hash "
           "gating for that msg_type — C11 incident).";
}

TEST(WireDispatchTable, EveryExpectedMsgTypeMapsToExpectedTier)
{
    for (const auto &[msg_type, expected_tier] : kExpectedTiers)
    {
        auto actual = wd::tier_for_msg_type(msg_type);
        ASSERT_TRUE(actual.has_value())
            << "msg_type '" << msg_type << "' absent from kDispatchTable "
            << "(broker will reply UNKNOWN_MSG_TYPE for it)";
        EXPECT_EQ(*actual, expected_tier)
            << "msg_type '" << msg_type << "' routes to tier '" << *actual
            << "', expected '" << expected_tier << "'";
    }
}

TEST(WireDispatchTable, RenamedBroadcastMsgTypesAreLive)
{
    // Direct pin for the SEND_NOTIFY rename (Group 5 broadcast rename).
    // Both new names must resolve; both old names must not.
    EXPECT_TRUE(wd::tier_for_msg_type("CHANNEL_BROADCAST_SEND_NOTIFY")
                    .has_value())
        << "CHANNEL_BROADCAST_SEND_NOTIFY missing from dispatch table";
    EXPECT_TRUE(wd::tier_for_msg_type("BAND_BROADCAST_SEND_NOTIFY")
                    .has_value())
        << "BAND_BROADCAST_SEND_NOTIFY missing from dispatch table";
    EXPECT_FALSE(wd::tier_for_msg_type("CHANNEL_BROADCAST_REQ")
                     .has_value())
        << "old CHANNEL_BROADCAST_REQ literal still resolves — the "
           "SEND_NOTIFY rename did not land completely";
    EXPECT_FALSE(wd::tier_for_msg_type("BAND_BROADCAST_REQ")
                     .has_value())
        << "old BAND_BROADCAST_REQ literal still resolves — the "
           "SEND_NOTIFY rename did not land completely";
}

TEST(WireDispatchTable, UnknownMsgTypeReturnsNullopt)
{
    EXPECT_FALSE(wd::tier_for_msg_type("").has_value());
    EXPECT_FALSE(wd::tier_for_msg_type("NOT_A_REAL_MSG_TYPE").has_value());
    EXPECT_FALSE(wd::tier_for_msg_type("REG_ACK").has_value())
        << "ACK msg_types are broker-outbound, never received; must not "
           "appear in the receive dispatch table";
}
