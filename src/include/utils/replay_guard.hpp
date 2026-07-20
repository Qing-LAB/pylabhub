#pragma once
/**
 * @file replay_guard.hpp
 * @brief Sliding-window nonce dedup shared by every plane that needs
 *        application-level replay defense (I-REPLAY-BOUND).
 *
 * ONE mechanism, many callers.  The hub REG/admin plane (via
 * `HubState::nonce_seen`) and the role inbox receiver (HEP-CORE-0027 §3)
 * both dedup client nonces through a `ReplayGuard`.  Each caller owns its
 * own instance — the hub's store is process-local to the hub, the
 * inbox's is process-local to the role — but the check-and-record LOGIC
 * lives here, not duplicated per plane.
 *
 * Keyed by an `identity` string (role_uid on the REG/admin plane, the
 * sender's ZMQ identity on the inbox).  An (identity, nonce) pair is
 * fresh unless it was recorded within `window_ms` of the current call.
 *
 * CLOCK SOURCE — SECURITY-CRITICAL, OWNED BY THE GUARD.  The guard reads
 * its OWN clock; there is deliberately NO per-call timestamp argument, so
 * a caller CANNOT feed the client-supplied wall stamp into the dedup
 * window.  That footgun — pruning against attacker-controllable time —
 * would let an authenticated peer forward-stamp a frame to evict an
 * earlier nonce and then replay it, defeating I-REPLAY-BOUND.  The client
 * wall stamp belongs ONLY in the caller's SEPARATE skew gate (a distinct
 * reject cleared BEFORE calling); this class never sees it.
 *
 * The default clock is a MONOTONIC steady clock (immune to wall-clock /
 * NTP jumps).  Tests inject a controllable `ClockFn` at construction to
 * drive time deterministically.  The window is a duration, so the
 * arbitrary steady-clock epoch is irrelevant.
 *
 * WINDOW SIZING.  A replay stays skew-acceptable for up to `2 * skew`
 * after the original (the skew tolerance applies to both the original
 * acceptance and the replay), so callers MUST size `window_ms >= 2 *
 * skew_tolerance_ms` for the dedup to catch every skew-acceptable replay.
 *
 * Prune-on-access keeps the footprint bounded to the live window without
 * a background sweep: every call for an identity first drops that
 * identity's entries older than the window.  Thread-safe via an internal
 * mutex, so a role-side inbox receiver thread and, on the hub, the
 * single-pumper dispatch thread each use it without external locking.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pylabhub::utils
{

class ReplayGuard
{
public:
    /// Trusted clock: returns a monotonically non-decreasing time in ms.
    /// Production installs a steady clock; tests inject a fake to drive
    /// time deterministically.  NEVER wired to a client-supplied value.
    using ClockFn = std::function<std::uint64_t()>;

    /// Default: monotonic steady clock (ms).  A caller cannot substitute a
    /// per-call timestamp; the only way to influence the clock is to inject
    /// a `ClockFn` here, which production never points at client input.
    ReplayGuard() : clock_(&steady_now_ms) {}

    /// Test / advanced seam: inject the trusted clock.  An empty `clock`
    /// falls back to the steady clock (fail-safe — never a null call).
    explicit ReplayGuard(ClockFn clock)
        : clock_(clock ? std::move(clock) : ClockFn{&steady_now_ms})
    {
    }

    /// Freshness check + record, atomic under the guard's lock.  Returns
    /// true when (identity, nonce) is fresh (and records it); false on
    /// reuse within `window_ms`, OR on empty identity/nonce (fail-closed
    /// — a caller that cannot name the sender or the nonce is rejected).
    ///
    /// The reference time is read from the guard's OWN trusted clock — see
    /// the SECURITY-CRITICAL note in the file header.  `window_ms` MUST be
    /// >= 2 * the caller's skew tolerance.
    [[nodiscard]] bool check_and_record(std::string_view  identity,
                                        std::string_view  nonce,
                                        std::uint64_t     window_ms)
    {
        if (identity.empty() || nonce.empty())
            return false;

        std::lock_guard<std::mutex> lk(mu_);
        const std::uint64_t now_ms = clock_();
        auto &entries = by_identity_[std::string(identity)];

        // Prune-on-access: drop entries older than (now_ms - window_ms).
        // Underflow-safe: if window_ms > now_ms, use 0 as the floor.
        const std::uint64_t cutoff =
            (now_ms > window_ms) ? (now_ms - window_ms) : 0;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [cutoff](const Entry &e) {
                                         return e.recorded_ms < cutoff;
                                     }),
                      entries.end());

        for (const auto &e : entries)
            if (e.nonce == nonce)
                return false; // duplicate within the window

        entries.push_back(Entry{std::string(nonce), now_ms});
        return true;
    }

private:
    struct Entry
    {
        std::string   nonce;
        std::uint64_t recorded_ms; ///< trusted-clock time this entry was stored
    };

    /// Monotonic steady clock in ms.  Epoch is arbitrary — only durations
    /// (window comparisons) are meaningful, which is all the guard uses.
    static std::uint64_t steady_now_ms()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    ClockFn                                             clock_;
    std::mutex                                          mu_;
    std::unordered_map<std::string, std::vector<Entry>> by_identity_;
};

} // namespace pylabhub::utils
