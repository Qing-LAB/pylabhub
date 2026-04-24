/**
 * @file test_naming.cpp
 * @brief L2 unit tests for the identifier validator + parsers.
 *
 * Spec source: docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md §G2.2.0b
 * "Naming conventions for hub identifiers."  These tests are the
 * executable form of that spec — every rule in the doc maps to at
 * least one valid-case and one invalid-case test.
 */

#include "utils/naming.hpp"

#include <gtest/gtest.h>

using pylabhub::hub::extract_role_tag;
using pylabhub::hub::format_role_ref;
using pylabhub::hub::IdentifierKind;
using pylabhub::hub::is_valid_identifier;
using pylabhub::hub::parse_peer_uid;
using pylabhub::hub::parse_role_uid;
using pylabhub::hub::parse_schema_id;
using pylabhub::hub::SchemaIdParts;
using pylabhub::hub::TaggedUidParts;

// ─── NameComponent charset ─────────────────────────────────────────────────

TEST(NamingChannel, Accepts_LetterStart_AlphanumUnderscoreDash)
{
    EXPECT_TRUE(is_valid_identifier("sensor", IdentifierKind::Channel));
    EXPECT_TRUE(is_valid_identifier("Sensor_Data", IdentifierKind::Channel));
    EXPECT_TRUE(is_valid_identifier("sensor-1", IdentifierKind::Channel));
    EXPECT_TRUE(is_valid_identifier("lab.sensor.temp", IdentifierKind::Channel));
    EXPECT_TRUE(is_valid_identifier("a", IdentifierKind::Channel));
}

