/**
 * @file actor_host.cpp
 * @brief ProducerRoleWorker, ConsumerRoleWorker, and ActorHost implementations.
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
 *           Do NOT store beyond on_write().
 * Consumer: from_buffer on read-only memoryview — zero-copy, field writes raise
 *           TypeError. Do NOT store beyond on_read().
 *
 * ## Consumer flexzone
 *
 * Consumer accesses the SHM flexzone directly (zero-copy). A read-only
 * memoryview wraps the SHM pointer — Python cannot write flexzone fields.
 * C++ can still read the pointer (used by is_fz_accepted()).
 *
 * ## Checksum flow (producer per-write)
 *   1. Zero slot buffer.
 *   2. Call on_write(slot, fz, api) under GIL.
 *   3. If commit: slot_handle->commit(schema_slot_size_).
 *   4. If Update|Enforce: update_checksum_slot().
 *   5. If Update|Enforce and has_fz_: update_checksum_flexible_zone().
 *   6. release_write_slot().
 *
 * ## Checksum flow (consumer per-read)
 *   1. acquire_consume_slot().
 *   2. If Enforce: verify slot checksum.
 *   3. If Enforce and has_fz_: check is_fz_accepted OR verify fz checksum.
 *   4. Set api_.set_slot_valid().
 *   5. Call on_read(slot, fz, api) or on_read(None, fz, api, timed_out=True).
 *   6. release_consume_slot().
 */
#include "actor_host.hpp"
#include "actor_dispatch_table.hpp"

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
// Anonymous-namespace helpers (shared by both worker types)
// ============================================================================

namespace
{

py::object try_get_attr(py::module_ &mod, const char *name)
{
    if (py::hasattr(mod, name))
        return mod.attr(name);
    return py::none();
}

bool is_callable(const py::object &obj)
{
    return !obj.is_none() && py::bool_(py::isinstance<py::function>(obj));
}

py::module_ exec_script_file(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Actor: cannot open script: " + path);

    std::string code((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    const fs::path script_dir = fs::path(path).parent_path();
    py::module_    sys        = py::module_::import("sys");
    py::list       sys_path   = sys.attr("path").cast<py::list>();
    sys_path.insert(0, script_dir.string());

    py::dict globals;
    globals["__builtins__"] = py::module_::import("builtins");
    py::exec(code, globals);

    py::module_ pseudo =
        py::module_::import("types").attr("ModuleType")("_actor_script");
    for (auto item : globals)
        pseudo.attr(item.first.cast<std::string>().c_str()) = item.second;
    return pseudo;
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

bool parse_on_write_return(const py::object &ret)
{
    if (ret.is_none())
        return true;
    if (py::isinstance<py::bool_>(ret))
        return ret.cast<bool>();
    LOGGER_ERROR("[actor] on_write() must return bool or None — treating as discard");
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

ProducerRoleWorker::ProducerRoleWorker(const std::string  &role_name,
                                        const RoleConfig   &role_cfg,
                                        const std::string  &actor_uid,
                                        hub::Messenger     &messenger,
                                        std::atomic<bool>  &shutdown,
                                        const py::object   &on_init_fn,
                                        const py::object   &on_write_fn,
                                        const py::object   &on_message_fn,
                                        const py::object   &on_stop_fn)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , messenger_(messenger)
    , shutdown_(shutdown)
    , py_on_init_(on_init_fn)
    , py_on_write_(on_write_fn)
    , py_on_message_(on_message_fn)
    , py_on_stop_(on_stop_fn)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
    api_.set_trigger_fn([this]() { notify_trigger(); });
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

bool ProducerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!build_slot_types_())
        return false;

    // ── Create Producer ───────────────────────────────────────────────────────
    hub::ProducerOptions opts;
    opts.channel_name = role_cfg_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = role_cfg_.has_shm;

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

    auto maybe_producer = hub::Producer::create(messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to create producer for channel '{}'",
                     role_name_, role_cfg_.channel);
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
                    LOGGER_ERROR("[actor/{}] on_message error: {}", role_name_, e.what());
                }
            });
    }

    if (!producer_->start())
    {
        LOGGER_ERROR("[actor/{}] producer->start() failed", role_name_);
        return false;
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
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor/{}] Failed to build flexzone view: {}", role_name_, e.what());
            return false;
        }
    }

    // ── ZMQ-only slot buffer ──────────────────────────────────────────────────
    if (!role_cfg_.has_shm && schema_slot_size_ > 0)
        zmq_slot_buf_.resize(schema_slot_size_, std::byte{0});

    LOGGER_INFO("[actor/{}] producer started on channel '{}'",
                role_name_, role_cfg_.channel);

    running_.store(true);
    call_on_init();

    if (role_cfg_.has_shm)
        loop_thread_ = std::thread([this] { run_loop_shm(); });
    else
        loop_thread_ = std::thread([this] { run_loop_zmq(); });

    return true;
}

