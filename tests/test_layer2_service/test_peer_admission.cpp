/**
 * @file test_peer_admission.cpp
 * @brief L2 unit tests for the PeerAdmission policy abstraction
 *        (HEP-CORE-0035 §4.5 / HEP-CORE-0036 §I1; corrected layering
 *        per docs/tech_draft/peer_admission_architecture_design.md §4).
 *
 * Phase A scope: value semantics of `PeerIdentity` and `PeerAllowlist`
 * plus the abstract contract of `PeerAdmission`.  No transport is
 * wired here — the concrete CURVE+ZAP behavior, SHM behavior, etc., are
 * tested at their respective transport-level test files in Phase C / G.
 *
 * Pattern 1 (pure POSIX value-type test; no LOGGER_*, no lifecycle).
 */

#include "utils/security/peer_admission.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using pylabhub::utils::security::PeerAdmission;
using pylabhub::utils::security::PeerAllowlist;
using pylabhub::utils::security::PeerIdentity;

// ── PeerIdentity — value semantics ──────────────────────────────────────────

/// Equality is byte-exact on (kind, data).  Same kind + same data → equal.
TEST(PeerIdentityTest, Equality_SameKindSameData_True)
{
    PeerIdentity a{"curve", "ABCDEF1234567890"};
    PeerIdentity b{"curve", "ABCDEF1234567890"};
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);
}

/// Different kind → not equal even when data matches.  Catches a
/// regression that collapsed the (kind, data) pair into just `data`,
/// which would let a CURVE pubkey match a hex-encoded SHM secret if
/// they happened to share the same string.
TEST(PeerIdentityTest, Equality_DifferentKind_False)
{
    PeerIdentity curve_id{"curve", "deadbeef"};
    PeerIdentity shm_id{"shm",     "deadbeef"};
    EXPECT_NE(curve_id, shm_id);
}

/// Different data → not equal even when kind matches.
TEST(PeerIdentityTest, Equality_DifferentData_False)
{
    PeerIdentity a{"curve", "pubkey_A"};
    PeerIdentity b{"curve", "pubkey_B"};
    EXPECT_NE(a, b);
}

/// Empty fields are valid PeerIdentity values; they compare like any
/// other strings.  This is not a "no peer" sentinel — code that needs
/// to express absence uses `std::optional<PeerIdentity>`.
TEST(PeerIdentityTest, Equality_EmptyFields_TwoEmptiesEqual)
{
    PeerIdentity a;
    PeerIdentity b;
    EXPECT_EQ(a, b);
}

/// Lexicographic ordering: compare kind first, then data.  Required for
/// `std::set` membership and for deterministic snapshot serialization.
TEST(PeerIdentityTest, Ordering_KindBeforeData)
{
    PeerIdentity a{"curve",      "ZZZZ"}; // kind "curve" < kind "shm"
    PeerIdentity b{"shm",        "AAAA"};
    EXPECT_LT(a, b);

    PeerIdentity c{"curve",      "AAAA"};
    PeerIdentity d{"curve",      "BBBB"};
    EXPECT_LT(c, d); // same kind → data orders
}

/// PeerIdentity is usable as `std::set` key — pins the operator<
/// contract that downstream code (PeerAllowlist::peers) depends on.
TEST(PeerIdentityTest, IsSetKey_DeduplicatesByValue)
{
    std::set<PeerIdentity> s;
    s.insert({"curve", "key1"});
    s.insert({"curve", "key1"});  // duplicate
    s.insert({"curve", "key2"});
    EXPECT_EQ(s.size(), 2u);
}

// ── PeerAllowlist — set semantics + contains() + unrestricted escape ────────

/// contains() on empty allowlist returns false for every identity.
/// "Default-deny" semantic at the data-structure layer.
TEST(PeerAllowlistTest, Contains_EmptyList_DeniesAll)
{
    PeerAllowlist al;
    EXPECT_FALSE(al.contains({"curve", "anything"}));
    EXPECT_FALSE(al.contains({"shm",   "anything"}));
    EXPECT_TRUE(al.is_deny_all());
}

/// contains() returns true exactly for inserted identities.
TEST(PeerAllowlistTest, Contains_MatchesInsertedIdentities)
{
    PeerAllowlist al;
    al.peers.insert({"curve", "alice"});
    al.peers.insert({"curve", "bob"});

    EXPECT_TRUE(al.contains({"curve", "alice"}));
    EXPECT_TRUE(al.contains({"curve", "bob"}));
    EXPECT_FALSE(al.contains({"curve", "carol"}));
    // Wrong kind, right data — must not match (regression guard).
    EXPECT_FALSE(al.contains({"shm",   "alice"}));
    EXPECT_FALSE(al.is_deny_all());
}

