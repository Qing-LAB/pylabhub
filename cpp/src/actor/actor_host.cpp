/**
 * @file actor_host.cpp
 * @brief ProducerRoleWorker, ConsumerRoleWorker, and ActorHost implementations.
 *
 * ## Module import
 *
 * The script is a Python module/package imported via `importlib.import_module`.
 * The module's directory is prepended to `sys.path` once (no duplicates).
 * A synthetic module name `_plh_role_{actor_uid_hex}_{module_name}` is used
 * for `sys.modules` registration to prevent cross-actor namespace collisions.
 *
 * ## ctypes type construction
 *
 * A ctypes.LittleEndianStructure subclass is built once at start() from the
 * JSON schema. ctypes owns alignment/padding. For numpy_array mode, a numpy
 * dtype object is built instead.
 *
 * ## Slot view lifetimes
 *
 * Producer: writable from_buffer — zero-copy into SHM slot.
 *           Do NOT store beyond on_iteration().
 * Consumer: from_buffer on read-only memoryview — zero-copy, field writes raise
 *           TypeError. Do NOT store beyond on_iteration().
 *
 * ## Consumer flexzone
 *
 * Consumer accesses the SHM flexzone directly (zero-copy). A read-only
 * memoryview wraps the SHM pointer — Python cannot write flexzone fields.
 * C++ can still read the pointer (used by is_fz_accepted()).
 *
 * ## Checksum flow (producer per-write)
 *   1. Zero slot buffer.
 *   2. Drain incoming_queue_ (no GIL).
 *   3. Call on_iteration(slot, fz, msgs, api) under GIL.
 *   4. If commit: slot_handle->commit(schema_slot_size_).
 *   5. If Update|Enforce: update_checksum_slot().
 *   6. If Update|Enforce and has_fz_: update_checksum_flexible_zone().
 *   7. release_write_slot().
 *
 * ## Checksum flow (consumer per-read)
 *   1. acquire_consume_slot().
 *   2. If Enforce: verify slot checksum.
 *   3. If Enforce and has_fz_: check is_fz_accepted OR verify fz checksum.
 *   4. Set api_.set_slot_valid().
 *   5. Drain incoming_queue_ (no GIL).
 *   6. Call on_iteration(slot, fz, msgs, api) under GIL.
 *   7. release_consume_slot().
 *
 * ## Incoming queue and thread model (Phase 2 — embedded mode)
 *
 * Producer and Consumer are started via start_embedded() — their internal
 * peer_thread / data_thread / ctrl_thread are NOT launched.
 * zmq_thread_ (per worker) polls ZMQ sockets and calls handle_*_events_nowait().
 * Callbacks push IncomingMessage into incoming_queue_ and notify incoming_cv_.
 * The loop thread drains the queue before each GIL acquisition.
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

namespace pylabhub::actor
{

// ============================================================================
// Anonymous-namespace helpers (shared by both worker types)
// ============================================================================

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

// ============================================================================
// ProducerRoleWorker
// ============================================================================

ProducerRoleWorker::ProducerRoleWorker(const std::string     &role_name,
                                        const RoleConfig      &role_cfg,
                                        const std::string     &actor_uid,
                                        const ActorAuthConfig &auth,
                                        std::atomic<bool>     &shutdown,
                                        const py::module_     &script_module)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , auth_(auth)
    , shutdown_(shutdown)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
    // ── PylabhubEnv: fields available at construction time ────────────────────
    api_.set_channel(role_cfg_.channel);
    api_.set_broker(role_cfg_.broker);
    api_.set_kind_str("producer");
    // actor_name, log_level, script_dir wired by ActorHost::start() before start().

    // ── Look up callbacks from module (under GIL) ─────────────────────────────
    {
        py::gil_scoped_acquire g;
        py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
        py_on_init_      = py::getattr(script_module, "on_init",      py::none());
        py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
    }
}

ProducerRoleWorker::~ProducerRoleWorker()
{
    stop();
}

bool ProducerRoleWorker::build_slot_types_()
{
    return build_schema_types(role_cfg_,
                               slot_spec_, fz_spec_,
                               slot_type_, fz_type_,
                               schema_slot_size_, schema_fz_size_,
                               has_fz_);
}

void ProducerRoleWorker::print_layout_() const
{
    py::gil_scoped_acquire g;
    print_layout(slot_spec_, slot_type_, schema_slot_size_,
                  fz_spec_,  fz_type_,  schema_fz_size_,
                  role_name_ + " [producer]");
}

py::object ProducerRoleWorker::make_slot_view_(void *data, size_t size) const
{
    auto mv = py::memoryview::from_memory(data, static_cast<ssize_t>(size),
                                           /*readonly=*/false);
    if (!slot_spec_.has_schema)
        return py::bytearray(reinterpret_cast<const char *>(data), size);
    if (slot_spec_.exposure == SlotExposure::Ctypes)
        return slot_type_.attr("from_buffer")(mv);
    // numpy_array mode
    py::module_ np = py::module_::import("numpy");
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape) shape.append(d);
        return np.attr("ndarray")(shape, slot_type_, mv);
    }
    size_t itemsize = slot_type_.attr("itemsize").cast<size_t>();
    size_t count    = (itemsize > 0) ? (size / itemsize) : 0;
    return np.attr("ndarray")(py::make_tuple(static_cast<ssize_t>(count)),
                               slot_type_, mv);
}

