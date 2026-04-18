#pragma once
/**
 * @file processor_init.hpp
 * @brief Processor role init-directory registration (HEP-CORE-0024 §10).
 */

#include "pylabhub_utils_export.h"

namespace pylabhub::processor
{

/// Register "processor" init content with RoleDirectory.
PYLABHUB_UTILS_EXPORT void register_processor_init();

/// Register "proc" runtime content (host factory + callbacks) with
/// RoleRegistry. Called once from main() so plh_role can dispatch on
/// role tag. Throws std::runtime_error if already registered.
PYLABHUB_UTILS_EXPORT void register_processor_runtime();

} // namespace pylabhub::processor
