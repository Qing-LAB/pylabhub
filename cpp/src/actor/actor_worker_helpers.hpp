#pragma once
/**
 * @file actor_worker_helpers.hpp
 * @brief Internal helper functions shared by ProducerRoleWorker and ConsumerRoleWorker.
 *
 * Include this header from within translation units that also include actor_host.hpp.
 * All helpers are anonymous-namespace (translation-unit private) — each TU gets its own
 * copy; there are no exported symbols.
 *
 * **Do not install** — this is a private build-only header.
 */
#include "actor_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <zmq.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>


namespace py  = pybind11;
namespace fs  = std::filesystem;

// Suppress unused-function warnings for helpers not used in every TU that includes this header.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace pylabhub::actor
{


namespace
{

bool is_callable(const py::object &obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

/**
 * @brief Import a Python module by name, prepending base_dir to sys.path.
 *
 * Uses a synthetic module alias `_plh_{actor_uid_hex}_{module_name}` in
 * sys.modules to isolate module state between different actor instances
 * that load the same module name.
 *
 * @param module_name   Python module name (e.g. "sensor_node")
 * @param base_dir      Directory prepended to sys.path (may be empty)
 * @param actor_uid_hex 8-hex suffix from actor UID for aliasing
 * @return Imported module object (the real module, not the alias)
 * @throws py::error_already_set on import error
 */
py::module_ import_script_module(const std::string &module_name,
                                  const std::string &base_dir,
                                  const std::string &actor_uid_hex)
{
    py::module_ sys      = py::module_::import("sys");
    py::list    sys_path = sys.attr("path").cast<py::list>();

    // Prepend base_dir to sys.path (no duplicate).
    if (!base_dir.empty())
    {
        const std::string canon = fs::weakly_canonical(base_dir).string();
        bool found = false;
        for (auto item : sys_path)
        {
            if (item.cast<std::string>() == canon)
            {
                found = true;
                break;
            }
        }
        if (!found)
            sys_path.insert(0, canon);
    }

    // Import the module.
    py::module_ mod = py::module_::import(module_name.c_str());

    // Register under a synthetic alias so separate actor instances using
    // the same module_name each get an independent module object.
    const std::string alias = "_plh_" + actor_uid_hex + "_" + module_name;
    py::dict sys_modules = sys.attr("modules").cast<py::dict>();
    if (!sys_modules.contains(alias))
        sys_modules[alias.c_str()] = mod;

    return mod;
}

/**
 * @brief Load a role's `script/` Python package via `importlib.util.spec_from_file_location`.
 *
 * Resolution order:
 *   1. `<base_dir>/<module_name>/__init__.py`  — Python package (preferred)
 *   2. `<base_dir>/<module_name>.py`           — flat module file
 *
 * The module is registered in `sys.modules` under the unique alias
 * `_plh_{actor_uid_hex}_{role_name}`, preventing cross-role and cross-actor
 * collisions even when multiple roles use the same `module_name` (e.g. `"script"`).
 *
 * For packages, `submodule_search_locations` is set so that relative imports
 * (`from . import helpers`) resolve correctly within the `script/` directory.
 *
 * MUST be called with the GIL held.
 *
 * @throws std::runtime_error  if neither file variant is found
 * @throws py::error_already_set  on Python import / exec error
 */
py::module_ import_role_script_module(const std::string &role_name,
                                       const std::string &module_name,
                                       const std::string &base_dir,
                                       const std::string &actor_uid_hex)
{
    const fs::path base_path = base_dir.empty()
                               ? fs::current_path()
                               : fs::weakly_canonical(base_dir);

    const fs::path pkg_init = base_path / module_name / "__init__.py";
    const fs::path mod_file = base_path / (module_name + ".py");

    bool        is_package = false;
    std::string file_path;

    if (fs::exists(pkg_init))
    {
        file_path  = pkg_init.string();
        is_package = true;
    }
    else if (fs::exists(mod_file))
    {
        file_path = mod_file.string();
    }
    else
    {
        throw std::runtime_error(
            "Actor script not found for role '" + role_name +
            "': tried '" + pkg_init.string() +
            "' and '"    + mod_file.string() + "'");
    }

    // Unique alias: prevents collisions between roles and between actor instances.
    const std::string alias = "_plh_" + actor_uid_hex + "_" + role_name;

    py::module_ importlib_util = py::module_::import("importlib.util");
    py::module_ sys_mod        = py::module_::import("sys");
    py::dict    sys_modules    = sys_mod.attr("modules").cast<py::dict>();

    py::object spec;
    if (is_package)
    {
        // Pass submodule_search_locations so relative imports (from . import X) work.
        py::list search_locs;
        search_locs.append((base_path / module_name).string());
        spec = importlib_util.attr("spec_from_file_location")(
            alias, file_path,
            py::arg("submodule_search_locations") = search_locs);
    }
    else
    {
        spec = importlib_util.attr("spec_from_file_location")(alias, file_path);
    }

    if (spec.is_none())
        throw std::runtime_error(
            "spec_from_file_location failed for role '" + role_name +
            "' file '" + file_path + "'");

    py::object mod = importlib_util.attr("module_from_spec")(spec);
    sys_modules[alias.c_str()] = mod;
    spec.attr("loader").attr("exec_module")(mod);

    return mod.cast<py::module_>();
}

py::object json_type_to_ctypes(py::module_ &ct, const FieldDef &fd)
{
    py::object base;
    if      (fd.type_str == "bool")    base = ct.attr("c_bool");
    else if (fd.type_str == "int8")    base = ct.attr("c_int8");
    else if (fd.type_str == "uint8")   base = ct.attr("c_uint8");
    else if (fd.type_str == "int16")   base = ct.attr("c_int16");
    else if (fd.type_str == "uint16")  base = ct.attr("c_uint16");
    else if (fd.type_str == "int32")   base = ct.attr("c_int32");
    else if (fd.type_str == "uint32")  base = ct.attr("c_uint32");
    else if (fd.type_str == "int64")   base = ct.attr("c_int64");
    else if (fd.type_str == "uint64")  base = ct.attr("c_uint64");
    else if (fd.type_str == "float32") base = ct.attr("c_float");
    else if (fd.type_str == "float64") base = ct.attr("c_double");
    else if (fd.type_str == "string")
    {
        if (fd.length == 0)
            throw std::runtime_error(
                "Schema: string field '" + fd.name + "' needs 'length' > 0");
        return ct.attr("c_char").attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else if (fd.type_str == "bytes")
    {
        if (fd.length == 0)
            throw std::runtime_error(
                "Schema: bytes field '" + fd.name + "' needs 'length' > 0");
        base = ct.attr("c_uint8");
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else
    {
        throw std::runtime_error(
            "Schema: unknown type '" + fd.type_str + "' for field '" + fd.name + "'");
    }
    if (fd.count > 1)
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.count)));
    return base;
}

py::object build_ctypes_struct(const SchemaSpec &spec, const char *name)
{
    py::module_ ct = py::module_::import("ctypes");
    py::list fields;
    for (const auto &fd : spec.fields)
        fields.append(py::make_tuple(fd.name, json_type_to_ctypes(ct, fd)));
    py::dict kw;
    kw["_fields_"] = fields;
    if (spec.packing == "packed")
        kw["_pack_"] = py::int_(1);
    return py::eval("type")(name,
                             py::make_tuple(ct.attr("LittleEndianStructure")), kw);
}

py::object build_numpy_dtype(const SchemaSpec &spec)
{
    py::module_ np = py::module_::import("numpy");
    return np.attr("dtype")(spec.numpy_dtype);
}

/// Build a deterministic canonical string for one buffer schema.
/// Format (ctypes): "slot:name:type:count:len|name:type:count:len|..."
/// Format (numpy):  "slot:numpy_array:dtype[:dim...]"
void append_schema_canonical(std::string &out, const std::string &prefix,
                              const SchemaSpec &spec)
{
    out += prefix;
    if (spec.exposure == SlotExposure::NumpyArray)
    {
        out += "numpy_array:";
        out += spec.numpy_dtype;
        for (auto d : spec.numpy_shape)
        {
            out += ':';
            out += std::to_string(d);
        }
    }
    else
    {
        bool first = true;
        for (const auto &f : spec.fields)
        {
            if (!first) out += '|';
            first = false;
            out += f.name;  out += ':';
            out += f.type_str;  out += ':';
            out += std::to_string(f.count);  out += ':';
            out += std::to_string(f.length);
        }
    }
}

/// @brief Compute the actor-level schema layout declaration hash (Layer 2 of 3).
///
/// This is the second of three independent schema validation layers in pylabhub:
///
/// | Layer | Name                   | What it fingerprints        | When checked  |
/// |-------|------------------------|-----------------------------|---------------|
/// |   1   | Slot data checksum     | Slot memory content (BLAKE2b)| Per write/read|
/// |   2   | Schema declaration hash| JSON field layout (this fn) | At connect    |
/// |   3   | BLDS channel registry  | Channel schema registration | At REG/DISC   |
///
/// Returns raw 32-byte BLAKE2b-256 digest as std::string.
/// Returns empty string when no schema is defined (hash enforcement disabled).
std::string compute_schema_hash(const SchemaSpec &slot_spec, const SchemaSpec &fz_spec)
{
    if (!slot_spec.has_schema && !fz_spec.has_schema)
        return {};

    std::string canonical;
    canonical.reserve(256);
    if (slot_spec.has_schema)
        append_schema_canonical(canonical, "slot:", slot_spec);
    if (fz_spec.has_schema)
        append_schema_canonical(canonical, slot_spec.has_schema ? "|fz:" : "fz:", fz_spec);

    const auto hash =
        pylabhub::crypto::compute_blake2b_array(canonical.data(), canonical.size());
    return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

size_t ctypes_sizeof(const py::object &type_)
{
    py::module_ ct = py::module_::import("ctypes");
    return ct.attr("sizeof")(type_).cast<size_t>();
}

void print_ctypes_layout(const py::object &type_, const char *label, size_t total_size)
{
    py::module_ ct = py::module_::import("ctypes");
    std::cout << "\n" << label << " (ctypes.LittleEndianStructure)\n";
    py::list fields = type_.attr("_fields_");
    size_t   prev_end = 0;
    for (auto item : fields)
    {
        const auto name   = item[py::int_(0)].cast<std::string>();
        const auto desc   = type_.attr(name.c_str());
        const auto offset = desc.attr("offset").cast<size_t>();
        const auto size   = desc.attr("size").cast<size_t>();
        if (offset > prev_end)
            std::cout << "    [" << (offset - prev_end) << " bytes padding]\n";
        std::cout << "    " << name
                  << "  offset=" << offset << "  size=" << size << "\n";
        prev_end = offset + size;
    }
    if (prev_end < total_size)
        std::cout << "    [" << (total_size - prev_end) << " bytes trailing padding]\n";
    std::cout << "  Total: " << total_size
              << " bytes  (ctypes.sizeof = " << ctypes_sizeof(type_) << ")\n";
}

void print_numpy_layout(const py::object &dtype, const SchemaSpec &spec,
                         const char *label)
{
    size_t itemsize = dtype.attr("itemsize").cast<size_t>();
    std::cout << "\n" << label << " (numpy.ndarray)\n";
    std::cout << "  dtype: " << spec.numpy_dtype << "  itemsize=" << itemsize;
    if (!spec.numpy_shape.empty())
    {
        std::cout << "  shape=(";
        for (size_t i = 0; i < spec.numpy_shape.size(); ++i)
        {
            if (i > 0) std::cout << ", ";
            std::cout << spec.numpy_shape[i];
        }
        std::cout << ")";
    }
    std::cout << "\n";
}

bool parse_on_iteration_return(const py::object &ret)
{
    if (ret.is_none())
        return true;
    if (py::isinstance<py::bool_>(ret))
        return ret.cast<bool>();
    LOGGER_ERROR("[actor] on_iteration() must return bool or None — treating as discard");
    return false;
}

/// Common schema build logic for both worker types.
bool build_schema_types(const RoleConfig &cfg,
                         SchemaSpec       &slot_spec,
                         SchemaSpec       &fz_spec,
                         py::object       &slot_type,
                         py::object       &fz_type,
                         size_t           &schema_slot_size,
                         size_t           &schema_fz_size,
                         bool             &has_fz)
{
    try
    {
        if (!cfg.slot_schema_json.is_null())
            slot_spec = parse_schema_json(cfg.slot_schema_json);
        if (!cfg.flexzone_schema_json.is_null())
            fz_spec = parse_schema_json(cfg.flexzone_schema_json);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Schema parse error: {}", e.what());
        return false;
    }

    py::gil_scoped_acquire gil;
    try
    {
        // ── Slot type ─────────────────────────────────────────────────────────
        if (slot_spec.has_schema)
        {
            if (slot_spec.exposure == SlotExposure::Ctypes)
            {
                slot_type         = build_ctypes_struct(slot_spec, "SlotFrame");
                schema_slot_size  = ctypes_sizeof(slot_type);
            }
            else
            {
                slot_type        = build_numpy_dtype(slot_spec);
                schema_slot_size = slot_type.attr("itemsize").cast<size_t>();
                if (!slot_spec.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : slot_spec.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_slot_size = total * schema_slot_size;
                }
            }
        }
        else if (cfg.shm_slot_size > 0)
        {
            schema_slot_size = cfg.shm_slot_size;
        }

        // ── FlexZone type ─────────────────────────────────────────────────────
        if (fz_spec.has_schema)
        {
            has_fz = true;
            if (fz_spec.exposure == SlotExposure::Ctypes)
            {
                fz_type        = build_ctypes_struct(fz_spec, "FlexFrame");
                schema_fz_size = ctypes_sizeof(fz_type);
            }
            else
            {
                fz_type        = build_numpy_dtype(fz_spec);
                schema_fz_size = fz_type.attr("itemsize").cast<size_t>();
                if (!fz_spec.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : fz_spec.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_fz_size = total * schema_fz_size;
                }
            }
            // Round up to 4096-byte page boundary.
            schema_fz_size = (schema_fz_size + 4095U) & ~size_t{4095U};
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Failed to build Python schema types: {}", e.what());
        return false;
    }
    return true;
}

void print_layout(const SchemaSpec &slot_spec, const py::object &slot_type,
                   size_t schema_slot_size,
                   const SchemaSpec &fz_spec, const py::object &fz_type,
                   size_t schema_fz_size,
                   const std::string &role_label)
{
    std::cout << "\nRole: " << role_label << "\n";
    if (slot_spec.has_schema)
    {
        if (slot_spec.exposure == SlotExposure::Ctypes)
            print_ctypes_layout(slot_type, "  Slot layout: SlotFrame", schema_slot_size);
        else
            print_numpy_layout(slot_type, slot_spec, "  Slot layout");
    }
    if (fz_spec.has_schema)
    {
        if (fz_spec.exposure == SlotExposure::Ctypes)
            print_ctypes_layout(fz_type, "  FlexZone layout: FlexFrame", schema_fz_size);
        else
            print_numpy_layout(fz_type, fz_spec, "  FlexZone layout");
    }
}

} // anonymous namespace

} // namespace pylabhub::actor

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