TEST(NamingChannel, Rejects_DigitStart)
{
    EXPECT_FALSE(is_valid_identifier("1sensor", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("lab.1temp", IdentifierKind::Channel));
}

TEST(NamingChannel, Rejects_LeadingOrTrailingSeparator)
{
    EXPECT_FALSE(is_valid_identifier(".sensor", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sensor.", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sensor..data", IdentifierKind::Channel));
}

TEST(NamingChannel, Rejects_ForbiddenChars)
{
    EXPECT_FALSE(is_valid_identifier("sensor data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sensor/data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sensor:data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sensor@data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("", IdentifierKind::Channel));
}

TEST(NamingChannel, Rejects_ReservedFirstComponents)
{
    EXPECT_FALSE(is_valid_identifier("prod", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("prod.data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("cons.data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("proc.data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("hub.data", IdentifierKind::Channel));
    EXPECT_FALSE(is_valid_identifier("sys.data", IdentifierKind::Channel));

    // Near-matches are fine (exact-match check on first component):
    EXPECT_TRUE(is_valid_identifier("production.data", IdentifierKind::Channel));
    EXPECT_TRUE(is_valid_identifier("prod-test.data", IdentifierKind::Channel));
}

TEST(NamingChannel, LengthLimits)
{
    std::string ok_component(64, 'a'); ok_component[0] = 'A';
    EXPECT_TRUE(is_valid_identifier(ok_component, IdentifierKind::Channel));

    std::string too_long(65, 'a'); too_long[0] = 'A';
    EXPECT_FALSE(is_valid_identifier(too_long, IdentifierKind::Channel));

    // Total length cap 256 — applies per single identifier as a
    // parsing-boundary DoS defense (HEP-0033 §G2.2.0b).  Composite /
    // federated references use structured encoding and are not
    // subject to this cap.
    std::string tot(256, 'a'); tot[0] = 'A';
    for (size_t i = 60; i < tot.size(); i += 60) tot[i] = '.';
    EXPECT_TRUE(is_valid_identifier(tot, IdentifierKind::Channel));

    std::string tot_over(257, 'a'); tot_over[0] = 'A';
    for (size_t i = 60; i < tot_over.size(); i += 60) tot_over[i] = '.';
    EXPECT_FALSE(is_valid_identifier(tot_over, IdentifierKind::Channel));
}

// ─── Band ──────────────────────────────────────────────────────────────────

TEST(NamingBand, RequiresSigil)
{
    EXPECT_TRUE (is_valid_identifier("!alerts",          IdentifierKind::Band));
    EXPECT_TRUE (is_valid_identifier("!alerts.critical", IdentifierKind::Band));
    EXPECT_FALSE(is_valid_identifier("alerts",           IdentifierKind::Band));
    EXPECT_FALSE(is_valid_identifier("!",                IdentifierKind::Band));
    EXPECT_FALSE(is_valid_identifier("@alerts",          IdentifierKind::Band));
}

TEST(NamingBand, RejectsMalformedBody)
{
    // Adjacent dots and leading/trailing dots in the body.
    EXPECT_FALSE(is_valid_identifier("!alerts..critical", IdentifierKind::Band));
    EXPECT_FALSE(is_valid_identifier("!.alerts",          IdentifierKind::Band));
    EXPECT_FALSE(is_valid_identifier("!alerts.",          IdentifierKind::Band));
    // First char of first body component must be a letter.
    EXPECT_FALSE(is_valid_identifier("!1alerts",          IdentifierKind::Band));
    // Whitespace inside body.
    EXPECT_FALSE(is_valid_identifier("!alerts critical",  IdentifierKind::Band));
}

TEST(NamingBand, HonoursLengthLimits)
{
    // Per-component cap = 64: "!" + 64 chars ok; "!" + 65 chars over.
    std::string ok = "!" + std::string(64, 'a');
    EXPECT_TRUE (is_valid_identifier(ok,                       IdentifierKind::Band));
    std::string over = "!" + std::string(65, 'a');
    EXPECT_FALSE(is_valid_identifier(over,                     IdentifierKind::Band));
}

// ─── RoleUid — 3+ components, tag ∈ {prod,cons,proc} ──────────────────────

TEST(NamingRoleUid, RequiresThreeComponentsAndValidTag)
{
    EXPECT_TRUE (is_valid_identifier("prod.cam1.pid42",           IdentifierKind::RoleUid));
    EXPECT_TRUE (is_valid_identifier("cons.logger.host-lab1.pid", IdentifierKind::RoleUid));
    EXPECT_TRUE (is_valid_identifier("proc.filter.abc123",        IdentifierKind::RoleUid));

    // Tag not in set.
    EXPECT_FALSE(is_valid_identifier("role.x.y",      IdentifierKind::RoleUid));
    EXPECT_FALSE(is_valid_identifier("hub.lab1.main", IdentifierKind::RoleUid));
    EXPECT_FALSE(is_valid_identifier("PROD.cam.x",    IdentifierKind::RoleUid));  // case-sensitive

    // Insufficient components.
    EXPECT_FALSE(is_valid_identifier("prod",           IdentifierKind::RoleUid));
    EXPECT_FALSE(is_valid_identifier("prod.cam1",      IdentifierKind::RoleUid));

    // Dash is allowed within a component, not as a separator.
    EXPECT_TRUE (is_valid_identifier("prod.cam-1.pid42", IdentifierKind::RoleUid));
    EXPECT_FALSE(is_valid_identifier("prod-cam1-pid42",  IdentifierKind::RoleUid)); // no dots at all
}

TEST(NamingRoleUid, RejectsAdjacentDotsAndLeadingDigit)
{
    EXPECT_FALSE(is_valid_identifier("prod..cam.pid",  IdentifierKind::RoleUid));
    EXPECT_FALSE(is_valid_identifier("prod.1cam.pid",  IdentifierKind::RoleUid));
}

// ─── RoleName — plain dotted; no reserved-word restriction ────────────────

TEST(NamingRoleName, AllowsPlainOrTaggedForm)
{
    EXPECT_TRUE (is_valid_identifier("cam1",          IdentifierKind::RoleName));
    EXPECT_TRUE (is_valid_identifier("prod.cam1",     IdentifierKind::RoleName));
    EXPECT_TRUE (is_valid_identifier("lab.sensor.temp", IdentifierKind::RoleName));
    EXPECT_FALSE(is_valid_identifier("",              IdentifierKind::RoleName));
    EXPECT_FALSE(is_valid_identifier(".cam1",         IdentifierKind::RoleName));
}

TEST(NamingRoleName, RejectsMalformedBody)
{
    EXPECT_FALSE(is_valid_identifier("cam1..test",    IdentifierKind::RoleName));
    EXPECT_FALSE(is_valid_identifier("cam1.",         IdentifierKind::RoleName));
    EXPECT_FALSE(is_valid_identifier("1cam",          IdentifierKind::RoleName));
    EXPECT_FALSE(is_valid_identifier("cam 1",         IdentifierKind::RoleName));
}

TEST(NamingRoleName, HonoursLengthLimits)
{
    std::string ok(64, 'a'); ok[0] = 'A';
    EXPECT_TRUE (is_valid_identifier(ok,                       IdentifierKind::RoleName));
    std::string over(65, 'a'); over[0] = 'A';
    EXPECT_FALSE(is_valid_identifier(over,                     IdentifierKind::RoleName));
}

// ─── PeerUid — 3+ components, tag == 'hub' ────────────────────────────────

TEST(NamingPeerUid, OnlyHubTag)
{
    EXPECT_TRUE (is_valid_identifier("hub.lab1.pid42",       IdentifierKind::PeerUid));
    EXPECT_TRUE (is_valid_identifier("hub.lab1.main.pid9876", IdentifierKind::PeerUid));

    // Role tags are not valid peer tags.
    EXPECT_FALSE(is_valid_identifier("prod.lab1.pid42", IdentifierKind::PeerUid));
    EXPECT_FALSE(is_valid_identifier("cons.lab1.pid42", IdentifierKind::PeerUid));
    EXPECT_FALSE(is_valid_identifier("proc.lab1.pid42", IdentifierKind::PeerUid));

    // Insufficient components.
    EXPECT_FALSE(is_valid_identifier("hub",       IdentifierKind::PeerUid));
    EXPECT_FALSE(is_valid_identifier("hub.lab1",  IdentifierKind::PeerUid));

    // No more '@' sigil.
    EXPECT_FALSE(is_valid_identifier("@lab1", IdentifierKind::PeerUid));
}

// ─── Schema ────────────────────────────────────────────────────────────────

TEST(NamingSchema, SigilAndBaseAndMandatoryVersion)
{
    // Valid: $<base>.v<digits> with ≥1 base component
    EXPECT_TRUE (is_valid_identifier("$foo.v1",              IdentifierKind::Schema));
    EXPECT_TRUE (is_valid_identifier("$hep.core.slot.v2",    IdentifierKind::Schema));
    EXPECT_TRUE (is_valid_identifier("$lab.sensors.temp.v42",IdentifierKind::Schema));
    EXPECT_TRUE (is_valid_identifier("$x.v0",                IdentifierKind::Schema));

    // Missing sigil
    EXPECT_FALSE(is_valid_identifier("hep.core.slot.v1", IdentifierKind::Schema));

    // Missing version component entirely (no .v tail)
    EXPECT_FALSE(is_valid_identifier("$foo",             IdentifierKind::Schema));
    EXPECT_FALSE(is_valid_identifier("$foo.bar",         IdentifierKind::Schema));
    EXPECT_FALSE(is_valid_identifier("$hep.core.slot",   IdentifierKind::Schema));

    // Malformed version components
    EXPECT_FALSE(is_valid_identifier("$foo.v",    IdentifierKind::Schema)); // no digits
    EXPECT_FALSE(is_valid_identifier("$foo.V1",   IdentifierKind::Schema)); // uppercase V
    EXPECT_FALSE(is_valid_identifier("$foo.v1a",  IdentifierKind::Schema)); // trailing alpha
    EXPECT_FALSE(is_valid_identifier("$foo.version1", IdentifierKind::Schema));

    // Old '@<version>' form rejected (one canonical form only)
    EXPECT_FALSE(is_valid_identifier("$hep@2",         IdentifierKind::Schema));
    EXPECT_FALSE(is_valid_identifier("hep.core@2",     IdentifierKind::Schema));

    // Edge: empty, sigil-only, sigil + nothing but digits
    EXPECT_FALSE(is_valid_identifier("",          IdentifierKind::Schema));
    EXPECT_FALSE(is_valid_identifier("$",         IdentifierKind::Schema));
    EXPECT_FALSE(is_valid_identifier("$.v1",      IdentifierKind::Schema)); // empty base component
}

TEST(NamingParseSchemaId, SplitsBaseAndVersion)
{
    auto p = parse_schema_id("$foo.v1");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->base,          "foo");
    EXPECT_EQ(p->version_token, "v1");
    EXPECT_EQ(p->version,       1u);

    auto p2 = parse_schema_id("$lab.sensors.temp.v42");
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->base,          "lab.sensors.temp");
    EXPECT_EQ(p2->version_token, "v42");
    EXPECT_EQ(p2->version,       42u);

    auto p3 = parse_schema_id("$x.v0");
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(p3->version, 0u);

    EXPECT_FALSE(parse_schema_id("$foo").has_value());
    EXPECT_FALSE(parse_schema_id("$foo.bar").has_value());
    EXPECT_FALSE(parse_schema_id("foo.v1").has_value());
    EXPECT_FALSE(parse_schema_id("$foo@2").has_value());
}

// ─── SysKey ────────────────────────────────────────────────────────────────

TEST(NamingSysKey, RequiresSysPrefixAndOneMoreComponent)
{
    EXPECT_TRUE (is_valid_identifier("sys.invalid_identifier_rejected", IdentifierKind::SysKey));
    EXPECT_TRUE (is_valid_identifier("sys.close.VoluntaryDereg",        IdentifierKind::SysKey));
    EXPECT_FALSE(is_valid_identifier("sys",       IdentifierKind::SysKey));     // tag only
    EXPECT_FALSE(is_valid_identifier("sys.",      IdentifierKind::SysKey));     // trailing dot
    EXPECT_FALSE(is_valid_identifier("system.x",  IdentifierKind::SysKey));     // prefix exact-match
    EXPECT_FALSE(is_valid_identifier(".sys.x",    IdentifierKind::SysKey));
}

TEST(NamingSysKey, RejectsMalformedBody)
{
    EXPECT_FALSE(is_valid_identifier("sys..x",       IdentifierKind::SysKey));  // adjacent dots
    EXPECT_FALSE(is_valid_identifier("sys.1counter", IdentifierKind::SysKey));  // digit-start after sys.
    EXPECT_FALSE(is_valid_identifier("sys.bad key",  IdentifierKind::SysKey));  // whitespace
}

TEST(NamingSysKey, HonoursLengthLimits)
{
    // "sys." (4) + 64 chars = 68 — valid (under per-component + total cap).
    std::string ok_tail(64, 'a'); ok_tail[0] = 'A';
    std::string ok = "sys." + ok_tail;
    EXPECT_TRUE (is_valid_identifier(ok,   IdentifierKind::SysKey));
    // Component over 64 → rejected even though total is well under 256.
    std::string over_tail(65, 'a'); over_tail[0] = 'A';
    std::string over = "sys." + over_tail;
    EXPECT_FALSE(is_valid_identifier(over, IdentifierKind::SysKey));
}

// ─── parse_role_uid ────────────────────────────────────────────────────────

TEST(NamingParseRoleUid, SplitsThreePartsAndRejectsInvalid)
{
    auto p = parse_role_uid("prod.cam1.pid42");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->tag,    "prod");
    EXPECT_EQ(p->name,   "cam1");
    EXPECT_EQ(p->unique, "pid42");

    // Unique suffix can itself contain dots — everything after the
    // second dot is treated as the unique segment.
    auto p2 = parse_role_uid("cons.logger.host-lab1.pid9876");
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2->tag,    "cons");
    EXPECT_EQ(p2->name,   "logger");
    EXPECT_EQ(p2->unique, "host-lab1.pid9876");

    EXPECT_FALSE(parse_role_uid("prod.cam1").has_value());
    EXPECT_FALSE(parse_role_uid("hub.cam1.pid").has_value()); // peer tag, not role
    EXPECT_FALSE(parse_role_uid("").has_value());
}

// ─── parse_peer_uid ────────────────────────────────────────────────────────

TEST(NamingParsePeerUid, SplitsThreePartsAndRejectsRoleTags)
{
    auto p = parse_peer_uid("hub.lab1.pid42");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->tag,    "hub");
    EXPECT_EQ(p->name,   "lab1");
    EXPECT_EQ(p->unique, "pid42");

    EXPECT_FALSE(parse_peer_uid("prod.cam1.pid42").has_value());
    EXPECT_FALSE(parse_peer_uid("hub.lab1").has_value());
}

// ─── extract_role_tag ──────────────────────────────────────────────────────

TEST(NamingExtractRoleTag, ValidOnly)
{
    EXPECT_EQ(extract_role_tag("prod.cam1.pid42").value_or(""), "prod");
    EXPECT_EQ(extract_role_tag("cons.x.y").value_or(""),        "cons");
    EXPECT_EQ(extract_role_tag("proc.x.y").value_or(""),        "proc");
    EXPECT_FALSE(extract_role_tag("hub.x.y").has_value());   // peer, not role
    EXPECT_FALSE(extract_role_tag("plain").has_value());
}

// ─── format_role_ref ───────────────────────────────────────────────────────

TEST(NamingFormatRoleRef, UidPreferredWhenPresent)
{
    EXPECT_EQ(format_role_ref("prod.cam1.pid42"),           "prod.cam1.pid42");
    EXPECT_EQ(format_role_ref("prod.cam1.pid42", "cam1", "prod"), "prod.cam1.pid42");

    // Uid empty — fall back to [tag] name.
    EXPECT_EQ(format_role_ref({}, "cam1", "prod"), "[prod] cam1");
    EXPECT_EQ(format_role_ref({}, "cam1"),          "cam1");
    EXPECT_EQ(format_role_ref({}, {}, "prod"),      "[prod]");
    EXPECT_EQ(format_role_ref({}),                  "");
}
