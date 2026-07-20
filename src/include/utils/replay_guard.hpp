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
 * fresh unless it was recorded within `window_ms` (compared by
 * `wall_ts`).  Wall-clock SKEW is a DISTINCT reject the caller must clear
 * BEFORE calling — this class only dedups nonces.
 *
 * Prune-on-access keeps the footprint bounded to the live window without
 * a background sweep: every call for an identity first drops that
 * identity's entries older than the window.  Thread-safe via an internal
 * mutex, so a role-side inbox receiver thread and, on the hub, the
 * single-pumper dispatch thread each use it without external locking.
 */

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pylabhub::utils
{

class ReplayGuard
{
public:
    /// Freshness check + record, atomic under the guard's lock.  Returns
    /// true when (identity, nonce) is fresh (and records it); false on
    /// reuse within `window_ms`, OR on empty identity/nonce (fail-closed
    /// — a caller that cannot name the sender or the nonce is rejected).
    [[nodiscard]] bool check_and_record(std::string_view  identity,
                                        std::string_view  nonce,
                                        std::uint64_t     wall_ts,
                                        std::uint64_t     window_ms)
    {
        if (identity.empty() || nonce.empty())
            return false;

        std::lock_guard<std::mutex> lk(mu_);
        auto &entries = by_identity_[std::string(identity)];

        // Prune-on-access: drop entries older than (wall_ts - window_ms).
        // Underflow-safe: if window_ms > wall_ts, use 0 as the floor.
        const std::uint64_t cutoff =
            (wall_ts > window_ms) ? (wall_ts - window_ms) : 0;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [cutoff](const Entry &e) {
                                         return e.wall_ts < cutoff;
                                     }),
                      entries.end());

        for (const auto &e : entries)
            if (e.nonce == nonce)
                return false; // duplicate within the window

        entries.push_back(Entry{std::string(nonce), wall_ts});
        return true;
    }

private:
    struct Entry
    {
        std::string   nonce;
        std::uint64_t wall_ts;
    };
    std::mutex                                          mu_;
    std::unordered_map<std::string, std::vector<Entry>> by_identity_;
};

} // namespace pylabhub::utils