void ProducerRoleWorker::stop()
{
    if (!running_.load())
        return;
    running_.store(false);

    // Wake trigger waiters (interval_ms == -1 case).
    notify_trigger();

    if (loop_thread_.joinable())
        loop_thread_.join();

    call_on_stop();

    if (producer_.has_value())
    {
        producer_->stop();
        producer_->close();
        producer_.reset();
    }

    {
        py::gil_scoped_acquire g;
        api_.set_producer(nullptr);
        fz_inst_ = py::none();
        fz_mv_   = py::none();
        api_obj_ = py::none();
    }

    LOGGER_INFO("[actor/{}] producer stopped", role_name_);
}

void ProducerRoleWorker::notify_trigger()
{
    {
        std::unique_lock<std::mutex> lock(trigger_mu_);
        trigger_pending_ = true;
    }
    trigger_cv_.notify_one();
}

void ProducerRoleWorker::call_on_init()
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
        py_on_stop_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

bool ProducerRoleWorker::call_on_write_(py::object &slot)
{
    py::object ret;
    try
    {
        ret = py_on_write_(slot, fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor/{}] on_write error: {}", role_name_, e.what());
        if (role_cfg_.validation.on_python_error == ValidationPolicy::OnPyError::Stop)
            running_.store(false);
        return false;
    }
    return parse_on_write_return(ret);
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

    while (running_.load() && !shutdown_.load())
    {
        // ── interval_ms timing ────────────────────────────────────────────────
        if (role_cfg_.interval_ms == -1)
        {
            // Event-driven: wait for trigger_write() or stop signal.
            std::unique_lock<std::mutex> lock(trigger_mu_);
            trigger_cv_.wait(lock, [this]
            {
                return trigger_pending_ || !running_.load() || shutdown_.load();
            });
            trigger_pending_ = false;
            if (!running_.load() || shutdown_.load())
                break;
        }
        else if (role_cfg_.interval_ms > 0)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(role_cfg_.interval_ms));
            if (!running_.load() || shutdown_.load())
                break;
        }

        // ── Acquire SHM write slot ────────────────────────────────────────────
        auto slot_handle = shm->acquire_write_slot(100);
        if (!slot_handle)
            continue;
        if (!running_.load() || shutdown_.load())
            break;

        auto   span        = slot_handle->buffer_span();
        size_t write_bytes = std::min(span.size_bytes(), schema_slot_size_);
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

void ProducerRoleWorker::run_loop_zmq()
{
    const auto interval = std::chrono::milliseconds(
        role_cfg_.interval_ms > 0 ? role_cfg_.interval_ms : 10);

    while (running_.load() && !shutdown_.load())
    {
        if (role_cfg_.interval_ms == -1)
        {
            std::unique_lock<std::mutex> lock(trigger_mu_);
            trigger_cv_.wait(lock, [this]
            {
                return trigger_pending_ || !running_.load() || shutdown_.load();
            });
            trigger_pending_ = false;
            if (!running_.load() || shutdown_.load())
                break;
        }

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

        if (role_cfg_.interval_ms >= 0)
            std::this_thread::sleep_for(interval);
    }
}

// ============================================================================
// ConsumerRoleWorker
// ============================================================================

