#pragma once
/**
 * @file role_host_lifecycle.hpp
 * @brief Shared role-host lifecycle helpers (HEP-CORE-0034 Phase 5c).
 *
 * The three role hosts (producer, consumer, processor) duplicate
 * portions of their `worker_main_()` lifecycle.  This header pulls
 * out the safest, most mechanical pieces:
 *
 *  - `make_broker_comm_config()` — builds `BrokerRequestComm::Config`
 *    from the role's hub-reference + auth + identity.  Same shape
 *    across all three hosts; the only role-specific input is which
 *    `HubRefConfig` to use (in_hub / out_hub).
 *
 *  - `do_role_teardown()` — runs Steps 9-14 of the worker_main
 *    epilogue (stop_accepting → deregister → invoke_on_stop →
 *    finalize → notify → drain).  All three hosts run the same
 *    sequence; teardown_infrastructure_() (the role-specific cleanup)
 *    is invoked via the caller-provided callback because each host
 *    closes different infrastructure.
 *
 * Larger lifecycle dedup (extract the entire `worker_main_` skeleton
 * into a `RoleHostBase` template) is queued in TODO_MASTER as the
 * Phase 5c-large variant.  The pieces here are the cleanly safe
 * subset — no control-flow change, just call-site collapse.
 */

#include "utils/broker_request_comm.hpp"
#include "utils/config/auth_config.hpp"
#include "utils/config/hub_ref_config.hpp"

#include <functional>
#include <string>

namespace pylabhub::scripting
{
class RoleAPIBase;
class RoleHostCore;
class ScriptEngine;
}

namespace pylabhub::hub
{
class BrokerRequestComm;
}

namespace pylabhub::scripting
{

/**
 * @brief Build the ZMQ + CURVE config the role's ctrl thread uses to
 *        reach the broker.
 *
 * @param hub        The role-side hub reference (consumer uses
 *                   `config_.in_hub()`, producer uses `out_hub()`,
 *                   processor picks per-direction).
 * @param auth       The role's auth config (client keypair already
 *                   loaded by `RoleConfig::load_keypair()`).
 * @param role_uid   HEP-CORE-0023 §2 role uid.
 * @param role_name  Human-readable role name.
 */
inline ::pylabhub::hub::BrokerRequestComm::Config
make_broker_comm_config(const ::pylabhub::config::HubRefConfig  &hub,
                        const ::pylabhub::config::AuthConfig &auth,
                        const std::string                    &role_uid,
                        const std::string                    &role_name)
{
    ::pylabhub::hub::BrokerRequestComm::Config bc_cfg;
    bc_cfg.broker_endpoint = hub.broker;
    bc_cfg.broker_pubkey   = hub.broker_pubkey;
    bc_cfg.client_pubkey   = auth.client_pubkey;
    bc_cfg.client_seckey   = auth.client_seckey;
    bc_cfg.role_uid        = role_uid;
    bc_cfg.role_name       = role_name;
    return bc_cfg;
}

/**
 * @brief Run the worker-main epilogue (Steps 9-14).
 *
 * Sequence (preserved verbatim from each role host's pre-Phase-5c
 * implementation):
 *   9.  engine.stop_accepting()
 *   9a. if (has_api) api.deregister_from_broker()
 *   10. engine.invoke_on_stop()
 *   11. engine.finalize()
 *   12. broker_comm.stop() (non-destructive); core.set_running(false);
 *       core.notify_incoming()
 *   13. teardown_infrastructure() — caller-supplied callback because
 *       each host closes different objects (queues, inbox, etc.)
 *   14. api.thread_manager().drain()
 *
 * `broker_comm` may be nullptr when broker registration was never
 * established (matches the existing `if (broker_comm_) ...` guard
 * in each host).
 */
void do_role_teardown(
    ::pylabhub::scripting::ScriptEngine     &engine,
    ::pylabhub::scripting::RoleAPIBase      &api,
    ::pylabhub::scripting::RoleHostCore     &core,
    ::pylabhub::hub::BrokerRequestComm      *broker_comm,
    bool                                     has_api,
    std::function<void()>                    teardown_infrastructure);

} // namespace pylabhub::scripting
