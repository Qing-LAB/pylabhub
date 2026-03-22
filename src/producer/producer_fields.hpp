#pragma once
/**
 * @file producer_fields.hpp
 * @brief ProducerFields — role-specific config fields for the producer.
 *
 * Contains only fields not covered by RoleConfig's common/directional categories.
 * Used as the role_data<ProducerFields>() extension in RoleConfig.
 */

#include "utils/config/role_config.hpp"

#include <any>
#include <stdexcept>
#include <string>

namespace pylabhub::producer
{

/// Producer-specific config fields.
/// Schema definitions are producer-only — consumers discover them from the broker.
struct ProducerFields
{
    nlohmann::json out_slot_schema_json;      ///< Required. Output slot schema.
    nlohmann::json out_flexzone_schema_json;   ///< Optional. Flexzone schema (null = no flexzone).
};

/// Parse producer-specific fields from JSON.
/// Common fields are already populated in cfg.
inline std::any parse_producer_fields(const nlohmann::json &j,
                                       const config::RoleConfig & /*cfg*/)
{
    ProducerFields pf;
    pf.out_slot_schema_json = j.value("out_slot_schema", nlohmann::json{});
    pf.out_flexzone_schema_json = j.value("out_flexzone_schema", nlohmann::json{});

    if (pf.out_slot_schema_json.is_null() || pf.out_slot_schema_json.empty())
        throw std::runtime_error("producer: 'out_slot_schema' is required");

    return pf;
}

} // namespace pylabhub::producer
