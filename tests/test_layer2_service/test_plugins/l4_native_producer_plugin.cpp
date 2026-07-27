/**
 * @file l4_native_producer_plugin.cpp
 * @brief L4 native producer — a minimal production-shaped plugin for the
 *        live-broker schema/metrics query smoke.
 *
 * Used by `test_plh_hub_role_zmq_e2e.cpp`
 * (ZmqE2E_NativeProducer_LiveSchemaQueries): loaded by the real
 * `plh_role` binary against a real `plh_hub`, so the three v13 query
 * surfaces (`get_schema_json`, `get_channel_schema_json`,
 * `get_channel_metrics_json`) are exercised against a LIVE broker.
 * The no-broker (NULL-sentinel) half of the contract is pinned at L2
 * by `good_producer_plugin.cpp`; this plugin pins the success half.
 *
 * Unlike `good_producer_plugin` (an API-surface parity harness that
 * probes band/inbox/query calls with fake names on every produce
 * tick), this plugin does only what a production native producer
 * would do: write slots, report a metric, and consult schema/metrics
 * once at startup.  Results are emitted as a single log marker the
 * L4 parent asserts on:
 *
 *   `native_test: live_queries hub_schema=1 channel_schema=1 channel_metrics=1`
 *
 * The hub-global citation `$l4.native.frame.v1` is a fixture contract
 * with the L4 test, which drops the matching JSON file into
 * `<hub_dir>/schemas/` before hub startup (HEP-CORE-0034 §12).
 * Outside that environment (no broker / schema absent) the marker
 * simply reports 0s — only the L4 test asserts the all-1s form.
 */
#include "utils/native_engine_api.h"
#include "pylabhub_version.h" // PYLABHUB_VERSION_* for HEP-0032 axes

#include <cstdio>
#include <cstring>

// Producer out-slot: one float32 field "value" (matches the test config).
PLH_DECLARE_SCHEMA(OutSlotFrame, "value:float32:1:0", 4)

static const PlhNativeContext *g_ctx = nullptr;
static int g_produce_count = 0;
static int g_live_probed = 0;

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1, // little-endian
        PLH_NATIVE_API_VERSION,
        static_cast<uint16_t>(PYLABHUB_VERSION_MAJOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_MINOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_ROLLING),
        PLH_COMPONENT_SHM_MAJOR,
        PLH_COMPONENT_SHM_MINOR,
        PLH_COMPONENT_BROKER_PROTO_MAJOR,
        PLH_COMPONENT_BROKER_PROTO_MINOR,
        PLH_COMPONENT_ZMQ_FRAME_MAJOR,
        PLH_COMPONENT_ZMQ_FRAME_MINOR,
        PLH_COMPONENT_SCRIPT_API_MAJOR,
        PLH_COMPONENT_SCRIPT_API_MINOR,
        PLH_COMPONENT_SCRIPT_ENGINE_MAJOR,
        PLH_COMPONENT_SCRIPT_ENGINE_MINOR,
        PLH_COMPONENT_CONFIG_MAJOR,
        PLH_COMPONENT_CONFIG_MINOR,
        {0} // build_id: empty = "cannot verify freshness"
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    if (ctx->log)
    {
        char buf[160];
        std::snprintf(buf, sizeof buf, "native_test: init channel=%s",
                      ctx->channel ? ctx->channel : "(null)");
        ctx->log(ctx, PLH_LOG_INFO, buf);
    }
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void)
{
    g_ctx = nullptr;
}

extern "C" PLH_EXPORT const char *native_name(void)
{
    return "l4_native_producer";
}
extern "C" PLH_EXPORT const char *native_version(void)
{
    return "1.0.0";
}

extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)
{
    if (!tx || !tx->slot || tx->slot_size < sizeof(float))
        return false;

    *static_cast<float *>(tx->slot) = static_cast<float>(g_produce_count);
    g_produce_count++;

    if (g_ctx && g_ctx->report_metric)
        g_ctx->report_metric(g_ctx, "produced_total", static_cast<double>(g_produce_count));

    // ── One-shot live query probe (first produce tick = queue Active,
    //    REG_ACK processed, hub-globals loaded — all three queries are
    //    deterministically answerable).  Each reply is evaluated BEFORE
    //    the next call: the three share one thread-local scratch buffer
    //    (native_engine_api.h v13 lifetime rule). ──
    if (!g_live_probed && g_ctx && g_ctx->get_schema_json && g_ctx->get_channel_schema_json &&
        g_ctx->get_channel_metrics_json && g_ctx->channel)
    {
        g_live_probed = 1;

        const char *r = g_ctx->get_schema_json(g_ctx, "hub", "$l4.native.frame.v1");
        const int hub_ok =
            (r && std::strstr(r, "\"status\":\"success\"") && std::strstr(r, "\"blds\"")) ? 1 : 0;

        r = g_ctx->get_channel_schema_json(g_ctx, g_ctx->channel);
        const int ch_ok =
            (r && std::strstr(r, "\"status\":\"success\"") && std::strstr(r, "\"blds\"")) ? 1 : 0;

        r = g_ctx->get_channel_metrics_json(g_ctx, g_ctx->channel);
        const int m_ok =
            (r && std::strstr(r, "\"status\":\"success\"") && std::strstr(r, "\"metrics\"")) ? 1
                                                                                             : 0;

        if (g_ctx->log)
        {
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "native_test: live_queries hub_schema=%d channel_schema=%d "
                          "channel_metrics=%d",
                          hub_ok, ch_ok, m_ok);
            g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
        }
    }

    return true;
}

extern "C" PLH_EXPORT void on_stop(void)
{
    if (g_ctx && g_ctx->log)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "native_test: stop produced=%d", g_produce_count);
        g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
    }
}
