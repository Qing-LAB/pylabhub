/*
 * Native engine demo plugin (producer side).
 *
 * Writes count + value into each SHM slot at 50 Hz.  After N slots,
 * the role host's loop is stopped from the C side via
 * ctx->set_critical_error() (effectively "stop now, clean") to
 * exercise the native-engine shutdown path.
 *
 * Built externally by the demo's setup_commands (g++ -shared -fPIC).
 * Loaded by NativeEngine via dlopen at role startup.
 */
#include "utils/native_engine_api.h"
#include <cstdint>
#include <cstdio>

// Per-slot schema (matches producer.json:out_slot_schema)
// Schema must match config's auto-derived form (pipe-separated; the
// final "0" per field is the dim-marker, NOT a byte offset).
PLH_DECLARE_SCHEMA(OutSlotFrame, "count:int64:1:0|value:float64:1:0", 16)

namespace {
// SLOT_TARGET kept below the producer's ring size (out_shm_slot_count=64)
// so we can complete in a single ring-pass without needing a consumer
// to drain the ring.
constexpr int  SLOT_TARGET = 40;
const PlhNativeContext *g_ctx = nullptr;
int64_t g_count = 0;
bool    g_stopped = false;
}

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1,
        PLH_NATIVE_API_VERSION,
        0, 0, 0,  // version axes — empty = "cannot verify freshness"
        0, 0,     // shm
        0, 0,     // broker
        0, 0,     // zmq frame
        0, 0,     // script api
        0, 0,     // script engine
        0, 0,     // config
        {0}
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    if (ctx && ctx->log)
        ctx->log(ctx, PLH_LOG_INFO, "NativeProd loaded");
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void) { g_ctx = nullptr; }

extern "C" PLH_EXPORT const char *native_name(void)    { return "native_demo_producer"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

extern "C" PLH_EXPORT void on_init(const char * /*args_json*/)
{
    if (g_ctx && g_ctx->log)
        g_ctx->log(g_ctx, PLH_LOG_INFO, "NativeProd started");
}

extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)
{
    if (!tx || !tx->slot || tx->slot_size < 16)
        return false;

    // Layout matches OutSlotFrame: int64 count @ 0, float64 value @ 8.
    auto *count_ptr = static_cast<int64_t *>(tx->slot);
    auto *value_ptr = reinterpret_cast<double *>(static_cast<char *>(tx->slot) + 8);

    g_count++;
    *count_ptr = g_count;
    *value_ptr = static_cast<double>(g_count) * 0.5;

    if (g_count % 10 == 0 && g_ctx && g_ctx->log)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "NativeProd wrote count=%lld value=%.4f",
                      static_cast<long long>(g_count), *value_ptr);
        g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
    }

    if (!g_stopped && g_count >= SLOT_TARGET)
    {
        g_stopped = true;
        if (g_ctx && g_ctx->log)
            g_ctx->log(g_ctx, PLH_LOG_INFO,
                       "NativeProd reached target — calling request_stop() for clean exit");
        // request_stop() = "done with work, stop cleanly".
        // set_critical_error(msg) is reserved for actual unrecoverable
        // failures (emits [ERROR] log line by design).  Doc gap finding
        // 2026-05-21 — to be documented in README_NativePlugins.md.
        if (g_ctx && g_ctx->request_stop)
            g_ctx->request_stop(g_ctx);
    }
    return true;
}

extern "C" PLH_EXPORT void on_stop(const char * /*args_json*/)
{
    if (g_ctx && g_ctx->log)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "NativeProd STOPPED total=%lld",
                      static_cast<long long>(g_count));
        g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
    }
}
