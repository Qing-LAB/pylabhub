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

} // namespace pylabhub::consumer
