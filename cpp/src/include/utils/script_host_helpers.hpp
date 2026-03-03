#pragma once
/**
 * @file script_host_helpers.hpp
 * @brief Shared inline helpers for all script host binaries.
 *
 * These functions were previously duplicated in anonymous namespaces across
 * processor_script_host.cpp, producer_script_host.cpp, and
 * consumer_script_host.cpp.  They are now shared as inline functions in the
 * pylabhub::scripting namespace.
 *
 * **Internal header** — requires pybind11, nlohmann/json, and crypto_utils.
 * Not part of the public pylabhub-utils API.
 */

#include "utils/crypto_utils.hpp"
#include "utils/schema_library.hpp"
#include "utils/script_host_schema.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace py = pybind11;

namespace pylabhub::scripting
{

// ── Python callable check ────────────────────────────────────────────────────

inline bool is_callable(const py::object &obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

// ── Schema parsing ───────────────────────────────────────────────────────────

inline SchemaSpec parse_schema_json(const nlohmann::json &schema_obj)
{
    SchemaSpec spec;
    spec.has_schema = true;

    const std::string expose_as = schema_obj.value("expose_as", "ctypes");

    if (expose_as == "numpy_array")
    {
        spec.exposure = SlotExposure::NumpyArray;
        if (!schema_obj.contains("dtype") || !schema_obj["dtype"].is_string())
            throw std::runtime_error("Schema: 'numpy_array' mode requires a 'dtype' string");
        spec.numpy_dtype = schema_obj["dtype"].get<std::string>();
        if (schema_obj.contains("shape") && schema_obj["shape"].is_array())
        {
            for (const auto &dim : schema_obj["shape"])
            {
                if (!dim.is_number_integer())
                    throw std::runtime_error("Schema: 'shape' entries must be integers");
                spec.numpy_shape.push_back(dim.get<int64_t>());
            }
        }
        return spec;
    }

    if (expose_as != "ctypes")
        throw std::runtime_error("Schema: unknown 'expose_as' value '" + expose_as + "'");

    spec.exposure = SlotExposure::Ctypes;
    spec.packing  = schema_obj.value("packing", "natural");
    if (spec.packing != "natural" && spec.packing != "packed")
        throw std::runtime_error("Schema: 'packing' must be 'natural' or 'packed'");

    if (!schema_obj.contains("fields") || !schema_obj["fields"].is_array())
        throw std::runtime_error("Schema: ctypes mode requires a 'fields' array");

    for (const auto &f : schema_obj["fields"])
    {
        if (!f.contains("name") || !f["name"].is_string())
            throw std::runtime_error("Schema: each field must have a string 'name'");
        if (!f.contains("type") || !f["type"].is_string())
            throw std::runtime_error(
                "Schema: field '" + f["name"].get<std::string>() + "' missing 'type'");

        FieldDef fd;
        fd.name     = f["name"].get<std::string>();
        fd.type_str = f["type"].get<std::string>();
        fd.count    = f.value("count",  uint32_t{1});
        fd.length   = f.value("length", uint32_t{0});

        static constexpr std::array<std::string_view, 13> kValidTypes = {
            "bool", "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float32", "float64", "string", "bytes"};
        if (!std::any_of(kValidTypes.begin(), kValidTypes.end(),
                [&fd](std::string_view t) { return t == fd.type_str; }))
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' has unknown type '" + fd.type_str + "'");

        if (fd.count == 0)
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' has 'count' = 0 (must be >= 1)");

        if ((fd.type_str == "string" || fd.type_str == "bytes") && fd.length == 0)
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' of type '" + fd.type_str +
                "' requires 'length' > 0");

        spec.fields.push_back(std::move(fd));
    }

    if (spec.fields.empty())
        throw std::runtime_error("Schema: 'fields' array must not be empty");

    return spec;
}

// ── Named schema resolution (HEP-CORE-0016 Phase 5) ─────────────────────────

inline SchemaSpec schema_entry_to_spec(const schema::SchemaLayoutDef &layout)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.exposure   = SlotExposure::Ctypes;
    spec.packing    = "natural";

    for (const auto &sf : layout.fields)
    {
        FieldDef fd;
        fd.name = sf.name;
        if (sf.type == "char")
        {
            fd.type_str = "string";
            fd.length   = sf.count;
            fd.count    = 1;
        }
        else
        {
            fd.type_str = sf.type;
            fd.count    = sf.count;
            fd.length   = 0;
        }
        spec.fields.push_back(std::move(fd));
    }
    return spec;
}

inline SchemaSpec resolve_named_schema(const std::string &schema_id, bool use_flexzone,
                                       const char *label)
{
    schema::SchemaLibrary lib(schema::SchemaLibrary::default_search_dirs());
    lib.load_all();
    auto entry = lib.get(schema_id);
    if (!entry)
        throw std::runtime_error(
            std::string("[") + label + "] Named schema '" + schema_id +
            "' not found in schema library");
    const auto &layout = use_flexzone ? entry->flexzone : entry->slot;
    if (layout.fields.empty())
        throw std::runtime_error(
            std::string("[") + label + "] Named schema '" + schema_id +
            "' has no " + (use_flexzone ? "flexzone" : "slot") + " fields");
    return schema_entry_to_spec(layout);
}

