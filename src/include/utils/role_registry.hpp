#pragma once
/**
 * @file role_registry.hpp
 * @brief RoleRegistry — runtime-time registration of role host factories.
 *
 * Mirrors @c RoleDirectory::register_role() (init-time content: config
 * template, on_init callback) but for **runtime** dispatch: binaries like
 * @c plh_role look up a role tag and construct the matching
 * @ref RoleHostBase via the factory stored here.
 *
 * Two separate registries on purpose:
 *   - @c RoleDirectory::register_role()   — init-time  (plh_role init)
 *   - @c RoleRegistry::register_runtime() — runtime   (plh_role run)
 *
 * Registry keys are the **long-form** role name ("producer"/"consumer"/
 * "processor"), matching @c RoleConfig::load's validation tag and the
 * user-facing CLI (`--role producer`). The short internal form
 * ("prod"/"cons"/"proc") used by RoleAPIBase/ThreadManager is a role-host-
 * internal convention and does not appear in the registry.
 *
 * Registration is **lazy by convention**: binaries register only the role
 * they are about to run. Dispatching binaries therefore keep a local map
 * from role name → @c register_X_runtime and invoke exactly one.
 *
 * ABI-hardened:
 *   - Factory + parser are function pointers, NOT std::function
 *     (HEP-CORE-0032 Phase 3 — no std::function in ABI-exposed signatures).
 *   - Query API takes @c std::string_view — no allocation on the read path.
 *
 * Thread safety: an internal mutex guards register+lookup. Registration
 * happens at binary startup; lookups are read-only afterwards.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <any>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::config { class RoleConfig; }
namespace pylabhub::scripting
{
// RoleHostBase is a typedef (EngineHost<RoleAPIBase>) — forward-declare
// the template + the API class + define the alias.  Keeps this header
// free of engine_host.hpp's full include.
class RoleAPIBase;
template <typename ApiT> class EngineHost;
using RoleHostBase = EngineHost<RoleAPIBase>;
class ScriptEngine;
} // namespace pylabhub::scripting

namespace pylabhub::utils
{

/// Runtime metadata for a registered role.
struct RoleRuntimeInfo
{
    /// Registry key — long form ("producer"/"consumer"/"processor"). Also
    /// the tag passed to @c RoleConfig::load_from_directory for validation.
    std::string role_tag;

    /// Human-readable label for diagnostics ("Producer"/"Consumer"/...).
    std::string role_label;

    /// Factory: construct a concrete role host. Function pointer (NOT
    /// std::function) for ABI stability across compiler/stdlib versions.
    using HostFactory = std::unique_ptr<scripting::RoleHostBase>(*)(
        pylabhub::config::RoleConfig config,
        std::unique_ptr<scripting::ScriptEngine> engine,
        std::atomic<bool> *shutdown_flag);
    HostFactory host_factory = nullptr;

    /// Role-specific JSON parser — extracts role_data<T> from the full
    /// config JSON. Function pointer (NOT std::function) for ABI stability.
    /// May be nullptr if the role has no custom fields.
    ///
    /// Signature mirrors @c config::RoleConfig::RoleParser. std::function
    /// implicitly accepts a function pointer, so callers pass
    /// @c info->config_parser directly to @c RoleConfig::load_from_directory.
    using ConfigParser = std::any(*)(const nlohmann::json &,
                                     const config::RoleConfig &);
    ConfigParser config_parser = nullptr;
};

class PYLABHUB_UTILS_EXPORT RoleRegistry
{
  public:
    /// Fluent builder for a runtime registration. Commits on destruction
    /// (or explicitly via @ref commit). Lib-internal value type — no
    /// pimpl since there's no cross-DSO layout concern (only used as a
    /// stack-local temporary returned from @ref register_runtime).
    class PYLABHUB_UTILS_EXPORT RuntimeBuilder
    {
      public:
        explicit RuntimeBuilder(std::string_view role_tag);
        ~RuntimeBuilder();

        RuntimeBuilder(const RuntimeBuilder &)            = delete;
        RuntimeBuilder &operator=(const RuntimeBuilder &) = delete;
        RuntimeBuilder(RuntimeBuilder &&) noexcept        = default;
        RuntimeBuilder &operator=(RuntimeBuilder &&) noexcept = default;

        RuntimeBuilder &role_label(std::string_view label);
        RuntimeBuilder &host_factory(RoleRuntimeInfo::HostFactory f);
        RuntimeBuilder &config_parser(RoleRuntimeInfo::ConfigParser p);

        /// Insert the accumulated entry under the role_tag given at
        /// construction. Throws std::runtime_error if role_tag is already
        /// registered or if host_factory was not provided. Idempotent —
        /// repeated calls are no-ops. The destructor commits if not
        /// called explicitly.
        void commit();

      private:
        RoleRuntimeInfo entry_;
        bool            committed_ = false;
    };

    /// Begin a runtime registration for @p role_tag (long form —
    /// "producer"/"consumer"/"processor").
    static RuntimeBuilder register_runtime(std::string_view role_tag);

    /// Look up a previously registered runtime entry.
    /// @return pointer to the stored info (valid for process lifetime),
    ///         or nullptr if no entry matches @p role_tag.
    static const RoleRuntimeInfo *get_runtime(std::string_view role_tag);
};

} // namespace pylabhub::utils
