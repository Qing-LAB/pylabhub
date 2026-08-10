/**
 * @file admin_session.cpp
 * @brief Impl of the sealed admin-console session identity (HEP-CORE-0033
 *        §11.0.5).  Composed entirely from existing facilities — see the
 *        header docblock for the per-step delegation.
 */

#include "utils/admin_session.hpp"

#include "utils/format_tools.hpp" // bytes_to_hex / bytes_from_hex
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace pylabhub::admin
{
namespace
{
namespace sec = pylabhub::utils::security;
using json = nlohmann::json;

constexpr std::size_t kKeyBytes = sec::SecureSubsystem::kSecretboxKeyBytes;     // 32
constexpr std::size_t kNonceBytes = sec::SecureSubsystem::kSecretboxNonceBytes; // 24
constexpr std::size_t kMacBytes = sec::SecureSubsystem::kSecretboxMacBytes;     // 16

/// Serialize facts to the sealed plaintext.  Short keys keep the sealed id
/// compact; the format is private to this TU (only the hub ever reads it).
std::string serialize_facts(const AdminSessionFacts &f)
{
    return json{{"l", f.label}, {"p", f.peer_address}, {"r", f.routing_id}, {"t", f.issued_at_ms}}
        .dump();
}

std::optional<AdminSessionFacts> deserialize_facts(std::string_view pt)
{
    const json j = json::parse(pt, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("l") || !j.contains("p") || !j.contains("r") ||
        !j.contains("t"))
        return std::nullopt;
    if (!j["l"].is_string() || !j["p"].is_string() || !j["r"].is_string() ||
        !j["t"].is_number_unsigned())
        return std::nullopt;
    AdminSessionFacts f;
    f.label = j["l"].get<std::string>();
    f.peer_address = j["p"].get<std::string>();
    f.routing_id = j["r"].get<std::string>();
    f.issued_at_ms = j["t"].get<std::uint64_t>();
    return f;
}

} // namespace

void ensure_session_seal_key()
{
    auto &ks = sec::secure().keys();
    if (ks.has(kAdminSessionSealKeyName))
        return;
    // Minted straight into locked memory.  This previously generated
    // into a stack array, copied it in with `add_raw`, then wiped the
    // array — three steps during which the key existed in ordinary
    // memory the OS could page to disk before the wipe.  `add_random_key`
    // has no such window: there is no source buffer to page out.
    ks.add_random_key(kAdminSessionSealKeyName, kKeyBytes);
}

std::string seal_session_id(const AdminSessionFacts &facts)
{
    const std::string pt = serialize_facts(facts);

    // Sealed blob = nonce(24) || MAC(16) || ciphertext.  This function
    // used to assemble that layout by hand — generate a nonce, fetch the
    // key with `lookup_raw`, call the raw-key `secretbox_encrypt`, then
    // memcpy the two pieces together.  `secretbox_encrypt_using` emits
    // exactly the same bytes, so the sealed-id format is unchanged, and
    // the key never leaves the security module.
    std::string blob;
    blob.resize(pt.size() + sec::SecureSubsystem::kSealedOverheadBytes);

    std::size_t written = 0;
    try
    {
        written = sec::secure().secretbox_encrypt_using(
            kAdminSessionSealKeyName,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pt.data()),
                                          pt.size()),
            std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(blob.data()), blob.size()));
    }
    catch (const std::out_of_range &)
    {
        // Absent key throws; the previous `lookup_raw` shape reported it
        // as an empty return.  Preserve that contract — callers treat an
        // empty string as "could not seal", and a throw here would
        // escape into the admin request path.
        LOGGER_ERROR("[admin_session] seal: sealing key absent — "
                     "ensure_session_seal_key() must run first");
        return {};
    }
    if (written != blob.size())
    {
        LOGGER_ERROR("[admin_session] seal: secretbox_encrypt_using failed");
        return {};
    }

    // Ciphertext is not secret (it is handed to the operator), so hex via
    // format_tools is fine.
    return pylabhub::format_tools::bytes_to_hex(blob);
}

std::optional<AdminSessionFacts> open_session_id(std::string_view sealed_hex)
{
    // format_tools::bytes_from_hex returns the input unchanged on invalid hex;
    // detect that strictly: valid hex is even-length and decodes to half size.
    if (sealed_hex.size() % 2 != 0)
        return std::nullopt;
    const std::string blob = pylabhub::format_tools::bytes_from_hex(sealed_hex);
    if (blob.size() != sealed_hex.size() / 2)
        return std::nullopt; // decode failed (non-hex chars)
    if (blob.size() < kNonceBytes + kMacBytes)
        return std::nullopt; // too short to hold nonce + MAC

    // The whole blob, leading nonce included, goes to the named-key
    // operation — same bytes the hand-rolled path split apart.
    std::vector<std::uint8_t> pt(blob.size() - sec::SecureSubsystem::kSealedOverheadBytes);
    std::size_t got = 0;
    try
    {
        got = sec::secure().secretbox_decrypt_using(
            kAdminSessionSealKeyName,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(blob.data()),
                                          blob.size()),
            std::span<std::uint8_t>(pt.data(), pt.size()));
    }
    catch (const std::out_of_range &)
    {
        return std::nullopt; // no sealing key in this process
    }
    if (got == 0)
        return std::nullopt; // MAC failure: tampered, or foreign-instance key

    return deserialize_facts(std::string_view(reinterpret_cast<const char *>(pt.data()), got));
}

std::optional<AdminSessionFacts> verify_session_id(std::string_view sealed_hex,
                                                   std::string_view observed_peer_address,
                                                   std::string_view observed_routing_id)
{
    auto facts = open_session_id(sealed_hex);
    if (!facts)
        return std::nullopt;
    if (facts->peer_address != observed_peer_address || facts->routing_id != observed_routing_id)
        return std::nullopt; // replayed from a different connection
    return facts;
}

std::string origin_uid(const AdminSessionFacts &facts)
{
    return facts.label + "@" + facts.peer_address + "#" + std::to_string(facts.issued_at_ms);
}

} // namespace pylabhub::admin
