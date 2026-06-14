/**
 * @file domain_routing_table.cpp
 * @brief Implementation of DomainRoutingTable — public/free factories +
 *        log routing for the inline template `with_admission`.
 *
 * The lock-bounded callback shape lives in the header so the compiler
 * inlines per-caller.  The non-templated mutators + the two log thunks
 * live here to keep `utils/logger.hpp` (and transitively `<fmt>`) out
 * of the public security header surface.
 */
#include "utils/security/domain_routing_table.hpp"

#include "utils/logger.hpp"

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace pylabhub::utils::security
{

DomainRoutingTable::DomainRoutingTable()  = default;
DomainRoutingTable::~DomainRoutingTable() = default;

bool DomainRoutingTable::register_domain(std::string    domain,
                                          PeerAdmission &admission)
{
    if (domain.empty())
        return false;
    std::unique_lock<std::shared_mutex> lk(mu_);
    const auto [it, inserted] =
        map_.try_emplace(std::move(domain), std::ref(admission));
    (void) it;
    return inserted;
}

void DomainRoutingTable::unregister_domain(const std::string &domain) noexcept
{
    if (domain.empty())
        return;
    std::unique_lock<std::shared_mutex> lk(mu_);
    map_.erase(domain);
}

std::size_t DomainRoutingTable::size() const noexcept
{
    std::shared_lock<std::shared_mutex> lk(mu_);
    return map_.size();
}

void DomainRoutingTable::log_admission_threw_(const std::string &domain,
                                               const char *what)
{
    LOGGER_ERROR("DomainRoutingTable: admission for domain '{}' threw "
                 "— treating as deny.  what(): {}", domain, what);
}

void DomainRoutingTable::log_admission_threw_unknown_(
    const std::string &domain)
{
    LOGGER_ERROR("DomainRoutingTable: admission for domain '{}' threw "
                 "non-std exception — treating as deny.", domain);
}

} // namespace pylabhub::utils::security
