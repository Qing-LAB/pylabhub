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
 *   - Does NOT zeroize the secret on destruction.  That's HEP-CORE-0040
 *     (Locked Key Memory) territory: identity keypairs live in
 *     `LockedKey`-backed memory owned by the process-global `KeyStore`
 *     lifecycle module.  Production code does NOT construct or move
 *     bare `CurveKeypair` values around — it accesses identity keys
 *     via the use-not-export API (HEP-CORE-0040 §5.2 / §8.2):
 *     `key_store().pubkey(name)` returns a `std::string_view` into
 *     KeyStore-owned locked memory (non-secret half), and
 *     `key_store().with_seckey(name, callback)` invokes the callback
 *     with a `std::string_view` to the secret half — bytes never leave
 *     the LockedKey region.  The bare struct here is the simple
 *     value-type baseline used only by (a) the keygen utility below,
 *     (b) vault-create code paths that hand the two Z85 halves to
 *     `KeyStore::add_identity_from_z85` (which packs into a
 *     `SecureBuffer<80>` and zeroes the source), and (c) tests.  No
 *     call site should store a bare `CurveKeypair` for its lifetime.
 *   - Does NOT touch the vault layer.  Vault create/open is the
 *     persistence concern that wraps these keys with Argon2id KDF +
 *     XSalsa20-Poly1305 secretbox — see `hub_vault.hpp` /
 *     `role_vault.hpp`.
 *   - Does NOT distribute public keys.  HEP-CORE-0035 §4.8.3's
 *     operator workflow handles distribution via `--add-known-role`.
 */
#include "pylabhub_utils_export.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::utils::security
{

/// Strong-typed 40-character Z85-encoded CURVE PUBLIC key.
///
/// HEP-CORE-0036 §I10 + HEP-CORE-0040 §8.4 + AUTH_TODO §C2 — replaces
/// raw `std::string` carriage of pubkey-only fields (e.g.
/// `BRC::Config::broker_pubkey`, `ProducerPeer::pubkey_z85`,
/// `ProducerEntry::zmq_pubkey`, `ConsumerEntry::zmq_pubkey`, the
/// PULL-side serverkey parameter on `ZmqQueue::pull_from_*`).  The
/// invariant is enforced at construction: a `Z85PublicKey` is by
/// definition exactly 40 ASCII characters drawn from the Z85 alphabet
/// (RFC 32) — any other value is a programmer error rejected with a
/// loud exception at the construction site, NOT at the libzmq sockopt
/// boundary far downstream.
///
/// Construction shape — HEP-CORE-0040 §8.4.1.  There are exactly two
/// construction paths and they are disjoint:
///
///   1. `Z85PublicKey{}`              — Standby sentinel (40 internal
///                                       null bytes).  Used at the
///                                       HEP-CORE-0036 §6.7 Standby
///                                       rx-queue construction site
///                                       BEFORE producer_peers have
///                                       been delivered.  `empty()`
///                                       returns `true`.
///   2. `Z85PublicKey::validate(s)`   — validates `s` is exactly 40
///                                       Z85-alphabet chars and wraps.
///                                       Throws `std::invalid_argument`
///                                       on bad input.  Use at every
///                                       string → Z85PublicKey site
///                                       (config load / vault /
///                                       KnownRolesStore / wire
///                                       deserialization); `main()`
///                                       programs catch `std::exception`
///                                       at the phase boundary.
///
/// There is NO string-taking constructor.  A would-be writer of
/// `Z85PublicKey{maybe_empty_str}` is forced by the compiler to commit
/// to one of the two forms above, making the author intent visible at
/// every call site and removing the runtime ambiguity that the
/// previous-shape constructor created (the empty-string branch threw
/// instead of producing the Standby sentinel — bug class fixed
/// 2026-06-14, see HEP-CORE-0040 §8.4.1).
///
/// Pubkeys are PUBLIC by design (HEP-0040 §"pubkey memory placement"
/// review 2026-06-07): no `mlock`, no special zeroing, no callback
/// scope.  They travel on the wire and live in `known_roles.json` on
/// disk.  Confidentiality is irrelevant; integrity is the property
/// HEP-CORE-0035 §4.6 file ACLs protect.
class PYLABHUB_UTILS_EXPORT Z85PublicKey
{
public:
    /// Length of a Z85-encoded CURVE public key in ASCII chars.
    /// Same constant the libzmq sockopt expects.
    static constexpr std::size_t kZ85Chars = 40;

    /// Standby sentinel.  40 internal null bytes — distinguishable
    /// from any valid Z85 string (Z85 alphabet excludes `\0`) via
    /// `empty()`.  NOT a valid pubkey for any cryptographic operation.
    /// This is the ONE form of "unset" — there is no string-taking
    /// constructor that could produce a different "empty" instance.
    Z85PublicKey() noexcept;

    /// Static factory — validate-and-wrap.  Accepts exactly 40 chars
    /// from the Z85 alphabet (RFC 32 §4: `0-9 a-z A-Z .-:+=^!/*?&<>()[]{}@%$#`).
    /// Throws `std::invalid_argument` on bad input.  `main()` programs
    /// catch `std::exception` at every phase boundary and produce a
    /// clean exit-1 with a diagnostic.  See HEP-CORE-0040 §8.4.1.
    [[nodiscard]] static Z85PublicKey validate(std::string_view z85);

    Z85PublicKey(const Z85PublicKey &)            = default;
    Z85PublicKey(Z85PublicKey &&) noexcept        = default;
    Z85PublicKey &operator=(const Z85PublicKey &) = default;
    Z85PublicKey &operator=(Z85PublicKey &&) noexcept = default;
    ~Z85PublicKey()                               = default;

    /// View over the 40 Z85 chars.  Use to pass to libzmq sockopt
    /// (`zmq::sockopt::curve_serverkey` accepts string_view) without
    /// copying.  Lifetime tied to this instance.
    [[nodiscard]] std::string_view view() const noexcept { return z85_; }

    /// Underlying 40-byte ASCII storage.  Lifetime tied to this
    /// instance.
    [[nodiscard]] const std::string &str() const noexcept { return z85_; }

    /// `true` iff this is the default sentinel (40 zero bytes).
    /// Equivalent to "no pubkey set"; cryptographically invalid for
    /// any libzmq sockopt operation.
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] friend bool
    operator==(const Z85PublicKey &a, const Z85PublicKey &b) noexcept
    {
        return a.z85_ == b.z85_;
    }
    [[nodiscard]] friend bool
    operator!=(const Z85PublicKey &a, const Z85PublicKey &b) noexcept
    {
        return !(a == b);
    }

private:
    std::string z85_;
};

/// A ZMQ CURVE keypair — both members Z85-encoded ASCII (40 chars
/// each, no trailing null).  Trivially copyable; locked-memory
/// storage + zero-on-destruct is provided by
/// `KeyStore::add_identity_from_z85` (HEP-CORE-0040 §5).  Bare
/// value-typed instances exist only at the keygen / vault-decrypt
/// boundary; long-lived ownership belongs to KeyStore.
struct CurveKeypair
{
    std::string public_z85;
    std::string secret_z85;
};

/// Generate a fresh ZMQ CURVE keypair via libzmq's wrapper around
/// libsodium.  Cost: ~100 μs.  Throws `std::runtime_error` on
/// failure (does not happen in normal operation — libzmq's
/// `zmq_curve_keypair` only fails if its CSPRNG init fails).
[[nodiscard]] PYLABHUB_UTILS_EXPORT
CurveKeypair generate_curve_keypair();

} // namespace pylabhub::utils::security
