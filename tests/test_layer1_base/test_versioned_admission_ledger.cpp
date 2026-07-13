/**
 * @file test_versioned_admission_ledger.cpp
 * @brief Layer 1 tests for versioned_admission_ledger.hpp — the primitive
 *        that fan-in/fan-out/one-to-one topologies all reduce to.
 *
 * Pins the HEP-CORE-0042 §5.5.2 INVARIANT-BIND-CONFIRM-1..3 rules at the
 * primitive level, independent of any wire, topology, or HubState wiring.
 *
 * Test-#2480-race regression pins:
 *   - `admit(B)` after a role has `confirm(V=1)`ed at pre-B version:
 *     `is_visible_to(role, B)` returns false (was: true, bug).
 *   - `confirm` MUST use its arg, never infer from `current_version_`.
 *
 * Uses `int` pubkey / `int` role types for compactness — the primitive is
 * type-agnostic; the concrete `<std::string, std::string>` specialization
 * is tested at L2 (HubState).
 */
#include "utils/versioned_admission_ledger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>

using pylabhub::hub::VersionedAdmissionLedger;
using IntLedger    = VersionedAdmissionLedger<int, int>;
using StringLedger = VersionedAdmissionLedger<std::string, std::string>;

// ── Fresh-state invariants ──────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, FreshState_HasZeroVersionAndNoAdmissions)
{
    IntLedger l;
    EXPECT_EQ(l.current_version(), 0u);
    EXPECT_EQ(l.admitted_count(), 0u);
    EXPECT_FALSE(l.admission_version_of(42).has_value());
    EXPECT_FALSE(l.confirmed_version_of(7).has_value());
}

TEST(VersionedAdmissionLedger, FreshState_IsVisibleTo_ReturnsNulloptForUnconfirmedRole)
{
    IntLedger l;
    l.admit(42);  // pubkey admitted at version 1
    // But role 7 has never confirmed anything → nullopt (distinguishable
    // from "not admitted" per wire semantics: not_confirmed vs not_admitted).
    auto v = l.is_visible_to(/*role=*/7, /*pk=*/42);
    EXPECT_FALSE(v.has_value());
}

// ── admit() semantics ───────────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, Admit_AssignsMonotonicVersions)
{
    IntLedger l;
    EXPECT_EQ(l.admit(10), 1u);
    EXPECT_EQ(l.admit(20), 2u);
    EXPECT_EQ(l.admit(30), 3u);
    EXPECT_EQ(l.current_version(), 3u);
    EXPECT_EQ(l.admission_version_of(10).value(), 1u);
    EXPECT_EQ(l.admission_version_of(20).value(), 2u);
    EXPECT_EQ(l.admission_version_of(30).value(), 3u);
}

TEST(VersionedAdmissionLedger, Admit_IsIdempotent_NoBump_ReturnsOriginalVersion)
{
    IntLedger l;
    EXPECT_EQ(l.admit(10), 1u);
    EXPECT_EQ(l.admit(20), 2u);
    // Re-admitting 10 must NOT bump current_version_ (which would falsely
    // invalidate role confirmations at version 2).  MUST return the
    // ORIGINAL admission version (1), not the current version.
    EXPECT_EQ(l.admit(10), 1u);
    EXPECT_EQ(l.current_version(), 2u);
    EXPECT_EQ(l.admission_version_of(10).value(), 1u);
}

TEST(VersionedAdmissionLedger, Admit_AdmittedCountReflectsUniquePubkeys)
{
    IntLedger l;
    l.admit(10);
    l.admit(20);
    l.admit(10);  // idempotent — still count 2
    l.admit(30);
    EXPECT_EQ(l.admitted_count(), 3u);
}

// ── revoke() semantics ──────────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, Revoke_BumpsVersionAndRemovesPubkey)
{
    IntLedger l;
    l.admit(10);            // v1
    l.admit(20);            // v2
    EXPECT_EQ(l.revoke(10), 3u);
    EXPECT_EQ(l.current_version(), 3u);
    EXPECT_FALSE(l.admission_version_of(10).has_value());
    EXPECT_EQ(l.admission_version_of(20).value(), 2u);
    EXPECT_EQ(l.admitted_count(), 1u);
}

