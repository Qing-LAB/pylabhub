#pragma once
/**
 * @file hub_host_workers.h
 * @brief Workers for HubHost lifecycle tests
 *        (HEP-CORE-0033 §4 / Phase 6.1b; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace hub_host
{

int construct_without_startup_no_thread_spawn();
int broker_hub_state_is_hub_host_hub_state();
int startup_idempotent();
int shutdown_idempotent();
int run_main_loop_blocks_until_request_shutdown();
int startup_fails_cleanly_on_busy_port();
int destructor_cleans_up_even_without_explicit_shutdown();
int config_accessor_returns_loaded_value();
int startup_after_shutdown_throws();
int failed_startup_allows_retry();

} // namespace hub_host
} // namespace pylabhub::tests::worker
