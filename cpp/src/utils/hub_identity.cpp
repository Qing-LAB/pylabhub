/**
 * @file hub_identity.cpp
 * @brief UUID4 generation and validation using libsodium.
 */
#include "utils/hub_identity.hpp"

#include <sodium.h>

#include <cctype>
#include <cstdint>
#include <cstdio>

namespace pylabhub::utils
{

std::string generate_uuid4()
{
    // sodium_init() is idempotent and thread-safe; returns 0 (first call),
    // 1 (already initialised), or -1 (catastrophic failure — rare; randombytes_buf
    // still works on Linux via getrandom but behaviour on other platforms is
    // undefined). Capture the result so warn_unused_result is satisfied.
    const int sodium_rc = sodium_init();
    (void)sodium_rc;

    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));

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
    return std::string(buf);
}

bool is_valid_uuid4(const std::string &s)
{
    if (s.size() != 36U)
        return false;

    // Dashes at fixed positions
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;

    // Version nibble '4' at position 14
    if (s[14] != '4')
        return false;

    // Variant nibble in {8, 9, a, b} at position 19 (upper or lower case)
    const char v = s[19];
    if (v != '8' && v != '9' &&
        v != 'a' && v != 'b' &&
        v != 'A' && v != 'B')
        return false;

    // All non-dash positions must be hex digits
    for (std::size_t i = 0; i < 36U; ++i)
    {
        if (i == 8U || i == 13U || i == 18U || i == 23U)
            continue;
        if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0)
            return false;
    }
    return true;
}

} // namespace pylabhub::utils
