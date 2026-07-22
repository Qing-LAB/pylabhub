/**
 * @file role_api_flexzone_workers.cpp
 * @brief Worker bodies for L3 role-API flexzone integration tests
 *        (Pattern 3).
 *
 * Why a worker subprocess: `RoleAPIBase` constructs a `ThreadManager`
 * that registers a dynamic lifecycle module.  Without a `LifecycleGuard`
 * in scope, registration half-completes and teardown flakes.
 * `run_gtest_worker` owns the guard for the subprocess's lifetime.
 *
 * ── L3 BYPASS PATTERN ───────────────────────────────────────────────
 *
 * Worker functions in this file BYPASS the role host's
 * `worker_main_` + `setup_infrastructure_` sequence: construct
 * `RoleHostCore` (prod_core, cons_core) + `RoleAPIBase` directly and
 * manually populate state that `RoleHostFrame::setup_infrastructure_`
 * would set in production.  Per `feedback_test_bypass_explicit.md`,
 * this is OWNED, not hidden.
 *
 *   WHY:   L3 isolation — testing the flexzone data-plane integration
 *          (producer → SHM segment → consumer with flexzone schema)
 *          without the broker / role-host startup.  Full role hosts
 *          would require broker + worker threads on both sides; this
 *          isolation focuses on producer/consumer flexzone interaction.
 *
 *   NOT A MOCK — uses real RoleAPIBase + real RoleHostCore + real
 *   ShmQueue.  Follows `feedback_no_mocks_via_observability.md`.
 *
 *   CANONICAL STORAGE THESE BYPASSES POPULATE (keep in sync with
 *   production!):
 *     SLOT path:
 *       - `RoleHostCore::set_out_slot_spec(spec, logical_size)` — producer.
 *       - `RoleHostCore::set_in_slot_spec(spec, logical_size)`  — consumer.
 *     FLEXZONE path:
 *       - `RoleAPIBase::set_flexzone_info_cache_(cache)` — both sides,
 *         each carrying has_*_fz + logical_size + physical_size
 *         (physical = align_to_physical_page(logical)).
 *
 *   RE-EXAMINE WHEN:
 *     - Canonical slot or flexzone storage location moves again
 *       (verify the populates still match where production writes).
 *     - `system_page_size()` or the page-alignment helper changes
 *       (the physical_size populate is derived from align_to_physical_page).
 *     - Annually as part of test-debt review.
 *
 *   FUNCTIONS THIS PATTERN COVERS:
 *     - `build_payload_pair`  (helper used by SHM data-plane tests)
 *     - `shm_consumer_wrong_secret_rejected`
 *     - `shm_slot_checksum_corrupt_detected`
 *     - `shm_flexzone_checksum_corrupt_detected`
 */
#include "role_api_flexzone_workers.h"

#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include "utils/logger.hpp"
#include "utils/lifecycle.hpp"
#include "utils/data_block.hpp"
#include "utils/schema_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include "curve_test_setup.h" // seed_curve_identities
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"
#include "utils/security/key_store.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include <unistd.h> // ::dup for in-process SCM_RIGHTS substitute (#275-S1)

