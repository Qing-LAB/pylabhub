/**
 * @file actor_host.cpp
 * @brief ProducerActorHost and ConsumerActorHost implementations.
 *
 * ## ctypes type construction (build_slot_types_)
 *
 * For ctypes mode a `LittleEndianStructure` subclass is built once at start():
 *   type("SlotFrame", (ctypes.LittleEndianStructure,), {"_fields_": [...], "_pack_": 1})
 * ctypes owns alignment/padding; the actor just passes field name + ctypes type.
 *
 * For numpy_array mode a numpy dtype object is stored; per-cycle slot views are
 * built with numpy.ndarray(shape, dtype, buffer=memoryview).
 *
 * ## Checksum flow
 *
 * The DataBlock is always created with ChecksumPolicy::Manual so that checksum
 * timing is controlled entirely by the actor layer (not auto-enforced by DataBlock).
 *
 * Producer per-write:
 *   1. Zero slot buffer.
 *   2. Call on_write(slot, flexzone, api) under GIL.
 *   3. If commit: slot_handle->commit(schema_slot_size_).
 *   4. If Update|Enforce: slot_handle->update_checksum_slot().
 *   5. If Update|Enforce and has_fz_: slot_handle->update_checksum_flexible_zone().
 *   6. release_write_slot().
 *
 * Consumer per-read:
 *   1. acquire_consume_slot().
 *   2. If Enforce: verify slot checksum → if fail, apply on_checksum_fail policy.
 *   3. If Enforce and has_fz_: check accepted OR verify fz checksum.
 *   4. Set api_.set_slot_valid(pass).
 *   5. Call on_read(slot, flexzone, api) under GIL.
 *   6. release_consume_slot().
 */
#include "actor_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace py  = pybind11;
namespace fs  = std::filesystem;

namespace pylabhub::actor
{

// ============================================================================
// Anonymous-namespace helpers
// ============================================================================

namespace
{

/// Load a Python attribute by name; return py::none() if absent.
py::object try_get_attr(py::module_ &mod, const char *name)
{
    if (py::hasattr(mod, name))
        return mod.attr(name);
    return py::none();
}

/// True when a py::object is a callable Python function.
bool is_callable(const py::object &obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

/// Execute a Python script file and return the resulting pseudo-module.
py::module_ exec_script_file(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Runner: cannot open script: " + path);

    std::string code((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    // Prepend script directory to sys.path so relative imports work.
    const fs::path script_dir = fs::path(path).parent_path();
    py::module_    sys        = py::module_::import("sys");
    py::list       sys_path   = sys.attr("path").cast<py::list>();
    sys_path.insert(0, script_dir.string());

    py::dict globals;
    globals["__builtins__"] = py::module_::import("builtins");
    py::exec(code, globals);

    py::module_ pseudo =
        py::module_::import("types").attr("ModuleType")("_runner_script");
    for (auto item : globals)
        pseudo.attr(item.first.cast<std::string>().c_str()) = item.second;
    return pseudo;
}

/// Map a JSON type token to the corresponding ctypes type object.
/// Caller holds the GIL.
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
        // c_char * N — fixed-size byte string (accessible as bytes in Python)
        if (fd.length == 0)
            throw std::runtime_error(
                "Schema: string field '" + fd.name + "' needs 'length' > 0");
        return ct.attr("c_char").attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else if (fd.type_str == "bytes")
    {
        // c_uint8 * N — raw byte array
        if (fd.length == 0)
            throw std::runtime_error(
                "Schema: bytes field '" + fd.name + "' needs 'length' > 0");
        base = ct.attr("c_uint8");
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.length)));
    }
    else
    {
        throw std::runtime_error(
            "Schema: unknown type '" + fd.type_str +
            "' for field '" + fd.name + "'");
    }

    // Scalar or array
    if (fd.count > 1)
        return base.attr("__mul__")(py::int_(static_cast<int>(fd.count)));
    return base;
}

/**
 * @brief Build a ctypes.LittleEndianStructure class from a SchemaSpec.
 * @param spec  Must be Ctypes mode (has_schema == true).
 * @param name  Python class name ("SlotFrame" or "FlexFrame").
 * Caller holds the GIL.
 */
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

