#pragma once
/**
 * @file schema_utils.hpp (src/scripting/ — REDIRECT)
 * @brief Moved to src/include/utils/schema_utils.hpp.
 *
 * This redirect exists for backward compatibility during migration.
 * Include "utils/schema_utils.hpp" directly in new code.
 */
#include "utils/schema_utils.hpp"

// Bring hub:: names into scripting:: for backward compatibility.
namespace pylabhub::scripting
{
    // Types (moved from script_host_schema.hpp)
    using pylabhub::hub::FieldDef;
    using pylabhub::hub::SchemaSpec;
    using pylabhub::hub::SchemaFieldDesc;
    using pylabhub::hub::to_field_descs;
    // Functions (moved from this file's original location)
    using pylabhub::hub::parse_schema_json;
    using pylabhub::hub::resolve_schema;
    using pylabhub::hub::resolve_named_schema;
    using pylabhub::hub::schema_entry_to_spec;
    using pylabhub::hub::schema_spec_to_zmq_fields;
    using pylabhub::hub::compute_schema_hash;
    using pylabhub::hub::compute_schema_size;
    using pylabhub::hub::append_schema_canonical;
} // namespace pylabhub::scripting
