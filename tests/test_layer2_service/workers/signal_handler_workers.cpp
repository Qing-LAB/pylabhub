/**
 * @file signal_handler_workers.cpp
 * @brief Worker for the InteractiveSignalHandler/lifecycle integration test.
 */
#include "signal_handler_workers.h"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/interactive_signal_handler.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <source_location>

using pylabhub::InteractiveSignalHandler;
using pylabhub::SignalHandlerConfig;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::LifecycleManager;
using pylabhub::utils::Logger;
using pylabhub::utils::MakeModDefList;

namespace pylabhub::tests::worker
{
namespace signal_handler
{

int lifecycle_module_uninstalls_on_finalize()
{
    // No outer guard — the test body installs and tears down its own
    // LifecycleGuard so the cleanup callback fires inside the test scope.
    // run_worker_bare wraps the body in throw-on-failure + crash protection
    // without registering its own modules.
    return pylabhub::tests::helper::run_worker_bare(
        [&]()
        {
            std::atomic<bool>   shutdown{false};
            SignalHandlerConfig cfg{"test-binary", 5, false, true};
            InteractiveSignalHandler handler(cfg, &shutdown);
            handler.install();
            ASSERT_TRUE(handler.is_installed());

            {
                LifecycleGuard guard(MakeModDefList(Logger::GetLifecycleModule()));

                auto mod = handler.make_lifecycle_module();
                ASSERT_TRUE(LifecycleManager::instance().register_dynamic_module(
                    std::move(mod)));
                ASSERT_TRUE(LifecycleManager::instance().load_module(
                    "SignalHandler", std::source_location::current()));

                EXPECT_TRUE(handler.is_installed());
            } // guard destructor → finalize → SignalHandler cleanup → uninstall

            EXPECT_FALSE(handler.is_installed());
        },
        "signal_handler::lifecycle_module_uninstalls_on_finalize");
}

} // namespace signal_handler
} // namespace pylabhub::tests::worker

namespace
{

struct SignalHandlerWorkerRegistrar
{
    SignalHandlerWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "signal_handler")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::signal_handler;

                if (sc == "lifecycle_module_uninstalls_on_finalize")
                    return lifecycle_module_uninstalls_on_finalize();

                fmt::print(stderr,
                           "[signal_handler] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static SignalHandlerWorkerRegistrar g_signal_handler_registrar;

} // namespace
