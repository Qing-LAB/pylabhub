#pragma once
/**
 * @file role_api_base_test_access.h
 * @brief L2 test helper — install a legitimately-constructed
 *        `RoleHandler` on a `RoleAPIBase` for tests that exercise
 *        the `run_data_loop` API contract.
 *
 * **Not a bypass.**  This helper exposes ONE production-private slot
 * (`pImpl->handler_`) so that an L2 test can install a `RoleHandler`
 * it has constructed itself.  It does NOT silence, weaken, or
 * short-circuit any protocol check:
 *
 *   - The data-loop outer guard `RoleAPIBase::any_presence_authorized()`
 *     (HEP-CORE-0036 §8.2) runs exactly as in production: it scans
 *     the installed `RoleHandler::presences()` and returns `true` only
 *     when at least one Presence has `registration_state == Authorized`.
 *   - The test that wants the loop to enter must construct a Presence
 *     whose public `registration_state` atomic is set to `Authorized`
 *     (the SAME write that production code performs at the end of
 *     `apply_*_reg_ack`).
 *   - A test that constructs all presences in non-Authorized states
 *     will observe the same loop-refusal-to-enter behavior production
 *     would.
 *
 * Why this exists instead of going through `start_handler_threads`:
 * an L2 test of the data loop's API contract does NOT need (and
 * should not own) the broker connection / ctrl-thread / BRC setup
 * that production's start_handler_threads path provides.  L3 / L4
 * tests cover those.  L2 stays focused on what `run_data_loop`
 * promises to do given a populated `RoleAPIBase`.
 */

#include <memory>

namespace pylabhub::scripting
{
class RoleAPIBase;
class RoleHandler;

namespace test
{

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
class RoleAPIBaseTestAccess
{
  public:
    /// Install `handler` as `api.pImpl->handler_` — the same slot
    /// production fills via `start_handler_threads`.  After this
    /// call, `api.any_presence_authorized()` reflects the state of
    /// the installed handler's Presences (no override, no flag).
    /// Pre-existing handler (if any) is replaced.
    ///
    /// Class exists only when `PYLABHUB_BUILD_TESTS && !defined(NDEBUG)`.
    /// In Release / non-test builds the entire helper is physically
    /// absent; the L2 test workers GTEST_SKIP at the parent TEST_F.
    static void install_handler(RoleAPIBase &api,
                                std::unique_ptr<RoleHandler> handler);
};
#endif

} // namespace test
} // namespace pylabhub::scripting
