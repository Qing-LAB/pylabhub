// tests/test_layer3_datahub/workers/datahub_engine_workers.cpp
//
// L3 engine round-trip workers: engine writes to real SHM slot via
// RoleAPIBase + Producer; consumer reads back and verifies field values.
//
// Uses multifield schema (float64 + uint8 + int32 + float32[3] + bytes[8])
// to verify padding/alignment through the full stack.
#include "datahub_engine_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_schema_helpers.h"

#include "engine_module_params.hpp"
#include "python_engine.hpp"
#include "lua_engine.hpp"
#include "native_engine.hpp"

#include "utils/broker_service.hpp"
#include "utils/role_api_base.hpp"
#include "utils/schema_utils.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::ChannelSide;
using pylabhub::tests::multifield_schema;

namespace fs = std::filesystem;

namespace pylabhub::tests::worker::engine_roundtrip
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Shared helpers
// ============================================================================

namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    ~BrokerHandle()
    {
        if (thread.joinable())
        {
            if (service) service->stop();
            thread.join();
        }
    }

    BrokerHandle() = default;
    BrokerHandle(BrokerHandle &&) = default;
    BrokerHandle &operator=(BrokerHandle &&) = default;
    BrokerHandle(const BrokerHandle &) = delete;
    BrokerHandle &operator=(const BrokerHandle &) = delete;

    void stop_and_join()
    {
        if (service) service->stop();
        if (thread.joinable()) thread.join();
    }
};

BrokerHandle start_broker()
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = true;
    cfg.on_ready  = [promise](const std::string &ep, const std::string &pk) {
        promise->set_value({ep, pk});
    };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    auto *raw    = service.get();
    std::thread t([raw]() { raw->run(); });

    auto info = future.get();
    BrokerHandle h;
    h.service  = std::move(service);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

DataBlockConfig make_shm_config()
{
    DataBlockConfig cfg;
    cfg.physical_page_size    = DataBlockPageSize::Size4K;
    cfg.logical_unit_size     = 4096;
    cfg.ring_buffer_capacity  = 4;
    cfg.policy                = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy  = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret         = 0xDEADBEEFCAFEBABEULL;
    return cfg;
}

constexpr uint64_t kTestShmSecret = 0xDEADBEEFCAFEBABEULL;

// Multifield struct matching the schema — same as native module.
struct MultiFieldSlot
{
    double   ts;
    uint8_t  flag;
    int32_t  count;
    float    values[3];
    uint8_t  tag[8];
};
static_assert(sizeof(MultiFieldSlot) == 40);

} // anonymous namespace

// ============================================================================
// Engine round-trip: common infrastructure + engine-specific invocation
// ============================================================================

// Template: create broker, producer, consumer, engine, produce one slot, verify.
template <typename SetupEngine>
static int engine_roundtrip_impl(const char *tag, SetupEngine &&setup_engine)
{
    return run_gtest_worker(
        [&]() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            auto schema_spec = multifield_schema();
            auto zmq_fields  = schema_spec_to_zmq_fields(schema_spec);
            std::string channel = make_test_channel_name(
                (std::string("engine.") + tag).c_str());

            // Producer with SHM.
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = true;
            popts.zmq_schema   = zmq_fields;
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->start_queue());

            // Wire RoleAPIBase.
            RoleHostCore core;
            core.set_out_slot_spec(hub::SchemaSpec{schema_spec},
                                   hub::compute_schema_size(schema_spec, "aligned"));

            auto api = std::make_unique<RoleAPIBase>(core);
            api->set_role_tag("prod");
            api->set_uid(fmt::format("PROD-{}-L3-001", tag));
            api->set_name("EngineL3");
            api->set_channel(channel);
            api->set_log_level("error");
            api->set_producer(&*producer);

            // Engine-specific setup: init, load script, register types, build_api.
            auto engine = setup_engine(core, *api, schema_spec);

            // Produce: acquire SHM slot, invoke engine, commit.
            void *slot = producer->write_acquire(std::chrono::milliseconds{2000});
            ASSERT_NE(slot, nullptr);
            std::memset(slot, 0, 4096);

            std::vector<IncomingMessage> msgs;
            auto result = engine->invoke_produce(
                InvokeTx{slot, sizeof(MultiFieldSlot), nullptr, 0}, msgs);
            ASSERT_EQ(result, InvokeResult::Commit);
            producer->write_commit();

            // Consumer reads back from SHM.
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = zmq_fields;
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->start_queue());

            const void *data = consumer->read_acquire(
                std::chrono::milliseconds{2000});
            ASSERT_NE(data, nullptr)
                << "Consumer should read the slot written by " << tag << " engine";

            // Verify all fields through SHM.
            auto *s = static_cast<const MultiFieldSlot *>(data);
            EXPECT_DOUBLE_EQ(s->ts, 1.23456789);
            EXPECT_EQ(s->flag, 0xAB);
            EXPECT_EQ(s->count, -42);
            EXPECT_FLOAT_EQ(s->values[0], 1.0f);
            EXPECT_FLOAT_EQ(s->values[1], 2.5f);
            EXPECT_FLOAT_EQ(s->values[2], -3.75f);
            EXPECT_EQ(std::memcmp(s->tag, "DEADBEEF", 8), 0);

            consumer->read_release();

            // Verify spinlock access through RoleAPIBase with real SHM.
            EXPECT_EQ(api->spinlock_count(), 8u);
            auto lock = api->get_spinlock(0);
            ASSERT_TRUE(lock.try_lock_for(0));
            lock.unlock();

            // Verify schema logical size.
            EXPECT_EQ(api->slot_logical_size(), sizeof(MultiFieldSlot));

            // Cleanup.
            engine->finalize();
            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        fmt::format("engine.{}_shm_roundtrip", tag).c_str(),
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Python engine
// ============================================================================

