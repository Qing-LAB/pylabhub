#pragma once
/**
 * @file engine_module_params.hpp
 * @brief EngineModuleParams — bundles everything the engine lifecycle module needs.
 *
 * Created by the role host, passed as userdata to the lifecycle dynamic module.
 * The startup callback uses it to initialize the engine. The shutdown callback
 * uses it to finalize. The role host owns the struct (unique_ptr member).
 *
 * See docs/tech_draft/script_engine_lifecycle_module.md for the full design.
 */

#include "utils/script_engine.hpp"

#include <filesystem>
#include <string>

namespace pylabhub::scripting
{

/**
 * @brief Parameters for the engine lifecycle module startup/shutdown callbacks.
 *
 * Group A: from config (consumed by startup callback)
 * Group C: from infrastructure (role_ctx, filled after setup_infrastructure_)
 */
struct EngineModuleParams
{
    // Engine instance (not owned — role host owns via unique_ptr).
    ScriptEngine *engine{nullptr};
    RoleAPIBase  *api{nullptr};          ///< Fully-wired role API (owned by role host).

    // Startup parameters (Group A: from config).
    std::string tag;                       ///< "prod" / "cons" / "proc"
    std::filesystem::path script_dir;
    std::string entry_point;               ///< "init.lua" / "__init__.py" / lib filename
    std::string required_callback;         ///< "on_produce" / "on_consume" / "on_process"

    // Directional schemas — consistent naming across all roles.
    // Producer: out_slot_spec + out_fz_spec filled.
    // Consumer: in_slot_spec + in_fz_spec filled.
    // Processor: all four filled (in + out).
    hub::SchemaSpec in_slot_spec;               ///< Input slot schema (consumer, processor).
    hub::SchemaSpec out_slot_spec;              ///< Output slot schema (producer, processor).
    hub::SchemaSpec in_fz_spec;                 ///< Input flexzone schema (consumer, processor).
    hub::SchemaSpec out_fz_spec;                ///< Output flexzone schema (producer, processor).
    hub::SchemaSpec inbox_spec;                 ///< Inbox schema (empty if none).
    std::string in_packing{"aligned"};     ///< Input packing (consumer, processor).
    std::string out_packing{"aligned"};    ///< Output packing (producer, processor).

    // Module name for lifecycle registration.
    std::string module_name;
};

/// Lifecycle startup callback — initializes the engine fully.
/// Receives EngineModuleParams* as userdata.
void engine_lifecycle_startup(const char *arg, void *userdata);

/// Lifecycle shutdown callback — finalizes the engine (idempotent).
/// Receives EngineModuleParams* as userdata.
void engine_lifecycle_shutdown(const char *arg, void *userdata);

} // namespace pylabhub::scripting
