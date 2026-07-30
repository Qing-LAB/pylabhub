#pragma once
/**
 * @file pubkey_origin.hpp
 * @brief The pubkey origin index — the single structure that answers
 *        "what does this CURVE key mean to this hub."
 *
 * **The problem this solves.**  A CURVE handshake proves which key is on
 * the other end of a connection.  That is the only trustworthy statement
 * of identity the hub ever gets: everything else a caller sends — the
 * role uid in a request body, the routing id it chose at connect, the
 * address it dialed from — is a value the caller picked for itself.  For
 * the proven key to be usable, something has to turn it into a subject
 * the hub knows.  That is this index.
 *
 * Given a key, `resolve()` answers with the subject it belongs to and
 * whether that subject is a local role or a federation peer hub.  Every
 * layer above the socket asks this one question of this one structure:
 * the admission gates when deciding whether a request may register as
 * the role it names, the inbox when attributing a message to a sender,
 * and federation when deciding whether a link may speak for identities
 * other than its own.
 *
 * **Why one structure and not several.**  The same operator-managed data
 * was previously projected in four places — a ZAP allowlist built by
 * iterating the config, a second allowlist helper beside it, a linear
 * scan per registration, and a roster assembled for REG_ACK.  Four
 * copies of one mapping drift, and a drifted identity mapping is a
 * security defect rather than an inconsistency.  This index is the one
 * copy; `as_peer_allowlist()` is how the ZAP layer gets its view of it.
 *
 * See HEP-CORE-0035 §4.2 (this index, and its relation to the
 * per-channel `ChannelAccessIndex`, which answers a different question)
 * and §2 for the invariants it exists to enforce.
 */
#include "pylabhub_utils_export.h"
#include "utils/security/attested_key.hpp"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/peer_admission.hpp"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pylabhub::broker
{
struct KnownRole;
}

namespace pylabhub::utils::security
{

/// What a CURVE public key means to this hub — the subject it belongs
/// to, and which kind of subject that is.
///
/// The `kind` distinction is load-bearing, not descriptive.  A
/// `LocalRole` may act only as itself: the identity it claims must be
/// its own `subject_uid`.  A `FederationPeer` is the one subject that
/// may legitimately carry identities other than its own, because it
/// relays messages authored by roles on its own side — and it may do so
/// only under a declared trust mode (HEP-CORE-0035 §4.3).  Collapsing
/// the two would either forbid federation or silently permit
/// impersonation.
struct PYLABHUB_UTILS_EXPORT PubkeyOrigin
{
    enum class Kind
    {
        LocalRole,     ///< A role registered in this hub's known-roles list.
        FederationPeer ///< A peer hub this operator has federated with.
    };

    Kind kind{Kind::LocalRole};
    std::string subject_uid;  ///< Role uid, or peer hub uid.
};

/// Key → subject. Built once from the operator's configured roles and
/// peers; read by the ZAP handler and by every identity-consuming layer
/// above it.
///
/// **Uniqueness is enforced across both categories.**  Two subjects
/// sharing one key cannot be resolved to a single identity, so the build
/// refuses it rather than picking a winner.  The known-roles store
/// already rejects a key shared between two roles; this index also
/// catches the case that store cannot see — a role and a federation peer
/// configured with the same key — because it is the first place both
/// categories meet.
/// The outcome of checking a registration claim against what the transport
/// attested.  One value per distinct reason, because an operator reading a
/// rejection needs to know WHICH of these happened — "denied" alone cannot
/// distinguish a misconfigured role from an impersonation attempt.
enum class ClaimVerdict
{
    accepted,          ///< the claim belongs to this connection
    no_attestation,    ///< nothing was attested; this plane requires one
    unknown_key,       ///< attested, but this hub has no record of the key
    identity_mismatch, ///< claimed uid is not this principal's subject
    pubkey_mismatch,   ///< announced key differs from the attested key
    kind_not_permitted ///< a federation peer may not register as a role
};

/// Human-readable name, for logs and reject details.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string_view to_string(ClaimVerdict v) noexcept;

/// One local role as the roster carries it: the uid AND the key.
///
/// A bare key is not enough for the receiving side. A role that gets only
/// keys can see that a message came from `BBBB` and has no way to learn
/// that `BBBB` is alice — so it cannot name the sender to the application,
/// cannot keep per-sender sequence state, and cannot key replay tracking.
/// That is the defect the inbox plane has today (HEP-CORE-0027 §3.6), and
/// it is why this projection carries pairs even though the wire has not
/// migrated yet: nothing new should be written against the shape that is
/// already known to be wrong.
struct PYLABHUB_UTILS_EXPORT RosterEntry
{
    std::string uid;
    std::string pubkey_z85;

