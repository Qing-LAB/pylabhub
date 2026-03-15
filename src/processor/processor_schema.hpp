#pragma once
/**
 * @file processor_schema.hpp
 * @brief Slot and flexzone schema type definitions for pylabhub-processor.
 *
 * Thin alias layer — imports shared types from scripting::* into the
 * pylabhub::processor namespace.
 */

#include "utils/script_host_schema.hpp"

namespace pylabhub::processor
{

using scripting::SlotExposure;
using scripting::FieldDef;
using scripting::SchemaSpec;

} // namespace pylabhub::processor
