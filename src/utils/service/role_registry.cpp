/**
 * @file role_registry.cpp
 * @brief RoleRegistry — runtime registration of role host factories.
 *
 * Storage is a static unordered_map guarded by a single mutex. Registration
 * is expected lazily during binary startup; once dispatch begins, the
 * table is read-only in practice (binaries register exactly one role).
 */
#include "utils/role_registry.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace pylabhub::utils
{

namespace
{

std::mutex &registry_mutex_()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, RoleRuntimeInfo> &registry_()
{
    static std::unordered_map<std::string, RoleRuntimeInfo> r;
    return r;
}

} // namespace

// ─── RuntimeBuilder (plain value type, no pimpl) ─────────────────────────────

RoleRegistry::RuntimeBuilder::RuntimeBuilder(std::string_view role_tag)
{
    entry_.role_tag = std::string(role_tag);
}

RoleRegistry::RuntimeBuilder::~RuntimeBuilder()
{
    if (!committed_)
    {
        try
        {
            commit();
        }
        catch (...)
        {
            // Swallow — throwing from dtor is UB.
            // A caller that forgot to check commit() gets no entry.
        }
    }
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::role_label(std::string_view label)
{
    entry_.role_label = std::string(label);
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::host_factory(RoleRuntimeInfo::HostFactory f)
{
    entry_.host_factory = f;
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::config_parser(RoleRuntimeInfo::ConfigParser p)
{
    entry_.config_parser = p;
    return *this;
}

void RoleRegistry::RuntimeBuilder::commit()
{
    if (committed_)
        return;  // idempotent — second call is a no-op
    committed_ = true;

    if (entry_.host_factory == nullptr)
    {
        throw std::runtime_error(
            "RoleRegistry::register_runtime('" + entry_.role_tag +
            "'): host_factory() is required before commit");
    }

    std::lock_guard lk(registry_mutex_());
    auto &map = registry_();
    auto [it, inserted] = map.emplace(entry_.role_tag, entry_);
    if (!inserted)
    {
        throw std::runtime_error(
            "RoleRegistry::register_runtime: role_tag '" +
            entry_.role_tag + "' already registered");
    }
}

// ─── Static entry points ─────────────────────────────────────────────────────

RoleRegistry::RuntimeBuilder
RoleRegistry::register_runtime(std::string_view role_tag)
{
    return RuntimeBuilder(role_tag);
}

const RoleRuntimeInfo *
RoleRegistry::get_runtime(std::string_view role_tag)
{
    std::lock_guard lk(registry_mutex_());
    const auto &map = registry_();
    // unordered_map heterogeneous lookup requires C++20 transparent hash;
    // fall back to string copy for portability (lookup path, not hot).
    auto it = map.find(std::string(role_tag));
    return it == map.end() ? nullptr : &it->second;
}

} // namespace pylabhub::utils
