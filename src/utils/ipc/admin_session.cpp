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
    std::array<std::uint8_t, kKeyBytes> key{};
    sec::secure().random_bytes(std::span<std::uint8_t>(key.data(), key.size()));
    ks.add_raw(kAdminSessionSealKeyName,
               std::as_writable_bytes(std::span<std::uint8_t>(key.data(), key.size())));
    sec::secure().memzero(std::span<std::uint8_t>(key.data(), key.size()));
}

std::string seal_session_id(const AdminSessionFacts &facts)
{
    const std::string pt = serialize_facts(facts);

    std::array<std::uint8_t, kNonceBytes> nonce{};
    sec::secure().random_bytes(std::span<std::uint8_t>(nonce.data(), nonce.size()));

    std::vector<std::uint8_t> ct(pt.size() + kMacBytes);

    const auto keyspan = sec::secure().keys().lookup_raw(kAdminSessionSealKeyName);
    if (keyspan.size() != kKeyBytes)
    {
        LOGGER_ERROR("[admin_session] seal: sealing key absent/wrong size — "
                     "ensure_session_seal_key() must run first");
        return {};
    }

    const std::size_t written = sec::secure().secretbox_encrypt(
        ct.data(), ct.size(), reinterpret_cast<const std::uint8_t *>(pt.data()), pt.size(),
        std::span<const std::uint8_t, kNonceBytes>(nonce.data(), nonce.size()),
        std::span<const std::uint8_t, kKeyBytes>(
            reinterpret_cast<const std::uint8_t *>(keyspan.data()), kKeyBytes));
    if (written == 0)
    {
        LOGGER_ERROR("[admin_session] seal: secretbox_encrypt failed");
        return {};
    }

    // Sealed blob = nonce(24) || [MAC(16) || ciphertext].  Ciphertext is not
    // secret (it is handed to the operator), so hex via format_tools is fine.
    std::string blob;
    blob.resize(kNonceBytes + written);
    std::memcpy(blob.data(), nonce.data(), kNonceBytes);
    std::memcpy(blob.data() + kNonceBytes, ct.data(), written);
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

    const std::size_t ct_len = blob.size() - kNonceBytes;

    const auto keyspan = sec::secure().keys().lookup_raw(kAdminSessionSealKeyName);
    if (keyspan.size() != kKeyBytes)
        return std::nullopt;

    std::vector<std::uint8_t> pt(ct_len - kMacBytes);
    const std::size_t got = sec::secure().secretbox_decrypt(
        pt.data(), pt.size(), reinterpret_cast<const std::uint8_t *>(blob.data() + kNonceBytes),
        ct_len,
        std::span<const std::uint8_t, kNonceBytes>(
            reinterpret_cast<const std::uint8_t *>(blob.data()), kNonceBytes),
        std::span<const std::uint8_t, kKeyBytes>(
            reinterpret_cast<const std::uint8_t *>(keyspan.data()), kKeyBytes));
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