ConsumerRoleWorker::ConsumerRoleWorker(const std::string  &role_name,
                                        const RoleConfig   &role_cfg,
                                        const std::string  &actor_uid,
                                        hub::Messenger     &messenger,
                                        std::atomic<bool>  &shutdown,
                                        const py::object   &on_init_fn,
                                        const py::object   &on_read_fn,
                                        const py::object   &on_data_fn,
                                        const py::object   &on_stop_fn)
    : role_name_(role_name)
    , role_cfg_(role_cfg)
    , messenger_(messenger)
    , shutdown_(shutdown)
    , py_on_init_(on_init_fn)
    , py_on_read_(on_read_fn)
    , py_on_data_(on_data_fn)
    , py_on_stop_(on_stop_fn)
{
    api_.set_role_name(role_name);
    api_.set_actor_uid(actor_uid);
    api_.set_shutdown_flag(&shutdown_);
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
    // Zero-copy read-only view. Identical to producer except readonly=true.
    // from_buffer on a read-only memoryview: any field write raises TypeError.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto mv = py::memoryview::from_memory(const_cast<void *>(data),
                                           static_cast<ssize_t>(size),
                                           /*readonly=*/true);
    if (!slot_spec_.has_schema)
        return py::bytes(reinterpret_cast<const char *>(data), size); // Legacy

    if (slot_spec_.exposure == SlotExposure::Ctypes)
        return slot_type_.attr("from_buffer")(mv);  // zero-copy, readonly

    // numpy_array mode: frombuffer returns a read-only array.
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

bool ConsumerRoleWorker::start()
{
    if (running_.load())
        return false;

    if (!build_slot_types_())
        return false;

    hub::ConsumerOptions opts;
    opts.channel_name      = role_cfg_.channel;
    opts.shm_shared_secret = role_cfg_.has_shm ? role_cfg_.shm_secret : 0U;

    auto maybe_consumer = hub::Consumer::connect(messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[actor/{}] Failed to connect consumer to channel '{}'",
                     role_name_, role_cfg_.channel);
        return false;
    }
    consumer_ = std::move(maybe_consumer);

    wire_zmq_callback();

    if (!consumer_->start())
    {
        LOGGER_ERROR("[actor/{}] consumer->start() failed", role_name_);
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
                    // Consumer flexzone: zero-copy read-only view into SHM.
                    // The SHM pointer is valid for the consumer's lifetime.
                    // Python cannot write flexzone fields (TypeError).
                    const auto fz_span = shm->flexible_zone_span();

                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    fz_mv_ = py::memoryview::from_memory(
                        const_cast<std::byte *>(fz_span.data()),
                        static_cast<ssize_t>(fz_span.size_bytes()),
                        /*readonly=*/true);  // Consumer views producer's flexzone

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

                    // Validate initial flexzone checksum.
                    if (role_cfg_.validation.flexzone_checksum ==
                        ValidationPolicy::Checksum::Enforce)
                    {
                        const bool fz_ok = shm->verify_checksum_flexible_zone();
                        if (!fz_ok)
                        {
                            LOGGER_WARN("[actor/{}] Initial flexzone checksum failed",
                                        role_name_);
                        }
                    }
                }
            }
            if (fz_inst_.is_none())
                fz_inst_ = py::none();
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

    running_.store(true);
    call_on_init();

    if (role_cfg_.has_shm && consumer_->has_shm() && is_callable(py_on_read_))
        loop_thread_ = std::thread([this] { run_loop_shm(); });

    return true;
}

void ConsumerRoleWorker::stop()
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

    LOGGER_INFO("[actor/{}] consumer stopped", role_name_);
}

void ConsumerRoleWorker::wire_zmq_callback()
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
                LOGGER_ERROR("[actor/{}] on_data error: {}", role_name_, e.what());
                if (role_cfg_.validation.on_python_error ==
                    ValidationPolicy::OnPyError::Stop)
                    running_.store(false);
            }
        });
}

void ConsumerRoleWorker::call_on_init()
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
        py_on_stop_(fz_inst_, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor/{}] on_stop error: {}", role_name_, e.what());
    }
}

