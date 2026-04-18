#pragma once
/**
 * @file signal_handler_workers.h
 * @brief Worker for InteractiveSignalHandler ↔ Lifecycle integration test.
 *
 * Most InteractiveSignalHandler tests are pure-API (no LOGGER_*, no module
 * deps) and stay in the parent's gtest runner. The single test that drives
 * register_dynamic_module + load_module + LifecycleGuard teardown moves
 * into a worker so the LifecycleGuard owns its own subprocess.
 */

namespace pylabhub::tests::worker
{
namespace signal_handler
{

/** Verifies handler is uninstalled when the LifecycleGuard finalizes. */
int lifecycle_module_uninstalls_on_finalize();

} // namespace signal_handler
} // namespace pylabhub::tests::worker
