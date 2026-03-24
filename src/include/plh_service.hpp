#pragma once
/**
 * @file plh_service.hpp
 * @brief Layer 2: Service modules built on plh_base.
 *
 * Provides lifecycle management, file locking, logging, cryptographic utilities,
 * backoff strategies, UID generation, ZMQ context, and interactive signal handling.
 * Include this when you need application lifecycle, FileLock, Logger, CryptoUtils,
 * UID/UUID utilities, InteractiveSignalHandler, or backoff strategies for spin loops.
 */
#include "plh_base.hpp"

#include "utils/backoff_strategy.hpp"
#include "utils/lifecycle.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/file_lock.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/uid_utils.hpp"
#include "utils/uuid_utils.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/zmq_context.hpp"
