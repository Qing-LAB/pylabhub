#pragma once
/**
 * @file python_helpers.hpp
 * @brief Python-specific helpers — ctypes, slot views, direction objects, InboxHandle.
 *
 * Contains pybind11-dependent utilities: ctypes struct building, slot view
 * creation, read-only wrappers, numpy views, script import, debug layout
 * printing, direction objects (PyRxChannel/PyTxChannel/PyInboxMsg), and
 * InboxHandle.
 *
 * For clean schema utilities (parse, resolve, convert, hash, size) that
 * do NOT require pybind11, include schema_utils.hpp instead.
 *
 * **Internal header** — requires pybind11. Used only by python_engine.cpp
 * and *_api.cpp. Not part of the public pylabhub-utils API.
 */

#include "utils/schema_utils.hpp"

#include "utils/hub_inbox_queue.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace pylabhub::scripting
{

// ── Python callable check ────────────────────────────────────────────────────

inline bool is_callable(const py::object &obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

// Schema parsing, named schema resolution, and resolve_schema are in schema_utils.hpp.

// ── Slot view builder (unified) ──────────────────────────────────────────────

/// Build a zero-copy Python slot view from raw SHM memory.
///
/// @param spec         hub::SchemaSpec for this slot.
/// @param type         The ctypes type built at init time.
/// @param data         Pointer to raw slot data (may be const or writable).
/// @param size         Size in bytes.
/// @param is_read_side True for consumer/processor in_slot: no-schema path
///                     returns py::bytes (immutable); write side returns bytearray.
///                     Schema path always uses from_buffer() (requires non-const
///                     backing buffer even when the type blocks writes via __setattr__).
///
/// Note: for schema-based types, from_buffer() requires a non-const pointer.
/// The NOLINTNEXTLINE suppresses the const_cast warning — the slot is held alive
/// for the duration of the callback, and writes are blocked at the Python level
/// via the readonly wrapper (for read-side types) or are intentional (write side).
inline py::object make_slot_view(const hub::SchemaSpec &spec, const py::object &type,
                                  const void *data, size_t size, bool is_read_side)
{
    if (!spec.has_schema)
    {
        if (is_read_side)
            return py::bytes(reinterpret_cast<const char *>(data), size);
        return py::bytearray(reinterpret_cast<const char *>(data), size);
    }

    // ctypes.from_buffer() requires a writable buffer regardless of logical intent.
    // Write protection for the read side is enforced via __setattr__ on the type
    // (see wrap_as_readonly_ctypes). Subobject mutation through array subscript
    // is not blocked at the ctypes level — a known limitation of the approach.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto mv = py::memoryview::from_memory(const_cast<void *>(data),
                                           static_cast<py::ssize_t>(size),
                                           /*readonly=*/false);
    return type.attr("from_buffer")(mv);
}

// ── Read-only ctypes wrapper ──────────────────────────────────────────────────

/// Wrap a ctypes.Structure subclass to block accidental field writes.
///
/// Creates a new Python class that inherits from base_type and overrides
/// __setattr__ to raise AttributeError. The override fires only on writes,
/// so reads have zero extra cost. Applied to read-side types (consumer in_slot,
/// processor in_slot) to enforce the read-only contract at the Python level.
///
/// The underlying SHM buffer IS writable (required by from_buffer()), but the
/// __setattr__ guard ensures no script accidentally corrupts a producer's slot.
inline py::object wrap_as_readonly_ctypes(const py::object &base_type)
{
    py::dict ns;
    py::exec(R"PLH(
def _plh_readonly_setattr(self, name, value):
    raise AttributeError(
        "read-only slot: field '{}' cannot be written "
        "(in_slot is a zero-copy SHM view -- use out_slot to write)".format(name)
    )
)PLH",
             py::globals(), ns);

    const std::string name = base_type.attr("__name__").cast<std::string>() + "Readonly";
    py::dict kw;
    kw["__setattr__"] = ns["_plh_readonly_setattr"];
    return py::eval("type")(name.c_str(), py::make_tuple(base_type), kw);
}

// ── ctypes builders ─────────────────────────────────────────────────────────

inline py::object json_type_to_ctypes(py::module_ &ct, const hub::FieldDef &fd)
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

inline py::object build_ctypes_struct(const hub::SchemaSpec &spec, const std::string &name)
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

inline size_t ctypes_sizeof(const py::object &type_)
{
    return py::module_::import("ctypes").attr("sizeof")(type_).cast<size_t>();
}

// ── Numpy view for ctypes array fields ──────────────────────────────────────

/**
 * @brief Convert a ctypes array field to a numpy ndarray view (zero-copy).
 *
 * The dtype is inferred from the ctypes array element type. The returned
 * numpy array shares the same memory — no copy. Writes to the numpy array
 * modify the underlying slot buffer directly.
 *
 * Usage from Python: `pixels = api.as_numpy(slot.pixels)`
 *
 * @param ctypes_array  A ctypes array object (e.g., c_float_Array_307200).
 * @return numpy.ndarray view with inferred dtype.
 * @throws py::type_error if the argument is not a ctypes array.
 */