using pylabhub::scripting::ChannelSide;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::utils::Logger;
// Matches the using-directive style in role_api_loop_policy_workers.cpp
// and role_api_raii_workers.cpp so all role-api worker files in this dir
// pull the test helpers the same way.
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::role_api_flexzone
{
namespace
{

/// Lifecycle modules every flexzone worker needs.  Logger for worker-
/// begin/end milestones; SecureSubsystem + ZMQContext + DataBlock for the
/// RoleAPIBase + SHM queue construction.
static auto logger_module()
{
    return Logger::GetLifecycleModule();
}
static auto zmq_module()
{
    return ::pylabhub::hub::GetZMQContextModule();
}
static auto hub_module()
{
    return ::pylabhub::hub::GetDataBlockModule();
}

hub::TxQueueOptions make_producer_opts(const hub::SchemaSpec &slot_spec,
                                       const hub::SchemaSpec &fz_spec)
{
    // HEP-CORE-0041 1i-cleanup #275-S1: capability path only.  No
    // shared_secret (deleted by 1i-cleanup), no shm_name (capability
    // transport has no kernel-namespace name — the segment is an
    // anonymous memfd handed off via SCM_RIGHTS in production; in L3
    // tests we hand the fd off via ::dup in-process).  The caller
    // populates `shm_capability_fd` from
    // `IShmCapabilityProducer::borrow_fd()` before passing to
    // `build_tx_queue`.
    hub::TxQueueOptions opts;
    opts.has_shm = true;
    opts.shm_config.ring_buffer_capacity = 4;
    opts.shm_config.physical_page_size = hub::system_page_size();
    opts.shm_config.policy = hub::DataBlockPolicy::RingBuffer;
    opts.shm_config.consumer_sync_policy = hub::ConsumerSyncPolicy::Sequential;
    opts.shm_config.checksum_policy = hub::ChecksumPolicy::None;
    opts.slot_spec = slot_spec;
    opts.fz_spec = fz_spec;
    return opts;
}

hub::RxQueueOptions make_consumer_opts(const std::string &shm_channel,
                                       const hub::SchemaSpec &slot_spec)
{
    // HEP-CORE-0041 1i-cleanup #275-S1: capability path only.  No
    // shared_secret (deleted by 1i-cleanup).  `shm_name` is purely a
    // diagnostic label here — there is no kernel-namespace name on the
    // capability path.  The caller populates `shm_capability_fd`
    // (typically `::dup` of the producer's borrowed fd in L3
    // in-process tests) before passing to `build_rx_queue`.
    hub::RxQueueOptions opts;
    opts.shm_name = shm_channel;
    opts.slot_spec = slot_spec;
    return opts;
}

/// Compute the byte size of the SHM segment that ShmQueue's
/// fd-source factory expects for a given (slot_spec, fz_spec, ring,
/// page) config.  Mirrors `RoleHostFrame::prepare_tx_capability_`
/// exactly — the fd-source factory validates `fstat(fd).st_size ==
/// this value` (HEP-CORE-0041 §6.3 + data_block.hpp:1308).
size_t shm_segment_total_size(const hub::SchemaSpec &slot_spec, const hub::SchemaSpec &fz_spec,
                              uint32_t ring_buffer_capacity, hub::DataBlockPageSize page_size,
                              hub::DataBlockPolicy policy, hub::ConsumerSyncPolicy sync_policy,
                              hub::ChecksumPolicy checksum_policy)
{
    const auto slot_fields = hub::schema_spec_to_zmq_fields(slot_spec);
    auto [slot_layout, item_size] = hub::compute_field_layout(slot_fields, slot_spec.packing);
    size_t fz_size = 0;
    if (fz_spec.has_schema && !fz_spec.fields.empty())
    {
        const auto fz_fields = hub::schema_spec_to_zmq_fields(fz_spec);
        auto [fz_layout, raw_fz_size] = hub::compute_field_layout(fz_fields, fz_spec.packing);
        fz_size = hub::align_to_physical_page(raw_fz_size);
    }
    hub::DataBlockConfig cfg;
    cfg.logical_unit_size = item_size;
    cfg.flex_zone_size = fz_size;
    cfg.ring_buffer_capacity = ring_buffer_capacity;
    cfg.physical_page_size = page_size;
    cfg.policy = policy;
    cfg.consumer_sync_policy = sync_policy;
    cfg.checksum_policy = checksum_policy;
    return hub::datablock_layout_total_size(cfg);
}

/// Compute byte offsets for each field in @p spec (aligned packing).
std::vector<hub::FieldLayout> layout_for(const hub::SchemaSpec &spec)
{
    auto descs = hub::to_field_descs(spec.fields);
    auto [layout, _] = hub::compute_field_layout(descs, "aligned");
    return layout;
}

/// Produce (producer, consumer) RoleAPIBase pair on the same SHM channel
/// with @p fz_spec as the flexzone layout.
///
/// NON-MOVABLE / NON-COPYABLE: each `RoleAPIBase` stored here holds a
/// reference to the matching `RoleHostCore` inside this same struct.
/// Moving or copying would dangle.  Pattern: construct on stack, fill
/// via `build_payload_pair`, use, let destructor run in place.
///
/// HEP-CORE-0041 1i-cleanup #275-S1: `shm_transport` owns the memfd
/// for the capability path.  Declared FIRST so that destruction order
/// (reverse declaration order) is: cons → prod → cons_core → prod_core
/// → shm_transport.  The queues' fd-source dups + mmaps are released
/// when the RoleAPIBase instances destruct; the original memfd is
/// closed last when `shm_transport` destructs.  Mirrors the production
/// ordering where RoleHostFrame::shm_transport_ outlives the role's
/// queues.
struct PayloadPair
{
    std::unique_ptr<pylabhub::utils::security::IShmCapabilityProducer> shm_transport;
    RoleHostCore prod_core;
    RoleHostCore cons_core;
    std::unique_ptr<RoleAPIBase> prod;
    std::unique_ptr<RoleAPIBase> cons;
    size_t fz_size{0};

    PayloadPair() = default;
    PayloadPair(const PayloadPair &) = delete;
    PayloadPair(PayloadPair &&) = delete;
    PayloadPair &operator=(const PayloadPair &) = delete;
    PayloadPair &operator=(PayloadPair &&) = delete;
};

/// Optional knobs for `build_payload_pair`.  Defaults match the flexzone
/// round-trip tests: no slot checksum, no flexzone checksum.
struct PayloadPairOpts
{
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::None};
    bool flexzone_checksum{false};
};

/// IMPORTANT: `build_payload_pair` uses gtest `ASSERT_*` macros from a
/// non-test function.  This is only safe under `throw_on_failure=true`,
/// which `run_gtest_worker` sets before invoking the test lambda that
/// calls this helper.  Do NOT call `build_payload_pair` directly from a
/// plain `TEST_F` body or any other context that lacks
/// `throw_on_failure` — an ASSERT failure would silently `return;` from
/// this function and leave the caller with a partially-built pair.
///
/// HEP-CORE-0041 1i-cleanup #275-S1: capability-path migration.  The
/// helper:
///   1. Computes the SHM segment size from (slot_spec, fz_spec) via
///      `shm_segment_total_size` — same math as
///      `RoleHostFrame::prepare_tx_capability_` (production parity).
///   2. Mints an anonymous memfd via `create_shm_capability_producer`
///      (the same factory production uses) and stows it on
///      `out.shm_transport`.
///   3. Borrows the fd, sets it on `tx_opts.shm_capability_fd`, builds
///      the producer queue — the queue dups internally per the §6.3
///      fd-source contract.
///   4. `::dup`s the same fd (substitute for SCM_RIGHTS in this
///      in-process test), sets it on `rx_opts.shm_capability_fd`,
///      builds the consumer queue.  The dup is consumed by the queue
///      (queue dups internally; we close ours immediately after
///      `build_rx_queue` returns).
void build_payload_pair(PayloadPair &out, const hub::SchemaSpec &slot_spec,
                        const hub::SchemaSpec &fz_spec, const std::string &channel,
                        const char *uid_tag, const PayloadPairOpts &opts = {})
{
    out.fz_size = hub::align_to_physical_page(hub::compute_schema_size(fz_spec, fz_spec.packing));

    hub::TxQueueOptions tx_opts = make_producer_opts(slot_spec, fz_spec);
    tx_opts.shm_config.checksum_policy = opts.checksum_policy;
    tx_opts.flexzone_checksum = opts.flexzone_checksum;

    // Mint the capability transport with the EXACT segment size the
    // fd-source factory will validate against (see
    // `shm_segment_total_size` above for the size derivation that
    // mirrors `RoleHostFrame::prepare_tx_capability_`).
    namespace sec = pylabhub::utils::security;
    const size_t total =
        shm_segment_total_size(slot_spec, fz_spec, tx_opts.shm_config.ring_buffer_capacity,
                               tx_opts.shm_config.physical_page_size, tx_opts.shm_config.policy,
                               tx_opts.shm_config.consumer_sync_policy, opts.checksum_policy);
    ASSERT_GT(total, 0u) << "shm_segment_total_size returned 0";
    out.shm_transport = sec::create_shm_capability_producer(total);
    ASSERT_NE(out.shm_transport, nullptr)
        << "create_shm_capability_producer(" << total << ") returned nullptr";
    tx_opts.shm_capability_fd = out.shm_transport->borrow_fd();

    out.prod =
        std::make_unique<RoleAPIBase>(out.prod_core, "prod", std::string("prod.fz.") + uid_tag);
    out.prod->set_channel(channel);
    out.prod->set_name("fz-prod");
    // Populate the FlexzoneInfoCache (see file-header BYPASS PATTERN).
    // Mirrors RoleHostFrame::setup_infrastructure_ step 6.5: logical =
    // compute_schema_size; physical = align_to_physical_page(logical).
    // Producer presence → tx side.
    {
        RoleAPIBase::FlexzoneInfoCache fz_info;
        fz_info.has_tx_fz = fz_spec.has_schema;
        fz_info.tx_logical_size = hub::compute_schema_size(fz_spec, fz_spec.packing);
        fz_info.tx_physical_size = hub::align_to_physical_page(fz_info.tx_logical_size);
        out.prod->set_flexzone_info_cache_(fz_info);
    }
    ASSERT_TRUE(out.prod->build_tx_queue(tx_opts));
    // HEP-CORE-0036 §5b B-5 (#290): `apply_producer_reg_ack` requires a
    // valid `channel_name` post-B-5.  In this L3 fixture the SHM tx
    // queue is fully wired by `build_tx_queue` (capability path), and
    // `apply_master_approval` is a no-op for SHM (no `shm_secret`
    // expected in the ACK).  The pre-B-5 empty-object call was
    // therefore a vestigial no-op that never drove FSM state nor cache;
    // dropping it removes a fictional "ACK applied" assertion that
    // does not match what this fixture exercises.  The full ACK path
    // is tested at L3 Pattern-4 + L4 e2e.

    // One committed slot so the consumer can attach.
    void *slot = out.prod->write_acquire(std::chrono::milliseconds{500});
    ASSERT_NE(slot, nullptr);
    out.prod->write_commit();

    // Hand the consumer its own fd via ::dup — in-process substitute
    // for the §5.5 SCM_RIGHTS handoff used in production (the dup'd
    // fd is a distinct integer pointing at the same kernel memfd
    // object, matching what `recvmsg` SCM_RIGHTS would deliver).
    hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec);
    rx_opts.checksum_policy = opts.checksum_policy;
    rx_opts.flexzone_checksum = opts.flexzone_checksum;
    const int rx_fd_dup = ::dup(out.shm_transport->borrow_fd());
    ASSERT_GE(rx_fd_dup, 0) << "::dup(memfd) failed: errno=" << errno;
    rx_opts.shm_capability_fd = rx_fd_dup;

    out.cons =
        std::make_unique<RoleAPIBase>(out.cons_core, "cons", std::string("cons.fz.") + uid_tag);
    out.cons->set_channel(channel);
    out.cons->set_name("fz-cons");
    // Populate the FlexzoneInfoCache (see file-header BYPASS PATTERN).
    // Consumer presence → rx side.
    {
        RoleAPIBase::FlexzoneInfoCache fz_info;
        fz_info.has_rx_fz = fz_spec.has_schema;
        fz_info.rx_logical_size = hub::compute_schema_size(fz_spec, fz_spec.packing);
        fz_info.rx_physical_size = hub::align_to_physical_page(fz_info.rx_logical_size);
        out.cons->set_flexzone_info_cache_(fz_info);
    }
    const bool rx_built = out.cons->build_rx_queue(rx_opts);
    // The queue's fd-source factory dups internally — close our dup
    // immediately after build_rx_queue returns (success or failure) to
    // avoid a leak.  Mirrors the production teardown: the consumer
    // dial closes its own connected_fd after `set_shm_capability_fd`
    // returns in `apply_consumer_reg_ack_shm_`.
    ::close(rx_fd_dup);
    ASSERT_TRUE(rx_built);
    // HEP-CORE-0036 §5b B-5 (#290): vestigial.  The SHM rx queue is
    // already Active after `build_rx_queue` consumes the dup'd
    // capability fd.  Pre-B-5 the empty-object ACK fell through to the
    // ZMQ branch (empty `data_transport`) where `apply_master_approval`
    // is a no-op on a ShmQueue without `shm_secret`.  Post-B-5 this
    // path hard-errors on missing `channel_name` / `data_transport`,
    // and the call adds no behavior to assert against in this fixture
    // (no handler, no cache write, no FSM transition observable).
}

} // namespace