std::vector<ProducerRoleWorker::IncomingMessage>
ProducerRoleWorker::drain_incoming_queue_()
{
    std::vector<IncomingMessage> msgs;
    std::unique_lock<std::mutex> lock(incoming_mu_);
    if (!incoming_queue_.empty())
    {
        msgs.reserve(incoming_queue_.size());
        for (auto &m : incoming_queue_)
            msgs.push_back(std::move(m));
        incoming_queue_.clear();
    }
    return msgs;
}

py::list ProducerRoleWorker::build_messages_list_(
    std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        lst.append(py::make_tuple(
            m.sender,
            py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
    }
    return lst;
}

bool ProducerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!is_callable(py_on_iteration_))
    {
        LOGGER_WARN("[actor/{}] module has no 'on_iteration' — role not started",
                    role_name_);
        return false;
    }

    if (!build_slot_types_())
        return false;

    // ── Create Producer ───────────────────────────────────────────────────────
    hub::ProducerOptions opts;
    opts.channel_name = role_cfg_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = role_cfg_.has_shm;
    opts.schema_hash  = compute_schema_hash(slot_spec_, fz_spec_);
    opts.actor_name   = api_.actor_name();
    opts.actor_uid    = api_.uid();

    if (role_cfg_.has_shm)
    {
        opts.shm_config.shared_secret        = role_cfg_.shm_secret;
        opts.shm_config.ring_buffer_capacity = role_cfg_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Latest_only;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = schema_fz_size_;

        if (schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4K))
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4K;
            opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size4K);
        }
        else if (schema_slot_size_ <= static_cast<size_t>(hub::DataBlockPageSize::Size4M))
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size4M;
            opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size4M);
        }
        else
        {
            opts.shm_config.physical_page_size = hub::DataBlockPageSize::Size16M;
            opts.shm_config.logical_unit_size  =
                static_cast<size_t>(hub::DataBlockPageSize::Size16M);
        }
    }

    // Connect to the role's configured broker endpoint.
    if (!role_cfg_.broker.empty())
    {
        if (!messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey,
                                auth_.client_pubkey, auth_.client_seckey))
            LOGGER_WARN("[actor] Role '{}': broker connect failed ({}); running degraded",
                        role_name_, role_cfg_.broker);
    }

    auto maybe_producer = hub::Producer::create(messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to create producer for channel '{}'",
                     role_name_, role_cfg_.channel);
        return false;
    }
    producer_ = std::move(maybe_producer);

    // ── Wire LoopPolicy for acquire-side overrun detection (HEP-CORE-0008) ────
    if (auto *shm = producer_->shm(); shm != nullptr)
    {
        shm->set_loop_policy(role_cfg_.loop_policy, role_cfg_.period_ms);
        shm->clear_metrics();
    }

    // ── Wire on_consumer_message → incoming_queue_ ───────────────────────────
    // GIL-safe: peer_thread pushes to queue; loop_thread drains.
    producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data)
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            if (incoming_queue_.size() >= kMaxIncomingQueue)
            {
                LOGGER_WARN("[actor/{}] Incoming message queue full ({}) — dropping",
                            role_name_, kMaxIncomingQueue);
                return;
            }
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            incoming_queue_.push_back(std::move(msg));
            incoming_cv_.notify_one();
        });

    if (!producer_->start_embedded())
    {
        LOGGER_ERROR("[actor/{}] producer->start_embedded() failed", role_name_);
        return false;
    }

    // ── Phase 3: hand heartbeat responsibility to zmq_thread_ ────────────────
    // Suppress the Messenger periodic heartbeat; zmq_thread_ will send
    // application-level HEARTBEAT_REQ only when iteration_count_ advances.
    // Enqueue one immediate heartbeat to keep the channel alive during on_init
    // (in case on_init takes longer than the broker timeout).
    if (!role_cfg_.channel.empty())
    {
        messenger_.suppress_periodic_heartbeat(role_cfg_.channel);
        messenger_.enqueue_heartbeat(role_cfg_.channel);
    }

    // ── Build persistent flexzone view (writable, producer owns it) ───────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_producer(&*producer_);

            if (has_fz_)
            {
                auto *shm = producer_->shm();
                if (shm != nullptr)
                {
                    auto fz_span = shm->flexible_zone_span();
                    fz_mv_  = py::memoryview::from_memory(
                        fz_span.data(),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/false);  // Producer owns flexzone — writable

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape) shape.append(d);
                            fz_inst_ = np.attr("ndarray")(shape, fz_type_, fz_mv_);
                        }
                        else
                        {
                            size_t items = fz_span.size_bytes() /
                                           fz_type_.attr("itemsize").cast<size_t>();
                            fz_inst_ = np.attr("ndarray")(
                                py::make_tuple(static_cast<ssize_t>(items)),
                                fz_type_, fz_mv_);
                        }
                    }
                }
            }
            if (fz_inst_.is_none())
                fz_inst_ = py::none();

            // Wire flexzone pointer into api so api.flexzone() works.
            api_.set_flexzone_obj(&fz_inst_);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor/{}] Failed to build flexzone view: {}", role_name_, e.what());
            return false;
        }
    }

    LOGGER_INFO("[actor/{}] producer started on channel '{}'",
                role_name_, role_cfg_.channel);

    api_.reset_all_role_run_metrics();
    running_.store(true);

    // Launch zmq_thread_ BEFORE on_init so that any ZMQ sends (e.g. api.broadcast)
    // inside on_init are dispatched immediately. Mirrors old behaviour where peer_thread
    // was already running before on_init was called.
    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init();

    // Check if on_init cleared running_ (e.g. via api.stop() in on_init).
    if (!running_.load())
        return true;  // graceful early exit — not an error

    if (role_cfg_.loop_trigger == RoleConfig::LoopTrigger::Shm)
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_messenger(); });

    return true;
}