/// unrestricted=true is the explicit escape: contains() returns true
/// for every identity.  The data-structure documents this audit point
/// clearly so grep "unrestricted" finds every bypass site.
TEST(PeerAllowlistTest, Unrestricted_AdmitsEverything)
{
    PeerAllowlist al;
    al.unrestricted = true;
    EXPECT_TRUE(al.contains({"curve", "anyone"}));
    EXPECT_TRUE(al.contains({"shm",   "anyone"}));
    EXPECT_TRUE(al.contains({}));  // even an empty identity
    EXPECT_FALSE(al.is_deny_all());
}

/// unrestricted=true overrides the explicit peer set.  Catches a
/// regression that AND'd the two conditions ("must be in unrestricted
/// AND in peers"), which would silently make the escape a no-op when
/// peers is empty.
TEST(PeerAllowlistTest, Unrestricted_TrumpsPeersSet)
{
    PeerAllowlist al;
    al.unrestricted = true;
    // peers is empty
    EXPECT_TRUE(al.contains({"curve", "anyone"}));

    // Also true when peers has entries that don't match.
    al.peers.insert({"curve", "alice"});
    EXPECT_TRUE(al.contains({"curve", "stranger"}));
}

/// is_deny_all distinguishes "empty allowlist" from "unrestricted with
/// no specific peers".  Operationally these are very different: the
/// first denies everyone; the second admits everyone.
TEST(PeerAllowlistTest, IsDenyAll_DistinguishesEmptyFromUnrestricted)
{
    PeerAllowlist empty;
    EXPECT_TRUE(empty.is_deny_all());

    PeerAllowlist unrestricted;
    unrestricted.unrestricted = true;
    EXPECT_FALSE(unrestricted.is_deny_all());

    PeerAllowlist populated;
    populated.peers.insert({"curve", "alice"});
    EXPECT_FALSE(populated.is_deny_all());
}

// ── PeerAdmission — abstract contract checks via a test double ──────────────

namespace
{

/// Minimal in-memory PeerAdmission implementation used by Phase A tests
/// to pin the interface contract.  Phase C will exercise the same
/// interface via the real `ZmqQueue` ZAP-mediated behavior.
class InMemoryAdmission final : public PeerAdmission
{
public:
    bool set_peer_allowlist(PeerAllowlist allowlist) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        allowlist_ = std::move(allowlist);
        return true;
    }

    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return allowlist_;
    }

    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!allowlist_.has_value())
            return false;
        return allowlist_->contains(p);
    }

    bool admission_is_enforced() const noexcept override { return true; }

private:
    mutable std::mutex          mu_;
    std::optional<PeerAllowlist> allowlist_;
};

} // namespace

/// Default-constructed implementer denies everything until
/// set_peer_allowlist is called.  Pins "no allowlist installed ⇒
/// is_peer_allowed=false" at the interface contract level.
TEST(PeerAdmissionContractTest, DefaultState_DeniesAll)
{
    InMemoryAdmission a;
    EXPECT_FALSE(a.is_peer_allowed({"curve", "alice"}));
    EXPECT_FALSE(a.peer_allowlist_snapshot().has_value());
}

/// set_peer_allowlist → snapshot reflects the new state byte-for-byte.
TEST(PeerAdmissionContractTest, SetAllowlist_SnapshotEquals)
{
    InMemoryAdmission a;

    PeerAllowlist want;
    want.peers.insert({"curve", "alice"});
    want.peers.insert({"shm",   "0xdead"});

    ASSERT_TRUE(a.set_peer_allowlist(want));

    auto got = a.peer_allowlist_snapshot();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->peers, want.peers);
    EXPECT_EQ(got->unrestricted, want.unrestricted);
}

/// is_peer_allowed reflects the installed allowlist.
TEST(PeerAdmissionContractTest, IsPeerAllowed_RespectsAllowlist)
{
    InMemoryAdmission a;

    PeerAllowlist al;
    al.peers.insert({"curve", "alice"});
    ASSERT_TRUE(a.set_peer_allowlist(al));

    EXPECT_TRUE(a.is_peer_allowed({"curve", "alice"}));
    EXPECT_FALSE(a.is_peer_allowed({"curve", "bob"}));
    EXPECT_FALSE(a.is_peer_allowed({"shm", "alice"})); // wrong kind
}