// ============================================================================
// shm_roundtrip — T2 + T3: bidirectional flexzone access
// ============================================================================

int shm_roundtrip()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::simple_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec, make_test_channel_name("fz_roundtrip"),
                               "TEST");

            // Flexzone contract — three independent paths must agree:
            //   pair.fz_size                       — test-computed expectation
            //   flexzone_size(side)                — live ShmQueue's region span (from DataBlock)
            //   flexzone_physical_size(side)       — FlexzoneInfoCache (populated by the bypass)
            // And the cache-internal invariant must hold:
            //   physical_size == align_to_physical_page(logical_size)
            // Asserting these together cross-checks: (a) the test's
            // expected size matches DataBlock's internal layout
            // computation; (b) the cache matches the runtime queue;
            // (c) the cache's two stored values are consistent.

            void *tx_fz = pair.prod->flexzone(ChannelSide::Tx);
            ASSERT_NE(tx_fz, nullptr) << "Producer Tx flexzone must be non-null for SHM queue";
            EXPECT_TRUE(pair.prod->tx_has_shm());

            const size_t tx_queue_fz_sz = pair.prod->flexzone_size(ChannelSide::Tx);
            const size_t tx_cache_logical = pair.prod->flexzone_logical_size(ChannelSide::Tx);
            const size_t tx_cache_physical = pair.prod->flexzone_physical_size(ChannelSide::Tx);
            EXPECT_EQ(tx_queue_fz_sz, pair.fz_size)
                << "queue region size must match test computation";
            EXPECT_EQ(tx_cache_physical, tx_queue_fz_sz)
                << "cache physical_size must match the live queue region";
            EXPECT_EQ(tx_cache_physical, hub::align_to_physical_page(tx_cache_logical))
                << "cache invariant: physical == align_to_physical_page(logical)";
            EXPECT_GT(tx_cache_logical, 0u) << "logical size must be > 0 when fz_spec has a schema";

            void *rx_fz = pair.cons->flexzone(ChannelSide::Rx);
            ASSERT_NE(rx_fz, nullptr) << "Consumer Rx flexzone must be non-null for SHM queue";
            EXPECT_TRUE(pair.cons->rx_has_shm());

            const size_t rx_queue_fz_sz = pair.cons->flexzone_size(ChannelSide::Rx);
            const size_t rx_cache_logical = pair.cons->flexzone_logical_size(ChannelSide::Rx);
            const size_t rx_cache_physical = pair.cons->flexzone_physical_size(ChannelSide::Rx);
            EXPECT_EQ(rx_queue_fz_sz, pair.fz_size) << "Rx queue region size must match Tx";
            EXPECT_EQ(rx_cache_physical, rx_queue_fz_sz)
                << "Rx cache physical_size must match the live queue region";
            EXPECT_EQ(rx_cache_physical, hub::align_to_physical_page(rx_cache_logical))
                << "Rx cache invariant: physical == align_to_physical_page(logical)";
            EXPECT_GT(rx_cache_logical, 0u);

            // T2: producer's sentinel reaches the consumer's Rx flexzone.
            const float sentinel = 42.5f;
            std::memcpy(tx_fz, &sentinel, sizeof(sentinel));
            float read_back = 0.0f;
            std::memcpy(&read_back, rx_fz, sizeof(read_back));
            EXPECT_FLOAT_EQ(read_back, sentinel)
                << "Consumer Rx flexzone must see producer's written sentinel";

            // T3: consumer writes back, producer reads — bidirectional
            // (HEP-CORE-0002 TABLE 1 user-managed region).
            const float ack = 99.9f;
            std::memcpy(rx_fz, &ack, sizeof(ack));
            float prod_read = 0.0f;
            std::memcpy(&prod_read, tx_fz, sizeof(prod_read));
            EXPECT_FLOAT_EQ(prod_read, ack) << "Producer Tx flexzone must see consumer's ack";

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// zmq_tx_null — ZMQ-only Tx has no flexzone
// ============================================================================

