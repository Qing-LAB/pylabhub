/**
 * @file role_api_base.cpp
 * @brief RoleAPIBase implementation — unified role API (pure C++).
 */
#include "utils/role_api_base.hpp"

#include "utils/config/checksum_config.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_producer.hpp"
#include "utils/logger.hpp"
#include "utils/messenger.hpp"
#include "utils/metrics_json.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"
#include "utils/shared_memory_spinlock.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace pylabhub::scripting
{

// ============================================================================
// Impl
// ============================================================================

struct RoleAPIBase::Impl
{
    explicit Impl(RoleHostCore *c) : core(c) {}

    RoleHostCore    *core;
    hub::Producer   *producer{nullptr};
    hub::Consumer   *consumer{nullptr};
    hub::Messenger  *messenger{nullptr};
    hub::InboxQueue *inbox_queue{nullptr};

    std::string role_tag;   // "prod", "cons", "proc"
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::Enforced};
    bool stop_on_script_error{false};
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string script_dir;
    std::string role_dir;

    // Inbox client cache (keyed by target_uid).
    // Uses RoleHostCore::open_inbox() for atomic check-and-create.

    // Consumer sequence tracking.
    std::atomic<uint64_t> last_seq{0};
};

// ============================================================================
// Lifecycle
// ============================================================================

RoleAPIBase::RoleAPIBase(RoleHostCore &core)
    : pImpl(std::make_unique<Impl>(&core))
{}

RoleAPIBase::~RoleAPIBase() = default;
RoleAPIBase::RoleAPIBase(RoleAPIBase &&) noexcept = default;
RoleAPIBase &RoleAPIBase::operator=(RoleAPIBase &&) noexcept = default;

// ============================================================================
// Host wiring
// ============================================================================

void RoleAPIBase::set_role_tag(std::string tag)       { pImpl->role_tag = std::move(tag); }
void RoleAPIBase::set_producer(hub::Producer *p)      { pImpl->producer = p; }
void RoleAPIBase::set_consumer(hub::Consumer *c)      { pImpl->consumer = c; }
void RoleAPIBase::set_messenger(hub::Messenger *m)    { pImpl->messenger = m; }
void RoleAPIBase::set_inbox_queue(hub::InboxQueue *q) { pImpl->inbox_queue = q; }
void RoleAPIBase::set_uid(std::string uid)            { pImpl->uid = std::move(uid); }
void RoleAPIBase::set_name(std::string name)          { pImpl->name = std::move(name); }
void RoleAPIBase::set_channel(std::string c)          { pImpl->channel = std::move(c); }
void RoleAPIBase::set_out_channel(std::string c)      { pImpl->out_channel = std::move(c); }
void RoleAPIBase::set_log_level(std::string l)        { pImpl->log_level = std::move(l); }
void RoleAPIBase::set_script_dir(std::string d)       { pImpl->script_dir = std::move(d); }
void RoleAPIBase::set_role_dir(std::string d)         { pImpl->role_dir = std::move(d); }
void RoleAPIBase::set_checksum_policy(hub::ChecksumPolicy p) { pImpl->checksum_policy = p; }
void RoleAPIBase::set_stop_on_script_error(bool v)    { pImpl->stop_on_script_error = v; }

// ============================================================================
// Identity
// ============================================================================

const std::string &RoleAPIBase::role_tag() const   { return pImpl->role_tag; }
const std::string &RoleAPIBase::uid() const        { return pImpl->uid; }
const std::string &RoleAPIBase::name() const       { return pImpl->name; }
const std::string &RoleAPIBase::channel() const    { return pImpl->channel; }
const std::string &RoleAPIBase::out_channel() const { return pImpl->out_channel; }
const std::string &RoleAPIBase::log_level() const  { return pImpl->log_level; }
const std::string &RoleAPIBase::script_dir() const { return pImpl->script_dir; }
const std::string &RoleAPIBase::role_dir() const   { return pImpl->role_dir; }

std::string RoleAPIBase::logs_dir() const
{
    return pImpl->role_dir.empty() ? std::string{} : pImpl->role_dir + "/logs";
}

