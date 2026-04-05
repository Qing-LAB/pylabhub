/**
 * @file engine_module_params.cpp
 * @brief Engine lifecycle startup/shutdown callbacks.
 */
#include "engine_module_params.hpp"

#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"

#include <cassert>
#include <stdexcept>

namespace pylabhub::scripting
{

void engine_lifecycle_startup(const char * /*arg*/, void *userdata)
{
    auto *p = static_cast<EngineModuleParams *>(userdata);
    if (!p || !p->engine)
        throw std::runtime_error("engine_lifecycle_startup: null params or engine");

    // Step 1: Initialize engine.
    if (!p->engine->initialize(p->tag, p->api ? p->api->core() : nullptr))
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

    // Flexzone specs must be set by role host before engine startup.
    assert(!p->in_fz_spec.has_schema ||
           (p->api->core() && p->api->core()->has_in_fz() &&
            p->api->core()->in_schema_fz_size() % 4096 == 0));
    assert(!p->out_fz_spec.has_schema ||
           (p->api->core() && p->api->core()->has_out_fz() &&
            p->api->core()->out_schema_fz_size() % 4096 == 0));

    if (p->inbox_spec.has_schema)
    {
        if (!p->engine->register_slot_type(p->inbox_spec, "InboxFrame", p->inbox_spec.packing))
            throw std::runtime_error("register InboxFrame failed");
    }

    // Step 4: Build API with infrastructure context.
    if (!p->engine->build_api(*p->api))
        throw std::runtime_error("build_api failed");

    // Step 5: Cross-validate engine type sizes against schema logical sizes.
    // The engine's ctypes/FFI struct must match compute_schema_size exactly.
    auto *core = p->api->core();
    if (core)
    {
        auto check = [&](const char *type_name, size_t expected) {
            size_t actual = p->engine->type_sizeof(type_name);
            if (actual > 0 && actual != expected)
                throw std::runtime_error(
                    fmt::format("{} size mismatch: engine={} schema={}", type_name, actual, expected));
        };
        if (core->has_in_slot())
            check("InSlotFrame", core->in_slot_logical_size());
        if (core->has_out_slot())
            check("OutSlotFrame", core->out_slot_logical_size());
        if (core->has_in_fz())
        {
            size_t fz_logical = hub::compute_schema_size(p->in_fz_spec, p->in_packing);
            check("InFlexFrame", fz_logical);
        }
        if (core->has_out_fz())
        {
            size_t fz_logical = hub::compute_schema_size(p->out_fz_spec, p->out_packing);
            check("OutFlexFrame", fz_logical);
        }
    }

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