int zmq_tx_null()
{
    return run_gtest_worker(
        [&]()
        {
            // HEP-CORE-0040 §172: `build_tx_queue` constructs a
            // CURVE-wired PUSH queue via `*_with_auth`, which requires
            // the role's identity to be present in the process
            // KeyStore under `kRoleIdentityName`.  Seed it before
            // calling.  Without this seed the factory returns null
            // and ASSERT_TRUE on build_tx_queue fails.
            auto curve = pylabhub::tests::make_curve_setup({"prod.zmq-fz.tx"});
            pylabhub::tests::seed_curve_identities(curve);
            // The role-side data path uses the canonical production
            // name `kRoleIdentityName` (build_tx_queue uses it).  Add
            // that name pointing at the test's hub keypair (any
            // valid CURVE keypair works; this test doesn't exercise
            // wire authentication).
            pylabhub::utils::security::secure().keys().add_identity_from_z85(
                pylabhub::utils::security::kRoleIdentityName, curve.hub.public_z85,
                curve.hub.secret_z85);

            RoleHostCore core;
            auto api = std::make_unique<RoleAPIBase>(core, "prod", "prod.zmq-fz.tx");
            api->set_channel("test.fz.zmq.tx");

            hub::TxQueueOptions opts;
            opts.has_shm = false;
            opts.data_transport = "zmq";
            opts.zmq_node_endpoint = "tcp://127.0.0.1:0";
            opts.zmq_bind = true;
            opts.slot_spec = pylabhub::tests::simple_schema();

            ASSERT_TRUE(api->build_tx_queue(opts));
            // HEP-CORE-0036 §5b B-5 (#290): `channel_name` is now
            // mandatory in REG_ACK.  `initial_allowlist` is omitted —
            // ZmqQueue::apply_master_approval tolerates absence (keeps
            // prior allowlist state) and still drives Standby → Active
            // on the PUSH/bind side, which is what `queue_mechanism`
            // below requires to observe Mechanism::Curve.
            ASSERT_TRUE(api->apply_producer_reg_ack(
                nlohmann::json{{"channel_name", "test.fz.zmq.tx"},
                               // HEP-CORE-0042 §5.5.3 — apply_producer_reg_ack
                               // hard-errors on absent/zero `instance_id`.  Broker
                               // assigns starting at 1 (§5.2 monotonic).
                               {"instance_id", 1u}}));

            EXPECT_EQ(api->flexzone(ChannelSide::Tx), nullptr);
            EXPECT_EQ(api->flexzone_size(ChannelSide::Tx), 0u);
            EXPECT_FALSE(api->tx_has_shm());

            // C5 follow-up (#186) — script-visible CURVE mechanism
            // accessor pins the HEP-CORE-0035 §2 invariant at the
            // RoleAPIBase tier: any started ZmqQueue reports Curve.
            EXPECT_EQ(api->queue_mechanism(ChannelSide::Tx), pylabhub::hub::Mechanism::Curve);
            // Rx side is not wired in this scenario — accessor must
            // return Uninitialized (not throw, not return Plaintext).
            EXPECT_EQ(api->queue_mechanism(ChannelSide::Rx),
                      pylabhub::hub::Mechanism::Uninitialized);

            api->close_queues();

            // After close_queues(), the Tx queue is destroyed; the
            // accessor returns Uninitialized.  Pins the
            // "stop ⇒ reset" half of the invariant.
            EXPECT_EQ(api->queue_mechanism(ChannelSide::Tx),
                      pylabhub::hub::Mechanism::Uninitialized);
        },
        "role_api_flexzone::zmq_tx_null", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// zmq_rx_null — ZMQ-only Rx has no flexzone
// ============================================================================

int zmq_rx_null()
{
    return run_gtest_worker(
        [&]()
        {
            // Rx-side flexzone contract is a property of the Rx queue
            // itself.  Rx-build succeeds even against an unreachable
            // endpoint because the ZMQ connect is non-blocking.
            //
            // HEP-CORE-0035 §2 + #160 (C4): CURVE is unconditional on
            // every role↔hub data path, so `build_rx_queue` requires
            // the canonical KeyStore identity to be seeded AND
            // `producer_peers[0]` to carry the producer's CURVE pubkey.
            // We provide a synthetic peer (unreachable endpoint + a
            // freshly minted Z85 pubkey) so the test exercises the
            // flexzone contract without actually completing a CURVE
            // handshake.
            auto curve = pylabhub::tests::make_curve_setup({"prod.zmq-fz.rx"});
            pylabhub::tests::seed_curve_identities(curve);
            pylabhub::utils::security::secure().keys().add_identity_from_z85(
                pylabhub::utils::security::kRoleIdentityName, curve.hub.public_z85,
                curve.hub.secret_z85);

            RoleHostCore core;
            auto api = std::make_unique<RoleAPIBase>(core, "cons", "cons.zmq-fz.rx");
            api->set_channel("test.fz.zmq.rx");

            hub::RxQueueOptions rx_opts;
            rx_opts.data_transport = "zmq";
            rx_opts.slot_spec = pylabhub::tests::simple_schema();
            // Stage 1D (task #193, 2026-06-15): the consumer's connect
            // target now lives ONLY in producer_peers (HEP-CORE-0036
            // §6.4 + §6.7).  Test pre-populates here to enter Configured
            // at construction (legacy fast-path); production never does
            // — broker is the master via CONSUMER_REG_ACK.
            rx_opts.producer_peers.push_back(pylabhub::hub::ProducerPeer{
                /*role_uid=*/"prod.zmq-fz.rx",
                /*endpoint=*/"tcp://127.0.0.1:45599",
                /*pubkey_z85=*/curve.role("prod.zmq-fz.rx").public_z85});

            ASSERT_TRUE(api->build_rx_queue(rx_opts));
            // HEP-CORE-0042 Phase 3b.2 (2026-07-02) —
            // `apply_consumer_reg_ack` now issues §7.1
            // `CONSUMER_ATTACH_REQ_ZMQ` per declared producer
            // BEFORE queue Standby → Active.  That requires a live
            // BRC + a real broker to service the pre-attach REQs.
            // This isolation worker has neither (no `set_handler`,
            // no broker subprocess), so the call would fail on the
            // BRC lookup and return false.  The flexzone assertions
            // below depend on `build_rx_queue` output only — no
            // apply_consumer_reg_ack needed to exercise them.
            // Pre-3b.2 the call was a defensive no-op; now the test
            // is scoped precisely to the flexzone check.

            EXPECT_EQ(api->flexzone(ChannelSide::Rx), nullptr);
            EXPECT_EQ(api->flexzone_size(ChannelSide::Rx), 0u);
            EXPECT_FALSE(api->rx_has_shm());

            api->close_queues();
        },
        "role_api_flexzone::zmq_rx_null", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// shm_checksum_roundtrip — flexzone checksum update/verify
// ============================================================================

int shm_checksum_roundtrip()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::simple_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec, make_test_channel_name("fz_checksum"),
                               "CSUM", {hub::ChecksumPolicy::Enforced, /*flexzone_checksum=*/true});

            void *tx_fz = pair.prod->flexzone(ChannelSide::Tx);
            ASSERT_NE(tx_fz, nullptr);

            // Write payload then update the flexzone checksum — Tx-side
            // call, recomputes BLAKE2b over the user-managed region and
            // writes it into the SHM header slot.  `sync_flexzone_checksum`
            // is an alias for the same operation.
            const double value = 3.14159;
            std::memcpy(tx_fz, &value, sizeof(value));
            EXPECT_TRUE(pair.prod->update_flexzone_checksum())
                << "update_flexzone_checksum must succeed on SHM Tx";

            void *rx_fz = pair.cons->flexzone(ChannelSide::Rx);
            ASSERT_NE(rx_fz, nullptr);

            // Positive path: payload round-trips through the same
            // physical region.  NOTE: this does NOT alone prove
            // verify_checksum actually fires — if it were a no-op, a
            // valid-checksum read would still succeed.  The negative
            // proofs of wiring live in:
            //   - `shm_slot_checksum_corrupt_detected` (slot verify)
            //   - `shm_flexzone_checksum_corrupt_detected` (fz verify)
            double read_back = 0.0;
            std::memcpy(&read_back, rx_fz, sizeof(read_back));
            EXPECT_DOUBLE_EQ(read_back, value);

            // The committed slot (written by build_payload_pair) must
            // read back successfully under enforced verify.
            const void *rs = pair.cons->read_acquire(std::chrono::milliseconds{500});
            ASSERT_NE(rs, nullptr) << "read_acquire must succeed when checksum is valid";
            pair.cons->read_release();

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_checksum_roundtrip", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// shm_roundtrip_padding_sensitive — float64 + uint8 + int32
// ============================================================================
//
// padding_schema(): ts(float64)@0, flag(uint8)@8, count(int32)@12
// Total: 16 bytes aligned, 13 bytes packed.
//
// What this pins:
// 1. Field offsets on Tx match field offsets on Rx.  Schema layout is
//    deterministic — if it drifts, this test catches it.
// 2. The 3-byte padding between flag (1 byte at offset 8) and count
//    (4 bytes at offset 12) is NOT stomped by either side writing
//    past its declared bytes.
// 3. Writing a full struct on Tx and reading each field independently
//    on Rx produces bit-exact values.

int shm_roundtrip_padding_sensitive()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::padding_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec, make_test_channel_name("fz_padding"),
                               "PAD");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 3u) << "padding_schema has 3 fields";
            // Pin the aligned offsets + total size — any shift or
            // mis-pad would be caught here even without the downstream
            // read-back assertions.  padding_schema total = 16 aligned
            // (13 packed) per test_schema_helpers.h.
            EXPECT_EQ(layout[0].offset, 0u);  // ts: float64 @ 0
            EXPECT_EQ(layout[1].offset, 8u);  // flag: uint8 @ 8
            EXPECT_EQ(layout[2].offset, 12u); // count: int32 @ 12 (3 byte pad)
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 16u);
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "packed"), 13u);

            // Producer: write distinctive values at computed offsets.
            auto *tx = static_cast<uint8_t *>(pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            const double ts = 3.14159265358979;
            const uint8_t flag = 0xAB;
            const int32_t count = -42;

            std::memcpy(tx + layout[0].offset, &ts, sizeof(ts));
            std::memcpy(tx + layout[1].offset, &flag, sizeof(flag));
            std::memcpy(tx + layout[2].offset, &count, sizeof(count));

            // Consumer: read each field at the SAME offsets, verify bit-exact.
            auto *rx = static_cast<const uint8_t *>(pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            double read_ts = 0.0;
            uint8_t read_flag = 0;
            int32_t read_count = 0;
            std::memcpy(&read_ts, rx + layout[0].offset, sizeof(read_ts));
            std::memcpy(&read_flag, rx + layout[1].offset, sizeof(read_flag));
            std::memcpy(&read_count, rx + layout[2].offset, sizeof(read_count));

            EXPECT_DOUBLE_EQ(read_ts, ts);
            EXPECT_EQ(read_flag, flag);
            EXPECT_EQ(read_count, count);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_padding_sensitive", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// shm_roundtrip_all_types — every scalar type across alignment classes
// ============================================================================
//
// all_types_schema(): 13 fields ordered by descending alignment — f64, i64,
// u64, f32, i32, u32, i16, u16, bool, i8, u8, bytes[4], string[8].  This
// is the canonical "one field per supported scalar type" schema (see
// test_schema_helpers.h comment).  Each field gets a distinctive
// sentinel value that would expose wrong-offset reads as visibly wrong.

int shm_roundtrip_all_types()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::all_types_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec, make_test_channel_name("fz_alltypes"),
                               "ALL");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 13u);
            // Pin the aligned total size — the comment in
            // test_schema_helpers.h documents the expected value: 55
            // bytes of field data rounded up to 56 (multiple of 8) for
            // the 8-byte alignment of the first field.  A bug that
            // dropped trailing padding or miscounted a field would
            // shift this number and be caught here.
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 56u);

            auto *tx = static_cast<uint8_t *>(pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            // Distinctive sentinels — value chosen so that misreading at a
            // neighbour's offset would show up as a different (visible) number.
            const double v_f64 = -1.7976931348623157e+100; // near-min double
            const int64_t v_i64 = static_cast<int64_t>(0x0123456789ABCDEFLL);
            const uint64_t v_u64 = 0xFEDCBA9876543210ULL;
            const float v_f32 = 1.5e10F;
            const int32_t v_i32 = -0x12345678;
            const uint32_t v_u32 = 0xDEADBEEFu;
            const int16_t v_i16 = -0x1234;
            const uint16_t v_u16 = 0xBEEF;
            const bool v_bool = true;
            const int8_t v_i8 = -42;
            const uint8_t v_u8 = 0x5A;
            const uint8_t v_bytes[4] = {0x11, 0x22, 0x33, 0x44};
            const char v_str[8] = {'h', 'e', 'l', 'l', 'o', '\0', 'P', 'Q'};

            std::memcpy(tx + layout[0].offset, &v_f64, sizeof(v_f64));
            std::memcpy(tx + layout[1].offset, &v_i64, sizeof(v_i64));
            std::memcpy(tx + layout[2].offset, &v_u64, sizeof(v_u64));
            std::memcpy(tx + layout[3].offset, &v_f32, sizeof(v_f32));
            std::memcpy(tx + layout[4].offset, &v_i32, sizeof(v_i32));
            std::memcpy(tx + layout[5].offset, &v_u32, sizeof(v_u32));
            std::memcpy(tx + layout[6].offset, &v_i16, sizeof(v_i16));
            std::memcpy(tx + layout[7].offset, &v_u16, sizeof(v_u16));
            std::memcpy(tx + layout[8].offset, &v_bool, sizeof(v_bool));
            std::memcpy(tx + layout[9].offset, &v_i8, sizeof(v_i8));
            std::memcpy(tx + layout[10].offset, &v_u8, sizeof(v_u8));
            std::memcpy(tx + layout[11].offset, v_bytes, sizeof(v_bytes));
            std::memcpy(tx + layout[12].offset, v_str, sizeof(v_str));

            auto *rx = static_cast<const uint8_t *>(pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            double r_f64 = 0;
            std::memcpy(&r_f64, rx + layout[0].offset, sizeof(r_f64));
            int64_t r_i64 = 0;
            std::memcpy(&r_i64, rx + layout[1].offset, sizeof(r_i64));
            uint64_t r_u64 = 0;
            std::memcpy(&r_u64, rx + layout[2].offset, sizeof(r_u64));
            float r_f32 = 0;
            std::memcpy(&r_f32, rx + layout[3].offset, sizeof(r_f32));
            int32_t r_i32 = 0;
            std::memcpy(&r_i32, rx + layout[4].offset, sizeof(r_i32));
            uint32_t r_u32 = 0;
            std::memcpy(&r_u32, rx + layout[5].offset, sizeof(r_u32));
            int16_t r_i16 = 0;
            std::memcpy(&r_i16, rx + layout[6].offset, sizeof(r_i16));
            uint16_t r_u16 = 0;
            std::memcpy(&r_u16, rx + layout[7].offset, sizeof(r_u16));
            bool r_bool = false;
            std::memcpy(&r_bool, rx + layout[8].offset, sizeof(r_bool));
            int8_t r_i8 = 0;
            std::memcpy(&r_i8, rx + layout[9].offset, sizeof(r_i8));
            uint8_t r_u8 = 0;
            std::memcpy(&r_u8, rx + layout[10].offset, sizeof(r_u8));
            uint8_t r_bytes[4] = {0, 0, 0, 0};
            std::memcpy(r_bytes, rx + layout[11].offset, sizeof(r_bytes));
            char r_str[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            std::memcpy(r_str, rx + layout[12].offset, sizeof(r_str));

            EXPECT_DOUBLE_EQ(r_f64, v_f64);
            EXPECT_EQ(r_i64, v_i64);
            EXPECT_EQ(r_u64, v_u64);
            EXPECT_FLOAT_EQ(r_f32, v_f32);
            EXPECT_EQ(r_i32, v_i32);
            EXPECT_EQ(r_u32, v_u32);
            EXPECT_EQ(r_i16, v_i16);
            EXPECT_EQ(r_u16, v_u16);
            EXPECT_EQ(r_bool, v_bool);
            EXPECT_EQ(r_i8, v_i8);
            EXPECT_EQ(r_u8, v_u8);
            EXPECT_EQ(std::memcmp(r_bytes, v_bytes, 4), 0);
            EXPECT_EQ(std::memcmp(r_str, v_str, 8), 0);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_all_types", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// shm_roundtrip_array_field — scalar + array with alignment
// ============================================================================
//
// fz_array_schema(): version(uint32)@0, values(float64[2])@8
// Exercises count>1 handling + 4-byte pad between scalar and array.

int shm_roundtrip_array_field()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::fz_array_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec, make_test_channel_name("fz_array"), "ARR");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 2u);
            EXPECT_EQ(layout[0].offset, 0u); // uint32 @ 0
            EXPECT_EQ(layout[1].offset, 8u); // float64[2] @ 8 (4 byte pad)
            EXPECT_EQ(layout[1].byte_size, sizeof(double) * 2u);
            // Total aligned = 4 (u32) + 4 (pad) + 16 (f64[2]) = 24.
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 24u);

            auto *tx = static_cast<uint8_t *>(pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            const uint32_t version = 7;
            const double values[2] = {1.5, 2.5};
            std::memcpy(tx + layout[0].offset, &version, sizeof(version));
            std::memcpy(tx + layout[1].offset, values, sizeof(values));

            auto *rx = static_cast<const uint8_t *>(pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            uint32_t r_version = 0;
            double r_values[2] = {0.0, 0.0};
            std::memcpy(&r_version, rx + layout[0].offset, sizeof(r_version));
            std::memcpy(r_values, rx + layout[1].offset, sizeof(r_values));

            EXPECT_EQ(r_version, version);
            EXPECT_DOUBLE_EQ(r_values[0], values[0]);
            EXPECT_DOUBLE_EQ(r_values[1], values[1]);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_array_field", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ============================================================================
// NEGATIVE PATHS — prove the error surface actually fires
// ============================================================================

// ----------------------------------------------------------------------------
// shm_consumer_wrong_secret_rejected — RETIRED 2026-06-23 (HEP-CORE-0041
// 1i-mig-5 / #273; see docs/README/README_testing.md §1.2 rule 6)
// ----------------------------------------------------------------------------
//
// Original intent: validate the secret-mismatch gate end-to-end at the
// role layer.  Producer created SHM with secret S; consumer attempted
// attach with secret S^1 (wrong); the legacy
// `ShmQueue::create_reader(name, secret, ...)` factory's
// `memcmp(stored_secret, supplied_secret, 64) != 0` check rejected the
// attach.  Then verified a follow-up attach with the correct secret
// succeeded (recovery-after-fail).  Exercised the secret-based mutual
// auth gate end-to-end through both producer-side
// (`shm_config.shared_secret`) and consumer-side
// (`shm_shared_secret`) legacy machinery.
//
// Why retired:
//   1. The secret-mismatch gate IS the surface HEP-CORE-0041 deletes.
//      Under HEP-CORE-0041 §6 the auth gate moves from
//      header-stored-secret memcmp to capability-transport SCM_RIGHTS
//      + crypto_box challenge-response.  There is no "wrong secret" on
//      the capability path — the secret machinery is gone.  The whole
//      test premise dissolves; this is a rule-6 retirement
//      (`docs/README/README_testing.md` §1.2): when a design change
//      deletes the failure mode a test was probing, retire the test
//      rather than manufacture a synthetic mock that drives a
//      no-longer-meaningful failure path.
//   2. Coverage that the role-layer composition preserves:
//      * Per-API failure modes: `test_hub_shm_queue_capability.cpp`
//        (L2) Tests 1, 3, 4, 5, 6 — Standby/Active state machine,
//        SetCapabilityFd refusals, defensive negative-fd guardrail.
//      * Recovery-after-fail (build can be re-attempted with valid
//        inputs after rejecting bad ones): implicit in the L2
//        Standby → Configured → Active state machine, which proves
//        each transition is independently driven (no sticky
//        rejection state).
//      * End-to-end role-layer composition under real failure
//        conditions: future L4 test (1k / #258).
//   3. Constructing a synthetic-fail capability-path equivalent
//      (e.g. SCM_RIGHTS recv times out, crypto_box verify fails)
//      would manufacture a failure path the role layer doesn't drive
//      synchronously.  Per `docs/README/README_testing.md` §1.2
//      rule 2 (mock substitutes a value, not a code path), inventing
//      that test would be a bypass, not a mock.
//
// Future maintainers: do NOT reintroduce a role-level "build_rx_queue
// rejects bogus SHM auth" test on the capability path without first
// identifying a role-layer behavior that L2 + L4 do not cover, AND
// without satisfying every bullet in `docs/README/README_testing.md`
// §1.2.

// NOTE: no `shm_consumer_schema_mismatch_rejected` worker at this layer.
// `ShmQueue::create_reader` deliberately does NOT validate schema shape
// against the producer's header — that check lives at the broker /
// channel-registration layer, where the schema hash cross-check runs.
// A meaningful schema-mismatch negative test belongs in the broker
// tier, not in the no-broker L3 role-api integration tests.

// ----------------------------------------------------------------------------
// shm_consumer_nonexistent_rejected — RETIRED 2026-06-23 (HEP-CORE-0041
// 1i-mig-5 / #273)
// ----------------------------------------------------------------------------
//
// Original intent: validate that `build_rx_queue` returns false + leaves
// no half-state when the consumer tries to attach to a named SHM
// segment that was never created.  Drove the legacy
// `ShmQueue::create_reader(name, secret, ...)` factory by setting
// `RxQueueOptions::shm_shared_secret = 0xF00D...` + a nonexistent
// `shm_name`.  This exercised the `shm_open(name)` not-found path.
//
// Why retired:
//   1. The failure mode no longer exists on the capability transport
//      (HEP-CORE-0041 §6).  Post-1i-mig-4 (#272) the consumer-side SHM
//      attach goes through `apply_consumer_reg_ack_shm_` after REG_ACK
//      delivers an SHM fd via `SCM_RIGHTS` — there is no `shm_open(name)`
//      to fail on a nonexistent name.  The legacy branch in
//      `build_rx_queue` (gated on `shm_shared_secret != 0`) survives
//      only as a fallback for tests that still inject a secret, and
//      that fallback is removed entirely under 1i-cleanup (#275).
//   2. The downstream cleanup-on-fail behavior the original test
//      composed at the role layer is already covered at L2 by
//      `test_hub_shm_queue_capability.cpp`:
//        * Test 6 (`SetCapabilityFd_RefusesNegativeFd`) — invalid-fd guardrail
//        * Test 1 inverse — `start()` failure when DataBlock attach fails
//        * Tests 3-5 — state-machine refusals (Active / mutual exclusion)
//      No new behavior at the role-API boundary needs pinning that
//      isn't covered at L2 already.
//   3. Constructing a synthetic-failure capability-path equivalent
//      (e.g. an implausible-large fd or an undersized memfd) would be
//      either a re-test of L2's negative-fd guardrail (no new coverage)
//      or a DataBlock fd-source-factory test (wrong layer).  Neither
//      meets the bar "mock follows the same logic as production with
//      clear justification" — both manufacture a failure path that
//      production code never enters on the capability transport.
//   4. End-to-end role-layer composition under real failure conditions
//      is covered by the future L4 test (1k / #258).
//
// Future maintainers: do NOT reintroduce a role-level "build_rx_queue
// rejects bogus SHM" test on the capability path without first
// identifying a role-layer behavior that L2 + L4 do not cover.

// ----------------------------------------------------------------------------
// shm_slot_checksum_corrupt_detected
// ----------------------------------------------------------------------------
//
// Producer writes flexzone with enforced slot checksum, commits slot.
// Producer overwrites the LAST byte of the just-committed slot after
// write_commit() — the stored checksum in the SHM header is now stale.
// Consumer (attached with Enforced checksum_policy via rx_opts) must
// detect the mismatch: either read_acquire returns nullptr, or the
// slot-checksum error counter increments.  Without this test, a
// checksum-verify bug (e.g. verify_checksum_slot being a no-op) would
// not be caught by any other test — the positive-path
// `shm_checksum_roundtrip` passes regardless.
//
// Symmetric twin: `shm_flexzone_checksum_corrupt_detected` covers the
// flexzone-verify path.

int shm_slot_checksum_corrupt_detected()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::padding_schema(); // 16B aligned
            auto fz_spec = pylabhub::tests::simple_schema();

            const std::string channel = make_test_channel_name("fz_csum_corrupt");

            const size_t fz_logical = hub::compute_schema_size(fz_spec, fz_spec.packing);
            const size_t fz_physical = hub::align_to_physical_page(fz_logical);

            // HEP-CORE-0041 1i-cleanup #275-S1: capability path.  Mint
            // the SHM transport BEFORE prod_core so destruction order
            // (reverse) tears down queues first, then the memfd last.
            namespace sec = pylabhub::utils::security;
            hub::TxQueueOptions tx_opts = make_producer_opts(slot_spec, fz_spec);
            tx_opts.shm_config.checksum_policy = hub::ChecksumPolicy::Enforced;
            const size_t total = shm_segment_total_size(
                slot_spec, fz_spec, tx_opts.shm_config.ring_buffer_capacity,
                tx_opts.shm_config.physical_page_size, tx_opts.shm_config.policy,
                tx_opts.shm_config.consumer_sync_policy, hub::ChecksumPolicy::Enforced);
            ASSERT_GT(total, 0u);
            auto shm_transport = sec::create_shm_capability_producer(total);
            ASSERT_NE(shm_transport, nullptr);
            tx_opts.shm_capability_fd = shm_transport->borrow_fd();

            // Producer: enforced slot checksum.
            RoleHostCore prod_core;
            auto prod = std::make_unique<RoleAPIBase>(prod_core, "prod", "prod.fz-csum.bad");
            prod->set_channel(channel);
            prod->set_name("fz-csum-bad-prod");
            {
                RoleAPIBase::FlexzoneInfoCache fz_info;
                fz_info.has_tx_fz = fz_spec.has_schema;
                fz_info.tx_logical_size = fz_logical;
                fz_info.tx_physical_size = fz_physical;
                prod->set_flexzone_info_cache_(fz_info);
            }

            ASSERT_TRUE(prod->build_tx_queue(tx_opts));
            // HEP-CORE-0036 §5b B-5 (#290): vestigial.  See
            // `build_payload_pair` comment — SHM tx queue is fully
            // wired by `build_tx_queue`; `apply_master_approval` is a
            // no-op for SHM without `shm_secret`, and this fixture
            // does not exercise FSM or cache state that the call would
            // affect.

            // Write + commit a slot with a known pattern.
            auto *slot =
                static_cast<uint8_t *>(prod->write_acquire(std::chrono::milliseconds{500}));
            ASSERT_NE(slot, nullptr);
            std::memset(slot, 0xAA, prod->write_item_size());
            prod->write_commit();
            // write_commit computed the BLAKE2b slot checksum and
            // stored it in the SHM header alongside the slot payload.

            // Simulate on-wire corruption: flip a byte in the committed
            // slot so the stored checksum no longer matches the payload.
            // (Mutating after commit is normally a contract violation;
            // we do it here because it's the only way to exercise the
            // verify path without a bit-flipping fault injector.)
            const size_t item_size = prod->write_item_size();
            ASSERT_GT(item_size, 0u);
            slot[item_size - 1] = 0x55;

            // Consumer attaches with enforced verify.
            RoleHostCore cons_core;
            auto cons = std::make_unique<RoleAPIBase>(cons_core, "cons", "cons.fz-csum.bad");
            cons->set_channel(channel);
            cons->set_name("fz-csum-bad-cons");
            {
                RoleAPIBase::FlexzoneInfoCache fz_info;
                fz_info.has_rx_fz = fz_spec.has_schema;
                fz_info.rx_logical_size = fz_logical;
                fz_info.rx_physical_size = fz_physical;
                cons->set_flexzone_info_cache_(fz_info);
            }

            // In-process ::dup substitute for the production §5.5
            // SCM_RIGHTS handoff (the dup'd fd is consumed by the
            // queue's fd-source factory which dups internally — we
            // close ours right after build_rx_queue returns).
            hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec);
            rx_opts.checksum_policy = hub::ChecksumPolicy::Enforced;
            const int rx_fd_dup = ::dup(shm_transport->borrow_fd());
            ASSERT_GE(rx_fd_dup, 0) << "::dup(memfd) failed: errno=" << errno;
            rx_opts.shm_capability_fd = rx_fd_dup;
            // Enforced policy (set via rx_opts.checksum_policy) already
            // flips verify_slot on the reader at build time.  No
            // additional set_verify_checksum call — policy is tested by
            // its config, not by redundant API calls.
            const bool rx_built = cons->build_rx_queue(rx_opts);
            ::close(rx_fd_dup);
            ASSERT_TRUE(rx_built);
            // HEP-CORE-0036 §5b B-5 (#290): vestigial.  See
            // `build_payload_pair` comment — the SHM rx queue is fully
            // wired by `build_rx_queue` (consumes the dup'd capability
            // fd), and `apply_master_approval` is a no-op for SHM
            // without `shm_secret`.  This fixture exercises read-side
            // verification semantics, not the broker ACK delivery path.

            const uint64_t errors_before =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // read_acquire returns nullptr on verify failure (enforced).
            const void *rs = cons->read_acquire(std::chrono::milliseconds{200});
            const uint64_t errors_after = cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // Observable outcome: either the read returns null (slot
            // rejected by verify) OR the error counter increments.
            // Both are legitimate framework-defined failure signals;
            // the test requires at least one to fire.
            const bool detected = (rs == nullptr) || (errors_after > errors_before);
            EXPECT_TRUE(detected) << "corrupted slot must be rejected: read_acquire="
                                  << (rs ? "non-null" : "null") << ", checksum_error_count delta="
                                  << (errors_after - errors_before);

            if (rs != nullptr)
                cons->read_release();
            cons->close_queues();
            prod->close_queues();
        },
        "role_api_flexzone::shm_slot_checksum_corrupt_detected", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