    [[nodiscard]] bool operator<(const RosterEntry &o) const noexcept { return uid < o.uid; }
};

/// Key → subject, and the questions this hub can answer about a peer.
///
/// **Immutable by construction, not by convention.**  There are no mutating
/// members: an instance is produced complete by `Builder` and can only be
/// asked questions thereafter.  An earlier revision put `add_local_role` /
/// `add_federation_peer` directly on this class and relied on publishing it
/// as `shared_ptr<const>` to stop later edits — a rule enforced by a
/// keyword on the handle rather than by the type. Anyone holding a non-const
/// reference could edit a live authority.
///
/// The roster it mirrors DOES change during hub lifetime (roles are added
/// and revoked), and the CTRL allowlist it projects into is swapped
/// atomically on reload (HEP-CORE-0035 §4.8.5). Both are satisfied by
/// replacement rather than mutation: build a fresh authority, publish it as
/// `std::shared_ptr<const PeerAuthority>`, swap the pointer. Readers hold a
/// snapshot that cannot change under them; old readers drain naturally.
///
/// **Questions return answers, not subjects.**  There is deliberately no
/// `resolve()` handing back the internal record. Every authorization
/// question returns a `ClaimVerdict`, so there are no per-call-site
/// comparisons to get subtly wrong — a forgotten kind test, a laxer string
/// rule, a missing absent-case — and adding a plane cannot silently skip a
/// check it does not perform. The one exception is `local_role_uid`, which
/// exists because the inbox genuinely needs a NAME (see `RosterEntry`); it
/// returns the uid alone, by value, and only for a local role.
class PYLABHUB_UTILS_EXPORT PeerAuthority
{
  public:
    /// Fills in an authority, then hands over a finished one.
    ///
    /// Nested rather than a separate class with `friend` access: a nested
    /// class is a member of its enclosing class and can reach its private
    /// constructor without any encapsulation hole.
    class PYLABHUB_UTILS_EXPORT Builder
    {
      public:
        /// @throws std::runtime_error if the key is already registered to a
        ///         different subject, or is not a 40-char Z85 key.
        void add_local_role(const ::pylabhub::broker::KnownRole &role);

        /// @throws std::runtime_error under the same conditions.
        void add_federation_peer(std::string_view peer_uid, std::string_view pubkey_z85);

        /// Hand over the finished authority.  Consumes the builder, so a
        /// half-built authority cannot be published and the builder cannot
        /// keep editing what it already handed out.
        [[nodiscard]] PeerAuthority build() &&;

      private:
        void insert_(std::string_view pubkey_z85, PubkeyOrigin origin);
        std::unordered_map<Z85PublicKey, PubkeyOrigin> by_pubkey_;
    };

    /// Decide whether a registration claim belongs to this connection.
    ///
    /// @param attested         what the transport vouched for, if anything.
    /// @param claimed_uid      the role_uid in the request body.
    /// @param announced_pubkey the zmq_pubkey in the request body.  Nothing
    ///                         trusts it; it is a declaration that must be
    ///                         consistent, and every check over it denies.
    ///
    /// The federation rule lives here rather than at the call site: a
    /// `FederationPeer` may legitimately carry identities other than its own,
    /// but only under the delegation modes of HEP-CORE-0035 §4.3, which are
    /// NOT built. Until they are, such a principal is refused on the
    /// registration plane rather than silently permitted — leaving the most
    /// privileged principal class in front of an unimplemented policy is how
    /// holes ship. Note this tests the KIND, not just the uid string: a peer
    /// whose subject_uid happened to match would otherwise slip through a
    /// string comparison.
    [[nodiscard]] ClaimVerdict check_registration_claim(
        const std::optional<AttestedKey> &attested, std::string_view claimed_uid,
        std::string_view announced_pubkey) const;

    /// The uid of the local role this key belongs to, for message
    /// ATTRIBUTION (inbox sender, replay key, per-sender sequence state —
    /// HEP-CORE-0035 §4.2.2).  `nullopt` for an unknown key or a federation
    /// peer.
    ///
    /// The only query that returns an identity rather than a decision,
    /// because attribution genuinely needs a name. It returns the uid by
    /// value and nothing else — not the kind, not a pointer into the table.
    [[nodiscard]] std::optional<std::string> local_role_uid(const AttestedKey &attested) const;

    /// Is this key a federation peer hub rather than a local role?
    ///
    /// The whole of this structure's share of the federation decision. The
    /// rest — trust mode, and the peer's delegated role list from
    /// HUB_PEER_HELLO (§4.3/§4.4) — is peer state that does not live here.
    [[nodiscard]] bool is_federation_peer(const AttestedKey &attested) const;

    /// The ZAP layer's view: every known key as a `{curve, key}` identity.
    ///
    /// The ONLY sanctioned way to build a control-plane allowlist.
    /// `unrestricted` is always false — an empty authority is deny-all,
    /// which is the correct bootstrap state for a hub with no configured
    /// roles (HEP-CORE-0035 §4.8.4), not an invitation to admit everyone.
    [[nodiscard]] PeerAllowlist zap_allowlist() const;

    /// Local roles only, as `{uid, key}` pairs in uid order.
    ///
    /// Federation peer keys are excluded: they authorize a different plane
    /// (HEP-CORE-0027 §3.5). Deterministic order because this rides REG_ACK
    /// and an order that reshuffles per process makes wire captures and test
    /// pins unstable for no reason.
    [[nodiscard]] std::set<RosterEntry> local_role_roster() const;

    [[nodiscard]] std::size_t size() const noexcept { return by_pubkey_.size(); }
    [[nodiscard]] bool empty() const noexcept { return by_pubkey_.empty(); }

  private:
    explicit PeerAuthority(std::unordered_map<Z85PublicKey, PubkeyOrigin> by_pubkey) noexcept
        : by_pubkey_(std::move(by_pubkey))
    {
    }

    /// Internal. Deliberately not exposed — callers get verdicts.
    [[nodiscard]] const PubkeyOrigin *resolve_(const AttestedKey &attested) const;

    /// The ONLY stored state.  Keyed on the validated key type, so a lookup
    /// cannot be performed with — nor an entry stored from — an unvalidated
    /// string.  Everything else this class exposes is a view over this map,
    /// computed on demand: these are control-plane operations (a handful per
    /// second at most, and per-registration for the roster), so there is
    /// nothing here worth trading memory or a second container for.
    std::unordered_map<Z85PublicKey, PubkeyOrigin> by_pubkey_;
};

} // namespace pylabhub::utils::security