    py::object type_func = py::eval("type");
    return type_func(name,
                     py::make_tuple(ct.attr("LittleEndianStructure")),
                     kw);
}

/**
 * @brief Build a numpy dtype object from a SchemaSpec (numpy_array mode).
 * Caller holds the GIL.
 */
py::object build_numpy_dtype(const SchemaSpec &spec)
{
    py::module_ np = py::module_::import("numpy");
    return np.attr("dtype")(spec.numpy_dtype);
}

/**
 * @brief Return ctypes.sizeof(type_) bytes.
 * Caller holds the GIL.
 */
size_t ctypes_sizeof(const py::object &type_)
{
    py::module_ ct = py::module_::import("ctypes");
    return ct.attr("sizeof")(type_).cast<size_t>();
}

/**
 * @brief Print layout information for a ctypes struct to stdout.
 * Uses ctypes field descriptors to read offset and size.
 * Caller holds the GIL.
 */
void print_ctypes_layout(const py::object &type_, const char *label,
                          size_t total_size)
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

        // Padding before this field
        if (offset > prev_end)
            std::cout << "    [" << (offset - prev_end)
                      << " bytes padding]\n";

        std::cout << "    " << name
                  << "  offset=" << offset
                  << "  size=" << size << "\n";
        prev_end = offset + size;
    }
    // Trailing padding
    if (prev_end < total_size)
        std::cout << "    [" << (total_size - prev_end)
                  << " bytes trailing padding]\n";

    std::cout << "  Total: " << total_size
              << " bytes  (ctypes.sizeof = "
              << ctypes_sizeof(type_) << ")\n";
}

/// Print numpy array layout to stdout.
void print_numpy_layout(const py::object &dtype, const SchemaSpec &spec,
                         const char *label)
{
    py::module_ np      = py::module_::import("numpy");
    size_t      itemsize = dtype.attr("itemsize").cast<size_t>();

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

/// Interpret an on_write() return value: None or True → commit, False → discard.
bool parse_on_write_return(const py::object &ret)
{
    if (ret.is_none())
        return true;
    if (py::isinstance<py::bool_>(ret))
        return ret.cast<bool>();
    LOGGER_ERROR("[actor] on_write() must return bool or None, treating as discard");
    return false;
}

} // anonymous namespace

// ============================================================================
// ProducerActorHost — build_slot_types_ / print_layout_
// ============================================================================

