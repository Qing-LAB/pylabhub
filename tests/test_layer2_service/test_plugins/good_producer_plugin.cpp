/**
 * @file good_producer_plugin.cpp
 * @brief Test native module — a correct producer with all v2 API features.
 *
 * Used by test_scriptengine_native_dylib.cpp to exercise NativeEngine.
 * Tests both the C API (PlhNativeContext function pointers) and the C++
 * wrapper (plh::Context, SpinLockGuard, with_spinlock).
 */
#include "utils/native_engine_api.h"

#include <cstring>

// ── Schema declaration (matches test config) ────────────────────────────
// SlotFrame: one float32 field named "value"
PLH_DECLARE_SCHEMA(SlotFrame, "value:float32:1:0", 4)

// ── Module state ────────────────────────────────────────────────────────
static const PlhNativeContext *g_ctx = nullptr;
static int g_init_count = 0;
static int g_produce_count = 0;
static int g_stop_count = 0;

// V2 API test results (set during on_produce, queried by test harness).
static uint64_t g_v2_out_slots_written = 0;
static uint64_t g_v2_in_slots_received = 0;
static uint64_t g_v2_out_drop_count = 0;
static uint64_t g_v2_script_error_count = 0;
static size_t   g_v2_slot_logical_size = 0;
static uint32_t g_v2_spinlock_count = 0;
static int      g_v2_cpp_wrapper_ok = 0;

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
        ctx->log(ctx, PLH_LOG_INFO, "good_producer: init");
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

    // ── V1 API: custom metric ───────────────────────────────────────
    if (g_ctx && g_ctx->report_metric)
    {
        g_ctx->report_metric(g_ctx, "produce_count",
                             static_cast<double>(g_produce_count));
    }

    // ── V2 C API: read counters through renamed function pointers ───
    if (g_ctx)
    {
        if (g_ctx->out_slots_written)
            g_v2_out_slots_written = g_ctx->out_slots_written(g_ctx);
        if (g_ctx->in_slots_received)
            g_v2_in_slots_received = g_ctx->in_slots_received(g_ctx);
        if (g_ctx->out_drop_count)
            g_v2_out_drop_count = g_ctx->out_drop_count(g_ctx);
        if (g_ctx->script_error_count)
            g_v2_script_error_count = g_ctx->script_error_count(g_ctx);

        // Schema size and spinlock count (no SHM in test — expect 0 spinlocks).
        if (g_ctx->slot_logical_size)
            g_v2_slot_logical_size = g_ctx->slot_logical_size(g_ctx, PLH_SIDE_AUTO);
        if (g_ctx->spinlock_count)
            g_v2_spinlock_count = g_ctx->spinlock_count(g_ctx, PLH_SIDE_AUTO);
    }

    // ── V2 C++ wrapper: verify plh::Context works ───────────────────
#ifdef __cplusplus
    if (g_ctx)
    {
        plh::Context api(g_ctx);
        g_v2_cpp_wrapper_ok = 1;

        // Verify identity accessors.
        if (!api.uid() || !api.name() || !api.role_tag())
            g_v2_cpp_wrapper_ok = 0;

        // Verify counter accessors match C API results.
        if (api.out_slots_written() != g_v2_out_slots_written)
            g_v2_cpp_wrapper_ok = 0;
        if (api.in_slots_received() != g_v2_in_slots_received)
            g_v2_cpp_wrapper_ok = 0;
        if (api.out_drop_count() != g_v2_out_drop_count)
            g_v2_cpp_wrapper_ok = 0;
        if (api.script_error_count() != g_v2_script_error_count)
            g_v2_cpp_wrapper_ok = 0;

        // Verify schema size matches.
        if (api.slot_logical_size(PLH_SIDE_AUTO) != g_v2_slot_logical_size)
            g_v2_cpp_wrapper_ok = 0;

        // Verify spinlock count (0 without SHM).
        if (api.spinlock_count(PLH_SIDE_AUTO) != g_v2_spinlock_count)
            g_v2_cpp_wrapper_ok = 0;
    }
#endif

    // Report v2 validation results as custom metrics (test harness reads these).
    if (g_ctx && g_ctx->report_metric)
    {
        g_ctx->report_metric(g_ctx, "v2_out_slots_written",
                             static_cast<double>(g_v2_out_slots_written));
        g_ctx->report_metric(g_ctx, "v2_slot_logical_size",
                             static_cast<double>(g_v2_slot_logical_size));
        g_ctx->report_metric(g_ctx, "v2_spinlock_count",
                             static_cast<double>(g_v2_spinlock_count));
        g_ctx->report_metric(g_ctx, "v2_cpp_wrapper_ok",
                             static_cast<double>(g_v2_cpp_wrapper_ok));
    }

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

// V2 query symbols.
extern "C" PLH_EXPORT uint64_t test_get_v2_out_slots_written(void) { return g_v2_out_slots_written; }
extern "C" PLH_EXPORT uint64_t test_get_v2_in_slots_received(void) { return g_v2_in_slots_received; }
extern "C" PLH_EXPORT uint64_t test_get_v2_out_drop_count(void) { return g_v2_out_drop_count; }
extern "C" PLH_EXPORT uint64_t test_get_v2_script_error_count(void) { return g_v2_script_error_count; }
extern "C" PLH_EXPORT size_t   test_get_v2_slot_logical_size(void) { return g_v2_slot_logical_size; }
extern "C" PLH_EXPORT uint32_t test_get_v2_spinlock_count(void) { return g_v2_spinlock_count; }
extern "C" PLH_EXPORT int      test_get_v2_cpp_wrapper_ok(void) { return g_v2_cpp_wrapper_ok; }
