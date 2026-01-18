#pragma once

#include "utils/JsonConfig.hpp"

namespace pylabhub::utils
{

inline JsonConfigTransaction::JsonConfigTransaction(JsonConfig *owner,
                                                  JsonConfig::AccessFlags flags) noexcept
    : d_owner(owner), d_flags(flags)
{
}

inline JsonConfigTransaction::~JsonConfigTransaction() = default;

template <typename F>
void JsonConfigTransaction::read(F &&fn, std::error_code *ec) &&
{
    if (ec)
        *ec = std::error_code{};
    if (!d_owner)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return;
    }

    if ((d_flags & JsonConfig::AccessFlags::ReloadFirst) != JsonConfig::AccessFlags{})
    {
        if (!d_owner->reload(ec))
        {
            return;
        }
    }

    if (auto rlock = d_owner->lock_for_read(ec))
    {
        fn(rlock->json());
    }
}

template <typename F>
void JsonConfigTransaction::write(F &&fn, std::error_code *ec) &&
{
    if (ec)
        *ec = std::error_code{};
    if (!d_owner)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return;
    }

    if ((d_flags & JsonConfig::AccessFlags::ReloadFirst) != JsonConfig::AccessFlags{})
    {
        if (!d_owner->reload(ec))
        {
            return;
        }
    }

    if (auto wlock = d_owner->lock_for_write(ec))
    {
        fn(wlock->json());
        if ((d_flags & JsonConfig::AccessFlags::CommitAfter) != JsonConfig::AccessFlags{})
        {
            if (!wlock->commit(ec))
            {
                // wlock->commit sets the error code
            }
        }
    }
}

} // namespace pylabhub::utils
