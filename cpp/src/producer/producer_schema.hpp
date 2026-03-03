#pragma once
/**
 * @file producer_schema.hpp
 * @brief Slot and flexzone schema type definitions for pylabhub-producer.
 *
 * Thin alias layer — imports shared types from scripting::* into the
 * pylabhub::producer namespace.
 */

#include "utils/script_host_schema.hpp"

namespace pylabhub::producer
{

using scripting::SlotExposure;
using scripting::FieldDef;
using scripting::SchemaSpec;

} // namespace pylabhub::producer
