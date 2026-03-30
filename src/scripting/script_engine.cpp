/**
 * @file script_engine.cpp
 * @brief ScriptEngine non-virtual method implementations.
 *
 * open_inbox_client() is the single implementation for inbox connection
 * management. Uses RoleHostCore::open_inbox() for atomic
 * check-and-create under a single mutex — no TOCTOU race.
 */
#include "utils/script_engine.hpp"

#include "script_host_helpers.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/config/checksum_config.hpp"
#include "utils/logger.hpp"
#include "utils/messenger.hpp"

namespace pylabhub::scripting
{

std::optional<ScriptEngine::InboxOpenResult>
ScriptEngine::open_inbox_client(const std::string &target_uid)
{
    if (!ctx_.core || !ctx_.messenger)
        return std::nullopt;

    // Atomic check-and-create: the mutex in core_ is held for the entire
    // operation. If the entry exists and is running, returns it. Otherwise,
    // the creator lambda runs (broker query + InboxClient creation) under
    // the lock, then the result is stored. No race between concurrent
    // callers for the same target_uid.

    SchemaSpec result_spec;
    std::string result_packing;

    auto entry = ctx_.core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            auto info = ctx_.messenger->query_role_info(target_uid, 1000);
            if (!info.has_value())
                return std::nullopt;

            if (!info->inbox_schema.is_object() ||
                !info->inbox_schema.contains("fields"))
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

            size_t item_size = 0;
            for (const auto &f : spec.fields)
                item_size += f.length;

            auto zmq_fields = schema_spec_to_zmq_fields(spec, item_size);

            auto client_ptr = hub::InboxClient::connect_to(
                info->inbox_endpoint, ctx_.uid,
                std::move(zmq_fields), info->inbox_packing);
            if (!client_ptr)
            {
                LOGGER_WARN("[engine] open_inbox('{}'): connect failed",
                            target_uid);
                return std::nullopt;
            }
            if (!client_ptr->start())
            {
                LOGGER_WARN("[engine] open_inbox('{}'): start failed",
                            target_uid);
                return std::nullopt;
            }
            // Use the inbox OWNER's checksum policy (from ROLE_INFO_ACK), not our own.
            client_ptr->set_checksum_policy(
                config::string_to_checksum_policy(info->inbox_checksum));

            result_spec = std::move(spec);
            result_packing = info->inbox_packing;

            return RoleHostCore::InboxCacheEntry{
                std::shared_ptr<hub::InboxClient>(std::move(client_ptr)),
                "InboxSlot", item_size};
        });

    if (!entry)
        return std::nullopt;

    return InboxOpenResult{
        entry->client, std::move(result_spec),
        std::move(result_packing), entry->item_size};
}

} // namespace pylabhub::scripting