void ProducerRoleWorker::stop()
{
    // Guard: return only if fully idle (running=false AND no threads to join).
    // If running_=false but a thread is still joinable (e.g. api.stop() was called from
    // on_init, which exits before loop_thread_/zmq_thread_ are launched), we must still
    // join to avoid std::terminate in std::thread's destructor.
    if (!running_.load() && !loop_thread_.joinable() && !zmq_thread_.joinable())
        return;
    running_.store(false);

    // Wake the incoming_cv_ so the loop thread exits wait_for.
    incoming_cv_.notify_all();

    if (loop_thread_.joinable())
        loop_thread_.join();

    if (zmq_thread_.joinable())
        zmq_thread_.join();

    call_on_stop();

    if (producer_.has_value())
    {
        producer_->stop();
        producer_->close();
        producer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_flexzone_obj(nullptr);
        api_.set_producer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("[actor/{}] producer stopped", role_name_);
}

void ProducerRoleWorker::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_init error: {}", role_name_, e.what());
    }
    // Update flexzone checksum after on_init writes.
    if (has_fz_)
    {
        if (auto *shm = producer_->shm())
            (void)shm->update_checksum_flexible_zone();
    }
}

void ProducerRoleWorker::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !producer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

bool ProducerRoleWorker::call_on_iteration_(py::object &slot,
                                              py::object &fz,
                                              py::list   &msgs)
{
    py::object ret;
    try
    {
        ret = py_on_iteration_(slot, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_iteration error: {}", role_name_, e.what());
        if (role_cfg_.validation.on_python_error == ValidationPolicy::OnPyError::Stop)
            running_.store(false);
        return false;
    }
    return parse_on_iteration_return(ret);
}

bool ProducerRoleWorker::step_write_deadline_(
    std::chrono::steady_clock::time_point &next_deadline)
{
    if (role_cfg_.interval_ms > 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_deadline)
        {
            std::this_thread::sleep_for(next_deadline - now);
            next_deadline += std::chrono::milliseconds(role_cfg_.interval_ms);
        }
        else
        {
            // Overrun: deadline already past — no sleep needed.
            api_.increment_loop_overruns();
            if (role_cfg_.loop_timing == RoleConfig::LoopTimingPolicy::Compensating)
                next_deadline += std::chrono::milliseconds(role_cfg_.interval_ms);
            else // FixedPace
                next_deadline = now + std::chrono::milliseconds(role_cfg_.interval_ms);
        }
        if (!running_.load() || shutdown_.load())
            return false;
    }
    // interval_ms == 0: run at full throughput — no sleep, no overrun tracking.
    return true;
}

