/**
 * @file curve_keypair.cpp
 * @brief Implementation of `generate_curve_keypair`.
 *
 * Single TU; thin shim over libzmq's `zmq_curve_keypair`.  See
 * `curve_keypair.hpp` for the rationale (4 production duplications
 * consolidated to one entry point).
 */
#include "utils/security/curve_keypair.hpp"

#include <sodium.h>
#include <zmq.h>

#include <array>
#include <stdexcept>

namespace pylabhub::utils::security
{

namespace
{
// Z85-encoded CURVE key is 40 ASCII chars; libzmq's API writes 41 bytes
// including the trailing NUL.  Keep these as locals — they're tiny and
// the production callers all use the same constants.
constexpr std::size_t kZ85KeyLen = 40;
constexpr std::size_t kZ85BufLen = 41;
} // namespace

namespace
{

/// Z85 alphabet per RFC 32 §4 — the 85 printable ASCII chars libzmq
/// accepts on `zmq_z85_decode` for CURVE keys.  Compile-time lookup
/// table built once; index by `static_cast<unsigned char>(c)`.
constexpr bool make_z85_alphabet_table_(unsigned char c)
{
    // Decimal digits, lower-case, upper-case.
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    // The 23 punctuation chars in the Z85 alphabet:
    //   .-:+=^!/*?&<>()[]{}@%$#
    switch (c)
    {
    case '.': case '-': case ':': case '+': case '=':
    case '^': case '!': case '/': case '*': case '?':
    case '&': case '<': case '>': case '(': case ')':
    case '[': case ']': case '{': case '}': case '@':
    case '%': case '$': case '#':
        return true;
    }
    return false;
}

} // namespace

Z85PublicKey::Z85PublicKey() noexcept
    : z85_(Z85PublicKey::kZ85Chars, '\0')
{}

Z85PublicKey::Z85PublicKey(std::string_view z85)
    : z85_(z85.begin(), z85.end())
{
    if (z85.size() != Z85PublicKey::kZ85Chars)
    {
        throw std::invalid_argument(
            "pylabhub::utils::security::Z85PublicKey: input length " +
            std::to_string(z85.size()) +
            " is not the required " +
            std::to_string(Z85PublicKey::kZ85Chars) +
            " chars (CURVE public key, Z85-encoded — RFC 32 §4)");
    }
    for (std::size_t i = 0; i < z85.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(z85[i]);
        if (!make_z85_alphabet_table_(c))
        {
            throw std::invalid_argument(
                "pylabhub::utils::security::Z85PublicKey: input contains "
                "non-Z85 character at position " + std::to_string(i) +
                " (byte 0x" +
                [](unsigned char b) {
                    const char hex[] = "0123456789abcdef";
                    std::string s{hex[b >> 4], hex[b & 0xF]};
                    return s;
                }(c) +
                "); the Z85 alphabet is 0-9 a-z A-Z .-:+=^!/*?&<>()[]{}@%$# "
                "per RFC 32 §4");
        }
    }
}

bool Z85PublicKey::empty() const noexcept
{
    // Sentinel value is 40 zero bytes; that's the ONLY shape the
    // default ctor produces.  Any validated ctor input contains
    // only Z85 chars (which excludes \0).
    for (char c : z85_) {
        if (c != '\0') return false;
    }
    return true;
}

CurveKeypair generate_curve_keypair()
{
    std::array<char, kZ85BufLen> pub{};
    std::array<char, kZ85BufLen> sec{};
    if (::zmq_curve_keypair(pub.data(), sec.data()) != 0)
    {
        ::sodium_memzero(sec.data(), sec.size());
        ::sodium_memzero(pub.data(), pub.size());
        throw std::runtime_error(
            "pylabhub::utils::security::generate_curve_keypair: "
            "zmq_curve_keypair failed (libzmq CSPRNG init?)");
    }
    CurveKeypair out{
        std::string(pub.data(), kZ85KeyLen),
        std::string(sec.data(), kZ85KeyLen),
    };
    // Zero the local stack buffers after copying into out.  The
    // out.secret_z85 std::string still holds the secret; HEP-CORE-0035
    // §4.7 task #102 owns the in-process secret-lifetime story
    // (mlock + no-core-dump + zeroize-on-destroy).  This memzero is
    // the strictly local-buffer cleanup the vault paths already
    // performed; consolidating it here makes broker_service.cpp and
    // broker_request_comm.cpp inherit the same discipline.
    ::sodium_memzero(sec.data(), sec.size());
    ::sodium_memzero(pub.data(), pub.size());
    return out;
}

} // namespace pylabhub::utils::security
