#pragma once
/**
 * @file script_host_schema.hpp
 * @brief Backward-compatibility redirect — types moved to schema_types.hpp.
 *
 * This header is DEPRECATED. Include schema_types.hpp directly for types
 * (FieldDef, SchemaSpec) or schema_field_layout.hpp for to_field_descs().
 *
 * Provides namespace alias: pylabhub::scripting::{FieldDef, SchemaSpec}
 * → pylabhub::hub::{FieldDef, SchemaSpec} for code that hasn't migrated yet.
 */

#include "utils/schema_types.hpp"
#include "utils/schema_field_layout.hpp"

namespace pylabhub::scripting
{
    using pylabhub::hub::FieldDef;
    using pylabhub::hub::SchemaSpec;
    using pylabhub::hub::SchemaFieldDesc;
    using pylabhub::hub::to_field_descs;
} // namespace pylabhub::scripting
