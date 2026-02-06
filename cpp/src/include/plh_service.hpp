#pragma once
/**
 * @file plh_service.hpp
 * @brief Layer 2: Service modules built on plh_base.
 *
 * Provides lifecycle management, file locking, and logging.
 * Include this when you need application lifecycle, FileLock, or Logger.
 */
#include "plh_base.hpp"

#include "utils/lifecycle.hpp"
#include "utils/file_lock.hpp"
#include "utils/logger.hpp"
