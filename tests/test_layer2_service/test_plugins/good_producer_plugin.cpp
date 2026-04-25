/**
 * @file good_producer_plugin.cpp
 * @brief Test native module — a correct producer exercising all API features.
 *
 * Used by test_scriptengine_native_dylib.cpp to exercise NativeEngine.
 * Tests both the C API (PlhNativeContext function pointers) and the C++
 * wrapper (plh::Context, SpinLockGuard, with_spinlock).
 */
#include "utils/native_engine_api.h"
#include "pylabhub_version.h"  // PYLABHUB_VERSION_* for HEP-0032 axes

#include <cstdlib>
#include <cstring>

// ── Schema declarations (matches test config) ───────────────────────────
// All directional names + alias: one float32 field "value".
PLH_DECLARE_SCHEMA(SlotFrame,     "value:float32:1:0", 4)
PLH_DECLARE_SCHEMA(OutSlotFrame,  "value:float32:1:0", 4)
PLH_DECLARE_SCHEMA(InSlotFrame,   "value:float32:1:0", 4)
PLH_DECLARE_SCHEMA(OutFlexFrame,  "value:float32:1:0", 4)
PLH_DECLARE_SCHEMA(FlexFrame,     "value:float32:1:0", 4)
PLH_DECLARE_SCHEMA(InboxFrame,    "value:float32:1:0", 4)

// ── Module state ────────────────────────────────────────────────────────
static const PlhNativeContext *g_ctx = nullptr;
static int g_init_count = 0;
static int g_produce_count = 0;
static int g_stop_count = 0;

// API test results (set during on_produce, queried by test harness).
static uint64_t g_test_out_slots_written = 0;
static uint64_t g_test_in_slots_received = 0;
static uint64_t g_test_out_drop_count = 0;
static uint64_t g_test_script_error_count = 0;
static size_t   g_test_slot_logical_size = 0;
static uint32_t g_test_spinlock_count = 0;
static int      g_test_cpp_wrapper_ok = 0;

// Band API test results — without a broker, all 4 functions must return
// gracefully (no crash, sensible null/zero results).
static int g_test_band_join_null    = 0;  // 1 if band_join returned NULL
static int g_test_band_leave_zero   = 0;  // 1 if band_leave returned 0
static int g_test_band_send_ok      = 0;  // 1 if band_broadcast returned (no crash)
static int g_test_band_members_null = 0;  // 1 if band_members returned NULL