bool ProducerActorHost::build_slot_types_()
{
    // Parse schemas from config JSON.
    try
    {
        if (!config_.slot_schema_json.is_null())
            slot_spec_ = parse_schema_json(config_.slot_schema_json);

        if (!config_.flexzone_schema_json.is_null())
            fz_spec_ = parse_schema_json(config_.flexzone_schema_json);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Schema parse error: {}", e.what());
        return false;
    }

    // Build Python types under GIL.
    py::gil_scoped_acquire gil;
    try
    {
        // ── Slot type ─────────────────────────────────────────────────────────
        if (slot_spec_.has_schema)
        {
            if (slot_spec_.exposure == SlotExposure::Ctypes)
            {
                slot_type_ = build_ctypes_struct(slot_spec_, "SlotFrame");
                schema_slot_size_ = ctypes_sizeof(slot_type_);
            }
            else
            {
                slot_type_        = build_numpy_dtype(slot_spec_);
                schema_slot_size_ = slot_type_.attr("itemsize").cast<size_t>();
                if (!slot_spec_.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : slot_spec_.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_slot_size_ = total * schema_slot_size_;
                }
            }
        }
        else if (config_.shm_slot_size > 0)
        {
            // Legacy raw bytearray mode.
            schema_slot_size_ = config_.shm_slot_size;
        }

        // ── Flexzone type ─────────────────────────────────────────────────────
        if (fz_spec_.has_schema)
        {
            has_fz_ = true;
            if (fz_spec_.exposure == SlotExposure::Ctypes)
            {
                fz_type_      = build_ctypes_struct(fz_spec_, "FlexFrame");
                schema_fz_size_ = ctypes_sizeof(fz_type_);
            }
            else
            {
                fz_type_      = build_numpy_dtype(fz_spec_);
                schema_fz_size_ = fz_type_.attr("itemsize").cast<size_t>();
                if (!fz_spec_.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : fz_spec_.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_fz_size_ = total * schema_fz_size_;
                }
            }
            // Round up flexzone size to 4096-byte page boundary.
            schema_fz_size_ = (schema_fz_size_ + 4095U) & ~size_t{4095U};
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Failed to build ctypes/numpy types: {}", e.what());
        return false;
    }

    return true;
}

void ProducerActorHost::print_layout_() const
{
    if (slot_spec_.has_schema)
    {
        if (slot_spec_.exposure == SlotExposure::Ctypes)
        {
            print_ctypes_layout(slot_type_, "Slot layout: SlotFrame", schema_slot_size_);
        }
        else
        {
            print_numpy_layout(slot_type_, slot_spec_, "Slot layout");
        }
    }
    if (fz_spec_.has_schema)
    {
        if (fz_spec_.exposure == SlotExposure::Ctypes)
        {
            print_ctypes_layout(fz_type_, "FlexZone layout: FlexFrame",
                                schema_fz_size_);
        }
        else
        {
            print_numpy_layout(fz_type_, fz_spec_, "FlexZone layout");
        }
    }
}

py::object ProducerActorHost::make_slot_view_(void *data, size_t size) const
{
    auto mv = py::memoryview::from_memory(data, static_cast<ssize_t>(size),
                                           /*readonly=*/false);
    if (!slot_spec_.has_schema)
    {
        // Legacy: return writable bytearray.
        return py::bytearray(reinterpret_cast<const char *>(data), size);
    }
    if (slot_spec_.exposure == SlotExposure::Ctypes)
    {
        return slot_type_.attr("from_buffer")(mv);
    }
    // numpy_array mode: numpy.ndarray(shape, dtype, buffer=mv)
    py::module_ np = py::module_::import("numpy");
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape)
            shape.append(d);
        return np.attr("ndarray")(shape, slot_type_, mv);
    }
    // Auto 1-D: compute count from size / itemsize.
    size_t itemsize = slot_type_.attr("itemsize").cast<size_t>();
    size_t count    = (itemsize > 0) ? (size / itemsize) : 0;
    return np.attr("ndarray")(py::make_tuple(static_cast<ssize_t>(count)),
                               slot_type_, mv);
}

// ============================================================================
// ProducerActorHost — lifecycle
// ============================================================================

ProducerActorHost::ProducerActorHost(const ActorConfig &config,
                                       hub::Messenger     &messenger)
    : config_(config)
    , messenger_(messenger)
{
}

ProducerActorHost::~ProducerActorHost()
{
    stop();
}

bool ProducerActorHost::load_script(bool verbose_validation)
{
    script_loaded_ = false;
    py::gil_scoped_acquire gil;

    try
    {
        py::module_ script = exec_script_file(config_.script_path);

        py_on_init_    = try_get_attr(script, "on_init");
        py_on_write_   = try_get_attr(script, "on_write");
        py_on_message_ = try_get_attr(script, "on_message");
        py_on_stop_    = try_get_attr(script, "on_stop");

        if (!is_callable(py_on_write_))
        {
            const std::string msg =
                "Runner: producer script '" + config_.script_path +
                "' must define 'def on_write(slot, flexzone, api):'";
            LOGGER_ERROR("{}", msg);
            if (verbose_validation)
                std::cerr << msg << "\n";
            return false;
        }

        if (verbose_validation)
        {
            std::cout << "Script loaded OK: " << config_.script_path << "\n";
            std::cout << "  on_init:    "
                      << (is_callable(py_on_init_) ? "found"
                                                   : "not defined (optional)") << "\n";
            std::cout << "  on_write:   found (required)\n";
            std::cout << "  on_message: "
                      << (is_callable(py_on_message_) ? "found"
                                                      : "not defined (optional)") << "\n";
            std::cout << "  on_stop:    "
                      << (is_callable(py_on_stop_) ? "found"
                                                   : "not defined (optional)") << "\n";

            // Build types and print layout for --validate output.
            if (build_slot_types_())
                print_layout_();
        }
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: script load error: {}", e.what());
        if (verbose_validation)
            std::cerr << "Script error: " << e.what() << "\n";
        return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("Runner: script load error: {}", e.what());
        if (verbose_validation)
            std::cerr << "Error: " << e.what() << "\n";
        return false;
    }

    script_loaded_ = true;
    return true;
}

