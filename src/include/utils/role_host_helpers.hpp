#pragma once
/**
 * @file role_host_helpers.hpp
 * @brief Shared helpers for unified role hosts (engine-agnostic).
 *
 * These functions are extracted from ProducerRoleHost / ConsumerRoleHost /
 * ProcessorRoleHost to eliminate duplication across the three role hosts.
 * All functions are engine-agnostic — they operate on ScriptEngine, RoleHostCore,
 * InboxQueue, and Messenger without knowing the underlying script engine type.
 */

#include "utils/broker_request_comm.hpp"
#include "utils/role_host_core.hpp"
#include "utils/role_api_base.hpp"
#include "utils/config/checksum_config.hpp"
#include "utils/config/inbox_config.hpp"
#include "utils/data_block_policy.hpp"
#include "utils/schema_utils.hpp"
#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/hub_inbox_queue.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// drain_inbox_sync: moved to RoleAPIBase::drain_inbox_sync() (role_api_base.cpp).
// The shared data loop frame calls it in Step C.

// ============================================================================
// wait_for_roles — HEP-0023 startup coordination
// ============================================================================

/**
 * @brief Wait for required peer roles to appear in the broker before continuing.
 *
 * Blocks for up to `wr.timeout_ms` per role.  Logs progress.
 *
 * Pre-M5 (Wave-B M5 prep): takes `RoleAPIBase &api` instead of a raw
 * `BrokerRequestComm &`.  The per-role polling delegates to
 * `api.wait_for_role(uid, timeout_ms)`, which is handler-aware (Class B
 * role-scope query via `pImpl->resolve_bc_for_role()`).  This decouples
 * startup-wait from the role host's BRC ownership model, so M5 can
 * delete `broker_comm_` from producer/consumer/processor without
 * touching this helper.
 *
 * @param api           RoleAPIBase instance — must have either
 *                      `start_ctrl_thread` (legacy) or
 *                      `start_handler_threads` (handler-mode) running.
 * @param wait_list     List of roles to wait for (from config).
 * @param log_tag       Log prefix, e.g. "[prod]".
 * @return true if all roles found; false on first timeout.
 */
inline bool wait_for_roles(RoleAPIBase                      &api,
                            const std::vector<WaitForRole>   &wait_list,
                            const char                       *log_tag)
{
    for (const auto &wr : wait_list)
    {
        LOGGER_INFO("{} Startup: waiting for role '{}' (timeout {}ms)...",
                    log_tag, wr.uid, wr.timeout_ms);
        if (!api.wait_for_role(wr.uid, wr.timeout_ms))
        {
            LOGGER_ERROR("{} Startup wait failed: role '{}' not present after {}ms",
                         log_tag, wr.uid, wr.timeout_ms);
            return false;
        }
        LOGGER_INFO("{} Startup: role '{}' found", log_tag, wr.uid);
    }
    return true;
}

// ============================================================================
// wait_for_peer_ready — RETIRED 2026-07-11 (queue-owned layer surgery)
// ============================================================================
//
// The dial-side readiness pull moved INSIDE `hub::ZmqQueue::finalize_connect`
// per HEP-CORE-0036 §I9.1 (topology and transport are queue-internal) +
// §6.6.3 layer-contract amendment.  Producer role host now calls the
// topology-agnostic `api.finalize_channel_connect(channel, timeout_ms,
// is_cancelled)` uniformly for every role; the queue itself drives the
// poll loop via an injected `hub::PeerReadinessOracle`.  Replaces the
// shipped-2026-07-11 shape (`wait_for_peer_ready` + `dial_now` in
// role code with a topology check).

// ============================================================================
// serialize_inbox_spec_json — inbox schema JSON for broker REG_REQ
// ============================================================================

/**
 * @brief Serialize an inbox hub::SchemaSpec into the JSON format expected by
 * the broker's REG_REQ `inbox_schema_json` wire field (HEP-CORE-0027 §3,
 * HEP-CORE-0034 §11.4).
 *
 * @param spec  The resolved inbox schema spec.
 * @return JSON object with "fields" array matching the spec.
 */
inline nlohmann::json serialize_inbox_spec_json(const hub::SchemaSpec &spec)
{
    nlohmann::json result;
    nlohmann::json fields_arr = nlohmann::json::array();
    for (const auto &f : spec.fields)
    {
        nlohmann::json fj;
        fj["name"]    = f.name;
        fj["type"]    = f.type_str;
        fj["count"]   = f.count;
        if (f.type_str == "string" || f.type_str == "bytes")
            fj["length"] = f.length;
        fields_arr.push_back(std::move(fj));
    }
    result["fields"] = std::move(fields_arr);
    if (spec.packing != "aligned")
        result["packing"] = spec.packing;
    return result;
}

// ============================================================================
// setup_inbox_facility — shared inbox queue setup for all role hosts
// ============================================================================

/**
 * @brief Result of inbox queue setup — fields consumed by producer/consumer/processor
 *        role hosts to populate their REG_REQ inbox metadata (HEP-CORE-0027,
 *        HEP-CORE-0034 §11.4).
 */
struct InboxSetupResult
{
    std::unique_ptr<hub::InboxQueue> queue;
    std::string actual_endpoint;   ///< OS-resolved endpoint (for broker registration).
    std::string schema_json;       ///< Serialized spec (for ROLE_INFO_REQ discovery).
    std::string packing;           ///< From inbox schema (sole source of truth).
    std::string checksum;          ///< Checksum policy string.
};

/**
 * @brief Create and start an InboxQueue from the inbox schema + config.
 *
 * Packing comes from inbox_spec.packing (always populated by parse_schema_json,
 * defaults to "aligned"). No fallback to transport packing — inbox is an
 * independent communication path with its own schema.
 *
 * @param inbox_spec       Resolved inbox schema (must have has_schema=true).
 * @param inbox_cfg        Inbox config (endpoint, buffer_depth, overflow_policy).
 * @param checksum_policy  Role-level checksum policy.
 * @param tag              Log tag (e.g. "prod", "cons", "proc").
 * @return InboxSetupResult on success, nullopt on failure.
 */
inline std::optional<InboxSetupResult>
setup_inbox_facility(const hub::SchemaSpec &inbox_spec,
                     config::InboxConfig   &inbox_cfg,
                     hub::ChecksumPolicy    checksum_policy,
                     const char            *tag)
{
    auto zmq_fields = hub::schema_spec_to_zmq_fields(inbox_spec);

    const int rcvhwm = (inbox_cfg.overflow_policy == "block")
        ? 0
        : static_cast<int>(inbox_cfg.buffer_depth);

    auto queue = hub::InboxQueue::bind_at(
        inbox_cfg.endpoint, std::move(zmq_fields), inbox_spec.packing, rcvhwm);
    if (!queue || !queue->start())
    {
        LOGGER_ERROR("[{}] Failed to start InboxQueue at '{}'", tag, inbox_cfg.endpoint);
        return std::nullopt;
    }

    queue->set_checksum_policy(checksum_policy);

    InboxSetupResult result;
    result.actual_endpoint = queue->actual_endpoint();
    result.schema_json     = serialize_inbox_spec_json(inbox_spec).dump();
    result.packing         = inbox_spec.packing;
    result.checksum        = config::checksum_policy_to_string(checksum_policy);
    result.queue           = std::move(queue);

    // Populate resolved fields on InboxConfig (single source of truth).
    inbox_cfg.schema_fields_json = result.schema_json;
    inbox_cfg.packing            = result.packing;
    inbox_cfg.checksum           = result.checksum;

    return result;
}

} // namespace pylabhub::scripting