int python_shm_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return engine_roundtrip_impl("python",
        [](RoleHostCore &core, RoleAPIBase &api, const hub::SchemaSpec &spec)
            -> std::unique_ptr<scripting::ScriptEngine>
        {
            // Write temp script.
            auto tmp = fs::temp_directory_path() / "plh_l3_py";
            fs::create_directories(tmp / "script" / "python");
            {
                std::ofstream f(tmp / "script" / "python" / "__init__.py");
                f << "import struct\n"
                     "def on_produce(tx, msgs, api):\n"
                     "    tx.slot.ts = 1.23456789\n"
                     "    tx.slot.flag = 0xAB\n"
                     "    tx.slot.count = -42\n"
                     "    tx.slot.values[0] = 1.0\n"
                     "    tx.slot.values[1] = 2.5\n"
                     "    tx.slot.values[2] = -3.75\n"
                     "    for i, c in enumerate(b'DEADBEEF'):\n"
                     "        tx.slot.tag[i] = c\n"
                     "    return True\n";
            }

            auto engine = std::make_unique<scripting::PythonEngine>();
            engine->set_python_venv("");

            scripting::EngineModuleParams params;
            params.engine            = engine.get();
            params.api               = &api;
            params.tag               = "prod";
            params.script_dir        = tmp / "script" / "python";
            params.entry_point       = "__init__.py";
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_packing       = "aligned";

            scripting::engine_lifecycle_startup(nullptr, &params);
            return engine;
        });
}

// ============================================================================
// Lua engine
// ============================================================================

int lua_shm_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return engine_roundtrip_impl("lua",
        [](RoleHostCore &core, RoleAPIBase &api, const hub::SchemaSpec &spec)
            -> std::unique_ptr<scripting::ScriptEngine>
        {
            auto tmp = fs::temp_directory_path() / "plh_l3_lua";
            fs::create_directories(tmp);
            {
                std::ofstream f(tmp / "init.lua");
                f << "function on_produce(tx, msgs, api)\n"
                     "    tx.slot.ts = 1.23456789\n"
                     "    tx.slot.flag = 0xAB\n"
                     "    tx.slot.count = -42\n"
                     "    tx.slot.values[0] = 1.0\n"
                     "    tx.slot.values[1] = 2.5\n"
                     "    tx.slot.values[2] = -3.75\n"
                     "    local tag = 'DEADBEEF'\n"
                     "    for i = 0, 7 do\n"
                     "        tx.slot.tag[i] = string.byte(tag, i + 1)\n"
                     "    end\n"
                     "    return true\n"
                     "end\n";
            }

            auto engine = std::make_unique<scripting::LuaEngine>();

            scripting::EngineModuleParams params;
            params.engine            = engine.get();
            params.api               = &api;
            params.tag               = "prod";
            params.script_dir        = tmp;
            params.entry_point       = "init.lua";
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_packing       = "aligned";

            scripting::engine_lifecycle_startup(nullptr, &params);
            return engine;
        });
}

// ============================================================================
// Native engine
// ============================================================================

#ifndef TEST_PLUGIN_DIR
#   define TEST_PLUGIN_DIR "."
#endif

int native_shm_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return engine_roundtrip_impl("native",
        [](RoleHostCore &core, RoleAPIBase &api, const hub::SchemaSpec &spec)
            -> std::unique_ptr<scripting::ScriptEngine>
        {
            fs::path dir(TEST_PLUGIN_DIR);
#if defined(_WIN32) || defined(_WIN64)
            fs::path lib = dir / "test_native_multifield_module.dll";
#elif defined(__APPLE__)
            fs::path lib = dir / "libtest_native_multifield_module.dylib";
#else
            fs::path lib = dir / "libtest_native_multifield_module.so";
#endif

            auto engine = std::make_unique<scripting::NativeEngine>();

            scripting::EngineModuleParams params;
            params.engine            = engine.get();
            params.api               = &api;
            params.tag               = "prod";
            params.script_dir        = lib.parent_path();
            params.entry_point       = lib.filename().string();
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_packing       = "aligned";

            scripting::engine_lifecycle_startup(nullptr, &params);
            return engine;
        });
}

} // namespace pylabhub::tests::worker::engine_roundtrip

// ============================================================================
// Worker dispatcher
// ============================================================================

namespace
{

struct EngineWorkerRegistrar
{
    EngineWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "engine")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::engine_roundtrip;
                if (scenario == "python_shm_roundtrip")
                    return python_shm_roundtrip(argc, argv);
                if (scenario == "lua_shm_roundtrip")
                    return lua_shm_roundtrip(argc, argv);
                if (scenario == "native_shm_roundtrip")
                    return native_shm_roundtrip(argc, argv);
                fmt::print(stderr, "ERROR: Unknown engine scenario '{}'\n",
                           scenario);
                return 1;
            });
    }
};

static EngineWorkerRegistrar g_engine_registrar; // NOLINT

} // anonymous namespace