std::string RoleAPIBase::run_dir() const
{
    return pImpl->role_dir.empty() ? std::string{} : pImpl->role_dir + "/run";
}

hub::ChecksumPolicy RoleAPIBase::checksum_policy() const { return pImpl->checksum_policy; }
bool RoleAPIBase::stop_on_script_error() const { return pImpl->stop_on_script_error; }

// ============================================================================
// Control
// ============================================================================

void RoleAPIBase::log(const std::string &level, const std::string &msg)
{
    if (level == "debug" || level == "Debug")
        LOGGER_DEBUG("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else
        LOGGER_INFO("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
}

void RoleAPIBase::stop()                        { pImpl->core->request_stop(); }
void RoleAPIBase::set_critical_error()          { pImpl->core->set_critical_error(); }
bool RoleAPIBase::critical_error() const        { return pImpl->core->is_critical_error(); }
std::string RoleAPIBase::stop_reason() const    { return pImpl->core->stop_reason_string(); }

// ============================================================================
// Broker queries
// ============================================================================

void RoleAPIBase::notify_channel(const std::string &target, const std::string &event,
                                 const std::string &data)
{
    if (pImpl->messenger)
        pImpl->messenger->enqueue_channel_notify(target, pImpl->uid, event, data);
}

void RoleAPIBase::broadcast_channel(const std::string &target, const std::string &msg,
                                    const std::string &data)
{
    if (pImpl->messenger)
        pImpl->messenger->enqueue_channel_broadcast(target, pImpl->uid, msg, data);
}

std::vector<nlohmann::json> RoleAPIBase::list_channels()
{
    if (!pImpl->messenger)
        return {};
    return pImpl->messenger->list_channels();
}

std::string RoleAPIBase::request_shm_info(const std::string &channel)
{
    if (!pImpl->messenger)
        return {};
    return pImpl->messenger->request_shm_info(channel);
}

// ============================================================================
// Messaging
// ============================================================================

bool RoleAPIBase::broadcast(const void *data, size_t size)
{
    if (!pImpl->producer)
        return false;
    pImpl->producer->send(data, size);
    return true;
}

bool RoleAPIBase::send(const std::string &identity_hex, const void *data, size_t size)
{
    if (!pImpl->producer)
        return false;
    const auto raw = format_tools::bytes_from_hex(identity_hex);
    pImpl->producer->send_to(raw, data, size);
    return true;
}

std::vector<std::string> RoleAPIBase::connected_consumers()
{
    if (!pImpl->producer)
        return {};
    std::vector<std::string> result;
    for (const auto &id : pImpl->producer->connected_consumers())
        result.push_back(format_tools::bytes_to_hex(id));
    return result;
}

// ============================================================================
// Inbox client management
// ============================================================================

std::optional<RoleAPIBase::InboxOpenResult>
RoleAPIBase::open_inbox_client(const std::string &target_uid)
{
    if (!pImpl->core || !pImpl->messenger)
        return std::nullopt;

    hub::SchemaSpec result_spec;
    std::string result_packing;

    auto entry = pImpl->core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            auto info = pImpl->messenger->query_role_info(target_uid, 1000);
            if (!info.has_value())
                return std::nullopt;

            if (!info->inbox_schema.is_object() ||
                !info->inbox_schema.contains("fields"))
                return std::nullopt;

            hub::SchemaSpec spec;
            try
            {
                spec = hub::parse_schema_json(info->inbox_schema);
            }
            catch (const std::exception &e)
            {
                LOGGER_WARN("[api] open_inbox('{}'): schema parse error: {}",
                            target_uid, e.what());
                return std::nullopt;
            }

            size_t item_size = hub::compute_schema_size(spec, info->inbox_packing);

            auto zmq_fields = hub::schema_spec_to_zmq_fields(spec);

            auto client_ptr = hub::InboxClient::connect_to(
                info->inbox_endpoint, pImpl->uid,
                std::move(zmq_fields), info->inbox_packing);
            if (!client_ptr)
            {
                LOGGER_WARN("[api] open_inbox('{}'): connect failed", target_uid);
                return std::nullopt;
            }
            if (!client_ptr->start())
            {
                LOGGER_WARN("[api] open_inbox('{}'): start failed", target_uid);
                return std::nullopt;
            }
            client_ptr->set_checksum_policy(
                config::string_to_checksum_policy(info->inbox_checksum));

            result_spec = std::move(spec);
            result_packing = info->inbox_packing;

            return RoleHostCore::InboxCacheEntry{
                std::shared_ptr<hub::InboxClient>(std::move(client_ptr)),
                "InboxSlot", item_size};
        });

    if (!entry)
        return std::nullopt;

    return InboxOpenResult{
        entry->client, std::move(result_spec),
        std::move(result_packing), entry->item_size};
}

