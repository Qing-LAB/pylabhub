/**
 * @file auth_config.cpp
 * @brief config::AuthConfig::load_keypair() — shared vault decryption.
 */
#include "utils/config/auth_config.hpp"
#include "utils/actor_vault.hpp"

#include <cstdio>
#include <filesystem>

namespace pylabhub::config
{

bool AuthConfig::load_keypair(const std::string &uid,
                               const std::string &password,
                               const char *role_tag)
{
    if (keyfile.empty())
        return false;

    if (!std::filesystem::exists(keyfile))
    {
        std::fprintf(stderr,
                     "[%s] auth.keyfile '%s': not found — using ephemeral CURVE identity\n",
                     role_tag, keyfile.c_str());
        return false;
    }

    const auto vault = pylabhub::utils::ActorVault::open(keyfile, uid, password);
    client_pubkey = vault.public_key();
    client_seckey = vault.secret_key();
    std::fprintf(stderr, "[%s] Loaded vault from '%s' (pubkey: %.8s...)\n",
                 role_tag, keyfile.c_str(), vault.public_key().c_str());
    return true;
}

} // namespace pylabhub::config
