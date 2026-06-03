#pragma once
/**
 * @file peer_admission.hpp
 * @brief Cross-transport admission contract — the gate.
 *
 * The pylabhub framework gates DATA-channel access by the validation of
 * CTRL-channel identity (see docs/archive/transient-2026-06-02/peer_admission_architecture_design.md
 * §2 threat model).  This header defines the policy surface — what an
 * allowlist looks like and how a transport answers "is this peer
 * admitted?" — without committing to any particular wire mechanism.
 *
 * The interface lives outside the Queue family on purpose.  Several
 * admission-bearing surfaces are NOT queues (broker ROUTER, admin REP,
 * federation peer ROUTER); coupling the gate to QueueReader/QueueWriter
 * would force them into the Queue hierarchy and would diamond every
 * QueueReader+QueueWriter concrete class (`ZmqQueue`, `ShmQueue`).  See
 * design doc §4.2 for the architectural rationale.
 *
 * Mechanism (CURVE+ZAP, POSIX uid+gid, broker-issued token, etc.) lives
 * in each concrete class.  The interface is mechanism-agnostic: a
 * `PeerIdentity` is an opaque (kind, data) tuple that the implementing
 * transport interprets in its native vocabulary.
 *
 * Thread-safety contract (the implementer's responsibility):
 *   - `set_peer_allowlist()` may be called from ANY thread.
 *   - `is_peer_allowed()` is called from the transport's enforcement
 *     context (e.g., the ZAP handler thread inside ZeroMQ libzmq).
 *   - Implementations MUST tolerate concurrent set + is_peer_allowed.
 *   - Recommended pattern: `pylabhub::utils::detail::PortableAtomicSharedPtr`
 *     for the queue's allowlist member; copy-on-write snapshot semantics.
 *
 * @see docs/archive/transient-2026-06-02/peer_admission_architecture_design.md (full design)
 * @see HEP-CORE-0035 (identity vault — provides the keys this gate uses)
 * @see HEP-CORE-0036 (data-plane auth — describes wire protocol; layering
 *                     in §3.3/§4.1/§6 is superseded by the design doc above)
 */

#include "pylabhub_utils_export.h"