bool RoleAPIBase::wait_for_role(const std::string &uid, int timeout_ms)
{
    if (!pImpl->messenger)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pImpl->messenger->query_role_presence(uid, kPollMs))
            return true;
    }
    return false;
}

void RoleAPIBase::close_all_inbox_clients()
{
    pImpl->core->clear_inbox_cache();
}

// ============================================================================
// Output side
// ============================================================================

void *RoleAPIBase::write_flexzone()
{
    return pImpl->producer ? pImpl->producer->write_flexzone() : nullptr;
}

const void *RoleAPIBase::read_flexzone() const
{
    return pImpl->producer ? pImpl->producer->read_flexzone() : nullptr;
}

size_t RoleAPIBase::flexzone_size() const
{
    return pImpl->producer ? pImpl->producer->flexzone_size() : 0;
}

bool RoleAPIBase::update_flexzone_checksum()
{
    if (!pImpl->producer)
        return false;
    pImpl->producer->sync_flexzone_checksum();
    return true;
}

uint64_t RoleAPIBase::out_slots_written() const { return pImpl->core->out_written(); }
uint64_t RoleAPIBase::out_drop_count() const    { return pImpl->core->drops(); }

size_t RoleAPIBase::out_capacity() const
{
    return pImpl->producer ? pImpl->producer->queue_capacity() : 0;
}

std::string RoleAPIBase::out_policy() const
{
    return pImpl->producer ? pImpl->producer->queue_policy_info() : std::string{};
}

// ============================================================================
// Input side
// ============================================================================

uint64_t RoleAPIBase::in_slots_received() const { return pImpl->core->in_received(); }

uint64_t RoleAPIBase::last_seq() const
{
    return pImpl->last_seq.load(std::memory_order_relaxed);
}

void RoleAPIBase::update_last_seq(uint64_t seq)
{
    pImpl->last_seq.store(seq, std::memory_order_relaxed);
}

size_t RoleAPIBase::in_capacity() const
{
    return pImpl->consumer ? pImpl->consumer->queue_capacity() : 0;
}

std::string RoleAPIBase::in_policy() const
{
    return pImpl->consumer ? pImpl->consumer->queue_policy_info() : std::string{};
}

void RoleAPIBase::set_verify_checksum(bool enable)
{
    if (pImpl->consumer)
        pImpl->consumer->set_verify_checksum(enable, false);
}

// ============================================================================
// Diagnostics
// ============================================================================

uint64_t RoleAPIBase::script_error_count() const { return pImpl->core->script_errors(); }
uint64_t RoleAPIBase::loop_overrun_count() const { return pImpl->core->loop_overrun_count(); }
uint64_t RoleAPIBase::last_cycle_work_us() const { return pImpl->core->last_cycle_work_us(); }

uint64_t RoleAPIBase::ctrl_queue_dropped() const
{
    uint64_t total = 0;
    if (pImpl->producer) total += pImpl->producer->ctrl_queue_dropped();
    if (pImpl->consumer) total += pImpl->consumer->ctrl_queue_dropped();
    return total;
}

// ============================================================================
// Spinlocks
// ============================================================================

hub::SharedSpinLock RoleAPIBase::get_spinlock(size_t index)
{
    if (pImpl->producer && pImpl->producer->has_shm())
        return pImpl->producer->get_spinlock(index);
    if (pImpl->consumer && pImpl->consumer->has_shm())
        return pImpl->consumer->get_spinlock(index);
    throw std::runtime_error("get_spinlock: SHM not connected");
}

