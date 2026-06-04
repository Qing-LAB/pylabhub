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
