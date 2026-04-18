/**
 * @file role_bootstrap.cpp
 * @brief Implementation of bootstrap_role_from_{dir,config}.
 */
#include "role_bootstrap.hpp"
#include "engine_factory.hpp"

#include "utils/config/role_config.hpp"
#include "utils/role_registry.hpp"

#include <stdexcept>
#include <utility>

namespace pylabhub::scripting
{

namespace
{

const utils::RoleRuntimeInfo &
require_info(std::string_view runtime_tag)
{
    const utils::RoleRuntimeInfo *info =
        utils::RoleRegistry::get_runtime(runtime_tag);
    if (!info)
    {
        throw std::runtime_error(
            "bootstrap_role: unknown runtime tag '" +
            std::string(runtime_tag) +
            "' — register_runtime() was not called for this role");
    }
    if (!info->host_factory)
    {
        throw std::runtime_error(
            "bootstrap_role: runtime tag '" + std::string(runtime_tag) +
            "' is registered but has no host_factory — registration is incomplete");
    }
    return *info;
}

std::unique_ptr<RoleHostBase>
finish_bootstrap(const utils::RoleRuntimeInfo &info,
                 config::RoleConfig cfg,
                 std::atomic<bool> *shutdown_flag)
{
    auto engine = make_engine_from_script_config(cfg.script());
    return info.host_factory(std::move(cfg), std::move(engine), shutdown_flag);
}

} // namespace

std::unique_ptr<RoleHostBase>
bootstrap_role_from_dir(std::string_view runtime_tag,
                         const std::string &role_dir,
                         std::atomic<bool> *shutdown_flag)
{
    const auto &info = require_info(runtime_tag);
    auto cfg = config::RoleConfig::load_from_directory(
        role_dir,
        info.config_role_name.c_str(),
        info.config_parser /* function ptr → std::function, nullptr ok */);
    return finish_bootstrap(info, std::move(cfg), shutdown_flag);
}

std::unique_ptr<RoleHostBase>
bootstrap_role_from_config(std::string_view runtime_tag,
                            const std::string &config_path,
                            std::atomic<bool> *shutdown_flag)
{
    const auto &info = require_info(runtime_tag);
    auto cfg = config::RoleConfig::load(
        config_path,
        info.config_role_name.c_str(),
        info.config_parser);
    return finish_bootstrap(info, std::move(cfg), shutdown_flag);
}

} // namespace pylabhub::scripting
