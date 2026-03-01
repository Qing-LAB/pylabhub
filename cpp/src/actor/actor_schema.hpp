#pragma once
/**
 * @file actor_schema.hpp
 * @brief Slot and flexzone schema definitions for pylabhub-actor.
 *
 * Two exposure modes:
 *
 * **Ctypes** (default): Fields are declared by name and type. C++ builds a
 * `ctypes.LittleEndianStructure` subclass. Python callbacks receive a typed
 * struct with named attribute access (`slot.count`, `slot.ts`).
 *
 * **NumpyArray**: The entire buffer is treated as a single flat `numpy.ndarray`
 * with a given dtype and optional shape. No named fields — callbacks receive
 * an ndarray directly. Useful when the slot is a homogeneous data array.
 *
 * JSON schema object examples:
 * @code{.json}
 * // Ctypes mode (default):
 * {
 *   "packing": "natural",
 *   "fields": [
 *     {"name": "count", "type": "int64"},
 *     {"name": "ts",    "type": "float64"},
 *     {"name": "buf",   "type": "float32", "count": 64}
 *   ]
 * }
 *
 * // NumpyArray mode:
 * {
 *   "expose_as": "numpy_array",
 *   "dtype":     "float32",
 *   "shape":     [64]
 * }
 * @endcode
 */

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pylabhub::actor
{

// ============================================================================
// SlotExposure
// ============================================================================

/**
 * @enum SlotExposure
 * @brief How the slot buffer is presented to Python callbacks.
 *
 * **Where set:** ActorSchemaBuilder::build() (actor_schema.cpp) parses the JSON
 *   schema's optional `"expose_as"` key. Absent or `"ctypes_struct"` → Ctypes;
 *   `"numpy_array"` → NumpyArray.
 * **Where applied:** actor_api.cpp — build_slot_view() constructs the Python object
 *   passed to on_iteration(slot, flexzone, messages, api) on every iteration.
 *
 * | Value      | Python callback receives                  | Best suited for              |
 * |------------|-------------------------------------------|------------------------------|
 * | Ctypes     | ctypes.LittleEndianStructure subclass      | Heterogeneous named fields   |
 * |            | with typed attribute access (slot.ts, …)   | (timestamps, flags, scalars) |
 * |            | Fields declared by "fields" in schema.     | Human-readable scripts       |
 * | NumpyArray | numpy.ndarray, dtype+shape from schema.   | Homogeneous arrays           |
 * |            | Zero-copy frombuffer() view of SHM slot.   | Vectorized NumPy operations  |
 * |            | No named attributes — direct array access. | DSP / signal processing      |
 *
 * Note: the flexzone buffer is always exposed as a ctypes struct regardless of
 * the slot mode (flexzone has heterogeneous fields by nature).
 *
 * JSON schema `"expose_as"` values: absent | `"ctypes_struct"` → Ctypes;
 * `"numpy_array"` → NumpyArray.
 * **Design doc:** HEP-CORE-0010-Actor-Thread-Model.md §4.1
 */
enum class SlotExposure
{
    Ctypes,    ///< ctypes.LittleEndianStructure with typed named fields (default)
    NumpyArray ///< numpy.ndarray(dtype, shape) — zero-copy frombuffer view of SHM slot
};

// ============================================================================
// FieldDef — one typed field (ctypes mode)
// ============================================================================

/**
 * @struct FieldDef
 * @brief Describes a single field in a ctypes struct schema.
 */
struct FieldDef
{
    std::string name;         ///< Python attribute name
    std::string type_str;     ///< Type token: "bool","int8/16/32/64","uint8/16/32/64",
                              ///<             "float32","float64","string","bytes"
    uint32_t    count{1};     ///< Array length (1 = scalar, N = ctypes array)
    uint32_t    length{0};    ///< For "string"/"bytes": byte length of the fixed-size field
};

// ============================================================================
// SchemaSpec — parsed schema for one buffer (slot or flexzone)
// ============================================================================

/**
 * @struct SchemaSpec
 * @brief Fully-parsed schema for a slot or flexzone buffer.
 *
 * `has_schema` is false when no schema was present in the config (backward-compat
 * legacy mode where `shm.slot_size` drives a raw bytearray slot).
 */
struct SchemaSpec
{
    bool has_schema{false}; ///< false = no schema present (legacy / not configured)

    // ── Ctypes mode ───────────────────────────────────────────────────────────
    SlotExposure          exposure{SlotExposure::Ctypes};
    std::vector<FieldDef> fields;
    std::string           packing{"natural"}; ///< "natural" or "packed"

    // ── NumpyArray mode ───────────────────────────────────────────────────────
    std::string           numpy_dtype{};  ///< e.g. "float32", "complex128"
    std::vector<int64_t>  numpy_shape{};  ///< e.g. {1024} or {32, 64}; empty = auto 1-D
};

// ============================================================================
// parse_schema_json
// ============================================================================

/**
 * @brief Parse a JSON schema object (the value of "slot_schema" or
 *        "flexzone_schema" in the actor config) into a SchemaSpec.
 *
 * @throws std::runtime_error on missing or invalid fields.
 */
SchemaSpec parse_schema_json(const nlohmann::json &schema_obj);

} // namespace pylabhub::actor
