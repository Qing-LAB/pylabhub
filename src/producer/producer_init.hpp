#pragma once
/**
 * @file producer_init.hpp
 * @brief Producer role init-directory registration (HEP-CORE-0024 §10).
 *
 * Registers the producer's config JSON template and starter Python script
 * template with RoleDirectory. Called once from main() before parsing args.
 */

#include "pylabhub_utils_export.h"

namespace pylabhub::producer
{

/// Register "producer" init content with RoleDirectory.
/// Safe to call multiple times — second call overwrites the first.
PYLABHUB_UTILS_EXPORT void register_producer_init();

} // namespace pylabhub::producer
