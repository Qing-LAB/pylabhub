#pragma once
/**
 * @file hub_queue_factory.hpp
 * @brief hub::Queue — the unified transport-agnostic factory for role code.
 *
 * Per HEP-CORE-0017 §3.3.0 (2026-07-08 topology migration):
 *
 *   std::unique_ptr<QueueReader>
 *   hub::Queue::create_reader(ChannelTopology topology,
 *                             Transport       transport,
 *                             RxOptions       opts);
 *
 *   std::unique_ptr<QueueWriter>
 *   hub::Queue::create_writer(ChannelTopology topology,
 *                             Transport       transport,
 *                             TxOptions       opts);
 *
 * Three facts — side (from role kind) + topology + transport — uniquely
 * determine the socket configuration.  The role provides `topology` from
 * config and inherits `side` from the role kind; the factory picks socket
 * type, bind direction, CURVE role, and endpoint owner from the §3.3.0
 * decision matrix.
 *
 * `hub::Queue` is a static-methods-only class per HEP-CORE-0017 §3.3.0
 * (`class Queue { Queue() = delete; static ... };`); it holds no state.
 * Two-level dispatch: `hub::Queue::create_*` applies §3.3.0 legality
 * gates, then delegates to `ZmqQueue::create_*` or `ShmQueue::create_*`
 * per transport.  Role code NEVER touches libzmq; role-host code NEVER
 * makes bind/connect decisions.
 */

