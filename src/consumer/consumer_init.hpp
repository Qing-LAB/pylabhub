#pragma once
/**
 * @file consumer_init.hpp
 * @brief Consumer role init-directory registration (HEP-CORE-0024 §10).
 */

#include "pylabhub_utils_export.h"

namespace pylabhub::consumer
{

/// Register "consumer" init content with RoleDirectory.
PYLABHUB_UTILS_EXPORT void register_consumer_init();

/// Register "cons" runtime content (host factory + callbacks) with
/// RoleRegistry. Called once from main() so plh_role can dispatch on
/// role tag. Throws std::runtime_error if already registered.
PYLABHUB_UTILS_EXPORT void register_consumer_runtime();

} // namespace pylabhub::consumer
