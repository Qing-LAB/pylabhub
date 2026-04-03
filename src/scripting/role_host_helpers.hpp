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

#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/hub_inbox_queue.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// drain_inbox_sync — drain all inbox messages non-blocking
// ============================================================================

/**
 * @brief Drain all pending inbox messages and invoke the engine's on_inbox handler.
 *
 * Called from the data loop right before the main script callback (Step C).
 * All messages are processed in FIFO order — no dropping.
 *
 * The engine uses its cached InboxFrame type (registered at startup via
 * register_slot_type) to build typed views. No type name is passed per call.
 *
 * @param inbox_queue   The inbox queue (may be nullptr if no inbox configured).
 * @param engine        The script engine for invoking on_inbox.
 */
inline void drain_inbox_sync(hub::InboxQueue *inbox_queue,
                              ScriptEngine    *engine)
{
    if (inbox_queue == nullptr || engine == nullptr)
        return;

    while (true)
    {
        const auto *item =
            inbox_queue->recv_one(std::chrono::milliseconds{0}); // non-blocking
        if (item == nullptr)
            break;

        engine->invoke_on_inbox(InvokeInbox{
            item->data, inbox_queue->item_size(),
            item->sender_id, item->seq});

        // Always ack success — don't drop inbox messages on script errors.
        inbox_queue->send_ack(0);
    }
}

// ============================================================================
// wait_for_roles — HEP-0023 startup coordination
// ============================================================================

/**
 * @brief Wait for required peer roles to appear in the broker before continuing.
 *
 * Blocks for up to timeout_ms per role. Logs progress.
 *
 * @param messenger     The messenger to query role presence.
 * @param wait_list     List of roles to wait for (from config).
 * @param log_tag       Log prefix, e.g. "[prod]".
 * @return true if all roles found; false on timeout.
 */
inline bool wait_for_roles(hub::Messenger                   &messenger,
                            const std::vector<WaitForRole>   &wait_list,
                            const char                       *log_tag)
{
    static constexpr int kPollMs = 200;

    for (const auto &wr : wait_list)
    {
        LOGGER_INFO("{} Startup: waiting for role '{}' (timeout {}ms)...",
                    log_tag, wr.uid, wr.timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{wr.timeout_ms};
        bool found = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                break;
            const int poll_ms = static_cast<int>(std::min<long long>(kPollMs, remaining));
            if (messenger.query_role_presence(wr.uid, poll_ms))
            {
                found = true;
                break;
            }
        }
        if (!found)
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
// serialize_inbox_spec_json — inbox schema JSON for broker REG_REQ
// ============================================================================

/**
 * @brief Serialize an inbox hub::SchemaSpec into the JSON format expected by
 * ProducerOptions/ConsumerOptions for broker registration.
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
    return result;
}

} // namespace pylabhub::scripting
