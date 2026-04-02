/**
 * @file good_producer_plugin.cpp
 * @brief Test plugin — a correct producer plugin with all features.
 *
 * Used by test_scriptengine_native_dylib.cpp to exercise NativeEngine.
 */
#include "utils/native_engine_api.h"

#include <cstring>

// ── Schema declaration (matches test config) ────────────────────────────
// SlotFrame: one float32 field named "value"
PLH_DECLARE_SCHEMA(SlotFrame, "value:float32:1:0", 4)

// ── Plugin state ────────────────────────────────────────────────────────
static const PlhNativeContext *g_ctx = nullptr;
static int g_init_count = 0;
static int g_produce_count = 0;
static int g_stop_count = 0;

// ── Required symbols ────────────────────────────────────────────────────

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1, // little-endian
        PLH_NATIVE_API_VERSION
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    g_init_count++;
    if (ctx->log)
        ctx->log(ctx, PLH_LOG_INFO, "good_producer_plugin: init");
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void)
{
    g_ctx = nullptr;
}

extern "C" PLH_EXPORT const char *native_name(void) { return "good_producer"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

// ── Callbacks ───────────────────────────────────────────────────────────

extern "C" PLH_EXPORT void on_init(const char * /*args_json*/)
{
    g_init_count += 100; // distinguishable from native_init
}

extern "C" PLH_EXPORT void on_stop(const char * /*args_json*/)
{
    g_stop_count++;
}

extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)
{
    if (!tx || !tx->slot || tx->slot_size < sizeof(float))
        return false;

    auto *slot = static_cast<float *>(tx->slot);
    *slot = 42.0f;
    g_produce_count++;

    // Report a custom metric via context function pointer.
    if (g_ctx && g_ctx->report_metric)
        g_ctx->report_metric(g_ctx, "produce_count",
                             static_cast<double>(g_produce_count));

    return true; // commit
}

extern "C" PLH_EXPORT void on_heartbeat(const char * /*args_json*/)
{
    // Called from ctrl_thread_ — args_json is NULL or JSON string.
}

// ── Query symbols for test verification ─────────────────────────────────

extern "C" PLH_EXPORT int test_get_init_count(void) { return g_init_count; }
extern "C" PLH_EXPORT int test_get_produce_count(void) { return g_produce_count; }
extern "C" PLH_EXPORT int test_get_stop_count(void) { return g_stop_count; }
