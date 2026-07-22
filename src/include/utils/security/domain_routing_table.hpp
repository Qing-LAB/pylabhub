#pragma once
/**
 * @file domain_routing_table.hpp
 * @brief ZAP-domain → PeerAdmission routing table with lock-bounded
 *        callback access + reentrance guard.
 *
 * Encapsulates the `(string → reference_wrapper<PeerAdmission>)` routing
 * map + shared_mutex that an admission dispatcher uses to look up the
 * correct `PeerAdmission` for an incoming request, AND the lock-scope
 * contract that prevents a concurrent unregister from freeing the
 * admission referent mid-decision.
 *
 * **Why a class** (and not an inline `unordered_map` per dispatcher):
 * the lock-scope contract + reentrance guard + exception safety are
 * non-trivial invariants.  Inlining them in every dispatcher invites
 * the kind of copy-paste drift that ships UAFs (the original
 * `ZapRouter::pump_one` bug recorded in
 * `docs/code_review/REVIEW_ZapRouter_UAF_2026-06-13.md`).  The
 * invariants live here, in one place.
 *
 * **Consumers:**
 *   - `ZapRouter` (CURVE / ZAP — the original consumer).
 *   - Future federation peer ROUTER (HEP-CORE-0037).
 *   - Future CURVE-wrapped admin REP (design §8 P-Admin option a).
 *
 * **Threading model.**
 *   - `register_domain` / `unregister_domain` — any thread; serialized
 *     by `std::shared_mutex` in unique mode.
 *   - `with_admission` — any thread; serialized by `std::shared_mutex`
 *     in shared mode.  Many concurrent dispatchers can call this
 *     against different (or the same) domain in parallel; concurrent
 *     `unregister_domain` blocks until all in-flight callbacks
 *     complete.
 *
 * @see HEP-CORE-0036 §7.4 (runtime-enforced contracts on the
 *      admission-reference lifetime).
 * @see peer_admission.hpp (`is_peer_allowed`'s 5-clause reentrance
 *      contract that callbacks passed to `with_admission` must honor).
 */

#include "pylabhub_utils_export.h"
#include "utils/recursion_guard.hpp"
#include "utils/security/peer_admission.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pylabhub::utils::security
{

class PYLABHUB_UTILS_EXPORT DomainRoutingTable
{
  public:
    DomainRoutingTable();
    ~DomainRoutingTable();

    DomainRoutingTable(const DomainRoutingTable &) = delete;
    DomainRoutingTable &operator=(const DomainRoutingTable &) = delete;
    DomainRoutingTable(DomainRoutingTable &&) = delete;
    DomainRoutingTable &operator=(DomainRoutingTable &&) = delete;

    /// Register `(domain, &admission)`.  Returns true on insert; false
    /// if `domain` is empty OR is already registered (idempotent — no
    /// overwrite).  Caller decides recovery on `false` (typically:
    /// throw + roll back any prior side effects).
    ///
    /// Non-null contract on `admission` is enforced by the reference
    /// parameter — callers cannot pass a null admission.
    [[nodiscard]] bool register_domain(std::string domain, PeerAdmission &admission);

    /// Remove `domain`.  Idempotent — silent no-op if `domain` is
    /// absent or empty.  Caller MUST run before the admission referent
    /// is destroyed.
    void unregister_domain(const std::string &domain) noexcept;

    /// **Lock-bounded admission lookup with reentrance guard.**
    ///
    /// Behavior:
    ///   1. Acquires the table's `shared_mutex` in shared mode.
    ///   2. Looks up `domain`; returns `std::nullopt` if absent.
    ///   3. Pushes `RecursionGuard(reentrance_key)` on the calling
    ///      thread (per `utils/recursion_guard.hpp` — thread-local
    ///      RAII) so callbacks that re-enter `register_domain` /
    ///      `unregister_domain` from inside `fn` can detect the
    ///      reentrance via `is_recursing(reentrance_key)` and take
    ///      their own action (refuse / panic).
    ///   4. Invokes `fn(admission)`.  Catches every exception
    ///      (`std::exception` and otherwise) and translates to
    ///      `false` (deny) with an error log.  Future implementers of
    ///      `is_peer_allowed` may throw; this prevents a throw from
    ///      crashing the dispatching poll thread.
    ///   5. Releases the shared_lock + RecursionGuard.
    ///
    /// **Lifetime guarantee:** the `admission` reference passed to
    /// `fn` is valid for the entire callback.  A concurrent
    /// `unregister_domain` blocks on the unique_lock until `fn`
    /// returns.
    ///
    /// **Caller obligations** on the RAII reentrance guard: the
    /// callback's transport-level public APIs (e.g.
    /// `ZapRouter::register_domain`, `ZmqQueue::set_peer_allowlist`)
    /// MUST check `RecursionGuard::is_recursing(reentrance_key)` at
    /// their entry points and react.  The table itself does not
    /// observe `is_recursing` against its own mutators — that would
    /// move the policy decision into the wrong layer.
    ///
    /// @param domain          ZAP domain to look up.
    /// @param reentrance_key  Caller's identity for the
    ///                        `RecursionGuard` (typically `this` —
    ///                        the dispatching object).  Used by the
    ///                        caller's `register_domain` /
    ///                        `unregister_domain` to detect reentrant
    ///                        calls from inside `fn`.
    /// @param fn              Callback receiving `PeerAdmission &`
    ///                        and returning `bool` (allow / deny).
    ///                        Must be synchronous and bounded-time
    ///                        per the contract in `peer_admission.hpp`.
    ///
    /// @return `std::nullopt` if `domain` is not registered.
    ///         Otherwise `fn`'s `bool` result (or `false` if `fn`
    ///         threw).
    template <class Fn>
    [[nodiscard]] std::optional<bool> with_admission(const std::string &domain,
                                                     const void *reentrance_key, Fn &&fn) const
    {
        std::optional<bool> decision;
        std::shared_lock<std::shared_mutex> lk(mu_);
        if (auto it = map_.find(domain); it != map_.end())
        {
            PeerAdmission &admission = it->second; // ref-wrapper → T&
            pylabhub::basics::RecursionGuard guard(reentrance_key);
            try
            {
                decision = std::forward<Fn>(fn)(admission);
            }
            catch (const std::exception &e)
            {
                log_admission_threw_(domain, e.what());
                decision = false;
            }
            catch (...)
            {
                log_admission_threw_unknown_(domain);
                decision = false;
            }
        }
        return decision;
    }

    /// For tests + diagnostics: count of currently-registered domains.
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    // Non-templated logger calls live in the .cpp to keep `<fmt>` /
    // `utils/logger.hpp` out of the public header.  The template
    // `with_admission` only references the function declarations.
    static void log_admission_threw_(const std::string &domain, const char *what);
    static void log_admission_threw_unknown_(const std::string &domain);

    std::unordered_map<std::string, std::reference_wrapper<PeerAdmission>> map_;
    mutable std::shared_mutex mu_;
};

} // namespace pylabhub::utils::security
