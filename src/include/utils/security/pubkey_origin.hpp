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
#include "utils/security/peer_admission.hpp"

#include <cstddef>
#include <optional>
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
    std::string subject_name; ///< Human-readable label, diagnostics only.
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

/// **Publication contract — build, publish as const, replace wholesale.**
///
/// This index is read concurrently from message paths and carries no lock,
/// so a published instance must never mutate.  But the operator roster it
/// mirrors DOES change during hub lifetime — roles are added and revoked,
/// and the CTRL allowlist this index projects into is already a
/// `PortableAtomicSharedPtr<PeerAllowlist>` swapped atomically on reload
/// (HEP-CORE-0035 §4.8.5).  An index that could not be replaced would drift
/// out of step with the allowlist it feeds: a revoked role still resolving
/// to a valid principal while ZAP had begun denying it.  Divergence between
/// those two views is precisely what collapsing five projections into one
/// index exists to prevent.
///
/// So immutability and updatability are BOTH required, and the resolution is
/// the pattern already used for the allowlist: build a fresh index, publish
/// it as `std::shared_ptr<const PubkeyOriginIndex>`, and swap the pointer.
/// Readers take a snapshot that cannot change under them; a reload installs
/// a new snapshot and old readers drain naturally.
///
/// Immutability after publication is therefore enforced by `const`, at
/// compile time — a reader holding `shared_ptr<const>` cannot reach `add_*`
/// at all.  An earlier revision used a `finalize()` flag that threw on later
/// mutation; that was worse in both directions, since it checked at runtime
/// what `const` checks at compile time AND it forbade the reload the
/// surrounding design requires.
///
/// **Never mutate an index that has been published.  Build a new one.**
class PYLABHUB_UTILS_EXPORT PubkeyOriginIndex
{
  public:
    /// Register a local role's key.
    /// @throws std::runtime_error if the key is already registered to a
    ///         different subject, or is not a 40-char Z85 key.
    /// Build-time only — call before publishing as `shared_ptr<const>`.
    /// See the publication contract above.
    void add_local_role(const ::pylabhub::broker::KnownRole &role);

    /// Register a federation peer hub's key.
    /// @throws std::runtime_error under the same conditions as
    ///         `add_local_role`.
    void add_federation_peer(std::string_view peer_uid, std::string_view pubkey_z85,
                             std::string_view display_name = {});

    /// Resolve an ATTESTED key to its subject.
    ///
    /// Takes an `AttestedKey` and not a string, deliberately.  A caller
    /// holding a key that merely arrived in a request body has nothing to
    /// pass here, so laundering a claim into an identity fails to COMPILE
    /// rather than silently succeeding.  That is the entire point of the
    /// surrounding mechanism, and a `string_view` overload would hand it
    /// straight back — do not add one.
    ///
    /// `std::nullopt` means the hub has no record of this key.  With
    /// Layer-1 ZAP enforcing, that is unreachable on an established
    /// connection — an unlisted key never completes a handshake — so a
    /// caller seeing `nullopt` is looking at either a configuration
    /// change mid-flight or a gate that is not doing its job.  Treat it
    /// as a rejection AND as something worth logging loudly.
    [[nodiscard]] std::optional<PubkeyOrigin> resolve(const AttestedKey &attested) const;

    /// Decide whether a registration claim belongs to this connection.
    ///
    /// **Gates call this; they do not call `resolve()` and compare for
    /// themselves.**  Handing a caller the principal so it can do its own
    /// comparison invites every call site to compare slightly differently —
    /// a forgotten kind test, a laxer string rule, a missing absent-case.
    /// With the comparison in here there are no per-site comparisons left
    /// to get wrong, and adding a plane cannot silently skip a check it
    /// does not perform.
    ///
    /// The federation rule lives here for exactly that reason: a
    /// `FederationPeer` may legitimately carry identities other than its
    /// own, but only under the delegation modes of HEP-CORE-0035 §4.3,
    /// which are NOT built.  Until they are, such a principal is refused on
    /// the registration plane rather than silently permitted — leaving the
    /// most privileged principal class in front of an unimplemented policy
    /// is how holes ship.  Note this tests the KIND, not just the uid
    /// string: a peer whose subject_uid happened to match would otherwise
    /// slip through a string comparison.
    ///
    /// @param attested         what the transport vouched for, if anything.
    /// @param claimed_uid      the role_uid in the request body.
    /// @param announced_pubkey the zmq_pubkey in the request body.  Nothing
    ///                         trusts it; it is a declaration that must be
    ///                         consistent, and every check over it denies.
    [[nodiscard]] ClaimVerdict check_registration_claim(
        const std::optional<AttestedKey> &attested, std::string_view claimed_uid,
        std::string_view announced_pubkey) const;

    /// The ZAP layer's view of this index: every known key as a
    /// `{"curve", key}` identity.
    ///
    /// This is the ONLY sanctioned way to build a control-plane
    /// allowlist.  `unrestricted` is always false — an empty index is
    /// deny-all, which is the correct bootstrap state for a hub with no
    /// configured roles (HEP-CORE-0035 §4.8.4), not an invitation to
    /// admit everyone.
    [[nodiscard]] PeerAllowlist as_peer_allowlist() const;

    /// Keys of kind `LocalRole` only, as a Z85 list.
    ///
    /// This is the inbox roster: role-to-role messaging authorizes any
    /// authenticated local role, but must NOT hand out federation peer
    /// keys, which authorize a different plane (HEP-CORE-0027 §3.5).
    /// Having one index expose both views keeps that distinction in one
    /// place instead of relying on each call site to filter correctly.
    [[nodiscard]] std::vector<std::string> local_role_pubkeys() const;

    [[nodiscard]] std::size_t size() const noexcept { return by_pubkey_.size(); }
    [[nodiscard]] bool empty() const noexcept { return by_pubkey_.empty(); }

  private:
    void insert_(std::string pubkey_z85, PubkeyOrigin origin);

    /// Transparent comparator so `resolve()` — which runs on the inbound
    /// message path — looks up from a `string_view` without materialising a
    /// `std::string` per call.  `is_transparent` is what opts
    /// `unordered_map` into the heterogeneous overloads (C++20).
    struct KeyHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };

    std::unordered_map<std::string, PubkeyOrigin, KeyHash, std::equal_to<>> by_pubkey_;

};

} // namespace pylabhub::utils::security