void ConsumerRoleWorker::call_on_read_timeout_()
{
    // on_read(None, flexzone, api, timed_out=True)
    py::gil_scoped_acquire g;
    try
    {
        api_.set_slot_valid(true);  // timeout is not a validity failure
        py_on_read_(py::none(), fz_inst_, api_obj_, py::arg("timed_out") = true);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor/{}] on_read (timeout) error: {}", role_name_, e.what());
        if (role_cfg_.validation.on_python_error == ValidationPolicy::OnPyError::Stop)
            running_.store(false);
    }
}

void ConsumerRoleWorker::run_loop_shm()
{
    auto *shm = consumer_->shm();
    if (shm == nullptr)
    {
        LOGGER_WARN("[actor/{}] SHM unavailable despite has_shm=true; "
                    "on_read will not be called", role_name_);
        return;
    }

    const auto &val = role_cfg_.validation;
    auto last_slot_time = std::chrono::steady_clock::now();

    while (running_.load() && !shutdown_.load())
    {
        auto slot_handle = shm->acquire_consume_slot(100);

        if (!slot_handle)
        {
            // No slot within the 100 ms poll window.
            if (role_cfg_.timeout_ms > 0 && is_callable(py_on_read_))
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_slot_time).count();
                if (elapsed >= static_cast<long long>(role_cfg_.timeout_ms))
                {
                    call_on_read_timeout_();
                    last_slot_time = std::chrono::steady_clock::now();
                }
            }
            continue;
        }
        if (!running_.load() || shutdown_.load())
            break;

        last_slot_time = std::chrono::steady_clock::now();

        const auto   span     = slot_handle->buffer_span();
        const size_t read_sz  = std::min(span.size_bytes(), schema_slot_size_);

        // ── Slot checksum enforcement ─────────────────────────────────────────
        bool slot_ok = true;
        if (val.slot_checksum == ValidationPolicy::Checksum::Enforce)
        {
            slot_ok = slot_handle->verify_checksum_slot();
            if (!slot_ok)
            {
                LOGGER_WARN("[actor/{}] Slot checksum failed (slot={})",
                            role_name_, slot_handle->slot_id());
            }
        }

        // ── FlexZone checksum enforcement ─────────────────────────────────────
        bool fz_ok = true;
        if (has_fz_ && val.flexzone_checksum == ValidationPolicy::Checksum::Enforce)
        {
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
                    LOGGER_WARN("[actor/{}] FlexZone checksum failed", role_name_);
                }
            }
        }

        // ── Decide whether to call on_read ────────────────────────────────────
        const bool overall_valid = slot_ok && fz_ok;
        bool       call_read     = overall_valid;

        if (!overall_valid &&
            val.on_checksum_fail == ValidationPolicy::OnFail::Pass)
            call_read = true;

        api_.set_slot_valid(overall_valid);

        if (call_read && is_callable(py_on_read_))
        {
            py::gil_scoped_acquire g;
            try
            {
                py_on_read_(make_slot_view_readonly_(span.data(), read_sz),
                             fz_inst_, api_obj_);
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("[actor/{}] on_read error: {}", role_name_, e.what());
                if (val.on_python_error == ValidationPolicy::OnPyError::Stop)
                    running_.store(false);
            }
        }

        (void)shm->release_consume_slot(*slot_handle);
    }
}

// ============================================================================
// ActorHost
// ============================================================================

ActorHost::ActorHost(const ActorConfig &config, hub::Messenger &messenger)
    : config_(config)
    , messenger_(messenger)
{
}

ActorHost::~ActorHost()
{
    stop();
}

