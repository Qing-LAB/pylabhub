#pragma once
/**
 * @file consumer_schema.hpp
 * @brief Slot and flexzone schema type definitions for pylabhub-consumer.
 *
 * Thin alias layer — imports shared types from scripting::* into the
 * pylabhub::consumer namespace.
 */

#include "utils/script_host_schema.hpp"

namespace pylabhub::consumer
{

using scripting::SlotExposure;
using scripting::FieldDef;
using scripting::SchemaSpec;

} // namespace pylabhub::consumer
