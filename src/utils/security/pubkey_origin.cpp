/**
 * @file pubkey_origin.cpp
 * @brief Implementation of the pubkey origin index (HEP-CORE-0035 §4.2).
 */
#include "utils/security/pubkey_origin.hpp"

#include "utils/role_identity_policy.hpp" // broker::KnownRole

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace pylabhub::utils::security
{

namespace
{

constexpr std::size_t kZ85PubkeyChars = 40;

const char *kind_label(PubkeyOrigin::Kind k) noexcept
{
    return k == PubkeyOrigin::Kind::LocalRole ? "role" : "federation peer";
}

} // namespace

void PeerAuthority::Builder::insert_(std::string_view pubkey_z85, PubkeyOrigin origin)
{
    // Validation is Z85PublicKey's job, not a length check repeated here.
    // Constructing the key IS the validation, and because the map is keyed
    // on that type an unvalidated key cannot be stored by any path.
    Z85PublicKey key;
    try
    {
        key = Z85PublicKey::validate(pubkey_z85);
    }
    catch (const std::invalid_argument &e)
    {
        throw std::runtime_error("pubkey origin index: " + std::string(kind_label(origin.kind)) +
                                 " '" + origin.subject_uid +
                                 "' has an invalid CURVE key: " + e.what());
    }

    const auto it = by_pubkey_.find(key);
    if (it != by_pubkey_.end())
    {
        // Re-registering the same subject with the same key is a no-op;
        // registering a second subject under one key is a configuration
        // error we refuse rather than resolve arbitrarily.  If this were
        // allowed, the identity a connection resolves to would depend on
        // config ordering — every gate above would silently inherit that
        // ambiguity.
        if (it->second.kind == origin.kind && it->second.subject_uid == origin.subject_uid)
            return;

        throw std::runtime_error(
            "pubkey origin index: one CURVE key is configured for two different subjects — " +
            std::string(kind_label(it->second.kind)) + " '" + it->second.subject_uid + "' and " +
            std::string(kind_label(origin.kind)) + " '" + origin.subject_uid +
            "'. A key identifies exactly one subject; give each its own key "
            "(plh_hub --init / --keygen).");
    }

    by_pubkey_.emplace(std::move(key), std::move(origin));
}

void PeerAuthority::Builder::add_local_role(const ::pylabhub::broker::KnownRole &role)
{
    insert_(role.pubkey_z85, PubkeyOrigin{PubkeyOrigin::Kind::LocalRole, role.uid});
}

void PeerAuthority::Builder::add_local_role(const RosterEntry &entry)
{
    insert_(entry.pubkey_z85, PubkeyOrigin{PubkeyOrigin::Kind::LocalRole, entry.uid});
}

void PeerAuthority::Builder::add_federation_peer(std::string_view peer_uid,
                                                 std::string_view pubkey_z85)
{
    insert_(pubkey_z85, PubkeyOrigin{PubkeyOrigin::Kind::FederationPeer, std::string(peer_uid)});
}

PeerAuthority PeerAuthority::Builder::build(std::uint64_t version) &&
{
    return PeerAuthority(std::move(by_pubkey_), version);
}

bool PeerAuthority::admits(const Z85PublicKey &key) const noexcept
{
    return by_pubkey_.find(key) != by_pubkey_.end();
}

const PubkeyOrigin *PeerAuthority::resolve_(const AttestedKey &attested) const
{
    const auto it = by_pubkey_.find(attested.key());
    if (it == by_pubkey_.end())
        return nullptr;
    return &it->second;
}

} // namespace pylabhub::utils::security