// ----------------------------------------------------------------------------
// shm_flexzone_checksum_corrupt_detected
// ----------------------------------------------------------------------------
//
// Symmetric negative twin for the flexzone-checksum path.  Without this,
// a regression that made `update_flexzone_checksum()` or the matching
// `verify_checksum_flexible_zone()` verify path go no-op would pass the
// positive `shm_checksum_roundtrip` and ship.
//
// Producer publishes a slot, writes the flexzone + calls
// update_flexzone_checksum (storing BLAKE2b over the region), then
// mutates the flexzone buffer.  Consumer with flexzone_checksum enabled
// + Enforced policy must detect the mismatch: read_acquire returns
// nullptr OR checksum_error_count (unified counter for slot+fz verify)
// increments.  Log substring "flexzone checksum error" proves the
// specific flexzone-verify path fired, not the slot path.

int shm_flexzone_checksum_corrupt_detected()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec = pylabhub::tests::simple_schema();

            const std::string channel = make_test_channel_name("fz_fz_csum_corrupt");

            const size_t fz_logical = hub::compute_schema_size(fz_spec, fz_spec.packing);
            const size_t fz_physical = hub::align_to_physical_page(fz_logical);

            // HEP-CORE-0041 1i-cleanup #275-S1: capability path.  Mint
            // the SHM transport BEFORE prod_core so destruction order
            // (reverse) tears down queues first, then the memfd last.
            namespace sec = pylabhub::utils::security;
            // Enforced slot checksum + flexzone checksum enabled.  Slot
            // checksum is incidental here — the flexzone counter is the
            // one we're going to drive.
            hub::TxQueueOptions tx_opts = make_producer_opts(slot_spec, fz_spec);
            tx_opts.shm_config.checksum_policy = hub::ChecksumPolicy::Enforced;
            tx_opts.flexzone_checksum = true;
            const size_t total = shm_segment_total_size(
                slot_spec, fz_spec, tx_opts.shm_config.ring_buffer_capacity,
                tx_opts.shm_config.physical_page_size, tx_opts.shm_config.policy,
                tx_opts.shm_config.consumer_sync_policy, hub::ChecksumPolicy::Enforced);
            ASSERT_GT(total, 0u);
            auto shm_transport = sec::create_shm_capability_producer(total);
            ASSERT_NE(shm_transport, nullptr);
            tx_opts.shm_capability_fd = shm_transport->borrow_fd();

            RoleHostCore prod_core;
            auto prod = std::make_unique<RoleAPIBase>(prod_core, "prod", "prod.fz-fz-csum.bad");
            prod->set_channel(channel);
            prod->set_name("fz-fz-csum-bad-prod");
            {
                RoleAPIBase::FlexzoneInfoCache fz_info;
                fz_info.has_tx_fz = fz_spec.has_schema;
                fz_info.tx_logical_size = fz_logical;
                fz_info.tx_physical_size = fz_physical;
                prod->set_flexzone_info_cache_(fz_info);
            }

            ASSERT_TRUE(prod->build_tx_queue(tx_opts));
            // HEP-CORE-0036 §5b B-5 (#290): vestigial.  See
            // `build_payload_pair` comment — SHM tx queue is fully
            // wired by `build_tx_queue`; `apply_master_approval` is a
            // no-op for SHM without `shm_secret`, and this fixture
            // does not exercise FSM or cache state that the call would
            // affect.

            // Write a slot with a pattern the consumer's slot-verify accepts.
            auto *slot =
                static_cast<uint8_t *>(prod->write_acquire(std::chrono::milliseconds{500}));
            ASSERT_NE(slot, nullptr);
            std::memset(slot, 0x11, prod->write_item_size());
            prod->write_commit();

            // Write flexzone, then update its checksum (stored in header).
            auto *fz = static_cast<uint8_t *>(prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(fz, nullptr);
            std::memset(fz, 0xA5, fz_physical);
            ASSERT_TRUE(prod->update_flexzone_checksum())
                << "update_flexzone_checksum must succeed";

            // CORRUPT the flexzone buffer AFTER the checksum was stored.
            // Stored BLAKE2b no longer matches the region.  Symmetric to
            // the slot-corruption pattern in shm_slot_checksum_corrupt_detected:
            // a deliberate contract violation used to exercise the verify
            // path that positive tests cannot reach.
            fz[0] ^= 0xFF;

            // Consumer: enforced slot verify + flexzone verify.
            RoleHostCore cons_core;
            auto cons = std::make_unique<RoleAPIBase>(cons_core, "cons", "cons.fz-fz-csum.bad");
            cons->set_channel(channel);
            cons->set_name("fz-fz-csum-bad-cons");
            {
                RoleAPIBase::FlexzoneInfoCache fz_info;
                fz_info.has_rx_fz = fz_spec.has_schema;
                fz_info.rx_logical_size = fz_logical;
                fz_info.rx_physical_size = fz_physical;
                cons->set_flexzone_info_cache_(fz_info);
            }

            // In-process ::dup substitute for the production §5.5
            // SCM_RIGHTS handoff (the queue's fd-source factory dups
            // internally — we close ours right after build_rx_queue).
            hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec);
            rx_opts.checksum_policy = hub::ChecksumPolicy::Enforced;
            rx_opts.flexzone_checksum = true;
            const int rx_fd_dup = ::dup(shm_transport->borrow_fd());
            ASSERT_GE(rx_fd_dup, 0) << "::dup(memfd) failed: errno=" << errno;
            rx_opts.shm_capability_fd = rx_fd_dup;
            const bool rx_built = cons->build_rx_queue(rx_opts);
            ::close(rx_fd_dup);
            ASSERT_TRUE(rx_built);
            // HEP-CORE-0036 §5b B-5 (#290): vestigial.  See
            // `build_payload_pair` comment — the SHM rx queue is fully
            // wired by `build_rx_queue` (consumes the dup'd capability
            // fd), and `apply_master_approval` is a no-op for SHM
            // without `shm_secret`.  This fixture exercises read-side
            // verification semantics, not the broker ACK delivery path.

            const uint64_t errors_before =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            const void *rs = cons->read_acquire(std::chrono::milliseconds{200});
            const uint64_t errors_after = cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // Either path is framework-legal: null return on enforced-fail
            // OR counter increment (the ShmQueue verify path bumps
            // checksum_error_count before returning nullptr).
            const bool detected = (rs == nullptr) || (errors_after > errors_before);
            EXPECT_TRUE(detected) << "corrupted flexzone must be rejected: read_acquire="
                                  << (rs ? "non-null" : "null") << ", checksum_error_count delta="
                                  << (errors_after - errors_before);

            if (rs != nullptr)
                cons->read_release();
            cons->close_queues();
            prod->close_queues();
        },
        "role_api_flexzone::shm_flexzone_checksum_corrupt_detected", logger_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::role_api_flexzone

