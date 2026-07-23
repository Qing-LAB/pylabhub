#pragma once
/**
 * @file hub_lua_integration_workers.h
 * @brief Workers for HubHost + LuaEngine integration tests
 *        (HEP-CORE-0033 Phase 7 D3.3; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace hub_lua_integration
{

int real_lua_script_on_init_on_stop_fire_and_log();
int real_lua_script_admin_console_print();
int script_syntax_error_startup_throws();
int on_tick_fires_periodically_when_idle();
int on_tick_catch_up_fixed_rate_with_compensation();
int read_accessors_all_reachable_from_on_init();
int request_shutdown_from_on_init_wakes_main_loop();
int event_observers_channel_registration_fires_on_channel_opened_and_on_role_registered();
int event_observer_consumer_registration_fires_on_consumer_added();
int post_event_from_on_init_fires_on_app_callback();
int augment_query_metrics_from_admin_thread_call_site_mutates_response();
int augment_null_return_keeps_default_response();

} // namespace hub_lua_integration
} // namespace pylabhub::tests::worker
