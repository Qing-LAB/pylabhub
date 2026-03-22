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
/// Schemas are discovered from the broker — consumer doesn't define them.
/// Currently empty; exists for future extensibility and type-safe role_data access.
struct ConsumerFields
{
    // Consumer discovers slot_schema and flexzone_schema from the broker at connect time.
    // No producer-defined schema fields needed here.
};

/// Parse consumer-specific fields from JSON.
inline std::any parse_consumer_fields(const nlohmann::json & /*j*/,
                                       const config::RoleConfig & /*cfg*/)
{
    return ConsumerFields{};
}

} // namespace pylabhub::consumer
