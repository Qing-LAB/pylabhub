#pragma once
/**
 * @file role_registry.hpp
 * @brief RoleRegistry — runtime-time registration of role host factories.
 *
 * Mirrors RoleDirectory::register_role() (init-time content: config template,
 * on_init callback) but for **runtime** dispatch: binaries like `plh_role`
 * look up a role tag and construct the matching @ref RoleHostBase via the
 * factory stored here.
 *
 * Two separate registries on purpose:
 *   - RoleDirectory::register_role()  — init-time (plh_role init)
 *   - RoleRegistry::register_runtime() — runtime  (plh_role run)
 *
 * Keyed by the short role tag used elsewhere in the codebase
 * ("prod"/"cons"/"proc"/custom). Distinct init/runtime keys may be chosen
 * independently; the canonical convention is to reuse the short tag.
 *
 * ABI-hardened vs register_role():
 *   - Factory is a **function pointer**, not std::function
 *     (HEP-CORE-0032 Phase 3 — no std::function in ABI-exposed signatures).
 *   - Query API takes @c std::string_view — no allocation on the read path.
 *   - Builder uses pimpl so fields may be appended without breaking layout.
 *
 * Thread safety: registration happens at static init / main startup under
 * an internal mutex. Lookups are read-only after all registrations complete.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <any>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::config { class RoleConfig; }
namespace pylabhub::scripting
{
class RoleHostBase;
class ScriptEngine;
} // namespace pylabhub::scripting

namespace pylabhub::utils
{

/// Runtime metadata for a registered role.
struct RoleRuntimeInfo
{
    /// Short tag used as the registry key ("prod"/"cons"/"proc"/...).
    std::string role_tag;

    /// Human-readable label for diagnostics ("Producer"/"Consumer"/...).
    std::string role_label;

    /// Factory: construct a concrete role host. Function pointer (NOT
    /// std::function) for ABI stability across compiler/stdlib versions.
    ///
    /// The caller moves ownership of config and engine into the factory.
    /// The shutdown_flag is a non-owning pointer shared with main.
    using HostFactory = std::unique_ptr<scripting::RoleHostBase>(*)(
        pylabhub::config::RoleConfig config,
        std::unique_ptr<scripting::ScriptEngine> engine,
        std::atomic<bool> *shutdown_flag);
    HostFactory host_factory = nullptr;

    /// NUL-terminated list of script callback names required by this role
    /// ("on_init"/"on_produce"/"on_stop"/...). Used by engine introspection
    /// and CLI --validate diagnostics. May be nullptr if the role has no
    /// callbacks.
    const char *const *engine_callbacks = nullptr;

    /// Long-form role name as accepted by @c config::RoleConfig::load
    /// and @c load_from_directory ("producer"/"consumer"/"processor").
    /// Stored separately from @c role_tag because the registry key uses
    /// the short internal form while config validation uses the long
    /// user-facing form.
    std::string config_role_name;

    /// Role-specific JSON parser — extracts role_data<T> from the full
    /// config JSON. Function pointer (NOT std::function) for ABI
    /// stability. May be nullptr if the role has no custom fields.
    ///
    /// Signature mirrors @c config::RoleConfig::RoleParser but strictly
    /// as a function pointer; std::function implicitly accepts a
    /// function pointer so RoleConfig::load can consume it directly.
    using ConfigParser = std::any(*)(const nlohmann::json &,
                                     const config::RoleConfig &);
    ConfigParser config_parser = nullptr;
};

class PYLABHUB_UTILS_EXPORT RoleRegistry
{
  public:
    /// Fluent builder for a runtime registration. Commits on destruction
    /// (or explicitly via @ref commit). Pimpl'd for ABI stability.
    class PYLABHUB_UTILS_EXPORT RuntimeBuilder
    {
      public:
        explicit RuntimeBuilder(std::string_view role_tag);
        ~RuntimeBuilder();

        RuntimeBuilder(const RuntimeBuilder &)            = delete;
        RuntimeBuilder &operator=(const RuntimeBuilder &) = delete;
        RuntimeBuilder(RuntimeBuilder &&) noexcept;
        RuntimeBuilder &operator=(RuntimeBuilder &&) noexcept;

        RuntimeBuilder &role_label(std::string_view label);
        RuntimeBuilder &host_factory(RoleRuntimeInfo::HostFactory f);
        RuntimeBuilder &engine_callbacks(const char *const *list);
        RuntimeBuilder &config_role_name(std::string_view long_name);
        RuntimeBuilder &config_parser(RoleRuntimeInfo::ConfigParser p);

        /// Insert the accumulated entry under the role_tag given at
        /// construction. Throws std::runtime_error if role_tag is already
        /// registered or if host_factory was not provided. May be called
        /// exactly once; destructor commits if not called explicitly.
        void commit();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// Begin a runtime registration for @p role_tag.
    static RuntimeBuilder register_runtime(std::string_view role_tag);

    /// Look up a previously registered runtime entry.
    /// @return pointer to the stored info (valid for process lifetime),
    ///         or nullptr if no entry matches @p role_tag.
    static const RoleRuntimeInfo *get_runtime(std::string_view role_tag);

    /// List all registered role tags (for CLI `--help` and diagnostics).
    /// Returns a snapshot; safe after all static registrations complete.
    static std::vector<std::string> list_registered_runtimes();
};

} // namespace pylabhub::utils
