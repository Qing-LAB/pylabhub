/**
 * @file test_v6_concepts_compile.cpp
 * @brief Compile-time verification of the v6 C++20 visitor concepts
 *        (#194 Phase C, HEP-CORE-0028 §5.3).
 *
 * This translation unit is built as a static library — it produces no
 * runtime test, but failing to compile is the test itself: it pins the
 * positive cases (noexcept lambdas satisfying the concept) AND verifies
 * the negative cases via static_assert(!Concept<Type>).
 *
 * The build target intentionally has no run-time side; the assertion is
 * "this file compiles."  If a future edit weakens the concept (drops
 * the noexcept requirement, accepts the wrong arg shape, etc.), the
 * build breaks.
 */

#include "utils/native_engine_api.h"

namespace
{

// ── Positive cases — these MUST satisfy the concept ──────────────────

/// Noexcept visitor with the right arg shape — must satisfy
/// AllowedPeerVisitor.
inline constexpr auto good_allowed_peer_visitor =
    [](const plh_allowed_peer_t *) noexcept {};

static_assert(plh::AllowedPeerVisitor<decltype(good_allowed_peer_visitor)>,
              "AllowedPeerVisitor must accept a noexcept lambda of the right shape");

inline constexpr auto good_band_member_visitor =
    [](const plh_band_member_t *) noexcept {};

static_assert(plh::BandMemberVisitor<decltype(good_band_member_visitor)>,
              "BandMemberVisitor must accept a noexcept lambda of the right shape");

// ── Negative case A: visitor is NOT noexcept ─────────────────────────

/// Same arg shape but no `noexcept` — must NOT satisfy the concept,
/// because HEP-CORE-0028 §4.8 mandates noexcept visitors at the C ABI
/// boundary.
inline constexpr auto throwing_allowed_peer_visitor =
    [](const plh_allowed_peer_t *) {};

static_assert(!plh::AllowedPeerVisitor<decltype(throwing_allowed_peer_visitor)>,
              "AllowedPeerVisitor must REJECT a visitor that can throw");

inline constexpr auto throwing_band_member_visitor =
    [](const plh_band_member_t *) {};

static_assert(!plh::BandMemberVisitor<decltype(throwing_band_member_visitor)>,
              "BandMemberVisitor must REJECT a visitor that can throw");

// ── Negative case B: wrong arg shape (cross-mixed types) ─────────────

/// A band visitor taking the allowed-peer arg type should not satisfy
/// AllowedPeerVisitor — the concept must enforce the typed-arg contract.
inline constexpr auto wrong_shape_visitor =
    [](const plh_band_member_t *) noexcept {};

static_assert(!plh::AllowedPeerVisitor<decltype(wrong_shape_visitor)>,
              "AllowedPeerVisitor must REJECT a visitor with the wrong arg type");

} // anonymous namespace