// ── Required symbols ────────────────────────────────────────────────────

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    // Populate the HEP-CORE-0032 ComponentVersions fields from the
    // C-visible PLH_COMPONENT_* constants in native_engine_api.h.  A
    // real third-party plugin would do the same — these are frozen at
    // plugin-compile time and record the header values the plugin was
    // built against.  build_id is left empty because plugins don't
    // have a standard git-SHA generation step.
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1, // little-endian
        PLH_NATIVE_API_VERSION,
        // HEP-0032 axes (struct_size >= extended size):
        static_cast<uint16_t>(PYLABHUB_VERSION_MAJOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_MINOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_ROLLING),
        PLH_COMPONENT_SHM_MAJOR,           PLH_COMPONENT_SHM_MINOR,
        PLH_COMPONENT_BROKER_PROTO_MAJOR,  PLH_COMPONENT_BROKER_PROTO_MINOR,
        PLH_COMPONENT_ZMQ_FRAME_MAJOR,     PLH_COMPONENT_ZMQ_FRAME_MINOR,
        PLH_COMPONENT_SCRIPT_API_MAJOR,    PLH_COMPONENT_SCRIPT_API_MINOR,
        PLH_COMPONENT_SCRIPT_ENGINE_MAJOR, PLH_COMPONENT_SCRIPT_ENGINE_MINOR,
        PLH_COMPONENT_CONFIG_MAJOR,        PLH_COMPONENT_CONFIG_MINOR,
        {0} // build_id: plugin-specific; empty = "cannot verify freshness"
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

    // Write flexzone if present.
    if (tx->fz && tx->fz_size >= sizeof(float))
        *static_cast<float *>(tx->fz) = 99.0f;

    // ── V1 API: custom metric ───────────────────────────────────────
    if (g_ctx && g_ctx->report_metric)
    {
        g_ctx->report_metric(g_ctx, "produce_count",
                             static_cast<double>(g_produce_count));
    }

    // ── C API: read counters through renamed function pointers ───
    if (g_ctx)
    {
        if (g_ctx->out_slots_written)
            g_test_out_slots_written = g_ctx->out_slots_written(g_ctx);
        if (g_ctx->in_slots_received)
            g_test_in_slots_received = g_ctx->in_slots_received(g_ctx);
        if (g_ctx->out_drop_count)
            g_test_out_drop_count = g_ctx->out_drop_count(g_ctx);
        if (g_ctx->script_error_count)
            g_test_script_error_count = g_ctx->script_error_count(g_ctx);

        // Schema size and spinlock count (no SHM in test — expect 0 spinlocks).
        if (g_ctx->slot_logical_size)
            g_test_slot_logical_size = g_ctx->slot_logical_size(g_ctx, PLH_SIDE_AUTO);
        if (g_ctx->spinlock_count)
            g_test_spinlock_count = g_ctx->spinlock_count(g_ctx, PLH_SIDE_AUTO);
    }

    // ── Band API: must return gracefully without a broker ────
    if (g_ctx)
    {
        if (g_ctx->band_join)
        {
            char *r = g_ctx->band_join(g_ctx, "!l2_test");
            g_test_band_join_null = (r == nullptr) ? 1 : 0;
            if (r) free(r);
        }
        if (g_ctx->band_leave)
        {
            g_test_band_leave_zero =
                (g_ctx->band_leave(g_ctx, "!l2_test") == 0) ? 1 : 0;
        }
        if (g_ctx->band_broadcast)
        {
            g_ctx->band_broadcast(g_ctx, "!l2_test", "{\"k\":1}");
            g_test_band_send_ok = 1; // reached next line = no crash
        }
        if (g_ctx->band_members)
        {
            char *r = g_ctx->band_members(g_ctx, "!l2_test");
            g_test_band_members_null = (r == nullptr) ? 1 : 0;
            if (r) free(r);
        }
    }

    // ── C++ wrapper: verify plh::Context works ───────────────────
#ifdef __cplusplus
    if (g_ctx)
    {
        plh::Context api(g_ctx);
        g_test_cpp_wrapper_ok = 1;

        // Verify identity accessors.
        if (!api.uid() || !api.name() || !api.role_tag())
            g_test_cpp_wrapper_ok = 0;

        // Verify counter accessors match C API results.
        if (api.out_slots_written() != g_test_out_slots_written)
            g_test_cpp_wrapper_ok = 0;
        if (api.in_slots_received() != g_test_in_slots_received)
            g_test_cpp_wrapper_ok = 0;
        if (api.out_drop_count() != g_test_out_drop_count)
            g_test_cpp_wrapper_ok = 0;
        if (api.script_error_count() != g_test_script_error_count)
            g_test_cpp_wrapper_ok = 0;

        // Verify schema size matches.
        if (api.slot_logical_size(PLH_SIDE_AUTO) != g_test_slot_logical_size)
            g_test_cpp_wrapper_ok = 0;

        // Verify spinlock count (0 without SHM).
        if (api.spinlock_count(PLH_SIDE_AUTO) != g_test_spinlock_count)
            g_test_cpp_wrapper_ok = 0;
    }