// ============================================================================
// Worker dispatch registrar
// ============================================================================

namespace
{

struct RoleApiFlexzoneWorkerRegistrar
{
    RoleApiFlexzoneWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "role_api_flexzone")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_api_flexzone;

                if (sc == "shm_roundtrip")
                    return shm_roundtrip();
                if (sc == "zmq_tx_null")
                    return zmq_tx_null();
                if (sc == "zmq_rx_null")
                    return zmq_rx_null();
                if (sc == "shm_checksum_roundtrip")
                    return shm_checksum_roundtrip();
                if (sc == "shm_roundtrip_padding_sensitive")
                    return shm_roundtrip_padding_sensitive();
                if (sc == "shm_roundtrip_all_types")
                    return shm_roundtrip_all_types();
                if (sc == "shm_roundtrip_array_field")
                    return shm_roundtrip_array_field();
                // 2026-06-23 (#273): shm_consumer_wrong_secret_rejected
                // and shm_consumer_nonexistent_rejected retired (see
                // doc-blocks above + docs/README/README_testing.md §1.2
                // rule 6).  The legacy SHM secret-based auth surface
                // they pinned is gone under HEP-CORE-0041.
                if (sc == "shm_slot_checksum_corrupt_detected")
                    return shm_slot_checksum_corrupt_detected();
                if (sc == "shm_flexzone_checksum_corrupt_detected")
                    return shm_flexzone_checksum_corrupt_detected();

                fmt::print(stderr, "[role_api_flexzone] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};

static RoleApiFlexzoneWorkerRegistrar g_role_api_flexzone_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