bool ProducerActorHost::start()
{
    if (!script_loaded_ || running_.load())
        return false;

    // ── Build typed Python objects ────────────────────────────────────────────
    if (!build_slot_types_())
        return false;

    // ── Configure and create the Producer ────────────────────────────────────
    hub::ProducerOptions opts;
    opts.channel_name = config_.channel_name;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = config_.has_shm;

    if (config_.has_shm)
    {
        opts.shm_config.shared_secret        = config_.shm_secret;
        opts.shm_config.ring_buffer_capacity = config_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Latest_only;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = schema_fz_size_;

        // Choose the smallest page size that fits the slot schema.
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

    auto maybe_producer = hub::Producer::create(messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("Runner: failed to create producer for channel '{}'",
                     config_.channel_name);
        return false;
    }
    producer_ = std::move(maybe_producer);

    // ── Wire on_message callback ──────────────────────────────────────────────
    if (is_callable(py_on_message_))
    {
        producer_->on_consumer_message(
            [this](const std::string &identity, std::span<const std::byte> data)
            {
                py::gil_scoped_acquire g;
                try
                {
                    py_on_message_(
                        identity,
                        py::bytes(reinterpret_cast<const char *>(data.data()),
                                  data.size()),
                        api_obj_);
                }
                catch (py::error_already_set &e)
                {
                    LOGGER_ERROR("Runner: on_message error: {}", e.what());
                }
            });
    }

    if (!producer_->start())
    {
        LOGGER_ERROR("Runner: producer start() failed for '{}'",
                     config_.channel_name);
        return false;
    }

    // ── Build persistent flexzone view ────────────────────────────────────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_producer(&*producer_);

            if (has_fz_)
            {
                auto *shm = producer_->shm();
                if (shm)
                {
                    auto fz_span = shm->flexible_zone_span();
                    fz_mv_  = py::memoryview::from_memory(
                        fz_span.data(),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/false);

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape)
                                shape.append(d);
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
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("Runner: failed to build flexzone view: {}", e.what());
            return false;
        }
    }

    // ── Allocate ZMQ-only slot buffer ─────────────────────────────────────────
    if (!config_.has_shm && schema_slot_size_ > 0)
        zmq_slot_buf_.resize(schema_slot_size_, std::byte{0});

    LOGGER_INFO("Runner: producer started for channel '{}'", config_.channel_name);

    running_.store(true);
    call_on_init();

    if (config_.has_shm)
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_zmq(); });

    return true;
}

void ProducerActorHost::stop()
{
    if (!running_.load())
        return;
    running_.store(false);
    if (loop_thread_.joinable())
        loop_thread_.join();

    call_on_stop();

    if (producer_.has_value())
    {
        producer_->stop();
        producer_->close();
        producer_.reset();
    }

    // Release Python objects while interpreter is still alive.
    {
        py::gil_scoped_acquire g;
        api_.set_producer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("Runner: producer stopped for channel '{}'", config_.channel_name);
}

bool ProducerActorHost::is_running() const noexcept
{
    return running_.load();
}

// ============================================================================
// ProducerActorHost — lifecycle hooks
// ============================================================================

void ProducerActorHost::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: on_init error: {}", e.what());
    }

    // Update flexzone checksum after on_init writes.
    if (has_fz_)
    {
        if (auto *shm = producer_->shm())
            (void)shm->update_checksum_flexible_zone();
    }
}