#include "pylabhub_utils_export.h"
#include "utils/hub_queue.hpp"                // QueueReader / QueueWriter
#include "utils/hub_state.hpp"                // ChannelTopology
#include "utils/hub_shm_queue.hpp"            // ShmQueue::{Rx,Tx}CreateOptions
#include "utils/hub_zmq_queue.hpp"            // ZmqQueue::{Rx,Tx}CreateOptions, kZmqDefaultBufferDepth
#include "utils/data_block_config.hpp"        // DataBlockPageSize
#include "utils/data_block_policy.hpp"        // ChecksumPolicy, ConsumerSyncPolicy, DataBlockPolicy
#include "utils/schema_types.hpp"             // SchemaFieldDesc
#include "utils/schema_blds.hpp"              // schema::SchemaInfo
#include "utils/security/curve_keypair.hpp"   // Z85PublicKey
#include "utils/security/key_store.hpp"       // kRoleIdentityName

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::hub
{

/// Transport enum per HEP-CORE-0017 §3.3.0.  Discriminator for the
/// unified factory's transport dispatch.  Wire-string equivalents live
/// in role config (`in_transport` / `out_transport`) and translate via
/// `transport_from_string` below.
enum class Transport
{
    Zmq,
    Shm,
};

/// Parse a wire `transport` string (from role config) into the enum.
/// Returns `std::nullopt` if the input is not one of {"zmq", "shm"};
/// callers surface this as a config-load error.
PYLABHUB_UTILS_EXPORT std::optional<Transport>
transport_from_string(std::string_view s) noexcept;

/// Transport-agnostic options for `hub::Queue::create_reader`.  Common
/// fields plus per-transport extras; the factory reads only the fields
/// matching the requested transport (unused fields are ignored per
/// HEP-CORE-0017 §3.3.0 struct doc).
///
/// Role-derived fields (identity_key_name, zap_domain, instance_id,
/// {consumer,producer,hub}_{uid,name}) MUST be filled by the caller
/// before invoking the factory — they are role state, not role config.
struct RxOptions
{
    // ── Common (transport-agnostic) ────────────────────────────────
    std::vector<SchemaFieldDesc> slot_schema;    // required
    std::string                  slot_packing = "aligned";
    /// See HEP-CORE-0017 §3.3.0 legality gate #4: BINDING side may
    /// set (bind hint); DIALING side must leave empty (endpoint
    /// arrives on REG_ACK).
    std::string                  endpoint_hint;
    std::string                  instance_id;    // empty → auto-derived
    ChecksumPolicy               checksum_policy = ChecksumPolicy::Enforced;

    // ── ZMQ-specific ───────────────────────────────────────────────
    std::string_view identity_key_name =
        ::pylabhub::utils::security::kRoleIdentityName;
    /// Consumer's CURVE serverkey — needed only on DIALING side.
    /// Empty on BINDING (fan-in consumer PULL/bind).
    ::pylabhub::utils::security::Z85PublicKey server_pubkey{};
    size_t          max_buffer_depth = kZmqDefaultBufferDepth;
    std::optional<std::array<uint8_t, 8>> schema_tag;

    // ── SHM-specific ───────────────────────────────────────────────
    std::vector<SchemaFieldDesc> fz_schema;         // reader may verify
    std::string                  fz_packing = "aligned";
    std::string                  channel_name;      // diagnostic + shm_name
    std::string                  consumer_uid;
    std::string                  consumer_name;
    bool                         verify_slot = false;
    bool                         verify_fz   = false;
};

/// Transport-agnostic options for `hub::Queue::create_writer`.
struct TxOptions
{
    // ── Common ─────────────────────────────────────────────────────
    std::vector<SchemaFieldDesc> slot_schema;
    std::string                  slot_packing = "aligned";
    std::string                  endpoint_hint;
    std::string                  instance_id;
    ChecksumPolicy               checksum_policy = ChecksumPolicy::Enforced;

    // ── ZMQ-specific ───────────────────────────────────────────────
    std::string_view identity_key_name =
        ::pylabhub::utils::security::kRoleIdentityName;
    /// Producer's CURVE serverkey — needed only on DIALING (fan-in
    /// producer PUSH-connect).  Empty on BINDING sides.
    ::pylabhub::utils::security::Z85PublicKey server_pubkey{};
    std::string                  zap_domain;   // empty → derived from instance_id
    int                          sndhwm = 0;
    size_t                       send_buffer_depth = kZmqDefaultBufferDepth;
    OverflowPolicy               overflow_policy = OverflowPolicy::Drop;
    int                          send_retry_interval_ms = 10;
    std::optional<std::array<uint8_t, 8>> schema_tag;

    // ── SHM-specific ───────────────────────────────────────────────
    std::string                  channel_name;
    std::vector<SchemaFieldDesc> fz_schema;
    std::string                  fz_packing = "aligned";
    uint32_t                     ring_buffer_capacity{0};
    DataBlockPageSize            page_size{DataBlockPageSize::Unset};
    DataBlockPolicy              policy{DataBlockPolicy::RingBuffer};
    ConsumerSyncPolicy           sync_policy{ConsumerSyncPolicy::Sequential};
    bool                         checksum_slot = false;
    bool                         checksum_fz   = false;
    bool                         always_clear_slot = true;
    std::string                  hub_uid;
    std::string                  hub_name;
    const schema::SchemaInfo    *slot_schema_info = nullptr;
    const schema::SchemaInfo    *fz_schema_info   = nullptr;
    std::string                  producer_uid;
    std::string                  producer_name;
};

/**
 * @class Queue
 * @brief Unified transport-agnostic factory per HEP-CORE-0017 §3.3.0.
 *
 * Static-methods-only.  No instances.  Applies the §3.3.0 legality gates
 * (5 gates enumerated in the HEP) then dispatches to the concrete
 * transport class's topology-parametric factory
 * (`ZmqQueue::create_*` / `ShmQueue::create_*`).  Returns the abstract
 * `unique_ptr<QueueReader>` / `unique_ptr<QueueWriter>` upcast; callers
 * hold only the abstract type per HEP-CORE-0017 §3.3.  See §3.3.0.1
 * for the internal dispatch pseudocode.
 */
class PYLABHUB_UTILS_EXPORT Queue
{
public:
    Queue() = delete;

    /// Construct the consumer-side queue for `(topology, transport)`.
    /// Applies §3.3.0 legality gates 1 + 4 + 5 (gate 2/3 are auto-
    /// satisfied by the enum value being FanOut / OneToOne).  Returns
    /// nullptr + LOGGER_ERROR on any gate failure; caller checks for
    /// null and propagates as config-load failure.
    [[nodiscard]] static std::unique_ptr<QueueReader>
    create_reader(ChannelTopology topology,
                  Transport       transport,
                  RxOptions       opts);

    /// Construct the producer-side queue for `(topology, transport)`.
    /// Same gate semantics as `create_reader`.
    [[nodiscard]] static std::unique_ptr<QueueWriter>
    create_writer(ChannelTopology topology,
                  Transport       transport,
                  TxOptions       opts);

    /// Reader-side legality per HEP-CORE-0017 §3.3.0 matrix:
    /// - Fan-in    → binding
    /// - Fan-out   → dialing
    /// - One-to-one → dialing
    /// Used by gate #4 (endpoint_hint validity) and available to
    /// callers for symmetric decisions.
    [[nodiscard]] static constexpr bool
    reader_is_binding_side(ChannelTopology t) noexcept
    {
        return t == ChannelTopology::FanIn;
    }

    /// Writer-side legality per §3.3.0 matrix:
    /// - Fan-in    → dialing
    /// - Fan-out   → binding
    /// - One-to-one → binding
    [[nodiscard]] static constexpr bool
    writer_is_binding_side(ChannelTopology t) noexcept
    {
        return t != ChannelTopology::FanIn;
    }
};

} // namespace pylabhub::hub
