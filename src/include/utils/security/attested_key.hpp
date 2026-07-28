#pragma once
/**
 * @file attested_key.hpp
 * @brief `AttestedKey` — a CURVE public key the transport VOUCHED for,
 *        and the one factory permitted to mint one.
 *
 * **A public key is never trusted; an attestation is.**  A public key is
 * published — it crosses the wire in every handshake, sits in operator
 * config, and is written to `hub.pubkey` world-readable on purpose.
 * Presenting one proves nothing, because anyone can copy one.  What the
 * CURVE handshake proves is possession of the matching *private* key; the
 * public key is only the NAME of what was proven.
 *
 * So the same forty bytes mean entirely different things depending on how
 * they reached us:
 *
 *   - in a request body — a **claim**: the sender wrote those bytes;
 *   - as ZAP-established handshake metadata — an **attestation**: this
 *     hub's own ZAP handler having verified the peer and told libzmq to
 *     stamp that key on every message from the connection.
 *
 * Trust lives in the provenance, never in the value.  This type exists so
 * that distinction survives a function call: the identity index accepts an
 * `AttestedKey`, not a `string_view`, so a caller holding a body-supplied
 * key has nothing to pass.  The mistake fails to COMPILE instead of quietly
 * laundering a claim into an identity — which is the exact defect the
 * surrounding work exists to close.
 *
 * **Why minting belongs to the ZAP module.**  Only ZAP can answer whether a
 * handshake was actually enforced on a given socket, because it owns the
 * zap_domain → admission table.  That question is not decorative:
 *
 *   - a peer MAY send its own ZMTP metadata property literally named
 *     `User-Id` (libzmq `mechanism.cpp` applies no name filter);
 *   - it is harmless only because `stream_engine_base.cpp` inserts ZAP
 *     properties FIRST and `std::map::insert` does not overwrite;
 *   - **that protection exists only where ZAP actually ran.**  On a socket
 *     with no enforced domain, a client-supplied `User-Id` lands unopposed.
 *
 * Minting anywhere else would therefore produce an `AttestedKey` whose name
 * is a lie.  `AttestedKey::from_transport()` refuses unless the domain is
 * registered with the live `ZapRouter`.
 *
 * See HEP-CORE-0035 §4.2 and the design draft §3 / §5b.
 */
#include "pylabhub_utils_export.h"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/peer_admission.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::utils::security
{

class PYLABHUB_UTILS_EXPORT AttestedKey
{
  public:
    /// Mint an attestation for one inbound message, or refuse.
    ///
    /// The ONLY way to obtain an `AttestedKey`.  A private constructor plus
    /// this single checked factory is what makes the type's name true:
    /// there is no path to an instance that skipped the check.
    ///
    /// @param zap_domain        the receiving socket's `ZMQ_ZAP_DOMAIN`.
    /// @param transport_user_id the `User-Id` metadata libzmq attached to
    ///                          the message, or empty when the connection
    ///                          carried no security mechanism.
    ///
    /// Returns `nullopt` — "nothing was attested here" — when:
    ///   - @p zap_domain is empty or is not registered with the live
    ///     `ZapRouter`, meaning no enforcement ran, so any `User-Id`
    ///     present is unvouched and must not be dressed up as proof;
    ///   - @p transport_user_id is empty (NULL-mechanism connection:
    ///     in-process harnesses, non-CURVE transports);
    ///   - the value is not a well-formed Z85 key (delegated to
///     `Z85PublicKey::validate`, which checks the ALPHABET and not
///     merely the length — a length-only check was the gap that made
///     this type worth revisiting).
    ///
    /// Absence is a legitimate state, not an error.  Planes that require an
    /// attestation reject on absence; planes that do not, proceed.
    [[nodiscard]] static std::optional<AttestedKey>
    from_transport(std::string_view zap_domain, std::string_view transport_user_id);

    /// The public key the transport attested.
    ///
    /// Held as `Z85PublicKey`, not a raw string: that type is the
    /// project's validated CURVE-pubkey representation (HEP-CORE-0040
    /// §8.4), so an `AttestedKey` cannot hold 40 bytes of arbitrary
    /// rubbish that merely happen to be the right length.
    [[nodiscard]] const Z85PublicKey &key() const noexcept { return key_; }

    /// Projection for admission APIs that speak `PeerIdentity`.
    [[nodiscard]] PeerIdentity as_peer_identity() const
    {
        return PeerIdentity{"curve", key_.str()};
    }

  private:
    // Construction is private and `from_transport` is the only factory, so
    // an instance cannot exist without having passed the enforcement check.
    // Do NOT add a public constructor and do NOT add a second factory
    // taking a bare string: either reopens the hole this type closes.
    //
    // Note there is deliberately no `friend` here.  An earlier draft put
    // the factory outside the class as a free function, which then needed
    // friendship to reach this constructor — encapsulation weakened to
    // accommodate an arbitrary placement choice.  The factory belongs with
    // the type it constructs.
    explicit AttestedKey(Z85PublicKey key) noexcept : key_(std::move(key)) {}

    Z85PublicKey key_;
};

} // namespace pylabhub::utils::security