void ProducerRoleWorker::run_loop_shm()
{
    auto *shm = producer_->shm();
    if (shm == nullptr)
    {
        LOGGER_ERROR("[actor/{}] SHM unavailable despite has_shm=true", role_name_);
        running_.store(false);
        return;
    }

    const auto &val = role_cfg_.validation;

    // Deadline initialised to now so the first write fires immediately.
    auto next_deadline = std::chrono::steady_clock::now();

    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        if (!step_write_deadline_(next_deadline))
            break;

        // ── Acquire SHM write slot ────────────────────────────────────────────
        // Timeout budget = interval_ms (scheduled mode) or kShmMaxRateMs (max-rate).
        // Rationale: after step_write_deadline_ returns, next_deadline is interval_ms
        // away. Using interval_ms as acquire budget means a slot miss is treated as an
        // overrun on the next step_write_deadline_ call — no false overruns.
        static constexpr int kShmMaxRateMs = 5;
        const int acquire_ms = role_cfg_.interval_ms > 0 ? role_cfg_.interval_ms
                                                          : kShmMaxRateMs;
        const auto work_start = std::chrono::steady_clock::now();
        auto slot_handle = shm->acquire_write_slot(acquire_ms);
        if (!slot_handle)
            continue;
        if (!running_.load() || shutdown_.load())
            break;

        // ── Drain incoming queue (no GIL) ─────────────────────────────────────
        auto msgs = drain_incoming_queue_();

        auto   span        = slot_handle->buffer_span();
        size_t write_bytes = std::min(span.size_bytes(), schema_slot_size_);
        std::memset(span.data(), 0, write_bytes);

        bool commit = false;
        {
            py::gil_scoped_acquire g;
            py::object slot = make_slot_view_(span.data(), write_bytes);
            py::list   mlst = build_messages_list_(msgs);
            commit = call_on_iteration_(slot, fz_inst_, mlst);
        }

        if (commit)
        {
            (void)slot_handle->commit(write_bytes);
            if (val.slot_checksum != ValidationPolicy::Checksum::None)
                (void)slot_handle->update_checksum_slot();
            if (has_fz_ && val.flexzone_checksum != ValidationPolicy::Checksum::None)
                (void)slot_handle->update_checksum_flexible_zone();
        }
        (void)shm->release_write_slot(*slot_handle);
        if (commit)
        {
            api_.set_last_cycle_work_us(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - work_start).count()));
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ProducerRoleWorker::run_loop_messenger()
{
    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        // Wait for incoming messages or timeout.
        std::vector<IncomingMessage> msgs;
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            incoming_cv_.wait_for(
                lock,
                std::chrono::milliseconds(role_cfg_.messenger_poll_ms),
                [this] {
                    return !incoming_queue_.empty() || !running_.load() || shutdown_.load();
                });
            if (!incoming_queue_.empty())
            {
                msgs.reserve(incoming_queue_.size());
                for (auto &m : incoming_queue_)
                    msgs.push_back(std::move(m));
                incoming_queue_.clear();
            }
        }

        if (!running_.load() || shutdown_.load())
            break;

        {
            py::gil_scoped_acquire g;
            py::object slot = py::none();
            py::list   mlst = build_messages_list_(msgs);
            call_on_iteration_(slot, fz_inst_, mlst);
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ProducerRoleWorker::run_zmq_thread_()
{
    void *peer_sock = producer_->peer_ctrl_socket_handle();
    if (peer_sock == nullptr)
        return;  // no broker/peer connection — nothing to poll

    zmq_pollitem_t items[1] = {{peer_sock, 0, ZMQ_POLLIN, 0}};
    uint64_t last_iter{0};

    // Phase 3: application-level heartbeat throttle.
    // Compute heartbeat interval from config:
    //   heartbeat_interval_ms > 0 → use it directly
    //   heartbeat_interval_ms = 0, interval_ms > 0 → 10 × interval_ms
    //   heartbeat_interval_ms = 0, interval_ms = 0 (max-rate) → 2000 ms
    const auto hb_interval = [&]() -> std::chrono::milliseconds
    {
        if (role_cfg_.heartbeat_interval_ms > 0)
            return std::chrono::milliseconds{role_cfg_.heartbeat_interval_ms};
        if (role_cfg_.interval_ms > 0)
            return std::chrono::milliseconds{role_cfg_.interval_ms * 10};
        return std::chrono::milliseconds{2000};
    }();
    // Initialise to (now - hb_interval) so the first iteration advance fires immediately.
    auto last_heartbeat = std::chrono::steady_clock::now() - hb_interval;

    while (running_.load(std::memory_order_relaxed) && !shutdown_.load(std::memory_order_relaxed))
    {
        const int rc = zmq_poll(items, 1, role_cfg_.messenger_poll_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            LOGGER_WARN("[actor/{}/zmq_thread] zmq_poll error: {}", role_name_, zmq_strerror(errno));
            break;
        }
        if (rc > 0 && (items[0].revents & ZMQ_POLLIN))
            producer_->handle_peer_events_nowait();

        // Send HEARTBEAT_REQ when iteration_count_ has advanced AND the throttle
        // window has elapsed.  Application-level liveness: a stalled loop (GIL
        // deadlock, SHM full) stops heartbeats even if TCP is alive.
        const uint64_t cur = iteration_count_.load(std::memory_order_relaxed);
        if (cur != last_iter)
        {
            last_iter = cur;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat >= hb_interval)
            {
                messenger_.enqueue_heartbeat(role_cfg_.channel);
                last_heartbeat = now;
            }
        }
    }
}

// ============================================================================
// ConsumerRoleWorker
// ============================================================================

ConsumerRoleWorker::ConsumerRoleWorker(const std::string     &role_name,
                                        const RoleConfig      &role_cfg,
                                        const std::string     &actor_uid,
                                        const ActorAuthConfig &auth,
                                        std::atomic<bool>     &shutdown,
                                        const py::module_     &script_module)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , auth_(auth)
    , shutdown_(shutdown)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
    // ── PylabhubEnv: fields available at construction time ────────────────────
    api_.set_channel(role_cfg_.channel);
    api_.set_broker(role_cfg_.broker);
    api_.set_kind_str("consumer");
    // actor_name, log_level, script_dir wired by ActorHost::start() before start().

    // ── Look up callbacks from module (under GIL) ─────────────────────────────
    {
        py::gil_scoped_acquire g;
        py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
        py_on_init_      = py::getattr(script_module, "on_init",      py::none());
        py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
    }
}

ConsumerRoleWorker::~ConsumerRoleWorker()
{
    stop();
}

bool ConsumerRoleWorker::build_slot_types_()
{
    return build_schema_types(role_cfg_,
                               slot_spec_, fz_spec_,
                               slot_type_, fz_type_,
                               schema_slot_size_, schema_fz_size_,
                               has_fz_);
}

void ConsumerRoleWorker::print_layout_() const
{
    py::gil_scoped_acquire g;
    print_layout(slot_spec_, slot_type_, schema_slot_size_,
                  fz_spec_,  fz_type_,  schema_fz_size_,
                  role_name_ + " [consumer]");
}

