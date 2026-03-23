#pragma once
/**
 * @file consumer_fields.hpp
 * @brief ConsumerFields — role-specific config fields for the consumer.
 *
 * Contains only fields not covered by RoleConfig's common/directional categories.
 * Used as the role_data<ConsumerFields>() extension in RoleConfig.
 */

#include "utils/config/role_config.hpp"

#include <any>
#include <string>

namespace pylabhub::consumer
{

/// Consumer-specific config fields.
/// Input schemas are optional — when present they are used for ctypes struct
/// building at startup (before broker discovery).
struct ConsumerFields
{
    nlohmann::json in_slot_schema_json;      ///< Optional. Input slot schema for ctypes.
    nlohmann::json in_flexzone_schema_json;  ///< Optional. Flexzone schema (null = no flexzone).
};

/// Parse consumer-specific fields from JSON.
inline std::any parse_consumer_fields(const nlohmann::json &j,
                                       const config::RoleConfig & /*cfg*/)
{
    ConsumerFields cf;
    cf.in_slot_schema_json     = j.value("in_slot_schema",     nlohmann::json{});
    cf.in_flexzone_schema_json = j.value("in_flexzone_schema", nlohmann::json{});
    return cf;
}

} // namespace pylabhub::consumer
