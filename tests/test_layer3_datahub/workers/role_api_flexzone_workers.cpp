/**
 * @file role_api_flexzone_workers.cpp
 * @brief Worker bodies for L3 role-API flexzone integration tests
 *        (Pattern 3).
 *
 * Why a worker subprocess: `RoleAPIBase` constructs a `ThreadManager`
 * that registers a dynamic lifecycle module.  Without a `LifecycleGuard`
 * in scope, registration half-completes and teardown flakes.
 * `run_gtest_worker` owns the guard for the subprocess's lifetime.
 */
#include "role_api_flexzone_workers.h"

#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include "utils/crypto_utils.hpp"
#include "utils/logger.hpp"
#include "utils/lifecycle.hpp"
#include "utils/data_block.hpp"
#include "utils/schema_utils.hpp"
#include "utils/zmq_context.hpp"

#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

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
/// begin/end milestones; CryptoUtils + ZMQContext + DataBlock for the
/// RoleAPIBase + SHM queue construction.
static auto logger_module() { return Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto zmq_module()    { return ::pylabhub::hub::GetZMQContextModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetDataBlockModule(); }

hub::TxQueueOptions make_producer_opts(const hub::SchemaSpec &slot_spec,
                                        const hub::SchemaSpec &fz_spec,
                                        uint64_t secret)
{
    hub::TxQueueOptions opts;
    opts.has_shm = true;
    opts.shm_config.shared_secret         = secret;
    opts.shm_config.ring_buffer_capacity  = 4;
    opts.shm_config.physical_page_size    = hub::system_page_size();
    opts.shm_config.policy                = hub::DataBlockPolicy::RingBuffer;
    opts.shm_config.consumer_sync_policy  = hub::ConsumerSyncPolicy::Sequential;
    opts.shm_config.checksum_policy       = hub::ChecksumPolicy::None;
    opts.slot_spec = slot_spec;
    opts.fz_spec   = fz_spec;
    return opts;
}

hub::RxQueueOptions make_consumer_opts(const std::string &shm_channel,
                                        const hub::SchemaSpec &slot_spec,
                                        uint64_t secret)
{
    hub::RxQueueOptions opts;
    opts.shm_name          = shm_channel;
    opts.shm_shared_secret = secret;
    opts.slot_spec         = slot_spec;
    return opts;
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
struct PayloadPair
{
    RoleHostCore                 prod_core;
    RoleHostCore                 cons_core;
    std::unique_ptr<RoleAPIBase> prod;
    std::unique_ptr<RoleAPIBase> cons;
    size_t                       fz_size{0};

    PayloadPair()                               = default;
    PayloadPair(const PayloadPair &)            = delete;
    PayloadPair(PayloadPair &&)                 = delete;
    PayloadPair &operator=(const PayloadPair &) = delete;
    PayloadPair &operator=(PayloadPair &&)      = delete;
};

/// Optional knobs for `build_payload_pair`.  Defaults match the flexzone
/// round-trip tests: no slot checksum, no flexzone checksum.
struct PayloadPairOpts
{
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::None};
    bool                flexzone_checksum{false};
};

/// IMPORTANT: `build_payload_pair` uses gtest `ASSERT_*` macros from a
/// non-test function.  This is only safe under `throw_on_failure=true`,
/// which `run_gtest_worker` sets before invoking the test lambda that
/// calls this helper.  Do NOT call `build_payload_pair` directly from a
/// plain `TEST_F` body or any other context that lacks
/// `throw_on_failure` — an ASSERT failure would silently `return;` from
/// this function and leave the caller with a partially-built pair.
void build_payload_pair(PayloadPair &out,
                         const hub::SchemaSpec &slot_spec,
                         const hub::SchemaSpec &fz_spec,
                         const std::string &channel,
                         uint64_t secret,
                         const char *uid_tag,
                         const PayloadPairOpts &opts = {})
{
    out.fz_size = hub::align_to_physical_page(
        hub::compute_schema_size(fz_spec, "aligned"));

    hub::TxQueueOptions tx_opts = make_producer_opts(slot_spec, fz_spec, secret);
    tx_opts.shm_config.checksum_policy = opts.checksum_policy;
    tx_opts.flexzone_checksum = opts.flexzone_checksum;

    out.prod_core.set_out_fz_spec(hub::SchemaSpec{fz_spec}, out.fz_size);
    out.prod = std::make_unique<RoleAPIBase>(
        out.prod_core, "prod", std::string("prod.fz.") + uid_tag);
    out.prod->set_channel(channel);
    out.prod->set_name("fz-prod");
    ASSERT_TRUE(out.prod->build_tx_queue(tx_opts));
    ASSERT_TRUE(out.prod->start_tx_queue());

    // One committed slot so the consumer can attach.
    void *slot = out.prod->write_acquire(std::chrono::milliseconds{500});
    ASSERT_NE(slot, nullptr);
    out.prod->write_commit();

    hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec, secret);
    rx_opts.checksum_policy   = opts.checksum_policy;
    rx_opts.flexzone_checksum = opts.flexzone_checksum;

    out.cons_core.set_in_fz_spec(hub::SchemaSpec{fz_spec}, out.fz_size);
    out.cons = std::make_unique<RoleAPIBase>(
        out.cons_core, "cons", std::string("cons.fz.") + uid_tag);
    out.cons->set_channel(channel);
    out.cons->set_name("fz-cons");
    ASSERT_TRUE(out.cons->build_rx_queue(rx_opts));
    ASSERT_TRUE(out.cons->start_rx_queue());
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
            auto fz_spec   = pylabhub::tests::simple_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec,
                make_test_channel_name("fz_roundtrip"),
                0xDEAD'BEEF'CAFE'1234ULL, "TEST");

            // Flexzone contract: non-null + size == computed aligned size
            // + has_shm true — on both Tx and Rx.
            void *tx_fz = pair.prod->flexzone(ChannelSide::Tx);
            ASSERT_NE(tx_fz, nullptr)
                << "Producer Tx flexzone must be non-null for SHM queue";
            EXPECT_EQ(pair.prod->flexzone_size(ChannelSide::Tx), pair.fz_size);
            EXPECT_TRUE(pair.prod->tx_has_shm());

            void *rx_fz = pair.cons->flexzone(ChannelSide::Rx);
            ASSERT_NE(rx_fz, nullptr)
                << "Consumer Rx flexzone must be non-null for SHM queue";
            EXPECT_EQ(pair.cons->flexzone_size(ChannelSide::Rx), pair.fz_size)
                << "Rx flexzone size must match Tx";
            EXPECT_TRUE(pair.cons->rx_has_shm());

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
            EXPECT_FLOAT_EQ(prod_read, ack)
                << "Producer Tx flexzone must see consumer's ack";

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip",
        logger_module(), crypto_module(), zmq_module(), hub_module());
}

