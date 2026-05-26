/**
 * @file engine_module_params.cpp
 * @brief Engine lifecycle startup/shutdown callbacks.
 */
#include "utils/engine_module_params.hpp"

#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp" // PYLABHUB_PHYSICAL_PAGE_SIZE, compute_schema_size

#include <cassert>
#include <stdexcept>

namespace pylabhub::scripting
{

void engine_lifecycle_startup(const char * /*arg*/, void *userdata)
{
    auto *p = static_cast<EngineModuleParams *>(userdata);
    if (!p || !p->engine || !p->api)
        throw std::runtime_error("engine_lifecycle_startup: null params, engine, or api");

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

    // Pre-startup invariants on the flexzone introspection cache.  The
    // cache is populated by RoleHostFrame at step 6.5 from each presence's
    // fz_spec.  Two parallel sources exist (params.fz_spec from worker_main_'s
    // local resolve; cache from build_presences_); both should agree.
    {
        const auto &fz = p->api->fz_info_cache();
        // (a) Cache vs params consistency — catches a role host that
        // populates params but skips the frame's cache populate.
        assert(fz.has_tx_fz == p->out_fz_spec.has_schema);
        assert(fz.has_rx_fz == p->in_fz_spec.has_schema);
        // (b) Single canonical invariant: physical == align(logical).
        // Subsumes the page-alignment property and catches drift
        // between the two cached values.
        assert(!fz.has_tx_fz ||
               fz.tx_physical_size == hub::align_to_physical_page(fz.tx_logical_size));
        assert(!fz.has_rx_fz ||
               fz.rx_physical_size == hub::align_to_physical_page(fz.rx_logical_size));
    }

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
        if (p->in_fz_spec.has_schema)
        {
            size_t fz_logical = hub::compute_schema_size(p->in_fz_spec, p->in_packing);
            check("InFlexFrame", fz_logical);
        }
        if (p->out_fz_spec.has_schema)
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