/// set_peer_allowlist has snapshot semantics: a second call REPLACES
/// the previous state (does NOT merge).  Catches a regression that
/// silently unioned add-ops at the interface layer; the broker glue is
/// the only thing allowed to compute unions.
TEST(PeerAdmissionContractTest, SetAllowlist_ReplacesNotMerges)
{
    InMemoryAdmission a;

    PeerAllowlist first;
    first.peers.insert({"curve", "alice"});
    ASSERT_TRUE(a.set_peer_allowlist(first));

    PeerAllowlist second;
    second.peers.insert({"curve", "bob"});
    ASSERT_TRUE(a.set_peer_allowlist(second));

    EXPECT_FALSE(a.is_peer_allowed({"curve", "alice"}));  // dropped
    EXPECT_TRUE(a.is_peer_allowed({"curve", "bob"}));
}

/// Empty allowlist (after a populated one) is a valid "revoke all"
/// state — deny everyone.  Catches a regression that treated "empty
/// allowlist" as "leave previous state alone".
TEST(PeerAdmissionContractTest, SetAllowlist_EmptyRevokesAll)
{
    InMemoryAdmission a;

    PeerAllowlist populated;
    populated.peers.insert({"curve", "alice"});
    ASSERT_TRUE(a.set_peer_allowlist(populated));
    EXPECT_TRUE(a.is_peer_allowed({"curve", "alice"}));

    ASSERT_TRUE(a.set_peer_allowlist(PeerAllowlist{}));
    EXPECT_FALSE(a.is_peer_allowed({"curve", "alice"}));
    auto snap = a.peer_allowlist_snapshot();
    ASSERT_TRUE(snap.has_value());
    EXPECT_TRUE(snap->is_deny_all());
}

/// admission_is_enforced is the broker's signal that the gate is real.
/// The test double here returns true; transitional implementations
/// (the future --allow-anonymous-data path) would return false.  This
/// test pins that the contract is queryable on a noexcept path
/// (callable from destructors / shutdown paths).
TEST(PeerAdmissionContractTest, AdmissionIsEnforced_NoexceptQueryable)
{
    InMemoryAdmission a;
    static_assert(noexcept(a.admission_is_enforced()),
                  "admission_is_enforced must be noexcept — broker may "
                  "query during shutdown / destructor paths");
    EXPECT_TRUE(a.admission_is_enforced());
}

// ── Concurrency contract — set and is_allowed run on different threads ──────

/// Concurrent set_peer_allowlist + peer_allowlist_snapshot must produce
/// internally-coherent snapshots — every returned snapshot must be one
/// of the writer's full snapshots, never a torn mix.
///
/// (Important: two CONSECUTIVE is_peer_allowed calls are NOT a
/// transaction — between the calls, a writer may have swapped the
/// allowlist.  That's snapshot semantics by design, NOT a bug.  This
/// test pins atomicity at the snapshot level, which is the actual
/// contract.)
TEST(PeerAdmissionContractTest, Concurrent_SetAndSnapshot_AlwaysCoherent)
{
    InMemoryAdmission a;

    PeerAllowlist al_alice;
    al_alice.peers.insert({"curve", "alice"});

    PeerAllowlist al_bob;
    al_bob.peers.insert({"curve", "bob"});

    a.set_peer_allowlist(al_alice);

    std::atomic<bool> stop{false};
    std::atomic<int>  saw_alice_snapshot{0};
    std::atomic<int>  saw_bob_snapshot{0};
    std::atomic<int>  saw_torn{0};

    constexpr int N_WRITERS = 2;
    constexpr int N_READERS = 4;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(N_WRITERS + N_READERS));

    for (int i = 0; i < N_WRITERS; ++i)
    {
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire))
            {
                a.set_peer_allowlist(al_alice);
                a.set_peer_allowlist(al_bob);
            }
        });
    }
    for (int i = 0; i < N_READERS; ++i)
    {
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire))
            {
                const auto snap = a.peer_allowlist_snapshot();
                if (!snap.has_value())
                {
                    saw_torn.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const bool only_alice =
                    snap->peers.size() == 1 &&
                    snap->peers.count({"curve", "alice"}) == 1;
                const bool only_bob =
                    snap->peers.size() == 1 &&
                    snap->peers.count({"curve", "bob"}) == 1;
                if (only_alice)
                    saw_alice_snapshot.fetch_add(1, std::memory_order_relaxed);
                else if (only_bob)
                    saw_bob_snapshot.fetch_add(1, std::memory_order_relaxed);
                else
                    saw_torn.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_release);
    for (auto &t : threads)
        t.join();

    EXPECT_GT(saw_alice_snapshot.load(), 0)
        << "Readers never observed the alice snapshot — atomic update "
           "never won the race";
    EXPECT_GT(saw_bob_snapshot.load(), 0)
        << "Readers never observed the bob snapshot — atomic update "
           "never won the race";
    EXPECT_EQ(saw_torn.load(), 0)
        << "Reader observed a torn snapshot (neither solely-alice nor "
           "solely-bob) — atomicity violation in set_peer_allowlist";
}