inline SchemaSpec resolve_schema(const nlohmann::json &schema_json, bool use_flexzone,
                                 const char *label)
{
    if (schema_json.is_null())
        return {};
    if (schema_json.is_string())
        return resolve_named_schema(schema_json.get<std::string>(), use_flexzone, label);
    return parse_schema_json(schema_json);
}

// ── ctypes / numpy builders ──────────────────────────────────────────────────

inline py::object json_type_to_ctypes(py::module_ &ct, const FieldDef &fd)
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
            throw std::runtime_error("Schema: string field '" + fd.name + "' needs 'length' > 0");
        return ct.attr("c_char").attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else if (fd.type_str == "bytes")
    {
        if (fd.length == 0)
            throw std::runtime_error("Schema: bytes field '" + fd.name + "' needs 'length' > 0");
        base = ct.attr("c_uint8");
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else
        throw std::runtime_error("Schema: unknown type '" + fd.type_str + "'");

    if (fd.count > 1)
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.count)));
    return base;
}

inline py::object build_ctypes_struct(const SchemaSpec &spec, const char *name)
{
    py::module_ ct = py::module_::import("ctypes");
    py::list    fields;
    for (const auto &fd : spec.fields)
        fields.append(py::make_tuple(fd.name, json_type_to_ctypes(ct, fd)));
    py::dict kw;
    kw["_fields_"] = fields;
    if (spec.packing == "packed")
        kw["_pack_"] = py::int_(1);
    return py::eval("type")(name, py::make_tuple(ct.attr("LittleEndianStructure")), kw);
}

inline py::object build_numpy_dtype(const SchemaSpec &spec)
{
    return py::module_::import("numpy").attr("dtype")(spec.numpy_dtype);
}

inline size_t ctypes_sizeof(const py::object &type_)
{
    return py::module_::import("ctypes").attr("sizeof")(type_).cast<size_t>();
}

// ── Schema hash ──────────────────────────────────────────────────────────────

inline void append_schema_canonical(std::string &out, const std::string &prefix,
                                    const SchemaSpec &spec)
{
    out += prefix;
    if (spec.exposure == SlotExposure::NumpyArray)
    {
        out += "numpy_array:";
        out += spec.numpy_dtype;
        for (auto d : spec.numpy_shape) { out += ':'; out += std::to_string(d); }
    }
    else
    {
        bool first = true;
        for (const auto &f : spec.fields)
        {
            if (!first) out += '|';
            first = false;
            out += f.name;    out += ':';
            out += f.type_str; out += ':';
            out += std::to_string(f.count);  out += ':';
            out += std::to_string(f.length);
        }
    }
}

inline std::string compute_schema_hash(const SchemaSpec &slot_spec, const SchemaSpec &fz_spec)
{
    if (!slot_spec.has_schema && !fz_spec.has_schema)
        return {};

    std::string canonical;
    canonical.reserve(256);
    if (slot_spec.has_schema)
        append_schema_canonical(canonical, "slot:", slot_spec);
    if (fz_spec.has_schema)
        append_schema_canonical(canonical, slot_spec.has_schema ? "|fz:" : "fz:", fz_spec);

    const auto hash = pylabhub::crypto::compute_blake2b_array(
        canonical.data(), canonical.size());
    return std::string(reinterpret_cast<const char *>(hash.data()), hash.size());
}

// ── Script import (HEP-CORE-0011 §2.3) ──────────────────────────────────────

inline py::module_ import_role_script_module(const std::string &role_name,
                                             const std::string &module_name,
                                             const std::string &base_dir,
                                             const std::string &uid_hex,
                                             const std::string &script_type)
{
    namespace fs = std::filesystem;

    const fs::path base_path = base_dir.empty()
                               ? fs::current_path()
                               : fs::weakly_canonical(base_dir);

    const fs::path pkg_init = base_path / module_name / script_type / "__init__.py";

    if (!fs::exists(pkg_init))
        throw std::runtime_error(
            "Script not found for '" + role_name +
            "': expected '" + pkg_init.string() + "'");

    const std::string alias     = "_plh_" + uid_hex + "_" + role_name;
    py::module_  importlib_util = py::module_::import("importlib.util");
    py::module_  sys_mod        = py::module_::import("sys");
    py::dict     sys_modules    = sys_mod.attr("modules").cast<py::dict>();

    py::list search_locs;
    search_locs.append(pkg_init.parent_path().string());

    py::object spec = importlib_util.attr("spec_from_file_location")(
        alias, pkg_init.string(),
        py::arg("submodule_search_locations") = search_locs);

    if (spec.is_none())
        throw std::runtime_error(
            "spec_from_file_location failed for '" + role_name +
            "' file '" + pkg_init.string() + "'");

    py::object mod = importlib_util.attr("module_from_spec")(spec);
    sys_modules[alias.c_str()] = mod;
    spec.attr("loader").attr("exec_module")(mod);
    return mod.cast<py::module_>();
}

// ── validate-mode layout printers ────────────────────────────────────────────

inline void print_ctypes_layout(const py::object &type_, const char *label, size_t total_size)
{
    py::module_ ct = py::module_::import("ctypes");
    std::cout << "\n" << label << " (ctypes.LittleEndianStructure)\n";
    py::list fields = type_.attr("_fields_");
    size_t prev_end = 0;
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

inline void print_numpy_layout(const py::object &dtype, const SchemaSpec &spec, const char *label)
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

} // namespace pylabhub::scripting
