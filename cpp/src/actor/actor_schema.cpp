/**
 * @file actor_schema.cpp
 * @brief JSON schema parsing for pylabhub-actor slot and flexzone definitions.
 */
#include "actor_schema.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace pylabhub::actor
{

SchemaSpec parse_schema_json(const nlohmann::json &schema_obj)
{
    SchemaSpec spec;
    spec.has_schema = true;

    // ── Determine exposure mode ───────────────────────────────────────────────
    const std::string expose_as = schema_obj.value("expose_as", "ctypes");

    if (expose_as == "numpy_array")
    {
        spec.exposure = SlotExposure::NumpyArray;

        if (!schema_obj.contains("dtype") || !schema_obj["dtype"].is_string())
        {
            throw std::runtime_error(
                "Schema: 'numpy_array' mode requires a 'dtype' string "
                "(e.g. \"float32\")");
        }
        spec.numpy_dtype = schema_obj["dtype"].get<std::string>();

        if (schema_obj.contains("shape") && schema_obj["shape"].is_array())
        {
            for (const auto &dim : schema_obj["shape"])
            {
                if (!dim.is_number_integer())
                {
                    throw std::runtime_error(
                        "Schema: 'shape' entries must be integers");
                }
                spec.numpy_shape.push_back(dim.get<int64_t>());
            }
        }
        return spec;
    }

    if (expose_as != "ctypes")
    {
        throw std::runtime_error(
            "Schema: unknown 'expose_as' value '" + expose_as +
            "' (must be 'ctypes' or 'numpy_array')");
    }

    // ── Ctypes mode ───────────────────────────────────────────────────────────
    spec.exposure = SlotExposure::Ctypes;
    spec.packing  = schema_obj.value("packing", "natural");

    if (spec.packing != "natural" && spec.packing != "packed")
    {
        throw std::runtime_error(
            "Schema: 'packing' must be 'natural' or 'packed', got '" +
            spec.packing + "'");
    }

    if (!schema_obj.contains("fields") || !schema_obj["fields"].is_array())
    {
        throw std::runtime_error(
            "Schema: ctypes mode requires a 'fields' array");
    }

    for (const auto &f : schema_obj["fields"])
    {
        if (!f.contains("name") || !f["name"].is_string())
        {
            throw std::runtime_error(
                "Schema: each field must have a string 'name'");
        }
        if (!f.contains("type") || !f["type"].is_string())
        {
            throw std::runtime_error(
                "Schema: field '" + f["name"].get<std::string>() +
                "' missing 'type'");
        }

        FieldDef fd;
        fd.name     = f["name"].get<std::string>();
        fd.type_str = f["type"].get<std::string>();
        fd.count    = f.value("count",  uint32_t{1});
        fd.length   = f.value("length", uint32_t{0});

        // Validate type string
        static constexpr std::array<std::string_view, 13> kValidTypes = {
            "bool", "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float32", "float64", "string", "bytes"};
        if (!std::any_of(kValidTypes.begin(), kValidTypes.end(),
                [&fd](std::string_view t) { return t == fd.type_str; }))
        {
            throw std::runtime_error(
                "Schema: field '" + fd.name +
                "' has unknown type '" + fd.type_str +
                "' (valid: bool, int8-64, uint8-64, float32/64, string, bytes)");
        }

        // count must be ≥ 1
        if (fd.count == 0)
        {
            throw std::runtime_error(
                "Schema: field '" + fd.name + "' has 'count' = 0 (must be >= 1)");
        }

        // Validate length for string/bytes types
        if ((fd.type_str == "string" || fd.type_str == "bytes") && fd.length == 0)
        {
            throw std::runtime_error(
                "Schema: field '" + fd.name +
                "' of type '" + fd.type_str +
                "' requires a 'length' > 0");
        }

        spec.fields.push_back(std::move(fd));
    }

    if (spec.fields.empty())
    {
        throw std::runtime_error("Schema: 'fields' array must not be empty");
    }

    return spec;
}

} // namespace pylabhub::actor
