/**
 * @file engine_module_params.cpp
 * @brief Engine lifecycle startup/shutdown callbacks.
 */
#include "engine_module_params.hpp"

#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_field_layout.hpp"

#include <stdexcept>

namespace pylabhub::scripting
{

void engine_lifecycle_startup(const char * /*arg*/, void *userdata)
{
    auto *p = static_cast<EngineModuleParams *>(userdata);
    if (!p || !p->engine)
        throw std::runtime_error("engine_lifecycle_startup: null params or engine");

    // Step 1: Initialize engine.
    if (!p->engine->initialize(p->tag, p->core))
        throw std::runtime_error("engine initialize failed");

    // Step 2: Load script.
    if (!p->engine->load_script(p->script_dir, p->entry_point, p->required_callback))
        throw std::runtime_error("load_script failed");

    // Step 3: Register slot and flexzone types — always directional names.
    if (p->in_slot_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->in_slot_spec, "InSlotFrame", p->in_packing))
            throw std::runtime_error("register InSlotFrame failed");
    }

    if (p->out_slot_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->out_slot_spec, "OutSlotFrame", p->out_packing))
            throw std::runtime_error("register OutSlotFrame failed");
    }

    if (p->in_fz_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->in_fz_spec, "InFlexFrame", p->in_packing))
            throw std::runtime_error("register InFlexFrame failed");
    }

    if (p->out_fz_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->out_fz_spec, "OutFlexFrame", p->out_packing))
            throw std::runtime_error("register OutFlexFrame failed");
    }

    // Set flexzone specs on core. Size is computed from schema via compute_field_layout —
    // infrastructure-authoritative, no engine involvement.
    auto compute_fz_size = [](const hub::SchemaSpec &spec, const std::string &packing) -> size_t {
        auto [layout, sz] = hub::compute_field_layout(hub::to_field_descs(spec.fields), packing);
        return sz;
    };

    if (p->core != nullptr && p->in_fz_spec.has_schema && !p->core->has_in_fz())
    {
        p->core->set_in_fz_spec(p->in_fz_spec, compute_fz_size(p->in_fz_spec, p->in_packing));
    }
    if (p->core != nullptr && p->out_fz_spec.has_schema && !p->core->has_out_fz())
    {
        p->core->set_out_fz_spec(p->out_fz_spec, compute_fz_size(p->out_fz_spec, p->out_packing));
    }

    if (p->inbox_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->inbox_spec, "InboxFrame", p->in_packing))
            throw std::runtime_error("register InboxFrame failed");
    }

    // Step 4: Build API with infrastructure context.
    if (!p->engine->build_api(p->role_ctx))
        throw std::runtime_error("build_api failed");

    LOGGER_INFO("[{}] engine lifecycle startup complete", p->tag);
}

void engine_lifecycle_shutdown(const char * /*arg*/, void *userdata)
{
    auto *p = static_cast<EngineModuleParams *>(userdata);
    if (!p || !p->engine)
        return;

    // finalize() is idempotent — no-op if already called by role host.
    p->engine->finalize();
}

} // namespace pylabhub::scripting