#endif

    // Report validation results as custom metrics (test harness reads these).
    if (g_ctx && g_ctx->report_metric)
    {
        g_ctx->report_metric(g_ctx, "test_out_slots_written",
                             static_cast<double>(g_test_out_slots_written));
        g_ctx->report_metric(g_ctx, "test_slot_logical_size",
                             static_cast<double>(g_test_slot_logical_size));
        g_ctx->report_metric(g_ctx, "test_spinlock_count",
                             static_cast<double>(g_test_spinlock_count));
        g_ctx->report_metric(g_ctx, "test_cpp_wrapper_ok",
                             static_cast<double>(g_test_cpp_wrapper_ok));

        // Band API (graceful behavior without broker).
        g_ctx->report_metric(g_ctx, "test_band_join_null",
                             static_cast<double>(g_test_band_join_null));
        g_ctx->report_metric(g_ctx, "test_band_leave_zero",
                             static_cast<double>(g_test_band_leave_zero));
        g_ctx->report_metric(g_ctx, "test_band_send_ok",
                             static_cast<double>(g_test_band_send_ok));
        g_ctx->report_metric(g_ctx, "test_band_members_null",
                             static_cast<double>(g_test_band_members_null));
    }

    return true; // commit
}

extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)
{
    if (!rx || !rx->slot || rx->slot_size < sizeof(float))
        return false;
    // Read the value — just verify it's accessible.
    auto val = *static_cast<const float *>(rx->slot);
    (void)val;
    return true; // release
}

extern "C" PLH_EXPORT bool on_process(const plh_rx_t *rx, const plh_tx_t *tx)
{
    if (!rx || !rx->slot || !tx || !tx->slot)
        return false;
    if (rx->slot_size < sizeof(float) || tx->slot_size < sizeof(float))
        return false;
    // Copy input * 2 to output.
    auto in_val = *static_cast<const float *>(rx->slot);
    *static_cast<float *>(tx->slot) = in_val * 2.0f;
    return true; // commit
}

extern "C" PLH_EXPORT bool on_inbox(const plh_inbox_msg_t *msg)
{
    if (!msg || !msg->data || msg->data_size < sizeof(float))
        return false;
    (void)*static_cast<const float *>(msg->data);
    (void)msg->sender_uid;
    (void)msg->seq;
    return true;
}

extern "C" PLH_EXPORT void on_heartbeat(const char * /*args_json*/)
{
    // Called from ctrl_thread_ — args_json is NULL or JSON string.
}

// ── Query symbols for test verification ─────────────────────────────────

extern "C" PLH_EXPORT int test_get_init_count(void) { return g_init_count; }
extern "C" PLH_EXPORT int test_get_produce_count(void) { return g_produce_count; }
extern "C" PLH_EXPORT int test_get_stop_count(void) { return g_stop_count; }

// Query symbols for test verification.
extern "C" PLH_EXPORT uint64_t test_get_out_slots_written(void) { return g_test_out_slots_written; }
extern "C" PLH_EXPORT uint64_t test_get_in_slots_received(void) { return g_test_in_slots_received; }
extern "C" PLH_EXPORT uint64_t test_get_out_drop_count(void) { return g_test_out_drop_count; }
extern "C" PLH_EXPORT uint64_t test_get_script_error_count(void) { return g_test_script_error_count; }
extern "C" PLH_EXPORT size_t   test_get_slot_logical_size(void) { return g_test_slot_logical_size; }
extern "C" PLH_EXPORT uint32_t test_get_spinlock_count(void) { return g_test_spinlock_count; }
extern "C" PLH_EXPORT int      test_get_cpp_wrapper_ok(void) { return g_test_cpp_wrapper_ok; }

// Band API query symbols (set during on_produce).
extern "C" PLH_EXPORT int test_get_band_join_null(void)    { return g_test_band_join_null; }
extern "C" PLH_EXPORT int test_get_band_leave_zero(void)   { return g_test_band_leave_zero; }
extern "C" PLH_EXPORT int test_get_band_send_ok(void)      { return g_test_band_send_ok; }
extern "C" PLH_EXPORT int test_get_band_members_null(void) { return g_test_band_members_null; }
