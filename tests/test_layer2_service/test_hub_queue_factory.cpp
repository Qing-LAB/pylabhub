/**
 * @file test_hub_queue_factory.cpp
 * @brief L2 tests for `hub::Queue` — the unified transport-agnostic
 *        factory added in Phase C step 4 (HEP-CORE-0017 §3.3.0).
 *
 * Pins:
 *   - `transport_from_string` parses {"zmq","shm"} and rejects others.
 *   - `reader_is_binding_side` / `writer_is_binding_side` match the
 *     §3.3.0 decision matrix (FanIn → reader-binding / writer-dialing;
 *     FanOut + OneToOne → reader-dialing / writer-binding).
 *   - Gate 1: `(FanIn, Shm)` refused on both `create_reader` and
 *     `create_writer` — nullptr + LOGGER_ERROR.
 *   - Dispatch: legal (topology, transport) cells produce non-null
 *     queues from the correct concrete transport class (verified by
 *     dynamic_cast against `ZmqQueue` / `ShmQueue`).
 *
 * Not tested here: end-to-end data-plane operation.  Those live in
 * `test_hub_zmq_queue.cpp` + `test_hub_shm_queue_capability.cpp` —
 * `hub::Queue` is a pure dispatcher, its behavior is defined by
 * (a) the gate, and (b) which concrete factory it invokes.
 *
 * Pattern 1+ — binary-wide `LifecycleGuard` for `Logger` +
 * `SecureSubsystem` + `DataBlock` + `ZMQContext` (needed because a
 * successful ZMQ construction path builds a socket that binds/connects;
 * we don't `start()` any of them, so the KeyStore lookup fires but the
 * data plane doesn't).
 */
#include "binary_lifecycle.h"
#include "curve_test_setup.h"
#include "utils/hub_queue_factory.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"
#include "utils/schema_types.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

// ── Binary-wide LifecycleGuard (Pattern 1+) ────────────────────────
//
// ZmqQueue construction hits `secure().keys().pubkey(kRoleIdentityName)`
// even without start() — the CURVE identity read is part of factory
// setup.  SecureSubsystem + ZMQContext modules therefore need to be up
// for the ZMQ-cell dispatch tests.
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                             pylabhub::hub::GetZMQContextModule())

class HubQueueFactoryTestEnvironment : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        auto &ks = pylabhub::utils::security::secure().keys();
        if (!ks.has(pylabhub::utils::security::kRoleIdentityName))
        {
            const auto kp = pylabhub::tests::gen_curve_keypair();
            ks.add_identity_from_z85(pylabhub::utils::security::kRoleIdentityName, kp.public_z85,
                                     kp.secret_z85);
        }
    }
};

static auto *kHubQueueFactoryTestEnv =
    ::testing::AddGlobalTestEnvironment(new HubQueueFactoryTestEnvironment);

namespace
{

using pylabhub::hub::ChannelTopology;
using pylabhub::hub::Queue;
using pylabhub::hub::RxOptions;
using pylabhub::hub::SchemaFieldDesc;
using pylabhub::hub::ShmQueue;
using pylabhub::hub::Transport;
using pylabhub::hub::transport_from_string;
using pylabhub::hub::TxOptions;
using pylabhub::hub::ZmqQueue;

std::vector<SchemaFieldDesc> make_slot_schema()
{
    // Minimal schema — one uint32 slot.  Same shape ZmqQueue's own
    // topology-factory tests use so both transports are happy.
    SchemaFieldDesc f;
    f.type_str = "uint32";
    f.count = 1;
    return {f};
}

// ── transport_from_string ──────────────────────────────────────────

TEST(HubQueueFactory, TransportParser_ZmqShm_Ok)
{
    EXPECT_EQ(transport_from_string("zmq"), Transport::Zmq);
    EXPECT_EQ(transport_from_string("shm"), Transport::Shm);
}

TEST(HubQueueFactory, TransportParser_Unknown_Nullopt)
{
    EXPECT_FALSE(transport_from_string("").has_value());
    EXPECT_FALSE(transport_from_string("tcp").has_value());
    EXPECT_FALSE(transport_from_string("ZMQ").has_value()); // case-sensitive
}

// ── Side-legality helpers (constexpr) ──────────────────────────────

TEST(HubQueueFactory, ReaderBindingSide_Matrix)
{
    EXPECT_TRUE(Queue::reader_is_binding_side(ChannelTopology::FanIn));
    EXPECT_FALSE(Queue::reader_is_binding_side(ChannelTopology::FanOut));
    EXPECT_FALSE(Queue::reader_is_binding_side(ChannelTopology::OneToOne));
}

TEST(HubQueueFactory, WriterBindingSide_Matrix)
{
    EXPECT_FALSE(Queue::writer_is_binding_side(ChannelTopology::FanIn));
    EXPECT_TRUE(Queue::writer_is_binding_side(ChannelTopology::FanOut));
    EXPECT_TRUE(Queue::writer_is_binding_side(ChannelTopology::OneToOne));
}

// ── Gate 1: (FanIn, Shm) refused ───────────────────────────────────

TEST(HubQueueFactory, Gate1_FanInShm_ReaderRefused)
{
    RxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "gate1_reader";
    opts.consumer_uid = "consumer-uid";
    auto q = Queue::create_reader(ChannelTopology::FanIn, Transport::Shm, std::move(opts));
    EXPECT_EQ(q, nullptr);
}

TEST(HubQueueFactory, Gate1_FanInShm_WriterRefused)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "gate1_writer";
    opts.producer_uid = "producer-uid";
    auto q = Queue::create_writer(ChannelTopology::FanIn, Transport::Shm, std::move(opts));
    EXPECT_EQ(q, nullptr);
}

