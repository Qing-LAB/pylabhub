/**
 * @file uuid_utils.cpp
 * @brief UUID4 generation and validation using libsodium.
 */
#include "utils/uuid_utils.hpp"
#include "utils/debug_info.hpp"
#include "utils/security/secure_memory_subsystem.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pylabhub::utils
{

std::string generate_uuid4()
{
    // SEC-Fold-2 migration (HEP-CORE-0043 §2.1): every libsodium call
    // in production code goes through the SecureSubsystem wrapper.
    // No `<sodium.h>` include in this file.  `secure().random_bytes`
    // PANICs at the module boundary if SMS isn't constructed
    // (broken singularity+init contract; HEP-0043 §1.2) — no runtime
    // "check + skip" branch here.
    std::byte bytes_raw[16];
    security::secure().random_bytes(
        std::span<std::byte>{bytes_raw, sizeof(bytes_raw)});
    // Retain uint8_t view for the RFC 4122 §4.4 bit-twiddling below.
    // std::byte and uint8_t are trivially interconvertible per
    // [basic.compound]; safe reinterpret_cast.
    auto *bytes = reinterpret_cast<uint8_t *>(bytes_raw);

    // RFC 4122 §4.4 — set version and variant nibbles.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0FU) | 0x40U);  // version = 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3FU) | 0x80U);  // variant = 10xx

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2],  bytes[3],
                  bytes[4], bytes[5],
                  bytes[6], bytes[7],
                  bytes[8], bytes[9],
                  bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string{buf};
}

bool is_valid_uuid4(const std::string &s)
{
    if (s.size() != 36U)
    {
        return false;
    }

    // Dashes at fixed positions
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
    {
        return false;
    }

    // Version nibble '4' at position 14
    if (s[14] != '4')
    {
        return false;
    }

    // Variant nibble in {8, 9, a, b} at position 19 (upper or lower case)
    const char v = s[19];
    if (v != '8' && v != '9' &&
        v != 'a' && v != 'b' &&
        v != 'A' && v != 'B')
    {
        return false;
    }

    // All non-dash positions must be hex digits
    for (std::size_t i = 0; i < 36U; ++i)
    {
        if (i == 8U || i == 13U || i == 18U || i == 23U)
        {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace pylabhub::utils
