/**
 * @file curve_keypair.cpp
 * @brief Implementation of `generate_curve_keypair`.
 *
 * Single TU; thin shim over libzmq's `zmq_curve_keypair`.  See
 * `curve_keypair.hpp` for the rationale (4 production duplications
 * consolidated to one entry point).
 */
#include "utils/security/curve_keypair.hpp"

#include <algorithm>
#include <optional>
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
    if (c >= '0' && c <= '9')
        return true;
    if (c >= 'a' && c <= 'z')
        return true;
    if (c >= 'A' && c <= 'Z')
        return true;
    // The 23 punctuation chars in the Z85 alphabet:
    //   .-:+=^!/*?&<>()[]{}@%$#
    switch (c)
    {
    case '.':
    case '-':
    case ':':
    case '+':
    case '=':
    case '^':
    case '!':
    case '/':
    case '*':
    case '?':
    case '&':
    case '<':
    case '>':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '@':
    case '%':
    case '$':
    case '#':
        return true;
    }
    return false;
}

} // namespace

Z85PublicKey::Z85PublicKey() noexcept = default; // z85_ is zero-initialized in-class

namespace
{

/// Operator-facing explanation of WHY @p z85 is not a well-formed key.
///
/// Only ever called on input `try_validate` has already rejected, so it
/// cannot admit anything the rule refused — the worst it could do if it ever
/// drifted is describe a throw vaguely, never turn a reject into an accept.
/// Kept off the accepting path because building this text allocates, and the
/// accepting path runs per inbound message.
std::string z85_pubkey_reject_reason(std::string_view z85)
{
    if (z85.size() != Z85PublicKey::kZ85Chars)
    {
        return "input length " + std::to_string(z85.size()) + " is not the required " +
               std::to_string(Z85PublicKey::kZ85Chars) +
               " chars (CURVE public key, Z85-encoded — RFC 32 §4)";
    }
    for (std::size_t i = 0; i < z85.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(z85[i]);
        if (!make_z85_alphabet_table_(c))
        {
            const char hex[] = "0123456789abcdef";
            const std::string byte_hex{hex[c >> 4], hex[c & 0xF]};
            return "input contains non-Z85 character at position " + std::to_string(i) +
                   " (byte 0x" + byte_hex +
                   "); the Z85 alphabet is 0-9 a-z A-Z .-:+=^!/*?&<>()[]{}@%$# per RFC 32 §4";
        }
    }
    // Unreachable via `validate`, which only asks after a rejection.  A
    // generic message rather than an assert: this is the error path, and a
    // vague throw beats aborting while reporting an error.
    return "input is not a well-formed Z85 CURVE public key";
}

} // namespace

std::optional<Z85PublicKey> Z85PublicKey::try_validate(std::string_view z85) noexcept
{
    // THE RULE.  This is the single authority on what a well-formed Z85
    // public key is; `validate` is this function plus a throw.  The verdict
    // is the primitive and throwing is a policy over it, not the other way
    // round — an earlier revision of this file had the two paths carrying
    // separate copies of the length check and the alphabet loop, which is
    // exactly how a throwing and a non-throwing ingress come to disagree
    // about what is authentic.
    if (z85.size() != Z85PublicKey::kZ85Chars)
        return std::nullopt;
    for (const char ch : z85)
    {
        if (!make_z85_alphabet_table_(static_cast<unsigned char>(ch)))
            return std::nullopt;
    }
    Z85PublicKey result;
    std::copy(z85.begin(), z85.end(), result.z85_.begin());
    return result;
}

Z85PublicKey Z85PublicKey::validate(std::string_view z85)
{
    if (auto key = try_validate(z85))
        return *std::move(key);
    // Rejected.  Re-scanning to build the diagnostic costs nothing here:
    // this path already ends in an exception.
    throw std::invalid_argument("pylabhub::utils::security::Z85PublicKey::validate: " +
                                z85_pubkey_reject_reason(z85));
}

bool Z85PublicKey::empty() const noexcept
{
    // Sentinel value is 40 zero bytes; that's the ONLY shape the
    // default ctor produces.  Any validated ctor input contains
    // only Z85 chars (which excludes \0).
    for (char c : z85_)
    {
        if (c != '\0')
            return false;
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
        throw std::runtime_error("pylabhub::utils::security::generate_curve_keypair: "
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
