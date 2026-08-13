/**
 * @file test_net_address.cpp
 * @brief Unit tests for validate_tcp_endpoint() — ZMQ endpoint validation.
 *
 * Tests IPv4, IPv6, DNS hostname, port range, and error cases.
 */
#include <gtest/gtest.h>

#include "utils/net_address.hpp"

using pylabhub::AddressType;
using pylabhub::validate_tcp_endpoint;

// ============================================================================
// IPv4 — valid
// ============================================================================

TEST(NetAddressTest, IPv4_Loopback)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:5570");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv4);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_EQ(r.port, 5570);
    EXPECT_FALSE(r.is_network_exposed());
}

TEST(NetAddressTest, IPv4_AllInterfaces)
{
    auto r = validate_tcp_endpoint("tcp://0.0.0.0:5570");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv4);
    EXPECT_EQ(r.host, "0.0.0.0");
    EXPECT_EQ(r.port, 5570);
    EXPECT_TRUE(r.is_network_exposed());
}

TEST(NetAddressTest, IPv4_AutoPort)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv4);
    EXPECT_EQ(r.port, 0);
}

TEST(NetAddressTest, IPv4_HighPort)
{
    auto r = validate_tcp_endpoint("tcp://192.168.1.100:65535");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv4);
    EXPECT_EQ(r.port, 65535);
}

// ============================================================================
// IPv6 — valid
// ============================================================================

TEST(NetAddressTest, IPv6_Loopback)
{
    auto r = validate_tcp_endpoint("tcp://[::1]:5570");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv6);
    EXPECT_EQ(r.host, "::1");
    EXPECT_EQ(r.port, 5570);
    EXPECT_FALSE(r.is_network_exposed());
}

TEST(NetAddressTest, IPv6_AllInterfaces)
{
    auto r = validate_tcp_endpoint("tcp://[::]:0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv6);
    EXPECT_EQ(r.host, "::");
    EXPECT_TRUE(r.is_network_exposed());
}

TEST(NetAddressTest, IPv6_FullAddress)
{
    auto r = validate_tcp_endpoint("tcp://[fe80::1]:8080");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::IPv6);
    EXPECT_EQ(r.host, "fe80::1");
    EXPECT_EQ(r.port, 8080);
}

// ============================================================================
// DNS hostname — valid
// ============================================================================

TEST(NetAddressTest, Hostname_Simple)
{
    auto r = validate_tcp_endpoint("tcp://localhost:5570");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::Hostname);
    EXPECT_EQ(r.host, "localhost");
    EXPECT_EQ(r.port, 5570);
}

TEST(NetAddressTest, Hostname_FQDN)
{
    auto r = validate_tcp_endpoint("tcp://sensor-hub.lab.example.com:9090");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::Hostname);
    EXPECT_EQ(r.host, "sensor-hub.lab.example.com");
}

TEST(NetAddressTest, Hostname_WithHyphen)
{
    auto r = validate_tcp_endpoint("tcp://my-host:0");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.type, AddressType::Hostname);
}

// ============================================================================
// Invalid — protocol
// ============================================================================

TEST(NetAddressTest, Invalid_NoProtocol)
{
    auto r = validate_tcp_endpoint("127.0.0.1:5570");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.type, AddressType::Invalid);
}

TEST(NetAddressTest, Invalid_WrongProtocol)
{
    auto r = validate_tcp_endpoint("udp://127.0.0.1:5570");
    EXPECT_FALSE(r.ok());
}

// ============================================================================
// Invalid — port
// ============================================================================

TEST(NetAddressTest, Invalid_PortTooHigh)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:70000");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_PortNegative)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:-1");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_PortNotNumeric)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:abc");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_MissingPort)
{
    auto r = validate_tcp_endpoint("tcp://127.0.0.1");
    EXPECT_FALSE(r.ok());
}

// ============================================================================
// Invalid — host
// ============================================================================

TEST(NetAddressTest, Invalid_EmptyHost)
{
    auto r = validate_tcp_endpoint("tcp://:5570");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_HostnameLeadingHyphen)
{
    auto r = validate_tcp_endpoint("tcp://-bad.host:5570");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_HostnameTrailingHyphen)
{
    auto r = validate_tcp_endpoint("tcp://bad-.host:5570");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_HostnameSpecialChars)
{
    auto r = validate_tcp_endpoint("tcp://bad host:5570");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_IPv6_MissingBracket)
{
    auto r = validate_tcp_endpoint("tcp://[::1:5570");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, Invalid_IPv6_BadChar)
{
    auto r = validate_tcp_endpoint("tcp://[::g1]:5570");
    EXPECT_FALSE(r.ok());
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(NetAddressTest, Empty_String)
{
    auto r = validate_tcp_endpoint("");
    EXPECT_FALSE(r.ok());
}

TEST(NetAddressTest, IPv4_Port0_IsDefault)
{
    // The default inbox endpoint.
    auto r = validate_tcp_endpoint("tcp://127.0.0.1:0");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.type, AddressType::IPv4);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_EQ(r.port, 0);
    EXPECT_FALSE(r.is_network_exposed());
}

// ============================================================================
// BoundAddress — the second role a TCP endpoint plays
// ============================================================================
//
// `validate_tcp_endpoint` above answers "is this a well-formed bind
// request", and correctly accepts port 0.  `BoundAddress` is the other
// role: an address a peer can actually dial.  Its whole contract is that
// it cannot be constructed from anything unusable, so these tests pin the
// refusals as hard as the acceptances (HEP-CORE-0036 §6.7.2).

TEST(BoundAddressTest, ResolvedIPv4_Parses)
{
    auto b = pylabhub::BoundAddress::try_validate("tcp://127.0.0.1:51234");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->str(), "tcp://127.0.0.1:51234");
}

TEST(BoundAddressTest, ResolvedIPv6_Parses)
{
    auto b = pylabhub::BoundAddress::try_validate("tcp://[::1]:51234");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->str(), "tcp://[::1]:51234");
}

TEST(BoundAddressTest, ResolvedHostname_Parses)
{
    auto b = pylabhub::BoundAddress::try_validate("tcp://sensor-hub.lab.example.com:9090");
    ASSERT_TRUE(b.has_value());
}

TEST(BoundAddressTest, PortZero_IsRefused)
{
    // THE contract.  Port 0 is a legal bind REQUEST — the assertion just
    // above (`IPv4_Port0_IsDefault`) pins that, and it is the inbox
    // default — but nothing can connect to it, so it is not an address.
    // If this ever passes, the type has stopped being worth having.
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://127.0.0.1:0").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://0.0.0.0:0").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://[::]:0").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://my-host:0").has_value());
}

TEST(BoundAddressTest, Malformed_IsRefused)
{
    // The six broker call sites this type replaced tested
    // `ok() && port == 0`, which ACCEPTED anything that failed to parse.
    // Parsing must refuse both kinds of unusable input, not just port 0.
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("127.0.0.1:5570").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("udp://127.0.0.1:5570").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://127.0.0.1").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://127.0.0.1:70000").has_value());
    EXPECT_FALSE(pylabhub::BoundAddress::try_validate("tcp://bad host:5570").has_value());
}
