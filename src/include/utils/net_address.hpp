#pragma once
/**
 * @file net_address.hpp
 * @brief TCP endpoint validation for ZMQ bind/connect addresses.
 *
 * Provides a reusable validator for ZMQ TCP endpoint strings used throughout
 * the pyLabHub configuration layer (inbox, broker, data transport, admin shell).
 *
 * Supports:
 * - IPv4:     "tcp://192.168.1.1:5555"
 * - IPv6:     "tcp://[::1]:5555"           (bracketed per ZMQ/RFC 2732)
 * - Hostname: "tcp://myhost.local:5555"    (DNS name, resolved at bind/connect)
 * - Auto port: any of the above with port 0 (OS assigns ephemeral port)
 *
 * ## Security note
 *
 * Binding on 0.0.0.0 (IPv4) or [::] (IPv6) exposes the socket to the network.
 * Check is_network_exposed() and ensure CurveZMQ encryption is enabled for
 * network-facing endpoints.
 */

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <ws2tcpip.h> // inet_pton
#else
#include <arpa/inet.h> // inet_pton
#endif

namespace pylabhub
{

/// Detected address type from endpoint parsing.
enum class AddressType : uint8_t
{
    IPv4,      ///< Dotted decimal (e.g. "192.168.1.1").
    IPv6,      ///< Hex colon notation (e.g. "::1", "fe80::1").
    Hostname,  ///< DNS name (e.g. "myhost.local").
    Invalid,   ///< Failed validation.
};

/// Result of validate_tcp_endpoint().
struct EndpointValidation
{
    AddressType type{AddressType::Invalid};
    std::string host;          ///< Parsed host (without brackets for IPv6).
    uint16_t    port{0};       ///< Parsed port number.
    std::string error;         ///< Empty on success; human-readable error on failure.

    /// True if validation succeeded.
    [[nodiscard]] bool ok() const noexcept { return type != AddressType::Invalid; }

    /// True if the host binds on all interfaces (0.0.0.0 or [::]).
    [[nodiscard]] bool is_network_exposed() const noexcept
    {
        return host == "0.0.0.0" || host == "::" || host == "0:0:0:0:0:0:0:0";
    }
};

/**
 * @brief Validate a ZMQ TCP endpoint string.
 *
 * Expected format: "tcp://<host>:<port>" or "tcp://[<ipv6>]:<port>"
 *
 * Detection logic:
 * 1. If host starts with '[', parse as IPv6 (bracketed).
 * 2. Try inet_pton(AF_INET, host) — if success, it's IPv4.
 * 3. Try inet_pton(AF_INET6, host) — if success, it's IPv6 (unbracketd — unusual but valid).
 * 4. Validate as DNS hostname (RFC 1123: alphanumeric + hyphens + dots,
 *    labels max 63 chars, total max 253 chars, no leading/trailing hyphens per label).
 * 5. Otherwise, Invalid.
 *
 * @param endpoint  Full ZMQ endpoint string (e.g. "tcp://127.0.0.1:5570").
 * @return Structured validation result.
 */
inline EndpointValidation validate_tcp_endpoint(const std::string &endpoint)
{
    EndpointValidation result;

    // Must start with "tcp://".
    if (endpoint.rfind("tcp://", 0) != 0)
    {
        result.error = "endpoint must start with 'tcp://' — got '" + endpoint + "'";
        return result;
    }

    const auto hostport = endpoint.substr(6); // after "tcp://"
    std::string host_str;
    std::string port_str;

    // ── Parse host and port ─────────────────────────────────────────────
    if (!hostport.empty() && hostport[0] == '[')
    {
        // IPv6 bracketed: [addr]:port
        const auto bracket_end = hostport.find(']');
        if (bracket_end == std::string::npos)
        {
            result.error = "IPv6 address missing closing ']' — got '" + endpoint + "'";
            return result;
        }
        host_str = hostport.substr(1, bracket_end - 1);
        if (bracket_end + 1 >= hostport.size() || hostport[bracket_end + 1] != ':')
        {
            result.error = "expected ':port' after ']' — got '" + endpoint + "'";
            return result;
        }
        port_str = hostport.substr(bracket_end + 2);
    }
    else
    {
        // IPv4 or hostname: host:port (last colon separates port).
        const auto colon = hostport.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon == hostport.size() - 1)
        {
            result.error = "must be 'tcp://<host>:<port>' — got '" + endpoint + "'";
            return result;
        }
        host_str = hostport.substr(0, colon);
        port_str = hostport.substr(colon + 1);
    }

    if (host_str.empty())
    {
        result.error = "host is empty — got '" + endpoint + "'";
        return result;
    }

    // ── Validate port ───────────────────────────────────────────────────
    char *end = nullptr;
    const long port_val = std::strtol(port_str.c_str(), &end, 10);
    if (end == port_str.c_str() || *end != '\0' || port_val < 0 || port_val > 65535)
    {
        result.error = "port must be 0-65535 — got '" + port_str + "'";
        return result;
    }

    // ── Detect and validate host type ───────────────────────────────────

    // Try IPv4 (inet_pton returns 1 on success).
    struct in_addr addr4{};
    if (inet_pton(AF_INET, host_str.c_str(), &addr4) == 1)
    {
        result.type = AddressType::IPv4;
        result.host = host_str;
        result.port = static_cast<uint16_t>(port_val);
        return result;
    }

    // Try IPv6.
    struct in6_addr addr6{};
    if (inet_pton(AF_INET6, host_str.c_str(), &addr6) == 1)
    {
        result.type = AddressType::IPv6;
        result.host = host_str;
        result.port = static_cast<uint16_t>(port_val);
        return result;
    }

    // Try DNS hostname (RFC 1123).
    // Rules: labels separated by dots, each label 1-63 chars, total max 253 chars.
    // Label chars: alphanumeric + hyphen. No leading/trailing hyphen per label.
    if (host_str.size() > 253)
    {
        result.error = "hostname exceeds 253 characters — got '" + host_str + "'";
        return result;
    }

    // Split into labels and validate each.
    size_t label_start = 0;
    bool valid_hostname = true;
    std::string hostname_error;

    for (size_t i = 0; i <= host_str.size(); ++i)
    {
        if (i == host_str.size() || host_str[i] == '.')
        {
            const size_t label_len = i - label_start;
            if (label_len == 0 || label_len > 63)
            {
                hostname_error = "label length must be 1-63 chars";
                valid_hostname = false;
                break;
            }
            if (host_str[label_start] == '-' || host_str[i - 1] == '-')
            {
                hostname_error = "label must not start or end with hyphen";
                valid_hostname = false;
                break;
            }
            for (size_t j = label_start; j < i; ++j)
            {
                char c = host_str[j];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '-'))
                {
                    hostname_error = std::string("invalid character '") + c + "' in hostname";
                    valid_hostname = false;
                    break;
                }
            }
            if (!valid_hostname)
                break;
            label_start = i + 1;
        }
    }

    if (valid_hostname)
    {
        result.type = AddressType::Hostname;
        result.host = host_str;
        result.port = static_cast<uint16_t>(port_val);
        return result;
    }

    result.error = "invalid host '" + host_str + "': " + hostname_error;
    return result;
}

} // namespace pylabhub
