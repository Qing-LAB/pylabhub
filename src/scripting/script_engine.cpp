/**
 * @file script_engine.cpp
 * @brief ScriptEngine non-virtual method implementations.
 *
 * open_inbox_client() is the single implementation for inbox connection
 * management. Both Lua and Python engines call this; each wraps the
 * result in its script-native handle format.
 */
#include "utils/script_engine.hpp"

#include "script_host_helpers.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "utils/messenger.hpp"

namespace pylabhub::scripting
{

std::optional<ScriptEngine::InboxOpenResult>
ScriptEngine::open_inbox_client(const std::string &target_uid)
{
    if (!ctx_.core || !ctx_.messenger)
        return std::nullopt;

    // Cache hit — return existing client with stored metadata.
    auto cached = ctx_.core->get_inbox_entry(target_uid);
    if (cached && cached->client && cached->client->is_running())
    {
        // Reconstruct minimal result from cache. Schema fields are not
        // stored in core_ (only type_name + item_size), but the client
        // is ready to use. Engines that need the full schema for type
        // building will have already built the type on the first call.
        return InboxOpenResult{
            cached->client, {}, "", cached->item_size};
    }

    // Broker round-trip — discover target's inbox endpoint and schema.
    auto info = ctx_.messenger->query_role_info(target_uid, /*timeout_ms=*/1000);
    if (!info.has_value())
        return std::nullopt;

    if (!info->inbox_schema.is_object() || !info->inbox_schema.contains("fields"))
        return std::nullopt;

    SchemaSpec spec;
    try
    {
        spec = parse_schema_json(info->inbox_schema);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("[engine] open_inbox('{}'): schema parse error: {}",
                    target_uid, e.what());
        return std::nullopt;
    }

    // Compute item size from schema fields.
    size_t item_size = 0;
    for (const auto &f : spec.fields)
        item_size += f.length;

    auto zmq_fields = schema_spec_to_zmq_fields(spec, item_size);

    auto client_ptr = hub::InboxClient::connect_to(
        info->inbox_endpoint, ctx_.uid, std::move(zmq_fields), info->inbox_packing);
    if (!client_ptr)
    {
        LOGGER_WARN("[engine] open_inbox('{}'): connect_to '{}' failed",
                    target_uid, info->inbox_endpoint);
        return std::nullopt;
    }
    if (!client_ptr->start())
    {
        LOGGER_WARN("[engine] open_inbox('{}'): start() failed", target_uid);
        return std::nullopt;
    }

    auto shared_client = std::shared_ptr<hub::InboxClient>(std::move(client_ptr));

    // Store in core_ (shared across all engine states).
    ctx_.core->set_inbox_entry(target_uid,
        {shared_client, "InboxSlot", item_size});

    return InboxOpenResult{
        std::move(shared_client), std::move(spec),
        info->inbox_packing, item_size};
}

} // namespace pylabhub::scripting