TEST(VersionedAdmissionLedger, Revoke_MissingPubkey_IsNoOp_NoBump)
{
    IntLedger l;
    l.admit(10);  // v1
    // Revoking a never-admitted pubkey is a full no-op — no bump, no
    // side effect on role confirmations.  Symmetric with `admit`'s
    // idempotent-no-bump contract: only real mutations advance the
    // version.  A false bump here would force spurious re-confirmation
    // traffic from every role that had already caught up.
    EXPECT_EQ(l.revoke(999), 1u);
    EXPECT_EQ(l.current_version(), 1u);
    // Original admission untouched.
    EXPECT_EQ(l.admission_version_of(10).value(), 1u);
}

TEST(VersionedAdmissionLedger, Revoke_ThenReAdmit_AssignsNewHigherVersion)
{
    IntLedger l;
    l.admit(10);  // v1
    l.revoke(10); // v2
    // Re-admitting after revoke must NOT return the old version 1; it must
    // assign a new higher version so a role that confirmed at v1 does not
    // see the re-admitted pubkey as visible until it re-confirms.
    EXPECT_EQ(l.admit(10), 3u);
    EXPECT_EQ(l.admission_version_of(10).value(), 3u);

    // Verify the reason: a role that confirmed at v2 (post-revoke, pre-
    // re-admit) sees the re-admitted 10 as NOT visible (adm=3 > conf=2).
    l.confirm(/*role=*/7, 2u);
    auto v = l.is_visible_to(7, 10);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(*v);
}

// ── confirm() semantics ─────────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, Confirm_AdvancesRoleVersion)
{
    IntLedger l;
    // Establish issued versions via admits so the clamp accepts legitimate
    // confirms (INVARIANT-BIND-CONFIRM-1 boundary).
    for (int i = 1; i <= 10; ++i) l.admit(i);
    ASSERT_EQ(l.current_version(), 10u);
    EXPECT_EQ(l.confirm(/*role=*/7, 5u), 5u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 5u);
    EXPECT_EQ(l.confirm(7, 10u), 10u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 10u);
}

TEST(VersionedAdmissionLedger, Confirm_IsMonotonic_StaleVersionsAbsorbed)
{
    IntLedger l;
    for (int i = 1; i <= 10; ++i) l.admit(i);  // current_version = 10
    l.confirm(7, 10u);
    // Stale APPLIED_REQ (from a delayed / duplicated wire message) must
    // NOT regress the stored value.
    EXPECT_EQ(l.confirm(7, 5u), 10u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 10u);
}

TEST(VersionedAdmissionLedger, Confirm_PerRoleIndependent)
{
    IntLedger l;
    for (int i = 1; i <= 10; ++i) l.admit(i);  // current_version = 10
    l.confirm(7, 10u);
    l.confirm(8, 3u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 10u);
    EXPECT_EQ(l.confirmed_version_of(8).value(), 3u);
    // Role 9 never confirmed → nullopt.
    EXPECT_FALSE(l.confirmed_version_of(9).has_value());
}

TEST(VersionedAdmissionLedger, Confirm_WithZero_IsNoOpButRecordsRole)
{
    IntLedger l;
    // confirm(0) records the role with confirmed=0 — legal wire round-trip
    // for a binding-side role that has "installed nothing".
    EXPECT_EQ(l.confirm(7, 0u), 0u);
    EXPECT_TRUE(l.confirmed_version_of(7).has_value());
    EXPECT_EQ(l.confirmed_version_of(7).value(), 0u);
}

