#pragma once
/**
 * @file curve_keypair.hpp
 * @brief ZMQ CURVE keypair value type + generation utility.
 *
 * Z85 ("Z85 — encoding" per the ZMQ RFC 32) is the canonical 5-of-4-bytes
 * ASCII encoding for the 32-byte CURVE keys libzmq accepts on
 * `zmq::sockopt::curve_publickey` / `curve_secretkey`.  A keypair is
 * thus a pair of 40-character ASCII strings.
 *
 * Background — why a struct instead of two parallel strings:
 *
 * Prior to this consolidation, four production sites generated CURVE
 * keypairs inline, each with its own buffer-and-copy idiom:
 *
 *   - `BrokerService::run()` — broker auto-keygen when no operator
 *     keys are configured (broker_service.cpp).
 *   - `BrokerRequestComm::connect()` — BRC auto-keygen for unattended
 *     clients (broker_request_comm.cpp).
 *   - `HubVault::create()` — operator-driven `--keygen` for the hub
 *     vault.
 *   - `RoleVault::create()` — operator-driven `--keygen` for the role
 *     vault.
 *
 * All four use the same `zmq_curve_keypair(char[41], char[41])` C API
 * + the same 40-of-41 `std::string` conversion.  This file is the
 * shared utility they all call.
 *
 * Scope — what this file does NOT do:
 *
 *   - Does NOT zeroize the secret on destruction.  That's HEP-CORE-0035
 *     §4.7 runtime-key-handling territory (task #102), which adds the
 *     `RuntimeSecret` wrapper for in-process key lifetime.  Callers
 *     that need zeroization wrap a `CurveKeypair` in `RuntimeSecret`
 *     (or the equivalent) at the boundary; the bare struct here is the
 *     simple value-type baseline.
 *   - Does NOT touch the vault layer.  Vault create/open is the
 *     persistence concern that wraps these keys with Argon2id KDF +
 *     XSalsa20-Poly1305 secretbox — see `hub_vault.hpp` /
 *     `role_vault.hpp`.
 *   - Does NOT distribute public keys.  HEP-CORE-0035 §4.8.3's
 *     operator workflow handles distribution via `--add-known-role`.
 */
#include "pylabhub_utils_export.h"

#include <string>

namespace pylabhub::utils::security
{

/// A ZMQ CURVE keypair — both members Z85-encoded ASCII (40 chars
/// each, no trailing null).  Trivially copyable; secret-side
/// zeroization is the caller's responsibility (see HEP-CORE-0035
/// §4.7 / task #102 for the zeroizing wrapper when that lands).
struct CurveKeypair
{
    std::string public_z85;
    std::string secret_z85;

    [[nodiscard]] bool empty() const noexcept
    {
        return public_z85.empty() && secret_z85.empty();
    }
};

/// Generate a fresh ZMQ CURVE keypair via libzmq's wrapper around
/// libsodium.  Cost: ~100 μs.  Throws `std::runtime_error` on
/// failure (does not happen in normal operation — libzmq's
/// `zmq_curve_keypair` only fails if its CSPRNG init fails).
[[nodiscard]] PYLABHUB_UTILS_EXPORT
CurveKeypair generate_curve_keypair();

} // namespace pylabhub::utils::security