namespace pylabhub::utils::security
{

std::string_view to_string(ClaimVerdict v) noexcept
{
    switch (v)
    {
    case ClaimVerdict::accepted:
        return "accepted";
    case ClaimVerdict::no_attestation:
        return "no_attestation";
    case ClaimVerdict::unknown_key:
        return "unknown_key";
    case ClaimVerdict::identity_mismatch:
        return "identity_mismatch";
    case ClaimVerdict::pubkey_mismatch:
        return "pubkey_mismatch";
    case ClaimVerdict::kind_not_permitted:
        return "kind_not_permitted";
    }
    return "unknown";
}

AttributedSender PeerAuthority::attribute_sender(const std::optional<AttestedKey> &attested) const
{
    // The root question the other two are built on: which local role, if
    // any, does this connection belong to?
    //
    // Acting as a role at all requires proof.  A connection that produced no
    // attestation reached us without an enforced handshake, so there is
    // nothing to resolve.
    if (!attested.has_value())
        return {ClaimVerdict::no_attestation, {}};

    const PubkeyOrigin *origin = resolve_(*attested);
    if (origin == nullptr)
        return {ClaimVerdict::unknown_key, {}};

    // Kind before uid, deliberately.  A federation peer whose subject_uid
    // coincided with a role's would pass the uid comparison callers make on
    // top of this; only the kind test refuses it.
    if (origin->kind != PubkeyOrigin::Kind::LocalRole)
        return {ClaimVerdict::kind_not_permitted, {}};

    return {ClaimVerdict::accepted, origin->subject_uid};
}

ClaimVerdict PeerAuthority::check_role_ownership(const std::optional<AttestedKey> &attested,
                                                 std::string_view claimed_uid) const
{
    // Checking a claim is naming the connection and then comparing.  Every
    // refusal below the comparison — no proof, unknown key, a federation
    // peer acting as a role — is a refusal to NAME it, so it belongs to
    // `attribute_sender` and is answered there once.  Two copies of that
    // resolution would agree only on the day they were written, and the
    // kind-before-uid ordering is exactly the kind of detail one copy
    // eventually loses.
    const AttributedSender who = attribute_sender(attested);
    if (who.verdict != ClaimVerdict::accepted)
        return who.verdict;

    return claimed_uid == who.uid ? ClaimVerdict::accepted : ClaimVerdict::identity_mismatch;
}

ClaimVerdict PeerAuthority::check_registration_claim(const std::optional<AttestedKey> &attested,
                                                     std::string_view claimed_uid,
                                                     std::string_view announced_pubkey) const
{
    // Registration is ownership plus one extra obligation: the body
    // DECLARES a key, and that declaration must agree with what the
    // transport proved.  This is what makes the declaration a cross-check
    // rather than decoration — it can only ever deny.
    //
    // The attestation check is repeated ahead of the comparison because
    // reading `attested->key()` requires it; `check_role_ownership` then
    // re-tests it rather than taking a bare key, so neither entry point
    // depends on the other having already looked.
    if (!attested.has_value())
        return ClaimVerdict::no_attestation;

    if (announced_pubkey != attested->key().view())
        return ClaimVerdict::pubkey_mismatch;

    // Everything past the declaration is the same question every other
    // REG-family message asks: does this proven key own the claimed uid?
    // One rule, one implementation — two copies would agree only on the
    // day they were written.
    return check_role_ownership(attested, claimed_uid);
}

} // namespace pylabhub::utils::security

namespace pylabhub::utils::security
{

bool PeerAuthority::is_federation_peer(const AttestedKey &attested) const
{
    const PubkeyOrigin *origin = resolve_(attested);
    return origin != nullptr && origin->kind == PubkeyOrigin::Kind::FederationPeer;
}

PeerAllowlist PeerAuthority::zap_allowlist() const
{
    PeerAllowlist al;
    for (const auto &[key, origin] : by_pubkey_)
        al.peers.insert(PeerIdentity{kCurveMechanism, std::string{key.view()}});
    return al;
}

std::set<RosterEntry> PeerAuthority::local_role_roster() const
{
    // std::set: deterministic uid order from the container, per RosterEntry's
    // operator<.  The roster rides REG_ACK.
    std::set<RosterEntry> out;
    for (const auto &[key, origin] : by_pubkey_)
        if (origin.kind == PubkeyOrigin::Kind::LocalRole)
            out.insert(RosterEntry{origin.subject_uid, std::string{key.view()}});
    return out;
}

} // namespace pylabhub::utils::security
