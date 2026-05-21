/*
 * Hub-side native script — validates hub_script_runner.cpp B12 fix.
 *
 * If the fix is missing, `entry_point` falls back to "__init__.py"
 * and load_script fails ("native engine not found").  With the fix,
 * load_script tries "plugin.so" and succeeds.
 *
 * Plugin emits:
 *   - on_init:  "HubNative started" log + initial metric.
 *   - on_tick:  per-tick log + monotone metric.
 *   - on_stop:  final summary log.
 *
 * Note: on_tick is dispatched by the native engine through its
 * generic-invoke fallback (`engine.invoke("on_tick")` from
 * HubAPI::dispatch_tick).  Each tick does a dlsym lookup (acceptable
 * for sub-second tick rates) — exporting `on_tick` with the FnVoid
 * ABI (`void on_tick(const char *args_json)`) is the contract.
 */
#include "utils/native_engine_api.h"
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace {
const PlhNativeContext *g_ctx = nullptr;
int64_t g_tick_count = 0;
double g_t0 = 0.0;

double now_s()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
}

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1, PLH_NATIVE_API_VERSION,
        0,0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, {0}
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void) { g_ctx = nullptr; }
extern "C" PLH_EXPORT const char *native_name(void)    { return "hub_native_demo"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

// Lifecycle: NO args per the documented C ABI (native_engine_api.h
// §315-319 says `void name(void)`).  Reviewed in this session — the
// L2 examples take an unused `const char *` by mistake; matching the
// canonical ABI here.
extern "C" PLH_EXPORT void on_init(void)
{
    g_t0 = now_s();
    if (g_ctx && g_ctx->log)
        g_ctx->log(g_ctx, PLH_LOG_INFO, "HubNative started — on_init fired");
    if (g_ctx && g_ctx->report_metric)
        g_ctx->report_metric(g_ctx, "hub_native_init", 1.0);
}

// on_tick uses the generic-invoke fallback shape (FnVoid) — needs
// to take a `const char *args_json` parameter.  Native engine's
// invoke("on_tick") dlsym-resolves and calls with nullptr.
extern "C" PLH_EXPORT void on_tick(const char * /*args_json*/)
{
    g_tick_count++;
    if (!g_ctx || !g_ctx->log) return;
    char buf[160];
    double elapsed = now_s() - g_t0;
    std::snprintf(buf, sizeof(buf),
        "HubNative tick=%lld elapsed_s=%.3f heartbeat",
        static_cast<long long>(g_tick_count), elapsed);
    g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
    if (g_ctx->report_metric)
    {
        g_ctx->report_metric(g_ctx, "hub_native_tick_count",
                             static_cast<double>(g_tick_count));
        g_ctx->report_metric(g_ctx, "hub_native_uptime_s", elapsed);
    }
}

extern "C" PLH_EXPORT void on_stop(void)
{
    if (!g_ctx || !g_ctx->log) return;
    char buf[160];
    double elapsed = now_s() - g_t0;
    std::snprintf(buf, sizeof(buf),
        "HubNative STOPPED ticks=%lld elapsed_s=%.3f",
        static_cast<long long>(g_tick_count), elapsed);
    g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
}