TEST(VersionedAdmissionLedger, Confirm_ClampsAtCurrentVersion_AdversarialInput)
{
    // HEP-CORE-0042 §5.5.2 INVARIANT-BIND-CONFIRM-1 clamp guard.
    // A role fabricating `applied_version >> current_version_` (buggy
    // handler, adversarial peer with valid CURVE creds, wire corruption)
    // MUST NOT be able to store a confirmed_version above what the
    // ledger has issued.  Without this clamp, every subsequent admit
    // would become immediately visible to that role.
    IntLedger l;
    l.admit(10);  // current_version = 1
    l.admit(20);  // current_version = 2
    // Attacker/buggy caller sends applied_version = UINT64_MAX.
    const std::uint64_t stored =
        l.confirm(7, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(stored, 2u) << "must clamp at current_version_";
    EXPECT_EQ(l.confirmed_version_of(7).value(), 2u);
    // Verify the intended defense: a NEW admission (version 3) is NOT
    // immediately visible to role 7 (would have been TRUE under the
    // pre-clamp behavior where confirmed=UINT64_MAX >= 3).
    l.admit(30);  // current_version = 3
    auto vis = l.is_visible_to(7, 30);
    ASSERT_TRUE(vis.has_value());
    EXPECT_FALSE(*vis) << "new admission must NOT be visible to a role "
                          "whose confirm was clamped down";
}

TEST(VersionedAdmissionLedger, Confirm_ClampInteractsCorrectlyWithMonotonicity)
{
    // The clamp lives inside the same call as monotonicity.  Verify the
    // interaction: after a legitimate confirm(V=2), a follow-up
    // confirm(applied=1000) clamps to current_version_ and takes the
    // max — the earlier 2 stays because 2 < clamped.
    IntLedger l;
    l.admit(10);  // v1
    l.admit(20);  // v2
    l.confirm(7, 2u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 2u);
    // Adversarial follow-up.
    l.confirm(7, 1000u);
    // Clamped to 2, max(2, 2) = 2 — unchanged.
    EXPECT_EQ(l.confirmed_version_of(7).value(), 2u);
    // Legitimate follow-up at v2 after another admit.
    l.admit(30);  // v3
    l.confirm(7, 3u);
    EXPECT_EQ(l.confirmed_version_of(7).value(), 3u);
}

TEST(VersionedAdmissionLedger, Confirm_ClampAllowsExactCurrentVersion)
{
    // Boundary: confirm(applied == current_version_) is a valid, non-
    // adversarial confirmation.  Must succeed and store current_version_.
    IntLedger l;
    l.admit(10);  // v1
    l.admit(20);  // v2
    const std::uint64_t v = l.current_version();
    ASSERT_EQ(v, 2u);
    EXPECT_EQ(l.confirm(7, v), v);
    EXPECT_EQ(l.confirmed_version_of(7).value(), v);
}

// ── is_visible_to() — the core query ────────────────────────────────────────

TEST(VersionedAdmissionLedger, IsVisibleTo_TrueWhenAdmissionLeqConfirmed)
{
    IntLedger l;
    l.admit(10);        // v1
    l.confirm(7, 1u);   // role 7 confirmed at v1
    auto v = l.is_visible_to(7, 10);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(*v);
}

TEST(VersionedAdmissionLedger, IsVisibleTo_FalseWhenAdmissionGtConfirmed)
{
    IntLedger l;
    l.admit(10);       // v1
    l.admit(20);       // v2
    l.confirm(7, 1u);  // role 7 confirmed at v1 only
    // Pubkey 20 admitted at v2 > confirmed 1 → NOT visible.
    // This is the exact test-#2480 race pin: 20 was admitted AFTER role 7's
    // last APPLIED_REQ; broker MUST NOT confirm 20 for role 7 until role 7
    // sends a new APPLIED_REQ with version >= 2.
    auto v = l.is_visible_to(7, 20);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(*v);
}

TEST(VersionedAdmissionLedger, IsVisibleTo_FalseWhenPubkeyNotAdmitted)
{
    IntLedger l;
    l.admit(10);
    l.confirm(7, 1u);
    auto v = l.is_visible_to(7, 999);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(*v);
}

TEST(VersionedAdmissionLedger, IsVisibleTo_NulloptWhenRoleUnknown)
{
    IntLedger l;
    l.admit(10);
    // Role 7 never called confirm() — the wire caller (dialing role's
    // CHECK_PEER_READY_REQ path) distinguishes "not_confirmed" from
    // "not_admitted" via this nullopt/false split.
    EXPECT_FALSE(l.is_visible_to(7, 10).has_value());
}

// ── Race-relevant sequences ─────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, Test2480_RaceRegression_TwoProducerAdmissionSequence)
{
    // Mirror the test-#2480 timeline at the primitive level:
    //   T1: producer A admitted → v1
    //   T2: consumer's APPLIED_REQ(applied_version=1) → confirm(cons, 1)
    //   T3: producer B admitted → v2
    //   T4: producer B queries: am I visible to consumer?
    //   T5: consumer's APPLIED_REQ(applied_version=2) → confirm(cons, 2)
    //   T6: producer B queries again.
    //
    // Pre-fix bug: at T4, broker's `binding_side_confirmed_allowlist` was
    // populated by snapshotting CURRENT authorized_consumer_pubkeys at
    // T2, which at that instant already contained B (broker had just
    // admitted B before processing consumer's T2 APPLIED_REQ).  Broker
    // wrongly answered "ready" to B.
    //
    // Post-fix (this ledger): at T4, admission_version(B)=v2 >
    // confirmed_version(cons)=v1 → NOT visible.  Correct.  At T6, after
    // confirm(cons, 2), admission_version(B)=v2 <= confirmed_version(cons)=v2
    // → visible.  Correct.
    StringLedger l;
    const std::string A    = "A_pubkey";
    const std::string B    = "B_pubkey";
    const std::string cons = "cons_uid";

    // T1
    auto vA = l.admit(A);
    EXPECT_EQ(vA, 1u);
    // T2
    l.confirm(cons, 1u);
    // T3
    auto vB = l.admit(B);
    EXPECT_EQ(vB, 2u);
    // T4 — the moment of the race
    auto t4 = l.is_visible_to(cons, B);
    ASSERT_TRUE(t4.has_value());
    EXPECT_FALSE(*t4);
    // T5
    l.confirm(cons, 2u);
    // T6
    auto t6 = l.is_visible_to(cons, B);
    ASSERT_TRUE(t6.has_value());
    EXPECT_TRUE(*t6);
    // A is visible throughout (its version <= confirmed from T2 onwards).
    auto vAvis = l.is_visible_to(cons, A);
    ASSERT_TRUE(vAvis.has_value());
    EXPECT_TRUE(*vAvis);
}

TEST(VersionedAdmissionLedger, RevocationRace_RevokedPubkeyImmediatelyInvisible)
{
    // Even before the role re-confirms, a revoked pubkey MUST become
    // invisible immediately — the admission_version map entry is gone,
    // so is_visible_to returns false regardless of the role's
    // confirmed_version.  Documents that "visibility" is a two-condition
    // predicate: (pk currently admitted) AND (admission_version <=
    // confirmed_version).  Revocation kills the first condition
    // immediately, without needing role confirmation.
    IntLedger l;
    l.admit(10);      // v1
    l.confirm(7, 1u); // role 7 fully confirmed at v1
    ASSERT_TRUE(l.is_visible_to(7, 10).value());

    l.revoke(10);
    auto post = l.is_visible_to(7, 10);
    ASSERT_TRUE(post.has_value());
    EXPECT_FALSE(*post);
}

// ── Observability accessors ─────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, AdmittedSnapshot_ContainsAllAdmittedPubkeys)
{
    IntLedger l;
    l.admit(10);
    l.admit(20);
    l.admit(30);
    l.revoke(20);
    auto snap = l.admitted_snapshot();
    EXPECT_EQ(snap.size(), 2u);
    // Order is unspecified — sort for comparison.
    std::sort(snap.begin(), snap.end());
    EXPECT_EQ(snap[0], 10);
    EXPECT_EQ(snap[1], 30);
}

TEST(VersionedAdmissionLedger, ForEachAdmitted_VisitsEveryPubkeyExactlyOnce)
{
    IntLedger l;
    l.admit(10);
    l.admit(20);
    l.admit(30);
    l.revoke(20);  // now: {10, 30}
    std::vector<int> visited;
    l.for_each_admitted([&](int pk) { visited.push_back(pk); });
    std::sort(visited.begin(), visited.end());
    ASSERT_EQ(visited.size(), 2u);
    EXPECT_EQ(visited[0], 10);
    EXPECT_EQ(visited[1], 30);
}

TEST(VersionedAdmissionLedger, ForEachAdmitted_EmptyLedger_VisitsNothing)
{
    IntLedger l;
    int count = 0;
    l.for_each_admitted([&](int) { ++count; });
    EXPECT_EQ(count, 0);
    // After all-revoked path too.
    l.admit(10);
    l.revoke(10);
    l.for_each_admitted([&](int) { ++count; });
    EXPECT_EQ(count, 0);
}

TEST(VersionedAdmissionLedger, ForEachAdmitted_MatchesSnapshot)
{
    // Equivalence pin: for_each_admitted visits the same set as
    // admitted_snapshot returns.  Order unspecified — compare as sets.
    IntLedger l;
    for (int i = 1; i <= 100; ++i) l.admit(i);
    for (int i = 10; i <= 20; ++i) l.revoke(i);
    std::vector<int> via_snapshot = l.admitted_snapshot();
    std::vector<int> via_visitor;
    l.for_each_admitted([&](int pk) { via_visitor.push_back(pk); });
    std::sort(via_snapshot.begin(), via_snapshot.end());
    std::sort(via_visitor.begin(), via_visitor.end());
    EXPECT_EQ(via_snapshot, via_visitor);
}

// ── Reset semantics ─────────────────────────────────────────────────────────

TEST(VersionedAdmissionLedger, MaxConfirmedVersion_NoRoles_ReturnsZero)
{
    IntLedger l;
    EXPECT_EQ(l.max_confirmed_version(), 0u);
    l.admit(10);
    // Admissions alone don't confirm — max stays 0.
    EXPECT_EQ(l.max_confirmed_version(), 0u);
}

TEST(VersionedAdmissionLedger, MaxConfirmedVersion_ReturnsHighestAcrossRoles)
{
    IntLedger l;
    for (int i = 1; i <= 10; ++i) l.admit(i);
    l.confirm(7, 3u);
    l.confirm(8, 7u);
    l.confirm(9, 5u);
    EXPECT_EQ(l.max_confirmed_version(), 7u);
    // Advance role 7 past 8 — max moves up.
    l.confirm(7, 9u);
    EXPECT_EQ(l.max_confirmed_version(), 9u);
    // Role 8 stale (retrieved) confirm — no regression, max unchanged.
    l.confirm(8, 2u);
    EXPECT_EQ(l.max_confirmed_version(), 9u);
}

TEST(VersionedAdmissionLedger, MaxConfirmedVersion_ClampedInputRespected)
{
    // The clamp inside confirm() applies before max is computed.  A
    // role that fabricates applied=UINT64_MAX gets clamped to
    // current_version_; max reflects that, not the raw wire input.
    IntLedger l;
    for (int i = 1; i <= 5; ++i) l.admit(i);
    l.confirm(7, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(l.max_confirmed_version(), 5u);
}

TEST(VersionedAdmissionLedger, ResetRoleConfirmation_KeepsAdmissions)
{
    IntLedger l;
    l.admit(10);      // v1
    l.confirm(7, 1u);
    l.confirm(8, 1u);
    EXPECT_TRUE(l.reset_role_confirmation(7));
    // Role 7 is gone — visibility returns nullopt again.
    EXPECT_FALSE(l.is_visible_to(7, 10).has_value());
    // Role 8 unaffected.
    EXPECT_TRUE(l.is_visible_to(8, 10).value());
    // Admissions unaffected.
    EXPECT_EQ(l.admission_version_of(10).value(), 1u);
    EXPECT_EQ(l.current_version(), 1u);
    // Idempotent: resetting a non-existent role's confirmation returns false.
    EXPECT_FALSE(l.reset_role_confirmation(999));
}
