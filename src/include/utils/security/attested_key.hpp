#pragma once
/**
 * @file attested_key.hpp
 * @brief `AttestedKey` — a CURVE public key the transport VOUCHED for,
 *        and the one function permitted to mint one.
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
 * is a lie.  `attest_from_transport()` refuses unless the domain is
 * registered with the live `ZapRouter`.
 *
 * See HEP-CORE-0035 §4.2 and the design draft §3 / §5b.
 */
#include "pylabhub_utils_export.h"
#include "utils/security/peer_admission.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::utils::security
{

class PYLABHUB_UTILS_EXPORT AttestedKey
{
  public:
    /// The Z85 public key the transport attested (40 chars).
    [[nodiscard]] std::string_view z85() const noexcept { return key_; }

    /// Projection for admission APIs that speak `PeerIdentity`.
    [[nodiscard]] PeerIdentity as_peer_identity() const { return PeerIdentity{"curve", key_}; }

  private:
    // Ingress-only construction.  Do NOT add a public constructor, and do
    // NOT add a factory taking a bare string: either would reopen the hole
    // this type exists to close.
    friend PYLABHUB_UTILS_EXPORT std::optional<AttestedKey>
    attest_from_transport(std::string_view zap_domain, std::string_view transport_user_id);

    explicit AttestedKey(std::string key) noexcept : key_(std::move(key)) {}

    std::string key_;
};

/// Mint an attestation for one inbound message, or refuse.
///
/// @param zap_domain          the receiving socket's `ZMQ_ZAP_DOMAIN`,
///                            resolved once when the socket was bound.
/// @param transport_user_id   the `User-Id` metadata libzmq attached to the
///                            message, or empty if the connection carried
///                            no security mechanism.
///
/// Returns `nullopt` — meaning "nothing was attested here" — when:
///   - @p zap_domain is empty or is not registered with the live
///     `ZapRouter` (no enforcement ran, so any `User-Id` present is
///     unvouched and MUST NOT be dressed up as proof);
///   - @p transport_user_id is empty (NULL-mechanism connection: in-process
///     harnesses, non-CURVE transports);
///   - the value is not a well-formed 40-character Z85 key.
///
/// Absence is a legitimate state, not an error.  Planes that require an
/// attestation reject on absence; planes that do not, proceed.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<AttestedKey>
attest_from_transport(std::string_view zap_domain, std::string_view transport_user_id);

} // namespace pylabhub::utils::security
