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

} // namespace pylabhub::processor