bool ActorHost::load_script(bool verbose)
{
    script_loaded_ = false;

    py::gil_scoped_acquire g;

    // Clear any stale dispatch table from a previous import.
    try
    {
        py::module_::import("pylabhub_actor").attr("_clear_dispatch_table")();
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor] Failed to clear dispatch table: {}", e.what());
        return false;
    }

    // Import the script — decorators fire, populating the dispatch table.
    try
    {
        (void)exec_script_file(config_.script_path);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[actor] Script load error: {}", e.what());
        if (verbose) std::cerr << "Script error: " << e.what() << "\n";
        return false;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[actor] Script load error: {}", e.what());
        if (verbose) std::cerr << "Error: " << e.what() << "\n";
        return false;
    }

    // Read dispatch table — find which roles have registered handlers.
    const ActorDispatchTable &tbl = get_dispatch_table();

    // Collect all role names mentioned in any event map.
    std::unordered_map<std::string, bool> activated; // role_name -> has_write (producer)
    for (const auto &[name, _] : tbl.on_write)   activated[name] = true;
    for (const auto &[name, _] : tbl.on_message) activated.emplace(name, false);
    for (const auto &[name, _] : tbl.on_read)    activated[name] = false;
    for (const auto &[name, _] : tbl.on_data)    activated.emplace(name, false);
    for (const auto &[name, _] : tbl.on_init)    activated.emplace(name, false);
    for (const auto &[name, _] : tbl.on_stop_p)  activated.emplace(name, false);
    for (const auto &[name, _] : tbl.on_stop_c)  activated.emplace(name, false);

    if (activated.empty())
    {
        LOGGER_WARN("[actor] Script '{}' registered no role handlers — "
                    "nothing to activate", config_.script_path);
    }

    // Warn about registered roles not present in the config.
    for (const auto &[name, _] : activated)
    {
        if (config_.roles.find(name) == config_.roles.end())
        {
            LOGGER_WARN("[actor] Script registered handler for role '{}' but "
                        "that role is not defined in the config — ignoring", name);
        }
    }

    if (verbose)
    {
        std::cout << "\nScript: " << config_.script_path << "\n";
        std::cout << "Actor uid: "
                  << (config_.actor_uid.empty() ? "(auto)" : config_.actor_uid)
                  << "\n";
        print_role_summary();
    }

    script_loaded_ = true;
    return true;
}

bool ActorHost::start()
{
    if (!script_loaded_)
        return false;

    const ActorDispatchTable &tbl = get_dispatch_table();

    auto get_fn = [](const std::unordered_map<std::string, py::object> &map,
                      const std::string &key) -> const py::object &
    {
        static const py::object none_ = py::none();
        auto it = map.find(key);
        return (it != map.end()) ? it->second : none_;
    };

    bool any_started = false;

    // ── Start producer roles ──────────────────────────────────────────────────
    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (role_cfg.kind != RoleConfig::Kind::Producer)
            continue;

        // Must have at least on_write registered to be meaningful.
        if (tbl.on_write.find(role_name) == tbl.on_write.end())
        {
            LOGGER_WARN("[actor] Producer role '{}' has no on_write handler — "
                        "skipping", role_name);
            continue;
        }

        auto worker = std::make_unique<ProducerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, messenger_, shutdown_,
            get_fn(tbl.on_init,    role_name),
            get_fn(tbl.on_write,   role_name),
            get_fn(tbl.on_message, role_name),
            get_fn(tbl.on_stop_p,  role_name));

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

        const bool has_read = tbl.on_read.count(role_name) > 0;
        const bool has_data = tbl.on_data.count(role_name) > 0;

        if (!has_read && !has_data)
        {
            LOGGER_WARN("[actor] Consumer role '{}' has neither on_read nor on_data "
                        "handler — skipping", role_name);
            continue;
        }

        auto worker = std::make_unique<ConsumerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, messenger_, shutdown_,
            get_fn(tbl.on_init,   role_name),
            get_fn(tbl.on_read,   role_name),
            get_fn(tbl.on_data,   role_name),
            get_fn(tbl.on_stop_c, role_name));

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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void ActorHost::print_role_summary() const
{
    const ActorDispatchTable &tbl = get_dispatch_table();

    std::cout << "\nConfigured roles:\n";
    for (const auto &[name, cfg] : config_.roles)
    {
        const char *kind_str = (cfg.kind == RoleConfig::Kind::Producer)
            ? "producer" : "consumer";
        const bool activated = (cfg.kind == RoleConfig::Kind::Producer)
            ? (tbl.on_write.count(name) > 0)
            : (tbl.on_read.count(name) > 0 || tbl.on_data.count(name) > 0);
        std::cout << "  " << name << "  [" << kind_str << "]"
                  << "  channel=" << cfg.channel
                  << "  " << (activated ? "ACTIVATED" : "not activated") << "\n";
    }
}

} // namespace pylabhub::actor
