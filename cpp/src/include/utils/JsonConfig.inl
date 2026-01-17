#pragma once

#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"



inline bool has_flag(JsonConfig::AccessFlags composite, JsonConfig::AccessFlags flag)
{
    return (static_cast<int>(composite) & static_cast<int>(flag)) != 0;
}

template <typename F>
void with_json_read(JsonConfig::Transaction &&tx, F &&fn, std::error_code *ec)
{
    static_assert(
        std::is_invocable_v<F, const nlohmann::json &>,
        "with_json_read(Func) requires callable invocable as f(const nlohmann::json&)");

    if (!tx.owner || !tx.owner->is_initialized())
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return;
    }

    if (pylabhub::basics::RecursionGuard::is_recursing(tx.owner))
    {
        if (ec)
            *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return;
    }
    pylabhub::basics::RecursionGuard guard(tx.owner);

    try
    {
        if (has_flag(tx.flags, JsonConfig::AccessFlags::ReloadFirst))
        {
            if (!tx.owner->reload(ec))
            {
                // reload() failed, it has set the error code.
                return;
            }
        }

        if (auto r = tx.owner->lock_for_read(ec))
        {
            std::forward<F>(fn)(r->json());
            if (ec)
                *ec = std::error_code{};
        }
        // lock_for_read failed, it has set the error code.
    }
    catch (const std::exception &ex)
    {
        LOGGER_ERROR("JsonConfig::with_json_read: exception in user callback: {}", ex.what());
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::with_json_read: unknown exception in user callback");
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
    }
}

template <typename F>
void with_json_write(JsonConfig::Transaction &&tx, F &&fn, std::error_code *ec)
{
    static_assert(std::is_invocable_v<F, nlohmann::json &>,
                  "with_json_write(Func) requires callable invocable as f(nlohmann::json&)");

    if (!tx.owner || !tx.owner->is_initialized())
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return;
    }
    
    if (pylabhub::basics::RecursionGuard::is_recursing(tx.owner))
    {
        if (ec)
            *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return;
    }
    pylabhub::basics::RecursionGuard guard(tx.owner);

    try
    {
        if (has_flag(tx.flags, JsonConfig::AccessFlags::ReloadFirst))
        {
            if (!tx.owner->reload(ec))
            {
                // reload() failed, it has set the error code.
                return;
            }
        }

        if (auto w = tx.owner->lock_for_write(ec))
        {
            std::forward<F>(fn)(w->json());

            if (has_flag(tx.flags, JsonConfig::AccessFlags::CommitAfter))
            {
                if (!w->commit(ec))
                {
                    // commit failed, it has set the error code.
                    return;
                }
            }
            if (ec)
                *ec = std::error_code{};
        }
        // lock_for_write failed, it has set the error code.
    }
    catch (const std::exception &ex)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: exception in user callback: {}", ex.what());
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: unknown exception in user callback");
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
    }
}