py::object ConsumerRoleWorker::make_slot_view_readonly_(const void *data,
                                                          size_t      size) const
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto mv = py::memoryview::from_memory(const_cast<void *>(data),
                                           static_cast<ssize_t>(size),
                                           /*readonly=*/true);
    if (!slot_spec_.has_schema)
        return py::bytes(reinterpret_cast<const char *>(data), size);

    if (slot_spec_.exposure == SlotExposure::Ctypes)
        return slot_type_.attr("from_buffer")(mv);

    py::module_ np  = py::module_::import("numpy");
    py::object  arr = np.attr("frombuffer")(mv, slot_type_);
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape) shape.append(d);
        arr = arr.attr("reshape")(shape);
    }
    return arr;
}

std::vector<ConsumerRoleWorker::IncomingMessage>
ConsumerRoleWorker::drain_incoming_queue_()
{
    std::vector<IncomingMessage> msgs;
    std::unique_lock<std::mutex> lock(incoming_mu_);
    if (!incoming_queue_.empty())
    {
        msgs.reserve(incoming_queue_.size());
        for (auto &m : incoming_queue_)
            msgs.push_back(std::move(m));
        incoming_queue_.clear();
    }
    return msgs;
}

py::list ConsumerRoleWorker::build_messages_list_(
    std::vector<IncomingMessage> &msgs)
{
    py::list lst;
    for (auto &m : msgs)
    {
        lst.append(py::make_tuple(
            m.sender,
            py::bytes(reinterpret_cast<const char *>(m.data.data()), m.data.size())));
    }
    return lst;
}

bool ConsumerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!is_callable(py_on_iteration_))
    {
        LOGGER_WARN("[actor/{}] module has no 'on_iteration' — role not started",
                    role_name_);
        return false;
    }

    if (!build_slot_types_())
        return false;

    hub::ConsumerOptions opts;
    opts.channel_name         = role_cfg_.channel;
    opts.shm_shared_secret    = role_cfg_.has_shm ? role_cfg_.shm_secret : 0U;
    opts.expected_schema_hash = compute_schema_hash(slot_spec_, fz_spec_);
    opts.consumer_uid         = api_.uid();
    opts.consumer_name        = api_.actor_name();

    if (!role_cfg_.broker.empty())
    {
        if (!messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey,
                                auth_.client_pubkey, auth_.client_seckey))
            LOGGER_WARN("[actor] Role '{}': broker connect failed ({}); running degraded",
                        role_name_, role_cfg_.broker);
    }

    auto maybe_consumer = hub::Consumer::connect(messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to connect consumer to channel '{}'",
                     role_name_, role_cfg_.channel);
        return false;
    }
    consumer_ = std::move(maybe_consumer);

    // ── Wire LoopPolicy for acquire-side overrun detection (HEP-CORE-0008) ────
    if (auto *shm = consumer_->shm(); shm != nullptr)
    {
        shm->set_loop_policy(role_cfg_.loop_policy, role_cfg_.period_ms);
        shm->clear_metrics();
    }

    // ── Wire on_zmq_data → incoming_queue_ ───────────────────────────────────
    // GIL-safe: data_thread pushes to queue; loop_thread drains.
    consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            if (incoming_queue_.size() >= kMaxIncomingQueue)
            {
                LOGGER_WARN("[actor/{}] Incoming message queue full ({}) — dropping",
                            role_name_, kMaxIncomingQueue);
                return;
            }
            IncomingMessage msg;
            msg.sender = {};  // ZMQ broadcast has no sender identity
            msg.data.assign(data.begin(), data.end());
            incoming_queue_.push_back(std::move(msg));
            incoming_cv_.notify_one();
        });

    if (!consumer_->start_embedded())
    {
        LOGGER_ERROR("[actor/{}] consumer->start_embedded() failed", role_name_);
        return false;
    }

    // ── Build API and persistent flexzone view ────────────────────────────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_consumer(&*consumer_);

            if (has_fz_)
            {
                auto *shm = consumer_->shm();
                if (shm != nullptr)
                {
                    const auto fz_span = shm->flexible_zone_span();

                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    fz_mv_ = py::memoryview::from_memory(
                        const_cast<std::byte *>(fz_span.data()),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/true);

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        py::object  arr = np.attr("frombuffer")(fz_mv_, fz_type_);
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape) shape.append(d);
                            arr = arr.attr("reshape")(shape);
                        }
                        fz_inst_ = arr;
                    }

                    if (role_cfg_.validation.flexzone_checksum ==
                        ValidationPolicy::Checksum::Enforce)
                    {
                        const bool fz_ok = shm->verify_checksum_flexible_zone();
                        if (!fz_ok)
                            LOGGER_WARN("[actor/{}] Initial flexzone checksum failed",
                                        role_name_);
                    }
                }
            }
            if (fz_inst_.is_none())
                fz_inst_ = py::none();

            // Wire flexzone pointer into api so api.flexzone() works.
            api_.set_flexzone_obj(&fz_inst_);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor/{}] Failed to build consumer flexzone view: {}",
                         role_name_, e.what());
            return false;
        }
    }

    LOGGER_INFO("[actor/{}] consumer connected to channel '{}'",
                role_name_, role_cfg_.channel);

    api_.reset_all_role_run_metrics();
    running_.store(true);

    // Launch zmq_thread_ BEFORE on_init so that ctrl events (e.g. channel-closing
    // notifications) and queued ctrl sends are processed immediately. Mirrors old
    // behaviour where ctrl_thread was already running before on_init was called.
    zmq_thread_ = std::thread([this] { run_zmq_thread_(); });

    call_on_init();

    // Check if on_init cleared running_ via api.stop().
    if (!running_.load())
        return true;

    if (role_cfg_.loop_trigger == RoleConfig::LoopTrigger::Shm &&
        consumer_->has_shm())
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_messenger(); });

    return true;
}

