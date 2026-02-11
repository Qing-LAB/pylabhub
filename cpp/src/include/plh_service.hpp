#pragma once
/**
 * @file plh_service.hpp
 * @brief Layer 2: Service modules built on plh_base.
 *
 * Provides lifecycle management, file locking, logging, cryptographic utilities,
 * and backoff strategies for concurrency primitives.
 * Include this when you need application lifecycle, FileLock, Logger, CryptoUtils,
 * or backoff strategies for spin loops.
 */
#include "plh_base.hpp"

#include "utils/backoff_strategy.hpp"
#include "utils/lifecycle.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/file_lock.hpp"
#include "utils/logger.hpp"