inline py::object as_numpy_view(py::object ctypes_array)
{
    py::module_ np = py::module_::import("numpy");
    py::module_ ct = py::module_::import("ctypes");

    // Verify it's a ctypes array (has _type_ and _length_ attributes).
    if (!py::hasattr(ctypes_array, "_type_") || !py::hasattr(ctypes_array, "_length_"))
        throw py::type_error("as_numpy: argument must be a ctypes array field "
                             "(e.g., slot.pixels for a field with count > 1)");

    // Get the element ctype (e.g., ctypes.c_float).
    py::object ctype = ctypes_array.attr("_type_");

    // Map ctypes element type to numpy dtype string.
    // Using ctypes sizeof for reliable mapping.
    py::object dtype;
    if (ctype.is(ct.attr("c_float")))       dtype = np.attr("float32");
    else if (ctype.is(ct.attr("c_double"))) dtype = np.attr("float64");
    else if (ctype.is(ct.attr("c_int8")))   dtype = np.attr("int8");
    else if (ctype.is(ct.attr("c_uint8")))  dtype = np.attr("uint8");
    else if (ctype.is(ct.attr("c_int16")))  dtype = np.attr("int16");
    else if (ctype.is(ct.attr("c_uint16"))) dtype = np.attr("uint16");
    else if (ctype.is(ct.attr("c_int32")))  dtype = np.attr("int32");
    else if (ctype.is(ct.attr("c_uint32"))) dtype = np.attr("uint32");
    else if (ctype.is(ct.attr("c_int64")))  dtype = np.attr("int64");
    else if (ctype.is(ct.attr("c_uint64"))) dtype = np.attr("uint64");
    else if (ctype.is(ct.attr("c_bool")))   dtype = np.attr("bool_");
    else
        throw py::type_error("as_numpy: unsupported ctypes element type for numpy conversion");

    return np.attr("frombuffer")(ctypes_array, py::arg("dtype") = dtype);
}

// Schema hash is in schema_utils.hpp.

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

// Schema size computation and ZMQ field conversion are in schema_utils.hpp.

// ── Direction objects — script-level rx/tx/msg wrappers ──────────────────────

/// Receive direction object passed to on_consume(rx, ...) and on_process(rx, ...).
/// rx.slot = typed slot view (read-only), rx.fz = flexzone view (mutable per HEP-0002).
struct PyRxChannel
{
    py::object slot{py::none()};
    py::object fz{py::none()};
};

/// Transmit direction object passed to on_produce(tx, ...) and on_process(..., tx, ...).
/// tx.slot = typed slot view (writable), tx.fz = flexzone view (mutable).
struct PyTxChannel
{
    py::object slot{py::none()};
    py::object fz{py::none()};
};

/// Inbox message object passed to on_inbox(msg, api).
/// msg.data = typed inbox payload (read-only copy), msg.sender_uid, msg.seq.
struct PyInboxMsg
{
    py::object  data{py::none()};
    std::string sender_uid;
    uint64_t    seq{0};
};

// ── InboxHandle — Python-facing wrapper for hub::InboxClient ─────────────────

/**
 * @class InboxHandle
 * @brief Python-visible wrapper for hub::InboxClient.
 *
 * Returned by api.open_inbox(uid). The script holds this object to send typed
 * messages to another role's inbox.
 *
 * Typical script pattern:
 * @code
 *   handle = api.open_inbox("PROD-SENSOR-12345678")  # None if not online
 *   if handle and handle.is_ready():
 *       slot = handle.acquire()
 *       slot.value = 42.0
 *       rc = handle.send()           # 0=OK
 *       if rc != 0:
 *           handle = None            # let check_readiness() rediscover next cycle
 * @endcode
 */
class InboxHandle
{
  public:
    InboxHandle(std::shared_ptr<hub::InboxClient> client,
                hub::SchemaSpec                         spec,
                py::object                         slot_type,
                size_t                             item_size)
        : client_(std::move(client))
        , spec_(std::move(spec))
        , slot_type_(std::move(slot_type))
        , item_size_(item_size)
    {
    }

    /// Returns a writable ctypes slot view backed by the client's write buffer.
    /// Returns None if the client is not connected.
    py::object acquire()
    {
        if (!client_ || !client_->is_running())
            return py::none();
        void *buf = client_->acquire();
        if (!buf)
            return py::none();
        return make_slot_view(spec_, slot_type_, buf, item_size_, /*is_read_side=*/false);
    }

    /// Send the slot contents. Blocks for ACK up to timeout_ms. GIL released during wait.
    /// Returns 0=OK, non-zero=error code, 255=send failure or ACK timeout.
    int send(int timeout_ms = 5000)
    {
        if (!client_ || !client_->is_running())
            return 255;
        py::gil_scoped_release rel;
        return static_cast<int>(client_->send(std::chrono::milliseconds{timeout_ms}));
    }

    /// Discard the current slot without sending. The caller's loop continues normally.
    void discard()
    {
        if (client_)
            client_->abort();
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
        return client_ && client_->is_running();
    }

    /// Stop the underlying client and invalidate this handle.
    /// After calling this, is_ready() returns false; acquire/send return error values.
    void close()
    {
        if (client_)
            client_->stop();
    }

    /// Release Python objects. Call with GIL held before interpreter shutdown.
    void clear_pyobjects()
    {
        slot_type_ = py::none();
    }

  private:
    std::shared_ptr<hub::InboxClient> client_;
    hub::SchemaSpec                         spec_;
    py::object                         slot_type_{py::none()};
    size_t                             item_size_{0};
};

} // namespace pylabhub::scripting