void ConsumerRoleWorker::stop()
{
    // Guard: return only if fully idle (running=false AND no threads to join).
    // Covers the on_init api.stop() early-exit case — zmq_thread_ may be joinable
    // while running_ is already false.
    if (!running_.load() && !loop_thread_.joinable() && !zmq_thread_.joinable())
        return;
    running_.store(false);

    // Wake the incoming_cv_ so the loop thread exits wait_for.
    incoming_cv_.notify_all();

    if (loop_thread_.joinable())
        loop_thread_.join();

    if (zmq_thread_.joinable())
        zmq_thread_.join();

    call_on_stop();

    if (consumer_.has_value())
    {
        consumer_->stop();
        consumer_->close();
        consumer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_flexzone_obj(nullptr);
        api_.set_consumer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("[actor/{}] consumer stopped", role_name_);
}

void ConsumerRoleWorker::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_init error: {}", role_name_, e.what());
    }
}

void ConsumerRoleWorker::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !consumer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

void ConsumerRoleWorker::call_on_iteration_(py::object &slot,
                                              py::object &fz,
                                              py::list   &msgs)
{
    try
    {
        py_on_iteration_(slot, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[actor/{}] on_iteration error: {}", role_name_, e.what());
        if (role_cfg_.validation.on_python_error == ValidationPolicy::OnPyError::Stop)
            running_.store(false);
    }
}

void ConsumerRoleWorker::run_loop_shm()
{
    auto *shm = consumer_->shm();
    if (shm == nullptr)
    {
        LOGGER_WARN("[actor/{}] SHM unavailable; falling back to messenger loop",
                    role_name_);
        run_loop_messenger();
        return;
    }

    const auto &val = role_cfg_.validation;

    // Timeout budget for acquire_consume_slot:
    //   timeout_ms > 0  → use timeout_ms as the wait budget; fire on_iteration(None)
    //                      on timeout (silence notification for watchdog / heartbeat use).
    //   timeout_ms == 0 → kShmMaxRateMs: max-rate spin; no timeout callback on miss.
    //   timeout_ms == -1→ kShmBlockMs: near-indefinite wait; bounded for shutdown
    //                      responsiveness; no timeout callback on miss.
    static constexpr int kShmMaxRateMs  = 5;
    static constexpr int kShmBlockMs    = 5000;
    const int acquire_ms = role_cfg_.timeout_ms > 0  ? role_cfg_.timeout_ms
                         : role_cfg_.timeout_ms == 0 ? kShmMaxRateMs
                         : kShmBlockMs;

    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        auto slot_handle = shm->acquire_consume_slot(acquire_ms);
        if (!slot_handle)
        {
            // Slot not available within the timeout window.
            auto msgs = drain_incoming_queue_();
            if (role_cfg_.timeout_ms > 0 || !msgs.empty())
            {
                // Notify script: silence interval OR queued ZMQ messages to deliver.
                py::gil_scoped_acquire g;
                py::object none_slot = py::none();
                py::list   mlst      = build_messages_list_(msgs);
                call_on_iteration_(none_slot, fz_inst_, mlst);
                iteration_count_.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        if (!running_.load() || shutdown_.load())
            break;

        const auto   span     = slot_handle->buffer_span();
        const size_t read_sz  = std::min(span.size_bytes(), schema_slot_size_);

        // ── Slot checksum enforcement ─────────────────────────────────────────
        bool slot_ok = true;
        if (val.slot_checksum == ValidationPolicy::Checksum::Enforce)
        {
            slot_ok = slot_handle->verify_checksum_slot();
            if (!slot_ok)
                LOGGER_WARN("[actor/{}] Slot checksum failed (slot={})",
                            role_name_, slot_handle->slot_id());
        }

        // ── FlexZone checksum enforcement ─────────────────────────────────────
        bool fz_ok = true;
        if (has_fz_ && val.flexzone_checksum == ValidationPolicy::Checksum::Enforce)
        {
            const auto fz_span = slot_handle->flexible_zone_span();
            if (api_.is_fz_accepted(fz_span))
                fz_ok = true;
            else
            {
                fz_ok = slot_handle->verify_checksum_flexible_zone();
                if (!fz_ok)
                    LOGGER_WARN("[actor/{}] FlexZone checksum failed", role_name_);
            }
        }

        const bool overall_valid = slot_ok && fz_ok;
        bool       call_iter     = overall_valid;
        if (!overall_valid &&
            val.on_checksum_fail == ValidationPolicy::OnFail::Pass)
            call_iter = true;

        api_.set_slot_valid(overall_valid);

        // ── Drain incoming queue (no GIL) ─────────────────────────────────────
        auto msgs = drain_incoming_queue_();

        if (call_iter)
        {
            py::gil_scoped_acquire g;
            py::object slot_obj = make_slot_view_readonly_(span.data(), read_sz);
            py::list   mlst     = build_messages_list_(msgs);
            call_on_iteration_(slot_obj, fz_inst_, mlst);
        }
        else if (!msgs.empty())
        {
            // Slot skipped due to checksum failure but we still deliver messages.
            py::gil_scoped_acquire g;
            py::object none_slot = py::none();
            py::list   mlst      = build_messages_list_(msgs);
            call_on_iteration_(none_slot, fz_inst_, mlst);
        }

        (void)shm->release_consume_slot(*slot_handle);

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ConsumerRoleWorker::run_loop_messenger()
{
    while (running_.load() && !shutdown_.load())
    {
        if (api_.critical_error())
            break;

        std::vector<IncomingMessage> msgs;
        {
            std::unique_lock<std::mutex> lock(incoming_mu_);
            incoming_cv_.wait_for(
                lock,
                std::chrono::milliseconds(role_cfg_.messenger_poll_ms),
                [this] {
                    return !incoming_queue_.empty() || !running_.load() || shutdown_.load();
                });
            if (!incoming_queue_.empty())
            {
                msgs.reserve(incoming_queue_.size());
                for (auto &m : incoming_queue_)
                    msgs.push_back(std::move(m));
                incoming_queue_.clear();
            }
        }

        if (!running_.load() || shutdown_.load())
            break;

        {
            py::gil_scoped_acquire g;
            py::object slot = py::none();
            py::list   mlst = build_messages_list_(msgs);
            call_on_iteration_(slot, fz_inst_, mlst);
        }

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        if (api_.critical_error())
            break;
    }
}

void ConsumerRoleWorker::run_zmq_thread_()
{
    void *data_sock = consumer_->data_zmq_socket_handle();  // nullptr for Bidir pattern
    void *ctrl_sock = consumer_->ctrl_zmq_socket_handle();
    if (ctrl_sock == nullptr)
        return;  // no ZMQ connection — nothing to poll

    zmq_pollitem_t items[2];
    int nfds = 0;
    if (data_sock != nullptr)
        items[nfds++] = {data_sock, 0, ZMQ_POLLIN, 0};
    const int ctrl_idx = nfds;
    items[nfds++] = {ctrl_sock, 0, ZMQ_POLLIN, 0};

    uint64_t last_iter{0};

    while (running_.load(std::memory_order_relaxed) && !shutdown_.load(std::memory_order_relaxed))
    {
        const int rc = zmq_poll(items, nfds, role_cfg_.messenger_poll_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            LOGGER_WARN("[actor/{}/zmq_thread] zmq_poll error: {}", role_name_, zmq_strerror(errno));
            break;
        }
        if (rc > 0)
        {
            if (data_sock != nullptr && (items[0].revents & ZMQ_POLLIN))
                consumer_->handle_data_events_nowait();
            if (items[ctrl_idx].revents & ZMQ_POLLIN)
                consumer_->handle_ctrl_events_nowait();
        }

        // Consumers do not own the channel; heartbeat responsibility stays with
        // the producer's zmq_thread_.  Track iteration_count_ for future use
        // (e.g. consumer-side application metrics).
        const uint64_t cur = iteration_count_.load(std::memory_order_relaxed);
        if (cur != last_iter)
            last_iter = cur;
    }
}

// ============================================================================
// ActorHost
// ============================================================================

ActorHost::ActorHost(const ActorConfig &config)
    : config_(config)
{
}

ActorHost::~ActorHost()
{
    stop();
}

bool ActorHost::load_script(bool verbose)
{
    script_loaded_ = false;
    role_modules_.clear();

    if (config_.roles.empty())
    {
        LOGGER_WARN("[actor] No roles configured — nothing to load");
        return false;
    }

    // Derive a short hex suffix from actor_uid for module aliasing.
    const std::string uid_hex = config_.actor_uid.size() >= 8
        ? config_.actor_uid.substr(config_.actor_uid.size() - 8)
        : config_.actor_uid;

    py::gil_scoped_acquire g;

    // ── Step 1: load actor-level module (fallback for roles without per-role script) ──
    py::module_ actor_module{};
    bool        actor_module_valid = false;

    if (!config_.script_module.empty())
    {
        try
        {
            actor_module       = import_script_module(
                config_.script_module, config_.script_base_dir, uid_hex);
            actor_module_valid = true;
            LOGGER_INFO("[actor] Actor-level script '{}' loaded", config_.script_module);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor] Actor-level script '{}' load error: {}",
                         config_.script_module, e.what());
            if (verbose)
                std::cerr << "Script error: " << e.what() << "\n";
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[actor] Actor-level script '{}' load error: {}",
                         config_.script_module, e.what());
            if (verbose)
                std::cerr << "Error: " << e.what() << "\n";
        }
    }

    // ── Step 2: resolve per-role modules (per-role overrides actor-level fallback) ────
    bool any_module = false;

    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (!role_cfg.script_module.empty())
        {
            // Per-role script: isolated load via spec_from_file_location.
            try
            {
                auto mod = import_role_script_module(
                    role_name, role_cfg.script_module,
                    role_cfg.script_base_dir, uid_hex);
                role_modules_[role_name] = mod;
                any_module = true;

                py::object on_iter = py::getattr(mod, "on_iteration", py::none());
                LOGGER_INFO("[actor/{}] Per-role script '{}' loaded{}",
                            role_name, role_cfg.script_module,
                            on_iter.is_none() ? " (WARNING: no on_iteration)" : "");
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("[actor/{}] Script load error: {}", role_name, e.what());
                if (verbose)
                    std::cerr << "Script error [" << role_name << "]: " << e.what() << "\n";
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[actor/{}] Script load error: {}", role_name, e.what());
                if (verbose)
                    std::cerr << "Error [" << role_name << "]: " << e.what() << "\n";
            }
        }
        else if (actor_module_valid)
        {
            // Fallback: roles without a per-role script share the actor-level module.
            role_modules_[role_name] = actor_module;
            any_module = true;
        }
        else
        {
            LOGGER_WARN("[actor/{}] No script configured — role will be skipped", role_name);
        }
    }

    if (!any_module)
    {
        LOGGER_ERROR("[actor] No script modules loaded — "
                     "add \"script\": {{\"module\": \"script\", \"path\": \"./roles/<role>\"}} "
                     "to each role in actor.json");
        if (verbose)
            std::cerr << "Error: No script modules loaded.\n"
                         "Add a \"script\" key to each role (or a top-level \"script\" fallback).\n";
        return false;
    }

    if (verbose)
    {
        std::cout << "\nActor uid: "
                  << (config_.actor_uid.empty() ? "(auto)" : config_.actor_uid) << "\n";
        print_role_summary();
    }

    script_loaded_ = true;
    return true;
}

