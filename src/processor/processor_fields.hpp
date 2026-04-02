#pragma once
/**
 * @file processor_fields.hpp
 * @brief ProcessorFields — role-specific config fields for the processor.
 *
 * Contains only fields not covered by RoleConfig's common/directional categories.
 * Used as the role_data<ProcessorFields>() extension in RoleConfig.
 */

#include "utils/config/role_config.hpp"

#include <any>
#include <stdexcept>
#include <string>

namespace pylabhub::processor
{

/// Processor-specific config fields.
/// Input schema is discovered from the broker. Output schema is defined here.
/// Flexzone schema is shared (applies to output side).
struct ProcessorFields
{
    nlohmann::json in_slot_schema_json;         ///< Required. Input slot schema.
    nlohmann::json out_slot_schema_json;        ///< Required. Output slot schema.
    nlohmann::json in_flexzone_schema_json;     ///< Optional. Input flexzone schema (from upstream).
    nlohmann::json out_flexzone_schema_json;    ///< Optional. Output flexzone schema.
};

/// Parse processor-specific fields from JSON.
/// Common + directional fields are already populated in cfg.
inline std::any parse_processor_fields(const nlohmann::json &j,
                                        const config::RoleConfig & /*cfg*/)
{
    ProcessorFields pf;
    pf.in_slot_schema_json = j.value("in_slot_schema", nlohmann::json{});
    pf.out_slot_schema_json = j.value("out_slot_schema", nlohmann::json{});
    pf.in_flexzone_schema_json = j.value("in_flexzone_schema", nlohmann::json{});
    pf.out_flexzone_schema_json = j.value("out_flexzone_schema", nlohmann::json{});

    if (pf.out_slot_schema_json.is_null() || pf.out_slot_schema_json.empty())
        throw std::runtime_error("processor: 'out_slot_schema' is required");

    return pf;
}

} // namespace pylabhub::processor