void ProducerActorHost::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !producer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: on_stop error: {}", e.what());
    }
}

bool ProducerActorHost::call_on_write_(py::object &slot)
{
    // GIL must already be held by caller.
    py::object ret;
    try
    {
        ret = py_on_write_(slot, fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: on_write error: {}", e.what());
        if (config_.validation.on_python_error == ValidationPolicy::OnPyError::Stop)
            running_.store(false);
        return false;
    }
    return parse_on_write_return(ret);
}

// ============================================================================
// ProducerActorHost — SHM write loop
// ============================================================================

void ProducerActorHost::run_loop_shm()
{
    auto *shm = producer_->shm();
    if (!shm)
    {
        LOGGER_ERROR("Runner: SHM not available despite has_shm=true");
        running_.store(false);
        return;
    }

    const auto &val = config_.validation;

    while (running_.load())
    {
        auto slot_handle = shm->acquire_write_slot(100);
        if (!slot_handle)
            continue;
        if (!running_.load())
            break;

        auto   span        = slot_handle->buffer_span();
        size_t write_bytes = std::min(span.size_bytes(), schema_slot_size_);

        // Zero the slot region that the schema covers.
        std::memset(span.data(), 0, write_bytes);

        bool commit = false;
        {
            py::gil_scoped_acquire g;
            py::object slot = make_slot_view_(span.data(), write_bytes);
            commit = call_on_write_(slot);
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
    }
}

// ============================================================================
// ProducerActorHost — ZMQ-only write loop
// ============================================================================

void ProducerActorHost::run_loop_zmq()
{
    const auto interval = std::chrono::milliseconds(
        config_.write_interval_ms > 0 ? config_.write_interval_ms : 10U);

    while (running_.load())
    {
        bool commit = false;
        {
            py::gil_scoped_acquire g;
            std::fill(zmq_slot_buf_.begin(), zmq_slot_buf_.end(), std::byte{0});
            py::object slot = make_slot_view_(zmq_slot_buf_.data(),
                                               zmq_slot_buf_.size());
            commit = call_on_write_(slot);

            if (commit && !zmq_slot_buf_.empty())
                producer_->send(zmq_slot_buf_.data(), zmq_slot_buf_.size());
        }

        std::this_thread::sleep_for(interval);
    }
}

// ============================================================================
// ConsumerActorHost — build_slot_types_ / print_layout_
// ============================================================================

bool ConsumerActorHost::build_slot_types_()
{
    try
    {
        if (!config_.slot_schema_json.is_null())
            slot_spec_ = parse_schema_json(config_.slot_schema_json);

        if (!config_.flexzone_schema_json.is_null())
            fz_spec_ = parse_schema_json(config_.flexzone_schema_json);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Schema parse error: {}", e.what());
        return false;
    }

    py::gil_scoped_acquire g;
    try
    {
        if (slot_spec_.has_schema)
        {
            if (slot_spec_.exposure == SlotExposure::Ctypes)
            {
                slot_type_        = build_ctypes_struct(slot_spec_, "SlotFrame");
                schema_slot_size_ = ctypes_sizeof(slot_type_);
            }
            else
            {
                slot_type_        = build_numpy_dtype(slot_spec_);
                schema_slot_size_ = slot_type_.attr("itemsize").cast<size_t>();
                if (!slot_spec_.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : slot_spec_.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_slot_size_ = total * schema_slot_size_;
                }
            }
        }
        else if (config_.shm_slot_size > 0)
        {
            schema_slot_size_ = config_.shm_slot_size;
        }

        if (fz_spec_.has_schema)
        {
            has_fz_ = true;
            if (fz_spec_.exposure == SlotExposure::Ctypes)
            {
                fz_type_        = build_ctypes_struct(fz_spec_, "FlexFrame");
                schema_fz_size_ = ctypes_sizeof(fz_type_);
            }
            else
            {
                fz_type_        = build_numpy_dtype(fz_spec_);
                schema_fz_size_ = fz_type_.attr("itemsize").cast<size_t>();
                if (!fz_spec_.numpy_shape.empty())
                {
                    size_t total = 1;
                    for (auto d : fz_spec_.numpy_shape)
                        total *= static_cast<size_t>(d);
                    schema_fz_size_ = total * schema_fz_size_;
                }
            }
            schema_fz_size_ = (schema_fz_size_ + 4095U) & ~size_t{4095U};
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Failed to build ctypes/numpy types: {}", e.what());
        return false;
    }

    return true;
}

void ConsumerActorHost::print_layout_() const
{
    if (slot_spec_.has_schema)
    {
        if (slot_spec_.exposure == SlotExposure::Ctypes)
            print_ctypes_layout(slot_type_, "Slot layout: SlotFrame",
                                schema_slot_size_);
        else
            print_numpy_layout(slot_type_, slot_spec_, "Slot layout");
    }
    if (fz_spec_.has_schema)
    {
        if (fz_spec_.exposure == SlotExposure::Ctypes)
            print_ctypes_layout(fz_type_, "FlexZone layout: FlexFrame",
                                schema_fz_size_);
        else
            print_numpy_layout(fz_type_, fz_spec_, "FlexZone layout");
    }
}

py::object ConsumerActorHost::make_slot_view_readonly_(const void *data,
                                                        size_t      size) const
{
    // Consumer uses from_buffer_copy (readonly copy — user may store ref).
    auto mv = py::memoryview::from_memory(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<void *>(data), static_cast<ssize_t>(size), /*readonly=*/true);

    if (!slot_spec_.has_schema)
    {
        // Legacy: return bytes object.
        return py::bytes(reinterpret_cast<const char *>(data), size);
    }

    if (slot_spec_.exposure == SlotExposure::Ctypes)
    {
        // from_buffer_copy works with readonly buffers.
        return slot_type_.attr("from_buffer_copy")(mv);
    }

    // numpy_array mode: frombuffer (returns readonly array from const buffer).
    py::module_ np = py::module_::import("numpy");
    py::object  arr = np.attr("frombuffer")(mv, slot_type_);
    if (!slot_spec_.numpy_shape.empty())
    {
        py::list shape;
        for (auto d : slot_spec_.numpy_shape)
            shape.append(d);
        arr = arr.attr("reshape")(shape);
    }
    return arr;
}

// ============================================================================
// ConsumerActorHost — lifecycle
// ============================================================================

ConsumerActorHost::ConsumerActorHost(const ActorConfig &config,
                                       hub::Messenger     &messenger)
    : config_(config)
    , messenger_(messenger)
{
}

ConsumerActorHost::~ConsumerActorHost()
{
    stop();
}

bool ConsumerActorHost::load_script(bool verbose_validation)
{
    script_loaded_ = false;
    py::gil_scoped_acquire g;

    try
    {
        py::module_ script = exec_script_file(config_.script_path);

        py_on_init_ = try_get_attr(script, "on_init");
        py_on_data_ = try_get_attr(script, "on_data");
        py_on_read_ = try_get_attr(script, "on_read");
        py_on_stop_ = try_get_attr(script, "on_stop");

        const bool has_data = is_callable(py_on_data_);
        const bool has_read = is_callable(py_on_read_);

        if (!has_data && !has_read)
        {
            const std::string msg =
                "Runner: consumer script '" + config_.script_path +
                "' must define 'def on_data(data, api):' and/or "
                "'def on_read(slot, flexzone, api):'";
            LOGGER_ERROR("{}", msg);
            if (verbose_validation)
                std::cerr << msg << "\n";
            return false;
        }

        if (verbose_validation)
        {
            std::cout << "Script loaded OK: " << config_.script_path << "\n";
            std::cout << "  on_init: "
                      << (is_callable(py_on_init_) ? "found"
                                                   : "not defined (optional)") << "\n";
            std::cout << "  on_data: " << (has_data ? "found" : "not defined") << "\n";
            std::cout << "  on_read: " << (has_read ? "found" : "not defined") << "\n";
            std::cout << "  on_stop: "
                      << (is_callable(py_on_stop_) ? "found"
                                                   : "not defined (optional)") << "\n";

            if (build_slot_types_())
                print_layout_();
        }
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: script load error: {}", e.what());
        if (verbose_validation)
            std::cerr << "Script error: " << e.what() << "\n";
        return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("Runner: script load error: {}", e.what());
        if (verbose_validation)
            std::cerr << "Error: " << e.what() << "\n";
        return false;
    }

    script_loaded_ = true;
    return true;
}

bool ConsumerActorHost::start()
{
    if (!script_loaded_ || running_.load())
        return false;

    if (!build_slot_types_())
        return false;

    hub::ConsumerOptions opts;
    opts.channel_name      = config_.channel_name;
    opts.shm_shared_secret = config_.has_shm ? config_.shm_secret : 0U;

    auto maybe_consumer = hub::Consumer::connect(messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("Runner: failed to connect consumer to channel '{}'",
                     config_.channel_name);
        return false;
    }
    consumer_ = std::move(maybe_consumer);

    wire_zmq_callback();

    if (!consumer_->start())
    {
        LOGGER_ERROR("Runner: consumer start() failed for '{}'",
                     config_.channel_name);
        return false;
    }

    // ── Build API proxy and persistent flexzone view ──────────────────────────
    {
        py::gil_scoped_acquire g;
        try
        {
            api_obj_ = py::cast(&api_, py::return_value_policy::reference);
            api_.set_consumer(&*consumer_);

            if (has_fz_)
            {
                auto *shm = consumer_->shm();
                if (shm)
                {
                    const auto fz_span = shm->flexible_zone_span();

                    // Consumer flexzone: persistent COPY of SHM data (consumer
                    // cannot write to SHM flexzone — it is read-only mapped).
                    // The copy is stored in fz_buf_ (actor-owned memory) so
                    // the script can inspect and modify it locally.
                    fz_buf_.assign(fz_span.begin(), fz_span.end());
                    fz_mv_  = py::memoryview::from_memory(
                        fz_buf_.data(),
                        static_cast<ssize_t>(fz_buf_.size()),
                        /*readonly=*/false);

                    if (fz_spec_.exposure == SlotExposure::Ctypes)
                        fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
                    else
                    {
                        py::module_ np = py::module_::import("numpy");
                        if (!fz_spec_.numpy_shape.empty())
                        {
                            py::list shape;
                            for (auto d : fz_spec_.numpy_shape)
                                shape.append(d);
                            fz_inst_ = np.attr("ndarray")(shape, fz_type_, fz_mv_);
                        }
                        else
                        {
                            size_t items = fz_buf_.size() /
                                           fz_type_.attr("itemsize").cast<size_t>();
                            fz_inst_ = np.attr("ndarray")(
                                py::make_tuple(static_cast<ssize_t>(items)),
                                fz_type_, fz_mv_);
                        }
                    }

                    // Validate initial flexzone checksum.
                    if (config_.validation.flexzone_checksum ==
                        ValidationPolicy::Checksum::Enforce)
                    {
                        const bool fz_ok = shm->verify_checksum_flexible_zone();
                        if (!fz_ok)
                        {
                            LOGGER_WARN("[actor] Initial flexzone checksum failed "
                                        "for channel '{}'",
                                        config_.channel_name);
                        }
                    }
                }
            }

            if (fz_inst_.is_none())
                fz_inst_ = py::none();
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("Runner: failed to build consumer flexzone view: {}",
                         e.what());
            return false;
        }
    }

    LOGGER_INFO("Runner: consumer started for channel '{}'", config_.channel_name);

    running_.store(true);
    call_on_init();

    if (config_.has_shm && consumer_->has_shm() && is_callable(py_on_read_))
        loop_thread_ = std::thread([this] { run_loop_shm(); });

    return true;
}

void ConsumerActorHost::stop()
{
    if (!running_.load())
        return;
    running_.store(false);
    if (loop_thread_.joinable())
        loop_thread_.join();

    call_on_stop();

    if (consumer_.has_value())
    {
        consumer_->stop();
        consumer_->close();
        consumer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_consumer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("Runner: consumer stopped for channel '{}'", config_.channel_name);
}

bool ConsumerActorHost::is_running() const noexcept
{
    return running_.load();
}

// ============================================================================
// ConsumerActorHost — lifecycle hooks
// ============================================================================

void ConsumerActorHost::wire_zmq_callback()
{
    if (!is_callable(py_on_data_))
        return;
    consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            py::gil_scoped_acquire g;
            try
            {
                py_on_data_(
                    py::bytes(reinterpret_cast<const char *>(data.data()),
                              data.size()),
                    api_obj_);
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("Runner: on_data error: {}", e.what());
                if (config_.validation.on_python_error ==
                    ValidationPolicy::OnPyError::Stop)
                    running_.store(false);
            }
        });
}

void ConsumerActorHost::call_on_init()
{
    if (!is_callable(py_on_init_))
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_init_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: on_init error: {}", e.what());
    }
}

void ConsumerActorHost::call_on_stop()
{
    if (!is_callable(py_on_stop_) || !consumer_.has_value())
        return;
    py::gil_scoped_acquire g;
    try
    {
        py_on_stop_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("Runner: on_stop error: {}", e.what());
    }
}

// ============================================================================
// ConsumerActorHost — SHM read loop
// ============================================================================

void ConsumerActorHost::run_loop_shm()
{
    auto *shm = consumer_->shm();
    if (!shm)
    {
        LOGGER_WARN("Runner: SHM not available despite has_shm=true; "
                    "on_read will not be called");
        return;
    }

    const auto &val = config_.validation;

    while (running_.load())
    {
        auto slot_handle = shm->acquire_consume_slot(100);
        if (!slot_handle)
            continue;
        if (!running_.load())
            break;

        const auto span      = slot_handle->buffer_span();
        const size_t read_sz = std::min(span.size_bytes(), schema_slot_size_);

        // ── Slot checksum enforcement ─────────────────────────────────────────
        bool slot_ok = true;
        if (val.slot_checksum == ValidationPolicy::Checksum::Enforce)
        {
            slot_ok = slot_handle->verify_checksum_slot();
            if (!slot_ok)
            {
                LOGGER_WARN("[actor] Slot checksum failed (channel='{}' slot={})",
                            config_.channel_name, slot_handle->slot_id());
            }
        }

        // ── FlexZone checksum enforcement ────────────────────────────────────
        bool fz_ok = true;
        if (has_fz_ && val.flexzone_checksum == ValidationPolicy::Checksum::Enforce)
        {
            // If the script has accepted the current flexzone state, trust it.
            const auto fz_span = slot_handle->flexible_zone_span();
            if (api_.is_fz_accepted(fz_span))
            {
                fz_ok = true;
            }
            else
            {
                fz_ok = slot_handle->verify_checksum_flexible_zone();
                if (!fz_ok)
                {
                    LOGGER_WARN("[actor] FlexZone checksum failed (channel='{}')",
                                config_.channel_name);
                    // Refresh actor-owned copy from SHM so the script sees latest data.
                    if (!fz_span.empty() && fz_buf_.size() == fz_span.size())
                        std::memcpy(fz_buf_.data(), fz_span.data(), fz_span.size());
                }
            }
        }

        // ── Decide whether to call on_read ────────────────────────────────────
        const bool overall_valid = slot_ok && fz_ok;
        bool       call_read     = overall_valid;

        if (!overall_valid && val.on_checksum_fail == ValidationPolicy::OnFail::Pass)
        {
            call_read = true;  // call the script, let it decide
        }

        api_.set_slot_valid(overall_valid);

        if (call_read)
        {
            py::gil_scoped_acquire g;
            try
            {
                py_on_read_(make_slot_view_readonly_(span.data(), read_sz),
                             fz_inst_, api_obj_);
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("Runner: on_read error: {}", e.what());
                if (val.on_python_error == ValidationPolicy::OnPyError::Stop)
                    running_.store(false);
            }
        }

        (void)shm->release_consume_slot(*slot_handle);
    }
}

} // namespace pylabhub::actor
