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

void PubkeyOriginIndex::insert_(std::string pubkey_z85, PubkeyOrigin origin)
{
    if (pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::runtime_error("pubkey origin index: " + std::string(kind_label(origin.kind)) +
                                 " '" + origin.subject_uid + "' has a CURVE key of " +
                                 std::to_string(pubkey_z85.size()) + " characters, expected " +
                                 std::to_string(kZ85PubkeyChars) +
                                 " (Z85-encoded CURVE25519 public key)");
    }

    const auto it = by_pubkey_.find(pubkey_z85);
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

    by_pubkey_.emplace(std::move(pubkey_z85), std::move(origin));
}

void PubkeyOriginIndex::add_local_role(const ::pylabhub::broker::KnownRole &role)
{
    insert_(role.pubkey_z85,
            PubkeyOrigin{PubkeyOrigin::Kind::LocalRole, role.uid, role.name});
}

void PubkeyOriginIndex::add_federation_peer(std::string_view peer_uid,
                                            std::string_view pubkey_z85,
                                            std::string_view display_name)
{
    insert_(std::string(pubkey_z85), PubkeyOrigin{PubkeyOrigin::Kind::FederationPeer,
                                                  std::string(peer_uid),
                                                  std::string(display_name)});
}

std::optional<PubkeyOrigin> PubkeyOriginIndex::resolve(std::string_view pubkey_z85) const
{
    const auto it = by_pubkey_.find(std::string(pubkey_z85));
    if (it == by_pubkey_.end())
        return std::nullopt;
    return it->second;
}

PeerAllowlist PubkeyOriginIndex::as_peer_allowlist() const
{
    PeerAllowlist al;
    for (const auto &[pubkey, origin] : by_pubkey_)
        al.peers.insert(PeerIdentity{"curve", pubkey});
    return al;
}

std::vector<std::string> PubkeyOriginIndex::local_role_pubkeys() const
{
    std::vector<std::string> out;
    out.reserve(by_pubkey_.size());
    for (const auto &[pubkey, origin] : by_pubkey_)
    {
        if (origin.kind == PubkeyOrigin::Kind::LocalRole)
            out.push_back(pubkey);
    }
    // Deterministic order — the roster rides REG_ACK, and a set that
    // reshuffles per process makes wire captures and test pins unstable
    // for no reason.
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace pylabhub::utils::security