#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace pylabhub::utils::security
{

/// Identifier of a peer presenting itself to an admission-gated surface.
/// Cross-transport: each transport extracts the kind it cares about.
///
/// Conventional `kind` values (string-based for forward compatibility —
/// new transports can introduce new kinds without amending this header):
///   - `"curve"`     — `data` is the peer's CURVE public key in Z85
///                     (40 ASCII chars; libsodium `crypto_box_PUBLICKEYBYTES`
///                     base85-encoded).  Used by ZmqQueue, InboxQueue,
///                     BrokerService ROUTER, AdminService (if CURVE-wrapped),
///                     federation peer ROUTER.
///   - `"posix"`     — `data` is `"<uid>:<gid>"` (decimal, colon-separated).
///                     Reserved for transports that gate by kernel-checked
///                     credentials (e.g., ShmQueue with UID guard option).
///   - `"shm"`       — `data` is hex-encoded broker-issued ephemeral
///                     shared-memory secret.  Used by ShmQueue (the
///                     broker controls who learns the secret via
///                     CONSUMER_REG_ACK).
///   - `"federation"`— Reserved for HEP-CORE-0037 (cross-hub peer
///                     identity).  Shape TBD.
///   - `"script"`    — Reserved for HEP-CORE-0038 (script-accessible
///                     vault keystore identities).
///
/// Comparison is byte-exact on the (kind, data) pair.  PeerIdentity is
/// meant for set-membership tests, not for rich identity-attribute
/// matching.  Two identities differ if either field differs by even
/// one byte.
struct PYLABHUB_UTILS_EXPORT PeerIdentity
{
    std::string kind;
    std::string data;

    /// Byte-exact equality on the (kind, data) pair.
    [[nodiscard]] bool
    operator==(const PeerIdentity &o) const noexcept
    {
        return kind == o.kind && data == o.data;
    }

    [[nodiscard]] bool
    operator!=(const PeerIdentity &o) const noexcept
    {
        return !(*this == o);
    }

    /// Lexicographic order on (kind, data) — required for std::set
    /// membership and for deterministic snapshot serialization.
    [[nodiscard]] bool
    operator<(const PeerIdentity &o) const noexcept
    {
        return std::tie(kind, data) < std::tie(o.kind, o.data);
    }
};

/// Snapshot of admitted peers.
///
/// **Snapshot semantics — NOT delta semantics.**  Passing this to
/// `PeerAdmission::set_peer_allowlist` replaces the implementer's
/// internal state in full.  The caller (typically the broker glue) is
/// responsible for computing unions when adding a peer to an existing
/// allowlist without dropping prior members; the interface itself does
/// not maintain history.  Snapshot semantics make recovery from a
/// missed update trivial (the next snapshot corrects state).
///
/// `unrestricted` is the explicit escape hatch for test fixtures and the
/// transitional `--allow-anonymous-data` operator flag (see design doc
/// §8 P-Default).  When true, `contains()` returns true for any
/// identity regardless of `peers`.  Production code paths MUST NOT set
/// `unrestricted = true`; an auditor scanning for this field can find
/// every place that bypasses the gate.
struct PYLABHUB_UTILS_EXPORT PeerAllowlist
{
    std::set<PeerIdentity> peers;
    bool                   unrestricted{false};

    /// True iff `p` is admitted by this allowlist.  Encapsulates the
    /// `unrestricted`-escape logic so concrete `PeerAdmission`
    /// implementers don't have to reimplement it.
    [[nodiscard]] bool
    contains(const PeerIdentity &p) const noexcept
    {
        if (unrestricted)
            return true;
        return peers.find(p) != peers.end();
    }

    /// Convenience: true iff the allowlist would reject every possible
    /// identity (i.e., empty + not unrestricted).  Useful for "gate
    /// is fully closed" diagnostics; production code typically uses
    /// `contains()` directly.
    [[nodiscard]] bool
    is_deny_all() const noexcept
    {
        return !unrestricted && peers.empty();
    }
};

/// Pure abstract interface — the gate.
///
/// **No defaults.** Implementers must provide all four methods.  This
/// is deliberate: silently inheriting a deny-default (or, worse, an
/// allow-default) from a base class would hide policy decisions inside
/// a default override.  Forcing every concrete class to declare its
/// admission stance makes the policy auditable by code review.
///
/// **Who implements this:**
///   - `ZmqQueue` (CURVE+ZAP allowlist on PUSH bind side; client-mode
///     no-allowlist on PULL connect side)
///   - `ShmQueue` (broker-issued ephemeral secret + optional UID guard)
///   - `InboxQueue` (CURVE+ZAP)
///   - `BrokerServiceImpl` (CTRL channel ROUTER — KnownRoleAllowlist on
///     role pubkey)
///   - `AdminServiceImpl` (if CURVE-wrapped per design §8 P-Admin
///     option a; loopback enforcement otherwise has no PeerAdmission
///     because the kernel gates the bind)
///   - Federation peer ROUTER (KnownPeerAllowlist on peer pubkey)
///
/// **Who calls `is_peer_allowed`:** the transport-side enforcement
/// layer.  For CURVE transports, the ZAP handler thread.  For SHM, the
/// segment attach path.  Application code does NOT call this directly —
/// it's the kernel-equivalent gate of the transport.
class PYLABHUB_UTILS_EXPORT PeerAdmission
{
public:
    virtual ~PeerAdmission() = default;

    /// Replace the allowlist (snapshot semantics).  Returns true on
    /// accept; false iff the implementer rejected the update — e.g.,
    /// the transport doesn't support dynamic admission, or this
    /// instance is in a transitional mode that ignores updates.
    ///
    /// May be called from any thread.  Implementations MUST be
    /// concurrent-safe with `is_peer_allowed`.
    virtual bool set_peer_allowlist(PeerAllowlist allowlist) = 0;

    /// Return the current allowlist, or std::nullopt if this instance
    /// doesn't expose its state (e.g., a write-only implementation or
    /// a fixture).  The returned snapshot is a value copy and stable
    /// for the caller's use.
    [[nodiscard]] virtual std::optional<PeerAllowlist>
    peer_allowlist_snapshot() const = 0;

    /// Test admission for a candidate identity.  Called by the
    /// transport's enforcement layer (ZAP handler, SHM attach path,
    /// etc.) at handshake time.
    ///
    /// Thread context: NOT the caller's thread — the implementer's
    /// enforcement context.  Must be safe with concurrent
    /// `set_peer_allowlist`.
    ///
    /// @return true iff @p peer is currently admitted.
    [[nodiscard]] virtual bool
    is_peer_allowed(const PeerIdentity &peer) const = 0;

    /// True iff this instance's transport currently enforces admission
    /// at the kernel/wire level.  False iff the instance is in a
    /// transitional mode (e.g., `--allow-anonymous-data`) where the
    /// gate is declared but not enforced.
    ///
    /// Used by the broker to decide whether to publish a producer's
    /// endpoint to consumers: if `admission_is_enforced() == false`
    /// AND the operator did not set the transitional flag, the
    /// broker rejects the registration rather than expose an
    /// ungated endpoint.  Also used by L4 tests to confirm the gate
    /// is wire-enforced, not merely policy-declared.
    [[nodiscard]] virtual bool
    admission_is_enforced() const noexcept = 0;
};

} // namespace pylabhub::utils::security