uint32_t RoleAPIBase::spinlock_count() const
{
    if (pImpl->producer)
        return pImpl->producer->spinlock_count();
    if (pImpl->consumer)
        return pImpl->consumer->spinlock_count();
    return 0;
}

// ============================================================================
// Custom metrics
// ============================================================================

void RoleAPIBase::report_metric(const std::string &key, double value)
{
    pImpl->core->report_metric(key, value);
}

void RoleAPIBase::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    pImpl->core->report_metrics(kv);
}

void RoleAPIBase::clear_custom_metrics()
{
    pImpl->core->clear_custom_metrics();
}

// ============================================================================
// Metrics snapshot — data-driven structure
// ============================================================================

nlohmann::json RoleAPIBase::snapshot_metrics_json() const
{
    nlohmann::json result;
    const bool has_in  = (pImpl->consumer != nullptr);
    const bool has_out = (pImpl->producer != nullptr);

    // Queue metrics: key depends on which sides exist.
    if (has_in && has_out)
    {
        nlohmann::json iq, oq;
        hub::queue_metrics_to_json(iq, pImpl->consumer->queue_metrics());
        hub::queue_metrics_to_json(oq, pImpl->producer->queue_metrics());
        result["in_queue"] = std::move(iq);
        result["out_queue"] = std::move(oq);
    }
    else if (has_out)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->producer->queue_metrics());
        result["queue"] = std::move(q);
    }
    else if (has_in)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->consumer->queue_metrics());
        result["queue"] = std::move(q);
    }

    // Loop metrics.
    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, pImpl->core->loop_metrics());
        result["loop"] = std::move(lm);
    }

    // Role metrics — core counters always present, queue-specific gated on pointers.
    nlohmann::json role;
    role["out_written"]   = pImpl->core->out_written();
    role["in_received"]   = pImpl->core->in_received();
    role["drops"]         = pImpl->core->drops();
    role["script_errors"] = pImpl->core->script_errors();

    if (has_in && has_out)
    {
        role["ctrl_queue_dropped"] = {
            {"input",  pImpl->consumer->ctrl_queue_dropped()},
            {"output", pImpl->producer->ctrl_queue_dropped()}
        };
    }
    else
    {
        uint64_t dropped = 0;
        if (has_out) dropped += pImpl->producer->ctrl_queue_dropped();
        if (has_in)  dropped += pImpl->consumer->ctrl_queue_dropped();
        role["ctrl_queue_dropped"] = dropped;
    }
    result["role"] = std::move(role);

    // Inbox metrics.
    if (pImpl->inbox_queue)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, pImpl->inbox_queue->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    // Custom metrics.
    {
        auto cm = pImpl->core->custom_metrics_snapshot();
        if (!cm.empty())
            result["custom"] = nlohmann::json(cm);
    }

    return result;
}

// ============================================================================
// Shared script state — delegates to RoleHostCore
// ============================================================================

void RoleAPIBase::set_shared_data(const std::string &key, StateValue value)
{
    pImpl->core->set_shared_data(key, std::move(value));
}

std::optional<RoleAPIBase::StateValue> RoleAPIBase::get_shared_data(const std::string &key) const
{
    return pImpl->core->get_shared_data(key);
}

void RoleAPIBase::remove_shared_data(const std::string &key)
{
    pImpl->core->remove_shared_data(key);
}

void RoleAPIBase::clear_shared_data()
{
    pImpl->core->clear_shared_data();
}

// ============================================================================
// Infrastructure access
// ============================================================================

RoleHostCore *RoleAPIBase::core() const     { return pImpl->core; }
hub::Producer *RoleAPIBase::producer() const { return pImpl->producer; }
hub::Consumer *RoleAPIBase::consumer() const { return pImpl->consumer; }
hub::InboxQueue *RoleAPIBase::inbox_queue() const { return pImpl->inbox_queue; }

} // namespace pylabhub::scripting
