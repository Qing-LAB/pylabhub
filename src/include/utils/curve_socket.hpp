#pragma once
/**
 * @file curve_socket.hpp
 * @brief Arm a ZMQ socket for CURVE — server or client — from a KeyStore-held
 *        identity keypair (use-not-export).  HEP-CORE-0036 §7 + HEP-CORE-0043.
 *
 * Consolidates the `curve_server` arming sequence that was otherwise inlined
 * and copy-pasted across `broker_service` (ROUTER), `admin_service` (admin
 * socket), and `hub_inbox_queue` (inbox ROUTER).  Every CURVE-server socket
 * in the hub arms identically: `curve_server=1` + `curve_publickey` +
 * `curve_secretkey`, the secret flowing LockedKey → libzmq inside the
 * `with_seckey` callback and never materializing as a copy.
 *
 * `arm_curve_client` is the dialing mirror; it additionally pins the peer's
 * pubkey, which is what makes the connection authenticated rather than merely
 * encrypted.  Both exist so no site writes the sockopt sequence out by hand —
 * an incomplete sequence does not fail loudly, it silently downgrades (see the
 * mechanism-selection note on `arm_curve_client`).
 *
 * The server helper does ONLY the CURVE-server key arm.  The ZAP policy is the
 * caller's, because it differs by socket:
 *   - broker / inbox ROUTER: `zap_domain = "<domain>"` + `ZapRouter::
 *     register_domain(...)` (key-gated admission).
 *   - admin console: `zap_enforce_domain = 1` with an empty domain
 *     (crypto-only, token/session authority; off the HEP-CORE-0036 §7.4
 *     single-pumper).
 */

#include "utils/debug_info.hpp" // PLH_PANIC — unpinned-peer refusal
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
inline void arm_curve_server(zmq::socket_t &sock, std::string_view identity_key_name)
{
    namespace sec = pylabhub::utils::security;
    auto &ks = sec::secure().keys();
    sock.set(zmq::sockopt::curve_server, 1);
    sock.set(zmq::sockopt::curve_publickey, ks.pubkey(identity_key_name));
    ks.with_seckey(identity_key_name, [&](std::string_view seckey)
                   { sock.set(zmq::sockopt::curve_secretkey, seckey); });
}

/// Arm @p sock as a CURVE client: present our own identity keypair and PIN the
/// peer we are willing to talk to via @p server_pubkey_z85.
///
/// The mirror of `arm_curve_server`, and it exists for the same reason: the
/// three-sockopt sequence was written out separately at every dialing site, so
/// each site had its own opportunity to get the set incomplete.  That is not
/// hypothetical — an incomplete set does not fail loudly.  libzmq picks the
/// mechanism from whichever curve options were set (`options.cpp`
/// `set_curve_key` flips `mechanism = ZMQ_CURVE` on ANY of them), so:
///   - all three set        → a real CURVE client;
///   - publickey/secretkey but NO serverkey → still CURVE, but handshaking
///     against a zero key: the connection silently never establishes;
///   - NONE set             → mechanism stays ZMQ_NULL, i.e. PLAINTEXT, and
///     the socket connects and sends perfectly happily.
///
/// The last case is why `server_pubkey_z85` is required rather than optional.
/// A dialing socket with no pinned peer key has nothing to authenticate
/// against; there is no useful "connect anyway and see" mode
/// (HEP-CORE-0035 §2).  Passing an empty key is a programmer error and
/// aborts, matching `ZmqQueue::start` / `InboxQueue::start`.
inline void arm_curve_client(zmq::socket_t &sock, std::string_view identity_key_name,
                             std::string_view server_pubkey_z85)
{
    namespace sec = pylabhub::utils::security;
    if (server_pubkey_z85.empty())
    {
        PLH_PANIC("arm_curve_client: empty server_pubkey for identity '{}' — a dialing "
                  "socket with no pinned peer key cannot authenticate anyone.  Leaving it "
                  "unset does not fail closed: libzmq would keep the socket on the NULL "
                  "mechanism and connect in PLAINTEXT (HEP-CORE-0035 §2).",
                  identity_key_name);
    }
    auto &ks = sec::secure().keys();
    sock.set(zmq::sockopt::curve_serverkey, server_pubkey_z85);
    sock.set(zmq::sockopt::curve_publickey, ks.pubkey(identity_key_name));
    ks.with_seckey(identity_key_name, [&](std::string_view seckey)
                   { sock.set(zmq::sockopt::curve_secretkey, seckey); });
}

} // namespace pylabhub::utils