// ── Dispatch: SHM legal cells produce ShmQueue ─────────────────────

TEST(HubQueueFactory, Dispatch_OneToOneShm_Reader_IsShmQueue)
{
    RxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "dispatch_1_1_shm_r";
    opts.consumer_uid = "consumer-uid";
    auto q = Queue::create_reader(ChannelTopology::OneToOne, Transport::Shm, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ShmQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_OneToOneShm_Writer_IsShmQueue)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "dispatch_1_1_shm_w";
    opts.producer_uid = "producer-uid";
    opts.ring_buffer_capacity = 4;
    auto q = Queue::create_writer(ChannelTopology::OneToOne, Transport::Shm, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ShmQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_FanOutShm_Reader_IsShmQueue)
{
    RxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "dispatch_fanout_shm_r";
    opts.consumer_uid = "consumer-uid";
    auto q = Queue::create_reader(ChannelTopology::FanOut, Transport::Shm, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ShmQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_FanOutShm_Writer_IsShmQueue)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.channel_name = "dispatch_fanout_shm_w";
    opts.producer_uid = "producer-uid";
    opts.ring_buffer_capacity = 4;
    auto q = Queue::create_writer(ChannelTopology::FanOut, Transport::Shm, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ShmQueue *>(q.get()), nullptr);
}

// ── Dispatch: ZMQ legal cells produce ZmqQueue ─────────────────────
//
// ZMQ construction needs a valid endpoint (empty means Standby) —
// pass a bind hint for binding sides, and for dialing sides we pass
// empty and get a Standby queue (still non-null, still a ZmqQueue).

TEST(HubQueueFactory, Dispatch_FanInZmq_Reader_IsZmqQueue)
{
    RxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.endpoint_hint = "tcp://127.0.0.1:*"; // binding side (fan-in consumer)
    opts.instance_id = "test:fanin-zmq-r:tx";
    auto q = Queue::create_reader(ChannelTopology::FanIn, Transport::Zmq, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ZmqQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_FanInZmq_Writer_IsZmqQueue)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    // Fan-in producer dials — leave endpoint_hint empty (Standby).
    opts.instance_id = "test:fanin-zmq-w:tx";
    auto q = Queue::create_writer(ChannelTopology::FanIn, Transport::Zmq, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ZmqQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_FanOutZmq_Writer_IsZmqQueue)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.endpoint_hint = "tcp://127.0.0.1:*"; // fan-out producer binds
    opts.instance_id = "test:fanout-zmq-w:tx";
    auto q = Queue::create_writer(ChannelTopology::FanOut, Transport::Zmq, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ZmqQueue *>(q.get()), nullptr);
}

TEST(HubQueueFactory, Dispatch_OneToOneZmq_Writer_IsZmqQueue)
{
    TxOptions opts;
    opts.slot_schema = make_slot_schema();
    opts.endpoint_hint = "tcp://127.0.0.1:*"; // one-to-one producer binds
    opts.instance_id = "test:1-1-zmq-w:tx";
    auto q = Queue::create_writer(ChannelTopology::OneToOne, Transport::Zmq, std::move(opts));
    ASSERT_NE(q, nullptr);
    EXPECT_NE(dynamic_cast<ZmqQueue *>(q.get()), nullptr);
}

} // namespace