bool ActorHost::start()
{
    if (!script_loaded_)
        return false;

    bool any_started = false;

    // ── Start producer roles ──────────────────────────────────────────────────
    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (role_cfg.kind != RoleConfig::Kind::Producer)
            continue;

        const auto mod_it = role_modules_.find(role_name);
        if (mod_it == role_modules_.end())
        {
            LOGGER_WARN("[actor/{}] No script module — skipping producer role", role_name);
            continue;
        }

        // script_dir shown in api.script_dir(): prefer per-role, fall back to actor-level.
        const std::string script_dir = role_cfg.script_base_dir.empty()
                                       ? config_.script_base_dir
                                       : role_cfg.script_base_dir;

        auto worker = std::make_unique<ProducerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, config_.auth, shutdown_,
            mod_it->second);

        worker->set_env_actor_name(config_.actor_name);
        worker->set_env_log_level(config_.log_level);
        worker->set_env_script_dir(script_dir);

        if (!worker->start())
        {
            LOGGER_ERROR("[actor] Failed to start producer role '{}'", role_name);
            continue;
        }
        producers_[role_name] = std::move(worker);
        any_started = true;
    }

    // ── Start consumer roles ──────────────────────────────────────────────────
    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (role_cfg.kind != RoleConfig::Kind::Consumer)
            continue;

        const auto mod_it = role_modules_.find(role_name);
        if (mod_it == role_modules_.end())
        {
            LOGGER_WARN("[actor/{}] No script module — skipping consumer role", role_name);
            continue;
        }

        const std::string script_dir = role_cfg.script_base_dir.empty()
                                       ? config_.script_base_dir
                                       : role_cfg.script_base_dir;

        auto worker = std::make_unique<ConsumerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, config_.auth, shutdown_,
            mod_it->second);

        worker->set_env_actor_name(config_.actor_name);
        worker->set_env_log_level(config_.log_level);
        worker->set_env_script_dir(script_dir);

        if (!worker->start())
        {
            LOGGER_ERROR("[actor] Failed to start consumer role '{}'", role_name);
            continue;
        }
        consumers_[role_name] = std::move(worker);
        any_started = true;
    }

    return any_started;
}

