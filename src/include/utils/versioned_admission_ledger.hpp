#pragma once
/**
 * @file versioned_admission_ledger.hpp
 * @brief Per-channel ledger of admission versions + per-role confirmation.
 *
 * The primitive that fan-in, fan-out, and one-to-one topologies all
 * reduce to: **admissions carry versions; roles carry confirmed-
 * versions; a peer is visible to a role iff its admission-version is
 * <= that role's confirmed-version**.  See HEP-CORE-0042 §5.5.2 for
 * the normative rules (INVARIANT-BIND-CONFIRM-1..3).
 *
 * Replaces the pre-2026-07-13 scattered mix of
 * `authorized_consumer_pubkeys` (set), `channel_version` (uint64_t),
 * `confirmed_version_per_producer` (map), and
 * `binding_side_confirmed_allowlist` (set) on
 * `pylabhub::hub::ChannelAccessEntry`.  The old set-snapshot mechanism
 * (`_on_binding_confirmed` copying current `authorized_consumer_pubkeys`
 * into the confirmed set) was race-prone: broker could over-confirm
 * pubkeys admitted after the consumer's APPLIED_REQ was sent but
 * before broker processed it (test #2480 evidence, 2026-07-12).
 *
 * ## Concurrency
 *
 * Non-thread-safe by design.  The caller (HubState) owns the mutex
 * that serializes access.  Keeping locking out of the primitive lets
 * HubState take one lock for a compound operation
 * (admit-then-confirm-then-query) instead of one per call.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pylabhub::hub
{

/// Versioned admission ledger.  Two parameterized types:
///   - `PubkeyT`  — the admitted peer's identity (std::string z85 in
///                  production; int / std::string used in unit tests).
///   - `RoleUidT` — the confirming role's identity (std::string uid in
///                  production).
///
/// Both types must be hashable + equality-comparable + copyable.
///
/// Version monotonicity: `current_version_` starts at 0 and is bumped
/// on every state-mutating operation (`admit`, `revoke`).  A version
/// of 0 means "nothing has ever been admitted."  `confirm(V=0)` is a
/// legal but zero-effect no-op (max(0, 0) == 0); useful only for
/// completeness of the wire round-trip.
template <typename PubkeyT, typename RoleUidT> class VersionedAdmissionLedger
{
  public:
    /// Admit `pk`.  Returns the version assigned.
    ///
    /// Idempotent: re-admitting an already-admitted pubkey is a no-op
    /// that returns the pubkey's ORIGINAL admission version.  Does NOT
    /// bump `current_version_` on the idempotent path — the ledger's
    /// state truly did not change, and bumping would falsely invalidate
    /// role confirmations.
    std::uint64_t admit(const PubkeyT &pk)
    {
        auto it = admission_version_.find(pk);
        if (it != admission_version_.end())
        {
            return it->second;
        }
        ++current_version_;
        admission_version_[pk] = current_version_;
        return current_version_;
    }

    /// Revoke `pk`.  Returns the `current_version_` after the call.
    ///
    /// Bumps `current_version_` iff `pk` was actually admitted (i.e.,
    /// this call caused a real state change).  A revoke of a never-
    /// admitted pubkey is a full no-op — no bump, no side effects on
    /// role confirmations.  Symmetric with `admit`'s idempotent-no-
    /// bump contract: only real mutations advance the version.
    ///
    /// Revoked-then-re-admitted safety is preserved by `admit`'s
    /// post-erase behavior: the re-admitted pubkey gets a fresh
    /// (higher) admission version, so a role confirmed at the pre-
    /// revoke version sees the re-admission as NOT visible until it
    /// re-confirms — see `Revoke_ThenReAdmit_AssignsNewHigherVersion`
    /// L1 test for the exact sequence.
    std::uint64_t revoke(const PubkeyT &pk)
    {
        auto erased = admission_version_.erase(pk);
        if (erased > 0)
        {
            ++current_version_;
        }
        return current_version_;
    }

    /// Advance confirmed_version for `role_uid` to
    /// `max(current_confirmed, min(applied_version, current_version_))`.
    /// Called from `CHANNEL_AUTH_APPLIED_REQ` handler.  Returns the
    /// confirmed version after the advance.
    ///
    /// **Two guards, both load-bearing (HEP-CORE-0042 §5.5.2
    /// INVARIANT-BIND-CONFIRM-1):**
    ///
    /// 1. **Monotonic** — a stale (older) applied_version is silently
    ///    absorbed as a no-op; the stored value never regresses.
    ///
    /// 2. **Bounded** — applied_version is CLAMPED at
    ///    `current_version_` before storage.  A role cannot confirm at
    ///    a version the ledger has not yet issued.  Without this,
    ///    a role that fabricates `applied_version=UINT64_MAX` (buggy
    ///    or adversarial) would make EVERY subsequent `admit`
    ///    immediately visible (`admission_version(new_pk)=N <=
    ///    confirmed=UINT64_MAX` always holds).  The clamp is at the
    ///    primitive layer — not at the handler — so no caller can
    ///    bypass it.  This is the architectural defense against
    ///    the class of "over-confirmation" bugs the whole ledger
    ///    was built to eliminate.
    std::uint64_t confirm(const RoleUidT &role_uid, std::uint64_t applied_version)
    {
        const std::uint64_t bounded =
            (applied_version < current_version_) ? applied_version : current_version_;
        auto &slot = confirmed_version_[role_uid];
        if (bounded > slot)
        {
            slot = bounded;
        }
        return slot;
    }

    /// The core visibility query.
    ///
    /// Returns:
    ///   - `std::nullopt` — `role_uid` has never confirmed anything.
    ///                      Callers distinguish "role has not installed
    ///                      anything yet" from "role installed but pk
    ///                      not admitted."  Wire callers surface this
    ///                      as `not_ready`, `reason="not_confirmed"`.
    ///   - `false`        — `pk` is not currently admitted, OR its
    ///                      admission_version > confirmed_version.
    ///                      Wire: `not_ready`, `reason="not_admitted"`.
    ///   - `true`         — `pk` currently admitted AND
    ///                      admission_version <= confirmed_version.
    ///                      Wire: `ready`.
    ///
    /// Correctness contract (HEP-CORE-0042 §5.5.2 INVARIANT-BIND-CONFIRM-2):
    /// the confirmed_version is set exclusively by `confirm()` from wire
    /// evidence.  This ledger MUST NOT infer confirmation from
    /// `current_version_` or from the presence of `pk` in the
    /// admission map — those would reintroduce the pre-2026-07-13
    /// set-snapshot bug.
    std::optional<bool> is_visible_to(const RoleUidT &role_uid, const PubkeyT &pk) const
    {
        auto conf_it = confirmed_version_.find(role_uid);
        if (conf_it == confirmed_version_.end())
        {
            return std::nullopt;
        }
        auto adm_it = admission_version_.find(pk);
        if (adm_it == admission_version_.end())
        {
            return std::optional<bool>{false};
        }
        return std::optional<bool>{adm_it->second <= conf_it->second};
    }

    // ── Observability accessors ──────────────────────────────────────

    std::uint64_t current_version() const noexcept { return current_version_; }

    std::optional<std::uint64_t> admission_version_of(const PubkeyT &pk) const
    {
        auto it = admission_version_.find(pk);
        if (it == admission_version_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::uint64_t> confirmed_version_of(const RoleUidT &role_uid) const
    {
        auto it = confirmed_version_.find(role_uid);
        if (it == confirmed_version_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    /// Maximum confirmed_version across ALL roles that have ever
    /// confirmed against this ledger.  Returns 0 if no role has
    /// confirmed anything yet.  Used by admin / observability
    /// serializers to summarize "how caught up is the channel" with
    /// a single scalar (HEP-CORE-0033 §8 admin surface).  Not used
    /// for correctness decisions — those go through
    /// `is_visible_to(role_uid, pk)` which is per-role.
    std::uint64_t max_confirmed_version() const noexcept
    {
        std::uint64_t max_v = 0;
        for (const auto &kv : confirmed_version_)
        {
            if (kv.second > max_v)
            {
                max_v = kv.second;
            }
        }
        return max_v;
    }

    std::size_t admitted_count() const noexcept { return admission_version_.size(); }

    /// Snapshot of currently-admitted pubkeys.  Order is unspecified
    /// (unordered_map iteration order); callers that need stable order
    /// (e.g., wire serialization) sort at the boundary.
    ///
    /// Allocates a fresh vector every call.  Prefer `for_each_admitted`
    /// on hot paths — that visitor iterates in-place without allocation.
    std::vector<PubkeyT> admitted_snapshot() const
    {
        std::vector<PubkeyT> out;
        out.reserve(admission_version_.size());
        for (const auto &kv : admission_version_)
        {
            out.push_back(kv.first);
        }
        return out;
    }

    /// Zero-allocation visitor over currently-admitted pubkeys.
    /// `visitor` is invoked as `visitor(pubkey)` for each admitted
    /// pubkey exactly once.  Iteration order is unspecified (matches
    /// `admitted_snapshot`).  The visitor MUST NOT mutate the ledger
    /// (no re-entrant admit/revoke/confirm) — the caller holds the
    /// HubState mutex; nested calls would deadlock or corrupt
    /// iteration.  Prefer this over `admitted_snapshot` on hot wire
    /// paths (REG_ACK initial_allowlist, GET_CHANNEL_AUTH_ACK
    /// allowlist).
    template <typename Fn> void for_each_admitted(Fn &&visitor) const
    {
        for (const auto &kv : admission_version_)
        {
            visitor(kv.first);
        }
    }

    /// Reset only a single role's confirmation, keeping admissions.
    /// Used by HEP-CORE-0042 §5.4 producer re-registration and
    /// producer-drop non-last-producer paths (`_on_producer_added`,
    /// `_on_producer_dropped` non-last, HeartbeatTimeout non-last).
    /// Returns true iff the role had a prior confirmation.
    bool reset_role_confirmation(const RoleUidT &role_uid) noexcept
    {
        return confirmed_version_.erase(role_uid) > 0;
    }

  private:
    std::uint64_t current_version_ = 0;
    std::unordered_map<PubkeyT, std::uint64_t> admission_version_;
    std::unordered_map<RoleUidT, std::uint64_t> confirmed_version_;
};

} // namespace pylabhub::hub
