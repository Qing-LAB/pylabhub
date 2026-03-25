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
