#pragma once
/**
 * @file admin_session.hpp
 * @brief Admin operator-console session identity — a sealed, connection-bound
 *        credential (HEP-CORE-0033 §11.0.5).
 *
 * The operator authenticates ONCE at console establishment (admin token,
 * §11.3).  On success the hub mints a **session id** that is returned to the
 * operator and presented on every subsequent message in place of the raw
 * token.  The session id is sealed with the hub's per-instance key so it is
 * opaque to everyone but this hub instance, and it embeds the connection
 * facts so a message replayed from a different connection is rejected.
 *
 * This translation unit is intentionally transport-agnostic: it mints and
 * verifies the opaque credential and derives the provenance stamp.  Whether
 * the credential travels in a typed WireEnvelope body or elsewhere is the
 * console transport's concern (§11.1), not this module's.
 *
 * COMPOSED FROM EXISTING INFRASTRUCTURE — this module adds NO new crypto,
 * encoding, or key-storage primitive.  Each step delegates to an existing,
 * HEP-owned facility:
 *   - seal / unseal   → `secure().secretbox_encrypt/decrypt` + `random_bytes`
 *                       (SecureSubsystem, HEP-CORE-0043 §Cat-1a/1c).
 *   - per-instance key → KeyStore `add_raw` + use-not-export accessor
 *                       (KeyStore, HEP-CORE-0040 §5).
 *   - hex encode/decode → `format_tools::bytes_to_hex` / `bytes_from_hex`.
 *   - constant-time compare / scrub → `secure().memcmp_ct` / `memzero`.
 *
 * Files:
 *   - Header: `src/include/utils/admin_session.hpp` (this file).
 *   - Impl:   `src/utils/ipc/admin_session.cpp` (groups with
 *             `admin_service.cpp`).
 */

#include "pylabhub_utils_export.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::admin
{

/// Connection facts sealed into a session id (HEP-CORE-0033 §11.0.5).  All
/// fields except `label` are hub-observed at establishment (never
/// client-claimed) so the per-message fact check cannot be spoofed.
struct AdminSessionFacts
{
    std::string   label;         ///< Operator-supplied, human-readable (e.g. "alice-laptop").
    std::string   peer_address;  ///< Hub-observed ZMQ "Peer-Address" (peer IP;
                                 ///< libzmq surfaces the address, not the port).
    std::string   routing_id;    ///< Hub-observed ROUTER routing identity of the console.
    std::uint64_t issued_at_ms;  ///< Hub wall clock at establishment.
};

/// KeyStore entry name for the hub's per-instance session-sealing key.  The
/// key is ephemeral to the hub instance: minted at first use, discarded on
/// restart — which naturally invalidates every outstanding session id.
inline constexpr std::string_view kAdminSessionSealKeyName = "admin.session.seal";

/// Mint the per-instance sealing key into the KeyStore if absent; idempotent.
/// 32 random bytes via `secure().random_bytes` → `KeyStore::add_raw`; the
/// source buffer is scrubbed.  Requires SecureSubsystem to be initialized.
/// Call once during admin-console startup, before minting any session id.
PYLABHUB_UTILS_EXPORT void ensure_session_seal_key();

/// Seal @p facts into an opaque, connection-bound session id: JSON(facts)
/// encrypted+authenticated under the per-instance key (`secretbox`) with a
/// fresh random nonce, then hex-encoded.  Only this hub instance can open it.
/// Requires `ensure_session_seal_key()` to have run.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
seal_session_id(const AdminSessionFacts &facts);

/// Open a sealed session id.  Returns the embedded facts, or `std::nullopt`
/// if the input is not valid hex, is truncated, was tampered with, or was
/// sealed by a different hub instance / key (MAC failure).  No fact matching
/// is performed here — see `verify_session_id`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<AdminSessionFacts>
open_session_id(std::string_view sealed_hex);

/// Verify a session id against the facts a message ACTUALLY arrived with.
/// Opens the seal AND checks that the embedded `peer_address` and
/// `routing_id` equal the observed values.  (The seal's AEAD tag is the
/// integrity boundary; the fact values are not secret, so the equality
/// check need not be constant-time.)  Returns the opened facts on success —
/// the caller uses `origin_uid(facts)` as the provenance stamp (§11.0.5) —
/// or `std::nullopt` on any open failure or fact mismatch (tampered,
/// foreign-instance, or replayed from another connection).
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<AdminSessionFacts>
verify_session_id(std::string_view sealed_hex,
                  std::string_view observed_peer_address,
                  std::string_view observed_routing_id);

/// Provenance stamp for logs + `sender_uid` on actuated NOTIFYs (§11.0.5).
/// Human-readable and session-unique: `"<label>@<peer_address>#<issued_at_ms>"`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
origin_uid(const AdminSessionFacts &facts);

} // namespace pylabhub::admin
