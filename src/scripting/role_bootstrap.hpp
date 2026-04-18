#pragma once
/**
 * @file role_bootstrap.hpp
 * @brief Generic "config + engine + host" wiring used by plh_role.
 *
 * Composes `RoleRegistry::get_runtime` + `RoleConfig::load*` +
 * `make_engine_from_script_config` + host factory into a single call.
 * Each standalone role binary (producer/consumer/processor main) still
 * does the wiring inline (they already know their role statically); this
 * helper exists so the unified `plh_role` binary can dispatch entirely
 * through the registry without hard-coding role types.
 *
 * Lives in the scripting layer because it calls the engine factory,
 * which pulls in LuaEngine + PythonEngine.
 */

#include "utils/role_host_base.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::scripting
{

/// Bootstrap a role host from a role directory.
///
/// Steps:
///   1. Look up the RoleRuntimeInfo for @p runtime_tag ("prod"/"cons"/"proc").
///   2. Load config via @c RoleConfig::load_from_directory using the
///      info's @c config_role_name + @c config_parser.
///   3. Build a ScriptEngine via @ref make_engine_from_script_config.
///   4. Invoke the registered @c host_factory.
///
/// @throws std::runtime_error if @p runtime_tag is not registered or if
///         config load fails; propagates any exception from the factory.
std::unique_ptr<RoleHostBase> bootstrap_role_from_dir(
    std::string_view runtime_tag,
    const std::string &role_dir,
    std::atomic<bool> *shutdown_flag);

/// Same as @ref bootstrap_role_from_dir but loads from an explicit config
/// file path instead of a role directory.
std::unique_ptr<RoleHostBase> bootstrap_role_from_config(
    std::string_view runtime_tag,
    const std::string &config_path,
    std::atomic<bool> *shutdown_flag);

} // namespace pylabhub::scripting
