#pragma once
/**
 * @file role_identity_policy.hpp
 * @brief Defines `KnownRole` — one entry in the hub's operator-managed
 *        known-roles allowlist.
 *
 * **History.**  This file formerly also carried the legacy
 * `RoleIdentityPolicy` enum + `ChannelPolicyOverride` — a self-asserted
 * string-identity gate (`BrokerServiceImpl::check_role_identity`) that
 * pre-dated the CURVE-required role model.  That gate was **redundant
 * with the ZAP pubkey allowlist** (HEP-CORE-0035 §4.5) and never
 * consulted the connecting socket's CURVE pubkey, so it was deleted per
 * HEP-CORE-0035 §8 Phase 6.  Production CURVE authentication is enforced
 * by the ZAP handler at the socket layer (HEP-0035 §4.1); see
 * `src/utils/security/zap_router.cpp` +
 * `KnownRolesStore::as_peer_allowlist`.
 *
 * `KnownRole` itself is **not** legacy: it is the live record type for
 * the vault-backed known-roles allowlist.  Its `pubkey_z85` field is the
 * cryptographic identity anchor the broker's ZAP handler matches every
 * CURVE handshake against.  (This is why HEP-0035 §8 Phase 6's original
 * "delete KnownRole" line does not apply — the type became load-bearing
 * once known_roles moved into the encrypted vault, §4.8.)
 *
 * TODO(followup): the filename still says "role_identity_policy"; it now
 * defines only `KnownRole` and should be renamed (e.g. `known_role.hpp`)
 * with its ~5 includers updated.
 */
#include "pylabhub_utils_export.h"

#include <string>

namespace pylabhub::broker
{

/// One entry in the hub's operator-managed known-roles allowlist.
///
/// `pubkey_z85` is the **cryptographic identity anchor** — the value the
/// broker's ZAP handler matches the connecting CURVE handshake against
/// (HEP-CORE-0035 §4.5 / §4.8).  The string fields name/uid/role are
/// human-readable bookkeeping used by `--list-known-roles` output and
/// operator logs; the ZAP gate keys on `pubkey_z85` alone.  Entries are
/// loaded from the encrypted hub vault at broker start (`HubConfig`) and
/// projected into the ZAP `PeerAllowlist` by
/// `KnownRolesStore::as_peer_allowlist`.
struct PYLABHUB_UTILS_EXPORT KnownRole
{
    std::string name;        ///< Role human name (e.g. "lab.daq.sensor1")
    std::string uid;         ///< Role UID string (e.g. "prod.sensor.uid12345678")
    std::string role;        ///< "producer", "consumer", or "any" (empty = "any")
    std::string pubkey_z85;  ///< CURVE public key (Z85, 40 chars).  Populated
                             ///< by `--add-known-role`; the ZAP allowlist anchor.
};

} // namespace pylabhub::broker
