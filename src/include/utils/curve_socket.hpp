#pragma once
/**
 * @file curve_socket.hpp
 * @brief Arm a ZMQ socket as a CURVE server from a KeyStore-held identity
 *        keypair (use-not-export).  HEP-CORE-0036 §7 + HEP-CORE-0043.
 *
 * Consolidates the `curve_server` arming sequence that was otherwise inlined
 * and copy-pasted across `broker_service` (ROUTER), `admin_service` (admin
 * socket), and `hub_inbox_queue` (inbox ROUTER).  Every CURVE-server socket
 * in the hub arms identically: `curve_server=1` + `curve_publickey` +
 * `curve_secretkey`, the secret flowing LockedKey → libzmq inside the
 * `with_seckey` callback and never materializing as a copy.
 *
 * This helper does ONLY the CURVE-server key arm.  The ZAP policy is the
 * caller's, because it differs by socket:
 *   - broker / inbox ROUTER: `zap_domain = "<domain>"` + `ZapRouter::
 *     register_domain(...)` (key-gated admission).
 *   - admin console: `zap_enforce_domain = 1` with an empty domain
 *     (crypto-only, token/session authority; off the HEP-CORE-0036 §7.4
 *     single-pumper).
 */

#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include <cppzmq/zmq.hpp>

#include <string_view>

namespace pylabhub::utils
{

/// Arm @p sock as a CURVE server keyed by the KeyStore identity named
/// @p identity_key_name (e.g. `security::kHubIdentityName`).  Sets
/// `curve_server` + `curve_publickey` + `curve_secretkey`; the secret is
/// read use-not-export inside `KeyStore::with_seckey` and never copied out.
/// Requires SecureSubsystem initialized and the named identity present.
/// The caller applies the ZAP policy afterwards (see the file docblock).
inline void arm_curve_server(zmq::socket_t   &sock,
                             std::string_view identity_key_name)
{
    namespace sec = pylabhub::utils::security;
    auto &ks = sec::secure().keys();
    sock.set(zmq::sockopt::curve_server, 1);
    sock.set(zmq::sockopt::curve_publickey, ks.pubkey(identity_key_name));
    ks.with_seckey(identity_key_name, [&](std::string_view seckey) {
        sock.set(zmq::sockopt::curve_secretkey, seckey);
    });
}

} // namespace pylabhub::utils