void ActorHost::stop()
{
    for (auto &[name, worker] : producers_)
        worker->stop();
    producers_.clear();

    for (auto &[name, worker] : consumers_)
        worker->stop();
    consumers_.clear();
}

bool ActorHost::is_running() const noexcept
{
    for (const auto &[_, w] : producers_)
        if (w->is_running()) return true;
    for (const auto &[_, w] : consumers_)
        if (w->is_running()) return true;
    return false;
}

void ActorHost::wait_for_shutdown()
{
    while (!shutdown_.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));
}

void ActorHost::print_role_summary() const
{
    std::cout << "\nConfigured roles:\n";
    for (const auto &[name, cfg] : config_.roles)
    {
        const char *kind_str = (cfg.kind == RoleConfig::Kind::Producer)
            ? "producer" : "consumer";

        const auto mod_it = role_modules_.find(name);
        const bool has_module = (mod_it != role_modules_.end());

        bool has_iteration = false;
        if (has_module && script_loaded_)
        {
            py::gil_scoped_acquire g;
            has_iteration =
                !py::getattr(mod_it->second, "on_iteration", py::none()).is_none();
        }

        const char *status = has_iteration   ? "ACTIVATED"
                           : has_module      ? "no on_iteration"
                           :                   "no script";

        std::cout << "  " << name << "  [" << kind_str << "]"
                  << "  channel=" << cfg.channel
                  << "  " << status << "\n";
    }
}

} // namespace pylabhub::actor
