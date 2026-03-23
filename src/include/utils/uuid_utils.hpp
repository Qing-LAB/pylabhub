#pragma once
/**
 * @file uuid_utils.hpp
 * @brief UUID4 generation and validation for hub and role identities.
 *
 * Used by `--init` flows for hub and role directories. Provides
 * cryptographically random UUID4 strings (RFC 4122, version 4) via
 * libsodium's `randombytes_buf()`.
 *
 * ## Design constraints
 *
 * - No dependency on JSON, ZMQ, Logger, or lifecycle.
 * - Safe to call before lifecycle startup — `sodium_init()` is called
 *   internally and is idempotent/thread-safe.
 * - Generated strings are always lowercase hex, 36 characters:
 *     `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`
 *
 * ## Usage
 *
 * @code{.cpp}
 *   const std::string uid = pylabhub::utils::generate_uuid4();
 *   // e.g. "e7a9f3b2-4c11-4a87-9f1d-c2d04b3e1a56"
 *   assert(pylabhub::utils::is_valid_uuid4(uid));
 * @endcode
 */

#include "pylabhub_utils_export.h"

#include <string>

namespace pylabhub::utils
{

/**
 * @brief Generate a UUID4 (random) string per RFC 4122.
 *
 * Format: `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` (36 chars, lowercase hex).
 * Entropy source: `randombytes_buf()` (libsodium, CSRNG).
 *
 * Thread-safe. Never returns an empty string.
 */
PYLABHUB_UTILS_EXPORT std::string generate_uuid4();

/**
 * @brief Return true if @p s is a well-formed UUID4 string.
 *
 * Checks:
 *   - Exactly 36 characters.
 *   - Dashes at positions 8, 13, 18, 23.
 *   - Hex digits at all other positions (upper or lower case).
 *   - Version nibble == '4' at position 14.
 *   - Variant nibble in {'8','9','a','b','A','B'} at position 19.
 */
PYLABHUB_UTILS_EXPORT bool is_valid_uuid4(const std::string &s);

} // namespace pylabhub::utils