// ============================================================================
// zmq_tx_null — ZMQ-only Tx has no flexzone
// ============================================================================

int zmq_tx_null()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = std::make_unique<RoleAPIBase>(
                core, "prod", "prod.zmq-fz.tx");
            api->set_channel("test.fz.zmq.tx");

            hub::TxQueueOptions opts;
            opts.has_shm           = false;
            opts.data_transport    = "zmq";
            opts.zmq_node_endpoint = "tcp://127.0.0.1:0";
            opts.zmq_bind          = true;
            opts.slot_spec         = pylabhub::tests::simple_schema();

            ASSERT_TRUE(api->build_tx_queue(opts));
            ASSERT_TRUE(api->start_tx_queue());

            EXPECT_EQ(api->flexzone(ChannelSide::Tx), nullptr);
            EXPECT_EQ(api->flexzone_size(ChannelSide::Tx), 0u);
            EXPECT_FALSE(api->tx_has_shm());

            api->close_queues();
        },
        "role_api_flexzone::zmq_tx_null",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            // itself; no producer peer required.  Rx-build succeeds
            // even against an unreachable endpoint because the ZMQ
            // connect is non-blocking.
            RoleHostCore core;
            auto api = std::make_unique<RoleAPIBase>(
                core, "cons", "cons.zmq-fz.rx");
            api->set_channel("test.fz.zmq.rx");

            hub::RxQueueOptions rx_opts;
            rx_opts.data_transport    = "zmq";
            rx_opts.zmq_node_endpoint = "tcp://127.0.0.1:45599";
            rx_opts.slot_spec         = pylabhub::tests::simple_schema();

            ASSERT_TRUE(api->build_rx_queue(rx_opts));
            ASSERT_TRUE(api->start_rx_queue());

            EXPECT_EQ(api->flexzone(ChannelSide::Rx), nullptr);
            EXPECT_EQ(api->flexzone_size(ChannelSide::Rx), 0u);
            EXPECT_FALSE(api->rx_has_shm());

            api->close_queues();
        },
        "role_api_flexzone::zmq_rx_null",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            auto fz_spec   = pylabhub::tests::simple_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec,
                make_test_channel_name("fz_checksum"),
                0xBABE'FACE'FEED'F00DULL, "CSUM",
                {hub::ChecksumPolicy::Enforced, /*flexzone_checksum=*/true});

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
            const void *rs = pair.cons->read_acquire(
                std::chrono::milliseconds{500});
            ASSERT_NE(rs, nullptr)
                << "read_acquire must succeed when checksum is valid";
            pair.cons->read_release();

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_checksum_roundtrip",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            auto fz_spec   = pylabhub::tests::padding_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec,
                                make_test_channel_name("fz_padding"),
                                0xCAFE'F00D'FACE'0001ULL, "PAD");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 3u) << "padding_schema has 3 fields";
            // Pin the aligned offsets + total size — any shift or
            // mis-pad would be caught here even without the downstream
            // read-back assertions.  padding_schema total = 16 aligned
            // (13 packed) per test_schema_helpers.h.
            EXPECT_EQ(layout[0].offset, 0u);   // ts: float64 @ 0
            EXPECT_EQ(layout[1].offset, 8u);   // flag: uint8 @ 8
            EXPECT_EQ(layout[2].offset, 12u);  // count: int32 @ 12 (3 byte pad)
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 16u);
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "packed"),  13u);

            // Producer: write distinctive values at computed offsets.
            auto *tx = static_cast<uint8_t*>(pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            const double  ts     = 3.14159265358979;
            const uint8_t flag   = 0xAB;
            const int32_t count  = -42;

            std::memcpy(tx + layout[0].offset, &ts,    sizeof(ts));
            std::memcpy(tx + layout[1].offset, &flag,  sizeof(flag));
            std::memcpy(tx + layout[2].offset, &count, sizeof(count));

            // Consumer: read each field at the SAME offsets, verify bit-exact.
            auto *rx = static_cast<const uint8_t*>(
                pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            double  read_ts = 0.0;
            uint8_t read_flag = 0;
            int32_t read_count = 0;
            std::memcpy(&read_ts,    rx + layout[0].offset, sizeof(read_ts));
            std::memcpy(&read_flag,  rx + layout[1].offset, sizeof(read_flag));
            std::memcpy(&read_count, rx + layout[2].offset, sizeof(read_count));

            EXPECT_DOUBLE_EQ(read_ts,   ts);
            EXPECT_EQ(read_flag,  flag);
            EXPECT_EQ(read_count, count);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_padding_sensitive",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            auto fz_spec   = pylabhub::tests::all_types_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec,
                                make_test_channel_name("fz_alltypes"),
                                0xCAFE'F00D'FACE'0002ULL, "ALL");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 13u);
            // Pin the aligned total size — the comment in
            // test_schema_helpers.h documents the expected value: 55
            // bytes of field data rounded up to 56 (multiple of 8) for
            // the 8-byte alignment of the first field.  A bug that
            // dropped trailing padding or miscounted a field would
            // shift this number and be caught here.
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 56u);

            auto *tx = static_cast<uint8_t*>(
                pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            // Distinctive sentinels — value chosen so that misreading at a
            // neighbour's offset would show up as a different (visible) number.
            const double   v_f64    = -1.7976931348623157e+100;  // near-min double
            const int64_t  v_i64    = static_cast<int64_t>(0x0123456789ABCDEFLL);
            const uint64_t v_u64    = 0xFEDCBA9876543210ULL;
            const float    v_f32    = 1.5e10F;
            const int32_t  v_i32    = -0x12345678;
            const uint32_t v_u32    = 0xDEADBEEFu;
            const int16_t  v_i16    = -0x1234;
            const uint16_t v_u16    = 0xBEEF;
            const bool     v_bool   = true;
            const int8_t   v_i8     = -42;
            const uint8_t  v_u8     = 0x5A;
            const uint8_t  v_bytes[4] = {0x11, 0x22, 0x33, 0x44};
            const char     v_str[8]   = {'h','e','l','l','o','\0','P','Q'};

            std::memcpy(tx + layout[0].offset,  &v_f64,  sizeof(v_f64));
            std::memcpy(tx + layout[1].offset,  &v_i64,  sizeof(v_i64));
            std::memcpy(tx + layout[2].offset,  &v_u64,  sizeof(v_u64));
            std::memcpy(tx + layout[3].offset,  &v_f32,  sizeof(v_f32));
            std::memcpy(tx + layout[4].offset,  &v_i32,  sizeof(v_i32));
            std::memcpy(tx + layout[5].offset,  &v_u32,  sizeof(v_u32));
            std::memcpy(tx + layout[6].offset,  &v_i16,  sizeof(v_i16));
            std::memcpy(tx + layout[7].offset,  &v_u16,  sizeof(v_u16));
            std::memcpy(tx + layout[8].offset,  &v_bool, sizeof(v_bool));
            std::memcpy(tx + layout[9].offset,  &v_i8,   sizeof(v_i8));
            std::memcpy(tx + layout[10].offset, &v_u8,   sizeof(v_u8));
            std::memcpy(tx + layout[11].offset, v_bytes, sizeof(v_bytes));
            std::memcpy(tx + layout[12].offset, v_str,   sizeof(v_str));

            auto *rx = static_cast<const uint8_t*>(
                pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            double   r_f64 = 0;   std::memcpy(&r_f64, rx + layout[0].offset,  sizeof(r_f64));
            int64_t  r_i64 = 0;   std::memcpy(&r_i64, rx + layout[1].offset,  sizeof(r_i64));
            uint64_t r_u64 = 0;   std::memcpy(&r_u64, rx + layout[2].offset,  sizeof(r_u64));
            float    r_f32 = 0;   std::memcpy(&r_f32, rx + layout[3].offset,  sizeof(r_f32));
            int32_t  r_i32 = 0;   std::memcpy(&r_i32, rx + layout[4].offset,  sizeof(r_i32));
            uint32_t r_u32 = 0;   std::memcpy(&r_u32, rx + layout[5].offset,  sizeof(r_u32));
            int16_t  r_i16 = 0;   std::memcpy(&r_i16, rx + layout[6].offset,  sizeof(r_i16));
            uint16_t r_u16 = 0;   std::memcpy(&r_u16, rx + layout[7].offset,  sizeof(r_u16));
            bool     r_bool= false;std::memcpy(&r_bool,rx + layout[8].offset, sizeof(r_bool));
            int8_t   r_i8  = 0;   std::memcpy(&r_i8,  rx + layout[9].offset,  sizeof(r_i8));
            uint8_t  r_u8  = 0;   std::memcpy(&r_u8,  rx + layout[10].offset, sizeof(r_u8));
            uint8_t  r_bytes[4] = {0,0,0,0};
            std::memcpy(r_bytes, rx + layout[11].offset, sizeof(r_bytes));
            char     r_str[8]   = {0,0,0,0,0,0,0,0};
            std::memcpy(r_str,   rx + layout[12].offset, sizeof(r_str));

            EXPECT_DOUBLE_EQ(r_f64, v_f64);
            EXPECT_EQ(r_i64,  v_i64);
            EXPECT_EQ(r_u64,  v_u64);
            EXPECT_FLOAT_EQ(r_f32, v_f32);
            EXPECT_EQ(r_i32,  v_i32);
            EXPECT_EQ(r_u32,  v_u32);
            EXPECT_EQ(r_i16,  v_i16);
            EXPECT_EQ(r_u16,  v_u16);
            EXPECT_EQ(r_bool, v_bool);
            EXPECT_EQ(r_i8,   v_i8);
            EXPECT_EQ(r_u8,   v_u8);
            EXPECT_EQ(std::memcmp(r_bytes, v_bytes, 4), 0);
            EXPECT_EQ(std::memcmp(r_str,   v_str,   8), 0);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_all_types",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            auto fz_spec   = pylabhub::tests::fz_array_schema();

            PayloadPair pair;
            build_payload_pair(pair, slot_spec, fz_spec,
                                make_test_channel_name("fz_array"),
                                0xCAFE'F00D'FACE'0003ULL, "ARR");

            const auto layout = layout_for(fz_spec);
            ASSERT_EQ(layout.size(), 2u);
            EXPECT_EQ(layout[0].offset, 0u);   // uint32 @ 0
            EXPECT_EQ(layout[1].offset, 8u);   // float64[2] @ 8 (4 byte pad)
            EXPECT_EQ(layout[1].byte_size, sizeof(double) * 2u);
            // Total aligned = 4 (u32) + 4 (pad) + 16 (f64[2]) = 24.
            EXPECT_EQ(hub::compute_schema_size(fz_spec, "aligned"), 24u);

            auto *tx = static_cast<uint8_t*>(pair.prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(tx, nullptr);

            const uint32_t version = 7;
            const double   values[2] = {1.5, 2.5};
            std::memcpy(tx + layout[0].offset, &version, sizeof(version));
            std::memcpy(tx + layout[1].offset, values,   sizeof(values));

            auto *rx = static_cast<const uint8_t*>(
                pair.cons->flexzone(ChannelSide::Rx));
            ASSERT_NE(rx, nullptr);

            uint32_t r_version = 0;
            double   r_values[2] = {0.0, 0.0};
            std::memcpy(&r_version, rx + layout[0].offset, sizeof(r_version));
            std::memcpy(r_values,   rx + layout[1].offset, sizeof(r_values));

            EXPECT_EQ(r_version, version);
            EXPECT_DOUBLE_EQ(r_values[0], values[0]);
            EXPECT_DOUBLE_EQ(r_values[1], values[1]);

            pair.cons->close_queues();
            pair.prod->close_queues();
        },
        "role_api_flexzone::shm_roundtrip_array_field",
        logger_module(), crypto_module(), zmq_module(), hub_module());
}

// ============================================================================
// NEGATIVE PATHS — prove the error surface actually fires
// ============================================================================

// ----------------------------------------------------------------------------
// shm_consumer_wrong_secret_rejected
// ----------------------------------------------------------------------------
//
// Producer creates SHM with secret S; consumer tries to attach with
// secret S^1.  build_rx_queue must return false, and the library must
// emit a WARN log with "shared_secret mismatch" (symmetrical with the
// WriteAttach path).  Subsequent attach with the correct secret must
// succeed — proves the failure was the secret, not a sticky corruption.

int shm_consumer_wrong_secret_rejected()
{
    return run_gtest_worker(
        [&]()
        {
            auto slot_spec = pylabhub::tests::simple_schema();
            auto fz_spec   = pylabhub::tests::simple_schema();

            const std::string channel = make_test_channel_name("fz_wrong_secret");
            const uint64_t    secret  = 0xA5A5'5A5A'DEAD'BEEFULL;

            const size_t fz_size = hub::align_to_physical_page(
                hub::compute_schema_size(fz_spec, "aligned"));

            RoleHostCore prod_core;
            prod_core.set_out_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto prod = std::make_unique<RoleAPIBase>(
                prod_core, "prod", "prod.fz.wrong");
            prod->set_channel(channel);
            prod->set_name("fz-wrong-prod");
            ASSERT_TRUE(prod->build_tx_queue(
                make_producer_opts(slot_spec, fz_spec, secret)));
            ASSERT_TRUE(prod->start_tx_queue());

            // Commit one slot so the SHM is in a usable state.
            ASSERT_NE(prod->write_acquire(std::chrono::milliseconds{500}),
                      nullptr);
            prod->write_commit();

            // Consumer attempts attach with WRONG secret.  Must fail.
            RoleHostCore cons_core;
            cons_core.set_in_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto cons = std::make_unique<RoleAPIBase>(
                cons_core, "cons", "cons.fz.wrong");
            cons->set_channel(channel);
            cons->set_name("fz-wrong-cons");

            auto bad_opts = make_consumer_opts(channel, slot_spec,
                                                secret ^ 1ULL);
            EXPECT_FALSE(cons->build_rx_queue(bad_opts))
                << "build_rx_queue must reject wrong shared_secret";
            // No half-state: a failed build must not leave a dangling
            // rx_queue internal pointer.  Symmetric with the
            // shm_consumer_nonexistent_rejected post-failure check.
            EXPECT_FALSE(cons->rx_has_shm());
            EXPECT_EQ(cons->flexzone(ChannelSide::Rx), nullptr);

            // Sanity: build_rx_queue with the CORRECT secret still works
            // afterwards — proves the failure was the secret, not a
            // sticky state-corruption that would mask real bugs.
            auto good_opts = make_consumer_opts(channel, slot_spec, secret);
            EXPECT_TRUE(cons->build_rx_queue(good_opts))
                << "build_rx_queue must succeed with correct secret";
            EXPECT_TRUE(cons->start_rx_queue());

            cons->close_queues();
            prod->close_queues();
        },
        "role_api_flexzone::shm_consumer_wrong_secret_rejected",
        logger_module(), crypto_module(), zmq_module(), hub_module());
}

// NOTE: no `shm_consumer_schema_mismatch_rejected` worker at this layer.
// `ShmQueue::create_reader` deliberately does NOT validate schema shape
// against the producer's header — that check lives at the broker /
// channel-registration layer, where the schema hash cross-check runs.
// A meaningful schema-mismatch negative test belongs in the broker
// tier, not in the no-broker L3 role-api integration tests.

// ----------------------------------------------------------------------------
// shm_consumer_nonexistent_rejected
// ----------------------------------------------------------------------------
//
// Consumer tries to attach to an SHM segment that was never created.
// build_rx_queue must return false.  Proves the existence-check path
// in ShmQueue::create_reader is live at the role-API boundary.

int shm_consumer_nonexistent_rejected()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = std::make_unique<RoleAPIBase>(
                core, "cons", "cons.fz.noexist");
            api->set_channel("test.fz.noexist");
            api->set_name("fz-noexist-cons");

            hub::RxQueueOptions opts;
            opts.shm_name          = make_test_channel_name(
                "fz_nonexistent_segment");
            opts.shm_shared_secret = 0xF00D'D00D'BAD0'BAD0ULL;
            opts.slot_spec         = pylabhub::tests::simple_schema();

            EXPECT_FALSE(api->build_rx_queue(opts))
                << "build_rx_queue must reject nonexistent SHM segment";
            // Prove no half-state: queue pointer is null after failed build,
            // so rx_has_shm / flexzone accessors behave as if build never ran.
            EXPECT_FALSE(api->rx_has_shm());
            EXPECT_EQ(api->flexzone(ChannelSide::Rx), nullptr);
        },
        "role_api_flexzone::shm_consumer_nonexistent_rejected",
        // No ZMQ code path reached on a nonexistent-SHM attach failure —
        // drop zmq_module.  Crypto stays: DataBlock declares it as a
        // hard lifecycle dependency (LifecycleGuard aborts without it),
        // even if no signing/HMAC work runs on the failure path.
        logger_module(), crypto_module(), hub_module());
}

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
            auto slot_spec = pylabhub::tests::padding_schema();  // 16B aligned
            auto fz_spec   = pylabhub::tests::simple_schema();

            const std::string channel = make_test_channel_name("fz_csum_corrupt");
            const uint64_t    secret  = 0xBADD'C0DE'DEAD'BEEFULL;

            const size_t fz_size = hub::align_to_physical_page(
                hub::compute_schema_size(fz_spec, "aligned"));

            // Producer: enforced slot checksum.
            RoleHostCore prod_core;
            prod_core.set_out_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto prod = std::make_unique<RoleAPIBase>(
                prod_core, "prod", "prod.fz-csum.bad");
            prod->set_channel(channel);
            prod->set_name("fz-csum-bad-prod");

            hub::TxQueueOptions opts = make_producer_opts(slot_spec, fz_spec,
                                                           secret);
            opts.shm_config.checksum_policy = hub::ChecksumPolicy::Enforced;
            ASSERT_TRUE(prod->build_tx_queue(opts));
            ASSERT_TRUE(prod->start_tx_queue());

            // Write + commit a slot with a known pattern.
            auto *slot = static_cast<uint8_t*>(prod->write_acquire(
                std::chrono::milliseconds{500}));
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
            cons_core.set_in_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto cons = std::make_unique<RoleAPIBase>(
                cons_core, "cons", "cons.fz-csum.bad");
            cons->set_channel(channel);
            cons->set_name("fz-csum-bad-cons");

            hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec,
                                                              secret);
            rx_opts.checksum_policy = hub::ChecksumPolicy::Enforced;
            // Enforced policy (set via rx_opts.checksum_policy) already
            // flips verify_slot on the reader at build time.  No
            // additional set_verify_checksum call — policy is tested by
            // its config, not by redundant API calls.
            ASSERT_TRUE(cons->build_rx_queue(rx_opts));
            ASSERT_TRUE(cons->start_rx_queue());

            const uint64_t errors_before =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // read_acquire returns nullptr on verify failure (enforced).
            const void *rs = cons->read_acquire(
                std::chrono::milliseconds{200});
            const uint64_t errors_after =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // Observable outcome: either the read returns null (slot
            // rejected by verify) OR the error counter increments.
            // Both are legitimate framework-defined failure signals;
            // the test requires at least one to fire.
            const bool detected =
                (rs == nullptr) || (errors_after > errors_before);
            EXPECT_TRUE(detected)
                << "corrupted slot must be rejected: read_acquire="
                << (rs ? "non-null" : "null")
                << ", checksum_error_count delta="
                << (errors_after - errors_before);

            if (rs != nullptr)
                cons->read_release();
            cons->close_queues();
            prod->close_queues();
        },
        "role_api_flexzone::shm_slot_checksum_corrupt_detected",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
            auto fz_spec   = pylabhub::tests::simple_schema();

            const std::string channel = make_test_channel_name("fz_fz_csum_corrupt");
            const uint64_t    secret  = 0xBADD'F00D'FACE'D00DULL;

            const size_t fz_size = hub::align_to_physical_page(
                hub::compute_schema_size(fz_spec, "aligned"));

            RoleHostCore prod_core;
            prod_core.set_out_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto prod = std::make_unique<RoleAPIBase>(
                prod_core, "prod", "prod.fz-fz-csum.bad");
            prod->set_channel(channel);
            prod->set_name("fz-fz-csum-bad-prod");

            // Enforced slot checksum + flexzone checksum enabled.  Slot
            // checksum is incidental here — the flexzone counter is the
            // one we're going to drive.
            hub::TxQueueOptions opts = make_producer_opts(slot_spec, fz_spec,
                                                           secret);
            opts.shm_config.checksum_policy = hub::ChecksumPolicy::Enforced;
            opts.flexzone_checksum          = true;
            ASSERT_TRUE(prod->build_tx_queue(opts));
            ASSERT_TRUE(prod->start_tx_queue());

            // Write a slot with a pattern the consumer's slot-verify accepts.
            auto *slot = static_cast<uint8_t*>(prod->write_acquire(
                std::chrono::milliseconds{500}));
            ASSERT_NE(slot, nullptr);
            std::memset(slot, 0x11, prod->write_item_size());
            prod->write_commit();

            // Write flexzone, then update its checksum (stored in header).
            auto *fz = static_cast<uint8_t*>(prod->flexzone(ChannelSide::Tx));
            ASSERT_NE(fz, nullptr);
            std::memset(fz, 0xA5, fz_size);
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
            cons_core.set_in_fz_spec(hub::SchemaSpec{fz_spec}, fz_size);
            auto cons = std::make_unique<RoleAPIBase>(
                cons_core, "cons", "cons.fz-fz-csum.bad");
            cons->set_channel(channel);
            cons->set_name("fz-fz-csum-bad-cons");

            hub::RxQueueOptions rx_opts = make_consumer_opts(channel, slot_spec,
                                                              secret);
            rx_opts.checksum_policy   = hub::ChecksumPolicy::Enforced;
            rx_opts.flexzone_checksum = true;
            ASSERT_TRUE(cons->build_rx_queue(rx_opts));
            ASSERT_TRUE(cons->start_rx_queue());

            const uint64_t errors_before =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            const void *rs = cons->read_acquire(
                std::chrono::milliseconds{200});
            const uint64_t errors_after =
                cons->queue_metrics(ChannelSide::Rx).checksum_error_count;

            // Either path is framework-legal: null return on enforced-fail
            // OR counter increment (the ShmQueue verify path bumps
            // checksum_error_count before returning nullptr).
            const bool detected =
                (rs == nullptr) || (errors_after > errors_before);
            EXPECT_TRUE(detected)
                << "corrupted flexzone must be rejected: read_acquire="
                << (rs ? "non-null" : "null")
                << ", checksum_error_count delta="
                << (errors_after - errors_before);

            if (rs != nullptr)
                cons->read_release();
            cons->close_queues();
            prod->close_queues();
        },
        "role_api_flexzone::shm_flexzone_checksum_corrupt_detected",
        logger_module(), crypto_module(), zmq_module(), hub_module());
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
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_api_flexzone")
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
                if (sc == "shm_consumer_wrong_secret_rejected")
                    return shm_consumer_wrong_secret_rejected();
                if (sc == "shm_consumer_nonexistent_rejected")
                    return shm_consumer_nonexistent_rejected();
                if (sc == "shm_slot_checksum_corrupt_detected")
                    return shm_slot_checksum_corrupt_detected();
                if (sc == "shm_flexzone_checksum_corrupt_detected")
                    return shm_flexzone_checksum_corrupt_detected();

                fmt::print(stderr,
                           "[role_api_flexzone] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};

static RoleApiFlexzoneWorkerRegistrar g_role_api_flexzone_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
