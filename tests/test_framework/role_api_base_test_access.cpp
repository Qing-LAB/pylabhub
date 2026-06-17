/**
 * @file role_api_base_test_access.cpp
 * @brief Implementation of the L2 test access helper.
 *
 * Routes through `RoleAPIBase`'s private `install_handler_for_test_`
 * method — reachable here because this class is friended in
 * `role_api_base.hpp`.  Both the friend declaration and the
 * `install_handler_for_test_` method are physically gated to
 * `PYLABHUB_BUILD_TESTS && !defined(NDEBUG)` builds, so this
 * implementation matches the same gate.  In Release / non-test
 * builds the symbol is absent — callers must wrap their call sites
 * accordingly (the L2 test workers use GTEST_SKIP at the parent
 * TEST_F level).
 */
#include "role_api_base_test_access.h"

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
#include "utils/role_api_base.hpp"
#include "utils/role_handler.hpp"
#endif

namespace pylabhub::scripting::test
{

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
void RoleAPIBaseTestAccess::install_handler(
    RoleAPIBase &api, std::unique_ptr<RoleHandler> handler)
{
    api.install_handler_for_test_(std::move(handler));
}
#endif

} // namespace pylabhub::scripting::test
