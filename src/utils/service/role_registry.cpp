/**
 * @file role_registry.cpp
 * @brief RoleRegistry — runtime registration of role host factories.
 *
 * Storage is a static unordered_map guarded by a single mutex. Registration
 * is expected during static init / main startup; once plh_role begins
 * dispatching, the table is read-only in practice.
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

// ─── RuntimeBuilder ──────────────────────────────────────────────────────────

struct RoleRegistry::RuntimeBuilder::Impl
{
    RoleRuntimeInfo entry;
    bool            committed = false;
};

RoleRegistry::RuntimeBuilder::RuntimeBuilder(std::string_view role_tag)
    : impl_(std::make_unique<Impl>())
{
    impl_->entry.role_tag = std::string(role_tag);
}

RoleRegistry::RuntimeBuilder::~RuntimeBuilder()
{
    if (impl_ && !impl_->committed)
    {
        try
        {
            commit();
        }
        catch (...)
        {
            // Swallow — throwing from dtor is UB.
            // The caller that forgot to check commit() gets no entry.
        }
    }
}

RoleRegistry::RuntimeBuilder::RuntimeBuilder(RuntimeBuilder &&) noexcept = default;
RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::operator=(RuntimeBuilder &&) noexcept = default;

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::role_label(std::string_view label)
{
    impl_->entry.role_label = std::string(label);
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::host_factory(RoleRuntimeInfo::HostFactory f)
{
    impl_->entry.host_factory = f;
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::engine_callbacks(const char *const *list)
{
    impl_->entry.engine_callbacks = list;
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::config_role_name(std::string_view long_name)
{
    impl_->entry.config_role_name = std::string(long_name);
    return *this;
}

RoleRegistry::RuntimeBuilder &
RoleRegistry::RuntimeBuilder::config_parser(RoleRuntimeInfo::ConfigParser p)
{
    impl_->entry.config_parser = p;
    return *this;
}

void RoleRegistry::RuntimeBuilder::commit()
{
    if (impl_->committed)
        return;  // idempotent — second call is a no-op
    impl_->committed = true;

    if (impl_->entry.host_factory == nullptr)
    {
        throw std::runtime_error(
            "RoleRegistry::register_runtime('" + impl_->entry.role_tag +
            "'): host_factory() is required before commit");
    }

    std::lock_guard lk(registry_mutex_());
    auto &map = registry_();
    auto [it, inserted] = map.emplace(impl_->entry.role_tag, impl_->entry);
    if (!inserted)
    {
        throw std::runtime_error(
            "RoleRegistry::register_runtime: role_tag '" +
            impl_->entry.role_tag + "' already registered");
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
    // fall back to string copy for portability (registration path only).
    auto it = map.find(std::string(role_tag));
    return it == map.end() ? nullptr : &it->second;
}

std::vector<std::string>
RoleRegistry::list_registered_runtimes()
{
    std::lock_guard lk(registry_mutex_());
    const auto &map = registry_();
    std::vector<std::string> out;
    out.reserve(map.size());
    for (const auto &[k, _v] : map)
        out.push_back(k);
    return out;
}

} // namespace pylabhub::utils
